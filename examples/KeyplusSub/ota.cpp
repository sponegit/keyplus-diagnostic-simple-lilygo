/**
 * @file      ota.cpp
 * @brief     서브 OTA 수신 구현 — KpOta::Receiver ↔ Update. 설계는 ota.h.
 */
#include "ota.h"
#include "config.h"
#include "link.h"

#include <KpOta.h>
#include <Update.h>
#include "esp_ota_ops.h"
#include "hwmark.h"   // lib/KpLink — config.h 의 HW_MODEL("v2-sub") 기준

namespace Ota {

static KpOta::Receiver  s_rx;
static HwMark::Scanner  s_mark;
static bool     s_baudSwitched = false;
static uint32_t s_switchedAt   = 0;
static uint32_t s_lastFrameAt  = 0;
static uint32_t s_rebootAt     = 0;     // END OK 후 ACK 가 나갈 시간을 두고 재부팅

static bool     s_pendingVerify = false;
static bool     s_confirmed     = false;
static uint32_t s_trialStart    = 0;

// --- Update 콜백 ----------------------------------------------------------------
static bool flashBegin(uint32_t size, const char *md5, void *)
{
    s_mark = HwMark::Scanner();
    if (!Update.begin(size, U_FLASH)) {
        Serial.printf("[OTA] Update.begin 실패 err=%u (크기 %lu)\n",
                      (unsigned)Update.getError(), (unsigned long)size);
        return false;
    }
    if (md5 && md5[0]) Update.setMD5(md5);
    return true;
}

static bool flashWrite(const uint8_t *data, size_t n, void *)
{
    s_mark.feed(data, n);
    return Update.write(const_cast<uint8_t *>(data), n) == n;
}

static KpOta::Status flashEnd(void *)
{
    if (s_mark.foreign()) {
        Serial.printf("[OTA] 다른 하드웨어 이미지(%s) — 서브는 %s. 확정 안 함\n",
                      s_mark.foreignModel(), HW_MODEL);
        Update.abort();
        return KpOta::Status::FOREIGN_HW;
    }
    if (!Update.end(true)) {
        Serial.printf("[OTA] Update.end 실패 err=%u (MD5 불일치/불완전)\n", (unsigned)Update.getError());
        return KpOta::Status::VERIFY_FAIL;
    }
    Serial.printf("[OTA] 플래시 완료·검증 통과 (마커 %s) — 재부팅\n",
                  s_mark.matched() ? "일치" : "없음");
    return KpOta::Status::OK;
}

static void flashAbort(void *) { Update.abort(); }

static void reply(const uint8_t *p, size_t n, void *)
{
    Link::sendFrame(KpLink::OTA_ACK, p, (uint16_t)n);
}

static void restoreBaud()
{
    if (!s_baudSwitched) return;
    Link::setBaud(LINK_BAUD);
    s_baudSwitched = false;
    Serial.printf("[OTA] 링크 속도 복귀 %d\n", LINK_BAUD);
}

void begin()
{
    KpOta::Receiver::Flash f = {flashBegin, flashWrite, flashEnd, flashAbort, nullptr};
    s_rx.begin(f, reply, nullptr);

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (running && esp_ota_get_state_partition(running, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pendingVerify = true;
        s_trialStart = millis();
        Serial.printf("[OTA] 새 이미지 시험 부팅(%s) — 메인 링크가 올라오면 확정, %lus 안에 안 되면 롤백\n",
                      running->label, (unsigned long)(OTA_CONFIRM_DEADLINE_MS / 1000));
    }
}

void onFrame(const KpLink::Frame &f)
{
    const uint32_t now = millis();
    s_lastFrameAt = now;
    switch (f.type) {
    case KpLink::OTA_BEGIN: {
        const bool wasActive = s_rx.active();
        s_rx.onBegin(f.payload, f.len, now);
        if (s_rx.active()) {
            const KpOta::Begin &b = s_rx.info();
            if (!wasActive)
                Serial.printf("[OTA] 수신 시작 — %lu bytes, ver %s, md5 %s\n", (unsigned long)b.size,
                              b.version[0] ? b.version : "?", b.md5[0] ? "있음" : "없음");
            // ACK 는 이미 옛 속도로 나갔다(reply). 다 나간 뒤 전환한다.
            if (b.baud && b.baud != (uint32_t)LINK_BAUD && !s_baudSwitched) {
                Link::setBaud(b.baud);
                s_baudSwitched = true;
                s_switchedAt = now;
            }
        }
        break;
    }
    case KpLink::OTA_CHUNK:
        s_rx.onChunk(f.payload, f.len, now);
        break;
    case KpLink::OTA_END:
        s_rx.onEnd(f.payload, f.len, now);
        if (s_rx.finished()) {
            if (!s_rebootAt) s_rebootAt = now + 300;
        } else if (!s_rx.active()) {
            restoreBaud();   // 검증 실패 — 옛 이미지로 계속 돈다
        }
        break;
    default:
        break;
    }
}

void tick()
{
    const uint32_t now = millis();

    // 전환 직후 OTA 프레임이 안 온다 = 메인이 ACK 를 못 받고 옛 속도로 BEGIN 을 재전송 중.
    if (s_baudSwitched && s_rx.active() && s_rx.received() == 0 &&
        (uint32_t)(now - s_lastFrameAt) >= OTA_BAUD_FALLBACK_MS &&
        (uint32_t)(now - s_switchedAt) >= OTA_BAUD_FALLBACK_MS) {
        Serial.println("[OTA] 전환 속도에서 무응답 — 옛 속도로 돌아가 BEGIN 재전송을 기다린다");
        restoreBaud();
    }

    if (s_rx.tick(now)) {
        Serial.printf("[OTA] %lus 무진전 — 포기(%lu bytes 받음)\n",
                      (unsigned long)(KPOTA_IDLE_ABORT_MS / 1000), (unsigned long)s_rx.received());
        restoreBaud();
    }

    if (s_rebootAt && (int32_t)(now - s_rebootAt) >= 0) {
        Serial.flush();
        ESP.restart();
    }

    if (s_pendingVerify && !s_confirmed &&
        (uint32_t)(now - s_trialStart) >= OTA_CONFIRM_DEADLINE_MS) {
        Serial.println("[OTA] 期限 안에 메인 링크 없음 — 직전 이미지로 롤백");
        Serial.flush();
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

void onLinkUp()
{
    if (!s_pendingVerify || s_confirmed) return;
    esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    s_confirmed = true;
    Serial.printf("[OTA] 새 이미지 확정(mark_valid) %s\n", e == ESP_OK ? "OK" : "err");
}

bool active() { return s_rx.active(); }

void printStatus(Stream &io)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    io.println("[OTA] 서브 OTA");
    io.printf("  running      : %s%s\n", running ? running->label : "?",
              s_pendingVerify ? (s_confirmed ? " (확정됨)" : " (시험 부팅 — 미확정)") : "");
    if (s_rx.active())
        io.printf("  receiving    : %lu / %lu bytes%s\n", (unsigned long)s_rx.received(),
                  (unsigned long)s_rx.info().size, s_baudSwitched ? " (고속)" : "");
}

} // namespace Ota
