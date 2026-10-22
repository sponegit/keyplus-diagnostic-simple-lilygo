/**
 * @file      sublink.cpp
 * @brief     v2 서브 링크 구현 — UART2 ↔ KpLink::Session. 설계는 sublink.h·작업서 S2.
 */
#include "sublink.h"
#include "config.h"

#if FEATURE_SUBLINK

#include <KpLinkSession.h>
#include "console.h"
#include "log.h"
#if FEATURE_CARKEY
#include "carkey.h"
#endif

namespace Sublink {

static HardwareSerial     s_uart(2);
static KpLink::Session    s_sess;
static uint32_t           s_lastMismatchLogAt = 0;
static uint32_t           s_mismatchSeen      = 0;

// --- 콘솔 중계 스트림 (S3) ----------------------------------------------------
// ⚠️ 잠금이 없다 — 로그는 loop 태스크에서만 쓴다는 전제다(LED Ticker 는 출력하지 않는다).
//    다른 태스크·타이머 콜백에서 DbgConsole 로 찍기 시작하면 줄 버퍼와 Session 이 경합한다.
// 로그 한 줄의 상한. 넘으면 끊어서 보낸다(LOG payload 는 텍스트 조각일 뿐 의미 단위가 아니다).
static const size_t LOG_LINE_MAX = 200;

class LinkConsole : public Stream {
public:
    size_t write(uint8_t c) override
    {
        if (c == '\r') return 1;
        if (c == '\n') { flushLine(); return 1; }
        line_[len_++] = (char)c;
        if (len_ >= LOG_LINE_MAX) flushLine();
        return 1;
    }
    size_t write(const uint8_t *buf, size_t n) override
    {
        for (size_t i = 0; i < n; i++) write(buf[i]);
        return n;
    }
    int  available() override { return (int)(inHead_ - inTail_); }
    int  read() override
    {
        if (inHead_ == inTail_) return -1;
        return in_[inTail_++ % sizeof(in_)];
    }
    int  peek() override { return (inHead_ == inTail_) ? -1 : in_[inTail_ % sizeof(in_)]; }
    void flush() override {}

    // CONSOLE_IN 프레임 수신 — 입력 링에 넣는다. 넘치면 나머지를 버린다.
    void pushInput(const uint8_t *p, size_t n)
    {
        for (size_t i = 0; i < n; i++) {
            if (inHead_ - inTail_ >= sizeof(in_)) { inDropped_++; continue; }
            in_[inHead_++ % sizeof(in_)] = p[i];
        }
    }

    uint32_t sent() const { return sent_; }
    uint32_t droppedDown() const { return droppedDown_; }
    uint32_t droppedBusy() const { return droppedBusy_; }
    uint32_t inDropped() const { return inDropped_; }

private:
    void flushLine()
    {
        if (len_ == 0) return;
        const size_t n = len_;
        len_ = 0;
        if (!s_sess.up()) { droppedDown_++; return; }   // 링크 다운 — 쌓아 둘 곳이 없다

        // 혼잡으로 버린 줄이 있으면 이번 줄 앞에 알린다(서브 화면에서 누락을 알 수 있게).
        char    pre[40];
        size_t  pl = 0;
        if (pendingBusy_) {
            pl = (size_t)snprintf(pre, sizeof(pre), "[...%lu줄 드롭] ", (unsigned long)pendingBusy_);
            if (pl >= sizeof(pre)) pl = sizeof(pre) - 1;
        }
        // 송신 버퍼에 이 프레임이 다 들어갈 자리가 없으면 버린다 — 블로킹 쓰기로 loop 를
        // 붙잡거나 뒤따를 차키 프레임을 밀리게 하지 않는다.
        const size_t frameLen = KPLINK_HEADER_LEN + pl + n + KPLINK_CRC_LEN;
        if ((size_t)s_uart.availableForWrite() < frameLen) {
            droppedBusy_++;
            pendingBusy_++;
            return;
        }
        uint8_t payload[sizeof(pre) + LOG_LINE_MAX];
        memcpy(payload, pre, pl);
        memcpy(payload + pl, line_, n);
        if (s_sess.send(KpLink::LOG, payload, (uint16_t)(pl + n)) >= 0) {
            sent_++;
            pendingBusy_ = 0;
        }
    }

