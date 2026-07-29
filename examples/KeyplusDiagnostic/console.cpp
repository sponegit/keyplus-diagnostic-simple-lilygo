/**
 * @file      console.cpp
 * @brief     디버그 콘솔 스트림 구현 — U0 + 보조 UART(UART2).
 */
#include "console.h"

#if FEATURE_AUX_CONSOLE

// UART2 — UART0은 USB-C 브리지, UART1은 모뎀(SerialAT)이 쓰므로 남는 건 2번뿐이다.
HardwareSerial   SerialAux(2);
static TeeStream s_tee(Serial, SerialAux);
Stream          &DbgConsole = s_tee;

#else

Stream &DbgConsole = Serial;

#endif

void consoleBegin()
{
    Serial.begin(115200);
#if FEATURE_AUX_CONSOLE
    // Arduino-ESP32 시그니처는 (baud, config, rxPin, txPin) 순이다.
    SerialAux.begin(AUX_CONSOLE_BAUD, SERIAL_8N1, AUX_CONSOLE_RX_PIN, AUX_CONSOLE_TX_PIN);
#endif
}
