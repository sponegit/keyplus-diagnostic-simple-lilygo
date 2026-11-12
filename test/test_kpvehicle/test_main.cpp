// KpVehicle 차량 상태 프레임 코덱 단위 테스트 — pio test -e native
#include <unity.h>

#include <KpVehicle.h>

using namespace KpVehicle;

void setUp() {}
void tearDown() {}

void test_roundtrip_all_combinations() {
    for (int m = 0; m < 8; m++) {
        const Values v = make(m & 1, m & 2, m & 4);
        uint8_t buf[KPVEHICLE_PAYLOAD_LEN];
        TEST_ASSERT_EQUAL(KPVEHICLE_PAYLOAD_LEN, encode(v, buf, sizeof(buf)));
        Values d;
        TEST_ASSERT_TRUE(decode(buf, sizeof(buf), &d));
        TEST_ASSERT_EQUAL(m & 1 ? true : false, d.ignOn());
        TEST_ASSERT_EQUAL(m & 2 ? true : false, d.driving());
        TEST_ASSERT_EQUAL(m & 4 ? true : false, d.gnssOn());
    }
}

void test_lengths() {
    uint8_t buf[4] = {};
    TEST_ASSERT_EQUAL(0, encode(make(true, true, true), buf, 0));
    Values d;
    TEST_ASSERT_FALSE(decode(buf, 0, &d));
    buf[0] = GNSS_ON;
    TEST_ASSERT_TRUE(decode(buf, sizeof(buf), &d));   // 뒤에 덧붙은 필드는 무시
    TEST_ASSERT_TRUE(d.gnssOn());
    TEST_ASSERT_FALSE(d.driving());
}

void test_reserved_bits_dropped() {
    uint8_t buf[1] = {0xF8 | DRIVING};
    Values d;
    decode(buf, 1, &d);
    TEST_ASSERT_EQUAL_UINT8(DRIVING, d.flags);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_roundtrip_all_combinations);
    RUN_TEST(test_lengths);
    RUN_TEST(test_reserved_bits_dropped);
    return UNITY_END();
}
