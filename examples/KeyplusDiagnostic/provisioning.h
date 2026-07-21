/**
 * @file      provisioning.h
 * @brief     단말 신원(device_id) 프로비저닝 — NVS 저장 + Debug Console 입력
 *
 * device_id를 컴파일 하드코딩 대신 NVS에 저장한다. 입력 소스는 현재 "시리얼 콘솔"이며,
 * 서버 자동 프로비저닝(설계 01 §4, /internal/provision)이 서면 그 응답으로 대체된다.
 * 저장 구조(NVS)는 동일하므로 소스만 바뀐다.
 *
 * Debug Console(시리얼 모니터)에서 명령 입력:
 *   setid vt-2607-0001-x7q   device_id 설정(형식 검증 후 NVS 저장) — 적용은 재부팅
 *   setpw <password>         MQTT 비밀번호 설정(실 브로커용)
 *   showid                   현재 값 출력
 *   clearid                  NVS 초기화 → 컴파일 기본값(DEVICE_ID)으로 복귀
 *   help                     명령 목록
 */
#pragma once
#include <Arduino.h>

namespace Prov {

// NVS에서 device_id/password 로드. 없으면 컴파일 기본값(config.h DEVICE_ID)으로 시드.
void begin();

// 현재 device_id가 형식(vt-YYMM-NNNN-XXX)에 맞는가.
bool hasValidId();

// 현재 device_id / MQTT 비밀번호(없으면 빈 문자열).
const String &deviceId();
const String &mqttPassword();

// 시리얼 입력 처리 (loop 매 틱 호출, 논블로킹). 위 명령들을 파싱한다.
void handleSerial(Stream &io);

// 명령 안내 출력.
void printHelp(Stream &io);

} // namespace Prov
