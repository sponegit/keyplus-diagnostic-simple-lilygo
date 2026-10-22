#include "KpLinkSession.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace KpLink {

// HELLO payload 배치 — [0] 규약 버전은 버전이 바뀌어도 위치 고정(헤더 주석).
//   [0] proto  [1] role  [2] flags(b0 seen, b1 up, b2 boot)  [3] fwLen  [fw..]  [hwLen]  [hw..]
static const uint8_t HELLO_SEEN = 0x01;
static const uint8_t HELLO_UP   = 0x02;
static const uint8_t HELLO_BOOT = 0x04;

static void copyId(char* dst, const char* src) {
    size_t n = src ? strlen(src) : 0;
    if (n > KPLINK_ID_MAX) n = KPLINK_ID_MAX;
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
}

const char* reasonName(Session::Reason r) {
    switch (r) {
        case Session::Reason::HANDSHAKE:    return "handshake";
        case Session::Reason::TIMEOUT:      return "timeout";
        case Session::Reason::PEER_RESTART: return "peer-restart";
        case Session::Reason::PEER_LOST:    return "peer-lost";
    }
    return "?";
}

void Session::begin(Role role, const char* fw, const char* hw,
                    WriteFn write, void* ctx, uint32_t now) {
    role_  = role;
    copyId(fw_, fw);
    copyId(hw_, hw);
    write_ = write;
    ctx_   = ctx;
    dec_.reset();
    up_ = seen_ = false;
    boot_ = true;
    stats_ = {};
    peerProto_ = 0;
    peerFw_[0] = peerHw_[0] = '\0';
    lastRxAt_ = lastByteAt_ = lastBeatAt_ = now;
    started_ = true;
    sendHello();
    lastHelloAt_ = now;
}

int Session::sendRaw(uint8_t type, const uint8_t* payload, uint16_t len) {
    if (!started_ || !write_) return -1;
    uint8_t buf[KPLINK_MAX_FRAME];
    const uint8_t seq = txSeq_;
    size_t n = encode(type, seq, payload, len, buf, sizeof(buf));
    if (n == 0) return -1;
    txSeq_++;
    write_(buf, n, ctx_);
    stats_.txFrames++;
    return seq;
}

int Session::send(uint8_t type, const uint8_t* payload, uint16_t len) {
    return sendRaw(type, payload, len);
}

void Session::sendHello() {
    uint8_t p[4 + 2 * KPLINK_ID_MAX + 1];
    size_t  i = 0;
    p[i++] = KPLINK_PROTO_VERSION;
    p[i++] = (uint8_t)role_;
    p[i++] = (seen_ ? HELLO_SEEN : 0) | (up_ ? HELLO_UP : 0) | (boot_ ? HELLO_BOOT : 0);
    const uint8_t fl = (uint8_t)strlen(fw_);
    p[i++] = fl;
    memcpy(p + i, fw_, fl); i += fl;
    const uint8_t hl = (uint8_t)strlen(hw_);
    p[i++] = hl;
    memcpy(p + i, hw_, hl); i += hl;
    sendRaw(HELLO, p, (uint16_t)i);
}

void Session::setUp(bool up, Reason why, uint32_t now) {
    if (up_ == up) return;
    up_ = up;
    if (up) {
        stats_.ups++;
        stats_.lastUpAt = now;
        boot_ = false;
        lastBeatAt_ = now;
    } else {
        stats_.downs++;
        if (why == Reason::TIMEOUT)      stats_.timeouts++;
        if (why == Reason::PEER_RESTART) stats_.peerRestarts++;
        if (why == Reason::PEER_LOST)    stats_.peerLost++;
    }
    if (onState_) onState_(up, why, ctx_);
}

void Session::onBytes(const uint8_t* data, size_t n, uint32_t now) {
    if (!started_) return;
    if (n) lastByteAt_ = now;
    Frame f;
    while (n) {
        size_t took = dec_.feed(data, n);
        data += took;
        n    -= took;
        while (dec_.next(f)) handle(f, now);
    }
}

void Session::handle(const Frame& f, uint32_t now) {
    stats_.rxFrames++;
    if (f.type == HELLO) {
        handleHello(f, now);
        return;
    }
    // 규약 불일치 상태(상대 HELLO 가 다른 버전)에서는 앱 프레임을 믿지 않는다.
    if (peerProto_ != 0 && peerProto_ != KPLINK_PROTO_VERSION) return;
    lastRxAt_ = now;
    if (f.type == HEARTBEAT) return;
    if (onFrame_) onFrame_(f, ctx_);
}

