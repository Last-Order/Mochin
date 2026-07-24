#pragma once

/*
 * board 组件通过本文件集中提供 Waveshare ESP32-S3-Touch-LCD-1.54
 * 的板级硬件配置。
 *
 * 为什么要单独放在这里：
 * 1. 业务代码不需要记忆具体 GPIO，只使用 BOARD_ 开头的有意义名称。
 * 2. 将来更换板卡或核对原理图时，只需优先检查这一处。
 * 3. 避免显示、按键、触控等组件各自复制一份引脚定义并逐渐不一致。
 *
 * 注意：这些数值来自本板官方资料和示例，不可套用其他 ESP32-S3 板卡。
 */

// gpio_num_t 和 SPI2_HOST 等类型、枚举由下面两个驱动头文件提供。
#include "driver/gpio.h"
#include "driver/spi_master.h"

// LCD 使用 ESP32-S3 的 SPI2 控制器。SPI0/1 通常保留给 Flash/PSRAM。
#define BOARD_LCD_HOST SPI2_HOST

// ST7789 四线 SPI 接口引脚。
// DC：区分“命令”和“像素数据”；CS：选择 LCD 设备。
#define BOARD_LCD_PIN_DC GPIO_NUM_45
#define BOARD_LCD_PIN_CS GPIO_NUM_21
// SCLK：SPI 时钟；MOSI：ESP32 向 LCD 发送数据，本屏无需 MISO。
#define BOARD_LCD_PIN_SCLK GPIO_NUM_38
#define BOARD_LCD_PIN_MOSI GPIO_NUM_39
// RESET 低电平复位 LCD；BACKLIGHT 高电平点亮背光。
#define BOARD_LCD_PIN_RESET GPIO_NUM_40
#define BOARD_LCD_PIN_BACKLIGHT GPIO_NUM_46

// LCD 可见区域为 240×240，像素格式由显示组件配置为 RGB565。
#define BOARD_LCD_WIDTH 240
#define BOARD_LCD_HEIGHT 240
// SPI 像素时钟已经在实物上验证为 40 MHz。
#define BOARD_LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)

// 三个板载按键均为低电平有效：松开通常读到 1，按下读到 0。
#define BOARD_BUTTON_PIN_BOOT GPIO_NUM_0
#define BOARD_BUTTON_PIN_PWR GPIO_NUM_5
#define BOARD_BUTTON_PIN_PLUS GPIO_NUM_4
