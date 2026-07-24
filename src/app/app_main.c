/*
 * 这是应用入口文件，刻意只保留“模块如何协作”的业务逻辑。
 *
 * 阅读建议：
 * 1. 先从 app_main() 看清初始化顺序；
 * 2. 再看 on_button_pressed() 理解事件如何更新画面；
 * 3. 硬件细节分别进入 display 和 input 组件查看。
 */

#include "display/app_display.h"
#include "input/board_buttons.h"
#include "esp_err.h"
#include "esp_log.h"

// TAG 会出现在串口日志每一行中，便于判断日志属于哪个模块。
static const char *TAG = "button_display";

/*
 * 使用“指定下标初始化”建立 按键 ID -> 界面颜色 的对应关系。
 * 例如 [BOARD_BUTTON_BOOT] 明确表示该颜色属于 BOOT，即使以后调整枚举
 * 的排列顺序，也比只写三个没有名称的颜色更容易检查。
 */
static const app_display_accent_t BUTTON_ACCENTS[BOARD_BUTTON_COUNT] = {
    [BOARD_BUTTON_BOOT] = APP_DISPLAY_ACCENT_YELLOW,
    [BOARD_BUTTON_PWR] = APP_DISPLAY_ACCENT_MAGENTA,
    [BOARD_BUTTON_PLUS] = APP_DISPLAY_ACCENT_GREEN,
};

static void on_button_pressed(board_button_id_t button, void *user_ctx)
{
    // 当前没有额外上下文要传给回调，但显式标记可避免编译器警告。
    (void)user_ctx;

    // 防御性检查：错误 ID 不应被当作数组下标使用，否则可能越界。
    if (button < 0 || button >= BOARD_BUTTON_COUNT) {
        ESP_LOGW(TAG, "Ignoring unknown button id %d", button);
        return;
    }

    // 输入组件负责提供按键名称，应用层只决定该名称用什么颜色显示。
    const char *name = board_button_get_name(button);
    esp_err_t ret = app_display_show_button(name, BUTTON_ACCENTS[button]);

    // 回调中不使用 ESP_ERROR_CHECK，以免一次显示错误直接导致整机重启。
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to display %s button: %s", name,
                 esp_err_to_name(ret));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing Waveshare ESP32-S3-Touch-LCD-1.54");

    /*
     * 初始化顺序很重要：
     * 1. 先初始化 LCD 并显示提示画面；
     * 2. 再初始化按键 GPIO；
     * 3. 最后启动按键任务，避免任务过早回调尚未就绪的显示模块。
     *
     * ESP_ERROR_CHECK 会在初始化失败时打印错误并停止继续运行，避免程序
     * 带着一个未正确初始化的硬件模块进入不可预测状态。
     */
    ESP_ERROR_CHECK(app_display_init());
    ESP_ERROR_CHECK(board_buttons_init());
    ESP_ERROR_CHECK(board_buttons_start(on_button_pressed, NULL));

    /*
     * app_main 返回是安全的：按键监听已经运行在 board_buttons 创建的
     * 独立 FreeRTOS 任务中。ESP-IDF 会结束 main_task，但不会结束该任务。
     */
    ESP_LOGI(TAG, "Application is ready");
}
