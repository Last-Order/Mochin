#pragma once

/*
 * standby 组件负责待机页面及其逐帧动画。
 *
 * 页面依赖已经初始化的 LVGL 显示端口；它不初始化 LCD，也不直接操作 SPI、
 * 面板或背光。场景负责提供环境背景，角色动画在所有场景间复用。
 */

#include "esp_err.h"
#include "standby/standby_scene.h"

/**
 * @brief 使用默认的屋内场景创建待机页面并启动循环动画。
 *
 * 必须在 lcd_display_init() 之后、lcd_display_enable() 之前调用。函数会
 * 创建场景背景、角色图片和 LVGL 定时器，并立即准备首帧；整个程序只能
 * 调用一次。等价于 standby_page_start_with_scene(STANDBY_SCENE_INDOOR)。
 *
 * @return ESP_OK 成功；资源不完整、内存不足或调用顺序错误时返回对应错误。
 */
esp_err_t standby_page_start(void);

/**
 * @brief 使用指定场景创建待机页面并启动循环动画。
 *
 * 必须在 lcd_display_init() 之后、lcd_display_enable() 之前调用，且整个
 * 程序只能调用一次。scene_id 必须是已注册场景，背景资源会在创建 LVGL
 * 对象前完成大小校验。
 *
 * @param scene_id 初始场景标识。
 *
 * @return ESP_OK 成功；场景无效、资源不完整、内存不足或调用顺序错误时
 *         返回对应错误。
 */
esp_err_t standby_page_start_with_scene(standby_scene_id_t scene_id);

/**
 * @brief 在待机页面运行期间切换场景背景。
 *
 * 必须在页面启动后从普通任务上下文调用，不可在 LVGL 定时器或事件回调中
 * 调用。函数取得 LVGL port 锁，只替换背景图片，角色的当前帧和动画节奏
 * 保持不变。
 *
 * @param scene_id 目标场景标识。
 *
 * @return ESP_OK 成功；页面未启动、场景无效、资源损坏或无法取得 LVGL
 *         锁时返回对应错误。
 */
esp_err_t standby_page_set_scene(standby_scene_id_t scene_id);
