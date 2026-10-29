@echo off
chcp 65001 >nul
echo ========================================
echo   Конвертер картинок в RGB565 для ESP32
echo ========================================
echo.

:: Проверка Python
python --version >nul 2>&1
if %errorlevel% neq 0 (
    echo [ОШИБКА] Python не установлен или не добавлен в PATH!
    echo Скачайте и установите Python с python.org (обязательно поставьте галочку Add Python to PATH).
    pause
    exit /b
)

:: Установка Pillow
echo Проверка библиотеки Pillow...
python -c "import PIL" >nul 2>&1
if %errorlevel% neq 0 (
    echo Библиотека Pillow не найдена. Устанавливаю...
    python -m pip install Pillow
)

:: Запуск конвертера
echo.
echo Запуск конвертации...
python convert.py

echo.
echo ========================================
echo Готово! Возьмите файлы .rgb из папки images_out
echo и закиньте их на SD-карту в папку /images/
echo ========================================
pause