/*
 * 应用入口只负责编排电源、显示硬件和待机页面。
 *
 * 进入 app_main 后首先接管电池供电；LCD 初始化期间背光保持关闭，待机
 * 页面把首帧完整绘制到 LCD GRAM 后，应用层才点亮面板。页面准备完成后
 * 再启动 PWR 长按关机监控，避免开机按键被误判为关机。
 */

#include "display/lcd_display.h"
#include "esp_err.h"
#include "esp_log.h"
#include "power/power_manager.h"
#include "standby/standby_page.h"

static const char *TAG = "pet_app";

/**
 * @brief PWR 长按关机前关闭显示。
 *
 * 此回调运行在电源监控任务中。当前应用没有需要落盘的状态，因此只关闭
 * 面板和背光；返回后电源组件会拉低 BAT_EN。
 */
static esp_err_t app_prepare_power_off(void *context)
{
    (void)context;
    return lcd_display_disable();
}

void app_main(void)
{
    /*
     * BAT_EN 必须在其他耗时初始化之前拉高，否则使用电池启动时松开 PWR
     * 会立即断电，LCD 和应用代码也就没有机会完成初始化。
     */
    ESP_ERROR_CHECK(power_manager_init());

    ESP_LOGI(TAG, "Starting standby animation");

    ESP_ERROR_CHECK(lcd_display_init());
    ESP_ERROR_CHECK(standby_page_start());
    ESP_ERROR_CHECK(lcd_display_enable());
    ESP_ERROR_CHECK(
        power_manager_start(app_prepare_power_off, NULL));

    /*
     * 页面动画由 LVGL 定时器驱动。app_main 返回后 LVGL 自己的任务仍会继续
     * 刷新，PWR 也由电源监控任务处理，因此这里不需要常驻循环。
     */
    ESP_LOGI(TAG, "Standby page is ready");
}
