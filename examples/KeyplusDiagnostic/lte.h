/**
 * @file      lte.h
 * @brief     1단계 (증분 A) — A7670E LTE 브링업 모듈
 *
 * 유심(LG U+) 데이터패스 검증까지 담당한다: APN 설정 → 망 등록 → PDP(gprs)
 * 컨텍스트 → 평문 HTTP GET 1회로 인터넷 도달 확인. 서버(EMQX)와 무관하게
 * "이 유심으로 인터넷이 되는가"를 검증하는 단계.
 *
 * MQTT/TLS 접속은 증분 B에서 별도 모듈(mqtt.h)로 얹는다.
 * 설계: keyplus-docs/.../260706-simple-obd2-diagnostic/lte-mqtt-device-design.md
 */
#pragma once

// utilities.h가 TINY_GSM_MODEM_A7670를 정의하므로 TinyGsmClient.h보다 먼저 포함.
#include "utilities.h"
#include <TinyGsmClient.h>
#include "config.h"   // LTE_AT_PROBE_MS — modemAlive 기본 인자

// 현재 망 상태 스냅샷 (디버그/보고용).
struct LteStatus {
    bool    registered;     // 홈/로밍 등록 완료
    bool    dataUp;         // PDP(gprs) 컨텍스트 활성
    int     rssi;           // getSignalQuality() 원값 (0~31, 99=미상)
    String  oper;           // 통신사명
    String  ip;             // 할당된 로컬 IP
};

namespace Lte {

// 망 등록 + PDP 컨텍스트까지 올린다. APN은 config.h(LTE_APN) 사용.
// LTE_REG_TIMEOUT_MS 내 등록 실패 또는 등록 거부(REG_DENIED) 시 false.
// 등록은 필드 실패 1순위(APN/신호)라 진행상황을 log에 출력한다.
// ⚠️ 진입 시 modemAlive()로 모뎀 응답을 먼저 확인한다 — 무응답이면 등록 폴링
//    (최대 LTE_REG_TIMEOUT_MS)이 통째로 낭비이므로 즉시 false를 반환한다.
bool begin(TinyGsm &modem, Stream &log);

// 모뎀이 AT에 응답하는가. timeoutMs 안에 응답 없으면 false.
// 전원 부족으로 모뎀이 내부 리셋/불능이 된 상태를 빠르게 가려낸다
// (그 경우 CSQ는 99, 등록 상태는 no-result로 나온다).
// 기본값 LTE_AT_PROBE_MS(2초)는 "리셋을 걸기 전 최종 확인"용이라 넉넉하다.
// 경로를 가르기만 하는 자리(폴 중 확인, 재브링업 게이트)에서는 MODEM_PROBE_TIMEOUT_MS
// 같은 짧은 값을 넘겨 죽은 모뎀에 초 단위를 태우지 않는다.
bool modemAlive(TinyGsm &modem, uint32_t timeoutMs = LTE_AT_PROBE_MS);

// 모뎀 PMU 다이 온도(°C, AT+CPMUTEMP). 실패/미지원이면 0.
// ⚠️ 주변 온도가 아니라 모뎀 내부 온도다. 열 셧다운 가설을 가리는 계측용
//    (A7670 계열은 대략 80°C 대에서 보호 동작에 들어간다).
// ⚠️ AT 왕복이므로 모뎀이 죽은 뒤엔 못 읽는다 — 주기적으로 표본을 남겨
//    "사망 직전 값"을 확보하는 방식으로만 쓸 수 있다.
int modemTempC(TinyGsm &modem);

// 접속 기술을 config.h LTE_CNMP_MODE(기본 38 = LTE 전용)로 고정한다(AT+CNMP).
// GSM 폴백은 TX 가 2A 급 버스트라 전원 마진이 얇은 보드에서 브라운아웃을 부른다.
// 이미 그 값이면 쓰지 않는다 — CNMP 쓰기는 망 재탐색을 유발해 등록을 늦춘다.
// begin() 이 등록 폴링 직전에 호출한다. 실패해도 브링업은 계속된다(best-effort).
bool lockLteOnly(TinyGsm &modem, Stream &log);

// 모뎀 소프트 리셋(AT+CFUN=1,1) 후 AT 재개까지 대기. 성공 시 true.
// ⚠️ 최대 LTE_MODEM_RESET_WAIT_MS 동안 블로킹한다 — 복구 경로 전용.
//    이걸로도 안 살아나면 호출측이 PWRKEY 하드 전원 사이클로 승격한다.
bool softReset(TinyGsm &modem, Stream &log);

// 망+데이터 모두 살아있는지. 재접속 판단용.
bool isUp(TinyGsm &modem);

// 현재 상태 스냅샷 채우기.
void status(TinyGsm &modem, LteStatus &out);

// 상태를 사람이 읽는 형태로 출력 (디버그용).
void printStatus(const LteStatus &s, Stream &log);

// 데이터패스 검증: LTE_TEST_HOST로 평문 HTTP GET 1회.
// 반환: HTTP 상태코드(200 등). 접속 실패 등 음수는 오류.
int httpGetCheck(TinyGsm &modem, Stream &log);

// PDP 컨텍스트 해제 (저전력 진입/정리 시).
void end(TinyGsm &modem);

} // namespace Lte
