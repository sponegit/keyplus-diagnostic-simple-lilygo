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

namespace Sublink {

static HardwareSerial     s_uart(2);
static KpLink::Session    s_sess;
static uint32_t           s_lastMismatchLogAt = 0;
static uint32_t           s_mismatchSeen      = 0;

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
    }
}

static void onFrame(const KpLink::Frame &f, void *)
{
    // S2 에는 앱 프레임 소비자가 없다. S3(LOG·CONSOLE_IN)·S4(차키)부터 여기서 분기한다.
    LOGD(DbgConsole, "[LINK] 미처리 프레임 type=0x%02X seq=%u len=%u\n",
         f.type, (unsigned)f.seq, (unsigned)f.len);
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
