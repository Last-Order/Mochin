# ESP32-S3-Touch-LCD-1.54 开发指引

## 项目概况

本仓库面向 Waveshare `ESP32-S3-Touch-LCD-1.54` 触控版，是一个适合
初学者阅读和扩展的纯 ESP-IDF 工程。

- 仅支持 `esp32s3`，不要改成 Arduino 工程或引入 Arduino compatibility
  layer。
- 自有代码按 ESP-IDF 组件拆分，统一放在 `src/`。
- 官方要求 ESP-IDF v5.5.0 或更高；本仓库已使用 v6.0.2 编译和运行。
- 引入旧示例或第三方组件时，必须检查 ESP-IDF v6 API 兼容性。

## 目录结构

```text
.
├── CMakeLists.txt          # 工程入口，固定目标并注册 src/ 组件
├── sdkconfig.defaults      # Flash、PSRAM、控制台等稳定默认配置
├── partitions.csv          # 16 MB Flash 自定义分区表
├── README.md               # 项目简介
├── AGENTS.md               # 开发约定和已验证结论
└── src/
    ├── app/                # 应用入口，只负责编排组件
    ├── board/              # 板级引脚和硬件参数
    ├── display/            # LCD 与图形显示
    └── standby/            # 待机页面、动画与页面资源
```

组件依赖保持单向：

```text
app ──> display ──> board
 └────> standby ──> LVGL
```

新增功能应在 `src/` 下建立职责单一的组件，不要把实现集中到
`app_main.c`。公开头文件放在 `include/<组件名>/`，并使用带组件前缀的
引用形式，例如 `#include "display/lcd_display.h"`。

`sdkconfig`、`sdkconfig.old`、`build/` 和 `managed_components/` 是生成内容，
不要提交或手工修改。稳定配置写入 `sdkconfig.defaults`，第三方依赖写入
对应组件的 `idf_component.yml`。

## 已确认的硬件基线

- MCU：ESP32-S3R8，双核 Xtensa LX7，最高 240 MHz
- Flash：16 MB，QIO、80 MHz
- PSRAM：8 MB Octal PSRAM、80 MHz，已加入 malloc heap
- USB：板载 Type-C 连接原生 USB Serial/JTAG
- LCD：1.54 英寸、240 × 240、ST7789、四线 SPI、RGB565
- 触控：CST816 系列电容触控，I2C
- 其他板载外设：QMI8658、ES8311/ES7210、麦克风、扬声器接口、
  Micro SD、按键和电池接口

已核对的引脚如下：

| 功能 | 信号 | GPIO | 备注 |
| --- | --- | ---: | --- |
| LCD | SCLK | 38 | SPI2_HOST |
| LCD | MOSI | 39 | 无需 MISO |
| LCD | RESET | 40 | 低电平复位 |
| LCD | DC | 45 | 命令/数据选择 |
| LCD | CS | 21 | 片选 |
| LCD | Backlight | 46 | 高电平点亮 |
| Touch | I2C SCL | 41 | 官方示例为 400 kHz |
| Touch | I2C SDA | 42 | 官方示例使用 I2C port 0 |
| Touch | INT | 48 | 触控中断 |
| Touch | RESET | 47 | 触控复位 |
| Button | BOOT | 0 | 低电平有效 |
| Button | PWR | 5 | 低电平有效 |
| Button | PLUS | 4 | 低电平有效 |

未列出的 SD、IMU、音频和电源管理引脚，必须从本板原理图或 Waveshare
官方 ESP-IDF 示例核对。不要套用其他 ESP32-S3 板卡的引脚，也不要把板载
外设占用的 GPIO 当作普通空闲引脚。

## 有用的已验证结论

- LCD 使用 `SPI2_HOST`，240 × 240 RGB565，SPI pixel clock 为 40 MHz。
- ST7789 需要开启颜色反转；当前 SPI mode 和初始化序列已通过实物验证。
- SPI DMA 缓冲必须使用 DMA 兼容内存；异步传输完成前不得释放、覆盖或
  复用正在发送的缓冲区。
