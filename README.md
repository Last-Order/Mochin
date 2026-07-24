# ESP32-S3-Touch-LCD-1.54

这是一个面向初学者的纯 ESP-IDF 项目，适用于 Waveshare
`ESP32-S3-Touch-LCD-1.54` 触控版开发板。

项目以 ESP-IDF 组件形式组织板级配置、显示、待机页面和应用入口，使用
`esp_lcd` 与 LVGL 驱动 240 × 240 ST7789 屏幕。

当前功能是带场景的桌宠待机页，默认使用温馨木质小房子的“屋内”场景。
场景背景和角色动画彼此独立：背景来自
`src/standby/assets/scene_indoor.png`，角色从
`src/standby/assets/purin_spritesheet.webp` 第一排按 192 × 208 裁切，
跳过第 8 个空帧并循环播放前 7 帧。生成工具把它们预处理为 LVGL 可直接
绘制的 RGB565 与 RGB565+A8 资源，设备运行时无需 PNG/WebP 解码器。
源图发生变化后可运行
`python src/standby/tools/generate_idle_asset.py` 重新生成全部待机资源。

场景通过 `standby_scene_id_t` 和组件内部注册表管理。默认启动接口保留，
也可使用 `standby_page_start_with_scene()` 指定初始场景，或在页面运行后
通过 `standby_page_set_scene()` 只替换背景；新增场景无需复制角色动画
逻辑。

使用锂电池时，按下 PWR 会临时启动供电，程序随即通过 GPIO2 保持电源，
松开按键后可继续运行。开机后再次长按 PWR 约 2 秒会关闭显示并撤销电池
供电保持；若 USB 仍连接，板卡仍会由 USB 供电。

开发约定和已验证的硬件信息见 [AGENTS.md](AGENTS.md)。
