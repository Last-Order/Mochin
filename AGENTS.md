# ESP32-S3-Touch-LCD-1.54 开发指引

## 适用范围

本仓库面向 Waveshare `ESP32-S3-Touch-LCD-1.54` 触控版，使用纯
ESP-IDF 开发。不要把它改回 Arduino 工程，也不要把 Arduino 库混入
ESP-IDF 的组件依赖。

当前工程是一个已经在实物上验证过的最小 LCD Hello World：

- 工程入口：`main/main.c`
- 构建目标：`esp32s3`
- 本机验证环境：Windows、ESP-IDF v6.0.2、开发板端口 COM5
- 已验证结果：编译、烧录、8 MB PSRAM 自检、ST7789 初始化和画面显示均成功

COM 口编号不是板卡的固定属性。断开/重新连接或更换 USB 口后应重新确认，
不要在新环境中盲目沿用 COM5。

## 权威资料

开发前优先查以下官方资料，不要凭其他 ESP32-S3 板卡的引脚定义猜测：

- 产品主页：
  <https://docs.waveshare.net/ESP32-S3-Touch-LCD-1.54/>
- ESP-IDF 开发说明：
  <https://docs.waveshare.net/ESP32-S3-Touch-LCD-1.54/Development-Environment-Setup-ESPIDF/>
- 使用说明、出厂固件与烧录方法：
  <https://docs.waveshare.net/ESP32-S3-Touch-LCD-1.54/Instructions-For-Use/>
- 原理图、器件手册和示例程序入口：
  <https://docs.waveshare.net/ESP32-S3-Touch-LCD-1.54/Resources-And-Documents/>
- Waveshare 官方示例仓库：
  <https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54>

Waveshare 的通用 ESP-IDF 教程使用其他板卡演示；复制其中的硬件代码前，
必须按本板原理图和官方示例重新核对 GPIO。

## 板卡基线

- MCU：ESP32-S3R8，双核 Xtensa LX7，最高 240 MHz
- Flash：16 MB，当前工程使用 QIO、80 MHz
- PSRAM：8 MB Octal PSRAM，当前工程使用 80 MHz 并加入 malloc heap
- USB：板载 Type-C 连接 ESP32-S3 原生 USB Serial/JTAG
- LCD：1.54 英寸、240 × 240、ST7789、四线 SPI、RGB565
- 触控：CST816 系列电容触控，I2C，仅触控版具备
- 其他板载外设：QMI8658 六轴 IMU、ES8311/ES7210 音频、麦克风、
  扬声器接口、Micro SD、按键、电池充放电接口

官方要求 ESP-IDF v5.5.0 或更高版本；官方示例目录以 v5.5.1 为基线，
文档安装截图以 v5.5.2 为例。本仓库已使用 v6.0.2 成功编译和运行。
引入官方旧示例或第三方组件时仍需检查其与 IDF v6 的 API 兼容性。

## 已确认的 LCD 与触控引脚

| 功能 | 信号 | GPIO | 备注 |
| --- | --- | ---: | --- |
| LCD | SCLK | 38 | 当前工程 SPI2_HOST |
| LCD | MOSI | 39 | LCD 无需 MISO |
| LCD | RESET | 40 | 低电平复位 |
| LCD | DC | 45 | 命令/数据选择 |
| LCD | CS | 21 | 片选 |
| LCD | Backlight | 46 | 高电平点亮 |
| Touch | I2C SCL | 41 | 官方示例 400 kHz |
| Touch | I2C SDA | 42 | 官方示例 I2C port 0 |
| Touch | INT | 48 | 触控中断 |
| Touch | RESET | 47 | 触控复位 |

当前 LCD 参数已经在实物上验证：

- 240 × 240、RGB565、40 MHz SPI pixel clock
- ST7789 颜色反转开启
- 帧缓冲区使用 DMA 兼容内存
- 当前初始化序列和 SPI mode 已能正常显示；修改时必须重新做实物验证

未列出的按键、SD、IMU、音频和电源管理引脚，必须从原理图或对应的
Waveshare 官方 ESP-IDF 示例中确认后再使用。不要把板载外设占用的 GPIO
当成普通空闲 GPIO。

## Windows 开发环境

优先从“ESP-IDF PowerShell”启动工程。当前机器也可以在普通 PowerShell
中手动激活：

```powershell
. C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1
cd D:\Git\ESP32-S3-Touch-LCD-1.54
idf.py --version
```

预期版本为 `ESP-IDF v6.0.2`。上述绝对路径只描述当前机器；不要将工具链
绝对路径写进 `CMakeLists.txt` 或源代码。

当前 Codex 终端曾出现同时存在 `PATH` 和 `Path` 两个环境变量的问题：
PowerShell 能找到 Ninja/编译器，但 CMake 读取了另一份路径。如果再次出现
“找不到 Ninja”或“找不到 xtensa-esp32s3-elf-gcc”，先检查：

