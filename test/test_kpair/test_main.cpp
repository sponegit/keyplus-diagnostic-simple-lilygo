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
    v.mask |= MAX; v.co10_max = v.pm25_max = 65535;
    char out[KPAIR_JSON_MAX];
    TEST_ASSERT_TRUE(formatJson(v, out, sizeof(out)) > 0);
}

void test_json_truncation_returns_zero() {
    char out[20];
    TEST_ASSERT_EQUAL(0, formatJson(full(), out, sizeof(out)));
}

void test_decode_drops_max_bit() {
    uint8_t buf[KPAIR_PAYLOAD_LEN];
    Values v = full();
    v.mask |= MAX;
    encode(v, buf, sizeof(buf));
    Values d;
    decode(buf, sizeof(buf), &d);
    TEST_ASSERT_EQUAL_HEX16(0, d.mask & MAX);   // MAX 는 메인 쪽 값 — 프레임으로 들어오지 않는다
}

void test_json_max_fields() {
    Values v = {};
    v.mask = PM | CO | MAX;
    v.pm1 = 1; v.pm25 = 2; v.pm10 = 3; v.co10 = 15;
    v.pm25_max = 40; v.co10_max = 123;
    char out[KPAIR_JSON_MAX];
    formatJson(v, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("{\"pm1\":1,\"pm25\":2,\"pm10\":3,\"co\":1.5,\"co_max\":12.3,\"pm25_max\":40}", out);
    v.mask = MAX;   // MAX 만으로는 담을 값이 없다
    TEST_ASSERT_EQUAL(0, formatJson(v, out, sizeof(out)));
}

static Values sample(int16_t t10, uint16_t pm25, uint16_t co10, bool withPmCo) {
    Values v = {};
    v.mask = T | RH;
    v.t10 = t10; v.rh10 = 500;
    if (withPmCo) {
        v.mask |= PM | CO;
        v.pm1 = pm25; v.pm25 = pm25; v.pm10 = pm25; v.co10 = co10;
    }
    return v;
}

void test_window_avg_and_max() {
    Window w; reset(w);
    add(w, sample(200, 10, 5, true));
    add(w, sample(210, 50, 80, true));   // 순간 상승
    add(w, sample(221, 12, 6, true));
    Values o;
    TEST_ASSERT_TRUE(take(w, &o));
    TEST_ASSERT_EQUAL_INT16(210, o.t10);           // (200+210+221)/3 = 210.33 → 210
    TEST_ASSERT_EQUAL_UINT16(24, o.pm25);          // 72/3
    TEST_ASSERT_EQUAL_UINT16(30, o.co10);          // 91/3 = 30.33
    TEST_ASSERT_EQUAL_UINT16(50, o.pm25_max);
    TEST_ASSERT_EQUAL_UINT16(80, o.co10_max);
    TEST_ASSERT_EQUAL_HEX16(T | RH | PM | CO | MAX, o.mask);
    TEST_ASSERT_FALSE(take(w, &o));                // 가져가면 비워진다
    TEST_ASSERT_EQUAL_HEX16(0, o.mask);
}

void test_window_per_field_counts() {
    // 주차 듀티: 앞 3프레임은 SHT41 만, 끝 2프레임에 PMS·CO 가 붙는다 → PM·CO 평균은 끝 2프레임만으로.
    Window w; reset(w);
    for (int i = 0; i < 3; i++) add(w, sample(300, 0, 0, false));
    add(w, sample(300, 20, 10, true));
    add(w, sample(300, 30, 20, true));
    Values o;
    take(w, &o);
    TEST_ASSERT_EQUAL_UINT16(25, o.pm25);
    TEST_ASSERT_EQUAL_UINT16(15, o.co10);
    TEST_ASSERT_EQUAL_INT16(300, o.t10);
}

void test_window_negative_rounding_and_guard() {
    Window w; reset(w);
    Values a = {}; a.mask = T | GUARD; a.t10 = -5; a.guard = 1;
    Values b = {}; b.mask = T | GUARD; b.t10 = -6; b.guard = 0;
    add(w, a); add(w, b);
    Values o;
    take(w, &o);
    TEST_ASSERT_EQUAL_INT16(-6, o.t10);    // -5.5 → -6 (0 에서 먼 쪽)
    TEST_ASSERT_EQUAL_UINT8(0, o.guard);   // 마지막 값
    TEST_ASSERT_EQUAL_HEX16(0, o.mask & MAX);   // CO·PM 없으면 MAX 도 없다
}

void test_window_empty_mask_ignored() {
    Window w; reset(w);
    Values e = {};
    add(w, e);
    TEST_ASSERT_EQUAL_UINT16(0, w.n);
    Values o;
    TEST_ASSERT_FALSE(take(w, &o));
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
    RUN_TEST(test_decode_drops_max_bit);
    RUN_TEST(test_json_max_fields);
    RUN_TEST(test_window_avg_and_max);
    RUN_TEST(test_window_per_field_counts);
    RUN_TEST(test_window_negative_rounding_and_guard);
    RUN_TEST(test_window_empty_mask_ignored);
    return UNITY_END();
}
