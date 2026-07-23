# ESP32-S3-Touch-LCD-1.54 — Button Display

Pure ESP-IDF project for the Waveshare ESP32-S3-Touch-LCD-1.54. It drives
the onboard 240 × 240 ST7789 LCD through ESP-IDF's `esp_lcd` API and listens
to all three onboard buttons. Pressing a button updates the display with its
silkscreen name: `BOOT`, `PWR`, or `PLUS`.

## Project structure

```text
.
├── components/
│   ├── board/          # Board pin map and fixed hardware parameters
│   ├── app_display/    # ST7789 initialization, drawing, and screen updates
│   └── board_buttons/  # Button GPIO setup, debounce, and polling task
└── main/
    └── app_main.c      # Application wiring and button-to-screen behavior
```

Public component APIs live in each component's `include/` directory. Keep
board-specific GPIO assignments in `board_config.h`; add new hardware drivers
as separate components instead of growing `app_main.c`.

## Requirements

- ESP-IDF 5.5.0 or newer (5.5.2 is the Waveshare documented version)
- A data-capable USB-C cable
- The board's current Windows COM port

## Build and flash

Open an ESP-IDF terminal in this directory, then run:

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
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

## Button pin map

| Button | GPIO | Active level |
| --- | ---: | ---: |
| BOOT | 0 | Low |
| PWR | 5 | Low |
| PLUS | 4 | Low |

The project defaults configure the ESP32-S3R8 for 16 MB QIO Flash, 8 MB Octal
PSRAM, and USB Serial/JTAG console output.
