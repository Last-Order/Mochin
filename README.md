# ESP32-S3-Touch-LCD-1.54 按键显示程序

这是一个面向初学者的纯 ESP-IDF 项目，适用于 Waveshare
`ESP32-S3-Touch-LCD-1.54`。程序使用 ESP-IDF 的 `esp_lcd` API 驱动
240 × 240 ST7789 屏幕，使用 LVGL 绘制界面，并监听三个板载按键。按下
按键后，屏幕会显示其丝印名称：`BOOT`、`PWR` 或 `PLUS`。

当前功能与引入 LVGL 前保持一致：开机显示 `PRESS A BUTTON`，三个按键
分别使用黄、品红、绿色强调色。LVGL 目前只接管文字、边框、布局、部分刷新
和绘制缓冲生命周期，为后续桌面宠物的图片与动画打基础。

## 目录结构

```text
.
├── CMakeLists.txt                 # ESP-IDF 工程入口，声明 src/ 组件目录
├── sdkconfig.defaults             # ESP32-S3、Flash、PSRAM 等默认配置
├── AGENTS.md                      # 板卡资料和项目开发约束
├── README.md
└── src/                           # 本项目自己的全部程序代码
    ├── app/                       # 应用层：只负责组织业务流程
    │   ├── CMakeLists.txt
    │   └── app_main.c
    ├── board/                     # 板级层：统一保存 GPIO 和硬件参数
    │   ├── CMakeLists.txt
    │   └── include/board/
    │       └── board_config.h
    ├── display/                   # 显示层：LCD 初始化、LVGL 界面和 DMA 刷新
    │   ├── CMakeLists.txt
    │   ├── idf_component.yml      # 锁定 LVGL 与 ESP 适配层版本
    │   ├── app_display.c
    │   └── include/display/
    │       └── app_display.h
    └── input/                     # 输入层：按键 GPIO、轮询和软件消抖
        ├── CMakeLists.txt
        ├── board_buttons.c
        └── include/input/
            └── board_buttons.h
```

模块依赖保持单向：

```text
app ──> display ──> board
 └────> input   ──> board
```

`app` 只调用其他模块的公开接口，不直接操作 GPIO。`display` 和 `input`
可以读取 `board` 中的硬件配置，但 `board` 不依赖上层模块。这样以后增加
触控、IMU、存储或网络功能时，可以继续在 `src/` 下建立独立组件，而不必
把所有逻辑堆进 `app_main.c`。

公开头文件放在组件的 `include/<模块名>/` 下，引用时带模块前缀，例如：

```c
#include "display/app_display.h"
#include "input/board_buttons.h"
```

## 建议阅读顺序

1. 从 `src/app/app_main.c` 查看程序入口和模块初始化顺序。
2. 阅读 `display/app_display.h` 和 `input/board_buttons.h`，了解应用层可以
   调用哪些公开接口。
3. 阅读 `board_buttons.c`，理解 FreeRTOS 轮询任务和软件消抖。
4. 阅读 `app_display.c`，理解 LCD 初始化、LVGL 对象、部分绘制缓冲和
   DMA 传输。
5. 最后查看 `board/board_config.h`，对应具体 GPIO 和硬件参数。

显示组件内部继续使用已经实物验证的 `esp_lcd` ST7789 初始化序列。LVGL
并不直接操作 GPIO 或 SPI，而是通过 `esp_lvgl_port` 把需要更新的区域交给
`esp_lcd`。当前锁定的组件版本为：

- LVGL `9.5.0`
- `espressif/esp_lvgl_port` `2.8.0~1`

首次配置或清理 `managed_components/` 后构建时，ESP-IDF 组件管理器会自动
下载这些依赖；不要手工修改 `managed_components/` 中的生成内容。

## 开发环境

- ESP-IDF 5.5.0 或更高版本
- 当前工程已经使用 ESP-IDF 6.0.2 验证
- 支持数据传输的 USB-C 线
- 板卡当前实际使用的 Windows COM 口

## 构建和烧录

在 ESP-IDF PowerShell 中进入项目根目录，然后执行：

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

`set-target` 通常只在首次配置或清理配置后执行。`<PORT>` 必须替换为当前
实际串口，例如 `COM5`；重新插拔 USB 后应再次确认串口号。

使用 `Ctrl+]` 退出串口监视器。如果无法进入下载模式，可以按住 `BOOT`，
重新连接 USB，松开 `BOOT` 后再次执行烧录命令。

## LCD 引脚

| 信号 | GPIO |
| --- | ---: |
| DC | 45 |
| CS | 21 |
| SCLK | 38 |
| MOSI | 39 |
| RESET | 40 |
| 背光 | 46 |

## 按键引脚

| 按键 | GPIO | 有效电平 |
| --- | ---: | ---: |
| BOOT | 0 | 低电平 |
| PWR | 5 | 低电平 |
| PLUS | 4 | 低电平 |

工程默认按照 ESP32-S3R8 配置 16 MB QIO Flash、8 MB Octal PSRAM，并使用
USB Serial/JTAG 输出串口日志。LVGL 当前使用两个 240 × 24 像素的 RGB565
DMA 绘制缓冲，总计约 23 KB；界面发生变化时只刷新脏区域，不再维护和发送
一块由应用代码手工绘制的整屏帧缓冲。
