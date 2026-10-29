from PIL import Image
import os

INPUT_DIR = "."
OUTPUT_DIR = "images_out"
WIDTH = 1024
HEIGHT = 600

if not os.path.exists(OUTPUT_DIR):
    os.makedirs(OUTPUT_DIR)

print("Ищу картинки...")
for filename in os.listdir(INPUT_DIR):
    if filename.lower().endswith(('.png', '.jpg', '.jpeg', '.bmp')):
        img_path = os.path.join(INPUT_DIR, filename)
        img = Image.open(img_path).convert("RGB")
        
        # Умный ресайз (чтобы картинка не плющилась)
        img_ratio = img.width / img.height
        target_ratio = WIDTH / HEIGHT
        
        if img_ratio > target_ratio:
            new_width = int(target_ratio * img.height)
            left = (img.width - new_width) / 2
            img = img.crop((left, 0, left + new_width, img.height))
        else:
            new_height = int(img.width / target_ratio)
            top = (img.height - new_height) / 2
            img = img.crop((0, top, img.width, top + new_height))
            
        img = img.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
        
        # Конвертация в RGB565
        out_filename = os.path.splitext(filename)[0] + ".rgb"
        out_path = os.path.join(OUTPUT_DIR, out_filename)
        
        with open(out_path, "wb") as f:
            for y in range(HEIGHT):
                for x in range(WIDTH):
                    r, g, b = img.getpixel((x, y))
                    # Упаковка в 16 бит
                    rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                    f.write(rgb565.to_bytes(2, byteorder='little'))
                    
        print(f"Конвертировано: {out_filename}")
print("Готово! Файлы лежат в папке images_out")