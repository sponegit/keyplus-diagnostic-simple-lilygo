/**
 * @file      wake.cpp
 * @brief     light sleep 기상 경로 진단 구현 — 설계 근거는 wake.h 참고.
 */
#include "wake.h"

#if FEATURE_WAKE_PROBE

#include "utilities.h"     // MODEM_RING_PIN
#include "log.h"
#include <esp_sleep.h>
#include <driver/uart.h>

namespace {

Stream *g_modemStream = nullptr;

// ISR 이 건드리는 값은 전부 volatile. 폭은 마이크로초로 잰다 — 60ms 인지 확인해야
// "이게 정말 모뎀의 URC 통지인가"를 판단할 수 있다(잡음이면 폭이 제멋대로다).
volatile uint32_t g_fall      = 0;   // 하강 엣지 수 = URC 통지 횟수
volatile uint32_t g_rise      = 0;
volatile uint32_t g_lastFallUs = 0;
volatile uint32_t g_lastWidthUs = 0; // 마지막 LOW 펄스 폭
volatile uint32_t g_lastFallMs = 0;

void IRAM_ATTR riIsr()
{
    uint32_t us = micros();
    if (digitalRead(MODEM_RING_PIN) == LOW) {
        g_fall++;
        g_lastFallUs = us;
        g_lastFallMs = millis();
    } else {
        if (g_lastFallUs) g_lastWidthUs = us - g_lastFallUs;
        g_rise++;
    }
}

const char *causeStr(esp_sleep_wakeup_cause_t c)
{
    switch (c) {
        case ESP_SLEEP_WAKEUP_TIMER: return "TIMER(아무것도 안 깨움)";
        case ESP_SLEEP_WAKEUP_EXT0:  return "EXT0 = RI 핀";
        case ESP_SLEEP_WAKEUP_EXT1:  return "EXT1";
        case ESP_SLEEP_WAKEUP_UART:  return "UART 엣지";
        case ESP_SLEEP_WAKEUP_GPIO:  return "GPIO";
        case ESP_SLEEP_WAKEUP_UNDEFINED: return "UNDEFINED(수면 진입 실패 가능)";
        default: return "기타";
    }
}

// 실제로 light sleep 에 들어갔다 나오면서 **무엇이 깨웠는지**를 본다.
// 이 한 번의 시험이 light sleep 설계 전체를 결정한다.
void sleepTrial(Stream &io, uint32_t sec)
{
    if (sec == 0 || sec > 60) sec = 20;

    io.printf("[WAKE] light sleep %lu초 진입 — 이 구간의 PPK2 전류가 곧 light sleep 실측치다.\n",
              (unsigned long)sec);
    io.println("       지금 서버에서 차키 명령을 보내면 그게 깨우는지까지 한 번에 확인된다.");
    io.flush();
    delay(50);                     // 위 문장이 UART 로 다 빠져나갈 시간

    // ⚠️ ext0 와 attachInterrupt 는 같은 핀을 두고 다툰다. 시험 동안만 떼었다 붙인다.
    detachInterrupt(digitalPinToInterrupt(MODEM_RING_PIN));

    esp_sleep_enable_timer_wakeup((uint64_t)sec * 1000000ULL);   // 아무도 안 깨울 때의 탈출구
    // RI 는 평소 HIGH, URC 직전 LOW 로 떨어진다 → LOW(0) 에서 깨운다.
    esp_err_t e0 = esp_sleep_enable_ext0_wakeup((gpio_num_t)MODEM_RING_PIN, 0);
    // UART1 = 모뎀. 엣지 3개면 깨운다(URC 앞 "\r\n" 이 희생 바이트가 되는 설계).
    esp_err_t eu = ESP_OK;
    if (g_modemStream) {
        uart_set_wakeup_threshold(UART_NUM_1, 3);
        eu = esp_sleep_enable_uart_wakeup(UART_NUM_1);
    }

    uint32_t t0 = millis();
    esp_err_t es = esp_light_sleep_start();
    uint32_t slept = millis() - t0;
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    // 기상 직후의 UART 상태를 그대로 찍는다 — URC 가 살아남았는지가 여기서 갈린다.
    int avail = g_modemStream ? g_modemStream->available() : -1;
    int first = (avail > 0) ? g_modemStream->peek() : -1;

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    attachInterrupt(digitalPinToInterrupt(MODEM_RING_PIN), riIsr, CHANGE);

    io.printf("[WAKE] 기상: 원인=%s  잔 시간=%lums (요청 %lu초)\n",
              causeStr(cause), (unsigned long)slept, (unsigned long)sec);
    io.printf("       설정: ext0(RI)=%s uart=%s sleep=%s\n",
              e0 == ESP_OK ? "ok" : esp_err_to_name(e0),
              g_modemStream ? (eu == ESP_OK ? "ok" : esp_err_to_name(eu)) : "미사용",
              es == ESP_OK ? "ok" : esp_err_to_name(es));
    if (avail >= 0) {
        io.printf("       모뎀 UART 대기 바이트=%d 첫바이트=%s\n", avail,
                  first < 0 ? "없음" : (first == '+' ? "'+'(URC 본문 살아있음)"
                                      : first == '\r' ? "CR" : first == '\n' ? "LF" : "기타"));
    }

    // 판정 — 다음 설계가 여기서 갈린다.
    if (es != ESP_OK) {
        io.println("       → ❌ light sleep 진입 자체가 실패했다. 다른 기상소스/드라이버 충돌을 본다.");
    } else if (cause == ESP_SLEEP_WAKEUP_EXT0) {
        io.println("       → ✅ RI(GPIO33)가 깨웠다 = **배선 있음**. 이게 최선의 경로다.");
        io.println("          URC 본문 전에 60ms 여유가 있어 바이트 유실 없이 재울 수 있다.");
    } else if (cause == ESP_SLEEP_WAKEUP_UART) {
        io.println("       → ⭕ UART 엣지가 깨웠다. RI 없이도 재울 수 있다.");
        io.println("          단 앞쪽 바이트가 희생되므로 위 '첫바이트'가 '+' 인지 반드시 확인할 것.");
    } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
        io.println("       → ⚠️ 아무도 안 깨웠다. 이 구간에 URC 가 없었으면 정상이다 —");
        io.println("          발행(30초)이나 차키 명령을 걸어두고 다시 시험할 것.");
        io.println("          URC 가 분명히 있었는데도 TIMER 면 두 경로 다 실패다.");
    }
    io.printf("       (참고) RI 누적 하강=%lu 마지막 펄스폭=%luus\n",
              (unsigned long)g_fall, (unsigned long)g_lastWidthUs);
}

}  // namespace

