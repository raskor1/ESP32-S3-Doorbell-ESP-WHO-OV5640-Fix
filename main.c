#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_random.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "lvgl.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "jpeg_decoder.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"

#define RELAY_GPIO GPIO_NUM_20
LV_FONT_DECLARE(font_ru_24);
LV_FONT_DECLARE(font_clock);

#define SLIDE_COUNT_MAX 5 
static uint8_t *slide_data[SLIDE_COUNT_MAX] = {0};
static int slide_count = 0;
static int current_slide = 0;
static lv_obj_t *slide_img = NULL;
static lv_timer_t *slideshow_timer = NULL;
static lv_img_dsc_t slide_img_dsc;

static int swipe_start_x = -1;
static int swipe_start_y = -1;
static int64_t swipe_start_t = 0;
#define SWIPE_THRESHOLD  200
#define SWIPE_TIME_LIMIT 500

static void write_log(const char *msg);
static lv_timer_t *clock_timer = NULL;
static lv_obj_t *clock_label = NULL;
static lv_obj_t *date_label = NULL;
static lv_coord_t clock_pos_x = 300;
static lv_coord_t clock_pos_y = 200;

static lv_obj_t *video_canvas = NULL;
static lv_obj_t *status_label = NULL;
static uint8_t *out_buf = NULL;
static volatile bool is_video_active = false;

static lv_timer_t *idle_timer = NULL;
#define IDLE_TIMEOUT_MS 60000

static void show_main_screen(lv_event_t *e);
static void show_call_screen(lv_event_t *e);
static void reset_idle_timer(void);

static void stop_slideshow(void) {
    if (slideshow_timer) { lv_timer_del(slideshow_timer); slideshow_timer = NULL; }
}

static void next_slide_cb(lv_timer_t *t) {
    if (slide_count == 0 || !slide_img) return;
    current_slide = (current_slide + 1) % slide_count;
    slide_img_dsc.data = slide_data[current_slide];
    lv_obj_invalidate(slide_img);
}

static void load_slides(void) {
    DIR *dir = opendir("/sdcard/images");
    if (!dir) { 
        ESP_LOGW("SLIDE", "!!! НЕТ ПАПКИ /sdcard/images !!!"); 
        return; 
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && slide_count < SLIDE_COUNT_MAX) {
                ESP_LOGI("SLIDE", "Вижу файл: %s", ent->d_name); // <-- Логируем ВСЁ, что видим
         if (!strstr(ent->d_name, ".rgb") && !strstr(ent->d_name, ".RGB")) continue;
        
        char path[64];
        snprintf(path, sizeof(path), "/sdcard/images/%s", ent->d_name);
        FILE *f = fopen(path, "rb");
        if (!f) { 
            ESP_LOGE("SLIDE", "НЕ МОГУ ОТКРЫТЬ: %s", path); // <-- Если не открылся
            continue; 
        }
        
        uint32_t sz = 1024 * 600 * 2;
        uint8_t *buf = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
        if (fread(buf, 1, sz, f) == sz) {
            slide_data[slide_count++] = buf;
            ESP_LOGI("SLIDE", "Загружено: %s", ent->d_name);
        } else { 
            ESP_LOGE("SLIDE", "ОШИБКА РАЗМЕРА: %s (нужно ровно 1228800 байт)", ent->d_name);
            heap_caps_free(buf); 
        }
        fclose(f);
    }
    closedir(dir);
    ESP_LOGI("SLIDE", "ВСЕГО НАЙДЕНО КАРТИНОК: %d", slide_count);
    
    if (slide_count > 0) {
        slide_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        slide_img_dsc.header.w = 1024;
        slide_img_dsc.header.h = 600;
        slide_img_dsc.data_size = 1024 * 600 * 2;
        slide_img_dsc.data = slide_data[0];
    }
}

static void idle_timeout_cb(lv_timer_t *t) {
    bsp_display_lock(0);
    show_main_screen(NULL);
    bsp_display_unlock();
    idle_timer = NULL;
}

static void reset_idle_timer(void) {
    if (idle_timer) { lv_timer_del(idle_timer); idle_timer = NULL; }
    idle_timer = lv_timer_create(idle_timeout_cb, IDLE_TIMEOUT_MS, NULL);
    lv_timer_set_repeat_count(idle_timer, 1);
}

