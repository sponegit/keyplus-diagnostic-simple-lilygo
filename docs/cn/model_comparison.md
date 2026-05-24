<div align="center">

[English](../model_comparison.md) | [中文](./model_comparison.md)

</div>

## 型号对比

| 产品                         | 模块                     | QWIIC | 无缝 <br> 电源 <br> 切换 | GNSS<br> 路由<br> 到 SOC | GNSS<br>PPS | eSIM<br>焊盘 | BMS | 摄像头<br>接口 | 深度睡眠<br> 电流 | 引脚<br>兼容 |
| ------------------------------ | ------------------------ | ----- | ---------------------------------- | --------------------------- | ----------- | ----------- | --- | ------------------- | --------------------- | ----------------- |
| T-A7670X                       | ESP32-WROVER-B(N4R8)     | ❌     | ❌                                  | ❌                           | ❌           | ❌           | ✅   | ❌                   | 平均: 157uA            | ❌                 |
| T-A7608X                       | ESP32-WROVER-B(N4R8)     | ❌     | ❌                                  | ❌                           | ❌           | ❌           | ✅   | ❌                   | 平均: 240uA            | ❌                 |
| T-SIM7000G                     | ESP32-WROVER-B(N4R8)     | ❌     | ❌                                  | ❌                           | ❌           | ❌           | ✅   | ❌                   | 平均: 500uA            | ❌                 |
| T-SIM7600G                     | ESP32-WROVER-B(N4R8)     | ❌     | ❌                                  | ❌                           | ❌           | ❌           | ✅   | ❌                   | 平均: 200uA            | ❌                 |
| T-SIM7080G-S3                  | ESP32-S3-WROOM-1(N16R8)  | ❌     | ❌                                  | ❌                           | ❌           | ❌           | ❌   | ✅                   | 平均: 1.1mA            | ❌                 |
| T-SIM7670G-S3                  | ESP32-S3-WROOM-1(N16R8)  | ❌     | ❌                                  | ❌                           | ❌           | ❌           | ✅   | ❌                   | 平均: 497uA            | ❌                 |
| T-A7608X-S3                    | ESP32-S3-WROOM-1(N16R8)  | ❌     | ❌                                  | ❌                           | ❌           | ❌           | ✅   | ❌                   | 平均: 368uA            | ❌                 |
| T-A7670X-S3<br>-**Standard**   | ESP32-S3-WROOM-1(NR16R2) | ✅     | ✅                                  | ✅                           | ✅           | ✅           | ✅   | ✅                   | 平均: 314uA            | ✅                 |
| T-SIM7670G-S3<br>-**Standard** | ESP32-S3-WROOM-1(NR16R2) | ✅     | ✅                                  | ✅                           | ✅           | ✅           | ✅   | ✅                   | 平均: 147uA            | ✅                 |
| T-SIM7000G-S3<br>-**Standard** | ESP32-S3-WROOM-1(NR16R2) | ✅     | ✅                                  | ✅                           | ❌           | ✅           | ✅   | ✅                   | 平均: 166uA            | ✅                 |
| T-SIM7080G-S3<br>-**Standard** | ESP32-S3-WROOM-1(NR16R2) | ✅     | ✅                                  | ✅                           | ❌           | ✅           | ✅   | ✅                   | 平均: 128uA            | ✅                 |
| T-SIM7600G-S3<br>-**Standard** | ESP32-S3-WROOM-1(NR16R2) | ✅     | ✅                                  | ❌                           | ❌           | ✅           | ✅   | ✅                   | 平均: 128uA            | ✅                 |

- ESP32-WROVER-B(N4R8): 4MB Flash, 8MB PSRAM
- ESP32-S3-WROOM-1(N16R8): 16MB Flash, 8MB PSRAM (Octal SPI)
- ESP32-S3-WROOM-1(NR16R2): 16MB Flash, 2MB PSRAM (Quad SPI)
- 深度睡眠电流测试记录可以在 [DeepSleep.ino](../examples/DeepSleep/DeepSleep.ino) 中找到
- SIM7000G/SIM7080G/SIM7600G 没有 PPS 功能，因此该功能不可用。
