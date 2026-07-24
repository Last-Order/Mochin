# ESP32-S3-Touch-LCD-1.54

这是一个面向初学者的纯 ESP-IDF 项目，适用于 Waveshare
`ESP32-S3-Touch-LCD-1.54` 触控版开发板。

项目以 ESP-IDF 组件形式组织板级配置、显示、待机页面和应用入口，使用
`esp_lcd` 与 LVGL 驱动 240 × 240 ST7789 屏幕。

当前功能是桌宠待机页：从 `src/standby/assets/purin_spritesheet.webp` 的
第一排按 192 × 192 网格裁切，跳过第 8 个空帧，将前 7 帧循环播放。生成
工具把 WebP 预处理为 LVGL 可直接绘制的 RGB565+A8 资源，设备运行时无需
WebP 解码器。开发约定和已验证的硬件信息见 [AGENTS.md](AGENTS.md)。
