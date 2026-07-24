#pragma once

/*
 * display 组件只提供显示硬件生命周期，不包含具体页面。
 *
 * 调用顺序：
 * 1. lcd_display_init() 建立 ST7789、SPI 和 LVGL，背光保持关闭；
 * 2. 页面组件创建对象并准备首帧；
 * 3. lcd_display_enable() 刷新首帧并点亮面板。
 */

#include "esp_err.h"

/**
 * @brief 初始化 LCD 硬件和 LVGL 显示端口。
 *
 * 只能在 app_main 的初始化阶段调用一次。成功返回后 LVGL 已可创建页面，
 * 但面板与背光仍关闭。
 *
 * @return ESP_OK 成功；重复调用或硬件初始化失败时返回对应错误。
 */
esp_err_t lcd_display_init(void);

/**
 * @brief 完成首帧刷新并点亮 LCD 面板与背光。
 *
 * 必须在 lcd_display_init() 和首个页面创建完成后调用，只能调用一次。
 *
 * @return ESP_OK 成功；调用顺序错误或硬件操作失败时返回对应错误。
 */
esp_err_t lcd_display_enable(void);