- 当前锁定 LVGL `9.5.0` 和 `espressif/esp_lvgl_port` `2.8.0~1`。
- 自定义分区表包含 24 KiB NVS、4 KiB PHY、8 MiB factory 应用分区和
  256 KiB coredump 分区；当前不启用 OTA。
- 修改 LCD 的 swap/mirror 或显示方向时，必须同步验证触控坐标变换。

## 开发环境与常用命令

优先使用 ESP-IDF PowerShell。当前机器也可在普通 PowerShell 中激活：

```powershell
. C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1
cd D:\Git\ESP32-S3-Touch-LCD-1.54
idf.py --version
```

常规流程：

```powershell
idf.py build
idf.py -p <PORT> flash monitor
```

仅在首次配置目标或清理配置后运行：

```powershell
idf.py set-target esp32s3
```

COM 口不是板卡的固定属性，重新连接或更换 USB 口后必须重新确认。
`Ctrl+]` 可退出监视器。若无法进入下载模式，可按住 BOOT 重新连接 USB，
松开 BOOT 后再次烧录。

当前 Codex 终端曾同时存在 `PATH` 和 `Path`，导致 CMake 找不到 Ninja 或
编译器。遇到类似问题先检查：

```powershell
cmake -E environment | Select-String '^(PATH|Path)='
```

应改用干净的 ESP-IDF PowerShell 环境，不要把本机工具链绝对路径写入工程。

## 代码约定

- 使用 ESP-IDF 原生 API；LCD 优先使用 `esp_lcd`，GPIO、SPI、I2C 使用
  当前 IDF 的新驱动接口。
- 根目录只保留工程入口、配置和文档；不要新增根级 `main/` 或
  `components/`。
- 板级引脚集中放在 `src/board`，其他组件不得重复维护 GPIO 定义。
- 新增或修改 C/C++、头文件和 CMake 时提供充分、准确的中文注释。
- 文件头说明模块职责和依赖关系；公开接口使用中文 Doxygen 注释说明参数、
  返回值、调用顺序和运行上下文。
- 注释重点解释初始化顺序、有效电平、任务与回调、DMA 生命周期、并发同步
  和错误处理背后的原因，避免逐行翻译代码。
- 初始化阶段检查返回值，优先使用 `ESP_ERROR_CHECK`、
  `ESP_RETURN_ON_ERROR` 等宏。
- 第三方组件通过组件管理器引入并锁定已验证版本。
- 各硬件组件记录清晰的 `ESP_LOGI` 初始化里程碑。
- 大图像和大块图形缓冲优先放入 PSRAM。
- 密码、API Key 等秘密不得写入仓库。
- 保持根 `CMakeLists.txt` 中的 `IDF_TARGET=esp32s3` 和
  `SUPPORTED_TARGETS=esp32s3`。

## 交付前验证

1. `idf.py build` 无错误完成。
2. 涉及硬件行为且用户允许时，使用重新确认的 `<PORT>` 烧录。
3. 检查启动日志中的芯片目标、Flash、PSRAM 和各组件初始化结果。
4. 显示功能以实物画面为准，串口日志不能替代观察。
5. 触控功能至少验证四角、中心、坐标方向和旋转映射。
6. 记录实际 ESP-IDF 版本、COM 口和验证结果。

除非用户明确要求，不要运行 `erase-flash`，以免额外清除 NVS 等数据。

## 权威资料

- [产品主页](https://docs.waveshare.net/ESP32-S3-Touch-LCD-1.54/)
- [ESP-IDF 开发环境](https://docs.waveshare.net/ESP32-S3-Touch-LCD-1.54/Development-Environment-Setup-ESPIDF/)
- [使用与固件恢复](https://docs.waveshare.net/ESP32-S3-Touch-LCD-1.54/Instructions-For-Use/)
- [原理图与器件资料](https://docs.waveshare.net/ESP32-S3-Touch-LCD-1.54/Resources-And-Documents/)
- [Waveshare 官方示例](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54)
