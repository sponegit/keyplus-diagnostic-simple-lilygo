
<div align="center" markdown="1">
  <img src="../../../../.github/LilyGo_logo.png" alt="LilyGo logo" width="100"/>
</div>

<h1 align = "center">🌟LilyGo SIM7070G Upgrade Guide🌟</h1>

<div align="center">

[English](./sim7070g_upgrade.md) | [中文](../../../cn/upgrade/sim7070g/sim7070g_upgrade.md)

</div>

## Resources

- [SIM7070G Driver](https://drive.google.com/file/d/1K-lMOBCx5dZSjUh-D9ZhNeKuxCFDA38W/view?usp=sharing)
- [SIM7070G FlashTools](https://drive.google.com/file/d/1nOqJuzBgE8KrMkpQUAWy3nJwqEBVABVn/view?usp=sharing)
- [SIM7070G Firmware 1951B07SIM7070](https://drive.google.com/file/d/15xsbw6fGNWFsfe4rdMqnXoVO_RRpNNBR/view?usp=sharing)
- [SIM7070G Firmware 1951B10SIM7070](https://drive.google.com/file/d/18Rc2s50rytoKrbS8g1ETEML65PZnmiOg/view?usp=sharing)
- [SIM7070G Firmware 1951B14SIM7070](https://drive.google.com/file/d/1ir56_G4uvgZQv9L-g7D6RqfRH5vDA16Z/view?usp=sharing)
- [SIM7070G Firmware 1951B15SIM7070](https://drive.google.com/file/d/1zoQrI5xMViDqClUtYJvhgU3kD-h64SoT/view?usp=sharing)
- [SIM7070G Firmware 1951B16SIM7070](https://drive.google.com/file/d/10scjUW4nFGLG06-9R0em6NTgeyjUxlfh/view?usp=sharing)
- [SIM7070G Firmware 1951B17SIM7070](https://drive.google.com/file/d/1407ttpsEjZiHPP1j-1geD3_TY0YCEa9m/view?usp=sharing)

## `1` Write to ATDebug

* Write [ATDebug](../../../../examples/ATdebug/ATdebug.ino) to start the modem without running any application

## `2` Connect to the Modem USB port

- The SIM7070G ESP32 version has a reserved USB solder point on the battery holder side for the modem. You need to connect a USB cable to this solder point, or directly solder a USB-A cable to it, as shown below.

![USB](../../esp32/sim7000-esp32/upgrade/images/16.png)
![USB](../../esp32/sim7000-esp32/upgrade/images/17.png)

## `3` Update Driver

See [Video](https://drive.google.com/file/d/1fjATMGHZh8ZwjOUoGXA_aA54Z22hGoDF/view?usp=sharing)

## `4` Update the firmware

See [Video](https://drive.google.com/file/d/1ZrwwADJAxep2M4qlIomm9SqYPKORoaLt/view?usp=sharing)
