// KpLink::Session 전송 계층 단위 테스트 — pio test -e native
// 메인·서브 두 인스턴스를 메모리 파이프로 잇고 시각을 흉내 낸다.
#include <unity.h>

#include <string.h>
#include <vector>

#include <KpLinkSession.h>

using namespace KpLink;

void setUp() {}
void tearDown() {}

// --- 시뮬레이터 ----------------------------------------------------------------

struct Events {
    int ups = 0, downs = 0;
    Session::Reason lastWhy = Session::Reason::HANDSHAKE;
    std::vector<Frame> frames;            // payload 포인터는 콜백 안에서만 유효 — 타입·seq 만 본다
    std::vector<std::vector<uint8_t>> payloads;
};

// 세션 하나 + 그 세션이 보낸 바이트 + 받은 이벤트. 콜백 ctx 로 Side* 를 넘긴다.
struct Side {
    Session              s;
    std::vector<uint8_t> out;
    Events               ev;
};

static size_t writeTo(const uint8_t* d, size_t n, void* ctx) {
    auto* side = static_cast<Side*>(ctx);
    side->out.insert(side->out.end(), d, d + n);
    return n;
}

static void stateCb(bool up, Session::Reason why, void* ctx) {
    auto* side = static_cast<Side*>(ctx);
    if (up) side->ev.ups++; else side->ev.downs++;
    side->ev.lastWhy = why;
}

static void frameCb(const Frame& f, void* ctx) {
    auto* side = static_cast<Side*>(ctx);
    side->ev.frames.push_back(f);
    side->ev.payloads.emplace_back(f.payload, f.payload + f.len);
}

struct Sim {
    Side     a, b;    // a = 메인, b = 서브
    uint32_t now = 1000;
    bool     cutAB = false, cutBA = false;   // 선 끊김(바이트 유실)

    void start(const char* aFw = "2.0.0", const char* bFw = "0.1.0") {
        a.s.begin(Role::MAIN, aFw, "v2", writeTo, &a, now);
        b.s.begin(Role::SUB, bFw, "v2-sub", writeTo, &b, now);
        for (Side* s : {&a, &b}) { s->s.onState(stateCb); s->s.onFrame(frameCb); }
    }
    // 대기 중인 바이트를 상대에게 배달(끊겼으면 버림).
    void deliver() {
        for (int guard = 0; guard < 20 && (!a.out.empty() || !b.out.empty()); guard++) {
            std::vector<uint8_t> ab, ba;
            ab.swap(a.out);
            ba.swap(b.out);
            if (!cutAB && !ab.empty()) b.s.onBytes(ab.data(), ab.size(), now);
            if (!cutBA && !ba.empty()) a.s.onBytes(ba.data(), ba.size(), now);
        }
    }
    // ms 만큼 10ms 단위로 진행 — 배달, tick.
    void run(uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 10) {
            now += 10;
            deliver();
            a.s.tick(now);
            b.s.tick(now);
            deliver();
        }
    }
};

// --- 테스트 --------------------------------------------------------------------

void test_handshake_brings_both_up_quickly() {
    Sim sim;
    sim.start();
    sim.run(100);
    TEST_ASSERT_TRUE(sim.a.s.up());
    TEST_ASSERT_TRUE(sim.b.s.up());
    TEST_ASSERT_EQUAL_STRING("0.1.0", sim.a.s.peerFw());
    TEST_ASSERT_EQUAL_STRING("v2-sub", sim.a.s.peerHw());
    TEST_ASSERT_EQUAL_STRING("2.0.0", sim.b.s.peerFw());
    TEST_ASSERT_EQUAL(1, sim.a.ev.ups);
    TEST_ASSERT_EQUAL(1, sim.b.ev.ups);
}

void test_handshake_when_peer_starts_later() {
    Sim sim;
    sim.start();
    sim.cutAB = sim.cutBA = true;   // 서브가 아직 안 켜진 것처럼
    sim.run(3000);
    TEST_ASSERT_FALSE(sim.a.s.up());
    sim.cutAB = sim.cutBA = false;
    sim.run(1500);                  // HELLO 재전송 주기(1s) 안에 올라와야 한다
    TEST_ASSERT_TRUE(sim.a.s.up());
    TEST_ASSERT_TRUE(sim.b.s.up());
}

void test_no_hello_storm_once_up() {
    Sim sim;
    sim.start();
    sim.run(200);
    uint32_t txA = sim.a.s.stats().txFrames;
    sim.run(10000);
    // 10초 동안 하트비트 ~10개만. HELLO 핑퐁이 이어지면 훨씬 많아진다.
    uint32_t sent = sim.a.s.stats().txFrames - txA;
    TEST_ASSERT_TRUE(sent >= 9 && sent <= 11);
    TEST_ASSERT_EQUAL(1, sim.a.ev.ups);
    TEST_ASSERT_EQUAL(0, sim.a.ev.downs);
}

