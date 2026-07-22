/**
 * @file      carkey.cpp
 * @brief     차키 버튼 GPIO 제어 구현 (열림/잠금). 극성·배선은 carkey.h 헤더 주석 참조.
 */
#include "carkey.h"
#include "config.h"

namespace Carkey {

// 게이트 레벨: HIGH=누름(2N7002 ON), LOW=뗌(OFF). carkey.h 극성 주석 참조.
static const int PRESS_LEVEL   = HIGH;
static const int RELEASE_LEVEL = LOW;

static int pinFor(Button b) {
    return (b == Button::LOCK) ? PIN_KEY_LOCK : PIN_KEY_UNLOCK;
}
static const char *nameFor(Button b) {
    return (b == Button::LOCK) ? "LOCK" : "UNLOCK";
}

void begin()
{
    // 두 게이트를 즉시 OUTPUT LOW(버튼 뗌)로 확정 — 부팅 초기 플로팅/오동작 방지.
    pinMode(PIN_KEY_LOCK,   OUTPUT);
    digitalWrite(PIN_KEY_LOCK,   RELEASE_LEVEL);
    pinMode(PIN_KEY_UNLOCK, OUTPUT);
    digitalWrite(PIN_KEY_UNLOCK, RELEASE_LEVEL);
    Serial.printf("[KEY] begin — lock=GPIO%d unlock=GPIO%d (HIGH=press), released\n",
                  PIN_KEY_LOCK, PIN_KEY_UNLOCK);
}

void press(Button b, uint16_t holdMs)
{
    const int pin = pinFor(b);
    Serial.printf("[KEY] %s press (%ums)\n", nameFor(b), holdMs);
    digitalWrite(pin, PRESS_LEVEL);     // 게이트 ON → 버튼 눌림
    delay(holdMs);                      // 유지(블로킹 — LED는 Ticker라 무관)
    digitalWrite(pin, RELEASE_LEVEL);   // 게이트 OFF → Hi-Z, fob 풀업 복귀(뗌)
}

void lock()   { press(Button::LOCK,   CARKEY_PRESS_MS); }
void unlock() { press(Button::UNLOCK, CARKEY_PRESS_MS); }

bool tryConsole(const String &cmd, const String &arg, Stream &io)
{
    // 선택 인자: 누름 유지시간(ms). 미지정/무효면 기본값 CARKEY_PRESS_MS.
    long ms = arg.length() ? arg.toInt() : 0;
    uint16_t hold = (ms > 0 && ms <= 5000) ? (uint16_t)ms : CARKEY_PRESS_MS;

    if (cmd == "lock") {
        press(Button::LOCK, hold);
        return true;
    }
    if (cmd == "unlock") {
        press(Button::UNLOCK, hold);
        return true;
    }
    return false;
}

} // namespace Carkey
