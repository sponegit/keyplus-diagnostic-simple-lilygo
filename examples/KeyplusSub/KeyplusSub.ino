// ===========================================================================
// Keyplus v2 서브보드 펌웨어 (LilyGO T7-S3)
//   센서 수집 · 온도 가드 · 전원 레일 게이팅 · 저전압 판정 · 차키 시퀀서
//   메인(T-A7670E)과는 UART0(43/44) 한 가닥으로 KpLink 프레임을 주고받는다.
//
// 현재: 골격만 — 부팅 배너, 레일 OFF 고정, 메인 링크 포트 개방.
// ===========================================================================
#include <Arduino.h>
#include <KpLink.h>
#include "config.h"

static HardwareSerial& Link = Serial0;   // UART0 — USB-CDC 콘솔(Serial)과 별개

void setup() {
    // 레일 게이트는 온도 가드가 판정하기 전까지 OFF 가 안전한 기본값이다.
    const uint8_t rails[] = {PIN_RAIL_GNSS, PIN_RAIL_PMS_5V, PIN_RAIL_SENSOR_3V3, PIN_RAIL_CO_5V};
    for (uint8_t pin : rails) {
        digitalWrite(pin, LOW);
        pinMode(pin, OUTPUT);
    }
    // 차키 핀(4~9)은 건드리지 않는다 — 하이임피던스로 두어야 fob 가 "뗌"을 유지한다.

    Serial.begin(115200);
    Link.begin(LINK_BAUD, SERIAL_8N1, PIN_LINK_RX, PIN_LINK_TX);

    Serial.printf("\n[BOOT] KeyplusSub fw=%s hw=%s kplink=v%d\n",
                  FW_VERSION, HW_MODEL, KPLINK_PROTO_VERSION);
}

void loop() {
    delay(10);
}
