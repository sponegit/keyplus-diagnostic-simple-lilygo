#include "KpLink.h"

#include <string.h>

namespace KpLink {

uint16_t crc16(const uint8_t* data, size_t len, uint16_t crc) {
    while (len--) {
        crc ^= (uint16_t)(*data++) << 8;
        for (uint8_t i = 0; i < 8; i++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

size_t encode(uint8_t type, uint8_t seq, const uint8_t* payload, uint16_t len,
              uint8_t* out, size_t outCap) {
    const size_t total = KPLINK_HEADER_LEN + (size_t)len + KPLINK_CRC_LEN;
    if (len > KPLINK_MAX_PAYLOAD || total > outCap) return 0;
    if (len > 0 && payload == nullptr) return 0;

    out[0] = KPLINK_SYNC0;
    out[1] = KPLINK_SYNC1;
    out[2] = type;
    out[3] = seq;
    out[4] = (uint8_t)(len & 0xFF);
    out[5] = (uint8_t)(len >> 8);
    if (len) memcpy(out + KPLINK_HEADER_LEN, payload, len);

    const uint16_t crc = crc16(out + 2, (KPLINK_HEADER_LEN - 2) + (size_t)len);
    out[KPLINK_HEADER_LEN + len]     = (uint8_t)(crc & 0xFF);
    out[KPLINK_HEADER_LEN + len + 1] = (uint8_t)(crc >> 8);
    return total;
}

// --- Decoder ----------------------------------------------------------------

void Decoder::reset() {
    len_     = 0;
    consume_ = 0;
    stats_   = {};
}

// 직전에 내준 프레임을 지운다. payload 포인터 유효기간이 여기서 끝난다.
void Decoder::compact() {
    if (consume_ == 0) return;
    dropFront(consume_);
    consume_ = 0;
}

void Decoder::dropFront(size_t n) {
    if (n >= len_) { len_ = 0; return; }
    memmove(buf_, buf_ + n, len_ - n);
    len_ -= n;
}

size_t Decoder::feed(const uint8_t* data, size_t n) {
    compact();
    const size_t room = sizeof(buf_) - len_;
    if (n > room) n = room;
    memcpy(buf_ + len_, data, n);
    len_ += n;
    return n;
}

void Decoder::expire() {
    compact();
    if (len_ == 0) return;
    dropFront(1);
    stats_.expired++;
}

bool Decoder::next(Frame& out) {
    compact();
    for (;;) {
        // 1) 싱크 찾기 — 0xAA 가 아닌 선두 바이트는 버린다.
        size_t skip = 0;
        while (skip < len_ && buf_[skip] != KPLINK_SYNC0) skip++;
        if (skip) { dropFront(skip); stats_.dropped += skip; }
        if (len_ < 2) return false;
        if (buf_[1] != KPLINK_SYNC1) { dropFront(1); stats_.dropped++; continue; }

        // 2) 헤더
        if (len_ < KPLINK_HEADER_LEN) return false;
        const uint16_t plen = (uint16_t)buf_[4] | ((uint16_t)buf_[5] << 8);
        if (plen > KPLINK_MAX_PAYLOAD) { dropFront(1); stats_.lenErr++; continue; }

        // 3) 본문 + CRC 가 다 올 때까지 기다린다.
        const size_t total = KPLINK_HEADER_LEN + (size_t)plen + KPLINK_CRC_LEN;
        if (len_ < total) return false;

        const uint16_t want = (uint16_t)buf_[KPLINK_HEADER_LEN + plen] |
                              ((uint16_t)buf_[KPLINK_HEADER_LEN + plen + 1] << 8);
        const uint16_t got  = crc16(buf_ + 2, (KPLINK_HEADER_LEN - 2) + (size_t)plen);
        if (want != got) { dropFront(1); stats_.crcErr++; continue; }

        out.type    = buf_[2];
        out.seq     = buf_[3];
        out.len     = plen;
        out.payload = buf_ + KPLINK_HEADER_LEN;
        consume_    = total;   // 다음 호출 때 지운다 — 그때까지 payload 유효
        stats_.frames++;
        return true;
    }
}

}  // namespace KpLink
