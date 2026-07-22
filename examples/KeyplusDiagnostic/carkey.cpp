/**
 * @file      carkey.cpp
 * @brief     차키 버튼 GPIO 제어 구현 (열림/잠금). 극성·배선은 carkey.h 헤더 주석 참조.
 */
#include "carkey.h"
#include "config.h"

namespace Carkey {

// 구동 방식별 핀모드/극성 (config.h CARKEY_DRIVE_SEL). carkey.h 극성 주석 참조.
#if CARKEY_DRIVE_SEL == CARKEY_DRIVE_DIRECT
// GPIO 직결(오픈드레인): LOW=라인을 GND로 당김(누름) / HIGH=Hi-Z(fob 풀업 복귀=뗌).
static const int PIN_MODE_SEL  = OUTPUT_OPEN_DRAIN;
static const int PRESS_LEVEL   = LOW;
static const int RELEASE_LEVEL = HIGH;
static const char *DRIVE_NAME  = "DIRECT/open-drain (LOW=press)";
#else
// 2N7002 게이트(푸시풀): HIGH=게이트 ON(누름) / LOW=OFF(뗌).
static const int PIN_MODE_SEL  = OUTPUT;
static const int PRESS_LEVEL   = HIGH;
static const int RELEASE_LEVEL = LOW;
static const char *DRIVE_NAME  = "MOSFET/push-pull (HIGH=press)";
#endif

static int pinFor(Button b) {
    return (b == Button::LOCK) ? PIN_KEY_LOCK : PIN_KEY_UNLOCK;
}
static const char *nameFor(Button b) {
    return (b == Button::LOCK) ? "LOCK" : "UNLOCK";
}

void begin()
{
    // 두 라인을 즉시 뗌(release) 상태로 확정 — 부팅 초기 플로팅/오동작 방지.
    // DIRECT면 오픈드레인 HIGH(Hi-Z), MOSFET면 OUTPUT LOW(게이트 OFF).
    pinMode(PIN_KEY_LOCK,   PIN_MODE_SEL);
    digitalWrite(PIN_KEY_LOCK,   RELEASE_LEVEL);
    pinMode(PIN_KEY_UNLOCK, PIN_MODE_SEL);
    digitalWrite(PIN_KEY_UNLOCK, RELEASE_LEVEL);
    Serial.printf("[KEY] begin — lock=GPIO%d unlock=GPIO%d, drive=%s, released\n",
                  PIN_KEY_LOCK, PIN_KEY_UNLOCK, DRIVE_NAME);
}

void press(Button b, uint16_t holdMs)
{
    const int pin = pinFor(b);
    Serial.printf("[KEY] %s press (%ums)\n", nameFor(b), holdMs);
    digitalWrite(pin, PRESS_LEVEL);     // 라인을 누름 레벨로 (DIRECT=LOW / MOSFET=HIGH)
    delay(holdMs);                      // 유지(블로킹 — LED는 Ticker라 무관)
    digitalWrite(pin, RELEASE_LEVEL);   // 뗌: fob 내부 풀업 복귀 (DIRECT=Hi-Z / MOSFET=OFF)
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
