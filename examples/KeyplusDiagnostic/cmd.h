/**
 * @file      cmd.h
 * @brief     MQTT cmd 다운링크 — 서버 명령 수신 → 실행(차키) → ack 발행
 *
 * 계약(@keyplus-diagnostic/shared command.ts):
 *   수신 v1/{device_id}/cmd       {command_id, type, payload?, expires_at(epoch)}
 *   발행 v1/{device_id}/cmd/ack   {command_id, result, vehicle_state?, ts(epoch)}   (QoS1)
 *   type: door_lock/door_unlock(→Carkey) / remote_start(미지원→failed) / config_update / ota_start
 *   result: received(비종결) / done / failed / rejected_ignition_on / expired_on_device
 *
 * 수신 경로: 래퍼 mqtt_set_callback(콜백) + mqtt_handle(loop 매틱 펌핑, Mqtt::handle).
 *   ⚠️ 콜백은 mqtt_handle 내부(AT 파싱 중)에서 불리므로 큐에 복사만 하고,
 *      실제 실행/ack 발행은 loop의 Cmd::handle()에서(AT 재진입 회피).
 *   ⚠️ 래퍼 clean_session=1 하드코딩 → 재접속마다 구독이 사라짐 → 접속 성공 시마다 subscribe().
 *      (오프라인 큐잉 없음. 온라인 배달만. 서버 스위퍼/만료가 보완)
 *
 * 수신 큐(CMD_RX_QUEUE_N): 서버 flushPendingForDevice() 가 pending 을 순회하며 연속
 * 발행하므로 명령이 loop 주기(50ms)보다 촘촘히 도착할 수 있다. 1칸 버퍼였을 때는 뒤엣것이
 * 로그도 ack 도 없이 사라졌다 — 큐가 가득 찼을 때만 드롭하고, 그때는 반드시 로그를 남긴다.
 *
 * 발행 페이싱: 처리 1건마다 ack 를 내므로 큐를 연속으로 비우면 그게 곧 연속 publish 다
 * (모뎀은 겹치면 세션이 깨진다). handle() 은 틱당 1건만, 그리고 직전 발행으로부터
 * PUBLISH_MIN_GAP_MS 가 지난 뒤에만 처리한다. ota_start 는 received ack 를 먼저 내고
 * 실행을 CMD_DEFER_EXEC_MS 뒤로 미룬다.
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

// 새 세션 진입/모뎀 리셋 시 호출 — 구독 플래그를 내려 재구독이 일어나게 한다.
// clean_session=1 이라 세션이 바뀌면 브로커 쪽 구독은 사라진다.
void markUnsubscribed();

// --- 진단 콘솔용 상태 조회 ---------------------------------------------------
// 구독 토픽(미구독이면 빈 문자열), 마지막 subscribe 성공 여부, 미처리 수신 유무.
// clean_session=1이라 재접속마다 재구독하므로, "접속됐는데 명령이 안 온다"를 볼 때 쓴다.
// droppedRxCount()는 큐 오버플로로 버린 누적 건수 — 0이 아니면 명령이 실제로 유실됐다.
String   topic();
bool     isSubscribed();
bool     hasPendingRx();
int      pendingRxCount();
uint32_t droppedRxCount();

// cmd/ack 1건 발행 (QoS1). ts는 내부에서 모뎀 시각(epoch)으로 채운다.
// handle() 내부 + OTA 재부팅 후 지연 ack(Ota::flushPendingAck)에서 공용으로 쓴다.
void sendAck(TinyGsm &modem, Stream &log, const String &cmdId, const char *result);

} // namespace Cmd
