#include "KpOta.h"

#include <string.h>

namespace KpOta {

const char* statusName(Status s) {
    switch (s) {
        case Status::OK:            return "ok";
        case Status::NOT_STARTED:   return "not-started";
        case Status::NO_SPACE:      return "no-space";
        case Status::WRITE_FAIL:    return "write-fail";
        case Status::BAD_OFFSET:    return "bad-offset";
        case Status::SIZE_MISMATCH: return "size-mismatch";
        case Status::FOREIGN_HW:    return "foreign-hw";
        case Status::VERIFY_FAIL:   return "verify-fail";
        case Status::BAD_FRAME:     return "bad-frame";
    }
    return "?";
}

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t encodeBegin(const Begin& b, uint8_t* out, size_t cap) {
    const size_t ml = strnlen(b.md5, KPOTA_MD5_LEN + 1);
    const size_t vl = strnlen(b.version, KPOTA_VER_MAX + 1);
    if (ml > KPOTA_MD5_LEN || vl > KPOTA_VER_MAX) return 0;
    const size_t len = 4 + 4 + 1 + ml + 1 + vl;
    if (len > cap) return 0;
    put32(out, b.size);
    put32(out + 4, b.baud);
    out[8] = (uint8_t)ml;
    memcpy(out + 9, b.md5, ml);
    out[9 + ml] = (uint8_t)vl;
    memcpy(out + 10 + ml, b.version, vl);
    return len;
}

bool decodeBegin(const uint8_t* p, size_t len, Begin* b) {
    if (len < 10) return false;
    b->size = get32(p);
    b->baud = get32(p + 4);
    const size_t ml = p[8];
    if (ml > KPOTA_MD5_LEN || len < 10 + ml) return false;
    const size_t vl = p[9 + ml];
    if (vl > KPOTA_VER_MAX || len != 10 + ml + vl) return false;
    memcpy(b->md5, p + 9, ml);
    b->md5[ml] = '\0';
    memcpy(b->version, p + 10 + ml, vl);
    b->version[vl] = '\0';
    return b->size > 0;
}

size_t encodeChunk(uint32_t offset, const uint8_t* data, size_t n, uint8_t* out, size_t cap) {
    if (n == 0 || n > KPOTA_CHUNK_MAX || 4 + n > cap) return 0;
    put32(out, offset);
    memcpy(out + 4, data, n);
    return 4 + n;
}

bool decodeChunk(const uint8_t* p, size_t len, uint32_t* offset, const uint8_t** data, size_t* n) {
    if (len < 5 || len - 4 > KPOTA_CHUNK_MAX) return false;
    *offset = get32(p);
    *data = p + 4;
    *n = len - 4;
    return true;
}

size_t encodeEnd(uint32_t total, uint8_t* out, size_t cap) {
    if (cap < 4) return 0;
    put32(out, total);
    return 4;
}

bool decodeEnd(const uint8_t* p, size_t len, uint32_t* total) {
    if (len != 4) return false;
    *total = get32(p);
    return true;
}

size_t encodeAck(Phase ph, Status st, uint32_t next, uint8_t* out, size_t cap) {
    if (cap < KPOTA_ACK_LEN) return 0;
    out[0] = (uint8_t)ph;
    out[1] = (uint8_t)st;
    put32(out + 2, next);
    return KPOTA_ACK_LEN;
}

bool decodeAck(const uint8_t* p, size_t len, Phase* ph, Status* st, uint32_t* next) {
    if (len != KPOTA_ACK_LEN) return false;
    if (p[0] < (uint8_t)Phase::BEGIN || p[0] > (uint8_t)Phase::END) return false;
    if (p[1] > (uint8_t)Status::BAD_FRAME) return false;
    *ph = (Phase)p[0];
    *st = (Status)p[1];
    *next = get32(p + 2);
    return true;
}

// --- Receiver ----------------------------------------------------------------------

void Receiver::begin(const Flash& flash, ReplyFn reply, void* replyCtx) {
    flash_ = flash;
    reply_ = reply;
    replyCtx_ = replyCtx;
    active_ = finished_ = false;
    next_ = 0;
}

void Receiver::reply(Phase ph, Status st) {
    if (!reply_) return;
    uint8_t buf[KPOTA_ACK_LEN];
    reply_(buf, encodeAck(ph, st, next_, buf, sizeof(buf)), replyCtx_);
}

void Receiver::fail() {
    if (active_ && flash_.abort) flash_.abort(flash_.ctx);
    active_ = false;
}

void Receiver::onBegin(const uint8_t* p, size_t len, uint32_t now) {
    if (finished_) return;   // 이미 확정 — 재부팅을 기다린다
    Begin b;
    if (!decodeBegin(p, len, &b)) { reply(Phase::BEGIN, Status::BAD_FRAME); return; }

    // 같은 BEGIN 의 재전송(ACK 유실) — 처음부터 다시 쓰지 않고 OK 만 다시 준다.
    if (active_ && next_ == 0 && b.size == info_.size &&
        !strcmp(b.md5, info_.md5) && !strcmp(b.version, info_.version)) {
        lastAt_ = now;
        reply(Phase::BEGIN, Status::OK);
        return;
    }
    fail();   // 진행 중이던 다른 전송은 버린다(메인이 새로 시작했다)
    next_ = 0;
    if (!flash_.begin || !flash_.begin(b.size, b.md5, flash_.ctx)) {
        reply(Phase::BEGIN, Status::NO_SPACE);
        return;
    }
    info_ = b;
    active_ = true;
    lastAt_ = now;
    reply(Phase::BEGIN, Status::OK);
}

void Receiver::onChunk(const uint8_t* p, size_t len, uint32_t now) {
    uint32_t off;
    const uint8_t* data;
    size_t n;
    if (!decodeChunk(p, len, &off, &data, &n)) { reply(Phase::CHUNK, Status::BAD_FRAME); return; }
    if (!active_) { reply(Phase::CHUNK, Status::NOT_STARTED); return; }
    lastAt_ = now;

    if (off + n <= next_) {            // 이미 받은 조각(ACK 유실로 재전송) — 쓰지 않는다
        reply(Phase::CHUNK, Status::OK);
        return;
    }
    if (off != next_) {                 // 건너뛴 조각 — 메인은 next 부터 다시 보내야 하는데
        reply(Phase::CHUNK, Status::BAD_OFFSET);   // 되감을 수 없으므로 사실상 실패다
        return;
    }
    if ((uint64_t)off + n > info_.size) { fail(); reply(Phase::CHUNK, Status::SIZE_MISMATCH); return; }
    if (!flash_.write(data, n, flash_.ctx)) { fail(); reply(Phase::CHUNK, Status::WRITE_FAIL); return; }
    next_ += (uint32_t)n;
    reply(Phase::CHUNK, Status::OK);
}

void Receiver::onEnd(const uint8_t* p, size_t len, uint32_t now) {
    uint32_t total;
    if (finished_) { reply(Phase::END, Status::OK); return; }   // END 재전송
    if (!decodeEnd(p, len, &total)) { reply(Phase::END, Status::BAD_FRAME); return; }
    if (!active_) { reply(Phase::END, Status::NOT_STARTED); return; }
    lastAt_ = now;
    if (total != next_ || total != info_.size) { fail(); reply(Phase::END, Status::SIZE_MISMATCH); return; }
    Status st = flash_.end ? flash_.end(flash_.ctx) : Status::VERIFY_FAIL;
    active_ = false;   // end 는 성공·실패 모두 Update 를 닫는다
    if (st == Status::OK) finished_ = true;
    reply(Phase::END, st);
}

bool Receiver::tick(uint32_t now) {
    if (!active_) return false;
    if ((uint32_t)(now - lastAt_) < KPOTA_IDLE_ABORT_MS) return false;
    fail();
    return true;
}

}  // namespace KpOta