```powershell
cmake -E environment | Select-String '^(PATH|Path)='
```

应使用干净的子进程环境或重新打开 ESP-IDF PowerShell，使子进程只继承一份
正确 PATH。不要通过硬编码编译器位置来污染项目配置。

## 构建、烧录与监视

常规流程：

```powershell
idf.py build
idf.py -p COM5 flash monitor
```

仅当首次配置目标或清理过配置时运行：

```powershell
idf.py set-target esp32s3
```

`set-target` 会重新生成 `sdkconfig`，因此板卡的稳定配置应保存在
`sdkconfig.defaults`。`sdkconfig`、`sdkconfig.old`、`build/` 和
`managed_components/` 都是生成内容，不要提交。

退出串口监视器：

```text
Ctrl + ]
```

若无法进入下载模式：

1. 按住 BOOT。
2. 连接或重新连接 USB。
3. 松开 BOOT。
4. 重新执行烧录命令。
5. 下载完成后复位或重新上电。

烧录前关闭 Arduino 串口监视器、其他串口工具和残留的 `idf.py monitor`，
否则 COM 口可能被占用。除非用户明确要求，不要运行 `erase-flash`；
它会额外清除 NVS、Wi-Fi 配置等数据。

## 修改代码时的约定

- 使用 ESP-IDF 原生 API，LCD 优先使用 `esp_lcd`，GPIO/SPI/I2C 使用当前
  IDF 的新驱动接口。
- 新组件通过组件管理器和 `main/idf_component.yml` 声明，并锁定经过验证的
  版本；不要随意引入整个 Arduino compatibility layer。
- 初始化阶段检查每个返回值，优先使用 `ESP_ERROR_CHECK`、
  `ESP_RETURN_ON_ERROR` 等 IDF 宏。
- 为 LCD、触控、网络和存储分别记录清晰的 `ESP_LOGI` 启动里程碑，方便仅靠
  串口判断故障发生在哪一层。
- 大图像或 LVGL 缓冲优先放入 PSRAM；SPI DMA 缓冲必须具备 DMA 能力。
- 异步 LCD 传输完成前，不得释放、覆盖或复用正在发送的缓冲区。
- 修改显示方向时，LCD 的 swap/mirror 与触控坐标变换必须一起验证。
- Wi-Fi 密码、API Key 等秘密不得写进仓库；使用未提交的本地配置或安全存储。
- 保持 `CMakeLists.txt` 中的 `IDF_TARGET=esp32s3` 和
  `SUPPORTED_TARGETS=esp32s3`。

## 推荐的功能扩展路径

官方 ESP-IDF 示例可作为对应功能的参考：

1. `02_button_example`：按键单击、双击和长按。
2. `03_qmi8658_example`：读取加速度计、陀螺仪、温度和时间戳。
3. `04_sd_card_test`：SDMMC 与 FAT32 文件系统。
4. `05_lvgl_example`：LCD、CST816 触控和 LVGL；官方示例支持 LVGL v8/v9，
   默认示范 v9.3.0。
5. `01_factory`：综合硬件与出厂演示。
6. `06_esp-brookesia`：触控版 App 风格 UI。

引入 LVGL 时，先完成“纯色/文字显示 → 触控原始坐标 → LVGL 显示 →
LVGL 触控输入”的分层验证，不要一次性同时接入所有外设。

## 每次交付前的最低验证

1. `idf.py build` 必须无错误完成。
2. 涉及硬件行为且用户允许烧录时，执行 `idf.py -p <PORT> flash`。
3. 检查启动日志中芯片目标、Flash/PSRAM 和 `app_main()` 是否正常。
4. 显示功能必须观察实物画面；串口打印“绘制完成”不能替代屏幕检查。
5. 触控功能至少验证四角、中心、坐标方向和旋转后的映射。
6. 记录实际 ESP-IDF 版本、COM 口和验证结果。

当前 Hello World 的已知成功日志包含：

```text
esp_psram: SPI SRAM memory test OK
lcd_hello_world: Initializing Waveshare ESP32-S3-Touch-LCD-1.54
lcd_hello_world: Hello World is now displayed
```

## 出厂固件恢复

ESP32 没有桌面系统式的“系统内应用安装”。常规烧录会替换 bootloader、
分区表和应用镜像；芯片 ROM 下载程序仍保留，所以可以随时重新烧录。

恢复 Waveshare 触控版出厂固件时，使用官方示例包中
`ESP32-S3-Touch-LCD-1.54-Demo/Firmware` 的合并 `.bin`，按官方说明使用
Flash Download Tool，选择 `ESP32-S3`、`USB`、实际 COM 口，并将镜像地址
设为 `0x00`。必须确认选择的是带 `Touch` 的固件。