void Session::handleHello(const Frame& f, uint32_t now) {
    if (f.len < 1) return;
    peerProto_ = f.payload[0];
    if (peerProto_ != KPLINK_PROTO_VERSION) {
        stats_.protoMismatch++;
        setUp(false, Reason::HANDSHAKE, now);
        return;   // 버전이 다르면 나머지 배치를 해석할 근거가 없다
    }
    if (f.len < 5) return;

    size_t i = 1;
    const uint8_t role  = f.payload[i++];
    const uint8_t flags = f.payload[i++];
    const uint8_t fl    = f.payload[i++];
    if (fl > KPLINK_ID_MAX || i + fl + 1 > f.len) return;
    const uint8_t* fw = f.payload + i; i += fl;
    const uint8_t hl  = f.payload[i++];
    if (hl > KPLINK_ID_MAX || i + hl > f.len) return;
    const uint8_t* hw = f.payload + i;

    lastRxAt_ = now;
    peerRole_ = (Role)role;
    memcpy(peerFw_, fw, fl); peerFw_[fl] = '\0';
    memcpy(peerHw_, hw, hl); peerHw_[hl] = '\0';

    const bool peerSeen = flags & HELLO_SEEN;
    const bool peerUp   = flags & HELLO_UP;
    const bool peerBoot = flags & HELLO_BOOT;

    // 업 상태인데 상대가 우리를 모른다 = 상대가 재부팅했거나 상대 쪽에서 타임아웃이 났다.
    if (up_ && !peerSeen) setUp(false, peerBoot ? Reason::PEER_RESTART : Reason::PEER_LOST, now);

    seen_ = true;
    if (peerSeen) setUp(true, Reason::HANDSHAKE, now);
    if (!peerUp) {
        sendHello();
        lastHelloAt_ = now;
    }
}

void Session::tick(uint32_t now) {
    if (!started_) return;

    // 가짜 헤더가 큰 len 을 주장해 진짜 프레임이 묶인 경우 — 바이트 공백이면 풀어 준다.
    if (dec_.pending() && (uint32_t)(now - lastByteAt_) >= KPLINK_BYTE_GAP_MS) {
        dec_.expire();
        Frame f;
        while (dec_.next(f)) handle(f, now);
    }

    if (up_) {
        if ((uint32_t)(now - lastRxAt_) >= KPLINK_LINK_TIMEOUT_MS) {
            setUp(false, Reason::TIMEOUT, now);
            seen_ = false;          // 상대를 잊고 처음부터 — 상대는 seen=0 을 보고 재연결한다
            sendHello();
            lastHelloAt_ = now;
            return;
        }
        if ((uint32_t)(now - lastBeatAt_) >= KPLINK_HEARTBEAT_MS) {
            sendRaw(HEARTBEAT, nullptr, 0);
            lastBeatAt_ = now;
        }
    } else {
        // 다운 상태에서 상대가 오래 조용하면 상대를 잊는다(seen 유지 시 상대 재부팅을 놓친다).
        if (seen_ && (uint32_t)(now - lastRxAt_) >= KPLINK_LINK_TIMEOUT_MS) seen_ = false;
        if ((uint32_t)(now - lastHelloAt_) >= KPLINK_HELLO_INTERVAL_MS) {
            sendHello();
            lastHelloAt_ = now;
        }
    }
}

// 고정 버퍼에 printf 를 이어 붙인다. 넘치면 잘린 채로 멈춘다(C++11 — 가변 람다를 안 쓴다).
namespace {
struct Appender {
    char*  buf;
    size_t cap;
    size_t len;
    void put(const char* fmt, ...) {
        if (cap == 0 || len + 1 >= cap) return;
        va_list ap;
        va_start(ap, fmt);
        int w = vsnprintf(buf + len, cap - len, fmt, ap);
        va_end(ap);
        if (w > 0) len = (len + (size_t)w < cap) ? len + (size_t)w : cap - 1;
    }
};
}  // namespace

size_t formatStatus(const Session& s, uint32_t now, char* buf, size_t cap) {
    const Session::Stats&  st = s.stats();
    const Decoder::Stats&  rx = s.rxStats();
    Appender a{buf, cap, 0};
#define put(...) a.put(__VA_ARGS__)
    if (s.up()) {
        const uint32_t sec = (uint32_t)(now - st.lastUpAt) / 1000;
        put("  state        : UP (%lum %02lus)\n",
            (unsigned long)(sec / 60), (unsigned long)(sec % 60));
    } else {
        put("  state        : DOWN\n");
    }
    if (s.peerProto())
        put("  peer         : fw %s  hw %s  proto %u (ours %u)\n",
            s.peerFw()[0] ? s.peerFw() : "?", s.peerHw()[0] ? s.peerHw() : "?",
            (unsigned)s.peerProto(), (unsigned)KPLINK_PROTO_VERSION);
    else
        put("  peer         : (HELLO 미수신)\n");
    put("  frames tx/rx : %lu / %lu\n", (unsigned long)st.txFrames, (unsigned long)st.rxFrames);
    put("  up/down      : %lu / %lu  (timeout %lu, peer-restart %lu, peer-lost %lu)\n",
        (unsigned long)st.ups, (unsigned long)st.downs, (unsigned long)st.timeouts,
        (unsigned long)st.peerRestarts, (unsigned long)st.peerLost);
    put("  rx errors    : crc %lu, len %lu, dropped %lu, expired %lu\n",
        (unsigned long)rx.crcErr, (unsigned long)rx.lenErr,
        (unsigned long)rx.dropped, (unsigned long)rx.expired);
    if (st.protoMismatch) put("  ⚠️ 규약 불일치 HELLO: %lu\n", (unsigned long)st.protoMismatch);
#undef put
    return a.len;
}

}  // namespace KpLink
