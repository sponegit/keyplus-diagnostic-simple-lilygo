// KpPower 전원 측정값·저전압 상태머신 단위 테스트 — pio test -e native
#include <unity.h>

#include <string.h>

#include <KpPower.h>

using namespace KpPower;

void setUp() {}
void tearDown() {}

// HARDWARE §2.3 값
static const UvParams P = {12400, 11800, 13200, 50, 60000, 60000, 10000, 5000};

// from~to(포함) 를 1초 간격으로 mv 로 넣는다. 마지막 시각을 돌려준다.
static uint32_t feed(UvMachine& m, uint32_t from, uint32_t to, uint16_t mv) {
    for (uint32_t t = from; t <= to; t += 1000) m.update(t, mv);
    return to;
}

void test_roundtrip() {
    Values v = {12517, -45, (uint8_t)Uv::WARN};
    uint8_t buf[KPPOWER_PAYLOAD_LEN];
    TEST_ASSERT_EQUAL(KPPOWER_PAYLOAD_LEN, encode(v, buf, sizeof(buf)));
    Values d;
    TEST_ASSERT_TRUE(decode(buf, sizeof(buf), &d));
    TEST_ASSERT_EQUAL_UINT16(12517, d.mv);
    TEST_ASSERT_EQUAL_INT16(-45, d.ma);
    TEST_ASSERT_EQUAL_UINT8(1, d.uv);
}

void test_codec_lengths() {
    uint8_t buf[KPPOWER_PAYLOAD_LEN + 3] = {};
    Values v = {12000, 100, 0};
    TEST_ASSERT_EQUAL(0, encode(v, buf, KPPOWER_PAYLOAD_LEN - 1));
    encode(v, buf, sizeof(buf));
    Values d;
    TEST_ASSERT_FALSE(decode(buf, KPPOWER_PAYLOAD_LEN - 1, &d));
    TEST_ASSERT_TRUE(decode(buf, sizeof(buf), &d));   // 뒤에 덧붙은 필드는 무시
}

