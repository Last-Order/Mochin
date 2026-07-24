/*
 * 板载按键组件的内部实现。
 *
 * 这里采用“定时轮询 + 软件消抖”，而不是在 GPIO 边沿中断里直接更新屏幕。
 * 原因是机械按键在按下和松开瞬间会快速抖动出多个高低电平；中断会被连续
 * 触发，而且中断上下文不能安全执行等待 LCD DMA 这类耗时操作。
 *
 * 独立 FreeRTOS 任务每 10 ms 读取一次 GPIO，确认电平连续稳定 30 ms 后，
 * 才向应用层报告一次状态变化。
 */

#include "input/board_buttons.h"

#include <stdbool.h>
#include <stddef.h>

#include "board/board_config.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 每 10 ms 采样一次，足够及时，同时不会让任务一直占用 CPU。
#define BUTTON_POLL_INTERVAL_MS 10
// 新电平保持 30 ms 才被接受，用来过滤机械触点抖动。
#define BUTTON_DEBOUNCE_MS 30
// 任务会调用显示回调，4096 字节栈为调用链和日志保留余量。
#define BUTTON_TASK_STACK_SIZE 4096
// 普通应用任务优先级，不需要高于系统关键任务。
#define BUTTON_TASK_PRIORITY 5

static const char *TAG = "board_buttons";

/*
 * 每个按键需要同时保存“最近一次采样值”和“已经确认的稳定值”。
 *
 * 例子：稳定值原来是 1，某次采样突然变成 0：
 * - sampled_level 立即记录 0，并记下变化时间；
 * - 如果后续采样又跳回 1，计时重新开始；
 * - 只有 0 连续保持 30 ms，stable_level 才正式更新为 0 并上报按下。
 */
typedef struct {
    board_button_id_t id;          // 应用层使用的逻辑 ID
    gpio_num_t gpio_num;           // 实际读取的 ESP32-S3 GPIO
    const char *name;              // PCB 丝印名称，用于日志和屏幕
    int sampled_level;             // 最近一次读取到的原始电平
    int stable_level;              // 已通过消抖确认的稳定电平
    TickType_t sample_changed_at;  // 原始电平最后变化时的系统 tick
} button_state_t;

// 这个表是“逻辑按键、物理 GPIO、显示名称”的唯一对应关系。
static button_state_t s_buttons[] = {
    {
        .id = BOARD_BUTTON_BOOT,
        .gpio_num = BOARD_BUTTON_PIN_BOOT,
        .name = "BOOT",
    },
    {
        .id = BOARD_BUTTON_PWR,
        .gpio_num = BOARD_BUTTON_PIN_PWR,
        .name = "PWR",
    },
    {
        .id = BOARD_BUTTON_PLUS,
        .gpio_num = BOARD_BUTTON_PIN_PLUS,
        .name = "PLUS",
    },
};

/*
 * 组件生命周期状态：
 * - s_initialized：GPIO 和每个按键的初始电平已经准备好；
 * - s_task：非 NULL 表示轮询任务已经创建；
 * - s_callback/s_callback_context：检测到按下后通知应用层。
 */
static bool s_initialized;
static TaskHandle_t s_task;
static board_button_pressed_cb_t s_callback;
static void *s_callback_context;

/**
 * @brief 按键轮询任务，也是软件消抖状态机的主体。
 *
 * 此函数由 FreeRTOS 调度，不由应用层直接调用，并且在 while(true) 中持续
 * 运行。每轮处理完所有按键后主动 vTaskDelay()，把 CPU 让给其他任务。
 */
