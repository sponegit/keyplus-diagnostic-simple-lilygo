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
#include "gps.h"      // GpsFix
#include "obd2.h"     // Obd2::Data
#include "buffer.h"   // Buf::Record (백필)
#include "fast.h"     // Fast::Sample (고빈도 창)

namespace Mqtt {

// CA 로드 → TLS 접속(서버검증) → status online(retained) + LWT 등록.
// 성공 시 true. 실패 사유는 log로 출력.
bool begin(TinyGsm &modem, Stream &log);

// 브로커 연결 유지 여부.
bool isConnected(TinyGsm &modem);

// 마지막 telemetry 발행 시점에 읽은 망 상태. loop의 [STAT] 한 줄이 재사용해
// 상태 출력 때문에 AT 왕복이 늘지 않게 한다(발행 전이면 0).
int lastRssi();
int lastReg();

// 진단 콘솔용 — 접속 시점에 조립되므로 접속 전이면 빈 문자열.
// serviceStarted()는 CMQTTSTART(부팅당 1회) 완료 여부로, 세션 접속 여부(isConnected)와 다르다.
String clientId();
String topicTelemetry();
bool   serviceStarted();

// 모뎀 리셋 직후 호출 — 펌웨어가 들고 있던 서비스/접속 플래그를 모뎀 상태에 맞춘다.
// 이걸 빠뜨리면 다음 begin()이 CMQTTSTART를 건너뛰어 접속이 영영 실패한다.
void resetServiceState();

// 콜백/keepalive 펌핑. loop 매 틱 호출.
void handle(TinyGsm &modem);

// 끊겨 있으면 재접속(begin 재실행). 백오프는 호출측(loop)에서 관리.
bool ensure(TinyGsm &modem, Stream &log);

// --- 발행 간 간격 가드 -------------------------------------------------------
// 모뎀은 publish 를 1건씩만 처리하고 겹치면 세션이 깨진다(실측). 래퍼 mqtt_publish()
// 는 AT+CMQTTPUB 의 OK 까지만 기다리고 결과 URC 는 비동기로 늦게 오므로, "반환됨"을
// "발행 끝"으로 볼 수 없다 — 호출측이 직접 간격을 벌려야 한다.
//
// 이 모듈의 publish*() 는 발행 직후 스스로 notePublish() 한다. cmd ack 처럼 모듈 밖에서
// modem.mqtt_publish() 를 직접 부르는 경로는 **발행 직후 notePublish() 를 불러야** 이
// 기준이 맞는다. 빠뜨리면 가드가 그 발행을 못 보고 곧바로 다음 발행을 통과시킨다.
void     notePublish();
uint32_t lastPublishAt();
bool     publishGapElapsed(uint32_t now, uint32_t gapMs);

// --- 발행 진입 생존 게이트 (R6) ---------------------------------------------
// 죽은 모뎀에 mqtt_publish 를 던지면 래퍼가 CMQTTTOPIC·CMQTTPAYLOAD 의 '>' 프롬프트를
// 각각 10초 기다린 뒤에야 실패한다 — 발행 1건에 20초 넘게 loop 가 멈춘다.
// 짧은 AT 프로브로 미리 끊고, 무응답이면 세션을 사망 처리(다음 틱 재접속)한 뒤 false.
// 이 모듈의 publish*() 는 스스로 부른다. cmd ack 처럼 발행 체인 밖 경로는 직접 부를 것.
//   what: 로그에 찍을 발행 종류("telemetry" / "ack" 등).
bool publishReady(TinyGsm &modem, const char *what, Stream &log);

// --- 발행 완료 확인 (P3) ----------------------------------------------------
// 래퍼 mqtt_publish() 의 true 는 "모뎀이 요청을 받아들였다"까지다. 실제 전달 결과는
// 비동기 URC `+CMQTTPUB: <idx>,<err>` 로 오고, 그걸 파싱해 두는 건 패치한 래퍼다
// (lib/TinyGSM TinyGsmMqttA76xx.h / TinyGsmClientA7670.h).
//
// 발행을 내보낸 뒤 MQTT_PUB_ACK_TIMEOUT_MS 안에 결과가 오지 않았는가.
// 참이면 세션이 죽은 것으로 본다 — 모뎀이 URC 조차 못 낼 상태라는 뜻이다.
// ⚠️ Mqtt::handle() 이 URC 를 펌핑한 **뒤에** 물어야 한다. 안 그러면 도착한 결과를
//    아직 읽지 않은 채로 "미도착"이라 판정한다.
bool pubAckOverdue(TinyGsm &modem, uint32_t now);
// 내보낸 발행의 결과 URC 를 아직 기다리는 중인가. 거짓이면 모뎀이 그 발행을 끝냈다는 뜻.
// 접속 직후 구독 시점을 "추측 대기" 대신 이 신호로 잡는 데 쓴다.
bool pubAckPending(TinyGsm &modem);
// 마지막 발행 결과 코드. 0=성공, -1=아직 결과 없음, 그 외=모뎀 에러.
int  lastPubErr(TinyGsm &modem);

// --- CMQTT 서비스 시작 연속 실패 --------------------------------------------
// begin() 안의 mqtt_begin(CMQTTSTART)이 연속 몇 번 실패했는가. 성공하면 0 이 된다.
// 이게 쌓인다는 건 펌웨어와 모뎀의 상태가 어긋났다는 뜻이다 — 펌웨어는 "서비스 꺼짐",
// 모뎀은 "켜짐". resetServiceState() 가 AT 를 보내지 않는 설계라 모뎀이 멀쩡한 채로
// 그 경로를 타면 어긋난 채 남고, 이후 CMQTTSTART 는 계속 실패한다. LTE(PDP)는 정상이라
// 재브링업·모뎀 리셋 경로도 안 타므로 **스스로는 빠져나오지 못한다**.
//   → 호출측(loop)이 임계에 닿으면 모뎀 리셋으로 상태를 강제 재동기한다.
int  serviceStartFails();
void clearServiceStartFails();

// --- 세션 접속 연속 실패 (CMQTTCONNECT) --------------------------------------
// serviceStartFails() 와 **다른 축**이다. 저건 "서비스가 안 뜬다"(CMQTTSTART)이고,
// 이건 "서비스는 떴는데 브로커에 못 붙는다"이다. 종전에는 후자를 세는 곳이 없어서
// 모뎀도 살아 있고 Lte::isUp() 도 참인데 CMQTTCONNECT 만 계속 실패하는 상태에 빠지면
// 승격 트리거가 하나도 없었다 — 백오프가 15초에서 포화된 채 재부팅 전까지 무한 반복
// (실측 260810: mqtt=OFF 25분+, 미전송 백로그 1500건, 사용자가 직접 재부팅해 회복).
//   → 호출측(loop)이 임계에 닿으면 ① 데이터패스 실검증 ② CMQTT 서비스 재시작
//     ③ 모뎀 리셋 순으로 승격한다. 접속에 성공하면 0 으로 돌아간다.
int  connectFails();
void clearConnectFails();
// 마지막 접속 시도가 어디서 막혔는가. 0=성공, 음수=단계 식별자(래퍼 MQTT_CONN_ERR_*),
// 양수=모뎀이 +CMQTTCONNECT 로 준 err 코드.
int  lastConnErr(TinyGsm &modem);

/**
 * status 재발행 (F1/F3) — `connectSession()` 의 online 발행과 **같은 토픽·QoS1·retain=1**.
 *   {"online":true[,"sub":true],"power_mode":"...","ignition_on":...}
 *
 * retain=1 을 유지하는 이유: 접속 시 online 이 retained 로 올라가 있어, 이 발행이
 * retain=0 이면 브로커의 retained 값이 `power_mode` 없는 낡은 상태로 남는다.
 *
 * sub=true 면 "구독 완료" 통지를 겸한다 — 서버가 pending flush 를 7초 추측 대기 없이
 * 즉시 돌린다(구식 펌웨어는 이 키를 안 보내므로 서버는 하위호환 경로를 탄다).
 *
 * ⚠️ 호출측이 publishGapElapsed() 로 직전 발행과의 간격을 확보한 뒤 부를 것.
 */
bool publishStatus(TinyGsm &modem, const char *powerMode, bool ignitionOn,
                   bool sub, Stream &log);

// telemetry 1회 발행 (QoS 1). withMeta=true면 최초 발행용 하드웨어 메타(imei/mac/fw) 포함.
// fix가 유효하면 gps 좌표를 싣고, obd.valid면 지원 PID를 obd 오브젝트로 동봉(6단계).
//
// ⚠️ fixFresh = "지금 이 순간 측위 중인가"(loop의 g_gpsFixNow). fix 인자는 측위를 놓쳐도
//    마지막 값을 유지하는 캐시라(g_lastFix) fix.valid 로는 신선도를 알 수 없다. ts 를
//    캐시 좌표의 UTC 로 계산하면 미측위 구간 내내 같은 ts 가 나가고, 서버 PK
//    (device_id, ts) 중복제거로 그 구간 telemetry 가 통째로 사라진다.
//
// vbatMv — 전원 계측(P2)의 마지막 표본(g_lastVbatMv). **0 이면 sys 에서 통째로 생략**한다.
//   0/-1 을 보내면 서버가 "전압 0V"를 관측값으로 오인해 방전 그래프가 바닥을 찍는다
//   (agg 를 "샘플 없으면 통째 생략"하는 것과 같은 규약).
//   지금껏 이 값은 [PWR] 로그로만 나가 UART 를 붙여야 볼 수 있었다 — 서버는 배터리를
//   추가하고도 전압을 하나도 못 봤다(260805 점검: vehicle_state.voltage 전부 NULL).
bool publishTelemetry(TinyGsm &modem, const GpsFix &fix, bool fixFresh, const Obd2::Data &obd,
                      uint32_t seq, bool withMeta, int vbatMv, Stream &log);

/**
 * 백필 1건 발행 — **기존 telemetry 토픽·스키마 그대로**, ts 만 과거 시각이다.
 * 서버 변경이 필요 없다: telemetry PK 가 (device_id, ts) + ON CONFLICT DO NOTHING 이고,
 * vehicle_state UPSERT 도 `WHERE vehicle_state.ts < EXCLUDED.ts` 라 백필이 현재 상태를
 * 과거로 되돌리지 않는다.
 *
 * 백필 표시자는 `sys.bf=1` — ingest 가 sys 를 통째로 data JSONB 에 넣으므로 게이트웨이
 * 코드 변경 없이 저장된다. 창 집계가 있으면(rec.agg_n>0) `agg` 오브젝트를 함께 싣는다.
 *
 * ⚠️ rec.ts 는 호출 전에 Buf::resolveTs() 로 확정돼 있어야 한다(ts=0 이면 발행하지 않는다).
 */
bool publishBackfill(TinyGsm &modem, const Buf::Record &rec, Stream &log);

/**
 * 고빈도 창 1건 발행 — 별도 토픽 `v1/{id}/telemetry/fast` (QoS 1).
 * ⚠️ 서버에 EMQX Rule `v1/+/telemetry/fast` 가 있어야 라우팅된다(`+` 는 한 레벨만 매칭).
 *    Rule 이 없으면 발행은 성공하나 조용히 버려진다 — 무해하지만 데이터·전력 낭비다.
 * t0 가 0(시각 미상)이면 발행하지 않는다 — 서버가 샘플 시각을 만들 수 없다.
 */
bool publishFast(TinyGsm &modem, const Fast::Sample *win, int n, uint32_t t0, Stream &log);

// 모뎀 RTC(CCLK)에서 시각을 읽어 Clk base 를 갱신한다. 오프라인 구간에서 시각 기준을
// 확보하기 위한 것 — 망이 끊겨도 모뎀 RTC 는 계속 돈다. AT 왕복 1회이므로 호출측이
// 빈도를 제한한다(base 가 아예 없을 때만). 성공 시 true.
bool noteTimeFromModem(TinyGsm &modem);

// MQTT 서비스(CMQTT) 완전 종료 — 모뎀 SSL 컨텍스트를 해제해 HTTP(S) 서비스와의 충돌을 막는다.
// OTA 다운로드 진입 직전에 호출(어차피 OTA 성공 시 재부팅되므로 세션은 버린다).
void stopService(TinyGsm &modem);

} // namespace Mqtt
