/**
 * @file      gnss.h
 * @brief     MAX-M10S GNSS — 서브 (작업서 S7 1·2단계)
 *
 * UART1(RX15 · TX16, GNSS_BAUD 8N1 — 벤치 모듈은 38400). 모듈 설정(NMEA 1초 주기)을 그대로 받아 TinyGPSPlus 로 해석한다.
 * 모듈에 아무 명령도 보내지 않는다 — 설정 변경(UBX)은 필요해질 때 한다.
 *
 * 전원: GPIO38 레일을 켠 채로 둔다. 온도 가드 대상이 아니다(−40~85°C).
 *   주차 듀티(5분 주기·측위 후 2초 유지·90초 타임아웃)는 메인 VEHICLE(§3.5)이 생긴 뒤 2단계에서 한다.
 * fix 판정: 위치가 유효하고 마지막 갱신이 GNSS_FIX_STALE_MS 안일 때. TinyGPSPlus 의 isValid() 는
 *   한 번 잡으면 계속 true 라 나이를 함께 본다.
 *
 * 2단계: GNSS 프레임(KpGnss, GNSS_PUB_MS 주기 + fix 전이 시 즉시) → 메인 g_lastFix · 시각 소스(Clk SRC_GPS).
 *   링크가 내려가 있으면 버린다 — 위치는 최신값만 의미가 있다(POWER·AIR 와 같다).
 *
 * 콘솔: gnss (fix·위성·통계) | gnss raw (NMEA 원문 출력, 토글) | gnss scan (속도 찾기 — 약 10초 막힘)
 */
#pragma once

#include <Arduino.h>

namespace Gnss {

void begin();
void update();                       // loop 매 틱 — 쌓인 바이트를 해석

bool fix();                          // 지금 유효한 위치가 있나

bool tryConsole(const char *line);
void printStatus(Stream &io);

} // namespace Gnss
