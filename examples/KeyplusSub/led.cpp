/**
 * @file      led.cpp
 * @brief     서브 상태 LED 구현. 설계는 led.h.
 */
#include "led.h"
#include "config.h"
#include "co.h"
#include "guard.h"
#include "pwr.h"

namespace Led {

enum class Pat : uint8_t { OFF, FAULT, GUARD, UV_WARN, UV_CUT, KEY };

static const char *patName(Pat p)
{
    switch (p) {
    case Pat::KEY:     return "차키 누름 (계속 켬)";
    case Pat::UV_CUT:  return "저전압 차단 (빠른 점멸)";
    case Pat::UV_WARN: return "저전압 경고 (3번)";
    case Pat::GUARD:   return "온도 가드 (2번)";
    case Pat::FAULT:   return "센서 고장 (1번)";
    default:           return "정상 (꺼짐)";
    }
}

static uint8_t  s_keys    = 0;          // 눌린 채널 비트(bit0 = io1)
static Pat      s_pat     = Pat::OFF;
static uint32_t s_patAt   = 0;          // 패턴이 바뀐 시각 — 주기를 여기서 시작해 첫 깜빡임이 바로 보이게
static bool     s_level   = false;
static bool     s_test    = false;
static uint32_t s_testAt  = 0;

static void write(bool on)
{
    if (on == s_level) return;
    s_level = on;
    digitalWrite(PIN_STATUS_LED, on ? LED_ON_LEVEL : !LED_ON_LEVEL);
}

void begin()
{
    digitalWrite(PIN_STATUS_LED, !LED_ON_LEVEL);
    pinMode(PIN_STATUS_LED, OUTPUT);
    s_level = false;
}

void setKey(uint8_t io, bool pressed)
{
    if (io < 1 || io > 8) return;
    const uint8_t bit = (uint8_t)(1u << (io - 1));
    s_keys = pressed ? (s_keys | bit) : (s_keys & ~bit);
}

void clearKeys() { s_keys = 0; }

// 사유를 모아 이번에 보일 패턴을 고른다. 우선순위는 led.h 표.
static bool sensorFault(uint32_t now)
{
    if (now < LED_FAULT_GRACE_MS) return false;
    return Guard::noSensor() || !Pwr::present() || Co::fault();
}

static Pat pick(uint32_t now)
{
    if (s_keys) return Pat::KEY;
    switch (Pwr::uv()) {
    case KpPower::Uv::CUT:  return Pat::UV_CUT;
    case KpPower::Uv::WARN: return Pat::UV_WARN;
    default: break;
    }
    if (Guard::known() && Guard::state() != KpAir::Guard::NORMAL) return Pat::GUARD;
    if (sensorFault(now)) return Pat::FAULT;
    return Pat::OFF;
}

static uint8_t blinks(Pat p)
{
    switch (p) {
    case Pat::UV_WARN: return 3;
    case Pat::GUARD:   return 2;
    case Pat::FAULT:   return 1;
    default:           return 0;
    }
}

static bool levelOf(Pat p, uint32_t t)
{
    switch (p) {
    case Pat::KEY:    return true;
    case Pat::OFF:    return false;
    case Pat::UV_CUT: return (t % 200) < 100;
    default: {
        // 2초 주기 안에서 300ms 간격으로 120ms 씩 n 번 켜고 나머지는 쉰다.
        const uint32_t ph = t % LED_PERIOD_MS;
        const uint32_t k  = ph / LED_BLINK_GAP_MS;
        return k < blinks(p) && (ph % LED_BLINK_GAP_MS) < LED_BLINK_ON_MS;
    }
    }
}

void update()
{
    const uint32_t now = millis();
    Pat p;
    if (s_test && !s_keys) {
        // OFF 를 뺀 5개를 3초씩 돌린다.
        static const Pat order[] = {Pat::FAULT, Pat::GUARD, Pat::UV_WARN, Pat::UV_CUT, Pat::KEY};
        p = order[((now - s_testAt) / 3000) % (sizeof(order) / sizeof(order[0]))];
    } else {
        p = pick(now);
    }
    if (p != s_pat) {
        s_pat   = p;
        s_patAt = now;
        if (!s_test) Serial.printf("[LED] %s\n", patName(p));
    }
    write(levelOf(p, now - s_patAt));
}

bool tryConsole(const char *line)
{
    if (!strcmp(line, "led")) {
        printStatus(Serial);
        return true;
    }
    if (!strcmp(line, "led test")) {
        s_test   = !s_test;
        s_testAt = millis();
        Serial.println(s_test ? "[LED] 시험 — 센서 고장 → 온도 가드 → 저전압 경고 → 저전압 차단 → 차키, 3초씩 (다시 led test 로 끔)"
                              : "[LED] 시험 끔 — 실제 상태로");
        return true;
    }
    return false;
}

void printStatus(Stream &io)
{
    const uint32_t now = millis();
    io.printf("[LED] GPIO%d (켬=%s)\n", PIN_STATUS_LED, LED_ON_LEVEL == HIGH ? "HIGH" : "LOW");
    io.printf("  패턴         : %s%s\n", patName(s_pat), s_test ? " — 시험 중(led test 로 끔)" : "");
    io.printf("  사유         : 차키 %s · 저전압 %s · 온도 가드 %s\n", s_keys ? "누름" : "-",
              KpPower::uvName(Pwr::uv()),
              !Guard::known() ? "판단 전" : Guard::state() == KpAir::Guard::NORMAL ? "정상" : "차단");
    if (now < LED_FAULT_GRACE_MS)
        io.printf("  센서 고장    : 부팅 직후라 아직 안 봄(%lus 남음)\n",
                  (unsigned long)((LED_FAULT_GRACE_MS - now + 999) / 1000));
    else
        io.printf("  센서 고장    : SHT41 %s · INA228 %s · ZE15-CO %s\n", Guard::noSensor() ? "⚠️없음" : "정상",
                  Pwr::present() ? "정상" : "⚠️없음", Co::fault() ? "⚠️고장 비트" : "정상");
}

} // namespace Led