static void swipe_handler(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSING) {
        if (swipe_start_x < 0) {
            swipe_start_x = p.x; swipe_start_y = p.y; swipe_start_t = lv_tick_get();
        }
    } else if (code == LV_EVENT_RELEASED) {
        if (swipe_start_x >= 0) {
            int dx = p.x - swipe_start_x;
            int dy = p.y - swipe_start_y;
            int64_t dt = lv_tick_get() - swipe_start_t;
            if (dt < SWIPE_TIME_LIMIT && abs(dx) > SWIPE_THRESHOLD && abs(dx) > abs(dy) * 2) {
                reset_idle_timer();
                if (dx > 0) { show_main_screen(NULL); } else { show_call_screen(NULL); }
            }
            swipe_start_x = -1;
        }
    } else if (code == LV_EVENT_CLICKED) { swipe_start_x = -1; }
}

static void add_swipe_detector(lv_obj_t *scr) {
    lv_obj_t *overlay = lv_obj_create(scr);
    lv_obj_set_size(overlay, 1024, 600);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, swipe_handler, LV_EVENT_ALL, NULL);
    lv_obj_move_to_index(overlay, 0);
}

static void update_clock_cb(lv_timer_t *timer) {
    time_t now; struct tm timeinfo; time(&now); localtime_r(&now, &timeinfo);
    if (clock_label) lv_label_set_text_fmt(clock_label, "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    if (date_label) lv_label_set_text_fmt(date_label, "%02d.%02d.%04d", timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);

    static int move_counter = 0;
    move_counter++;
    if (move_counter >= 10) {
        move_counter = 0;
        lv_coord_t scr_w = lv_obj_get_width(lv_scr_act());
        lv_coord_t scr_h = lv_obj_get_height(lv_scr_act());
        lv_coord_t clk_w = lv_obj_get_width(clock_label);
        lv_coord_t total_h = 180;
        lv_coord_t max_x = scr_w - clk_w - 30;
        lv_coord_t max_y = scr_h - total_h - 20;
        if (max_x < 20) max_x = 20;
        if (max_y < 20) max_y = 20;
        clock_pos_x = lv_rand(20, max_x);
        clock_pos_y = lv_rand(20, max_y);
        if (clock_label) {
            lv_obj_set_pos(clock_label, clock_pos_x, clock_pos_y);
            if (date_label) lv_obj_set_pos(date_label, clock_pos_x + 120, clock_pos_y + 130);
        }
    }
}

static void create_clock(lv_obj_t *scr) {
    clock_label = lv_label_create(scr);
    lv_obj_set_style_text_font(clock_label, &font_clock, 0);
    lv_obj_set_style_text_color(clock_label, lv_color_white(), 0);
    lv_obj_set_pos(clock_label, clock_pos_x, clock_pos_y);
    lv_obj_add_flag(clock_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(clock_label, show_call_screen, LV_EVENT_CLICKED, NULL);

    date_label = lv_label_create(scr);
    lv_obj_set_style_text_font(date_label, &font_ru_24, 0);
    lv_obj_set_style_text_color(date_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_pos(date_label, clock_pos_x + 120, clock_pos_y + 130);

    clock_timer = lv_timer_create(update_clock_cb, 1000, NULL);
    update_clock_cb(clock_timer);
}

// ПРОЗРАЧНЫЕ КНОПКИ
// ПРОЗРАЧНЫЕ КНОПКИ С ПОДСВЕТКОЙ ПРИ НАЖАТИИ
static lv_obj_t* create_transparent_btn(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, lv_event_cb_t event_cb) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    
    // --- ОБЫЧНОЕ СОСТОЯНИЕ (Прозрачное) ---
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_white(), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    
    // --- СОСТОЯНИЕ ПРИ НАЖАТИИ (Яркая заливка) ---
    lv_obj_set_style_bg_opa(btn, LV_OPA_60, LV_STATE_PRESSED);                // Включаем заливку на 60%
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x00AAFF), LV_STATE_PRESSED); // Синий цвет
    lv_obj_set_style_border_color(btn, lv_color_hex(0x00AAFF), LV_STATE_PRESSED); // Рамка тоже синяя

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &font_ru_24, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
    
    if (event_cb != NULL) {
        lv_obj_add_event_cb(btn, event_cb, LV_EVENT_CLICKED, NULL);
    }
    return btn;
}