void test_cable_cut_detected_within_timeout_and_recovers() {
    Sim sim;
    sim.start();
    sim.run(200);
    sim.cutAB = sim.cutBA = true;
    sim.run(KPLINK_LINK_TIMEOUT_MS - 1000);
    TEST_ASSERT_TRUE(sim.a.s.up());            // 아직 타임아웃 전
    sim.run(1100);
    TEST_ASSERT_FALSE(sim.a.s.up());
    TEST_ASSERT_FALSE(sim.b.s.up());
    TEST_ASSERT_TRUE(sim.a.ev.lastWhy == Session::Reason::TIMEOUT);
    TEST_ASSERT_EQUAL(1, sim.a.s.stats().timeouts);
    sim.cutAB = sim.cutBA = false;
    sim.run(1500);
    TEST_ASSERT_TRUE(sim.a.s.up());
    TEST_ASSERT_TRUE(sim.b.s.up());
}

void test_one_way_cut_downs_both_sides() {
    // 서브→메인만 끊김: 메인은 타임아웃, 서브는 메인의 seen=0 HELLO 를 보고 다운.
    Sim sim;
    sim.start();
    sim.run(200);
    sim.cutBA = true;
    sim.run(KPLINK_LINK_TIMEOUT_MS + 200);
    TEST_ASSERT_FALSE(sim.a.s.up());
    TEST_ASSERT_FALSE(sim.b.s.up());
    TEST_ASSERT_TRUE(sim.b.ev.lastWhy == Session::Reason::PEER_LOST);
}

void test_peer_restart_is_reported_and_reconnects() {
    Sim sim;
    sim.start();
    sim.run(200);
    // 서브 재부팅 — begin 을 다시 부른다(HELLO boot=1, seen=0).
    sim.b.s.begin(Role::SUB, "0.1.1", "v2-sub", writeTo, &sim.b, sim.now);
    sim.run(100);
    TEST_ASSERT_EQUAL(1, sim.a.s.stats().peerRestarts);
    TEST_ASSERT_EQUAL(1, sim.a.ev.downs);
    TEST_ASSERT_TRUE(sim.a.s.up());
    TEST_ASSERT_TRUE(sim.b.s.up());
    TEST_ASSERT_EQUAL_STRING("0.1.1", sim.a.s.peerFw());   // 새 버전으로 갱신(S5 OTA 확인에 쓴다)
}

void test_main_blocked_long_is_peer_lost_not_restart() {
    // 메인 loop 가 LTE 등록으로 30초 막힘 — 서브 바이트는 RX 버퍼에 쌓였다가 한꺼번에 들어온다.
    Sim sim;
    sim.start();
    sim.run(200);
    std::vector<uint8_t> backlog;
    for (int t = 0; t < 30000; t += 10) {
        sim.now += 10;
        sim.b.s.tick(sim.now);                       // 서브만 돈다
        backlog.insert(backlog.end(), sim.b.out.begin(), sim.b.out.end());
        sim.b.out.clear();
        sim.a.out.clear();
    }
    TEST_ASSERT_FALSE(sim.b.s.up());                 // 서브는 먼저 타임아웃
    sim.a.s.onBytes(backlog.data(), backlog.size(), sim.now);   // 풀리면 수신부터
    sim.a.s.tick(sim.now);
    sim.run(200);
    TEST_ASSERT_EQUAL(0, sim.a.s.stats().timeouts);      // 메인은 타임아웃으로 오판하지 않는다
    TEST_ASSERT_EQUAL(0, sim.a.s.stats().peerRestarts);  // 재부팅으로도 오판하지 않는다
    TEST_ASSERT_EQUAL(1, sim.a.s.stats().peerLost);
    TEST_ASSERT_TRUE(sim.a.s.up());
    TEST_ASSERT_TRUE(sim.b.s.up());
}

void test_protocol_mismatch_never_comes_up() {
    Sim sim;
    sim.start();
    // 서브가 다른 규약 버전이라고 주장하는 HELLO 를 직접 주입한다.
    uint8_t p[] = {KPLINK_PROTO_VERSION + 1, 2, 0, 0, 0};
    uint8_t buf[64];
    size_t n = encode(HELLO, 0, p, sizeof(p), buf, sizeof(buf));
    sim.cutBA = true;                     // 진짜 서브 HELLO 는 막는다
    sim.a.s.onBytes(buf, n, sim.now);
    sim.run(3000);
    TEST_ASSERT_FALSE(sim.a.s.up());
    TEST_ASSERT_TRUE(sim.a.s.stats().protoMismatch >= 1);
    TEST_ASSERT_EQUAL(KPLINK_PROTO_VERSION + 1, sim.a.s.peerProto());
}

