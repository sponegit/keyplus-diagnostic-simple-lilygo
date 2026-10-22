// KpKey 차키 시퀀스 단위 테스트 — pio test -e native
#include <unity.h>

#include <string.h>
#include <vector>

#include <KpKey.h>

using namespace KpKey;

void setUp() {}
void tearDown() {}

// --- 검증 ----------------------------------------------------------------------

void test_validate_limits() {
    Step ok[] = {{1, 200, 300}, {4, 2500, 0}};
    TEST_ASSERT_TRUE(validate(ok, 2) == Error::OK);
    TEST_ASSERT_TRUE(validate(ok, 0) == Error::NO_STEPS);
    Step io0[] = {{0, 100, 0}}, io7[] = {{7, 100, 0}};
    TEST_ASSERT_TRUE(validate(io0, 1) == Error::BAD_IO);
    TEST_ASSERT_TRUE(validate(io7, 1) == Error::BAD_IO);
    Step p0[] = {{1, 0, 0}}, pBig[] = {{1, KPKEY_PRESS_MAX_MS + 1, 0}};
    TEST_ASSERT_TRUE(validate(p0, 1) == Error::BAD_PRESS);
    TEST_ASSERT_TRUE(validate(pBig, 1) == Error::BAD_PRESS);
    Step wBig[] = {{1, 100, KPKEY_WAIT_MAX_MS + 1}};
    TEST_ASSERT_TRUE(validate(wBig, 1) == Error::BAD_WAIT);
    Step many[KPKEY_MAX_STEPS + 1];
    for (auto& s : many) s = {1, 10, 0};
    TEST_ASSERT_TRUE(validate(many, KPKEY_MAX_STEPS) == Error::OK);
    TEST_ASSERT_TRUE(validate(many, KPKEY_MAX_STEPS + 1) == Error::TOO_MANY_STEPS);
    Step longSeq[] = {{1, 10000, 10000}, {2, 10000, 1}};   // 30001ms
    TEST_ASSERT_TRUE(validate(longSeq, 2) == Error::TOO_LONG);
}

// --- JSON ----------------------------------------------------------------------

void test_parse_steps_from_command_body() {
    const char* body =
        "{\"command_id\":\"c-1\",\"type\":\"remote_start\",\"payload\":{\"steps\":["
        "{\"io\":1,\"press_ms\":200,\"wait_ms\":300},"
        "{ \"wait_ms\" : 300 , \"io\" : 1, \"press_ms\":200 },"
        "{\"io\":4,\"press_ms\":2500}]},\"expires_at\":1790000000}";
    Step s[KPKEY_MAX_STEPS];
    size_t n = 0;
    TEST_ASSERT_TRUE(parseStepsJson(body, s, KPKEY_MAX_STEPS, &n) == Error::OK);
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL(1, s[1].io);
    TEST_ASSERT_EQUAL(200, s[1].pressMs);
    TEST_ASSERT_EQUAL(300, s[1].waitMs);
    TEST_ASSERT_EQUAL(4, s[2].io);
    TEST_ASSERT_EQUAL(0, s[2].waitMs);   // wait 생략 = 0
    TEST_ASSERT_EQUAL(3500, totalMs(s, n));
}

