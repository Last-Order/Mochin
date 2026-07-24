/*
 * 桌宠待机页面。
 *
 * 原始 WebP 第一排按 192 × 192 网格包含 8 格，其中最后一格为空。构建
 * 资源只保存前 7 帧，并采用 RGB565 颜色平面紧跟 A8 透明度平面的 LVGL
 * 原生布局。运行时只切换图片描述符，不解码、不复制整帧，也不创建额外
 * FreeRTOS 任务。
 */

#include "standby/standby_page.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#define STANDBY_FRAME_WIDTH 192
#define STANDBY_FRAME_HEIGHT 192
#define STANDBY_FRAME_COUNT 7
#define STANDBY_FRAME_INTERVAL_MS 400
#define STANDBY_FRAME_STRIDE (STANDBY_FRAME_WIDTH * sizeof(uint16_t))
#define STANDBY_FRAME_COLOR_BYTES \
    (STANDBY_FRAME_STRIDE * STANDBY_FRAME_HEIGHT)
#define STANDBY_FRAME_ALPHA_BYTES \
    (STANDBY_FRAME_WIDTH * STANDBY_FRAME_HEIGHT)
#define STANDBY_FRAME_BYTES \
    (STANDBY_FRAME_COLOR_BYTES + STANDBY_FRAME_ALPHA_BYTES)
#define STANDBY_ASSET_BYTES \
    (STANDBY_FRAME_BYTES * STANDBY_FRAME_COUNT)

// 深暖色背景能衬托浅黄色角色，同时不会在待机时产生大面积刺眼白光。
#define STANDBY_BG_TOP 0x21171C
#define STANDBY_BG_BOTTOM 0x4B2F32

/*
 * ESP-IDF EMBED_FILES 使用嵌入文件的文件名生成链接符号。start/end 直接
 * 指向 Flash 映射区，动画期间不需要把约 756 KiB 资源复制进 RAM/PSRAM。
 */
extern const uint8_t purin_idle_asset_start[]
    asm("_binary_purin_idle_rgb565a8_start");
extern const uint8_t purin_idle_asset_end[]
    asm("_binary_purin_idle_rgb565a8_end");

static const char *TAG = "standby_page";

typedef struct {
    lv_obj_t *image;
    lv_timer_t *timer;
    size_t frame_index;
    bool started;
} standby_page_context_t;

static standby_page_context_t s_page;
static lv_image_dsc_t s_frames[STANDBY_FRAME_COUNT];

/**
 * @brief 校验嵌入资源大小，并建立每帧对应的 LVGL 图片描述符。
 *
 * 描述符和数据都具有静态生命周期。lv_image_set_src() 只保存它们的地址，
 * 因此动画切帧不会分配内存或复制像素。
 */
static esp_err_t standby_prepare_frame_descriptors(void)
{
    const size_t asset_size =
        (size_t)(purin_idle_asset_end - purin_idle_asset_start);
    ESP_RETURN_ON_FALSE(
        asset_size == STANDBY_ASSET_BYTES, ESP_ERR_INVALID_SIZE, TAG,
        "Standby asset size is %u bytes, expected %u",
        (unsigned int)asset_size, (unsigned int)STANDBY_ASSET_BYTES);

    for (size_t i = 0; i < STANDBY_FRAME_COUNT; ++i) {
        s_frames[i] = (lv_image_dsc_t) {
            .header = {
                .magic = LV_IMAGE_HEADER_MAGIC,
                .cf = LV_COLOR_FORMAT_RGB565A8,
                .flags = 0,
                .w = STANDBY_FRAME_WIDTH,
                .h = STANDBY_FRAME_HEIGHT,
                .stride = STANDBY_FRAME_STRIDE,
                .reserved_2 = 0,
            },
            .data_size = STANDBY_FRAME_BYTES,
            .data = purin_idle_asset_start + i * STANDBY_FRAME_BYTES,
            .reserved = NULL,
            .reserved_2 = NULL,
        };
    }

    return ESP_OK;
}

/**
 * @brief LVGL 定时器回调：切换到下一帧并让图片对象失效重绘。
 *
 * LVGL 定时器回调运行在 LVGL 任务上下文中，因此这里不能再取得 port 锁，
 * 也不需要额外的跨任务同步。回调只修改一个静态对象和帧下标。
 */
static void standby_animation_timer_cb(lv_timer_t *timer)
{
    standby_page_context_t *page = lv_timer_get_user_data(timer);

    page->frame_index = (page->frame_index + 1) % STANDBY_FRAME_COUNT;
    lv_image_set_src(page->image, &s_frames[page->frame_index]);
}

esp_err_t standby_page_start(void)
{
    ESP_RETURN_ON_FALSE(!s_page.started, ESP_ERR_INVALID_STATE, TAG,
                        "Standby page is already started");
    ESP_RETURN_ON_FALSE(lv_display_get_default() != NULL,
                        ESP_ERR_INVALID_STATE, TAG,
                        "LVGL display is not initialized");
    ESP_RETURN_ON_ERROR(
        standby_prepare_frame_descriptors(), TAG,
        "Failed to prepare standby frames");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(0), ESP_FAIL, TAG,
                        "Failed to lock LVGL while creating standby page");

    lv_obj_t *screen = lv_screen_active();

    /*
     * 页面占满当前屏幕。使用纯背景渐变且不放文字或交互控件，让待机阶段
     * 只展示角色动画；后续页面切换可以整体替换这个对象树。
     */
    lv_obj_clean(screen);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(
        screen, lv_color_hex(STANDBY_BG_TOP), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(
        screen, lv_color_hex(STANDBY_BG_BOTTOM), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    s_page.image = lv_image_create(screen);
    if (s_page.image == NULL) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }

    s_page.frame_index = 0;
    lv_image_set_src(s_page.image, &s_frames[0]);
    lv_obj_center(s_page.image);
    lv_obj_remove_flag(
        s_page.image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /*
     * 定时器属于 LVGL 对象体系，由 LVGL 任务周期调用。400 ms 对应
     * 2.5 fps，一轮 7 帧约 2.8 秒，让轻微呼吸/眨眼动作保持舒缓。
     */
    s_page.timer = lv_timer_create(
        standby_animation_timer_cb, STANDBY_FRAME_INTERVAL_MS, &s_page);
    if (s_page.timer == NULL) {
        lv_obj_delete(s_page.image);
        s_page.image = NULL;
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }

    s_page.started = true;
    lvgl_port_unlock();

    ESP_LOGI(TAG, "Standby animation started: %d frames, %d ms/frame",
             STANDBY_FRAME_COUNT, STANDBY_FRAME_INTERVAL_MS);
    return ESP_OK;
}
