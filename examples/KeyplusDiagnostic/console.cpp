/**
 * @file      console.cpp
 * @brief     디버그 콘솔 스트림 구현 — U0 + 보조 포트(v2: 서브 링크 / v1: 보조 UART2).
 */
#include "console.h"

#if FEATURE_AUX_CONSOLE && FEATURE_SUBLINK
#error "보조 콘솔과 서브 링크는 같은 UART2·GPIO13/34 를 쓴다 — 둘 중 하나만 켤 것"
#endif

#if FEATURE_SUBLINK

// v2: 보조 포트 자리에 서브 링크 콘솔(LOG/CONSOLE_IN 프레임)을 꽂는다 — 작업서 S3.
// USB-C 와 서브 USB-CDC 양쪽에 같은 로그가 나가고, 두 쪽 어디서든 명령을 받는다.
#include "sublink.h"
static TeeStream s_tee(Serial, Sublink::console());
Stream          &DbgConsole = s_tee;

#elif FEATURE_AUX_CONSOLE

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