void test_json() {
    char out[KPPOWER_JSON_MAX];
    Values v = {12517, 45, 0};
    TEST_ASSERT_TRUE(formatJson(v, out, sizeof(out)) > 0);
    TEST_ASSERT_EQUAL_STRING("{\"v\":12.52,\"i\":0.045,\"uv\":0}", out);
    v = {9004, -1234, 2};
    formatJson(v, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("{\"v\":9.00,\"i\":-1.234,\"uv\":2}", out);
    v = {65535, -32768, 2};   // 최대 자릿수도 들어간다
    TEST_ASSERT_TRUE(formatJson(v, out, sizeof(out)) > 0);
    TEST_ASSERT_EQUAL(0, formatJson(v, out, 10));   // 잘리면 0
}

void test_crank_ignored() {
    UvMachine m(P);
    feed(m, 0, 10000, 12600);
    feed(m, 11000, 12000, 9000);    // 크랭킹 2초
    feed(m, 13000, 200000, 13800);  // 알터네이터
    TEST_ASSERT_EQUAL(Uv::NORMAL, m.state());
}

void test_warn_after_60s() {
    UvMachine m(P);
    feed(m, 0, 59000, 12300);
    TEST_ASSERT_EQUAL(Uv::NORMAL, m.state());
    TEST_ASSERT_TRUE(m.update(60000, 12300));
    TEST_ASSERT_EQUAL(Uv::WARN, m.state());
}

void test_warn_then_cut() {
    UvMachine m(P);
    feed(m, 0, 60000, 12300);
    TEST_ASSERT_EQUAL(Uv::WARN, m.state());
    feed(m, 61000, 120000, 11700);
    TEST_ASSERT_EQUAL(Uv::WARN, m.state());   // 11.8 밑으로 간 지 59초
    m.update(121000, 11700);
    TEST_ASSERT_EQUAL(Uv::CUT, m.state());
}

void test_direct_drop_goes_to_cut() {
    UvMachine m(P);
    feed(m, 0, 60000, 11500);
    TEST_ASSERT_EQUAL(Uv::CUT, m.state());   // 두 타이머가 같이 끝나면 CUT 이 이긴다
}

void test_recover_needs_13v2_for_10s() {
    UvMachine m(P);
    feed(m, 0, 60000, 12300);
    feed(m, 61000, 200000, 12900);   // 12.4 위지만 13.2 아래 — 그대로 경고
    TEST_ASSERT_EQUAL(Uv::WARN, m.state());
    feed(m, 201000, 209000, 13300);
    TEST_ASSERT_EQUAL(Uv::WARN, m.state());
    m.update(211000, 13300);
    TEST_ASSERT_EQUAL(Uv::NORMAL, m.state());
}

void test_recover_from_cut() {
    UvMachine m(P);
    feed(m, 0, 60000, 11500);
    TEST_ASSERT_EQUAL(Uv::CUT, m.state());
    feed(m, 61000, 71000, 13800);
    TEST_ASSERT_EQUAL(Uv::NORMAL, m.state());
}

void test_recover_dip_restarts_timer() {
    UvMachine m(P);
    feed(m, 0, 60000, 12300);
    feed(m, 61000, 68000, 13500);
    m.update(69000, 13000);           // 13.2 − band 아래로 — 리셋
    feed(m, 70000, 78000, 13500);
    TEST_ASSERT_EQUAL(Uv::WARN, m.state());
    feed(m, 79000, 80000, 13500);
    TEST_ASSERT_EQUAL(Uv::NORMAL, m.state());
}

void test_noise_near_threshold_keeps_timer() {
    UvMachine m(P);
    for (uint32_t t = 0; t <= 60000; t += 1000) m.update(t, (t / 1000) % 2 ? 12420 : 12380);
    TEST_ASSERT_EQUAL(Uv::WARN, m.state());   // 12.42 는 12.4 + band(50mV) 밑이라 리셋 안 됨
}

void test_clear_rise_resets_timer() {
    UvMachine m(P);
    feed(m, 0, 50000, 12300);
    m.update(51000, 12500);
    feed(m, 52000, 100000, 12300);
    TEST_ASSERT_EQUAL(Uv::NORMAL, m.state());   // 52초부터 다시 셈 — 48초째
}

void test_gap_discards_timer() {
    UvMachine m(P);
    feed(m, 0, 30000, 12300);
    m.update(90000, 12300);   // 60초 끊겼다 돌아옴
    TEST_ASSERT_EQUAL(Uv::NORMAL, m.state());
    feed(m, 91000, 149000, 12300);
    TEST_ASSERT_EQUAL(Uv::NORMAL, m.state());
    m.update(150000, 12300);
    TEST_ASSERT_EQUAL(Uv::WARN, m.state());
}

void test_pending() {
    UvMachine m(P);
    Uv next;
    uint32_t left;
    m.update(0, 12600);
    TEST_ASSERT_FALSE(m.pending(0, &next, &left));
    feed(m, 1000, 21000, 12300);
    TEST_ASSERT_TRUE(m.pending(21000, &next, &left));
    TEST_ASSERT_EQUAL(Uv::WARN, next);
    TEST_ASSERT_EQUAL_UINT32(40000, left);
    m.update(22000, 11700);
    TEST_ASSERT_TRUE(m.pending(22000, &next, &left));
    TEST_ASSERT_EQUAL(Uv::CUT, next);   // 더 심한 쪽을 보여 준다
}

void test_wraparound() {
    UvMachine m(P);
    const uint32_t base = 0xFFFFFFFFu - 30000;
    for (uint32_t i = 0; i <= 60; i++) m.update(base + i * 1000, 12300);
    TEST_ASSERT_EQUAL(Uv::WARN, m.state());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_codec_lengths);
    RUN_TEST(test_json);
    RUN_TEST(test_crank_ignored);
    RUN_TEST(test_warn_after_60s);
    RUN_TEST(test_warn_then_cut);
    RUN_TEST(test_direct_drop_goes_to_cut);
    RUN_TEST(test_recover_needs_13v2_for_10s);
    RUN_TEST(test_recover_from_cut);
    RUN_TEST(test_recover_dip_restarts_timer);
    RUN_TEST(test_noise_near_threshold_keeps_timer);
    RUN_TEST(test_clear_rise_resets_timer);
    RUN_TEST(test_gap_discards_timer);
    RUN_TEST(test_pending);
    RUN_TEST(test_wraparound);
    return UNITY_END();
}
