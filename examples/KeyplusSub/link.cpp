/**
 * @file      link.cpp
 * @brief     메인 링크 구현 — UART0 ↔ KpLink::Session. 설계는 link.h·작업서 S2.
 */
#include "link.h"
#include "config.h"

#include <KpLinkSession.h>
#include "carkey.h"

namespace Link {

static HardwareSerial  &s_uart = Serial0;   // UART0 — USB-CDC 콘솔(Serial)과 별개
static KpLink::Session  s_sess;

static size_t writeUart(const uint8_t *data, size_t n, void *)
{
    return s_uart.write(data, n);
}

static void onState(bool isUp, KpLink::Session::Reason why, void *)
{
    if (isUp) {
        Serial.printf("[LINK] 메인 연결됨 — fw %s hw %s\n", s_sess.peerFw(), s_sess.peerHw());
        // 메인이 재부팅했으면 차키 요청 id 가 새로 시작한다 — 중복 판정 기억을 지운다.
        Carkey::forget();
        if (strcmp(s_sess.peerHw(), LINK_PEER_HW) != 0) {
            Serial.printf("[LINK] ⚠️ 상대 hw '%s' — 기대값 '%s' 아님(배선·보드 확인)\n",
                          s_sess.peerHw(), LINK_PEER_HW);
        }
    } else {
        // 메인이 LTE 등록으로 막히면 timeout 이 나는 게 정상이다 — 풀리면 다시 연결된다.
        Serial.printf("[LINK] 메인 연결 끊김 (%s)\n", KpLink::reasonName(why));
    }
}

static void onFrame(const KpLink::Frame &f, void *)
{
    switch (f.type) {
    case KpLink::RESET:
        Serial.println("[LINK] 메인 요청으로 재부팅");
        Serial.flush();
        delay(50);
        ESP.restart();
        break;
    case KpLink::LOG:
        // 메인 로그 한 줄. USB 미연결이면 setTxTimeoutMs(0) 덕에 막히지 않고 버려진다.
        Serial.print("[M] ");
        Serial.write(f.payload, f.len);
        Serial.print('\n');
        break;
    case KpLink::KEY_CMD:
        Carkey::onCmd(f.payload, f.len);
        break;
    default:
        Serial.printf("[LINK] 미처리 프레임 type=0x%02X seq=%u len=%u\n",
                      f.type, (unsigned)f.seq, (unsigned)f.len);
        break;
    }
}

void begin()
{
    s_uart.setRxBufferSize(LINK_RX_BUF);
    s_uart.setTxBufferSize(LINK_TX_BUF);
    s_uart.begin(LINK_BAUD, SERIAL_8N1, PIN_LINK_RX, PIN_LINK_TX);

    s_sess.onState(onState);
    s_sess.onFrame(onFrame);
    s_sess.begin(KpLink::Role::SUB, FW_VERSION, HW_MODEL, writeUart, nullptr, millis());
}

void poll()
{
    uint8_t buf[256];
    int avail;
    while ((avail = s_uart.available()) > 0) {
        size_t n = s_uart.readBytes(buf, avail > (int)sizeof(buf) ? sizeof(buf) : (size_t)avail);
        if (n == 0) break;
        s_sess.onBytes(buf, n, millis());
    }
    s_sess.tick(millis());
}

bool up() { return s_sess.up(); }

bool sendFrame(uint8_t type, const uint8_t *payload, uint16_t len)
{
    if (!s_sess.up()) return false;
    return s_sess.send(type, payload, len) >= 0;
}

bool sendConsoleLine(const char *line)
{
    if (!s_sess.up()) return false;
    uint8_t buf[128];
    size_t  n = strlen(line);
    if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
    memcpy(buf, line, n);
    buf[n++] = '\n';   // 메인 콘솔은 줄 단위로 명령을 끊는다
    return s_sess.send(KpLink::CONSOLE_IN, buf, (uint16_t)n) >= 0;
}

void printStatus(Stream &io)
{
    char buf[512];
    KpLink::formatStatus(s_sess, millis(), buf, sizeof(buf));
    io.println("[LINK] 메인 링크");
    io.print(buf);
}

} // namespace Link
