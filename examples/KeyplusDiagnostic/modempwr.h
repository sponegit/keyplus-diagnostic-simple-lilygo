/**
 * @file      modempwr.h
 * @brief     모뎀 유휴 슬립 관리 (AT+CSCLK=1 + DTR 제어)
 *
 * 실측(2026-08-10, PPK2 5V): 모뎀이 망에 붙어 아무것도 안 하는 동안 **47mA** 를 먹는다.
 * mqtt 상태 120mA 중 40% 다. 원인은 슬립을 못 하게 막아둔 것이다 —
 * `modemPowerOn()` 이 DTR 을 LOW 로 고정하고 AT+CSCLK 설정이 아예 없었다.
 *
 * ── 동작 원리 ───────────────────────────────────────────────────────────────
 *   AT+CSCLK=1 : "DTR 이 HIGH 면 자도 된다" 로 모뎀에 위임한다.
 *   DTR HIGH   : 모뎀이 UART 를 끄고 슬로우클럭으로 내려간다(baseband 는 계속 살아
 *                있어 망 등록·페이징·TCP 세션이 유지된다 — 이게 핵심이다).
 *   DTR LOW    : 깨운다. UART 가 살아나기까지 최소 50ms 필요(A76xx 문서 기준).
 *
 * ⚠️ 자는 동안 보낸 AT 는 **응답 없이 사라진다.** 펌웨어는 그걸 "모뎀 무응답"으로
 *    읽고 프로브 실패 → 모뎀 리셋 경로를 탄다. 그래서 AT 를 보내는 모든 자리에서
 *    먼저 깨워야 하는데, 그 자리가 라이브러리 내부까지 흩어져 있어 열거가 불가능하다.
 *    → TinyGSM 의 sendAT 에 훅(`tinyGsmModemWakeHook`)을 걸어 한 곳에서 처리한다.
 *      이 파일이 그 훅을 정의한다. 즉 **AT 를 보내는 경로는 자동으로 안전하다.**
 *
 * ⚠️ 반대 방향(모뎀 → ESP32)은 훅으로 못 막는다. 자고 있던 모뎀이 URC 를 보내려고
 *    스스로 깨어날 때 첫 바이트가 깨질 여지가 있다. 차키 명령(+CMQTTRX*)이 그 경로라
 *    **이 기능의 합격 조건은 "재우는 동안에도 차키 명령이 제때 도착한다"** 이다.
 *    소비전류만 보고 합격시키면 안 된다.
 *    → 만약 명령 유실이 관찰되면 다음 수단은 RI 핀(MODEM_RING_PIN=GPIO33)이다.
 *      A76xx 는 슬립 중 URC 를 낼 때 RI 를 먼저 어서트하므로, 그 엣지에서 DTR 을
 *      내려 UART 를 먼저 깨우면 첫 바이트를 잃지 않는다. 지금은 배선/인터럽트를
 *      추가하지 않고 모뎀의 자체 기상만 믿는 단순한 형태로 먼저 검증한다.
 *
 * 재우는 판단은 "마지막 AT 이후 MODEM_SLEEP_IDLE_MS 동안 조용했는가" 하나뿐이다.
 * 접속·발행처럼 AT 가 연달아 나가는 구간은 자동으로 깨어 있는 상태가 유지되므로
 * 구간마다 별도 가드를 둘 필요가 없다.
 */
#pragma once

#include <Arduino.h>
// utilities.h 가 TINY_GSM_MODEM_A7670 를 정의하므로 TinyGsmClient.h 보다 먼저 포함한다
// (gps.h/lte.h 와 같은 규칙). TinyGsm 은 타입 별칭이라 전방선언이 불가능하다.
#include "utilities.h"
#include <TinyGsmClient.h>
#include "config.h"

#if FEATURE_MODEM_SLEEP

namespace ModemPwr {

// AT+CSCLK=1 적용. LTE 브링업이 끝난 뒤, 그리고 **모뎀 리셋 때마다** 부른다
// (리셋하면 CSCLK 가 0 으로 돌아간다). 실패해도 동작에는 지장이 없다 — 안 자는 것뿐이다.
bool begin(TinyGsm &modem);

// AT+CSCLK 이 실제로 걸렸는가. false 면 아래 함수들은 전부 무동작이다.
bool enabled();

// 현재 슬립 방식 — 1=DTR 슬립, 2=RX 슬립. 로그에 상수를 박지 말고 이걸 쓸 것
// (실제로는 2 를 걸면서 "CSCLK=1" 이라 찍던 버그가 있었다).
uint8_t mode();

// 런타임 on/off — PPK2 를 물린 채 재플래싱 없이 A/B 비교하려고 둔다.
// off 는 즉시 깨워서 DTR 을 LOW 로 고정한다(기존 동작과 동일해진다).
void setEnabled(TinyGsm &modem, bool on);

// 깨우기. 자고 있었으면 DTR LOW 후 MODEM_WAKE_SETTLE_MS 만큼 블로킹 대기한다.
// 이미 깨어 있으면 즉시 반환한다 — sendAT 훅이 매번 부르므로 이 경로가 싸야 한다.
void wake();

// 마지막 AT 이후 MODEM_SLEEP_IDLE_MS 이상 조용했으면 재운다(DTR HIGH). 아니면 무동작.
// loop 끝에서 매 틱 부른다.
void allowSleepIfIdle(uint32_t now);

// 지금 자고 있는가.
bool asleep();

// 누적 수면시간(ms)과 깨운 횟수 — 절감 효과가 실제로 나는지 보는 계기판.
// sleepRatioPct 는 begin 이후 전체 시간 대비 수면 비율이다.
uint32_t sleptMs();
uint32_t wakeCount();
uint8_t  sleepRatioPct(uint32_t now);

// 콘솔 'msleep [on|off]'. 처리했으면 true.
bool tryConsole(const String &cmd, const String &arg, TinyGsm &modem, Stream &io);

}  // namespace ModemPwr

#endif  // FEATURE_MODEM_SLEEP
