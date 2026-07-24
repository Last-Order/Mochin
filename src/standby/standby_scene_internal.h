#pragma once

/*
 * standby 组件内部的场景注册表接口。
 *
 * 该接口刻意不放入 public include 目录：应用层只使用稳定的场景标识，
 * 背景图片描述符和嵌入资源校验仍由 standby 组件独占。
 */

#include "esp_err.h"
#include "lvgl.h"
#include "standby/standby_scene.h"

/**
 * @brief 页面渲染所需的一份只读场景定义。
 */
typedef struct {
    /** 稳定场景标识。 */
    standby_scene_id_t id;
    /** 用于日志和诊断的英文短名称。 */
    const char *name;
    /** 覆盖完整屏幕的 LVGL RGB565 背景图片。 */
    const lv_image_dsc_t *background;
} standby_scene_definition_t;

/**
 * @brief 从注册表解析并校验场景。
 *
 * @param id 要查找的场景标识。
 * @param[out] definition 返回静态生命周期的只读场景定义。
 *
 * @return ESP_OK 成功；标识无效、输出指针为空或嵌入资源损坏时返回错误。
 */
esp_err_t standby_scene_resolve(
    standby_scene_id_t id,
    const standby_scene_definition_t **definition);