void test_parse_steps_errors() {
    Step s[KPKEY_MAX_STEPS];
    size_t n;
    TEST_ASSERT_TRUE(parseStepsJson("{\"type\":\"door_lock\"}", s, 16, &n) == Error::ABSENT);
    TEST_ASSERT_TRUE(parseStepsJson("{\"steps\":[]}", s, 16, &n) == Error::NO_STEPS);
    TEST_ASSERT_TRUE(parseStepsJson("{\"steps\":{}}", s, 16, &n) == Error::PARSE);
    TEST_ASSERT_TRUE(parseStepsJson("{\"steps\":[{\"io\":\"1\",\"press_ms\":200}]}", s, 16, &n) == Error::PARSE);
    TEST_ASSERT_TRUE(parseStepsJson("{\"steps\":[{\"io\":1,\"press_ms\":200.5}]}", s, 16, &n) == Error::PARSE);
    TEST_ASSERT_TRUE(parseStepsJson("{\"steps\":[{\"io\":1}]}", s, 16, &n) == Error::PARSE);   // press 필수
    TEST_ASSERT_TRUE(parseStepsJson("{\"steps\":[{\"io\":1,\"press_ms\":200}", s, 16, &n) == Error::PARSE);
    TEST_ASSERT_TRUE(parseStepsJson("{\"steps\":[{\"io\":9,\"press_ms\":200}]}", s, 16, &n) == Error::BAD_IO);
    TEST_ASSERT_TRUE(parseStepsJson("{\"steps\":[{\"io\":1,\"press_ms\":-5}]}", s, 16, &n) == Error::BAD_PRESS);
    TEST_ASSERT_TRUE(parseStepsJson("{\"steps\":[{\"io\":1,\"press_ms\":99999}]}", s, 16, &n) == Error::BAD_PRESS);
    TEST_ASSERT_TRUE(parseStepsJson("{\"steps\":[{\"io\":1,\"press_ms\":10,\"wait_ms\":-1}]}", s, 16, &n) == Error::BAD_WAIT);

    std::string many = "{\"steps\":[";
    for (int i = 0; i < KPKEY_MAX_STEPS + 1; i++) many += (i ? "," : "") + std::string("{\"io\":1,\"press_ms\":10}");
    many += "]}";
    TEST_ASSERT_TRUE(parseStepsJson(many.c_str(), s, 16, &n) == Error::TOO_MANY_STEPS);
}

// --- payload -------------------------------------------------------------------

void test_cmd_result_roundtrip_and_malformed() {
    Step in[] = {{1, 200, 300}, {6, 10000, 0}};
    uint8_t buf[KPKEY_CMD_MAX_LEN];
    size_t len = encodeCmd(0xBEEF, in, 2, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(13, len);
    uint16_t id;
    Step out[KPKEY_MAX_STEPS];
    size_t n;
    TEST_ASSERT_TRUE(decodeCmd(buf, len, &id, out, &n) == Error::OK);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, id);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL(10000, out[1].pressMs);
    TEST_ASSERT_TRUE(decodeCmd(buf, len - 1, &id, out, &n) == Error::PARSE);   // 길이 불일치
    buf[3] = 9;                                                                // io 범위 밖
    TEST_ASSERT_TRUE(decodeCmd(buf, len, &id, out, &n) == Error::BAD_IO);

    uint8_t r[KPKEY_RESULT_LEN];
    encodeResult(7, Status::DONE, 0, r, sizeof(r));
    Status s;
    uint8_t d;
    TEST_ASSERT_TRUE(decodeResult(r, sizeof(r), &id, &s, &d));
    TEST_ASSERT_EQUAL(7, id);
    TEST_ASSERT_TRUE(s == Status::DONE);
    r[2] = 0;
    TEST_ASSERT_FALSE(decodeResult(r, sizeof(r), &id, &s, &d));
}

// --- Executor ------------------------------------------------------------------

struct LineEv { uint32_t t; uint8_t io; bool pressed; };
struct ExecHarness {
    Executor ex;
    uint32_t now = 1000;
    std::vector<LineEv> lines;
    std::vector<std::vector<uint8_t>> replies;
    int pressedCount = 0;
    int maxPressed = 0;

    static void lineCb(uint8_t io, bool pressed, void* ctx) {
        auto* h = static_cast<ExecHarness*>(ctx);
        h->lines.push_back({h->now, io, pressed});
        h->pressedCount += pressed ? 1 : -1;
        if (h->pressedCount > h->maxPressed) h->maxPressed = h->pressedCount;
    }
    static void replyCb(const uint8_t* p, size_t n, void* ctx) {
        static_cast<ExecHarness*>(ctx)->replies.emplace_back(p, p + n);
    }
    ExecHarness() { ex.begin(lineCb, replyCb, this); }
    void cmd(uint16_t id, const Step* s, size_t n) {
        uint8_t buf[KPKEY_CMD_MAX_LEN];
        size_t len = encodeCmd(id, s, n, buf, sizeof(buf));
        ex.onCmd(buf, len, now);
    }
    void run(uint32_t ms) {
        for (uint32_t t = 0; t < ms; t++) { now++; ex.update(now); }
    }
    Status replyStatus(size_t i) {
        uint16_t id; Status s; uint8_t d;
        decodeResult(replies[i].data(), replies[i].size(), &id, &s, &d);
        return s;
    }
};

