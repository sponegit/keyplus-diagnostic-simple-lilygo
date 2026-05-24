
<div align="center" markdown="1">
  <img src="../../../../../.github/LilyGo_logo.png" alt="LilyGo logo" width="100"/>
</div>

<h1 align = "center">🌟LilyGo SIM7080 升级指南🌟</h1>

<div align="center">

[English](../../../../en/esp32s3/sim7080-s3-standard/upgrade/README.MD) | [中文](./README.MD)

</div>

---

## 资源

- [SIM7080G 刷机工具 v1.6.7](https://drive.google.com/file/d/1nOqJuzBgE8KrMkpQUAWy3nJwqEBVABVn/view?usp=sharing)
- [SIM7080G 驱动](https://drive.google.com/file/d/1-yT3QcNJQEoD4ZkTbAu4SbsyxcA5Wxn6/view?usp=sharing)
- [固件 1951B17SIM7080](https://drive.google.com/file/d/1-m0eF53mw9n0vw4vfNZ88cRR3GM0T-a1/view?usp=sharing)

## `1` 烧写 ATDebug

* 烧写 [ATDebug](../../../examples/ATdebug/ATdebug.ino) 以在不运行任何应用程序的情况下启动调制解调器

## `2` 连接调制解调器 USB 端口

- SIM7080G-S3-Standard 版本需要两个 USB-C 端口。
- 将 ESP32 USB-C 端口和调制解调器的 USB 端口都连接到电脑，一个端口用于供电，另一个用于调制解调器升级。


## `3` 更新驱动

![update_driver](../../../../en/esp32s3/sim7080-s3-standard/upgrade/images/update_driver.gif)

## `4` 更新固件

1. 点击 **Load -> SIM7080&7900&8100&8200&8950Series**

![upgrade-1](../../../../en/esp32s3/sim7080-s3-standard/upgrade/images/upgrade-1.png)

2. 点击 **... -> 选择固件文件夹（下载的固件为 rar 或 zip 压缩格式，需要先解压）**

![upgrade-2](../../../../en/esp32s3/sim7080-s3-standard/upgrade/images/upgrade-2.png)

3. 关闭固件选择 -> 点击 **Start**

![upgrade-4](../../../../en/esp32s3/sim7080-s3-standard/upgrade/images/upgrade-3.png)

4. 等待升级完成

![upgrade-5](../../../../en/esp32s3/sim7080-s3-standard/upgrade/images/upgrade-4.png)

5. 升级完成后，调制解调器的 USB 端口将再次出现在设备管理器中。点击 **stop**

![upgrade-5](../../../../en/esp32s3/sim7080-s3-standard/upgrade/images/upgrade-5.png)

6. 打开串口监视器，重启 ESP32，发送 `AT+SIMCOMATI` 查询当前运行的固件版本。

![upgrade-6](../../../../en/esp32s3/sim7080-s3-standard/upgrade/images/upgrade-6.png)

7. 升级完成