static void video_stream_task(void *pvParameters) {
    esp_http_client_config_t config = { .url = "http://192.168.4.2/stream", .timeout_ms = 5000 };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    uint8_t *jpeg_buf = heap_caps_malloc(1024 * 100, MALLOC_CAP_SPIRAM);
    uint8_t *stream_buf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    int stream_len = 0; bool in_jpeg = false; uint8_t last_byte = 0;

    while (1) {
        if (!is_video_active) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        if (esp_http_client_open(client, 0) != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
        esp_http_client_fetch_headers(client);
        stream_len = 0; in_jpeg = false; last_byte = 0;
        while (is_video_active) {
            int read_len = esp_http_client_read(client, (char*)stream_buf, 8192);
            if (read_len == 0) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
            if (read_len < 0) { break; }
            for (int i = 0; i < read_len; i++) {
                uint8_t b = stream_buf[i];
                if (!in_jpeg) {
                    if (last_byte == 0xFF && b == 0xD8) { in_jpeg = true; jpeg_buf[0] = 0xFF; jpeg_buf[1] = 0xD8; stream_len = 2; }
                } else {
                    jpeg_buf[stream_len++] = b;
                    if (stream_len >= 2 && jpeg_buf[stream_len-2] == 0xFF && jpeg_buf[stream_len-1] == 0xD9) {
                        bsp_display_lock(0);
                        esp_jpeg_image_cfg_t jpeg_cfg = {
                            .indata = jpeg_buf, .indata_size = stream_len, .outbuf = out_buf, .outbuf_size = 640 * 480 * 2,
                            .out_format = JPEG_IMAGE_FORMAT_RGB565, .out_scale = JPEG_IMAGE_SCALE_0,
                            .flags = { .swap_color_bytes = 0 }, // <--- ИСПРАВЛЕНИЕ ЦВЕТА
                        };
                        esp_jpeg_image_output_t img_info;
                        if (esp_jpeg_decode(&jpeg_cfg, &img_info) == ESP_OK) {
                            if (video_canvas && is_video_active) lv_obj_invalidate(video_canvas);
                        }
                        bsp_display_unlock();
                        in_jpeg = false; stream_len = 0;
                    }
                    if (stream_len >= 1024 * 100) { in_jpeg = false; stream_len = 0; }
                }
                last_byte = b;
            }
        }
        esp_http_client_close(client); vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void open_btn_cb(lv_event_t *e) {
    ESP_LOGI("UI", "Нажата кнопка ОТКРЫТЬ");
    write_log("Нажата кнопка ОТКРЫТЬ на экране P4"); // <--- ВОТ ЭТА СТРОЧКА
    gpio_set_level(RELAY_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(RELAY_GPIO, 0);
    reset_idle_timer();
}
static void answer_btn_cb(lv_event_t *e) { reset_idle_timer(); }
static void config_btn_cb(lv_event_t *e) { reset_idle_timer(); }
static void reject_btn_cb(lv_event_t *e) { reset_idle_timer(); show_main_screen(NULL); }
static void call_btn_cb(lv_event_t *e) { reset_idle_timer(); show_call_screen(NULL); }

static void show_call_screen(lv_event_t *e) {
    is_video_active = false;
    if (slideshow_timer) { lv_timer_del(slideshow_timer); slideshow_timer = NULL; }
    if (clock_timer) { lv_timer_del(clock_timer); clock_timer = NULL; }

    lv_obj_t *scr = lv_scr_act(); lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);

    video_canvas = lv_canvas_create(scr);
    lv_obj_set_pos(video_canvas, 20, 60);
    lv_canvas_set_buffer(video_canvas, out_buf, 640, 480, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_style_bg_color(video_canvas, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(video_canvas, LV_OPA_COVER, 0);

    status_label = lv_label_create(scr);
    lv_label_set_text(status_label, "ОТКРЫТО");
    lv_obj_set_style_text_font(status_label, &font_ru_24, 0);
    lv_obj_set_style_text_color(status_label, lv_color_white(), 0);
    lv_obj_align(status_label, LV_ALIGN_LEFT_MID, 150, -50);
    lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);

    create_transparent_btn(scr, "ОТВЕТИТЬ", 720, 50, 280, 90, answer_btn_cb);
    create_transparent_btn(scr, "ОТКРЫТЬ", 720, 170, 280, 90, open_btn_cb);
    lv_obj_t *btn_reject = create_transparent_btn(scr, "ОТКЛОНИТЬ", 720, 290, 280, 90, NULL);
    lv_obj_add_event_cb(btn_reject, reject_btn_cb, LV_EVENT_CLICKED, NULL);
    create_transparent_btn(scr, "КОНФИГ", 790, 500, 140, 60, config_btn_cb);

    is_video_active = true;
    add_swipe_detector(scr);
    reset_idle_timer();
}

static void show_main_screen(lv_event_t *e) {
    is_video_active = false;
    if (slideshow_timer) { lv_timer_del(slideshow_timer); slideshow_timer = NULL; }
    if (clock_timer) { lv_timer_del(clock_timer); clock_timer = NULL; }

    lv_obj_t *scr = lv_scr_act(); lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);

    if (slide_count > 0) {
        current_slide = 0;
        slide_img = lv_img_create(scr);
        lv_img_set_src(slide_img, &slide_img_dsc);
        lv_obj_set_pos(slide_img, 0, 0);
        lv_obj_set_size(slide_img, 1024, 600);

        lv_obj_t *call_btn = lv_btn_create(scr);
        lv_obj_set_size(call_btn, 90, 90);
        lv_obj_set_pos(call_btn, 920, 490);
        lv_obj_set_style_radius(call_btn, 45, 0);
        lv_obj_set_style_bg_color(call_btn, lv_color_make(0, 150, 0), 0);
        lv_obj_add_event_cb(call_btn, call_btn_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *call_label = lv_label_create(call_btn);
        lv_label_set_text(call_label, LV_SYMBOL_PLAY);
        lv_obj_center(call_label);

        slideshow_timer = lv_timer_create(next_slide_cb, 4000, NULL);
        clock_pos_x = lv_rand(50, 850); clock_pos_y = lv_rand(50, 500);
        create_clock(scr); add_swipe_detector(scr);
    } else {
        clock_pos_x = lv_rand(50, 850); clock_pos_y = lv_rand(50, 500);
        create_clock(scr); add_swipe_detector(scr);
    }
    reset_idle_timer();
}

static esp_err_t mount_sdcard(void) {
    esp_err_t ret;
    sd_pwr_ctrl_ldo_config_t ldo_config = { .ldo_chan_id = 4 };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
    ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
    if (ret != ESP_OK) return ret;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT(); host.slot = SDMMC_HOST_SLOT_0; host.pwr_ctrl_handle = pwr_ctrl_handle;
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4; slot_config.clk = GPIO_NUM_43; slot_config.cmd = GPIO_NUM_44;
    slot_config.d0 = GPIO_NUM_39; slot_config.d1 = GPIO_NUM_40; slot_config.d2 = GPIO_NUM_41; slot_config.d3 = GPIO_NUM_42;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t mount_config = { .format_if_mount_failed = false, .max_files = 5, .allocation_unit_size = 16 * 1024 };
    sdmmc_card_t *card;
    ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    return ret;
}
static void write_log(const char *msg) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    FILE *f = fopen("/sdcard/doorbell.log", "a");
    if (f != NULL) {
        fprintf(f, "[%02d.%02d.%04d %02d:%02d:%02d] %s\n",
                timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900,
                timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, msg);
        fclose(f);
    }
}
static void wifi_init_softap(void) {
    ESP_ERROR_CHECK(esp_netif_init()); ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT(); ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t wifi_config = { .ap = { .ssid = "Doorbell_Home", .ssid_len = strlen("Doorbell_Home"), .password = "password123", .max_connection = 4, .authmode = WIFI_AUTH_WPA2_PSK } };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static esp_err_t root_get_handler(httpd_req_t *req) {
    const char* html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'><title>Домофон</title><style>body{font-family:Arial;text-align:center;margin-top:20px;background:#f0f0f0;}.btn{background:#4CAF50;color:white;padding:15px 30px;font-size:20px;border:none;border-radius:10px;cursor:pointer;text-decoration:none;display:inline-block;margin:10px;}.time-form{background:white;padding:20px;border-radius:10px;display:inline-block;margin-top:20px;}</style></head><body><h1>Домофон</h1><a href='/open' class='btn'>ОТКРЫТЬ ДВЕРЬ</a><br><br><a href='/face_recognized' class='btn' style='background:#f44336'>ТЕСТ: РАСПОЗНАНО ЛИЦО</a><br><br><div class='time-form'><h3>Настройка времени</h3><form action='/set_time' method='get'>Дата: <input type='date' name='date' required><br><br>Время: <input type='time' step='1' name='time' required><br><br><input type='submit' value='Установить' class='btn'></form></div></body></html>";
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN); return ESP_OK;
}
static esp_err_t open_get_handler(httpd_req_t *req) {
    write_log("Открытие двери через HTTP (веб-сервер)"); // <--- ВОТ ЭТА СТРОЧКА
    gpio_set_level(RELAY_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(100)); 
    gpio_set_level(RELAY_GPIO, 0);
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
static void restore_call_screen_color_cb(lv_timer_t *t) {
    if (video_canvas) lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x101820), 0);
    if (status_label) lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
    lv_timer_del(t);
}
static esp_err_t face_recognized_get_handler(httpd_req_t *req) {
    ESP_LOGI("WEB", "СИГНАЛ: Лицо распознано! Открываем.");
    write_log("Лицо распознано! Дверь открыта."); // <--- ВОТ ЭТА СТРОЧКА
    gpio_set_level(RELAY_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(RELAY_GPIO, 0);
    bsp_display_lock(0);
    if (status_label && video_canvas) {
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0xCC0000), 0);
        lv_obj_clear_flag(status_label, LV_OBJ_FLAG_HIDDEN);
        lv_timer_t *restore_timer = lv_timer_create(restore_call_screen_color_cb, 5000, NULL);
        lv_timer_set_repeat_count(restore_timer, 1);
    }
    bsp_display_unlock();
    httpd_resp_send(req, "FACE OK", HTTPD_RESP_USE_STRLEN); return ESP_OK;
}
static esp_err_t set_time_get_handler(httpd_req_t *req) {
    char buf[100]; char date_str[20] = {0}, time_str[20] = {0};
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        if (httpd_query_key_value(buf, "date", date_str, sizeof(date_str)) != ESP_OK || httpd_query_key_value(buf, "time", time_str, sizeof(time_str)) != ESP_OK) {
            httpd_resp_send(req, "Ошибка", HTTPD_RESP_USE_STRLEN); return ESP_OK;
        }
    }
    int year, month, day, hour, min, sec = 0;
    sscanf(date_str, "%d-%d-%d", &year, &month, &day); sscanf(time_str, "%d:%d:%d", &hour, &min, &sec);
    struct tm timeinfo = { .tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day, .tm_hour = hour, .tm_min = min, .tm_sec = sec };
    time_t t = mktime(&timeinfo); struct timeval now = { .tv_sec = t }; settimeofday(&now, NULL);
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); return ESP_OK;
}
    // --- ФУНКЦИЯ ОТПРАВКИ ЛОГОВ В БРАУЗЕР (Она должна быть СНАРУЖИ!) ---
    static esp_err_t logs_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    FILE *f = fopen("/sdcard/doorbell.log", "r");
    if (f == NULL) {
        httpd_resp_send(req, "Логов пока нет.", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    
    char line[128];
    while (fgets(line, sizeof(line), f) != NULL) {
        httpd_resp_send_chunk(req, line, strlen(line));
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0); // Завершаем отправку
    return ESP_OK;
}

   // --- А ТУТ УЖЕ САМ ВЕБ-СЕРВЕР ---

static void start_webserver(void) {
    httpd_handle_t server = NULL; 
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/", .method = HTTP_GET, .handler = root_get_handler });
        httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/open", .method = HTTP_GET, .handler = open_get_handler });
        httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/logs", .method = HTTP_GET, .handler = logs_get_handler });
        httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/face_recognized", .method = HTTP_GET, .handler = face_recognized_get_handler });
        httpd_register_uri_handler(server, &(httpd_uri_t){ .uri = "/set_time", .method = HTTP_GET, .handler = set_time_get_handler });
    }
}     

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }
    gpio_reset_pin(RELAY_GPIO); gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT); gpio_set_level(RELAY_GPIO, 0);

    out_buf = heap_caps_malloc(640 * 480 * 2, MALLOC_CAP_SPIRAM);
    mount_sdcard(); load_slides(); wifi_init_softap(); start_webserver();

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = { .task_priority = 4, .task_stack = 16384, .task_affinity = 0, .task_max_sleep_ms = 500, .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT, .timer_period_ms = 5 },
        .buffer_size = 1024 * 600, .double_buffer = true,
        .flags = { .buff_dma = false, .buff_spiram = true, .sw_rotate = false }
    };
    bsp_display_start_with_config(&cfg); bsp_display_backlight_on();

    bsp_display_lock(0); show_main_screen(NULL); bsp_display_unlock();

    xTaskCreate(video_stream_task, "video_task", 8192, NULL, 5, NULL);

    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}