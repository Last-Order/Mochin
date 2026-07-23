#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

// Waveshare ESP32-S3-Touch-LCD-1.54 board-level hardware configuration.

#define BOARD_LCD_HOST SPI2_HOST

#define BOARD_LCD_PIN_DC GPIO_NUM_45
#define BOARD_LCD_PIN_CS GPIO_NUM_21
#define BOARD_LCD_PIN_SCLK GPIO_NUM_38
#define BOARD_LCD_PIN_MOSI GPIO_NUM_39
#define BOARD_LCD_PIN_RESET GPIO_NUM_40
#define BOARD_LCD_PIN_BACKLIGHT GPIO_NUM_46

#define BOARD_LCD_WIDTH 240
#define BOARD_LCD_HEIGHT 240
#define BOARD_LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)

#define BOARD_BUTTON_PIN_BOOT GPIO_NUM_0
#define BOARD_BUTTON_PIN_PWR GPIO_NUM_5
#define BOARD_BUTTON_PIN_PLUS GPIO_NUM_4
