/**
 * @file      mqtt.h
 * @brief     1단계 (증분 B) — A7670E 내장 MQTT/TLS + telemetry 발행 모듈
 *
 * 모뎀 내장 MQTT(AT+CMQTT*)를 포크 TinyGSM 래퍼로 사용. 서버 CA 검증(authMethod=1)
 * 으로 접속하고, status online(retained)+LWT를 등록한 뒤 telemetry를 주기 발행한다.
 *
 * 계약(SSOT): @keyplus-diagnostic/shared packages/shared/src/protocol
 *   토픽  v1/{device_id}/telemetry | status
 *   페이로드 telemetry {ts,seq,gps,net,sys,meta}
 * 설계: lte-mqtt-device-design.md §3~5
 *
 * ⚠️ 검증 단계: config.h MQTT_USE_TEST_BROKER=1 → test.mosquitto.org(익명 TLS).
 *    cmd 구독은 다음 증분(래퍼가 clean_session=1 하드코딩 → persistent 대응 필요).
 */
#pragma once

#include "utilities.h"
#include <TinyGsmClient.h>
#include "gps.h"    // GpsFix
#include "obd2.h"   // Obd2::Data

namespace Mqtt {

// CA 로드 → TLS 접속(서버검증) → status online(retained) + LWT 등록.
// 성공 시 true. 실패 사유는 log로 출력.
bool begin(TinyGsm &modem, Stream &log);

// 브로커 연결 유지 여부.
bool isConnected(TinyGsm &modem);

// 콜백/keepalive 펌핑. loop 매 틱 호출.
void handle(TinyGsm &modem);

// 끊겨 있으면 재접속(begin 재실행). 백오프는 호출측(loop)에서 관리.
bool ensure(TinyGsm &modem, Stream &log);

// telemetry 1회 발행 (QoS 1). withMeta=true면 최초 발행용 하드웨어 메타(imei/mac/fw) 포함.
// fix가 유효하면 gps 좌표/시각을 싣고, ts는 GPS UTC에서 epoch 계산(무효 시 0).
// obd.valid면 지원 PID를 obd 오브젝트로 동봉(6단계).
bool publishTelemetry(TinyGsm &modem, const GpsFix &fix, const Obd2::Data &obd,
                      uint32_t seq, bool withMeta, Stream &log);

// MQTT 서비스(CMQTT) 완전 종료 — 모뎀 SSL 컨텍스트를 해제해 HTTP(S) 서비스와의 충돌을 막는다.
// OTA 다운로드 진입 직전에 호출(어차피 OTA 성공 시 재부팅되므로 세션은 버린다).
void stopService(TinyGsm &modem);

} // namespace Mqtt
