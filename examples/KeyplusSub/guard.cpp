/**
 * @file      guard.cpp
 * @brief     온도 가드 구현. 설계는 guard.h.
 */
#include "guard.h"
#include "config.h"
#include "pms.h"
#include "co.h"

namespace Guard {

static KpAir::Guard s_state    = KpAir::Guard::NORMAL;
static bool         s_known    = false;
static bool         s_railOn   = false;
static bool         s_noSensor = false;   // SHT41 없이 켰다(감시 불가)
static bool         s_pwrBlock = false;   // 저전압 경고·차단 중(pwr.cpp)
static bool         s_simOn    = false;
static float        s_simT     = 0;
static float        s_lastT    = NAN;
static uint32_t     s_changeAt = 0;
static uint32_t     s_trips    = 0;

static const char *stateName(KpAir::Guard s)
{
    switch (s) {
    case KpAir::Guard::HOT:  return "고온 차단";
    case KpAir::Guard::COLD: return "저온 차단";
    default:                 return "정상";
    }
}

static void applyRails(bool on)
{
    if (on == s_railOn) return;
    s_railOn = on;
    const uint8_t lvl = on ? HIGH : LOW;
    digitalWrite(PIN_RAIL_SENSOR_3V3, lvl);
    digitalWrite(PIN_RAIL_PMS_5V, lvl);
    digitalWrite(PIN_RAIL_CO_5V, lvl);
    Pms::setPower(on);
    Co::setPower(on);
}

// 레일은 온도가 정상이고 저전압이 아닐 때만 켠다. 첫 판단 전에는 끈 채로 둔다.
static void refresh() { applyRails(s_known && s_state == KpAir::Guard::NORMAL && !s_pwrBlock); }

static void enter(KpAir::Guard next, float t, uint32_t now)
{
    const bool first = !s_known;
    s_known    = true;
    s_state    = next;
    s_changeAt = now;
    if (next != KpAir::Guard::NORMAL) {
        s_trips++;
        Serial.printf("[GUARD] %s — %.1f°C, 센서 레일 차단(GPIO%d·%d·%d)\n", stateName(next), t,
                      PIN_RAIL_PMS_5V, PIN_RAIL_SENSOR_3V3, PIN_RAIL_CO_5V);
    } else {
        Serial.printf("[GUARD] %s — %.1f°C, 센서 레일 %s\n", first ? "첫 판단" : "복귀", t,
                      s_pwrBlock ? "꺼 둠 (저전압 — pwr)" : first ? "켬" : "켬 (복귀 직후 값은 버린다)");
    }
    refresh();
}

void begin()
{
    // 핀 방향·LOW 는 setup 이 이미 잡았다(외부 풀다운과 같은 상태). 여기서는 상태만 맞춘다.
    s_railOn = false;
    Pms::setPower(false);
    Co::setPower(false);
}

void update(uint32_t now, bool valid, float tempC)
{
#if !GUARD_TEMP_ENABLE
    // 온도 차단 꺼짐 — 첫 호출에서 바로 "정상"으로 확정하고 레일을 켠다(SHT41 첫 값을 기다리지 않는다).
    // SHT41 유무는 계속 본다: LED 센서 고장 표시(noSensor)와 상태 출력용 온도.
    if (valid) {
        s_noSensor = false;
        s_lastT    = tempC;
    } else if (now >= GUARD_BOOT_WAIT_MS && isnan(s_lastT)) {
        s_noSensor = true;
    }
    if (!s_known) {
        s_known    = true;
        s_state    = KpAir::Guard::NORMAL;
        s_changeAt = now;
        Serial.printf("[GUARD] 온도 가드 꺼짐(GUARD_TEMP_ENABLE=0) — 센서 레일 %s\n",
                      s_pwrBlock ? "꺼 둠 (저전압 — pwr)" : "켬");
        refresh();
    }
    return;
#endif
    if (s_simOn) {
        valid = true;
        tempC = s_simT;
    }
    if (!valid) {
        // 부팅 후 SHT41 이 끝내 안 나오면 감시 없이 켠다. 운전 중 끊기면 마지막 상태 유지.
        if (!s_known && now >= GUARD_BOOT_WAIT_MS) {
            s_noSensor = true;
            Serial.println("[GUARD] ⚠️ SHT41 응답 없음 — 온도 감시 없이 센서 레일을 켠다");
            enter(KpAir::Guard::NORMAL, NAN, now);
        }
        return;
    }
    s_noSensor = false;
    s_lastT    = tempC;

    KpAir::Guard next = s_state;
    switch (s_state) {
    case KpAir::Guard::NORMAL:
        if (tempC > GUARD_HOT_OFF_C)       next = KpAir::Guard::HOT;
        else if (tempC < GUARD_COLD_OFF_C) next = KpAir::Guard::COLD;
        break;
    case KpAir::Guard::HOT:
        if (tempC < GUARD_HOT_ON_C)        next = KpAir::Guard::NORMAL;
        break;
    case KpAir::Guard::COLD:
        if (tempC > GUARD_COLD_ON_C)       next = KpAir::Guard::NORMAL;
        break;
    }
    if (!s_known || next != s_state) enter(next, tempC, now);
}

void setPowerBlock(bool block)
{
    if (block == s_pwrBlock) return;
    s_pwrBlock = block;
    refresh();
}

bool         powerBlocked() { return s_pwrBlock; }
bool         known()     { return s_known; }
bool         noSensor()  { return s_noSensor; }
bool         sensorsOn() { return s_railOn; }
KpAir::Guard state()     { return s_state; }

bool tryConsole(const char *line)
{
    if (!strcmp(line, "guard")) {
        printStatus(Serial);
        return true;
    }
    if (!strcmp(line, "guard sim off")) {
        s_simOn = false;
        Serial.println("[GUARD] 가짜 온도 끔 — SHT41 값으로 판단");
        return true;
    }
    if (!strncmp(line, "guard sim ", 10)) {
#if !GUARD_TEMP_ENABLE
        Serial.println("[GUARD] 온도 가드가 꺼져 있다(GUARD_TEMP_ENABLE=0) — 가짜 온도는 판단에 쓰이지 않는다");
        return true;
#endif
        char *end;
        const float t = strtof(line + 10, &end);
        if (end == line + 10) {
            Serial.println("[GUARD] 사용법: guard sim <°C> | guard sim off");
            return true;
        }
        s_simOn = true;
        s_simT  = t;
        Serial.printf("[GUARD] 가짜 온도 %.1f°C — 다음 측정(1초 안)부터 이 값으로 판단\n", t);
        return true;
    }
    return false;
}

void printStatus(Stream &io)
{
    io.println(GUARD_TEMP_ENABLE ? "[GUARD] 온도 가드"
                                 : "[GUARD] 온도 가드 — 꺼짐(GUARD_TEMP_ENABLE=0): 레일은 저전압 때만 끈다");
    io.printf("  state        : %s%s%s, 레일 %s (%lus 전 바뀜)\n", s_known ? stateName(s_state) : "판단 전",
              s_noSensor ? " · ⚠️ SHT41 없음(감시 불가)" : "", s_pwrBlock ? " · 저전압 차단(pwr)" : "",
              s_railOn ? "켬" : "끔",
              (unsigned long)((millis() - s_changeAt) / 1000));
    io.printf("  온도         : %.1f°C%s\n", s_lastT, s_simOn ? " (가짜 — guard sim off 로 해제)" : "");
    io.printf("  기준         : >%.0f 차단 / <%.0f 복귀, <%.0f 차단 / >%.0f 복귀\n", GUARD_HOT_OFF_C,
              GUARD_HOT_ON_C, GUARD_COLD_OFF_C, GUARD_COLD_ON_C);
    // 출력 전용 핀은 digitalRead 가 실제 레벨을 못 읽는다(입력 버퍼 꺼짐) — 쓴 값을 보여 준다.
    io.printf("  GPIO         : %d·%d·%d = %s, 차단 횟수 %lu\n", PIN_RAIL_PMS_5V, PIN_RAIL_SENSOR_3V3,
              PIN_RAIL_CO_5V, s_railOn ? "HIGH" : "LOW", (unsigned long)s_trips);
}

} // namespace Guard
