<div align="center">

[English](../../../en/esp32s3/t-eth-elite-s3/README.MD) | [中文](./README.MD)

</div>

<div align="center" markdown="1">
  <img src="../../../../.github/LilyGo_logo.png" alt="LilyGo logo" width="100"/>
</div>

<h1 align = "center">🌟LilyGo T-ETH-Elite ESP32S3 版本🌟</h1>

## 概述

* This page introduces the hardware parameters related to `LilyGo T-ETH-Elite ESP32S3 版本`

### 产品

| Product                     | SOC              | Flash          | PSRAM    |
| --------------------------- | ---------------- | -------------- | -------- |
| [T-ETH-Elite Main board][1] | ESP32-S3-WROOM-1 | 16MB(Quad-SPI) | 8MB(OPI) |

[1]: https://lilygo.cc/products/a-t-pcie?variant=43043718725813

### PlatformIO 快速入门

1. 安装 [Visual Studio Code](https://code.visualstudio.com/) 和 [Python](https://www.python.org/)
2. 在 `Visual Studio Code` 扩展中搜索 `PlatformIO` 插件并安装。
3. 安装完成后，需要重启 `Visual Studio Code`
4. 重启 `Visual Studio Code` 后，选择左上角的 `文件` -> `打开文件夹` -> 选择 `LilyGo-Modem-Series` 目录
5. 等待第三方依赖库安装完成
6. 点击 `platformio.ini` 文件，在 `platformio` 栏中
7. Cancel the `;` symbol in front of the line `default_envs = T-ETH-ELITE-XXXX` and use `T-ETH-ELITE-XXXX` as the default environment, Name **XXXX** Please select according to the actual version
8. 取消注释 `src_dir = xxxx` 中的一行，确保只有一行有效。例如，要启用 `src_dir = examples/ATdebug`，删除前面的 `;` 符号并保存文件。现在将编译 ATDebug 示例。
9. 点击左下角的 (✔) 符号进行编译
10. Connect the board to the computer USB-C
11. 点击 (→) 上传固件
12. 点击（插头符号）监控串口输出
13. 如果无法写入，或 USB 设备持续闪烁，请查看下方的 **常见问题**

### Arduino IDE 快速入门

1. 安装 [Arduino IDE](https://www.arduino.cc/en/software)
2. 安装 [Arduino ESP32](https://docs.espressif.com/projects/arduino-esp32/en/latest/)
3. 将 `lib` 目录中的所有文件夹复制到 `Sketchbook location` 目录。如何找到您自己的库位置，[请查看这里](https://support.arduino.cc/hc/en-us/articles/4415103213714-Find-sketches-libraries-board-cores-and-other-files-on-your-computer)
    * Windows: `C:\Users\%USERNAME%\Documents\Arduino\libraries`
    * MacOS: `$HOME/Documents/Arduino/libraries`
    * Linux: `$HOME/Arduino/libraries`
4. 打开相应的示例
    * 打开下载的 `LilyGo-Modem-Series`
    * 打开 `examples`
    * 选择示例文件并打开以 `ino` 结尾的文件
5. 在 Arduino IDE 中，在工具项目中选择相应的开发板，点击下方列表中的相应选项进行选择

    | 名称                                 | 值                               |
    | ------------------------------------ | ----------------------------------- |
    | Board                                | **ESP32S3 Dev Module**              |
    | Port                                 | 您的端口                           |
    | USB CDC On Boot                      | Enable                              |
    | CPU Frequency                        | 240MHZ(WiFi)                        |
    | Core Debug Level                     | None                                |
    | USB DFU On Boot                      | Disable                             |
    | Erase All Flash Before Sketch Upload | Disable                             |
    | Flash Mode                           | QIO 80Mhz                           |
    | Flash Size                           | **16MB(128Mb)**                     |
    | Arduino Runs On                      | Core1                               |
    | USB Firmware MSC On Boot             | Disable                             |
    | Partition Scheme                     | **16M Flash (3MB APP/9.9MB FATFS)** |
    | PSRAM                                | **OPI PSRAM**                       |
    | Upload Speed                         | 921600                              |
    | Programmer                           | **Esptool**                         |

     * `Partition Scheme` 请根据实际应用选择。例如选择 **Huge APP (3MB No OTA/1MB SPIFFS)** , 更多分区方案请查看[这里](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/partition-tables.html)

6. In the open sketch, select the `utilities.h` file and uncomment the line `// #define LILYGO_T_ETH_ELITE_XXXX`. Delete the `//` and save the file to use this board as the default configuration. Name **XXXX** Please select according to the actual version

   ![board_select](../../../static/board_select.jpg)

7. 上传 sketch

> \[!IMPORTANT]
>
> 1. 无法上传任何代码？请查看下方的常见问题
> 2. 打开 Arduino IDE 时，会提示有新的库可以更新。请点击不更新。更新后无法保证能够编译、运行等。请确保开发板正常运行后再决定是否更新。要知道由于库的差异，更新后可能会出现问题，或者默认配置（如 TinyGSM）将被覆盖。
> 3. 如果更新后运行异常，请按照 Arduino 快速入门第 4 条删除 libraries 中的目录，并将 lib 中的文件夹重新拖入 libraries 目录。

### 📍 Pins Map

| 名称         | GPIO NUM | 可用 |
| ------------ | -------- | ---- |
| Modem TX     | 6        | ❌    |
| Modem RX     | 4        | ❌    |
| Modem RING   | 1        | ❌    |
| Modem DTR    | 5        | ❌    |
| Modem PWRKEY | 3        | ❌    |
| GPS RX*      | 39       | ❌    |
| GPS TX*      | 42       | ❌    |
| SD SCK       | 10       | ❌    |
| SD MISO      | 9        | ❌    |
| SD MOSI      | 11       | ❌    |
| SD CS        | 12       | ❌    |
| Default SDA  | 17       | ✅️    |
| Default SCL  | 18       | ✅️    |
| ETH MISO     | 47       | ❌    |
| ETH MOSI     | 21       | ❌    |
| ETH SCK      | 48       | ❌    |
| ETH CS       | 45       | ❌    |
| ETH IRQ      | 14       | ❌    |
| ETH RESET    | NC       | ❌    |
| Board LED    | 38       | ❌    |

> \[!TIP]
>
> [ESP32-S3 Consolidated Pin Overview](https://documentation.espressif.com/esp32-s3_datasheet_en.pdf)
>
> Input/Output: Any GPIO not listed can be used as input/output.
>
> ADC Pins: Only pins 1-21 are allowed as ADC inputs.
>
> IO0/IO3/IO45/IO46 are Strapping Pins; their use is limited. Please see the **Strapping Pins** section of the [online documentation](https://documentation.espressif.com/esp32-s3_datasheet_en.pdf) for details.
>

### ⚡ Electrical parameters

| 特性             | 详情                     |
| -------------------- | --------------------------- |
| 🔗USB-C 输入电压 | 5V                          |
| 🔗POE                 | Class 0/IEEE 802.3AF/36~57V |

### 按钮说明

| 按钮 | 功能                            |
| ------ | ----------------------------------- |
| BOOT   | 可自定义功能/下载模式 |
| RESET  | 复位设备按钮                |

* Press and hold the BOT button before powering on. The board is now in waiting mode for downloading.
* 以上命名方式以开发板丝印为准

### 天线说明

|          | 功能                     |
| -------- | ---------------------------- |
| SIM/MAIN | LTE 主天线接口   |
| GPS/GNSS | GPS 有源天线接口 |

* 以上命名方式以开发板丝印为准

### LED 说明

| 功能            | 颜色                |
| ------------------- | -------------------- |
| 模块状态        | RED   (On the modem) |
| 模块网络状态 | RED   (On the modem) |

* 模块状态灯和网络状态灯无法关闭

### 资源

* [GPS 天线](../../../../datasheet/GPS%20Antenna%20Specifications.pdf)
* [LTE 天线](../../../../datasheet/LTE%20Antenna%20Specifications.pdf)

#### SIM7000X Series

* [T-SIM7000G 原理图](../../../../schematic/esp32/T-SIM7000G-200415.pdf)
* [SIMCOM 官方 SIM7000G 全部数据手册](https://cn.simcom.com/product/SIM7000X.html)
* [For SIM7000 series documentation，请访问 datasheet 目录。](../../../../datasheet/SIM7000/)

#### A7608X Series

* [SIMCOM 官方 A7608X 全部数据手册](https://cn.simcom.com/product/A7608X-H-E_SA.html)
* [For A7608 series documentation，请访问 datasheet 目录。](../../../../datasheet/A76XX/)

#### SIM7670G Series

* [SIMCOM 官方 A7670X 全部数据手册](https://cn.simcom.com/product/A7670X.html)
* [For A7670X series documentation，请访问 datasheet 目录。](../../../../datasheet/A76XX/)

#### SIM7600X Series

* [SIMCOM 官方 SIM7600X 全部数据手册](https://cn.simcom.com/product/SIM7600X.html)
* [For SIM7600 series documentation，请访问 datasheet 目录。](../../../../datasheet/SIM7600X/)

#### SIM7080G Series

* [SIMCOM 官方 SIM7080G-S3 全部数据手册](https://cn.simcom.com/product/SIM7080G.html)
* [For SIM7080G-S3 series documentation，请访问 datasheet 目录。](../../../../datasheet/SIM7080/)

### 常见问题

* **如果以上方法仍然无法上传 sketch，请按照以下方法测试 USB2TTL 通信是否正常。**
  1. 将 USB-C 连接到 PC
  2. 打开串口监视器，将波特率调整为 115200
  3. 按下开发板上的 RST 按钮重启
  4. 如果能看到启动信息，说明 USB2TTL 通信正常。如果串口监视器中没有任何显示，可能是 USB2TTL 桥接损坏

* **升级模块固件** 请参考这里
  1. [T-SIM7000G 升级文档](../sim7000-esp32/upgrade/sim7000_upgrade.md)
  2. [T-SIM7670G 升级文档](../../upgrade/sim7670g/sim7670g_upgrade.md)
  3. [T-A7670X   升级文档](../../../update_fw.md)
  4. [T-SIM7600X 升级文档](../sim7600-esp32/upgrade/sim7600_upgrade.md)
  5. [T-SIM7080G 升级文档](../../esp32s3/sim7080-s3-standard/upgrade/README.MD)

* **无法检测到 SIM 卡？**
   1. 需要先将 SIM 卡插入开发板，然后再开机检测 SIM 卡。如果顺序颠倒，将报告无法检测到 SIM 卡。

* **无法上传任何 sketch，请手动进入上传模式。**
   1. 通过 USB 线连接开发板
   2. Use wires to connect GND and BOOT together
   3. Press RST button
   4. Release the RESET button
   5. Disconnect BOOT from GND
   6. 上传 sketch

* **板载开关的作用**
  1. 使用内部电池组时，开关将正常工作以控制开发板的电源开/关。但是，如果使用外部电池组并连接到 VBAT 引脚，开关将被旁路，这意味着唯一的关机方式是断开电池。
  2. 开关仅用于电池供电，插入 USB 时无效

* **关于 VBUS 引脚**
  1. VBUS 引脚和 USB-C 在同一条线上。只有连接 USB-C 时，VBUS 才有电压
  2. 仅连接电池时，VBUS 没有电压输出
  3. 如果想连接外部电源而不连接 USB-C，VBUS 引脚是唯一的电压输入引脚。请注意，VBUS 引脚的最大输入为 5V，不要超过 5V

* *Unable to put the modem into sleep mode**
  1. PCIE 系列必须移除 VBUS 电感/电阻才能让模块进入睡眠模式

  <img  height="480" width="480" src=../../esp32/pcie-series-esp32/images/pcie.png>
