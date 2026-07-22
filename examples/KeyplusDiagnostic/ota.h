/**
 * @file      ota.h
 * @brief     8단계 — LTE OTA(ota_start): 모뎀 HTTP(S) 스트리밍 → Update.h → 재부팅
 *
 * 흐름(HttpsOTAUpgrade 예제 기반, 2단계 ack):
 *   [수신 loop] Cmd::handle() type=ota_start → Ota::start()
 *     ① Mqtt 서비스 중단(SSL 컨텍스트 해제) → ② https_get(size) → ③ Update.write 스트리밍
 *     ④ Update.end()(MD5 검증) 성공 → NVS(ns "ota")에 command_id·기대버전 저장 → esp_restart()
 *     ①~④ 중 실패면 false 반환 → 호출측이 즉시 failed ack(재부팅 안 함)
 *   [재부팅 후] MQTT 접속 성공 직후 → Ota::flushPendingAck()
 *     NVS pending 있으면 실행 이미지 FW_VERSION == 기대버전 → done, 불일치 → failed ack 후 소거
 *
 * 무결성: payload md5(32hex) 있으면 Update.setMD5()로 검증. 진위 보장 아님(상용은 HTTPS 필수).
 *
 * 하드 롤백(부트로더 레벨): 프레임워크에 CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y + 부트 WDT가
 *   켜져 있다. 단, arduino-esp32 코어는 기본적으로 부팅 시 mark_valid를 자동 호출해 롤백을 무력화
 *   하므로, .ino에서 verifyRollbackLater()를 true로 오버라이드해 자동 확정을 끄고 우리가 헬스체크
 *   (MQTT 접속) 통과 후 onHealthy()에서 직접 확정한다. 확정 전 크래시/행(WDT) 시 부트로더가
 *   직전 정상 이미지로 자동 롤백 → 구 이미지가 부팅해 버전 불일치로 failed ack.
 *
 * 계약: @keyplus-diagnostic/shared command.ts (type=ota_start, payload={url,md5,version,size})
 * 설계: phase8-firmware-ota-lte.md
 */
#pragma once

#include "utilities.h"
#include <TinyGsmClient.h>

namespace Ota {

// NVS pending 플래그 로드 + 실행 이미지 OTA 상태(PENDING_VERIFY 여부) 캐시 + 부팅 상태 로그.
// setup에서 1회 호출. 재부팅-후-성공/실패 판정과 롤백 확정 판단의 기준.
void begin();

// 발행 대기 중인 OTA 결과(성공/실패)가 있는가. loop가 접속 상태에서 이걸 보고 flush.
bool hasPending();

// ota_start 실행. 성공 시 NVS pending(result="") 저장 후 esp_restart()(반환하지 않음).
// 실패(URL/HTTP/공간/MD5/write) 시 NVS에 result="failed" 기록 후 false 반환.
//   ⚠️ 성공/실패 모두 ack는 여기서 발행하지 않는다(다운로드 중 MQTT 단절 가능) →
//      MQTT (재)접속 후 loop가 flushPendingAck()로 발행.
//   url : 모뎀이 GET할 바이너리 주소(로컬 평문 HTTP / 상용 HTTPS)
//   md5 : 32hex(빈 문자열이면 크기만 검증)   version : 기대 펌웨어 버전(빈 문자열 허용)
bool start(TinyGsm &modem, Stream &log,
           const String &cmdId, const String &url,
           const String &md5, const String &version);

// pending OTA 결과 ack 발행. MQTT 접속 상태에서 loop가 호출(hasPending() 가드).
// 발행 후 NVS 소거. (성공=버전 검증 done, 실패=기록된 failed)
void flushPendingAck(TinyGsm &modem, Stream &log);

// 헬스체크 통과(MQTT 접속) 시 호출. 실행 이미지가 PENDING_VERIFY면 mark_valid로 확정
// (= 부트로더 롤백 취소). 1회만 유효, 이후 no-op. 일반(비-OTA) 부팅에서는 no-op.
void onHealthy(Stream &log);

// loop 매 틱 감시. 실행 이미지가 PENDING_VERIFY인데 期限(OTA_CONFIRM_DEADLINE_MS) 내
// 확정(onHealthy) 못 하면 = 부팅되나 기능 불능 → 강제 롤백 재부팅(반환하지 않음).
void tick(Stream &log);

} // namespace Ota
