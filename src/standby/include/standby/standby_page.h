#pragma once

/*
 * standby 组件负责待机页面及其逐帧动画。
 *
 * 页面依赖已经初始化的 LVGL 显示端口；它不初始化 LCD，也不直接操作 SPI、
 * 面板或背光。
 */

#include "esp_err.h"

/**
 * @brief 创建待机页面并启动第一排雪碧帧的循环动画。
 *
 * 必须在 lcd_display_init() 之后、lcd_display_enable() 之前调用。函数会
 * 创建页面对象和 LVGL 定时器，并立即准备首帧；整个程序只能调用一次。
 *
 * @return ESP_OK 成功；资源不完整、内存不足或调用顺序错误时返回对应错误。
 */
esp_err_t standby_page_start(void);
