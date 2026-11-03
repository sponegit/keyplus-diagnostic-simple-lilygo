/**
 * @file      air.h
 * @brief     실내 공기질 센서 — 서브 (작업서 S8, 벤치 단계별 도입)
 *
 * 온보드 Qwiic(I2C 13·14)에 센서를 하나씩 붙여 가며 확인한다. 지금 들어간 것:
 *   1단계  SHT41 온·습도 (0x44) — 1초 주기, 고정밀 측정(0xFD)
 *   2단계  BMP280 기압 (0x76, 없으면 0x77) — normal 모드 연속 측정, 1초마다 결과만 읽는다.
 *          SCD41 기압 보정(3단계)에 쓴다. 없으면 AIR_PROBE_MS 마다 다시 찾는다(벤치에서 나중에 꽂아도 된다).
 *
 * 측정은 논블로킹이다: 명령을 쓰고 변환 시간(10ms)이 지난 다음 틱에 읽는다.
 * 센서가 응답하지 않으면 그 값만 "없음"으로 두고 나머지 동작(링크·차키)은 그대로 간다.
 *
 * ⚠️ I2C 는 loop 태스크에서만 쓴다. Wire 타임아웃을 짧게 걸어(I2C_TIMEOUT_MS) 버스가 붙잡혀도
 *    loop 워치독(5초)까지 가지 않게 한다.
 *
 * 콘솔: i2c (버스 스캔) | air (현재 값) | air watch (새 값마다 출력, 다시 치면 끔)
 */
#pragma once

#include <Arduino.h>

namespace Air {

void begin();
void update();                        // loop 매 틱

// 콘솔 훅: i2c / air 이면 처리하고 true.
bool tryConsole(const char *line);
void printStatus(Stream &io);

} // namespace Air
