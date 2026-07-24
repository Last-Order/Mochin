/*
 * 待机场景注册表与背景资源描述符。
 *
 * 每个场景在注册表中集中绑定“稳定标识、日志名称、背景图片和资源末地址”。
 * 页面代码只解析定义并设置 LVGL 图片源，因此以后增加新场景时不需要复制
 * 对象创建、角色动画或定时器逻辑。
 */

#include "standby_scene_internal.h"

#include <stddef.h>
#include <stdint.h>

#include "esp_check.h"
#include "esp_log.h"

#define STANDBY_SCENE_WIDTH 240
#define STANDBY_SCENE_HEIGHT 240
#define STANDBY_SCENE_STRIDE \
    (STANDBY_SCENE_WIDTH * sizeof(uint16_t))
#define STANDBY_SCENE_ASSET_BYTES \
    (STANDBY_SCENE_STRIDE * STANDBY_SCENE_HEIGHT)

/*
 * 背景以 LVGL 原生 RGB565、小端字节序存放在 Flash 映射区。显示组件会在
 * 最终 SPI flush 时统一完成 ST7789 所需的字节交换，这里不得预先交换。
 */
extern const uint8_t scene_indoor_asset_start[]
    asm("_binary_scene_indoor_rgb565_start");
extern const uint8_t scene_indoor_asset_end[]
    asm("_binary_scene_indoor_rgb565_end");

static const char *TAG = "standby_scene";

static const lv_image_dsc_t s_indoor_background = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RGB565,
        .flags = 0,
        .w = STANDBY_SCENE_WIDTH,
        .h = STANDBY_SCENE_HEIGHT,
        .stride = STANDBY_SCENE_STRIDE,
        .reserved_2 = 0,
    },
    .data_size = STANDBY_SCENE_ASSET_BYTES,
    .data = scene_indoor_asset_start,
    .reserved = NULL,
    .reserved_2 = NULL,
};

/**
 * @brief 注册表内部条目。
 *
 * asset_end 只用于启动时校验链接器嵌入文件是否完整，不暴露给页面层。
 */
typedef struct {
    standby_scene_definition_t definition;
    const uint8_t *asset_end;
} standby_scene_registry_entry_t;

static const standby_scene_registry_entry_t s_scene_registry[] = {
    {
        .definition = {
            .id = STANDBY_SCENE_INDOOR,
            .name = "indoor",
            .background = &s_indoor_background,
        },
        .asset_end = scene_indoor_asset_end,
    },
};

/*
 * 场景枚举按连续值维护。新增枚举但忘记注册资源会在编译期失败，而不是设备
 * 运行后悄悄退回错误背景。
 */
_Static_assert(
    STANDBY_SCENE_COUNT ==
        sizeof(s_scene_registry) / sizeof(s_scene_registry[0]),
    "Every standby scene must have one registry entry");

esp_err_t standby_scene_resolve(
    standby_scene_id_t id,
    const standby_scene_definition_t **definition)
{
    ESP_RETURN_ON_FALSE(
        definition != NULL, ESP_ERR_INVALID_ARG, TAG,
        "Scene definition output pointer is null");

    for (size_t i = 0;
         i < sizeof(s_scene_registry) / sizeof(s_scene_registry[0]);
         ++i) {
        const standby_scene_registry_entry_t *entry =
            &s_scene_registry[i];
        if (entry->definition.id != id) {
            continue;
        }

        const size_t asset_size =
            (size_t)(entry->asset_end -
                     entry->definition.background->data);
        ESP_RETURN_ON_FALSE(
            asset_size == entry->definition.background->data_size,
            ESP_ERR_INVALID_SIZE, TAG,
            "Scene '%s' asset size is %u bytes, expected %u",
            entry->definition.name, (unsigned int)asset_size,
            (unsigned int)entry->definition.background->data_size);

        *definition = &entry->definition;
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Unknown standby scene id: %d", (int)id);
    return ESP_ERR_NOT_FOUND;
}
