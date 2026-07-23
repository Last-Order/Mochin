# ESP32-S3-Touch-LCD-1.54 — ESP-IDF Hello World

Pure ESP-IDF project for the Waveshare ESP32-S3-Touch-LCD-1.54. It drives
the onboard 240 × 240 ST7789 LCD through ESP-IDF's `esp_lcd` API and displays
`HELLO WORLD!`.

The previous Arduino sketch is preserved in `helloworld/`, but it is not part
of the ESP-IDF build.

## Requirements

- ESP-IDF 5.5.0 or newer (5.5.2 is the Waveshare documented version)
- A data-capable USB-C cable
- Windows COM port for the board (currently `COM5`)

## Build and flash

Open an ESP-IDF terminal in this directory, then run:

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COM5 flash monitor
```

Exit the monitor with `Ctrl+]`.

If the first flash cannot connect, hold `BOOT`, start the flash command, and
release `BOOT` after the connection begins.

## LCD pin map

| Signal | GPIO |
| --- | ---: |
| DC | 45 |
| CS | 21 |
| SCLK | 38 |
| MOSI | 39 |
| RESET | 40 |
| Backlight | 46 |

The project defaults configure the ESP32-S3R8 for 16 MB QIO Flash, 8 MB Octal
PSRAM, and USB Serial/JTAG console output.