void test_app_frames_delivered_hello_heartbeat_consumed() {
    Sim sim;
    sim.start();
    sim.run(200);
    const uint8_t msg[] = {'h', 'i'};
    int seq = sim.a.s.send(LOG, msg, sizeof(msg));
    TEST_ASSERT_TRUE(seq >= 0);
    sim.run(3000);
    TEST_ASSERT_EQUAL(1, sim.b.ev.frames.size());       // HELLO·HEARTBEAT 는 앱에 안 온다
    TEST_ASSERT_EQUAL_HEX8(LOG, sim.b.ev.frames[0].type);
    TEST_ASSERT_EQUAL(seq, sim.b.ev.frames[0].seq);
    TEST_ASSERT_EQUAL('h', sim.b.ev.payloads[0][0]);
}

void test_noise_on_line_does_not_drop_link() {
    Sim sim;
    sim.start();
    sim.run(200);
    const uint8_t junk[] = {0xAA, 0x55, 0x10, 0x00, 0x40, 0x00, 0x13, 0x37, 0xAA, 0x00};
    for (int i = 0; i < 20; i++) {
        sim.a.s.onBytes(junk, sizeof(junk), sim.now);
        sim.run(300);
    }
    TEST_ASSERT_TRUE(sim.a.s.up());
    TEST_ASSERT_EQUAL(0, sim.a.ev.downs);
    TEST_ASSERT_TRUE(sim.a.s.rxStats().expired + sim.a.s.rxStats().crcErr > 0);
}

void test_stalled_false_header_released_by_byte_gap() {
    // 잡음이 len=1000 짜리 가짜 헤더를 만들면, 뒤따르는 하트비트(8바이트/초)가 그 1000바이트를
    // 다 채울 때까지 묶인다 → 타임아웃(5초)으로 링크가 끊긴다. 바이트 공백 expire 가 막아야 한다.
    Sim sim;
    sim.start();
    sim.run(200);
    const uint8_t falseHdr[] = {0xAA, 0x55, 0x10, 0x00, 0xE8, 0x03};
    sim.a.s.onBytes(falseHdr, sizeof(falseHdr), sim.now);
    sim.run(KPLINK_LINK_TIMEOUT_MS * 2);
    TEST_ASSERT_TRUE(sim.a.s.up());
    TEST_ASSERT_EQUAL(0, sim.a.ev.downs);
    TEST_ASSERT_TRUE(sim.a.s.rxStats().expired >= 1);
}

void test_format_status() {
    Sim sim;
    sim.start();
    char buf[512];
    formatStatus(sim.a.s, sim.now, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "DOWN"));
    sim.run(200);
    sim.now += 125000;   // 2분 5초 경과(하트비트 없이 시각만 — 포맷 확인용)
    size_t n = formatStatus(sim.a.s, sim.now, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(strlen(buf), n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "UP (2m 05s)"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "fw 0.1.0  hw v2-sub"));
    char tiny[16];
    n = formatStatus(sim.a.s, sim.now, tiny, sizeof(tiny));   // 잘려도 넘치지 않는다
    TEST_ASSERT_EQUAL(15, n);
    TEST_ASSERT_EQUAL('\0', tiny[15]);
}

void test_seq_increments_and_wraps() {
    Sim sim;
    sim.start();
    int first = sim.a.s.send(LOG, nullptr, 0);
    int last  = first;
    for (int i = 0; i < 300; i++) last = sim.a.s.send(LOG, nullptr, 0);
    TEST_ASSERT_EQUAL((first + 300) & 0xFF, last);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_handshake_brings_both_up_quickly);
    RUN_TEST(test_handshake_when_peer_starts_later);
    RUN_TEST(test_no_hello_storm_once_up);
    RUN_TEST(test_cable_cut_detected_within_timeout_and_recovers);
    RUN_TEST(test_one_way_cut_downs_both_sides);
    RUN_TEST(test_peer_restart_is_reported_and_reconnects);
    RUN_TEST(test_main_blocked_long_is_peer_lost_not_restart);
    RUN_TEST(test_protocol_mismatch_never_comes_up);
    RUN_TEST(test_app_frames_delivered_hello_heartbeat_consumed);
    RUN_TEST(test_noise_on_line_does_not_drop_link);
    RUN_TEST(test_stalled_false_header_released_by_byte_gap);
    RUN_TEST(test_format_status);
    RUN_TEST(test_seq_increments_and_wraps);
    return UNITY_END();
}
