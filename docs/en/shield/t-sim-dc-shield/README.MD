<div align="center" markdown="1">
  <img src="../../../../.github/LilyGo_logo.png" alt="LilyGo logo" width="100"/>
</div>

<h1 align = "center">🌟LilyGo T-SIM-DC-Shield Usage Guide🌟</h1>

<div align="center">

[English](./README.MD) | [中文](../../../cn/shield/t-sim-dc-shield/README.MD)

</div>


<!-- <div align="center" markdown="1">
  <img src="../../../../images/product/png/T-T-SIM-DC-Shield.png" alt="t-bat" width="480"/>
</div> -->

## Supported list
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
> Note: When using an external XT30 connector(DC) for power, do not plug in the USB-C port. Similarly, when using USB-C for power, do not connect an external XT30 connector(DC) for power.
>

### ⚡ Electrical parameters

| Features                           | Details                       |
| ---------------------------------- | ----------------------------- |
| ⚡DC interface allows input voltage | 7~36V                         |
| ⚡+5V                               | The maximum current can be 3A |
| ⚡+3V3                              | Provided by the motherboard   |

### LED Description

| FUNCTION     | COLOR | STATE                                                              |
| ------------ | ----- | ------------------------------------------------------------------ |
| 3v3          | Red   | The indicator light will illuminate if the power supply is normal. |
| 5v indicator | Red   | The indicator light will illuminate if the power supply is normal. |

### Default board type

* The T-SIM-DC-Shield supports multiple LilyGo Sim series products. The default resistor configuration is for the **T-XXXX-S3-Standard series**. If you need to change to a different motherboard, you need to switch the resistor positions on the back of the PCB.

<div align="center" markdown="1">
  <img src="./images/resistor-positions.png" alt="t-bat" width="480"/>
</div>

### Pin Definitions

#### T-SIMXXX-Standard-Series

| Label      | GPIO | Resistor |
| ---------- | ---- | -------- |
| A0         | 14   | R3       |
| A1         | 9    | R10      |
| D0         | 16   | R6       |
| D1         | 15   | R14      |
| SDA        | 3    | X        |
| SCL        | 2    | X        |
| Sensor IRQ | 21   | R12      |

#### T-A7670X-ESP32

| Label      | GPIO | Resistor |
| ---------- | ---- | -------- |
| A0         | 34   | R3       |
| A1         | 39   | R21      |
| D0         | 19   | R24      |
| D1         | 18   | R23      |
| SDA        | 21   | X        |
| SCL        | 22   | X        |
| Sensor IRQ | 32   | R19      |

* Not applicable to ESP32 A7670G+GPS products

#### T-A7608X-ESP32

| Label      | GPIO | Resistor |
| ---------- | ---- | -------- |
| A0         | 36   | R22      |
| A1         | 39   | R21      |
| D0         | 19   | R24      |
| D1         | 18   | R23      |
| SDA        | 21   | X        |
| SCL        | 22   | X        |
| Sensor IRQ | 32   | R19      |

#### T-A7608X-ESP32S3

| Label      | GPIO | Resistor |
| ---------- | ---- | -------- |
| A0         | 10   | R4       |
| A1         | 9    | R10      |
| D0         | 12   | R13      |
| D1         | 11   | R15      |
| SDA        | 2    | X        |
| SCL        | 1    | X        |
| Sensor IRQ | 8    | R18      |

#### T-SIM7670G-ESP32S3

| Label      | GPIO | Resistor |
| ---------- | ---- | -------- |
| A0         | 7    | R4       |
| A1         | 6    | R10      |
| D0         | 15   | R13      |
| D1         | 16   | R15      |
| SDA        | 2    | X        |
| SCL        | 1    | X        |
| Sensor IRQ | 8    | R18      |

#### T-SIM7000G-ESP32 OR T-SIM7070G-ESP32

| Label      | GPIO | Resistor |
| ---------- | ---- | -------- |
| A0         | 34   | R3       |
| A1         | 39   | R21      |
| D0         | 18   | R24      |
| D1         | 19   | R23      |
| SDA        | 21   | X        |
| SCL        | 22   | X        |
| Sensor IRQ | 32   | R19      |

#### T-SIM7600G-ESP32

| Label      | GPIO | Resistor |
| ---------- | ---- | -------- |
| A0         | X    | NC       |
| A1         | 39   | R21      |
| D0         | 19   | R24      |
| D1         | 18   | R23      |
| SDA        | 21   | X        |
| SCL        | 22   | X        |
| Sensor IRQ | 5    | R27      |

* A0 is unavailable; no GPIO is available.
* Due to compatibility design, when using the SIM7600G-ESP32 version, the pin header must be aligned at the top (ESP32 side) before insertion. Then, the two points shown in the image below need to be shorted during use.

<div align="left" markdown="1">
  <img src="./images/sim7600-esp32.png" alt="t-bat" width="480"/>
</div>

## Schematic

* [T-SIM-DC-Shield-Rev1.0 schematic](../../../../schematic/shield/T-Sim-DC-Shield-Rev1.0.pdf)
* [T-Sim-DC-Shield 3D](../../../../dimensions/T-Sim-DC-Shield/T-Sim-DC-Shield.7z)
* [T-Sim-DC-Shield.svg](../../../../dimensions/T-Sim-DC-Shield/T-Sim-DC-Shield.svg)
