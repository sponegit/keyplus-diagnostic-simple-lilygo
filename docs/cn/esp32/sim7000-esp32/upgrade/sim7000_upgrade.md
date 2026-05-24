
<div align="center" markdown="1">
  <img src="../../../../../.github/LilyGo_logo.png" alt="LilyGo logo" width="100"/>
</div>

<h1 align = "center">🌟LilyGo SIM7000 升级指南🌟</h1>

<div align="center">

[English](../../../../en/esp32/sim7000-esp32/upgrade/sim7000_upgrade.md) | [中文](./sim7000_upgrade.md)

</div>

---

## 资源

- [SIM7000 刷机工具 v1.5.8](https://drive.google.com/file/d/1BUtRWizTqEeI8x4khVRwp_ITektyUyoN/view?usp=sharing)
- [SIM7000 驱动](https://drive.google.com/file/d/1f02TTNoyirFPGWbob1khy9dnoBonoVe7/view?usp=sharing)
- [1529B11V01SIM7000G 修复 MQTTS,HTTPS](https://drive.google.com/file/d/12rZ9b7z3ONCPwtevOcz3khYl5vw4zGL3/view?usp=sharing)
- [备份和恢复 QCN 的工具和文件](https://drive.google.com/drive/folders/10Fik8zT4UFX1dmCLbZ0GkgIIsxu356QT)
- [SIM7000 刷机工具 v1.4.3](https://drive.google.com/file/d/1-ADY7_fbXehiQBJZhwp7Kht-ZQpFE1np/view?usp=sharing)

> \[!IMPORTANT]
>
> 注意：推荐使用 v1.5.8 版本的升级工具。不推荐使用其他版本，因为它们可能导致 qcn 丢失。
>


## `1` 烧写 ATDebug

* 烧写 [ATDebug](../../../../../examples/ATdebug/ATdebug.ino) 以在不运行任何应用程序的情况下启动调制解调器

## `2` 连接调制解调器 USB 端口

- SIM7000G ESP32 版本在电池座侧面为调制解调器预留了 USB 焊点。您需要将 USB 数据线连接到此焊点，或直接将 USB-A 数据线焊接上去，如下图所示。
- SIM7000G-S3-Standard 版本需要两个 USB-C 端口。
- 将 ESP32 USB-C 端口和调制解调器的 USB 端口都连接到电脑，一个端口用于供电，另一个用于调制解调器升级。

![USB](../../../../en/esp32/sim7000-esp32/upgrade/images/16.png)
![USB](../../../../en/esp32/sim7000-esp32/upgrade/images/17.png)

## `3` 更新驱动

![update_driver](../../../../en/esp32/sim7000-esp32/upgrade/images/update_driver.gif)

## `4` 更新固件

1. 点击 **Load -> MDM9206(SIM7000Series)**

![upgrade-1](../../../../en/esp32/sim7000-esp32/upgrade/images/upgrade-1.png)

2. 点击 **... -> 选择固件文件夹（下载的固件为 rar 或 zip 压缩格式，需要先解压）**

![upgrade-2](../../../../en/esp32/sim7000-esp32/upgrade/images/upgrade-2.png)

3. 关闭固件选择 -> 点击 **Start**

![upgrade-3](../../../../en/esp32/sim7000-esp32/upgrade/images/upgrade-3.png)

4. 等待升级完成

![upgrade-4](../../../../en/esp32/sim7000-esp32/upgrade/images/upgrade-4.png)

5. 升级完成后，调制解调器的 USB 端口将再次出现在设备管理器中。点击 **stop**

![upgrade-5](../../../../en/esp32/sim7000-esp32/upgrade/images/upgrade-5.png)

6. 打开串口监视器，重启 ESP32，发送 `AT+SIMCOMATI` 查询当前运行的固件版本。

![upgrade-6](../../../../en/esp32/sim7000-esp32/upgrade/images/upgrade-6.png)

7. 升级完成
