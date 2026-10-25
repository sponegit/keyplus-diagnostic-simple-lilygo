// KpOta 서브 OTA 수신기 단위 테스트 — pio test -e native
#include <unity.h>

#include <string.h>
#include <vector>

#include <KpOta.h>

using namespace KpOta;

void setUp() {}
void tearDown() {}

// --- 가짜 플래시 ------------------------------------------------------------------

struct FakeFlash {
    std::vector<uint8_t> image;
    uint32_t size = 0;
    bool     begun = false;
    int      begins = 0, aborts = 0, writes = 0;
    uint32_t capacity = 6553600;       // 서브 app 파티션
    bool     failWrite = false;
    Status   endResult = Status::OK;

    static bool begin(uint32_t sz, const char*, void* c) {
        auto* f = static_cast<FakeFlash*>(c);
        f->begins++;
        if (sz > f->capacity) return false;
        f->image.clear(); f->size = sz; f->begun = true;
        return true;
    }
    static bool write(const uint8_t* d, size_t n, void* c) {
        auto* f = static_cast<FakeFlash*>(c);
        if (f->failWrite) return false;
        f->writes++;
        f->image.insert(f->image.end(), d, d + n);
        return true;
    }
    static Status end(void* c) { auto* f = static_cast<FakeFlash*>(c); f->begun = false; return f->endResult; }
    static void abort(void* c) { auto* f = static_cast<FakeFlash*>(c); f->aborts++; f->begun = false; }
};

struct Rx {
    FakeFlash fl;
    Receiver  r;
    std::vector<std::vector<uint8_t>> acks;
    uint32_t  now = 1000;

    static void replyCb(const uint8_t* p, size_t n, void* c) {
        static_cast<Rx*>(c)->acks.emplace_back(p, p + n);
    }
    Rx() {
        Receiver::Flash f = {FakeFlash::begin, FakeFlash::write, FakeFlash::end, FakeFlash::abort, &fl};
        r.begin(f, replyCb, this);
    }
    void begin(uint32_t size, const char* md5 = "", const char* ver = "0.2.0") {
        Begin b = {};
        b.size = size; b.baud = 921600;
        strcpy(b.md5, md5); strcpy(b.version, ver);
        uint8_t buf[KPOTA_BEGIN_MAX];
        r.onBegin(buf, encodeBegin(b, buf, sizeof(buf)), now);
    }
    void chunk(uint32_t off, const std::vector<uint8_t>& img, size_t n) {
        uint8_t buf[4 + KPOTA_CHUNK_MAX];
        r.onChunk(buf, encodeChunk(off, img.data() + off, n, buf, sizeof(buf)), now);
    }
    void end(uint32_t total) {
        uint8_t buf[4];
        r.onEnd(buf, encodeEnd(total, buf, sizeof(buf)), now);
    }
    Status last(Phase* ph = nullptr, uint32_t* next = nullptr) {
        Phase p; Status s; uint32_t nx;
        TEST_ASSERT_TRUE(decodeAck(acks.back().data(), acks.back().size(), &p, &s, &nx));
        if (ph) *ph = p;
        if (next) *next = nx;
        return s;
    }
};

static std::vector<uint8_t> makeImage(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; i++) v[i] = (uint8_t)(i * 31 + 7);
    return v;
}

// --- 코덱 --------------------------------------------------------------------------

