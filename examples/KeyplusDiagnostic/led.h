/**
 * @file      led.h
 * @brief     상태표시 LED — 단일 외장 LED로 프로비저닝/통신 상태 표시(비블로킹)
 *
 * 하드웨어: bare T-A7670E는 SW 제어 온보드 LED가 없어 외장 LED를 GPIO23에 배선한다
 *   (config.h PIN_STATUS_LED/LED_ON_LEVEL). 3mm 빨강 + 1kΩ, active-high(HIGH=점등).
 *
 * 패턴(설계: phase3-firmware-provisioning-led.md §4, 확정본):
 *   BOOT          부팅/모뎀 기동       solid ON
 *   PROVISIONING  접속/발급 시도 중     느린 점멸(0.5s)
 *   UNPROVISIONED device_id/pw 없음     1회 점멸 버스트 반복
 *   REJECTED      인증/발급 거절(403)   2회 점멸 버스트 반복
 *   COMM_ERROR    LTE/브로커 통신오류   3회 점멸 버스트 반복
 *   MQTT_OK       정상 접속             heartbeat(3s마다 짧게)
 *
 * "3구분" 요건(미프로비저닝 vs 거절 vs 통신오류)은 버스트 횟수(1/2/3)로 육안 구분한다.
 * 사용: setup에서 begin() 후 상태 전이 지점에서 set()만 호출하면 된다. begin()이 Ticker로
 *   패턴을 자동 구동하므로 loop 블로킹(AT 명령 수초) 중에도 LED가 멈추지 않는다.
 *   (update()는 호환용 수동 렌더 — 필수 아님)
 */
#pragma once
#include <Arduino.h>

namespace Led {

enum class State {
    BOOT,           // 부팅/초기화 — solid ON
    UNPROVISIONED,  // 신원 없음 — 1회 버스트
    PROVISIONING,   // 접속/발급 시도 중 — 느린 점멸
    REJECTED,       // 거절(403)/인증 실패 — 2회 버스트
    COMM_ERROR,     // 통신 오류 — 3회 버스트
    MQTT_OK,        // 정상 접속 — heartbeat
};

void  begin();      // pinMode + BOOT 상태 진입(solid ON) + Ticker로 패턴 자동 구동 시작
void  set(State s); // 상태 전환(비블로킹, 동일 상태는 무시해 패턴 위상 유지)
State get();        // 현재 상태
void  update();     // (호환용) 수동 렌더 1회 — Ticker가 이미 구동하므로 호출 안 해도 됨

} // namespace Led