namespace Wake {

void begin(Stream *modemStream)
{
    g_modemStream = modemStream;
    // ⚠️ INPUT_PULLUP 으로 둔다. RI 는 평소 HIGH 라 배선이 있으면 풀업과 같은 상태이고,
    //    **배선이 없으면 뜬 핀이 잡음을 주워 가짜 엣지를 만든다** — 풀업이 그걸 막아
    //    "엣지 0회 = 배선 없음" 을 신뢰할 수 있게 한다.
    pinMode(MODEM_RING_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(MODEM_RING_PIN), riIsr, CHANGE);
}

bool tryConsole(const String &cmd, const String &arg, Stream &io)
{
    if (cmd != "wake") return false;

    String a = arg; a.trim(); a.toLowerCase();
    if (a.startsWith("sleep")) {
        String v = a.substring(5); v.trim();
        sleepTrial(io, (uint32_t)v.toInt());
        return true;
    }
    if (a.length()) {
        io.println("사용법: wake [sleep [초]]   (인자 없으면 RI 엣지 카운터 조회)");
        return true;
    }

    uint32_t now  = millis();
    uint32_t fall = g_fall;
    uint32_t lastMs = g_lastFallMs;
    io.printf("[WAKE] RI=GPIO%d 현재=%s 하강=%lu회 상승=%lu회 마지막펄스폭=%luus\n",
              MODEM_RING_PIN, digitalRead(MODEM_RING_PIN) ? "HIGH(유휴)" : "LOW",
              (unsigned long)fall, (unsigned long)g_rise, (unsigned long)g_lastWidthUs);
    if (fall == 0) {
        io.printf("       → 엣지 0회 (부팅 후 %lu초). URC 가 오갔는데도 0 이면 **RI 미배선**이다.\n",
                  (unsigned long)(now / 1000UL));
        io.println("          [MQTT] telemetry published 가 몇 줄 지난 뒤에 다시 볼 것(발행마다 +CMQTTPUB URC 가 뜬다).");
    } else {
        io.printf("       → 마지막 엣지 %lu초 전. 발행 주기와 맞아떨어지면 RI 배선이 살아 있는 것이다.\n",
                  (unsigned long)((now - lastMs) / 1000UL));
        io.println("          펄스폭이 60ms(60000us) 안팎이면 매뉴얼 3.3.2 의 URC 통지가 맞다.");
    }
    return true;
}

}  // namespace Wake

#endif  // FEATURE_WAKE_PROBE
