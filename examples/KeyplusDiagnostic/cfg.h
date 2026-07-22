/**
 * @file      cfg.h
 * @brief     8단계 — config_update 런타임 설정 (NVS "cfg" 지속 + 원격 반영)
 *
 * 서버 config_update 명령(payload의 알려진 키)을 NVS에 저장하고 런타임에 반영한다.
 * 부팅 시 NVS를 로드하되 없으면 config.h 기본값을 쓴다. 알 수 없는 키는 무시(전방호환).
 *
 * 지원 키(초기):
 *   telemetry_interval_ms → 즉시 적용(telemetryIntervalMs() 갱신)
 *   keepalive_s           → 다음 MQTT 접속에 반영(keepaliveS())
 * 확장은 키/게터 추가만.
 *
 * 계약: @keyplus-diagnostic/shared command.ts (type=config_update)
 * 설계: phase8-firmware-ota-lte.md §4
 */
#pragma once

#include <Arduino.h>

namespace Cfg {

// NVS(ns "cfg") 로드. 값이 없으면 config.h 기본값으로 초기화. setup에서 1회 호출.
void begin();

// telemetry 발행 주기(ms) — 즉시 적용값. loop가 매 발행 스케줄링 시 참조.
uint32_t telemetryIntervalMs();

// MQTT keepalive(초) — 다음 접속에 반영. mqtt connect가 참조.
int keepaliveS();

// config_update payload 반영. jsonBody에서 알려진 키만 파싱→검증→NVS 저장·런타임 갱신.
// 반환: 반영된 키 수(0이면 호출측이 failed ack).
int applyUpdate(const char *jsonBody, Stream &log);

} // namespace Cfg
