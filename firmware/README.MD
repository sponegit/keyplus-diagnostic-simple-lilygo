<div align="center" markdown="1">
  <img src="../.github/LilyGo_logo.png" alt="LilyGo logo" width="100"/>
</div>

<h1 align="center">LilyGo Modem AT Debug Firmware</h1>

<div align="center">

[English](./README.MD) | [中文](./README_CN.MD)

</div>

## Introduction

This catalog contains the AT debug firmware for the LilyGo modem series. This firmware is used to **test and debug modem AT commands**— for quick troubleshooting.

## ESP32 Version

| Product Name | AT Debug Firmware |
| --- | --- |
| [T-A7670X][1] | [esp32-t-a7670x_atdebug_20251201_0x0.bin](./esp32-t-a7670x_atdebug_20251201_0x0.bin) |
| [T-Call-A7670X-v1.0][2] | [esp32-t-call-a7670x-v1.0-atdebug-20251201_0x0.bin](./esp32-t-call-a7670x-v1.0-atdebug-20251201_0x0.bin) |
| [T-Call-A7670X-v1.1][2] | [esp32-t-call-a7670x-v1.1_atdebug_20251201_0x0.bin](./esp32-t-call-a7670x-v1.1_atdebug_20251201_0x0.bin) |
| [T-A7608][3] | [esp32-t-a7608x-atdebug-20251201_0x0.bin](./esp32-t-a7608x-atdebug-20251201_0x0.bin) |
| [T-SIM7000G][11] | [esp32-t-sim7000g_atdebug_20251201_0x0.bin](./esp32-t-sim7000g_atdebug_20251201_0x0.bin) |
| [T-SIM7070G][10] | [esp32-t-sim7070g_atdebug_20251201_0x0.bin](./esp32-t-sim7070g_atdebug_20251201_0x0.bin) |
| [T-SIM7600G][9] | [esp32-t-sim7600x-atdebug_20251201_0x0.bin](./esp32-t-sim7600x-atdebug_20251201_0x0.bin) |
| [T-PCI-E][8] | [esp32-t-pcie-atdebug-20251201_0x0.bin](./esp32-t-pcie-atdebug-20251201_0x0.bin) |
| [T-A7670X External GPS][1] | [esp32-t-a7670x-external-gps-a7670g-only-20260127_0x0.bin](./esp32-t-a7670x-external-gps-a7670g-only-20260127_0x0.bin) |

## ESP32-S3 Version

| Product Name | AT Debug Firmware |
| --- | --- |
| [T-A7608-S3][4] | [esp32s3-t-a7608x-atdebug-20251201_0x0.bin](./esp32s3-t-a7608x-atdebug-20251201_0x0.bin) |
| [T-SIM7670G-S3][5] | [esp32s3-t-sim7670g-atdebug-251201_0x0.bin](./esp32s3-t-sim7670g-atdebug-251201_0x0.bin) |
| [T-ETH-Elite-S3][7] | [esp32s3-t-eth-elite-atdebug-20251201_0x0.bin](./esp32s3-t-eth-elite-atdebug-20251201_0x0.bin) |

## Standard Series

| Product Name | Firmware |
| --- | --- |
| Standard Series AT Debug | [esp32s3-standard-series-atdebug-20251201_0x0.bin](./esp32s3-standard-series-atdebug-20251201_0x0.bin) |
| Standard Series Camera WebServer SoftAP | [esp32s3-standard-series-camera-web-server-softap-20251201_0x0.bin](./esp32s3-standard-series-camera-web-server-softap-20251201_0x0.bin) |
| Standard Series External GPS Shield | [esp32s3-standard-series-external-gps-20260108_0x0.bin](./esp32s3-standard-series-external-gps-20260108_0x0.bin) |

> Standard Series boards are pin-compatible across different modems, no need to distinguish between modem types when using firmware.

[1]: https://www.lilygo.cc/products/t-sim-a7670e
[2]: https://www.lilygo.cc/products/t-call-v1-4
[3]: https://lilygo.cc/products/t-a7608e-h?variant=42860532433077
[4]: https://lilygo.cc/products/t-a7608e-h
[5]: https://lilygo.cc/products/t-sim-7670g-s3
[7]: https://lilygo.cc/products/t-eth-elite-1
[8]: https://lilygo.cc/products/a-t-pcie
[9]: https://lilygo.cc/products/t-SIM7600
[10]: https://lilygo.cc/products/t-sim7070g
[11]: https://lilygo.cc/products/t-sim7000g

---

## Flashing Guide

### Method 1: LILYGO Spark (Recommended)

1. Download [LILYGO Spark](https://lilygo.cc/pages/lilygo-spark)

![LILYGO-Spark](./images/LILYGO-Spark.png)

### Method 2: ESP Download Tool

* [Flash Download Tool User Guide](https://docs.espressif.com/projects/esp-test-tools/en/latest/esp32/production_stage/tools/flash_download_tool.html)

| Step | ESP32 Version | ESP32-S3 Version |
| --- | --- | --- |
| Step 1 | ![esp32-1](./images/esp32-1.png) | ![esp32s3-1](./images/esp32s3-1.png) |
| Step 2 | ![esp32-2](./images/esp32-2.png) | ![esp32s3-2](./images/esp32s3-2.png) |

### Method 3: Web Flasher

1. Open [ESP Web Flasher Online](https://espressif.github.io/esptool-js/)
2. Select the correct board and baud rate
3. Choose the firmware `.bin` file
4. Click "Program"

![web_flasher](./images/web_flasher.gif)

> After flashing, press the **RST** button to reset the board.

### Method 4: Command Line (esptool)

Install esptool if you haven't:

```bash
pip install esptool
```

**ESP32:**

```bash
esptool --chip esp32 --baud 921600 --before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq 80m 0x0 firmware.bin
```

**ESP32-S3:**

```bash
esptool --chip esp32s3 --baud 921600 --before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq 80m 0x0 firmware.bin
```

Replace `firmware.bin` with the actual firmware file name you downloaded.

---

## FAQ

### Can't upload sketch — enter upload mode manually

1. Connect the board via USB cable
2. Press and hold the **BOOT** button
3. While holding BOOT, press the **RST** button
4. Release the **RST** button
5. Release the **BOOT** button
6. Upload your sketch

> [!IMPORTANT]
> **ESP32 version**: The SD card uses IO2 as CS. You **cannot upload software** in Arduino IDE when the SD card is inserted. **Remove the SD card** before uploading new code.
