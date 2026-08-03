/**
 * @file      clk.h
 * @brief     시각 기준(base) — 오프라인 적재·고빈도 창이 공유하는 단일 시각 규약
 *
 * 시각 소스(GPS UTC / 모뎀 CCLK)는 음영·미등록 구간에서 끊긴다. 끊긴 동안에도
 * "지금 몇 시인가"를 답할 수 있어야 오프라인 레코드에 발생 시각을 실을 수 있다.
 *   → 권위 시각을 얻을 때마다 base = (epoch, millis()) 쌍으로 기억하고,
 *     소스가 없는 구간은 millis() 외삽으로 답한다.
 *
 * 드리프트: ESP32 크리스탈 ±20ppm → 1시간 ±0.07초 / 하루 ±1.7초.
 * 서버 저장 단위가 초라 장시간 음영에서도 문제되지 않는다.
 *
 * ⚠️ 기존 Mqtt::publishTelemetry() 의 ts 산출(GPS 권위 → 모뎀 폴백 → 단조증가 가드)은
 *    건드리지 않는다. 0.2.11 에서 어렵게 확보한 동작이라, epoch 를 얻은 지점에서
 *    note() 만 추가로 부른다. 실시간 발행 ts 는 지금까지와 동일하게 나간다.
 *
 * 설계: offline-telemetry-buffer.md §4
 */
#pragma once

#include <Arduino.h>

namespace Clk {

// 시각 출처. 오프라인 레코드 flags b0-1 에 그대로 들어간다(2비트).
enum Src : uint8_t {
    SRC_NONE    = 0,   // base 없음 — 시각 미상
    SRC_DERIVED = 1,   // base 는 있으나 소스가 끊긴 지 오래 — millis 외삽
    SRC_MODEM   = 2,   // 모뎀 CCLK 에서 갱신됨
    SRC_GPS     = 3,   // GPS UTC 에서 갱신됨(권위)
};

// 권위 시각 확보 시 호출 — base 를 (epoch, millis()) 로 갱신한다.
// epoch 가 명백히 엉뚱하면(2024 이전/2060 이후) 무시한다.
// 같은 순간에 GPS 와 모뎀이 모두 있으면 GPS 가 이긴다(모뎀 note 가 GPS base 를
// 덮어써 출처가 격하되는 것을 막는다 — 두 소스의 오차는 초 단위로 무의미하다).
void note(uint32_t epoch, Src src);

// UTC 민간시각 버전 — GPS fix 의 연·월·일·시·분·초를 그대로 넘긴다.
// (Mqtt 에 의존하지 않으려고 변환을 여기 둔다 — FEATURE_LTE=0 빌드에서도 GPS 폴이 쓴다.)
void noteCivil(int y, int mo, int d, int hh, int mi, int ss, Src src);

// UTC 민간시각 → epoch(초). days_from_civil (Howard Hinnant).
uint32_t fromCivil(int y, int mo, int d, int hh, int mi, int ss);

// 현재 epoch(초). base 가 없으면 0 을 반환하고 srcOut 은 SRC_NONE 이다.
// srcOut 은 "이 값이 얼마나 믿을 만한가" — base 갱신이 최근이면 그 소스, 오래됐으면 DERIVED.
uint32_t now(Src *srcOut = nullptr);

// 마지막 base 의 출처(신선도 반영). base 가 없으면 SRC_NONE.
Src source();

const char *srcName(Src s);

} // namespace Clk
