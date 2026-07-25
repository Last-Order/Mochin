/*
 * 应用入口只负责编排电源、显示硬件、待机页面、网络和校时服务。
 *
 * 进入 app_main 后首先接管电池供电；LCD 初始化期间背光保持关闭，待机
 * 页面把首帧完整绘制到 LCD GRAM 后，应用层才点亮面板。页面准备完成后
 * 再启动 PWR 长按关机监控，避免开机按键被误判为关机。
 */

#include "display/lcd_display.h"
#include "esp_err.h"
#include "esp_log.h"
#include "network/network_manager.h"
#include "power/power_manager.h"
#include "standby/standby_page.h"
#include "time_service/time_service.h"

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
    /*
     * 首帧同步刷新只包含背景和角色。时钟对象在面板点亮后加入，后续刷新
     * 由 LVGL 自己的任务执行，避免 app_main 的默认任务栈承担复杂叠层绘制。
     */
    ESP_ERROR_CHECK(standby_page_show_clock());
    ESP_ERROR_CHECK(
        power_manager_start(app_prepare_power_off, NULL));

    /*
     * 网络和校时在页面完成首帧、PWR 监控已经启动后再初始化。Wi-Fi 扫描、
     * 认证、DHCP 和 SNTP 都由 ESP-IDF 后台任务异步执行，失败只能影响时钟
     * 是否从“--:--”切换为真实时间，不能阻塞动画或关机功能。
     */
    esp_err_t network_err = network_manager_init();
    if (network_err != ESP_OK) {
        ESP_LOGE(TAG, "Network initialization failed: %s",
                 esp_err_to_name(network_err));
    } else {
        const esp_err_t time_err = time_service_init();
        if (time_err != ESP_OK) {
            ESP_LOGE(TAG, "Time service initialization failed: %s",
                     esp_err_to_name(time_err));
        }

        network_err = network_manager_start();
        if (network_err != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi start skipped or failed: %s",
                     esp_err_to_name(network_err));
        }
    }

    /*
     * 页面和时钟由 LVGL 定时器驱动，网络由系统事件与 esp_timer 驱动，
     * PWR 由独立监控任务处理，因此 app_main 不需要常驻循环。
     */
    ESP_LOGI(TAG, "Standby page is ready; network startup is asynchronous");
}