static void board_buttons_task(void *arg)
{
    // xTaskCreate 支持传入任务参数；本任务暂时不需要，所以显式忽略。
    (void)arg;

    // FreeRTOS 用 tick 计时，pdMS_TO_TICKS 负责把毫秒转换成当前 tick 数。
    const TickType_t debounce_ticks = pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS);

    while (true) {
        // 同一轮所有按键共用一次时间戳，避免反复读取系统 tick。
        const TickType_t now = xTaskGetTickCount();

        for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]);
             ++i) {
            button_state_t *button = &s_buttons[i];
            const int level = gpio_get_level(button->gpio_num);

            /*
             * 第一步：发现原始采样值变化。
             * 只记录新值和起始时间，本轮不立即承认它，等待后续采样确认稳定。
             */
            if (level != button->sampled_level) {
                button->sampled_level = level;
                button->sample_changed_at = now;
                continue;
            }

            /*
             * 第二步：原始值没有继续变化。
             * - 若它已经等于稳定值，说明没有新事件；
             * - 若保持时间不足 30 ms，说明还在消抖观察期。
             *
             * TickType_t 是无符号计数，使用 now - old 的写法也能正确跨越
             * tick 溢出点，比直接比较绝对时间更安全。
             */
            if (level == button->stable_level ||
                now - button->sample_changed_at < debounce_ticks) {
                continue;
            }

            // 第三步：新电平保持时间足够长，正式接受本次状态变化。
            button->stable_level = level;

            /*
             * 本板按键低电平有效，所以只在稳定值变为 0 时上报“按下”。
             * 稳定值变回 1 代表松开，本应用暂时不需要松开事件。
             */
            if (level == 0) {
                ESP_LOGI(TAG, "%s button pressed (GPIO%d)", button->name,
                         button->gpio_num);

                // 回调运行在本任务上下文，允许应用层等待 LCD DMA 完成。
                s_callback(button->id, s_callback_context);
            }
        }

        // 延时既确定了采样周期，也防止这个无限循环占满一个 CPU 核心。
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_INTERVAL_MS));
    }
}

esp_err_t board_buttons_init(void)
{
    // 重复 gpio_config 虽然可能成功，但通常代表应用初始化顺序有错误。
    ESP_RETURN_ON_FALSE(!s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Buttons are already initialized");

    /*
     * pin_bit_mask 可以在一次 gpio_config() 中配置三个引脚。
     * 按键接地后产生低电平，因此输入端启用上拉、禁用下拉。
     * 本模块使用任务轮询，所以不启用 GPIO 中断。
     */
    const gpio_config_t button_config = {
        .pin_bit_mask = (1ULL << BOARD_BUTTON_PIN_BOOT) |
                        (1ULL << BOARD_BUTTON_PIN_PWR) |
                        (1ULL << BOARD_BUTTON_PIN_PLUS),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&button_config), TAG,
                        "Failed to configure button GPIOs");

    /*
     * 初始化时把原始值和稳定值都设为当前真实电平。
     * 否则程序刚启动时，默认的 0 可能被错误解释成一次按键事件。
     */
    const TickType_t now = xTaskGetTickCount();
    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
        const int level = gpio_get_level(s_buttons[i].gpio_num);
        s_buttons[i].sampled_level = level;
        s_buttons[i].stable_level = level;
        s_buttons[i].sample_changed_at = now;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Buttons initialized: BOOT=GPIO%d, PWR=GPIO%d, "
                  "PLUS=GPIO%d",
             BOARD_BUTTON_PIN_BOOT, BOARD_BUTTON_PIN_PWR,
             BOARD_BUTTON_PIN_PLUS);
    return ESP_OK;
}

esp_err_t board_buttons_start(board_button_pressed_cb_t callback,
                              void *user_ctx)
{
    // 在创建任务前验证生命周期和参数，错误会尽早暴露在启动日志中。
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "Buttons are not initialized");
    ESP_RETURN_ON_FALSE(s_task == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "Button task is already running");
    ESP_RETURN_ON_FALSE(callback != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Button callback must not be NULL");

    /*
     * 先保存回调，再创建任务。任务一旦被调度就可能立即检测按键，
     * 因此不能等 xTaskCreate() 返回后才设置回调。
     */
    s_callback = callback;
    s_callback_context = user_ctx;

    /*
     * 创建独立任务后，app_main() 可以安全返回。任务句柄保存在 s_task 中，
     * 同时也用于阻止重复创建第二个监听任务。
     */
    BaseType_t task_created = xTaskCreate(
        board_buttons_task, "board_buttons", BUTTON_TASK_STACK_SIZE, NULL,
        BUTTON_TASK_PRIORITY, &s_task);
    if (task_created != pdPASS) {
        // 创建失败时恢复状态，让调用者修复问题后仍有机会再次调用。
        s_callback = NULL;
        s_callback_context = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Button monitoring task started");
    return ESP_OK;
}

const char *board_button_get_name(board_button_id_t button)
{
    /*
     * 按 ID 查表而不是直接用 s_buttons[button]，这样以后即使表的排列顺序
     * 改变，也不会把按键映射到错误名称。
     */
    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
        if (s_buttons[i].id == button) {
            return s_buttons[i].name;
        }
    }

    // 返回固定字符串而不是 NULL，可简化日志和显示层的错误处理。
    return "UNKNOWN";
}
