/**
 * @file      carkey.cpp
 * @brief     차키 버튼 GPIO 제어 구현 (열림/잠금). 극성·배선은 carkey.h 헤더 주석 참조.
 */
#include "carkey.h"
#include "config.h"
#include "log.h"
#include "console.h"

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

// --- 논블로킹 누름 상태 ------------------------------------------------------
// delay()로 유지하면 트렁크처럼 2초 이상 눌러야 하는 기능에서 loop 가 그만큼 멈춰
// LED 패턴·콘솔·MQTT URC 펌핑이 정지한다. 누름은 걸어만 두고 update()가 뗀다.
static bool     s_active    = false;
static Button   s_activeBtn = Button::LOCK;
static int      s_activePin = -1;
static uint32_t s_releaseAt = 0;

uint32_t busyRemainMs();   // press() 의 거부 로그가 먼저 참조한다

void begin()
{
    // 두 라인을 즉시 뗌(release) 상태로 확정 — 부팅 초기 플로팅/오동작 방지.
    // ACTIVE_HIGH=1이면 OUTPUT LOW, 직결(오픈드레인)이면 HIGH(Hi-Z).
    pinMode(PIN_KEY_LOCK,   PIN_MODE_SEL);
    digitalWrite(PIN_KEY_LOCK,   RELEASE_LEVEL);
    pinMode(PIN_KEY_UNLOCK, PIN_MODE_SEL);
    digitalWrite(PIN_KEY_UNLOCK, RELEASE_LEVEL);
    // 배선/극성 확인용 — 부팅 배너에 이미 기능 목록이 나오므로 상세는 DEBUG.
    s_active = false;   // 재초기화 시 진행 중 누름 상태를 남기지 않는다
    LOGD(DbgConsole, "[KEY] begin — lock=GPIO%d unlock=GPIO%d, drive=%s, released\n",
         PIN_KEY_LOCK, PIN_KEY_UNLOCK, DRIVE_NAME);
}

bool press(Button b, uint16_t holdMs)
{
    if (s_active) {
        // 겹친 조작 — 두 라인을 동시에 누르거나 유지시간을 뒤엎으면 fob 동작이 불확실해진다.
        // 조용히 삼키지 않고 거부를 알린다(호출측이 ack 를 failed 로 낼 수 있도록).
        LOGW(DbgConsole, "[KEY] %s 거부 — %s 누름 진행 중(%lums 남음)\n",
             nameFor(b), nameFor(s_activeBtn), (unsigned long)busyRemainMs());
        return false;
    }

    uint32_t hold = holdMs ? holdMs : (uint32_t)CARKEY_PRESS_MS;
    if (hold > (uint32_t)CARKEY_PRESS_MAX_MS) hold = CARKEY_PRESS_MAX_MS;

    s_activeBtn = b;
    s_activePin = pinFor(b);
    s_releaseAt = millis() + hold;
    s_active    = true;
    digitalWrite(s_activePin, PRESS_LEVEL);   // 누름 레벨 (DIRECT=LOW / MOSFET=HIGH)

    // 실제 차키 동작은 앱 상태 전이 — 평상시에도 남아야 원격 명령 결과를 추적할 수 있다.
    LOGI(DbgConsole, "[KEY] %s press (%lums)\n", nameFor(b), (unsigned long)hold);
    return true;
}

void update()
{
    if (!s_active) return;
    // millis() 오버플로우 안전 비교.
    if ((int32_t)(millis() - s_releaseAt) < 0) return;

    digitalWrite(s_activePin, RELEASE_LEVEL);  // 뗌: fob 풀업 복귀 (DIRECT=Hi-Z / MOSFET=OFF)
    s_active = false;
    LOGD(DbgConsole, "[KEY] %s release\n", nameFor(s_activeBtn));
}

uint32_t busyRemainMs()
{
    if (!s_active) return 0;
    int32_t left = (int32_t)(s_releaseAt - millis());
    return left > 0 ? (uint32_t)left : 0;
}

bool lock(uint16_t holdMs)   { return press(Button::LOCK,   holdMs); }
bool unlock(uint16_t holdMs) { return press(Button::UNLOCK, holdMs); }

bool tryConsole(const String &cmd, const String &arg, Stream &io)
{
    if (cmd != "lock" && cmd != "unlock") return false;

    // 선택 인자: 누름 유지시간(ms). 미지정/무효면 기본값(press 내부에서 적용).
    long ms = arg.length() ? arg.toInt() : 0;
    if (ms < 0) ms = 0;
    if (ms > CARKEY_PRESS_MAX_MS) {
        io.printf("[KEY] %ldms 는 상한 초과 — %dms 로 제한합니다\n", ms, CARKEY_PRESS_MAX_MS);
        ms = CARKEY_PRESS_MAX_MS;
    }

    if (!press(cmd == "lock" ? Button::LOCK : Button::UNLOCK, (uint16_t)ms)) {
        io.println("[KEY] 다른 누름이 끝난 뒤 다시 시도하세요");
    }
    return true;
}

} // namespace Carkey
