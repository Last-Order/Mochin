#pragma once

/*
 * 待机场景的稳定标识。
 *
 * 场景只描述页面环境，角色动画仍由 standby_page 统一管理。后续增加场景时，
 * 在本枚举中追加标识，并在 standby_scene.c 的注册表中提供对应背景资源；
 * 页面创建和动画切帧逻辑不需要随场景数量增长而增加分支。
 */

/**
 * @brief 待机页面支持的场景标识。
 *
 * 枚举值可用于页面启动和运行时切换。STANDBY_SCENE_COUNT 是注册表完整性
 * 检查所用的哨兵值，不可作为实际场景传入。
 */
typedef enum {
    /** 温馨木质小房子的屋内场景，也是默认场景。 */
    STANDBY_SCENE_INDOOR = 0,

    /** 场景数量，仅用于边界和注册表检查。 */
    STANDBY_SCENE_COUNT,
} standby_scene_id_t;
