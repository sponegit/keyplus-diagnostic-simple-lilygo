<div align="center" markdown="1">
  <img src="../../../../.github/LilyGo_logo.png" alt="LilyGo logo" width="100"/>
</div>

<h1 align = "center">🌟LilyGo T-SIM-DC-Shield 使用指南🌟</h1>

<div align="center">

[English](../../../en/shield/t-sim-dc-shield/README.MD) | [中文](./README.MD)

</div>

---

<!-- <div align="center" markdown="1">
  <img src="../../../../images/product/png/T-T-SIM-DC-Shield.png" alt="t-bat" width="480"/>
</div> -->

## 支持列表
|                                  |     |
| -------------------------------- | --- |
| T-SIM7000G-Standard-Series       | ✅   |
| T-SIM7600G-Standard-Series       | ✅   |
| T-SIM7670G-Standard-Series       | ✅   |
| T-SIM7080G-Standard-Series       | ✅   |
| T-A7670X-Standard-Series         | ✅   |
| T-SIM7670G-ESP32S3 Version       | ✅   |
| T-A7608X-ESP32S3 Version         | ✅   |
| T-A7670X-ESP32 Version           | ✅   |
| T-A7608X-ESP32 Version           | ✅   |
| T-SIM7000G-ESP32 Version         | ✅   |
| T-SIM7600G-ESP32 Version         | ✅   |
| T-SIM7080G-ESP32S3 (PMU Version) | ❌   |
| T-A7670G+L76K-ESP32 Version      | ❌   |

> \[!IMPORTANT]
>
> 注意：使用外部 XT30 连接器 (DC) 供电时，请勿插入 USB-C 端口。同样，使用 USB-C 供电时，请勿连接外部 XT30 连接器 (DC) 供电。
>

### ⚡ 电气参数

| 特性                           | 详情                       |
| ------------------------------ | -------------------------- |
| ⚡DC 接口允许输入电压          | 7~36V                      |
| ⚡+5V                          | 最大电流可达 3A            |
| ⚡+3V3                         | 由主板提供                 |

### LED 说明

| 功能         | 颜色 | 状态                                     |
| ------------ | ---- | ---------------------------------------- |
| 3v3          | 红色 | 电源正常时指示灯亮。                     |
| 5v 指示灯    | 红色 | 电源正常时指示灯亮。                     |

### 默认板型

* T-SIM-DC-Shield 支持多种 LilyGo Sim 系列产品。默认电阻配置为 **T-XXXX-S3-Standard 系列**。如需更换为其他主板，需要切换 PCB 背面的电阻位置。

<div align="center" markdown="1">
  <img src="../../../en/shield/t-sim-dc-shield/images/resistor-positions.png" alt="t-bat" width="480"/>
</div>

### 引脚定义

#### T-SIMXXX-Standard-Series

| 标签       | GPIO | 电阻   |
| ---------- | ---- | ------ |
| A0         | 14   | R3     |
| A1         | 9    | R10    |
| D0         | 16   | R6     |
| D1         | 15   | R14    |
| SDA        | 3    | X      |
| SCL        | 2    | X      |
| Sensor IRQ | 21   | R12    |

#### T-A7670X-ESP32

| 标签       | GPIO | 电阻   |
| ---------- | ---- | ------ |
| A0         | 34   | R3     |
| A1         | 39   | R21    |
| D0         | 19   | R24    |
| D1         | 18   | R23    |
| SDA        | 21   | X      |
| SCL        | 22   | X      |
| Sensor IRQ | 32   | R19    |

* 不适用于 ESP32 A7670G+GPS 产品

#### T-A7608X-ESP32

| 标签       | GPIO | 电阻   |
| ---------- | ---- | ------ |
| A0         | 36   | R22    |
| A1         | 39   | R21    |
| D0         | 19   | R24    |
| D1         | 18   | R23    |
| SDA        | 21   | X      |
| SCL        | 22   | X      |
| Sensor IRQ | 32   | R19    |

#### T-A7608X-ESP32S3

| 标签       | GPIO | 电阻   |
| ---------- | ---- | ------ |
| A0         | 10   | R4     |
| A1         | 9    | R10    |
| D0         | 12   | R13    |
| D1         | 11   | R15    |
| SDA        | 2    | X      |
| SCL        | 1    | X      |
| Sensor IRQ | 8    | R18    |

#### T-SIM7670G-ESP32S3

| 标签       | GPIO | 电阻   |
| ---------- | ---- | ------ |
| A0         | 7    | R4     |
| A1         | 6    | R10    |
| D0         | 15   | R13    |
| D1         | 16   | R15    |
| SDA        | 2    | X      |
| SCL        | 1    | X      |
| Sensor IRQ | 8    | R18    |

#### T-SIM7000G-ESP32 或 T-SIM7070G-ESP32

| 标签       | GPIO | 电阻   |
| ---------- | ---- | ------ |
| A0         | 34   | R3     |
| A1         | 39   | R21    |
| D0         | 18   | R24    |
| D1         | 19   | R23    |
| SDA        | 21   | X      |
| SCL        | 22   | X      |
| Sensor IRQ | 32   | R19    |

#### T-SIM7600G-ESP32

| 标签       | GPIO | 电阻   |
| ---------- | ---- | ------ |
| A0         | X    | NC     |
| A1         | 39   | R21    |
| D0         | 19   | R24    |
| D1         | 18   | R23    |
| SDA        | 21   | X      |
| SCL        | 22   | X      |
| Sensor IRQ | 5    | R27    |

* A0 不可用；没有可用的 GPIO。
* 由于兼容性设计，使用 SIM7600G-ESP32 版本时，排针必须对准顶部（ESP32 侧）再插入。然后，使用时需要短接下图所示的两个点。

<div align="left" markdown="1">
  <img src="../../../en/shield/t-sim-dc-shield/images/sim7600-esp32.png" alt="t-bat" width="480"/>
</div>

## 原理图

* [T-SIM-DC-Shield-Rev1.0 原理图](../../../../schematic/shield/T-Sim-DC-Shield-Rev1.0.pdf)
* [T-Sim-DC-Shield 3D](../../../../dimensions/T-Sim-DC-Shield/T-Sim-DC-Shield.7z)
* [T-Sim-DC-Shield.svg](../../../../dimensions/T-Sim-DC-Shield/T-Sim-DC-Shield.svg)
