/**
 * @file      ota.cpp
 * @brief     LTE OTA 구현 — 모뎀 HTTP(S) 스트리밍 → Update.h → 재부팅 + NVS 2단계 ack.
 *
 * ack 전달 신뢰성: OTA는 다운로드 중 MQTT를 정지(SSL 컨텍스트 양보)하거나 수 분간 유휴로
 * NAT가 세션을 끊을 수 있어, ack를 그 자리에서 발행하면 유실될 수 있다. 따라서 결과를
 * **NVS에 기록**하고, MQTT가 (재)접속된 뒤 loop가 flushPendingAck()로 발행한다.
 *   성공 → 재부팅 후 새 이미지가 접속 시 done(버전 검증)
 *   실패(재부팅 전) → result="failed" 기록 → MQTT 재접속 시 failed
 */
#include "ota.h"
#include "config.h"
#include "cmd.h"
#include "mqtt.h"
#include <Update.h>
#include <Preferences.h>
#include "esp_ota_ops.h"

namespace Ota {

static const char *kNs = "ota";     // NVS namespace (pending OTA 지속)
static bool s_pendingRam = false;   // NVS pending 캐시(매 틱 NVS 읽기 회피)

// 하드 롤백 상태(부트 시 1회 판정 후 캐시).
static bool     s_pendingVerify = false;  // 실행 이미지가 PENDING_VERIFY(= OTA 후 미확정) 인가
static bool     s_confirmed     = false;  // onHealthy로 mark_valid 완료했는가
static uint32_t s_trialStart    = 0;      // PENDING_VERIFY 부팅 기준 millis (期限 계산용)

// --- NVS pending helpers -----------------------------------------------------
// result="" → 부팅 후 버전 검증으로 done/failed 판정. result="failed" → 그대로 확정.
static void savePending(const String &cmdId, const String &version, const char *result)
{
    Preferences p;
    p.begin(kNs, /*readOnly=*/false);
    p.putBool("pending", true);
    p.putString("cmd", cmdId);
    p.putString("ver", version);
    p.putString("result", result);
    p.end();
    s_pendingRam = true;
}

static void clearPending()
{
    Preferences p;
    p.begin(kNs, /*readOnly=*/false);
    p.clear();
    p.end();
    s_pendingRam = false;
}

void begin()
{
    Preferences p;
    p.begin(kNs, /*readOnly=*/true);
    s_pendingRam = p.getBool("pending", false);
    p.end();

    // 실행 이미지의 OTA 상태 판정 — PENDING_VERIFY면 이번 부팅이 "롤백 대기 중인 새 이미지".
    // (arduino-esp32 자동 mark_valid는 .ino verifyRollbackLater()=true로 꺼 둔 상태 전제.)
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (running && esp_ota_get_state_partition(running, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pendingVerify = true;
        s_trialStart = millis();
    }
    // 진단: 부팅 파티션/상태.
    // (label 예: app0/app1, state: 0=NEW,1=PENDING_VERIFY,2=VALID,3=INVALID,4=ABORTED,0xff=UNDEFINED)
    // s_pendingVerify=true면 확정(onHealthy) 또는 期限 초과 강제 롤백(tick) 대상.
    Serial.printf("[OTA] boot partition=%s state=%d pendingVerify=%d\n",
                  running ? running->label : "?", (int)st, s_pendingVerify ? 1 : 0);
}

bool hasPending() { return s_pendingRam; }

void onHealthy(Stream &log)
{
    if (!s_pendingVerify || s_confirmed) return;
    // 헬스체크 통과(MQTT 접속) → 새 이미지 정상 확정. 부트로더 롤백 취소.
    esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    s_confirmed = true;
    log.printf("[OTA] 새 이미지 확정(mark_valid) %s — 롤백 취소\n",
               e == ESP_OK ? "OK" : "err");
}

void tick(Stream &log)
{
    // PENDING_VERIFY인데 期限 내 확정(MQTT 접속) 못 함 = 부팅되나 기능 불능 → 강제 롤백.
    if (!s_pendingVerify || s_confirmed) return;
    if (millis() - s_trialStart < OTA_CONFIRM_DEADLINE_MS) return;
    log.printf("[OTA] ⚠️ %lus 내 헬스체크 실패 — 강제 롤백 재부팅\n",
               (unsigned long)(OTA_CONFIRM_DEADLINE_MS / 1000));
    delay(200);
    esp_ota_mark_app_invalid_rollback_and_reboot();  // 직전 정상 이미지로 롤백 후 재부팅(반환 안 함)
}

// --- ota_start 실행 ----------------------------------------------------------
// 실패 시 NVS에 failed 기록 후 false 반환(호출측은 ack하지 않음 — flush가 담당).
bool start(TinyGsm &modem, Stream &log,
           const String &cmdId, const String &url,
           const String &md5, const String &version)
{
    if (url.isEmpty() || !url.startsWith("http")) {
        log.printf("[OTA] 잘못된 URL: '%s' — 실패\n", url.c_str());
        savePending(cmdId, version, "failed");
        return false;
    }
    log.printf("[OTA] 시작 cmd=%s ver=%s\n  url=%s\n  md5=%s\n",
               cmdId.c_str(), version.c_str(), url.c_str(),
               md5.isEmpty() ? "(없음 — 크기만 검증)" : md5.c_str());

    // ① MQTT 서비스 중단 → 모뎀 SSL 컨텍스트를 HTTP(S) 서비스에 양보(충돌 회피).
    Mqtt::stopService(modem);
    delay(200);

    // ② HTTP(S) GET → 바디 크기 취득.
    modem.https_begin();
    if (!modem.https_set_url(url)) {
        log.println("[OTA] set_url 실패");
        modem.https_end();
        savePending(cmdId, version, "failed");
        return false;
    }
    size_t fwSize = 0;
    int httpCode = modem.https_get(&fwSize);
    if (httpCode != 200 || fwSize == 0) {
        log.printf("[OTA] HTTP GET 실패 code=%d size=%u\n", httpCode, (unsigned)fwSize);
        modem.https_end();
        savePending(cmdId, version, "failed");
        return false;
    }
    log.printf("[OTA] 펌웨어 크기 %u bytes — 플래시 시작\n", (unsigned)fwSize);

    // ③ Update 시작 + (있으면) MD5 기대값 지정 → end()에서 자동 검증.
    if (!Update.begin(fwSize)) {
        log.printf("[OTA] Update.begin 실패(공간 부족?) err=%d\n", Update.getError());
        modem.https_end();
        savePending(cmdId, version, "failed");
        return false;
    }
    if (!md5.isEmpty()) Update.setMD5(md5.c_str());

    // ④ 스트리밍 write (진행률 로그 + 무진전 타임아웃 가드).
    uint8_t  buf[OTA_CHUNK_SIZE];
    size_t   total = 0;
    int      lastPct = 0;
    uint32_t lastAdvance = millis();

    while (total < fwSize) {
        int len = modem.https_body(buf, OTA_CHUNK_SIZE);
        if (len > 0) {
            if (Update.write(buf, len) != (size_t)len) {
                log.printf("[OTA] Update.write 실패 err=%d — 중단\n", Update.getError());
                Update.abort();
                modem.https_end();
                savePending(cmdId, version, "failed");
                return false;
            }
            total += len;
            lastAdvance = millis();
            int pct = (int)((total * 100ULL) / fwSize);
            if (pct - lastPct >= OTA_PROGRESS_STEP_PCT || pct == 100) {
                lastPct = pct;
                log.printf("[OTA] %d%% (%u/%u)\n", pct, (unsigned)total, (unsigned)fwSize);
            }
        } else {
            if (millis() - lastAdvance > OTA_STALL_TIMEOUT_MS) {
                log.printf("[OTA] 다운로드 무진전 %lus — 타임아웃 실패(%u/%u)\n",
                           (unsigned long)(OTA_STALL_TIMEOUT_MS / 1000),
                           (unsigned)total, (unsigned)fwSize);
                Update.abort();
                modem.https_end();
                savePending(cmdId, version, "failed");
                return false;
            }
            delay(10);
        }
    }
    modem.https_end();

    // ⑤ 마감 — MD5/크기 검증 포함. 실패면 실행 이미지는 그대로(재부팅 안 함).
    if (!Update.end(true)) {
        log.printf("[OTA] Update.end 실패 err=%d (MD5 불일치/불완전) — 실패\n", Update.getError());
        savePending(cmdId, version, "failed");
        return false;
    }
    log.println("[OTA] ✅ 플래시 완료·검증 통과 — 재부팅 예약");

    // ⑥ 성공: 결과 미정(result="")으로 pending 저장 → 부팅 후 버전 검증. 그 뒤 재부팅.
    savePending(cmdId, version, "");
    delay(1000);
    ESP.restart();
    return true;   // 도달하지 않음
}

// --- pending ack flush (MQTT 접속 상태에서 loop가 호출) -----------------------
void flushPendingAck(TinyGsm &modem, Stream &log)
{
    if (!s_pendingRam) return;

    Preferences p;
    p.begin(kNs, /*readOnly=*/true);
    String cmdId  = p.getString("cmd", "");
    String ver    = p.getString("ver", "");
    String stored = p.getString("result", "");
    p.end();

    if (cmdId.isEmpty()) { clearPending(); return; }

    const char *result;
    if (stored.length()) {
        result = stored.c_str();                 // 재부팅 전 실패 등 확정 결과
    } else {
        // 성공 경로: 실행 이미지 버전이 기대버전과 일치하면 새 이미지 정상 부팅 → done.
        // 기대버전 미지정이면 best-effort done(어떤 이미지든 부팅에 성공). 불일치=롤백 → failed.
        bool ok = ver.isEmpty() || (ver == String(FW_VERSION));
        result = ok ? "done" : "failed";
    }
    log.printf("[OTA] pending ack: cmd=%s 기대=%s 실행=%s → %s\n",
               cmdId.c_str(), ver.isEmpty() ? "(미지정)" : ver.c_str(),
               FW_VERSION, result);

    Cmd::sendAck(modem, log, cmdId, result);
    clearPending();
}

} // namespace Ota
