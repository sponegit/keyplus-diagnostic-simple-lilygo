
<div align="center" markdown="1">
  <img src="../.github/LilyGo_logo.png" alt="LilyGo logo" width="100"/>
</div>

<h1 align = "center">🌟LilyGo A7670/A7608 升级指南🌟</h1>

<div align="center">

[English](../update_fw.md) | [中文](./update_fw.md)

</div>

# 前提条件

## 驱动程序

* [Windows USB 驱动程序](https://drive.google.com/drive/folders/1-7x2z00a5VO7GZS96C6mDupNTBXIh--H?usp=sharing)

## 烧录工具

> \[!IMPORTANT]
> [A76XX_A79XX_MADL V1.13 Only for Update] 存在 BUG，请勿使用此工具，请使用 V1.32 版本的升级工具
>
> 升级前，请发送 "AT+SIMCOMATI" 检查硬件版本。如果写入错误版本的固件，可能会损坏调制解调器。

* [A7670X//A7608X 烧录工具](https://drive.google.com/file/d/12nt5-wcsUT6bRaEhfOMBSq0EhOl8R2by/view?usp=sharing)
* [SIM7670G 烧录工具](https://drive.google.com/file/d/1-CHQOXQhCJRr1S8rP_AGHVWGqE823wem/view?usp=sharing)

## 固件

### A7670E-LNMV 无 GPS，无语音通话，无 SMS

* [🆕 A7670E-LNMV 23156B02A7670M5A_A7670E-LNMV_V101240423](https://drive.google.com/file/d/1nDTdMgSrb2Ku2NUtOZaKzZO6MYJnj4l-/view?usp=sharing)

### A7670E-LNXY-UBL 无 GPS，无语音通话，无 SMS

* [A7670E-LNXY-UBL 23124B03](https://drive.google.com/file/d/12i1YNPVsygzwKhNnboiGVRn_nG6QulQA/view?usp=sharing)

### A7670G-LLSE 无 GPS

<!-- * [A7670G B18](https://drive.google.com/file/d/10BxF-O2aPQ9vB5N97VT3bxPguURNoMvF/view?usp=sharing) Known HTTP 715 Errors Don't use this version- -->
* [A7670G-LLSE A124B01](https://drive.google.com/file/d/1-UOq_7TxU8aNAY4KKuXtnoRt0UZwipSQ/view?usp=sharing)

<!-- (LTE-FDD B1/B2/B3/B4/B5/B7/B8/B12*/B13*LB18*/B19*/B20/B25*/B26*/B28/B66/ LTE-TDD B34/B38/B39/B40/B41 GSM/GPRS/EDGE 850/900/1800/1900 MHz) -->

### A7670G-LABE 无 GPS

* [A7670G-LABE A069B01](https://drive.google.com/file/d/10oSTTqhw7ZiiZ_LiqAb3WYV3LXwbpKtV/view?usp=sharing)
* 🆕[A7670G-LABE A110B06](https://drive.google.com/file/d/10fsorFI8SuTlcffufgphpYbt2JjUpqtg/view?usp=sharing)

<!-- (LTE-FDD B1/B2/B3/B4/B5/B7/B8/B20B28/B66/ LTE-TDD B34/B38/B39/B40/B41 GSM/GPRS/EDGE 850/900/1800/1900 MHz) -->

------------------------

### A7670E-LASE 无 GPS

<!-- * [A7670E-LASE B14](https://drive.google.com/file/d/1ERblToPH4FoAo2dVYn3B--u_FzhIzeMQ/view?usp=sharing) Known HTTP 715 Errors Don't use this version-->
* [A7670E-LASE A124B01](https://drive.google.com/file/d/1-14KABQxVgjcJjJVuofp1BkZd0VYZVWA/view?usp=sharing)<!-- 20241008 Update Fixed  HTTP 715 error-->
* 🆕[A7670E-LASE A124B02](https://drive.google.com/file/d/1PJ6xogfZqH1fFin-V_Mio9rExiVIL-lz/view?usp=sharing)

------------------------

### A7670E-FASE 有 GPS

<!-- A011B07V01A7670M7_F_A7670E-FASE_CD_V101230711 -->
* [A7670E-FASE B07](https://drive.google.com/file/d/1-5A9w4MCXNz6F5ODhynne0rC9IUOeOdH/view?usp=sharing)<!-- 20241008 Update Fixed  HTTP 715 error-->

------------------------

### A7670SA-FASE 有 GPS

<!-- A011B07V01A7670M7_F_A7670SA-FASE_CD_V101230712 -->
* [A7670SA-FASE-A011B07V01A7670M7](https://drive.google.com/file/d/1-CcYlyPOYpIpcmSDjnfIXDE-GuazzLjO/view?usp=sharing) <!-- 20241009 Update Fixed  HTTP 715 error-->
<!-- A011B09A7670M7_F_A7670SA-FASE_CD_V101230712.rar -->
* 🆕[A7670SA-FASE-A011B09A7670M7](https://drive.google.com/file/d/1nyIHD29vpMsm7do0SJIF8WNhXoop1K2V/view?usp=sharing)

------------------------

### A7670SA-LASE 无 GPS

* [A7670SA-LASE B19](https://drive.google.com/file/d/1-evoE-qTLzQEG3OrGdrq9SVuDxEVGHeQ/view?usp=sharing)

### A7670SA-LASC 无 GPS

* [A7670SA-LASC B01](https://drive.google.com/file/d/127FpmjV9TNFJcNEUK03X03sGpGGqFc-u/view?usp=sharing)

------------------------

### SIM7670G-MNGV

参见 [LilyGo SIM7670G 升级指南](../en/upgrade/sim7670g/sim7670g_upgrade.md)

------------------------

### A7608E-H

<!-- * [A7608E-H B11](https://drive.google.com/file/d/1IfNkPfQmfG3oqbXEZl0YD_9qgLsN4e_D/view?usp=sharing) -->
* [A7608E-H B14](https://drive.google.com/file/d/1NNixH5VYale9fS1JkHSBdoM-KiiMboWY/view?usp=sharing)

------------------------

### A7608SA-H

* [🆕A7608SA-H A81C4B02A7600M7_T_A7608SA-H_V102250213](https://drive.google.com/file/d/1eIXt-tPJz9cF25YVeCH47hbQj1X1p2GS/view?usp=sharing)
* [A7608SA-H A81C4B01A7600M7_T_A7608SA-H_V102250213](https://drive.google.com/file/d/1lf7EFa7uaI9fyr25b4Ik2SW36P-22rWG/view?usp=sharing)
* [A7608SA-H A50C4B11V01A7600M7_A7608SA-H_V102220719](https://drive.google.com/file/d/1ktLzCjnd0TXzbiythU1EaWVkAokXzkf9/view?usp=sharing)
* [A7608SA-H A50C4B13A7600M7_A7608SA-H_V102240329](https://drive.google.com/file/d/1-LsgDug-Zz_0kU96HlCGtfks3ruvjcQb/view?usp=sharing)

> \[!IMPORTANT]
>
> A7608 系列在引导模式下需要安装专用驱动程序；请参见 [此处](https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series/issues/405#issuecomment-3575374939)
>
> 驱动目录: `A76XX_A79XX_MADL V1.32 Only for Update` -> `A1802&1803&1826Driver`

------------------------

**请将调制解调器上二维码中的信息提供给 LilyGo 以确认固件版本。**

> \[!IMPORTANT]
> 进入升级模式的步骤
>
> 1. 按住靠近调制解调器的 SBOOT 按钮，不要松开。
> 2. 插入 MICRO-USB
> 3. 插入 USB-C
> 4. 检查设备管理器，等待计算机端口生成
> 5. 端口识别后松开 SBOOT 按钮

### 录制视频

* [SIM7670G 升级记录](https://youtu.be/wTcMp0K8fXY)
* [A7670XX 升级记录](https://youtu.be/AZkm-Z7mKn8)


### 日志抓取

* [SIM7670G 日志抓取记录](https://youtu.be/4vvNANhz86A)
* [A7670XX 日志抓取记录](https://youtu.be/31MUyhX5UHs)



### 固件更新步骤

1. 上传 [ATDebug_sketch](../examples/ATdebug/ATdebug.ino)
2. 按住调制解调器旁边的按钮，然后插入 MicroUSB 端口
   ![](../images/upgrade/boot_pin.jpg)
   
3. 打开计算机的设备管理器，然后按照以下说明安装所有未知驱动程序

   ![](../images/upgrade/step2.jpg)
   ![](../images/upgrade/step3.jpg)
   ![](../images/upgrade/step4.jpg)
   ![](../images/upgrade/step5.jpg)

    重复以上操作以完成所有其他设备驱动程序的安装

4. 打开 `A76XX_A79XX_MADL V1.13 Only for Update.exe`
5. 按照下图步骤选择升级固件
   ![](../images/upgrade/step1.jpg)
6. 点击 GO 按钮，等待进度条完成。
   ![](../images/upgrade/step6.jpg)

7. 固件更新完成后，您可以发送 `AT+SIMCOMATI` 检查版本
