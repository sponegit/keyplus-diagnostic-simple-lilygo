// OTA 하드웨어 세대 마커 스캐너 단위 테스트 — pio test -e native
// 메인 이미지(HW_MODEL "v2") 기준으로 검사한다.
#include <unity.h>

#include <string.h>

#define HW_MODEL "v2"
#include <hwmark.h>

void setUp() {}
void tearDown() {}

// 마커 조각은 NUL 이 중간에 들어가므로 길이를 명시해 넘긴다.
template <size_t N>
static HwMark::Scanner scan(const char (&s)[N], size_t chunk = N) {
    HwMark::Scanner sc;
    const size_t len = N - 1;   // 리터럴 끝의 NUL 은 제외(본문의 \0 은 포함)
    for (size_t pos = 0; pos < len; pos += chunk) {
        size_t n = (len - pos < chunk) ? len - pos : chunk;
        sc.feed(reinterpret_cast<const uint8_t*>(s) + pos, n);
    }
    return sc;
}

void test_own_marker_matches() {
    auto sc = scan("....KPHW:v2\0....");
    TEST_ASSERT_TRUE(sc.matched());
    TEST_ASSERT_FALSE(sc.foreign());
}

void test_v1_marker_is_foreign() {
    auto sc = scan("xxKPHW:v1\0yy");
    TEST_ASSERT_TRUE(sc.foreign());
    TEST_ASSERT_EQUAL_STRING("v1", sc.foreignModel());
}

void test_sub_marker_is_foreign_to_main() {
    // "v2" 가 "v2-sub" 의 접두어라도 다른 모델이다 — 서브 이미지가 메인에 들어가면 안 된다.
    auto sc = scan("KPHW:v2-sub\0");
    TEST_ASSERT_FALSE(sc.matched());
    TEST_ASSERT_TRUE(sc.foreign());
    TEST_ASSERT_EQUAL_STRING("v2-sub", sc.foreignModel());
}

void test_marker_split_across_chunks() {
    auto sc = scan("aaaaKPHW:v1\0bbbb", 3);
    TEST_ASSERT_TRUE(sc.foreign());
    auto sc1 = scan("aaaaKPHW:v2\0bbbb", 1);
    TEST_ASSERT_TRUE(sc1.matched());
}

void test_no_marker_passes() {
    auto sc = scan("an old v1 image without any marker");
    TEST_ASSERT_FALSE(sc.matched());
    TEST_ASSERT_FALSE(sc.foreign());
}

void test_empty_model_is_not_a_marker() {
    // "KPHW:" 바로 뒤 NUL — 리터럴을 따로 둔 이미지에서 생기는 조각. 마커로 보면 안 된다.
    auto sc = scan("KPHW:\0");
    TEST_ASSERT_FALSE(sc.matched());
    TEST_ASSERT_FALSE(sc.foreign());
}

void test_non_printable_or_too_long_is_not_a_marker() {
    auto a = scan("KPHW:v\x01" "1\0");
    TEST_ASSERT_FALSE(a.foreign());
    auto b = scan("KPHW:abcdefghijklmnopq\0");   // 17자 > 15
    TEST_ASSERT_FALSE(b.foreign());
}

void test_restart_after_partial_prefix() {
    auto sc = scan("KPHKPHW:v1\0");
    TEST_ASSERT_TRUE(sc.foreign());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_own_marker_matches);
    RUN_TEST(test_v1_marker_is_foreign);
    RUN_TEST(test_sub_marker_is_foreign_to_main);
    RUN_TEST(test_marker_split_across_chunks);
    RUN_TEST(test_no_marker_passes);
    RUN_TEST(test_empty_model_is_not_a_marker);
    RUN_TEST(test_non_printable_or_too_long_is_not_a_marker);
    RUN_TEST(test_restart_after_partial_prefix);
    return UNITY_END();
}
