/*
 * 应用入口只负责编排显示硬件和待机页面。
 *
 * LCD 初始化期间背光保持关闭；待机页面把首帧完整绘制到 LCD GRAM 后，
 * 应用层才点亮面板，避免启动时短暂显示随机像素或空白页面。
 */

#include "display/lcd_display.h"
#include "esp_err.h"
#include "esp_log.h"
#include "standby/standby_page.h"

static const char *TAG = "pet_app";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting standby animation");

    ESP_ERROR_CHECK(lcd_display_init());
    ESP_ERROR_CHECK(standby_page_start());
    ESP_ERROR_CHECK(lcd_display_enable());

    /*
     * 页面动画由 LVGL 定时器驱动。app_main 返回后 LVGL 自己的任务仍会继续
     * 刷新，因此这里不需要常驻循环或额外的动画任务。
     */
    ESP_LOGI(TAG, "Standby page is ready");
}
