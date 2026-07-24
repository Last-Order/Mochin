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

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#define STANDBY_FRAME_WIDTH 192
#define STANDBY_FRAME_HEIGHT 208
#define STANDBY_FRAME_COUNT 7
#define STANDBY_FRAME_INTERVAL_MS 400
/*
 * 208 像素帧底部含 6 行透明余量。对象底部距屏幕 10 像素时，实际脚底仍
 * 统一落在第 223 行附近，同时保留少量前景地板。
 */
#define STANDBY_CHARACTER_GROUND_MARGIN 10
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
    lv_timer_t *timer;
    standby_scene_id_t scene_id;
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
    /*
     * 角色不再按外框几何中心摆放，而是以固定脚底基线对齐地面。所有帧
     * 尺寸一致，因此呼吸和眨眼切帧不会改变站立位置；底部余量也让完整
     * 脚部与屋内地板之间形成清晰接触关系。
     */
    lv_obj_align(
        s_page.image, LV_ALIGN_BOTTOM_MID, 0,
        -STANDBY_CHARACTER_GROUND_MARGIN);
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
        lv_obj_delete(s_page.background);
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
