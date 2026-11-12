/**
 * @file      guard.h
 * @brief     온도 가드 · 센서 레일 게이트 — 서브 (작업서 S8 8단계, HARDWARE §5.1)
 *
 * ⚠️ 현재 GUARD_TEMP_ENABLE=0(config.h) — 온도 차단은 꺼져 있다. 레일은 부팅 직후 켜고 저전압 때만 끈다.
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
 * 저전압 경고·소프트 차단(pwr.cpp, 작업서 S6) 중에도 레일을 끈다 — 온도 판단과 별개의 사유로 겹쳐 둔다.
 *
 * 주차 공기질 듀티(2026-11-12): 메인 VEHICLE 의 AIR_IDLE 이면 PMS5003 5V 레일(GPIO39)만 끈다.
 *   ZE15-CO 5V 는 켜 둔다(작업서 S8 "주차 중에도 ON" — 전기화학 셀 예열 반복을 피하고 주차 중 CO 를 놓치지 않게).
 *   3.3V 레일(SCD41·SGP41·BMP280)도 켜 둔다 — SGP41 지수 학습이 1초 연속 표본을 전제하고, SCD41 은
 *   air.cpp 가 주기 측정만 멈춘다. VEHICLE 을 받기 전·VEHICLE_STALE_MS 넘게 못 받으면 쉬지 않는다(안전 쪽).
 *   깨울 시점은 메인이 정한다(KpVehicle.h) — 메인이 다음 주차 telemetry AIR_WAKE_LEAD_MS 전에 AIR_IDLE 을 내린다.
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

// 저전압 경고·차단 중이면 true — 온도가 정상이어도 레일을 끈다.
void          setPowerBlock(bool block);
bool          powerBlocked();

bool          known();       // 첫 판단을 내렸나
bool          noSensor();    // 부팅 때 SHT41 이 없어 감시 없이 켰다(LED 센서 고장)
bool          sensorsOn();   // 3.3V 센서 레일(SCD41·SGP41·BMP280)이 켜져 있나
bool          airAwake();    // PMS5003 레일이 켜져 있고 SCD41 을 돌려도 되나 — 레일 ON + 듀티 깨어 있음
bool          dutyIdle();    // 메인 AIR_IDLE 로 쉬는 중

// 메인 VEHICLE 프레임(link.cpp onFrame) — 주차 공기질 듀티의 근거. 적용은 update() 에서.
void          onVehicle(const uint8_t *payload, size_t len);
KpAir::Guard  state();

bool tryConsole(const char *line);
void printStatus(Stream &io);

} // namespace Guard
