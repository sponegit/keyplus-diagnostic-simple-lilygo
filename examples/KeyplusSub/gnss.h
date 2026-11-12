/**
 * @file      gnss.h
 * @brief     MAX-M10S GNSS — 서브 (작업서 S7 1·2단계, 위치 주기 3단계 전원 듀티)
 *
 * UART1(RX15 · TX16, GNSS_BAUD 8N1 — 벤치 모듈은 38400). 모듈 설정(NMEA 1초 주기)을 그대로 받아 TinyGPSPlus 로 해석한다.
 * 모듈에 아무 명령도 보내지 않는다 — 설정 변경(UBX)은 필요해질 때 한다.
 *
 * 전원: GPIO38 레일. 온도 가드 대상이 아니다(−40~85°C).
 *   3단계(2026-11-12): **메인 VEHICLE 프레임(KpVehicle)의 GNSS_ON 을 그대로 따른다** — 주행 중 켬, 주차 중에는
 *   메인이 다음 주차 telemetry 직전에만 켜 달라고 한다. 켜고 끌 시점은 메인이 정한다(KpVehicle.h 머리 주석).
 *   VEHICLE 을 받기 전·VEHICLE_STALE_MS 넘게 못 받으면 켜 둔다(옛 메인·링크 끊김에도 위치를 잃지 않게).
 *   ⚠️ 끄면 모듈 백업 전원(V_BCKP)이 없을 경우 다음 켤 때 콜드 스타트(수십 초)다 — 콘솔 gnss 의 "첫 fix" 로 본다.
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

// 메인 VEHICLE 프레임(link.cpp onFrame) — 전원 듀티의 근거. 적용은 update() 에서.
void onVehicle(const uint8_t *payload, size_t len);

bool tryConsole(const char *line);
void printStatus(Stream &io);

} // namespace Gnss
