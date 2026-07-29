/**
 * @file      provisioning.h
 * @brief     단말 신원(device_id/pw) 프로비저닝 — NVS 저장 + 자동 HTTP 발급 + 콘솔 입력
 *
 * device_id/mqtt_pw를 컴파일 하드코딩 대신 NVS에 저장한다. 입력 소스는 3가지:
 *   1) 자동 프로비저닝: 부팅 시 크리덴셜이 없으면 서버 /internal/provision 에서 발급(주 경로)
 *   2) 시리얼 콘솔 setid/setpw: 비상 오버라이드
 * 저장 구조(NVS namespace "prov")는 동일하므로 소스만 바뀐다.
 *
 * Debug Console(시리얼 모니터) 명령 — 이 모듈이 시리얼 한 줄 읽기를 독점하고,
 * 자기 명령이 아니면 등록된 핸들러(Log / 앱 / Carkey)로 차례로 위임한다.
 *   setid vt-2607-0001-x7q   device_id 설정(형식 검증 후 NVS 저장) — 적용은 재부팅
 *   setpw <password>         MQTT 비밀번호 설정(실 브로커용)
 *   showid                   현재 값 출력
 *   clearid                  NVS 초기화 → 재부팅 시 자동 재발급(프로비저닝)
 *   help                     명령 목록
 *   log [레벨]               UART 로그 상세도 (Log::tryConsole)
 *   info / status [영역]     단말 정보 / 상태 조회 (앱 핸들러 — .ino)
 *   lock / unlock [ms]       차키 검증 (Carkey::tryConsole)
 */
#pragma once

// utilities.h가 TINY_GSM_MODEM_A7670를 정의하므로 TinyGsmClient.h보다 먼저 포함.
#include <Arduino.h>
#include "utilities.h"
#include <TinyGsmClient.h>

namespace Prov {

// NVS에서 device_id/password 로드. (PROVISION_SEED_FALLBACK=1이면 비었을 때 DEVICE_ID 시드)
void begin();

// 현재 device_id가 형식(vt-YYMM-NNNN-XXX)에 맞는가.
bool hasValidId();

// device_id(형식 유효) + mqtt_pw(비어있지 않음) 둘 다 확보됐는가 = 프로비저닝 완료 여부.
bool hasCredentials();

// 현재 device_id / MQTT 비밀번호(없으면 빈 문자열).
const String &deviceId();
const String &mqttPassword();

// 자동 프로비저닝 결과.
enum class ProvResult { OK, REJECTED, NETWORK_ERROR };

// /internal/provision 로 {imei,mac,fw} POST → device_id/mqtt_password 발급받아 NVS 저장.
// LTE 데이터패스가 up 상태에서 호출할 것.
//   200/201 → OK(저장)  | 403 → REJECTED(allowlist 문제, 재시도 금지) | 그 외/네트워크 → NETWORK_ERROR
ProvResult provisionOverHttp(TinyGsm &modem, Stream &log);

// 시리얼 입력 처리 (loop 매 틱 호출, 논블로킹).
void handleSerial(Stream &io);

// 명령 안내 출력.
void printHelp(Stream &io);

// --- 앱 명령 위임 -----------------------------------------------------------
// 이 모듈이 시리얼 한 줄 읽기를 독점하므로, 모뎀/GPS/OBD 같은 상위 상태를 다루는
// 명령(info/status)은 앱(.ino)이 핸들러로 등록해 처리한다. Prov가 그 의존성을
// 떠안지 않기 위한 것 — Carkey/Log처럼 tryConsole을 직접 부르면 순환 의존이 된다.
//   반환 true = 그 명령을 처리했음(이후 핸들러로 넘기지 않음).
typedef bool (*ConsoleHandler)(const String &cmd, const String &arg, Stream &io);
void setConsoleHandler(ConsoleHandler fn);

// 앱이 등록한 도움말 출력 훅(printHelp에서 이어서 호출). 미등록이면 생략.
typedef void (*HelpHandler)(Stream &io);
void setHelpHandler(HelpHandler fn);

} // namespace Prov
