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
bool begin(TinyGsm &modem, Stream &log);

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