    char     line_[LOG_LINE_MAX];
    size_t   len_ = 0;
    uint8_t  in_[256];
    uint32_t inHead_ = 0, inTail_ = 0;
    uint32_t sent_ = 0, droppedDown_ = 0, droppedBusy_ = 0, pendingBusy_ = 0, inDropped_ = 0;
};

static LinkConsole &linkConsole()
{
    // 함수 지역 static — console.cpp 의 전역 TeeStream 이 정적 초기화 중에 참조해도 안전하다.
    static LinkConsole c;
    return c;
}

Stream &console() { return linkConsole(); }

static size_t writeUart(const uint8_t *data, size_t n, void *)
{
    return s_uart.write(data, n);
}

static void onState(bool isUp, KpLink::Session::Reason why, void *)
{
    // 링크 전이는 앱 상태 전이 — 평상시(INFO)에도 남긴다. 현장에서 "차키가 왜 안 됐나"의 첫 단서.
    if (isUp) {
        LOGI(DbgConsole, "[LINK] 서브 연결됨 — fw %s hw %s\n", s_sess.peerFw(), s_sess.peerHw());
        if (strcmp(s_sess.peerHw(), SUBLINK_PEER_HW) != 0) {
            LOGW(DbgConsole, "[LINK] ⚠️ 상대 hw '%s' — 기대값 '%s' 아님(배선·보드 확인)\n",
                 s_sess.peerHw(), SUBLINK_PEER_HW);
        }
    } else {
        LOGW(DbgConsole, "[LINK] 서브 연결 끊김 (%s)\n", KpLink::reasonName(why));
#if FEATURE_CARKEY
        Carkey::linkLost();   // 진행 중 차키 요청은 여기서 실패로 끝낸다
#endif
    }
}

static void onFrame(const KpLink::Frame &f, void *)
{
    switch (f.type) {
    case KpLink::CONSOLE_IN:
        // 서브 콘솔 'm <명령>' — 메인 콘솔 입력으로. Prov::handleSerial 이 DbgConsole 에서 읽는다.
        linkConsole().pushInput(f.payload, f.len);
        break;
#if FEATURE_CARKEY
    case KpLink::KEY_RESULT:
        Carkey::onResult(f.payload, f.len);
        break;
#endif
    default:
        LOGD(DbgConsole, "[LINK] 미처리 프레임 type=0x%02X seq=%u len=%u\n",
             f.type, (unsigned)f.seq, (unsigned)f.len);
        break;
    }
}

void begin()
{
    // 버퍼 크기는 begin() 전에 정해야 적용된다.
    s_uart.setRxBufferSize(SUBLINK_RX_BUF);
    s_uart.setTxBufferSize(SUBLINK_TX_BUF);
    // Arduino-ESP32 시그니처는 (baud, config, rxPin, txPin) 순이다.
    s_uart.begin(SUBLINK_BAUD, SERIAL_8N1, SUBLINK_RX_PIN, SUBLINK_TX_PIN);

    s_sess.onState(onState);
    s_sess.onFrame(onFrame);
    s_sess.begin(KpLink::Role::MAIN, FW_VERSION, HW_MODEL, writeUart, nullptr, millis());
    LOGD(DbgConsole, "[LINK] UART2 TX=GPIO%d RX=GPIO%d @%d, proto %d\n",
         SUBLINK_TX_PIN, SUBLINK_RX_PIN, SUBLINK_BAUD, KPLINK_PROTO_VERSION);
}

void poll()
{
    // 수신 먼저 — loop 가 오래 막혔다 풀리면 쌓인 프레임부터 처리해야 타임아웃 오판이 없다.
    uint8_t buf[256];
    int avail;
    while ((avail = s_uart.available()) > 0) {
        size_t n = s_uart.readBytes(buf, avail > (int)sizeof(buf) ? sizeof(buf) : (size_t)avail);
        if (n == 0) break;
        s_sess.onBytes(buf, n, millis());
    }
    const uint32_t now = millis();
    s_sess.tick(now);

    // 규약 불일치는 HELLO 마다(1초) 쌓인다 — 처음 한 번과 이후 1분마다만 알린다.
    const uint32_t mm = s_sess.stats().protoMismatch;
    if (mm != s_mismatchSeen &&
        (s_mismatchSeen == 0 || (uint32_t)(now - s_lastMismatchLogAt) >= 60000UL)) {
        LOGE(DbgConsole, "[LINK] 규약 버전 불일치 — 서브 %u / 메인 %u. 링크를 올리지 않는다"
                         "(같은 커밋의 메인·서브를 함께 올릴 것)\n",
             (unsigned)s_sess.peerProto(), (unsigned)KPLINK_PROTO_VERSION);
        s_lastMismatchLogAt = now;
        s_mismatchSeen      = mm;
    }
}

bool up() { return s_sess.up(); }

const char *peerFw() { return s_sess.peerFw(); }

bool sendFrame(uint8_t type, const uint8_t *payload, uint16_t len)
{
    if (!s_sess.up()) return false;
    return s_sess.send(type, payload, len) >= 0;
}

bool requestReset()
{
    if (!s_sess.up()) return false;
    return s_sess.send(KpLink::RESET, nullptr, 0) >= 0;
}

void printStatus(Stream &io)
{
    char buf[512];
    KpLink::formatStatus(s_sess, millis(), buf, sizeof(buf));
    io.println("[LINK] 서브 링크");
    io.print(buf);
    const LinkConsole &c = linkConsole();
    io.printf("  log relay    : sent %lu, drop(down) %lu, drop(busy) %lu, input drop %lu\n",
              (unsigned long)c.sent(), (unsigned long)c.droppedDown(),
              (unsigned long)c.droppedBusy(), (unsigned long)c.inDropped());
}

bool tryConsole(const String &cmd, const String &arg, Stream &io)
{
    if (cmd != "sub") return false;
    String a = arg; a.trim(); a.toLowerCase();
    if (a == "reset") {
        if (requestReset()) io.println("[LINK] 서브에 재부팅 요청을 보냈다 — 수 초 뒤 다시 연결된다");
        else                io.println("[LINK] 링크가 내려가 있어 요청을 보낼 수 없다");
        return true;
    }
    io.println("[LINK] 사용법: sub reset");
    return true;
}

} // namespace Sublink

#endif  // FEATURE_SUBLINK