void test_executor_remote_start_timing() {
    ExecHarness h;
    Step seq[] = {{1, 200, 300}, {1, 200, 300}, {4, 2500, 0}};
    h.cmd(10, seq, 3);
    TEST_ASSERT_EQUAL(1, h.replies.size());
    TEST_ASSERT_TRUE(h.replyStatus(0) == Status::ACCEPTED);
    h.run(4000);
    // 누름/뗌 6개 이벤트, 시각이 정확해야 한다.
    TEST_ASSERT_EQUAL(6, h.lines.size());
    const uint32_t t0 = 1000;
    LineEv want[] = {{t0, 1, true},          {t0 + 200, 1, false},
                     {t0 + 500, 1, true},    {t0 + 700, 1, false},
                     {t0 + 1000, 4, true},   {t0 + 3500, 4, false}};
    for (int i = 0; i < 6; i++) {
        TEST_ASSERT_EQUAL(want[i].t, h.lines[i].t);
        TEST_ASSERT_EQUAL(want[i].io, h.lines[i].io);
        TEST_ASSERT_EQUAL(want[i].pressed, h.lines[i].pressed);
    }
    TEST_ASSERT_EQUAL(1, h.maxPressed);   // 동시에 두 채널을 누르지 않는다
    TEST_ASSERT_EQUAL(2, h.replies.size());
    TEST_ASSERT_TRUE(h.replyStatus(1) == Status::DONE);
    TEST_ASSERT_FALSE(h.ex.busy());
}

void test_executor_duplicate_never_executes_twice() {
    ExecHarness h;
    Step s[] = {{2, 500, 0}};
    h.cmd(5, s, 1);
    h.run(100);
    h.cmd(5, s, 1);                       // 재전송(실행 중)
    h.run(1000);
    h.cmd(5, s, 1);                       // 재전송(끝난 뒤)
    h.run(1000);
    TEST_ASSERT_EQUAL(1, h.ex.stats().executed);
    TEST_ASSERT_EQUAL(2, h.ex.stats().duplicates);
    TEST_ASSERT_EQUAL(2, h.lines.size());   // 누름 1 + 뗌 1
    TEST_ASSERT_TRUE(h.replyStatus(1) == Status::ACCEPTED);
    TEST_ASSERT_TRUE(h.replyStatus(3) == Status::DONE);   // 끝난 뒤 재전송엔 DONE 재응답
}

void test_executor_busy_and_reject() {
    ExecHarness h;
    Step s[] = {{1, 1000, 0}};
    h.cmd(1, s, 1);
    h.run(10);
    h.cmd(2, s, 1);
    TEST_ASSERT_TRUE(h.replyStatus(1) == Status::BUSY);
    Step bad[] = {{9, 100, 0}};
    h.run(2000);
    h.cmd(3, bad, 1);
    TEST_ASSERT_TRUE(h.replyStatus(h.replies.size() - 1) == Status::REJECTED);
    TEST_ASSERT_EQUAL(1, h.ex.stats().executed);
    TEST_ASSERT_EQUAL(2, h.lines.size());
}

void test_executor_forget_allows_reused_id_after_main_reboot() {
    ExecHarness h;
    Step s[] = {{1, 100, 0}};
    h.cmd(1, s, 1);
    h.run(500);
    h.ex.forget();                        // 링크 재연결(메인 재부팅 — id 가 다시 1부터)
    h.cmd(1, s, 1);
    h.run(500);
    TEST_ASSERT_EQUAL(2, h.ex.stats().executed);
}

void test_executor_abort_releases_and_blocks_retransmit() {
    ExecHarness h;
    Step s[] = {{3, 5000, 0}};
    h.cmd(8, s, 1);
    h.run(100);
    h.ex.abort();
    TEST_ASSERT_EQUAL(0, h.pressedCount);
    TEST_ASSERT_FALSE(h.ex.busy());
    h.cmd(8, s, 1);                       // 중단된 명령의 재전송 — 다시 누르면 안 된다
    h.run(100);
    TEST_ASSERT_EQUAL(1, h.ex.stats().executed);
    TEST_ASSERT_TRUE(h.replyStatus(h.replies.size() - 1) == Status::REJECTED);
}

void test_executor_ignores_too_short_payload() {
    ExecHarness h;
    uint8_t junk[] = {1, 2};
    h.ex.onCmd(junk, sizeof(junk), h.now);
    TEST_ASSERT_EQUAL(0, h.replies.size());
}

