/**
 * @file      carkey.cpp
 * @brief     차키 버튼 GPIO 제어 구현 (열림/잠금). 극성·배선은 carkey.h 헤더 주석 참조.
 */
#include "carkey.h"
#include "config.h"
#include "log.h"

namespace Carkey {

// 누름 극성/핀모드 결정 (config.h). 배선이 곧 극성이므로 CARKEY_DRIVE_SEL 이 기본을 정하고,
// CARKEY_ACTIVE_HIGH 는 AUTO 가 아닐 때만 극성을 강제 오버라이드한다(푸시풀 고정).
#if CARKEY_ACTIVE_HIGH == CARKEY_ACTIVE_AUTO
  #if CARKEY_DRIVE_SEL == CARKEY_DRIVE_DIRECT
    #define CK_PRESS_HIGH  0    // fob 패드 active-low 직결 → LOW 가 누름
    #define CK_OPEN_DRAIN  1    // 뗌은 Hi-Z 로 놓아 fob 내부 풀업이 복귀시킨다
  #else
    #define CK_PRESS_HIGH  1    // 2N7002 게이트 → HIGH 가 누름
    #define CK_OPEN_DRAIN  0
  #endif
#else
  #define CK_PRESS_HIGH  (CARKEY_ACTIVE_HIGH)
  #define CK_OPEN_DRAIN  0      // 극성 강제 시엔 양쪽 레벨을 실제로 구동해야 하므로 푸시풀
  #if CK_PRESS_HIGH && (CARKEY_DRIVE_SEL == CARKEY_DRIVE_DIRECT)
    #warning "CARKEY: 직결(DIRECT) 배선에 HIGH=누름 강제 — 뗌 상태가 GND 고정(버튼 상시 눌림) + fob 역급전 위험. 배선을 다시 확인할 것."
  #endif
#endif

#if CK_OPEN_DRAIN
static const int PIN_MODE_SEL  = OUTPUT_OPEN_DRAIN;
#else
static const int PIN_MODE_SEL  = OUTPUT;
#endif
#if CK_PRESS_HIGH
static const int PRESS_LEVEL   = HIGH;
static const int RELEASE_LEVEL = LOW;
#else
static const int PRESS_LEVEL   = LOW;
static const int RELEASE_LEVEL = HIGH;
#endif
static const char *DRIVE_NAME =
    CK_OPEN_DRAIN ? "open-drain (LOW=press, release=Hi-Z)"
                  : (CK_PRESS_HIGH ? "push-pull (HIGH=press)" : "push-pull (LOW=press)");

static int pinFor(Button b) {
    return (b == Button::LOCK) ? PIN_KEY_LOCK : PIN_KEY_UNLOCK;
}
static const char *nameFor(Button b) {
    return (b == Button::LOCK) ? "LOCK" : "UNLOCK";
}

void begin()
{
    // 두 라인을 즉시 뗌(release) 상태로 확정 — 부팅 초기 플로팅/오동작 방지.
    // ACTIVE_HIGH=1이면 OUTPUT LOW, 직결(오픈드레인)이면 HIGH(Hi-Z).
    pinMode(PIN_KEY_LOCK,   PIN_MODE_SEL);
    digitalWrite(PIN_KEY_LOCK,   RELEASE_LEVEL);
    pinMode(PIN_KEY_UNLOCK, PIN_MODE_SEL);
    digitalWrite(PIN_KEY_UNLOCK, RELEASE_LEVEL);
    // 배선/극성 확인용 — 부팅 배너에 이미 기능 목록이 나오므로 상세는 DEBUG.
    LOGD(Serial, "[KEY] begin — lock=GPIO%d unlock=GPIO%d, drive=%s, released\n",
         PIN_KEY_LOCK, PIN_KEY_UNLOCK, DRIVE_NAME);
}

void press(Button b, uint16_t holdMs)
{
    const int pin = pinFor(b);
    // 실제 차키 동작은 앱 상태 전이 — 평상시에도 남아야 원격 명령 결과를 추적할 수 있다.
    LOGI(Serial, "[KEY] %s press (%ums)\n", nameFor(b), holdMs);
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
