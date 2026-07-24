/*
 * 桌宠待机页面与场景编排。
 *
 * 原始 WebP 第一排按 192 × 208 划分为 8 格，最后一格为空。构建资源只
 * 保存前 7 帧，并采用 RGB565 颜色平面紧跟 A8 透明度平面的 LVGL 原生
 * 布局。场景背景是独立的全屏 RGB565 图片；运行时只切换静态图片描述
 * 符，不解码、不复制整帧，也不创建额外 FreeRTOS 任务。
 */

#include "standby/standby_page.h"
#include "standby_scene_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "time_service/time_service.h"

#define STANDBY_FRAME_WIDTH 192
#define STANDBY_FRAME_HEIGHT 208
#define STANDBY_FRAME_COUNT 7
#define STANDBY_FRAME_INTERVAL_MS 400
#define STANDBY_CLOCK_INTERVAL_MS 1000
#define STANDBY_CLOCK_PANEL_WIDTH 80
#define STANDBY_CLOCK_PANEL_HEIGHT 30
#define STANDBY_CLOCK_PANEL_Y 6
#define STANDBY_CLOCK_TEXT_BYTES 6
#define STANDBY_FRAME_STRIDE (STANDBY_FRAME_WIDTH * sizeof(uint16_t))
#define STANDBY_FRAME_COLOR_BYTES \
    (STANDBY_FRAME_STRIDE * STANDBY_FRAME_HEIGHT)
#define STANDBY_FRAME_ALPHA_BYTES \
    (STANDBY_FRAME_WIDTH * STANDBY_FRAME_HEIGHT)
#define STANDBY_FRAME_BYTES \
    (STANDBY_FRAME_COLOR_BYTES + STANDBY_FRAME_ALPHA_BYTES)
#define STANDBY_ASSET_BYTES \
    (STANDBY_FRAME_BYTES * STANDBY_FRAME_COUNT)

// 场景图片异常未覆盖屏幕时使用的深暖色兜底，不作为实际场景内容。
#define STANDBY_FALLBACK_BG 0x2F1E19
// 时钟卡片沿用木屋的深棕色与暖白高光，避免遮挡角色时显得突兀。
#define STANDBY_CLOCK_PANEL_BG 0x2B1710
#define STANDBY_CLOCK_PANEL_BORDER 0xF7D891
#define STANDBY_CLOCK_TEXT_COLOR 0xFFF2C2

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
    lv_obj_t *background;
    lv_obj_t *image;
    lv_obj_t *clock_panel;
    lv_obj_t *clock_label;
    lv_timer_t *animation_timer;
    lv_timer_t *clock_timer;
    standby_scene_id_t scene_id;
    size_t frame_index;
    char clock_text[STANDBY_CLOCK_TEXT_BYTES];
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

/**
 * @brief LVGL 定时器回调：读取已校准的本地时间并更新分钟显示。
 *
 * 时间服务不会阻塞；首次 SNTP 完成前返回 false，页面明确显示“--:--”。
 * 回调每秒检查一次以便同步后尽快显现，但只有文本实际变化时才让标签重绘。
 */
static void standby_clock_timer_cb(lv_timer_t *timer)
{
    standby_page_context_t *page = lv_timer_get_user_data(timer);
    struct tm local_time = {0};
    char next_text[STANDBY_CLOCK_TEXT_BYTES] = "--:--";

    if (time_service_get_local_time(&local_time)) {
        if (strftime(next_text, sizeof(next_text), "%H:%M",
                     &local_time) == 0U) {
            memcpy(next_text, "--:--", sizeof(next_text));
        }
    }

    if (strcmp(page->clock_text, next_text) == 0) {
        return;
    }

    memcpy(page->clock_text, next_text, sizeof(page->clock_text));
    lv_label_set_text(page->clock_label, page->clock_text);
}