// --- Client ↔ Executor 종단 -----------------------------------------------------

struct E2E {
    Client      cl;
    ExecHarness sub;
    std::vector<std::vector<uint8_t>> toSub, toMain;
    bool linkUp = true;
    int  dropToSub = 0, dropToMain = 0;   // 앞에서부터 N 개 유실
    int  cmdSends = 0;
    bool subDead = false;

    static bool sendCb(const uint8_t* p, size_t n, void* ctx) {
        auto* e = static_cast<E2E*>(ctx);
        if (!e->linkUp) return false;
        e->cmdSends++;
        e->toSub.emplace_back(p, p + n);
        return true;
    }
    static void replyCb(const uint8_t* p, size_t n, void* ctx) {
        static_cast<E2E*>(ctx)->toMain.emplace_back(p, p + n);
    }
    E2E() { cl.begin(sendCb, this, 100); }   // 실행기 구성은 E2EFixed 가 한다
    void step() {
        sub.now++;
        for (auto& m : toSub) {
            if (dropToSub > 0) { dropToSub--; continue; }
            if (!subDead) sub.ex.onCmd(m.data(), m.size(), sub.now);
        }
        toSub.clear();
        sub.ex.update(sub.now);
        for (auto& m : toMain) {
            if (dropToMain > 0) { dropToMain--; continue; }
            cl.onResult(m.data(), m.size(), sub.now);
        }
        toMain.clear();
        cl.update(sub.now);
    }
    void run(uint32_t ms) { for (uint32_t i = 0; i < ms; i++) step(); }
    bool outcome(bool* done, Client::Fail* why) {
        uint16_t id;
        return cl.takeOutcome(done, why, &id);
    }
};

// ExecHarness::lineCb 는 ctx 를 ExecHarness 로 본다 — E2E 에서는 reply 만 E2E 로 받도록
// 실행기를 직접 구성한다.
struct E2EFixed : E2E {
    static void lineCb(uint8_t io, bool pressed, void* ctx) {
        auto* e = static_cast<E2EFixed*>(ctx);
        ExecHarness::lineCb(io, pressed, &e->sub);
    }
    E2EFixed() { sub.ex.begin(lineCb, replyCb, this); }
};

static Step kLock[] = {{1, 200, 0}};

void test_e2e_normal() {
    E2EFixed e;
    TEST_ASSERT_TRUE(e.cl.start(kLock, 1, e.sub.now, nullptr) == Client::Start::OK);
    e.run(1000);
    bool done; Client::Fail why;
    TEST_ASSERT_TRUE(e.outcome(&done, &why));
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL(1, e.sub.ex.stats().executed);
    TEST_ASSERT_EQUAL(1, e.cmdSends);
}

void test_e2e_lost_command_is_retransmitted_once_executed() {
    E2EFixed e;
    e.dropToSub = 1;
    e.cl.start(kLock, 1, e.sub.now, nullptr);
    e.run(2000);
    bool done; Client::Fail why;
    TEST_ASSERT_TRUE(e.outcome(&done, &why));
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL(2, e.cmdSends);
    TEST_ASSERT_EQUAL(1, e.sub.ex.stats().executed);
}

void test_e2e_lost_accepts_do_not_double_press() {
    E2EFixed e;
    e.dropToMain = 2;                     // ACCEPTED 두 번 유실 → 메인은 두 번 재전송
    Step s[] = {{2, 1200, 0}};            // 재전송 창(500ms × 2)보다 길게
    e.cl.start(s, 1, e.sub.now, nullptr);
    e.run(4000);
    bool done; Client::Fail why;
    TEST_ASSERT_TRUE(e.outcome(&done, &why));
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL(3, e.cmdSends);
    TEST_ASSERT_EQUAL(1, e.sub.ex.stats().executed);   // 핵심: 실행은 한 번
    TEST_ASSERT_EQUAL(2, e.sub.ex.stats().duplicates);
}

void test_e2e_sub_dead_is_no_response() {
    E2EFixed e;
    e.subDead = true;
    e.cl.start(kLock, 1, e.sub.now, nullptr);
    e.run(KPKEY_ACCEPT_TIMEOUT_MS * (KPKEY_RETRIES + 1) - 10);
    TEST_ASSERT_TRUE(e.cl.busy());
    e.run(20);
    bool done; Client::Fail why;
    TEST_ASSERT_TRUE(e.outcome(&done, &why));
    TEST_ASSERT_FALSE(done);
    TEST_ASSERT_TRUE(why == Client::Fail::NO_RESPONSE);
    TEST_ASSERT_EQUAL(KPKEY_RETRIES + 1, e.cmdSends);
}

