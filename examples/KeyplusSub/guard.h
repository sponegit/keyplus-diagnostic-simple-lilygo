/**
 * @file      guard.h
 * @brief     온도 가드 · 센서 레일 게이트 — 서브 (작업서 S8 8단계, HARDWARE §5.1)
 *
 * SHT41(상시 레일)의 온도로 센서 레일을 켜고 끈다.
 *   50°C 초과 → 차단(HOT),  45°C 미만 → 복귀
 *   −5°C 미만 → 차단(COLD),  0°C 초과 → 복귀
 * 차단 대상: GPIO39 5V 먼지(PMS5003) · GPIO40 3.3V 센서(SCD41·SGP41·BMP280) · GPIO41 5V CO.
 * SHT41 은 상시 레일이라 차단 중에도 온·습도를 계속 발행한다(guard 값과 함께 — 나머지가 왜 비었는지의 근거).
 *
 * 부팅: 레일은 setup 에서 LOW(외부 풀다운과 같은 상태)로 시작하고, SHT41 첫 값을 보고 켠다.
 *   GUARD_BOOT_WAIT_MS 안에 SHT41 이 없으면 감시 없이 켠다 — 감시자 하나 고장으로 센서 전부를 잃지 않게.
 * 운전 중 SHT41 이 응답하지 않으면 마지막 상태를 유지한다.
 *
 * 레일을 끄고 켤 때 센서 쪽 처리(재탐색·SGP41 컨디셔닝·SCD41 첫 값 버림·PMS 30초 안정화)는
 * air.cpp·pms.cpp 가 sensorsOn() 을 보고 한다.
 *
 * ⚠️ 게이트 회로(MOSFET) 없는 벤치에서는 GPIO 만 바뀌고 전원은 그대로다. 그래서 차단 중에는 센서와
 *    통신하지 않고, PMS5003 은 UART 슬립 명령으로 팬을 세운다(벤치에서 눈으로 확인 가능).
 *    실제 가열 시험은 게이트 회로를 붙인 뒤에 한다 — 그 전에는 guard sim 으로 확인한다.
 *
 * 콘솔: guard | guard sim <°C> (가짜 온도로 판단) | guard sim off
 */
#pragma once

#include <Arduino.h>
#include <KpAir.h>

namespace Guard {

void begin();
// SHT41 측정 직후 부른다. valid=false 면 이번 값은 없음.
void update(uint32_t now, bool valid, float tempC);

bool          known();       // 첫 판단을 내렸나
bool          sensorsOn();   // 센서 레일이 켜져 있나
KpAir::Guard  state();

bool tryConsole(const char *line);
void printStatus(Stream &io);

} // namespace Guard
