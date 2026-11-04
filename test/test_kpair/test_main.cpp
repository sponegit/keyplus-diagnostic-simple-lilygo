// KpAir 공기질 묶음 단위 테스트 — pio test -e native
#include <unity.h>

#include <string.h>

#include <KpAir.h>

using namespace KpAir;

void setUp() {}
void tearDown() {}

static Values full() {
    Values v = {};
    v.mask = T | RH | P | CO2 | VOC | NOX | PM | CO | GUARD;
    v.t10 = 235; v.rh10 = 456; v.p10 = 10132; v.co2 = 612;
    v.voc = 100; v.nox = 1; v.pm1 = 3; v.pm25 = 5; v.pm10 = 6;
    v.co10 = 12; v.guard = 0;
    return v;
}

void test_roundtrip() {
    Values v = full();
    v.t10 = -53;   // 음수 온도
    uint8_t buf[KPAIR_PAYLOAD_LEN];
    TEST_ASSERT_EQUAL(KPAIR_PAYLOAD_LEN, encode(v, buf, sizeof(buf)));
    Values d;
    TEST_ASSERT_TRUE(decode(buf, sizeof(buf), &d));
    TEST_ASSERT_EQUAL_HEX16(v.mask, d.mask);
    TEST_ASSERT_EQUAL_INT16(-53, d.t10);
    TEST_ASSERT_EQUAL_UINT16(10132, d.p10);
    TEST_ASSERT_EQUAL_UINT16(612, d.co2);
    TEST_ASSERT_EQUAL_UINT16(5, d.pm25);
    TEST_ASSERT_EQUAL_UINT16(12, d.co10);
}

void test_encode_needs_room() {
    uint8_t buf[KPAIR_PAYLOAD_LEN - 1];
    TEST_ASSERT_EQUAL(0, encode(full(), buf, sizeof(buf)));
}

void test_decode_short_and_long() {
    uint8_t buf[KPAIR_PAYLOAD_LEN + 4] = {};
    encode(full(), buf, sizeof(buf));
    Values d;
    TEST_ASSERT_FALSE(decode(buf, KPAIR_PAYLOAD_LEN - 1, &d));
    TEST_ASSERT_TRUE(decode(buf, sizeof(buf), &d));   // 뒤에 덧붙은 필드는 무시
}

void test_decode_drops_unknown_bits() {
    uint8_t buf[KPAIR_PAYLOAD_LEN];
    Values v = {};
    v.mask = 0x8000 | CO2;
    v.co2 = 800;
    encode(v, buf, sizeof(buf));
    Values d;
    decode(buf, sizeof(buf), &d);
    TEST_ASSERT_EQUAL_HEX16(CO2, d.mask);
}

void test_json_full() {
    char out[KPAIR_JSON_MAX];
    size_t n = formatJson(full(), out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(
        "{\"t\":23.5,\"rh\":45.6,\"p\":1013.2,\"co2\":612,\"voc\":100,\"nox\":1,"
        "\"pm1\":3,\"pm25\":5,\"pm10\":6,\"co\":1.2,\"guard\":0}", out);
    TEST_ASSERT_EQUAL(strlen(out), n);
}

void test_json_partial_and_negative() {
    Values v = {};
    v.mask = T | CO2;
    v.t10 = -5;
    v.co2 = 450;
    char out[64];
    formatJson(v, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("{\"t\":-0.5,\"co2\":450}", out);
}

void test_json_empty_mask_is_omitted() {
    Values v = {};
    char out[16];
    TEST_ASSERT_EQUAL(0, formatJson(v, out, sizeof(out)));
}

void test_json_worst_case_fits() {
    Values v = full();
    v.t10 = -32768; v.rh10 = v.p10 = v.co2 = v.voc = v.nox = 65535;
    v.pm1 = v.pm25 = v.pm10 = v.co10 = 65535; v.guard = 255;
    char out[KPAIR_JSON_MAX];
    TEST_ASSERT_TRUE(formatJson(v, out, sizeof(out)) > 0);
}

void test_json_truncation_returns_zero() {
    char out[20];
    TEST_ASSERT_EQUAL(0, formatJson(full(), out, sizeof(out)));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_encode_needs_room);
    RUN_TEST(test_decode_short_and_long);
    RUN_TEST(test_decode_drops_unknown_bits);
    RUN_TEST(test_json_full);
    RUN_TEST(test_json_partial_and_negative);
    RUN_TEST(test_json_empty_mask_is_omitted);
    RUN_TEST(test_json_worst_case_fits);
    RUN_TEST(test_json_truncation_returns_zero);
    return UNITY_END();
}
