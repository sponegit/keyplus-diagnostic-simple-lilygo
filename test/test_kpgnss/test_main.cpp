// KpGnss 위치 프레임 코덱 단위 테스트 — pio test -e native
#include <unity.h>

#include <KpGnss.h>

using namespace KpGnss;

void setUp() {}
void tearDown() {}

// 2026-11-12 서브 콘솔 실측값(대전 근처)
static Values sample() {
    return make(true, 36.322426, 127.376240, 70.5f, 0.0f, 0.8f, 12, 2026, 11, 12, 0, 55, 45);
}

void test_roundtrip() {
    Values v = sample();
    uint8_t buf[KPGNSS_PAYLOAD_LEN];
    TEST_ASSERT_EQUAL(KPGNSS_PAYLOAD_LEN, encode(v, buf, sizeof(buf)));
    Values d;
    TEST_ASSERT_TRUE(decode(buf, sizeof(buf), &d));
    TEST_ASSERT_TRUE(d.fix());
    TEST_ASSERT_TRUE(d.timeOk());
    TEST_ASSERT_EQUAL_INT32(363224260, d.latE7);
    TEST_ASSERT_EQUAL_INT32(1273762400, d.lonE7);
    TEST_ASSERT_EQUAL_INT16(705, d.altDm);
    TEST_ASSERT_EQUAL_UINT8(8, d.hdopX10);
    TEST_ASSERT_EQUAL_UINT8(12, d.sat);
    TEST_ASSERT_EQUAL_UINT16(2026, d.year);
    TEST_ASSERT_EQUAL_UINT8(11, d.month);
    TEST_ASSERT_EQUAL_UINT8(12, d.day);
    TEST_ASSERT_EQUAL_UINT8(0, d.hour);
    TEST_ASSERT_EQUAL_UINT8(55, d.minute);
    TEST_ASSERT_EQUAL_UINT8(45, d.second);
}

void test_negative_coords_and_altitude() {
    // 남반구·서반구·해수면 아래
    Values v = make(true, -33.8688, -151.2093, -12.3f, 88.4f, 1.25f, 7, 2026, 1, 2, 3, 4, 5);
    uint8_t buf[KPGNSS_PAYLOAD_LEN];
    encode(v, buf, sizeof(buf));
    Values d;
    decode(buf, sizeof(buf), &d);
    TEST_ASSERT_EQUAL_INT32(-338688000, d.latE7);
    TEST_ASSERT_EQUAL_INT32(-1512093000, d.lonE7);
    TEST_ASSERT_EQUAL_INT16(-123, d.altDm);
    TEST_ASSERT_EQUAL_UINT16(884, d.spdX10);
    TEST_ASSERT_EQUAL_UINT8(13, d.hdopX10);   // 1.25 → 12.5 → 반올림 13
}

void test_codec_lengths() {
    uint8_t buf[KPGNSS_PAYLOAD_LEN + 4] = {};
    Values v = sample();
    TEST_ASSERT_EQUAL(0, encode(v, buf, KPGNSS_PAYLOAD_LEN - 1));
    encode(v, buf, sizeof(buf));
    Values d;
    TEST_ASSERT_FALSE(decode(buf, KPGNSS_PAYLOAD_LEN - 1, &d));
    TEST_ASSERT_TRUE(decode(buf, sizeof(buf), &d));   // 뒤에 덧붙은 필드는 무시
    TEST_ASSERT_EQUAL_INT32(363224260, d.latE7);
}

void test_nofix_keeps_time_and_quality() {
    // fix 가 없으면 위치는 0 이지만 위성 수·시각은 실린다(첫 fix 기다리는 동안 진단용)
    Values v = make(false, 36.3, 127.3, 70.0f, 0.0f, 99.9f, 3, 2026, 11, 12, 1, 2, 3);
    TEST_ASSERT_FALSE(v.fix());
    TEST_ASSERT_TRUE(v.timeOk());
    TEST_ASSERT_EQUAL_INT32(0, v.latE7);
    TEST_ASSERT_EQUAL_INT32(0, v.lonE7);
    TEST_ASSERT_EQUAL_UINT8(3, v.sat);
    TEST_ASSERT_EQUAL_UINT8(255, v.hdopX10);   // 25.5 초과는 255 로 자른다
}

void test_no_time() {
    Values v = make(true, 36.3, 127.3, 70.0f, 0.0f, 1.0f, 5, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_TRUE(v.fix());
    TEST_ASSERT_FALSE(v.timeOk());
}

void test_clamps() {
    Values v = make(true, 91.0, -181.0, 99999.0f, -5.0f, -1.0f, 300, 2026, 1, 1, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT32(900000000, v.latE7);
    TEST_ASSERT_EQUAL_INT32(-1800000000, v.lonE7);
    TEST_ASSERT_EQUAL_INT16(32767, v.altDm);
    TEST_ASSERT_EQUAL_UINT16(0, v.spdX10);
    TEST_ASSERT_EQUAL_UINT8(0, v.hdopX10);
    TEST_ASSERT_EQUAL_UINT8(255, v.sat);
}

void test_decode_drops_reserved_flag_bits() {
    uint8_t buf[KPGNSS_PAYLOAD_LEN];
    encode(sample(), buf, sizeof(buf));
    buf[0] |= 0xF0;   // 미래 버전이 세운 비트
    Values d;
    decode(buf, sizeof(buf), &d);
    TEST_ASSERT_EQUAL_UINT8(FIX | TIME_OK, d.flags);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_negative_coords_and_altitude);
    RUN_TEST(test_codec_lengths);
    RUN_TEST(test_nofix_keeps_time_and_quality);
    RUN_TEST(test_no_time);
    RUN_TEST(test_clamps);
    RUN_TEST(test_decode_drops_reserved_flag_bits);
    return UNITY_END();
}
