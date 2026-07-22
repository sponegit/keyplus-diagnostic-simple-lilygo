/**
 * @file      cmd.h
 * @brief     MQTT cmd 다운링크 — 서버 명령 수신 → 실행(차키) → ack 발행
 *
 * 계약(@keyplus-diagnostic/shared command.ts):
 *   수신 v1/{device_id}/cmd       {command_id, type, payload?, expires_at(epoch)}
 *   발행 v1/{device_id}/cmd/ack   {command_id, result, vehicle_state?, ts(epoch)}   (QoS1)
 *   type: door_lock/door_unlock(→Carkey) / remote_start/config_update/ota_start(미구현→failed)
 *   result: done / failed / rejected_ignition_on / expired_on_device
 *
 * 수신 경로: 래퍼 mqtt_set_callback(콜백) + mqtt_handle(loop 매틱 펌핑, Mqtt::handle).
 *   ⚠️ 콜백은 mqtt_handle 내부(AT 파싱 중)에서 불리므로 복사+플래그만 하고,
 *      실제 실행/ack 발행은 loop의 Cmd::handle()에서(AT 재진입 회피).
 *   ⚠️ 래퍼 clean_session=1 하드코딩 → 재접속마다 구독이 사라짐 → 접속 성공 시마다 subscribe().
 *      (오프라인 큐잉 없음. 온라인 배달만. 서버 스위퍼/만료가 보완)
 */
#pragma once

#include "utilities.h"
#include <TinyGsmClient.h>

namespace Cmd {

// MQTT RX 콜백 등록 (부팅당 1회, mqtt 서비스 시작 후).
void begin(TinyGsm &modem);

// v1/{device_id}/cmd 구독 (접속 성공 시마다 호출 — clean_session=1이라 재구독 필요).
void subscribe(TinyGsm &modem, Stream &log);

// loop 매틱: 수신된 명령이 있으면 파싱→실행→ack 발행 (논블로킹 진입, 실행은 짧게 블로킹).
void handle(TinyGsm &modem, Stream &log);

} // namespace Cmd
