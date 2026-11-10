/**
 * @file      pwr.h
 * @brief     INA228 전압·전류 감시 — 서브 (작업서 S6 1단계)
 *
 * I2C 0x40(A0=A1=GND), 상시 3.3V. 하이사이드 션트 R002(2mΩ) · ADCRANGE=1(±40.96mV → ±20.48A).
 * 연속 측정(VBUS·VSHUNT·온도, 각 1052µs × 평균 128 ≈ 0.4초)을 켜 두고 1초마다 결과만 읽는다.
 *   전압 = VBUS(IN+ 쪽 = 차량 배터리 쪽), 전류 = VSHUNT / 션트 — SHUNT_CAL 은 쓰지 않는다(환산을 한 곳에).
 * 없으면 AIR_PROBE_MS 마다 다시 찾는다. 상시 레일이라 온도 가드와 무관하다.
 *
 * 2단계: 저전압 2단 판정(lib/KpLink KpPower::UvMachine, HARDWARE §2.3) + POWER 프레임 발행
 *   (PWR_PUB_MS 주기, 상태가 바뀌면 바로). 경고·소프트 차단 중에는 센서 레일을 끈다(Guard::setPowerBlock).
 *   모뎀 차단·서버 경고는 메인 몫이다. INA228 이 없으면 판정을 멈추고(상태 유지) 발행도 하지 않는다.
 *
 * 콘솔: pwr (현재 값·저전압·통계) | pwr reset (최소/최대 초기화)
 *       pwr sim <V> (가짜 전압으로 판정·발행 — INA228 없어도 됨) | pwr sim off
 */
#pragma once

#include <Arduino.h>
#include <KpPower.h>

namespace Pwr {

void begin();
void update();                       // loop 매 틱

// 최근 값이 유효하면 true. mV · mA(부호 있음 — 역류면 음수). pwr sim 중이면 가짜 전압.
bool get(int32_t *mv, int32_t *ma);
KpPower::Uv uv();
bool present();                      // INA228 이 응답한다(없으면 저전압 감시 불가 — LED 센서 고장)

bool tryConsole(const char *line);
void printStatus(Stream &io);

} // namespace Pwr