void test_codec_roundtrip_and_malformed() {
    Begin b = {};
    b.size = 300000; b.baud = 921600;
    strcpy(b.md5, "0123456789abcdef0123456789abcdef");
    strcpy(b.version, "0.2.0");
    uint8_t buf[KPOTA_BEGIN_MAX];
    size_t n = encodeBegin(b, buf, sizeof(buf));
    Begin d;
    TEST_ASSERT_TRUE(decodeBegin(buf, n, &d));
    TEST_ASSERT_EQUAL(300000, d.size);
    TEST_ASSERT_EQUAL(921600, d.baud);
    TEST_ASSERT_EQUAL_STRING(b.md5, d.md5);
    TEST_ASSERT_EQUAL_STRING("0.2.0", d.version);
    TEST_ASSERT_FALSE(decodeBegin(buf, n - 1, &d));

    uint8_t ack[KPOTA_ACK_LEN];
    encodeAck(Phase::CHUNK, Status::OK, 4096, ack, sizeof(ack));
    Phase ph; Status st; uint32_t next;
    TEST_ASSERT_TRUE(decodeAck(ack, sizeof(ack), &ph, &st, &next));
    TEST_ASSERT_EQUAL(4096, next);
    ack[0] = 9;
    TEST_ASSERT_FALSE(decodeAck(ack, sizeof(ack), &ph, &st, &next));

    uint8_t data[KPOTA_CHUNK_MAX + 1] = {0};
    uint8_t out[8 + KPOTA_CHUNK_MAX];
    TEST_ASSERT_EQUAL(0, encodeChunk(0, data, KPOTA_CHUNK_MAX + 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL(4 + KPOTA_CHUNK_MAX, encodeChunk(0, data, KPOTA_CHUNK_MAX, out, sizeof(out)));
}

// --- 수신기 ------------------------------------------------------------------------

void test_full_transfer_writes_exact_image() {
    Rx x;
    auto img = makeImage(5000);
    x.begin(5000);
    TEST_ASSERT_TRUE(x.last() == Status::OK);
    // 모뎀이 주는 대로 크기가 제각각인 조각
    size_t sizes[] = {1024, 700, 1024, 1, 1024, 1024, 203};
    uint32_t off = 0;
    for (size_t s : sizes) { x.chunk(off, img, s); TEST_ASSERT_TRUE(x.last() == Status::OK); off += s; }
    TEST_ASSERT_EQUAL(5000, off);
    x.end(5000);
    TEST_ASSERT_TRUE(x.last() == Status::OK);
    TEST_ASSERT_TRUE(x.fl.image == img);
    TEST_ASSERT_TRUE(x.r.finished());
}

void test_duplicate_chunk_not_rewritten() {
    Rx x;
    auto img = makeImage(3000);
    x.begin(3000);
    x.chunk(0, img, 1024);
    x.chunk(0, img, 1024);                // ACK 유실 → 메인이 같은 조각 재전송
    uint32_t next;
    TEST_ASSERT_TRUE(x.last(nullptr, &next) == Status::OK);
    TEST_ASSERT_EQUAL(1024, next);
    TEST_ASSERT_EQUAL(1, x.fl.writes);
    x.chunk(1024, img, 1024);
    x.chunk(2048, img, 952);
    x.end(3000);
    TEST_ASSERT_TRUE(x.fl.image == img);
}

void test_skipped_offset_rejected() {
    Rx x;
    auto img = makeImage(3000);
    x.begin(3000);
    x.chunk(1024, img, 1024);
    uint32_t next;
    TEST_ASSERT_TRUE(x.last(nullptr, &next) == Status::BAD_OFFSET);
    TEST_ASSERT_EQUAL(0, next);
    TEST_ASSERT_EQUAL(0, x.fl.writes);
}

void test_chunk_or_end_before_begin() {
    Rx x;
    auto img = makeImage(100);
    x.chunk(0, img, 100);
    TEST_ASSERT_TRUE(x.last() == Status::NOT_STARTED);
    x.end(100);
    TEST_ASSERT_TRUE(x.last() == Status::NOT_STARTED);
}

void test_begin_retransmit_does_not_restart_but_new_begin_does() {
    Rx x;
    auto img = makeImage(2048);
    x.begin(2048);
    x.begin(2048);                        // BEGIN ACK 유실 → 재전송
    TEST_ASSERT_TRUE(x.last() == Status::OK);
    TEST_ASSERT_EQUAL(1, x.fl.begins);
    x.chunk(0, img, 1024);
    x.begin(4096, "", "0.3.0");          // 메인이 다른 전송을 새로 시작
    TEST_ASSERT_TRUE(x.last() == Status::OK);
    TEST_ASSERT_EQUAL(2, x.fl.begins);
    TEST_ASSERT_EQUAL(1, x.fl.aborts);
    TEST_ASSERT_EQUAL(0, x.r.received());
}

void test_image_too_big_is_no_space() {
    Rx x;
    x.begin(x.fl.capacity + 1);
    TEST_ASSERT_TRUE(x.last() == Status::NO_SPACE);
    TEST_ASSERT_FALSE(x.r.active());
}

void test_write_failure_aborts() {
    Rx x;
    auto img = makeImage(2048);
    x.begin(2048);
    x.fl.failWrite = true;
    x.chunk(0, img, 1024);
    TEST_ASSERT_TRUE(x.last() == Status::WRITE_FAIL);
    TEST_ASSERT_EQUAL(1, x.fl.aborts);
    TEST_ASSERT_FALSE(x.r.active());
}

void test_overrun_and_short_end_are_size_mismatch() {
    Rx a;
    auto img = makeImage(3000);
    a.begin(1500);
    a.chunk(0, img, 1024);
    a.chunk(1024, img, 1024);             // 1500 을 넘는다
    TEST_ASSERT_TRUE(a.last() == Status::SIZE_MISMATCH);

    Rx b;
    b.begin(3000);
    b.chunk(0, img, 1024);
    b.end(3000);                          // 다 안 받았는데 END
    TEST_ASSERT_TRUE(b.last() == Status::SIZE_MISMATCH);
    TEST_ASSERT_FALSE(b.r.finished());
}

void test_verify_failure_reported_not_finished() {
    Rx x;
    auto img = makeImage(1000);
    x.fl.endResult = Status::FOREIGN_HW;  // 메인 이미지가 서브로 온 경우
    x.begin(1000);
    x.chunk(0, img, 1000);
    x.end(1000);
    TEST_ASSERT_TRUE(x.last() == Status::FOREIGN_HW);
    TEST_ASSERT_FALSE(x.r.finished());
    TEST_ASSERT_FALSE(x.r.active());
}

void test_end_retransmit_after_finish_is_ok() {
    Rx x;
    auto img = makeImage(500);
    x.begin(500);
    x.chunk(0, img, 500);
    x.end(500);
    x.end(500);                           // END ACK 유실 → 재전송
    TEST_ASSERT_TRUE(x.last() == Status::OK);
    x.begin(500);                         // 확정 뒤엔 새 BEGIN 을 무시(재부팅 대기)
    TEST_ASSERT_EQUAL(1, x.fl.begins);
}

void test_idle_abort() {
    Rx x;
    x.begin(2048);
    x.now += KPOTA_IDLE_ABORT_MS - 1;
    TEST_ASSERT_FALSE(x.r.tick(x.now));
    x.now += 2;
    TEST_ASSERT_TRUE(x.r.tick(x.now));
    TEST_ASSERT_FALSE(x.r.active());
    TEST_ASSERT_EQUAL(1, x.fl.aborts);
    TEST_ASSERT_FALSE(x.r.tick(x.now + 100000));   // 한 번만
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_codec_roundtrip_and_malformed);
    RUN_TEST(test_full_transfer_writes_exact_image);
    RUN_TEST(test_duplicate_chunk_not_rewritten);
    RUN_TEST(test_skipped_offset_rejected);
    RUN_TEST(test_chunk_or_end_before_begin);
    RUN_TEST(test_begin_retransmit_does_not_restart_but_new_begin_does);
    RUN_TEST(test_image_too_big_is_no_space);
    RUN_TEST(test_write_failure_aborts);
    RUN_TEST(test_overrun_and_short_end_are_size_mismatch);
    RUN_TEST(test_verify_failure_reported_not_finished);
    RUN_TEST(test_end_retransmit_after_finish_is_ok);
    RUN_TEST(test_idle_abort);
    return UNITY_END();
}
