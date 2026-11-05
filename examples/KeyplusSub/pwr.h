/**
 * @file      pwr.h
 * @brief     INA228 전압·전류 감시 — 서브 (작업서 S6 1단계)
 *
 * I2C 0x40(A0=A1=GND), 상시 3.3V. 하이사이드 션트 R002(2mΩ) · ADCRANGE=1(±40.96mV → ±20.48A).
 * 연속 측정(VBUS·VSHUNT·온도, 각 1052µs × 평균 128 ≈ 0.4초)을 켜 두고 1초마다 결과만 읽는다.
 *   전압 = VBUS(IN+ 쪽 = 차량 배터리 쪽), 전류 = VSHUNT / 션트 — SHUNT_CAL 은 쓰지 않는다(환산을 한 곳에).
 * 없으면 AIR_PROBE_MS 마다 다시 찾는다. 상시 레일이라 온도 가드와 무관하다.
 *
 * 다음 단계(측정 확인 후): POWER 프레임 → 메인 telemetry pwr{v,i}, 저전압 2단 상태머신(HARDWARE §2.3).
 *
 * 콘솔: pwr (현재 값·최소/최대·통계) | pwr reset (최소/최대 초기화)
 */
#pragma once

#include <Arduino.h>

namespace Pwr {

void begin();
void update();                       // loop 매 틱

// 최근 값이 유효하면 true. mV · mA(부호 있음 — 역류면 음수).
bool get(int32_t *mv, int32_t *ma);

bool tryConsole(const char *line);
void printStatus(Stream &io);

} // namespace Pwr
