#include "frame_cap_pipeline.hpp"
#include "esp_video_init.h"
#include "video_capture.hpp"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_cam_ctlr.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

using namespace who::frame_cap;

#define MODEL_TIME 3

#if CONFIG_IDF_TARGET_ESP32S3
WhoFrameCap *get_dvp_frame_cap_pipeline()
{
    esp_cam_ctlr_dvp_pin_config_t dvp_pin_config = {
        .data_width = CAM_CTLR_DATA_WIDTH_8,
        .data_io = { GPIO_NUM_11, GPIO_NUM_9, GPIO_NUM_8, GPIO_NUM_10, GPIO_NUM_12, GPIO_NUM_18, GPIO_NUM_17, GPIO_NUM_16 },
        .vsync_io = GPIO_NUM_6,
        .de_io    = GPIO_NUM_7,
        .pclk_io  = GPIO_NUM_13,
        .xclk_io  = GPIO_NUM_15,
    };

    esp_video_init_sccb_config_t sccb_config = {
        .init_sccb = true,
        .i2c_config = { .port = 0, .scl_pin = GPIO_NUM_5, .sda_pin = GPIO_NUM_4 },
        .freq = 100000,
    };

    esp_video_init_dvp_config_t dvp_config = {
        .sccb_config = sccb_config,
        .reset_pin = GPIO_NUM_NC,
        .pwdn_pin  = GPIO_NUM_NC,
        .dvp_pin   = dvp_pin_config,
        .xclk_freq = 10000000,
    };

    esp_video_init_config_t video_config = {};
    video_config.dvp = &dvp_config;
    esp_video_init(&video_config);

    auto cap = new VideoCapture();
    auto cfg = VideoCapture::Config(ESP_VIDEO_DVP_DEVICE_NAME, V4L2_PIX_FMT_RGB565X, 3);

#ifdef BSP_BOARD_ESP32_S3_EYE
    cfg.set_vflip(true);
#elif defined(BSP_BOARD_ESP32_S3_KORVO_2)
    cfg.set_hflip(true).set_vflip(true);
#endif

    ESP_LOGI("CAM", "!!! 1. BEFORE cap->init !!!");
    esp_err_t err = cap->init(cfg);
    if (err != ESP_OK) {
        ESP_LOGE("CAM", "INIT FAILED: %s", esp_err_to_name(err));
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    ESP_LOGI("CAM", "!!! 2. AFTER cap->init (BEFORE start) !!!");
    esp_rom_printf("\n### DVP: BEFORE START ###\n");
    
    err = cap->start();
    esp_rom_printf("### DVP: AFTER START (ret=%d) ###\n", err);

    if (err != ESP_OK) {
        ESP_LOGE("CAM", "START FAILED: %s", esp_err_to_name(err));
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    ESP_LOGI("CAM", "!!! 3. AFTER cap->start !!!");

    esp_rom_printf("## P1: before new WhoFrameCap\n");
    auto frame_cap = new WhoFrameCap();
    
    esp_rom_printf("## P2: before add_node (WhoFetchNode)\n");
    frame_cap->add_node<WhoFetchNode>("FrameCapFetch", cap);
    
    esp_rom_printf("## P3: after add_node. RETURNING pipeline\n");
    return frame_cap;
}
#elif CONFIG_IDF_TARGET_ESP32P4
WhoFrameCap *get_mipi_csi_frame_cap_pipeline()
{
    esp_log_level_set("ISP_AWB", ESP_LOG_ERROR);
    bsp_camera_cfg_t cam_cfg{};
    ESP_ERROR_CHECK(bsp_camera_start(&cam_cfg));
    auto cap = new VideoCapture();
    auto cfg = VideoCapture::Config(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, V4L2_PIX_FMT_RGB565X, 3).set_hflip(true);
    cap->init(cfg);
    cap->start();
    auto frame_cap = new WhoFrameCap();
    frame_cap->add_node<WhoFetchNode>("FrameCapFetch", cap);
    return frame_cap;
}
#endif

#if CONFIG_ESP_VIDEO_ENABLE_USB_UVC_VIDEO_DEVICE
WhoFrameCap *get_uvc_frame_cap_pipeline()
{
    esp_video_init_usb_uvc_config_t usb_uvc_cfg = {
        .uvc = { .uvc_dev_num = 1, .task_stack = 4096, .task_priority = 10, .task_affinity = -1, },
        .usb = { .init_usb_host_lib = true, .peripheral_map = 0x00, .task_stack = 4096, .task_priority = 11, .task_affinity = -1, },
    };
    esp_video_init_config_t video_cfg = {};
    video_cfg.usb_uvc = &usb_uvc_cfg;
    esp_video_init(&video_cfg);

    auto cap = new VideoCapture();
    auto cfg = VideoCapture::Config(ESP_VIDEO_USB_UVC_NAME(0), V4L2_PIX_FMT_JPEG, 4).set_uvc_config({640, 480, 30});
    cap->init(cfg);
    cap->start();

    auto frame_cap = new WhoFrameCap();
    frame_cap->add_node<WhoFetchNode>("FrameCapFetch", cap, false);
    frame_cap->add_node<WhoDecodeNode>("FrameCapDecode", dl::image::DL_IMAGE_PIX_TYPE_RGB565LE, 2, false);
    frame_cap->add_node<WhoPPAResizeNode>(
        "FrameCapPPAResize", 800, 600, dl::image::DL_IMAGE_PIX_TYPE_RGB565LE, MODEL_TIME + 1);
    return frame_cap;
}
#endif
