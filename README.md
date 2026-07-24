# ESP32-S3-Touch-LCD-1.54

这是一个面向初学者的纯 ESP-IDF 项目，适用于 Waveshare
`ESP32-S3-Touch-LCD-1.54` 触控版开发板。

项目以 ESP-IDF 组件形式组织板级配置、显示、待机页面、网络、校时和应用
入口，使用 `esp_lcd` 与 LVGL 驱动 240 × 240 ST7789 屏幕。

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

待机页顶部显示北京时间 `HH:MM`。设备开机后先显示 `--:--`，连接
2.4 GHz Wi-Fi 并通过 SNTP 完成校时后自动切换为真实时间。Wi-Fi 在后台
保持连接并启用 Modem-sleep，路由器暂时离线时按退避策略自动重连，不会
阻塞角色动画。

使用锂电池时，按下 PWR 会临时启动供电，程序随即通过 GPIO2 保持电源，
松开按键后可继续运行。开机后再次长按 PWR 约 2 秒会关闭显示并撤销电池
供电保持；若 USB 仍连接，板卡仍会由 USB 供电。

## Wi-Fi 与时间配置

本阶段使用编译期配置连接单个 WPA2/WPA3 Personal 网络。ESP32-S3 只支持
2.4 GHz Wi-Fi，不能连接仅开启 5 GHz 的 SSID。

在 ESP-IDF PowerShell 中打开配置界面：

```powershell
idf.py menuconfig
```

然后设置：

- `Pet Network > Wi-Fi SSID`
- `Pet Network > Wi-Fi password`
- `Pet Time > NTP server`

NTP 服务器默认是 `pool.ntp.org`，也可以填写自建服务器的主机名或文本 IP
地址，例如 `192.168.1.10`。留空会禁用网络校时，页面会保持 `--:--`。
修改任一配置后需要重新构建并烧录：

```powershell
idf.py build
idf.py -p <PORT> flash monitor
```

SSID、密码和 NTP 地址会保存在本机生成的 `sdkconfig` 中并编译进固件。
`sdkconfig` 已被 `.gitignore` 忽略，不要把密码复制到源码、
`sdkconfig.defaults`、日志或提交记录中。该方式只用于第一阶段开发，后续
可替换为 BLE 配网和 NVS 凭据管理。

开发约定和已验证的硬件信息见 [AGENTS.md](AGENTS.md)。
