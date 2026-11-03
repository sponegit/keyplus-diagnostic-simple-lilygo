#include "KpKey.h"

#include <stdlib.h>
#include <string.h>

namespace KpKey {

const char* errorName(Error e) {
    switch (e) {
        case Error::OK:             return "ok";
        case Error::ABSENT:         return "absent";
        case Error::PARSE:          return "parse";
        case Error::NO_STEPS:       return "no-steps";
        case Error::TOO_MANY_STEPS: return "too-many-steps";
        case Error::BAD_IO:         return "bad-io";
        case Error::BAD_PRESS:      return "bad-press";
        case Error::BAD_WAIT:       return "bad-wait";
        case Error::TOO_LONG:       return "too-long";
    }
    return "?";
}

const char* ioName(uint8_t io) {
    static const char* const names[] = {"io1", "io2", "io3", "io4", "io5", "io6"};
    return (io >= KPKEY_IO_MIN && io <= KPKEY_IO_MAX) ? names[io - 1] : "?";
}

const char* statusName(Status s) {
    switch (s) {
        case Status::ACCEPTED: return "accepted";
        case Status::DONE:     return "done";
        case Status::BUSY:     return "busy";
        case Status::REJECTED: return "rejected";
    }
    return "?";
}

const char* failName(Client::Fail f) {
    switch (f) {
        case Client::Fail::NONE:         return "none";
        case Client::Fail::NO_RESPONSE:  return "no-response";
        case Client::Fail::SUB_BUSY:     return "sub-busy";
        case Client::Fail::SUB_REJECTED: return "sub-rejected";
        case Client::Fail::TIMEOUT:      return "timeout";
        case Client::Fail::LINK_LOST:    return "link-lost";
    }
    return "?";
}

uint32_t totalMs(const Step* steps, size_t n) {
    uint32_t t = 0;
    for (size_t i = 0; i < n; i++) t += (uint32_t)steps[i].pressMs + steps[i].waitMs;
    return t;
}

Error validate(const Step* steps, size_t n) {
    if (n == 0) return Error::NO_STEPS;
    if (n > KPKEY_MAX_STEPS) return Error::TOO_MANY_STEPS;
    for (size_t i = 0; i < n; i++) {
        if (steps[i].io < KPKEY_IO_MIN || steps[i].io > KPKEY_IO_MAX) return Error::BAD_IO;
        if (steps[i].pressMs < 1 || steps[i].pressMs > KPKEY_PRESS_MAX_MS) return Error::BAD_PRESS;
        if (steps[i].waitMs > KPKEY_WAIT_MAX_MS) return Error::BAD_WAIT;
    }
    if (totalMs(steps, n) > KPKEY_TOTAL_MAX_MS) return Error::TOO_LONG;
    return Error::OK;
}

// --- JSON ----------------------------------------------------------------------

static const char* skipWs(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

// 객체 하나 {"io":1,"press_ms":200,"wait_ms":300} — 키 순서 자유, 모르는 키는 숫자만 허용.
// 성공 시 '}' 다음을 반환, 실패 시 nullptr.
static const char* parseStepObj(const char* p, Step* st) {
    p = skipWs(p);
    if (*p != '{') return nullptr;
    p++;
    long io = 0, press = 0, wait = 0;
    bool haveIo = false, havePress = false;   // 음수 값과 '키 없음'을 구분한다
    for (;;) {
        p = skipWs(p);
        if (*p == '}') { p++; break; }
        if (*p != '"') return nullptr;
        const char* k = ++p;
        while (*p && *p != '"') p++;
        if (*p != '"') return nullptr;
        size_t klen = (size_t)(p - k);
        p = skipWs(p + 1);
        if (*p != ':') return nullptr;
        p = skipWs(p + 1);
        if (!(*p == '-' || (*p >= '0' && *p <= '9'))) return nullptr;   // 숫자만
        char* end = nullptr;
        long v = strtol(p, &end, 10);
        if (end == p) return nullptr;
        p = end;
        if (*p == '.') return nullptr;   // 소수 금지 — ms 는 정수
        if      (klen == 2 && !strncmp(k, "io", 2))       { io = v; haveIo = true; }
        else if (klen == 8 && !strncmp(k, "press_ms", 8)) { press = v; havePress = true; }
        else if (klen == 7 && !strncmp(k, "wait_ms", 7))  wait = v;
        p = skipWs(p);
        if (*p == ',') { p++; continue; }
        if (*p == '}') { p++; break; }
        return nullptr;
    }
    if (!haveIo || !havePress) return nullptr;   // 필수 키
    // 범위 밖 값은 일단 담고 validate 가 사유를 정한다 — 음수·큰 값은 반드시 범위 밖이 되게
    // 고정한다(그냥 캐스트하면 -5 가 65531 이 되는 식으로 우연히 범위 안에 들어올 수 있다).
    st->io      = (io < 0) ? 0 : (io > 255) ? 255 : (uint8_t)io;
    st->pressMs = (press < 0) ? 0 : (press > 65535) ? 65535 : (uint16_t)press;
    st->waitMs  = (wait < 0 || wait > 65535) ? 65535 : (uint16_t)wait;
    return p;
}

Error parseStepsJson(const char* json, Step* out, size_t max, size_t* n) {
    *n = 0;
    if (!json) return Error::ABSENT;
    const char* p = strstr(json, "\"steps\"");
    if (!p) return Error::ABSENT;
    p = skipWs(p + 7);
    if (*p != ':') return Error::PARSE;
    p = skipWs(p + 1);
    if (*p != '[') return Error::PARSE;
    p = skipWs(p + 1);
    size_t count = 0;
    if (*p == ']') return Error::NO_STEPS;
    for (;;) {
        Step st;
        p = parseStepObj(p, &st);
        if (!p) return Error::PARSE;
        if (count >= max || count >= KPKEY_MAX_STEPS) return Error::TOO_MANY_STEPS;
        out[count++] = st;
        p = skipWs(p);
        if (*p == ',') { p++; continue; }
        if (*p == ']') break;
        return Error::PARSE;
    }
    *n = count;
    return validate(out, count);
}

// --- 링크 payload -----------------------------------------------------------------

size_t encodeCmd(uint16_t id, const Step* steps, size_t n, uint8_t* out, size_t cap) {
    if (n > KPKEY_MAX_STEPS) return 0;
    const size_t len = 3 + 5 * n;
    if (len > cap) return 0;
    out[0] = (uint8_t)(id & 0xFF);
    out[1] = (uint8_t)(id >> 8);
    out[2] = (uint8_t)n;
    for (size_t i = 0; i < n; i++) {
        uint8_t* q = out + 3 + 5 * i;
        q[0] = steps[i].io;
        q[1] = (uint8_t)(steps[i].pressMs & 0xFF);
        q[2] = (uint8_t)(steps[i].pressMs >> 8);
        q[3] = (uint8_t)(steps[i].waitMs & 0xFF);
        q[4] = (uint8_t)(steps[i].waitMs >> 8);
    }
    return len;
}

Error decodeCmd(const uint8_t* p, size_t len, uint16_t* id, Step* out, size_t* n) {
    *n = 0;
    if (len < 3) return Error::PARSE;
    *id = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    const size_t count = p[2];
    if (len != 3 + 5 * count) return Error::PARSE;
    if (count > KPKEY_MAX_STEPS) return Error::TOO_MANY_STEPS;
    for (size_t i = 0; i < count; i++) {
        const uint8_t* q = p + 3 + 5 * i;
        out[i].io      = q[0];
        out[i].pressMs = (uint16_t)q[1] | ((uint16_t)q[2] << 8);
        out[i].waitMs  = (uint16_t)q[3] | ((uint16_t)q[4] << 8);
    }
    *n = count;
    return validate(out, count);
}

size_t encodeResult(uint16_t id, Status s, uint8_t detail, uint8_t* out, size_t cap) {
    if (cap < KPKEY_RESULT_LEN) return 0;
    out[0] = (uint8_t)(id & 0xFF);
    out[1] = (uint8_t)(id >> 8);
    out[2] = (uint8_t)s;
    out[3] = detail;
    return KPKEY_RESULT_LEN;
}

bool decodeResult(const uint8_t* p, size_t len, uint16_t* id, Status* s, uint8_t* detail) {
    if (len != KPKEY_RESULT_LEN) return false;
    if (p[2] < (uint8_t)Status::ACCEPTED || p[2] > (uint8_t)Status::REJECTED) return false;
    *id     = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    *s      = (Status)p[2];
    *detail = p[3];
    return true;
}

// --- Executor ----------------------------------------------------------------------

void Executor::begin(LineFn line, ReplyFn reply, void* ctx) {
    line_ = line;
    reply_ = reply;
    ctx_ = ctx;
    active_ = pressed_ = false;
    haveLast_ = false;
    stats_ = {};
}

void Executor::reply(uint16_t id, Status s, uint8_t detail) {
    if (!reply_) return;
    uint8_t buf[KPKEY_RESULT_LEN];
    size_t n = encodeResult(id, s, detail, buf, sizeof(buf));
    reply_(buf, n, ctx_);
}

void Executor::start(const Step* steps, size_t n, uint32_t now) {
    memcpy(steps_, steps, n * sizeof(Step));
    n_ = (uint8_t)n;
    idx_ = 0;
    active_ = true;
    pressed_ = true;
    phaseEnd_ = now + steps_[0].pressMs;
    if (line_) line_(steps_[0].io, true, ctx_);
    stats_.executed++;
}

void Executor::onCmd(const uint8_t* p, size_t len, uint32_t now) {
    uint16_t id = 0;
    Step     steps[KPKEY_MAX_STEPS];
    size_t   n = 0;
    Error    e = decodeCmd(p, len, &id, steps, &n);
    if (len < 3) return;   // id·count 조차 없으면 답할 대상이 없다

    // 중복(재전송) — 실행하지 않고 지금 상태를 다시 알린다.
    if (active_ && remote_ && id == curId_) {
        stats_.duplicates++;
        reply(id, Status::ACCEPTED, 0);
        return;
    }
    if (haveLast_ && id == lastId_) {
        stats_.duplicates++;
        reply(id, lastStatus_, lastDetail_);
        return;
    }

    if (e != Error::OK) {
        stats_.rejected++;
        haveLast_ = true; lastId_ = id; lastStatus_ = Status::REJECTED; lastDetail_ = (uint8_t)e;
        reply(id, Status::REJECTED, (uint8_t)e);
        return;
    }
    if (active_) {
        // 진행 중인 시퀀스를 뒤엎지 않는다. BUSY 는 기억하지 않는다 — 같은 id 재전송이
        // 시퀀스가 끝난 뒤 도착하면 그때는 실행해도 된다(메인은 이미 BUSY 로 실패 처리했으므로
        // 실제로는 재전송하지 않는다).
        stats_.busy++;
        reply(id, Status::BUSY, 0);
        return;
    }
    remote_ = true;
    curId_ = id;
    start(steps, n, now);
    reply(id, Status::ACCEPTED, 0);
}

Error Executor::startLocal(const Step* steps, size_t n, uint32_t now, bool* busy) {
    *busy = false;
    Error e = validate(steps, n);
    if (e != Error::OK) return e;
    if (active_) { *busy = true; return Error::OK; }
    remote_ = false;
    start(steps, n, now);
    return Error::OK;
}

void Executor::finish() {
    active_ = pressed_ = false;
    if (remote_) {
        haveLast_ = true; lastId_ = curId_; lastStatus_ = Status::DONE; lastDetail_ = 0;
        reply(curId_, Status::DONE, 0);
    }
}

void Executor::update(uint32_t now) {
    // 한 틱에 여러 단계를 넘길 수 있다(wait=0 등). 단계 수만큼만 돈다.
    for (int guard = 0; active_ && guard < 2 * KPKEY_MAX_STEPS + 1; guard++) {
        if ((int32_t)(now - phaseEnd_) < 0) return;
        if (pressed_) {
            pressed_ = false;
            if (line_) line_(steps_[idx_].io, false, ctx_);
            phaseEnd_ += steps_[idx_].waitMs;
        } else {
            if (++idx_ >= n_) { finish(); return; }
            pressed_ = true;
            if (line_) line_(steps_[idx_].io, true, ctx_);
            phaseEnd_ += steps_[idx_].pressMs;
        }
    }
}

void Executor::abort() {
    if (!active_) return;
    if (pressed_ && line_) line_(steps_[idx_].io, false, ctx_);
    active_ = pressed_ = false;
    if (remote_) {
        // 중단된 원격 명령의 재전송이 와도 다시 실행하지 않게 기억한다.
        haveLast_ = true; lastId_ = curId_; lastStatus_ = Status::REJECTED; lastDetail_ = 0;
    }
    stats_.aborted++;
}

void Executor::forget() {
    haveLast_ = false;
}

// --- Client ------------------------------------------------------------------------

void Client::begin(SendFn send, void* ctx, uint16_t firstId) {
    send_ = send;
    ctx_ = ctx;
    state_ = State::IDLE;
    nextId_ = firstId ? firstId : 1;
}

bool Client::sendCmd() {
    return send_ && send_(buf_, len_, ctx_);
}

Client::Start Client::start(const Step* steps, size_t n, uint32_t now, Error* err) {
    if (busy()) return Start::BUSY;
    Error e = validate(steps, n);
    if (err) *err = e;
    if (e != Error::OK) return Start::INVALID;

    id_ = nextId_++;
    if (nextId_ == 0) nextId_ = 1;
    len_ = encodeCmd(id_, steps, n, buf_, sizeof(buf_));
    total_ = totalMs(steps, n);
    if (!sendCmd()) return Start::LINK_DOWN;
    // 이전 결과를 아직 안 꺼냈어도 새 요청이 덮는다(호출측이 매 틱 takeOutcome 한다는 전제).
    state_ = State::WAIT_ACCEPT;
    tries_ = 0;
    deadline_ = now + KPKEY_ACCEPT_TIMEOUT_MS;
    return Start::OK;
}

void Client::finish(bool done, Fail why) {
    state_ = State::FINISHED;
    done_ = done;
    why_ = why;
}

void Client::onResult(const uint8_t* p, size_t len, uint32_t now) {
    uint16_t id;
    Status   s;
    uint8_t  detail;
    if (!decodeResult(p, len, &id, &s, &detail)) return;
    if (!busy() || id != id_) return;   // 지난 요청의 늦은 답 — 무시
    switch (s) {
        case Status::ACCEPTED:
            if (state_ == State::WAIT_ACCEPT) {
                state_ = State::RUNNING;
                deadline_ = now + total_ + KPKEY_DONE_MARGIN_MS;
            }
            break;
        case Status::DONE:     finish(true, Fail::NONE); break;
        case Status::BUSY:     finish(false, Fail::SUB_BUSY); break;
        case Status::REJECTED: finish(false, Fail::SUB_REJECTED); break;
    }
}

void Client::update(uint32_t now) {
    if (!busy()) return;
    if ((int32_t)(now - deadline_) < 0) return;
    if (state_ == State::WAIT_ACCEPT) {
        if (tries_ < KPKEY_RETRIES && sendCmd()) {
            tries_++;
            deadline_ = now + KPKEY_ACCEPT_TIMEOUT_MS;
            return;
        }
        finish(false, Fail::NO_RESPONSE);
    } else {
        finish(false, Fail::TIMEOUT);
    }
}

void Client::linkLost() {
    if (busy()) finish(false, Fail::LINK_LOST);
}

bool Client::takeOutcome(bool* done, Fail* why, uint16_t* id) {
    if (state_ != State::FINISHED) return false;
    state_ = State::IDLE;
    if (done) *done = done_;
    if (why)  *why  = why_;
    if (id)   *id   = id_;
    return true;
}

}  // namespace KpKey
