// KpLink 링크 코덱 단위 테스트 — pio test -e native
#include <unity.h>

#include <string.h>
#include <vector>

#include <KpLink.h>

using namespace KpLink;

void setUp() {}
void tearDown() {}

// --- 도우미 ------------------------------------------------------------------

static std::vector<uint8_t> makeFrame(uint8_t type, uint8_t seq,
                                      const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out(KPLINK_MAX_FRAME);
    size_t n = encode(type, seq, payload.empty() ? nullptr : payload.data(),
                      (uint16_t)payload.size(), out.data(), out.size());
    TEST_ASSERT_NOT_EQUAL(0, n);
    out.resize(n);
    return out;
}

static void append(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

// 스트림 전체를 chunk 크기로 쪼개 넣으며 나오는 프레임을 모은다.
struct Got {
    uint8_t type, seq;
    std::vector<uint8_t> payload;
};
static std::vector<Got> decodeAll(Decoder& d, const std::vector<uint8_t>& stream,
                                  size_t chunk) {
    std::vector<Got> got;
    size_t pos = 0;
    Frame f;
    while (pos < stream.size()) {
        size_t n = stream.size() - pos;
        if (n > chunk) n = chunk;
        pos += d.feed(stream.data() + pos, n);
        while (d.next(f)) {
            got.push_back({f.type, f.seq, std::vector<uint8_t>(f.payload, f.payload + f.len)});
        }
    }
    return got;
}

// --- CRC ---------------------------------------------------------------------

void test_crc16_ccitt_false_check_value() {
    // CRC-16/CCITT-FALSE 표준 검사값
    const uint8_t s[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    TEST_ASSERT_EQUAL_HEX16(0x29B1, crc16(s, sizeof(s)));
}

void test_crc16_continues_from_previous() {
    const uint8_t s[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    uint16_t part = crc16(s, 4);
    TEST_ASSERT_EQUAL_HEX16(0x29B1, crc16(s + 4, 5, part));
}

// --- 인코더 ------------------------------------------------------------------

void test_encode_layout() {
    const uint8_t p[] = {0x11, 0x22, 0x33};
    uint8_t out[32];
    size_t n = encode(KEY_CMD, 7, p, sizeof(p), out, sizeof(out));
    TEST_ASSERT_EQUAL(KPLINK_HEADER_LEN + 3 + KPLINK_CRC_LEN, n);
    TEST_ASSERT_EQUAL_HEX8(0xAA, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x55, out[1]);
    TEST_ASSERT_EQUAL_HEX8(KEY_CMD, out[2]);
    TEST_ASSERT_EQUAL(7, out[3]);
    TEST_ASSERT_EQUAL(3, out[4]);   // len LE
    TEST_ASSERT_EQUAL(0, out[5]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(p, out + 6, 3);
    uint16_t crc = crc16(out + 2, 4 + 3);
    TEST_ASSERT_EQUAL_HEX8(crc & 0xFF, out[9]);
    TEST_ASSERT_EQUAL_HEX8(crc >> 8, out[10]);
}

void test_encode_rejects_oversize_and_small_buffer() {
    static uint8_t big[KPLINK_MAX_PAYLOAD + 1];
    static uint8_t out[KPLINK_MAX_FRAME + 16];
    TEST_ASSERT_EQUAL(0, encode(LOG, 0, big, KPLINK_MAX_PAYLOAD + 1, out, sizeof(out)));
    TEST_ASSERT_NOT_EQUAL(0, encode(LOG, 0, big, KPLINK_MAX_PAYLOAD, out, sizeof(out)));
    uint8_t tiny[8];
    const uint8_t p[] = {1};
    TEST_ASSERT_EQUAL(0, encode(LOG, 0, p, 1, tiny, sizeof(tiny)));   // 9바이트 필요
    TEST_ASSERT_EQUAL(0, encode(LOG, 0, nullptr, 1, out, sizeof(out)));
}

// --- 디코더 ------------------------------------------------------------------

void test_roundtrip_whole_and_bytewise() {
    std::vector<uint8_t> p = {1, 2, 3, 4, 5};
    auto fr = makeFrame(HEARTBEAT, 42, p);
    for (size_t chunk : {fr.size(), (size_t)1, (size_t)3}) {
        Decoder d;
        auto got = decodeAll(d, fr, chunk);
        TEST_ASSERT_EQUAL(1, got.size());
        TEST_ASSERT_EQUAL_HEX8(HEARTBEAT, got[0].type);
        TEST_ASSERT_EQUAL(42, got[0].seq);
        TEST_ASSERT_TRUE(got[0].payload == p);
        TEST_ASSERT_EQUAL(0, d.pending());
    }
}

void test_empty_payload() {
    Decoder d;
    auto got = decodeAll(d, makeFrame(RESET, 0, {}), 64);
    TEST_ASSERT_EQUAL(1, got.size());
    TEST_ASSERT_EQUAL(0, got[0].payload.size());
}

void test_max_payload_roundtrip() {
    std::vector<uint8_t> p(KPLINK_MAX_PAYLOAD);
    for (size_t i = 0; i < p.size(); i++) p[i] = (uint8_t)(i * 7);
    Decoder d;
    auto got = decodeAll(d, makeFrame(OTA_CHUNK, 9, p), 100);
    TEST_ASSERT_EQUAL(1, got.size());
    TEST_ASSERT_TRUE(got[0].payload == p);
}

void test_back_to_back_frames_in_one_feed() {
    std::vector<uint8_t> s;
    append(s, makeFrame(LOG, 1, {'a'}));
    append(s, makeFrame(LOG, 2, {'b', 'c'}));
    append(s, makeFrame(POWER, 3, {}));
    Decoder d;
    auto got = decodeAll(d, s, s.size());
    TEST_ASSERT_EQUAL(3, got.size());
    TEST_ASSERT_EQUAL(1, got[0].seq);
    TEST_ASSERT_EQUAL(2, got[1].seq);
    TEST_ASSERT_EQUAL_HEX8(POWER, got[2].type);
    TEST_ASSERT_EQUAL(3, d.stats().frames);
}

void test_sync_bytes_inside_payload() {
    std::vector<uint8_t> p = {0xAA, 0x55, 0xAA, 0x55, 0x00, 0xAA};
    std::vector<uint8_t> s;
    append(s, makeFrame(AIR, 5, p));
    append(s, makeFrame(AIR, 6, p));
    Decoder d;
    auto got = decodeAll(d, s, 2);
    TEST_ASSERT_EQUAL(2, got.size());
    TEST_ASSERT_TRUE(got[0].payload == p);
    TEST_ASSERT_TRUE(got[1].payload == p);
}

void test_leading_garbage_like_rom_boot_log() {
    // 서브 리셋 시 ROM 이 UART0 에 찍는 부트 로그 + 잡음 바이트
    const char* rom = "ESP-ROM:esp32s3-20210327\r\nrst:0x1 (POWERON),boot:0x8\r\n\xAA\x00\xAA\xAA";
    std::vector<uint8_t> s(rom, rom + strlen(rom));
    append(s, makeFrame(HELLO, 0, {2, 0, 1}));
    Decoder d;
    auto got = decodeAll(d, s, 7);
    TEST_ASSERT_EQUAL(1, got.size());
    TEST_ASSERT_EQUAL_HEX8(HELLO, got[0].type);
    TEST_ASSERT_TRUE(d.stats().dropped > 0);
}

void test_crc_error_then_valid_frame() {
    auto bad = makeFrame(KEY_CMD, 1, {1, 2, 3});
    bad[7] ^= 0x01;   // payload 1비트 뒤집기
    std::vector<uint8_t> s = bad;
    append(s, makeFrame(KEY_CMD, 2, {4, 5, 6}));
    Decoder d;
    auto got = decodeAll(d, s, s.size());
    TEST_ASSERT_EQUAL(1, got.size());
    TEST_ASSERT_EQUAL(2, got[0].seq);
    TEST_ASSERT_EQUAL(1, d.stats().crcErr);
}

void test_oversize_len_header_is_skipped() {
    std::vector<uint8_t> s = {0xAA, 0x55, 0x10, 0x00, 0xFF, 0xFF};   // len 65535
    append(s, makeFrame(LOG, 3, {'x'}));
    Decoder d;
    auto got = decodeAll(d, s, s.size());
    TEST_ASSERT_EQUAL(1, got.size());
    TEST_ASSERT_EQUAL(3, got[0].seq);
    TEST_ASSERT_EQUAL(1, d.stats().lenErr);
}

void test_real_frame_hidden_inside_false_header() {
    // 가짜 헤더(len=3)가 진짜 프레임 앞부분을 자기 본문으로 삼는다 → CRC 실패 →
    // 선두 1바이트만 버리고 다시 찾아야 진짜 프레임을 건진다.
    std::vector<uint8_t> s = {0xAA, 0x55, 0x01, 0x00, 0x03, 0x00};
    append(s, makeFrame(KEY_RESULT, 77, {9, 8, 7, 6}));
    Decoder d;
    auto got = decodeAll(d, s, s.size());
    TEST_ASSERT_EQUAL(1, got.size());
    TEST_ASSERT_EQUAL_HEX8(KEY_RESULT, got[0].type);
    TEST_ASSERT_EQUAL(77, got[0].seq);
}

void test_expire_releases_stalled_false_header() {
    // 가짜 헤더가 len=1000 을 주장 — 뒤 진짜 프레임이 묶여 나오지 않는다.
    std::vector<uint8_t> s = {0xAA, 0x55, 0x01, 0x00, 0xE8, 0x03};
    append(s, makeFrame(HEARTBEAT, 1, {}));
    Decoder d;
    TEST_ASSERT_EQUAL(s.size(), d.feed(s.data(), s.size()));
    Frame f;
    TEST_ASSERT_FALSE(d.next(f));
    TEST_ASSERT_TRUE(d.pending() > 0);
    d.expire();   // 전송 계층이 "바이트 공백" 타임아웃 때 부른다
    TEST_ASSERT_TRUE(d.next(f));
    TEST_ASSERT_EQUAL_HEX8(HEARTBEAT, f.type);
    TEST_ASSERT_EQUAL(1, d.stats().expired);
}

void test_payload_valid_until_next_call() {
    std::vector<uint8_t> s;
    append(s, makeFrame(LOG, 1, {'A', 'B'}));
    append(s, makeFrame(LOG, 2, {'C', 'D'}));
    Decoder d;
    d.feed(s.data(), s.size());
    Frame f1;
    TEST_ASSERT_TRUE(d.next(f1));
    TEST_ASSERT_EQUAL('A', f1.payload[0]);   // 다음 호출 전 — 유효
    Frame f2;
    TEST_ASSERT_TRUE(d.next(f2));
    TEST_ASSERT_EQUAL('C', f2.payload[0]);
}

void test_stress_noise_and_buffer_limit() {
    // 결정적 의사난수(LCG) — 잡음 사이사이에 프레임 200개. 피드 버퍼를 넘치게 넣어도
    // feed/next 반복으로 전부 회수돼야 한다.
    uint32_t x = 12345;
    auto rnd = [&x]() { x = x * 1103515245u + 12345u; return (uint8_t)(x >> 16); };
    std::vector<uint8_t> s;
    const int N = 200;
    for (int i = 0; i < N; i++) {
        int noise = rnd() % 20;
        for (int k = 0; k < noise; k++) s.push_back(rnd());
        std::vector<uint8_t> p(rnd() % 64);
        for (auto& b : p) b = rnd();
        append(s, makeFrame(GNSS, (uint8_t)i, p));
    }
    Decoder d;
    auto got = decodeAll(d, s, 4096);   // 버퍼(2 * MAX_FRAME)보다 큰 덩어리
    // 잡음이 우연히 유효 프레임이 될 확률은 무시할 수준 — 진짜 프레임은 하나도 잃으면 안 된다.
    int seqHits = 0;
    for (auto& g : got) if (g.type == GNSS && g.seq == (uint8_t)seqHits) seqHits++;
    TEST_ASSERT_EQUAL(N, seqHits);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_crc16_ccitt_false_check_value);
    RUN_TEST(test_crc16_continues_from_previous);
    RUN_TEST(test_encode_layout);
    RUN_TEST(test_encode_rejects_oversize_and_small_buffer);
    RUN_TEST(test_roundtrip_whole_and_bytewise);
    RUN_TEST(test_empty_payload);
    RUN_TEST(test_max_payload_roundtrip);
    RUN_TEST(test_back_to_back_frames_in_one_feed);
    RUN_TEST(test_sync_bytes_inside_payload);
    RUN_TEST(test_leading_garbage_like_rom_boot_log);
    RUN_TEST(test_crc_error_then_valid_frame);
    RUN_TEST(test_oversize_len_header_is_skipped);
    RUN_TEST(test_real_frame_hidden_inside_false_header);
    RUN_TEST(test_expire_releases_stalled_false_header);
    RUN_TEST(test_payload_valid_until_next_call);
    RUN_TEST(test_stress_noise_and_buffer_limit);
    return UNITY_END();
}
