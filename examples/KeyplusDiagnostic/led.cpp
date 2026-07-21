/**
 * @file      led.cpp
 * @brief     상태표시 LED — 타이머(Ticker) 구동 패턴 엔진 구현.
 *
 * ⚠️ loop()는 AT 명령(Lte/Mqtt/Provision/GPS)에서 수초씩 블로킹된다. LED를 loop에서만
 *    그리면 그 동안 얼어붙으므로, Ticker(esp_timer, 고우선순위 태스크)로 25ms마다 패턴을
 *    렌더한다. → loop 블로킹과 무관하게 LED가 계속 동작한다.
 *
 * FEATURE_STATUS_LED=0 이면 모든 함수가 no-op이며 GPIO/타이머를 건드리지 않는다
 * (5b 차키 단계에서 GPIO23을 회수할 때 사용).
 */
#include "led.h"
#include "config.h"
#include <Ticker.h>

namespace Led {

static State    s_state     = State::BOOT;
static uint32_t s_epoch     = 0;    // 현재 상태 진입 시각(millis)
static int      s_lastLevel = -1;   // 마지막 출력 레벨(중복 digitalWrite 방지, -1=미출력)
static Ticker   s_ticker;           // 25ms 주기 렌더 타이머

// --- 패턴 타이밍(ms) -------------------------------------------------------
static const uint32_t BURST_ON_MS  = 150;   // 버스트 1펄스 점등
static const uint32_t BURST_OFF_MS = 150;   // 버스트 1펄스 소등
static const uint32_t BURST_GAP_MS = 1600;  // 버스트 후 정지(다음 버스트까지)
static const uint32_t SLOW_MS      = 500;   // 느린 점멸 반주기(접속 시도 중)
static const uint32_t HB_ON_MS     = 60;    // heartbeat 점등 폭(25ms 틱에 안정 포착)
static const uint32_t HB_PERIOD_MS = 3000;  // heartbeat 주기(정상 접속)
static const uint32_t TICK_MS      = 25;    // 렌더 주기

static void writeLevel(bool on)
{
#if FEATURE_STATUS_LED
    int lv = on ? (LED_ON_LEVEL) : (!LED_ON_LEVEL);
    if (lv != s_lastLevel) {              // 변화 시에만 출력
        digitalWrite(PIN_STATUS_LED, lv);
        s_lastLevel = lv;
    }
#else
    (void)on;
#endif
}

// N회 버스트: (점등+소등)×N 후 정지. elapsed(ms) 기준 현재 점등 여부 반환.
static bool burst(uint32_t elapsed, int pulses)
{
    const uint32_t unit  = BURST_ON_MS + BURST_OFF_MS;
    const uint32_t cycle = (uint32_t)pulses * unit + BURST_GAP_MS;
    uint32_t t = elapsed % cycle;
    for (int i = 0; i < pulses; i++) {
        uint32_t start = (uint32_t)i * unit;
        if (t >= start && t < start + BURST_ON_MS) return true;
    }
    return false;
}

// 현재 상태의 패턴을 계산해 LED에 출력. Ticker와 update() 양쪽에서 호출(멱등).
static void render()
{
    uint32_t elapsed = millis() - s_epoch;   // millis 오버플로우 안전(부호없음 감산)
    bool on = false;
    switch (s_state) {
        case State::BOOT:          on = true;                                break;
        case State::PROVISIONING:  on = ((elapsed / SLOW_MS) & 1U) == 0U;    break;
        case State::UNPROVISIONED: on = burst(elapsed, 1);                   break;
        case State::REJECTED:      on = burst(elapsed, 2);                   break;
        case State::COMM_ERROR:    on = burst(elapsed, 3);                   break;
        case State::MQTT_OK:       on = (elapsed % HB_PERIOD_MS) < HB_ON_MS; break;
    }
    writeLevel(on);
}

void begin()
{
#if FEATURE_STATUS_LED
    pinMode(PIN_STATUS_LED, OUTPUT);
#endif
    s_state     = State::BOOT;
    s_epoch     = millis();
    s_lastLevel = -1;
    writeLevel(true);   // BOOT = solid ON
#if FEATURE_STATUS_LED
    // loop 블로킹과 무관하게 패턴을 유지하도록 타이머로 렌더 구동.
    s_ticker.detach();
    s_ticker.attach_ms(TICK_MS, render);
#endif
}

void set(State s)
{
    if (s == s_state) return;   // 동일 상태 재설정은 무시(패턴 위상 유지)
    s_state = s;
    s_epoch = millis();
}

State get() { return s_state; }

// 호환용 수동 렌더 — Ticker가 이미 구동하므로 필수는 아니나, loop에서 불러도 무해(멱등).
void update() { render(); }

} // namespace Led