void test_e2e_lost_done_is_timeout() {
    // ACCEPTED 는 왔는데 DONE 이 유실 — 실행은 됐지만 메인은 확인 못 함 → failed(정직).
    E2EFixed e;
    e.cl.start(kLock, 1, e.sub.now, nullptr);
    e.step(); e.step();                   // ACCEPTED 수신
    TEST_ASSERT_TRUE(e.cl.accepted());
    e.dropToMain = 1;                     // 다음(DONE) 유실
    e.run(200 + KPKEY_DONE_MARGIN_MS + 10);
    bool done; Client::Fail why;
    TEST_ASSERT_TRUE(e.outcome(&done, &why));
    TEST_ASSERT_FALSE(done);
    TEST_ASSERT_TRUE(why == Client::Fail::TIMEOUT);
    TEST_ASSERT_EQUAL(1, e.sub.ex.stats().executed);
}

void test_e2e_link_down_busy_invalid_and_link_lost() {
    E2EFixed e;
    e.linkUp = false;
    TEST_ASSERT_TRUE(e.cl.start(kLock, 1, e.sub.now, nullptr) == Client::Start::LINK_DOWN);
    e.linkUp = true;
    Step bad[] = {{0, 100, 0}};
    Error err;
    TEST_ASSERT_TRUE(e.cl.start(bad, 1, e.sub.now, &err) == Client::Start::INVALID);
    TEST_ASSERT_TRUE(err == Error::BAD_IO);
    TEST_ASSERT_TRUE(e.cl.start(kLock, 1, e.sub.now, nullptr) == Client::Start::OK);
    TEST_ASSERT_TRUE(e.cl.start(kLock, 1, e.sub.now, nullptr) == Client::Start::BUSY);
    e.cl.linkLost();
    bool done; Client::Fail why;
    TEST_ASSERT_TRUE(e.outcome(&done, &why));
    TEST_ASSERT_TRUE(why == Client::Fail::LINK_LOST);
    TEST_ASSERT_FALSE(e.outcome(&done, &why));   // 한 번만 꺼내진다
}

void test_e2e_late_result_of_old_request_ignored() {
    E2EFixed e;
    e.subDead = true;
    e.cl.start(kLock, 1, e.sub.now, nullptr);
    e.run(2000);                          // NO_RESPONSE
    bool done; Client::Fail why;
    e.outcome(&done, &why);
    uint16_t oldId = e.cl.currentId();
    e.subDead = false;
    e.cl.start(kLock, 1, e.sub.now, nullptr);
    uint8_t r[KPKEY_RESULT_LEN];
    encodeResult(oldId, Status::DONE, 0, r, sizeof(r));
    e.cl.onResult(r, sizeof(r), e.sub.now);   // 이전 요청의 늦은 DONE
    TEST_ASSERT_TRUE(e.cl.busy());            // 새 요청은 영향받지 않는다
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_validate_limits);
    RUN_TEST(test_parse_steps_from_command_body);
    RUN_TEST(test_parse_steps_errors);
    RUN_TEST(test_cmd_result_roundtrip_and_malformed);
    RUN_TEST(test_executor_remote_start_timing);
    RUN_TEST(test_executor_duplicate_never_executes_twice);
    RUN_TEST(test_executor_busy_and_reject);
    RUN_TEST(test_executor_forget_allows_reused_id_after_main_reboot);
    RUN_TEST(test_executor_abort_releases_and_blocks_retransmit);
    RUN_TEST(test_executor_ignores_too_short_payload);
    RUN_TEST(test_e2e_normal);
    RUN_TEST(test_e2e_lost_command_is_retransmitted_once_executed);
    RUN_TEST(test_e2e_lost_accepts_do_not_double_press);
    RUN_TEST(test_e2e_sub_dead_is_no_response);
    RUN_TEST(test_e2e_lost_done_is_timeout);
    RUN_TEST(test_e2e_link_down_busy_invalid_and_link_lost);
    RUN_TEST(test_e2e_late_result_of_old_request_ignored);
    return UNITY_END();
}