esp_err_t standby_page_start_with_scene(standby_scene_id_t scene_id)
{
    ESP_RETURN_ON_FALSE(!s_page.started, ESP_ERR_INVALID_STATE, TAG,
                        "Standby page is already started");
    ESP_RETURN_ON_FALSE(lv_display_get_default() != NULL,
                        ESP_ERR_INVALID_STATE, TAG,
                        "LVGL display is not initialized");

    const standby_scene_definition_t *scene = NULL;
    ESP_RETURN_ON_ERROR(
        standby_scene_resolve(scene_id, &scene), TAG,
        "Failed to resolve initial standby scene");
    ESP_RETURN_ON_ERROR(
        standby_prepare_frame_descriptors(), TAG,
        "Failed to prepare standby frames");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(0), ESP_FAIL, TAG,
                        "Failed to lock LVGL while creating standby page");

    lv_obj_t *screen = lv_screen_active();

    /*
     * 页面占满当前屏幕，背景和角色分别使用两个图片对象。背景先创建，
     * LVGL 会按对象创建顺序把角色绘制在场景之上；后续切换场景时只需替换
     * background 的图片源，不影响角色帧和动画定时器。
     */
    lv_obj_clean(screen);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(
        screen, lv_color_hex(STANDBY_FALLBACK_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    s_page.background = lv_image_create(screen);
    if (s_page.background == NULL) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }
    lv_image_set_src(s_page.background, scene->background);
    lv_obj_align(s_page.background, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(
        s_page.background, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_page.image = lv_image_create(screen);
    if (s_page.image == NULL) {
        lv_obj_delete(s_page.background);
        s_page.background = NULL;
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }

    s_page.scene_id = scene_id;
    s_page.frame_index = 0;
    lv_image_set_src(s_page.image, &s_frames[0]);
    lv_obj_center(s_page.image);
    lv_obj_remove_flag(
        s_page.image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /*
     * 时钟在角色之后创建，始终位于透明角色图片之上。固定 80 × 30 卡片只
     * 占用屏幕顶部一小块区域；背景使用半透明深棕色，即使角色头部经过
     * 卡片下方也能保持文字可读。
     */
    s_page.clock_panel = lv_obj_create(screen);
    if (s_page.clock_panel == NULL) {
        lv_obj_delete(s_page.image);
        lv_obj_delete(s_page.background);
        s_page.image = NULL;
        s_page.background = NULL;
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_size(
        s_page.clock_panel, STANDBY_CLOCK_PANEL_WIDTH,
        STANDBY_CLOCK_PANEL_HEIGHT);
    lv_obj_align(
        s_page.clock_panel, LV_ALIGN_TOP_MID, 0,
        STANDBY_CLOCK_PANEL_Y);
    lv_obj_remove_flag(
        s_page.clock_panel,
        LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(
        s_page.clock_panel, lv_color_hex(STANDBY_CLOCK_PANEL_BG),
        LV_PART_MAIN);
    lv_obj_set_style_bg_opa(
        s_page.clock_panel, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_color(
        s_page.clock_panel,
        lv_color_hex(STANDBY_CLOCK_PANEL_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(
        s_page.clock_panel, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_width(
        s_page.clock_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(
        s_page.clock_panel, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(
        s_page.clock_panel, 0, LV_PART_MAIN);

    s_page.clock_label = lv_label_create(s_page.clock_panel);
    if (s_page.clock_label == NULL) {
        lv_obj_delete(s_page.clock_panel);
        lv_obj_delete(s_page.image);
        lv_obj_delete(s_page.background);
        s_page.clock_panel = NULL;
        s_page.image = NULL;
        s_page.background = NULL;
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }
    memcpy(s_page.clock_text, "--:--", sizeof(s_page.clock_text));
    lv_label_set_text(s_page.clock_label, s_page.clock_text);
    lv_obj_set_style_text_color(
        s_page.clock_label,
        lv_color_hex(STANDBY_CLOCK_TEXT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_text_font(
        s_page.clock_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(s_page.clock_label);
    lv_obj_remove_flag(
        s_page.clock_label,
        LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /*
     * 定时器属于 LVGL 对象体系，由 LVGL 任务周期调用。400 ms 对应
     * 2.5 fps，一轮 7 帧约 2.8 秒，让轻微呼吸/眨眼动作保持舒缓。
     */
    s_page.animation_timer = lv_timer_create(
        standby_animation_timer_cb, STANDBY_FRAME_INTERVAL_MS, &s_page);
    if (s_page.animation_timer == NULL) {
        lv_obj_delete(s_page.clock_panel);
        lv_obj_delete(s_page.image);
        lv_obj_delete(s_page.background);
        s_page.clock_panel = NULL;
        s_page.clock_label = NULL;
        s_page.image = NULL;
        s_page.background = NULL;
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }

    s_page.clock_timer = lv_timer_create(
        standby_clock_timer_cb, STANDBY_CLOCK_INTERVAL_MS, &s_page);
    if (s_page.clock_timer == NULL) {
        lv_timer_delete(s_page.animation_timer);
        lv_obj_delete(s_page.clock_panel);
        lv_obj_delete(s_page.image);
        lv_obj_delete(s_page.background);
        s_page.animation_timer = NULL;
        s_page.clock_panel = NULL;
        s_page.clock_label = NULL;
        s_page.image = NULL;
        s_page.background = NULL;
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }

    s_page.started = true;
    lvgl_port_unlock();

    ESP_LOGI(
        TAG,
        "Standby page started: scene=%s, %d frames, %d ms/frame",
        scene->name, STANDBY_FRAME_COUNT, STANDBY_FRAME_INTERVAL_MS);
    return ESP_OK;
}

esp_err_t standby_page_start(void)
{
    return standby_page_start_with_scene(STANDBY_SCENE_INDOOR);
}

esp_err_t standby_page_set_scene(standby_scene_id_t scene_id)
{
    ESP_RETURN_ON_FALSE(s_page.started, ESP_ERR_INVALID_STATE, TAG,
                        "Standby page is not started");

    const standby_scene_definition_t *scene = NULL;
    ESP_RETURN_ON_ERROR(
        standby_scene_resolve(scene_id, &scene), TAG,
        "Failed to resolve standby scene");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(0), ESP_FAIL, TAG,
                        "Failed to lock LVGL while switching scene");

    lv_image_set_src(s_page.background, scene->background);
    s_page.scene_id = scene_id;

    lvgl_port_unlock();
    ESP_LOGI(TAG, "Standby scene switched to: %s", scene->name);
    return ESP_OK;
}
