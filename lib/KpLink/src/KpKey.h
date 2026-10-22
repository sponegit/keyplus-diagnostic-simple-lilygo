#pragma once
// ===========================================================================
// KpKey — 차키 시퀀스 (작업서 S4 · D6)
//
// 차종별 버튼 조합(원격시동 등)은 펌웨어에 박지 않는다. 파트너사가 명령 payload 에
// steps[{io, press_ms, wait_ms}] 로 보내고, 펌웨어는 검증 후 순서대로 실행만 한다.
//
//   io       1~6 = 차키 채널 (1 LOCK · 2 UNLOCK · 3 TRUNK · 4 START · 5 OPT1 · 6 OPT2)
//   press_ms 누름 유지 1 ~ KPKEY_PRESS_MAX_MS
//   wait_ms  뗀 뒤 다음 단계까지 0 ~ KPKEY_WAIT_MAX_MS
//   단계 1 ~ KPKEY_MAX_STEPS, 전체 소요 ≤ KPKEY_TOTAL_MAX_MS
// 검증은 메인(서버 명령 수신)과 서브(실행 직전) 양쪽에서 같은 함수로 한다 — 서브가 최종 방어선.
//
// 링크 위 흐름 (작업서 §3.3 — 실행은 최대 1회):
//   메인 Client ── KEY_CMD{id, steps} ──▶ 서브 Executor
//              ◀── KEY_RESULT{id, ACCEPTED} ──   (500ms 안에 안 오면 같은 id 로 재전송, 최대 2회)
//              ◀── KEY_RESULT{id, DONE} ──       (시퀀스 끝)
//   서브는 id 로 중복을 걸러 같은 명령을 두 번 실행하지 않는다 — 차에 따라 같은 버튼 두 번이
//   잠금↔열림을 뒤집는다. 재전송을 받으면 실행 없이 현재 상태만 다시 답한다.
//
// Arduino 의존이 없다 — 호스트에서 단위 테스트한다(test/test_kpkey).
// ===========================================================================
#include <stddef.h>
#include <stdint.h>

#define KPKEY_IO_MIN             1
#define KPKEY_IO_MAX             6
// 채널 번호 — 파트너 API 문서에 공개하는 번호표와 같다. 서브 GPIO 4~9 에 순서대로 대응.
#define KPKEY_IO_LOCK            1
#define KPKEY_IO_UNLOCK          2
#define KPKEY_IO_TRUNK           3
#define KPKEY_IO_START           4
#define KPKEY_IO_OPT1            5
#define KPKEY_IO_OPT2            6
#define KPKEY_MAX_STEPS          16
#define KPKEY_PRESS_MAX_MS       10000
#define KPKEY_WAIT_MAX_MS        10000
#define KPKEY_TOTAL_MAX_MS       30000

// KEY_CMD 후 ACCEPTED 대기 — 넘으면 재전송. 메인 송신 버퍼(4KB)에 로그가 차 있으면 KEY_CMD 가
// 최대 ≈0.36초 늦게 나가므로 그보다 넉넉히 둔다(3회 × 500ms = 1.5초 창).
#define KPKEY_ACCEPT_TIMEOUT_MS  500
#define KPKEY_RETRIES            2      // 재전송 횟수(최초 전송 제외)
#define KPKEY_DONE_MARGIN_MS     2000   // ACCEPTED 후 DONE 대기 = 시퀀스 소요 + 이 여유

namespace KpKey {

struct Step {
    uint8_t  io;
    uint16_t pressMs;
    uint16_t waitMs;
};

enum class Error : uint8_t {
    OK = 0,
    ABSENT,          // JSON 에 steps 키가 없다(오류가 아니라 "기본 동작을 쓰라"는 뜻)
    PARSE,           // steps 형식이 깨졌다
    NO_STEPS,
    TOO_MANY_STEPS,
    BAD_IO,
    BAD_PRESS,
    BAD_WAIT,
    TOO_LONG,        // 전체 소요 초과
};
const char* errorName(Error e);
const char* ioName(uint8_t io);   // "LOCK" … "OPT2", 범위 밖은 "?"

Error    validate(const Step* steps, size_t n);
uint32_t totalMs(const Step* steps, size_t n);

// JSON 본문 어디에 있든 "steps":[{...},...] 를 찾아 파싱한다(cmd.cpp 의 평평한 간이 파서와
// 같은 수준 — 중첩 객체 순서·공백은 자유, 키는 io·press_ms·wait_ms). 검증까지 한다.
Error parseStepsJson(const char* json, Step* out, size_t max, size_t* n);

// --- 링크 payload -------------------------------------------------------------
// KEY_CMD    : [id:2 LE][count:1][count × (io:1, press:2 LE, wait:2 LE)]
// KEY_RESULT : [id:2 LE][status:1][detail:1]
enum class Status : uint8_t {
    ACCEPTED = 1,    // 실행 시작
    DONE     = 2,    // 시퀀스 완료(마지막 뗌까지)
    BUSY     = 3,    // 다른 시퀀스 진행 중 — 실행 안 함
    REJECTED = 4,    // 검증 실패 — 실행 안 함. detail = Error
};
const char* statusName(Status s);

#define KPKEY_CMD_MAX_LEN   (3 + 5 * KPKEY_MAX_STEPS)
#define KPKEY_RESULT_LEN    4

size_t encodeCmd(uint16_t id, const Step* steps, size_t n, uint8_t* out, size_t cap);
// 형식이 깨졌으면 PARSE, 내용이 범위를 벗어나면 해당 Error. id 는 형식만 맞으면 채운다.
Error  decodeCmd(const uint8_t* p, size_t len, uint16_t* id, Step* out, size_t* n);
size_t encodeResult(uint16_t id, Status s, uint8_t detail, uint8_t* out, size_t cap);
bool   decodeResult(const uint8_t* p, size_t len, uint16_t* id, Status* s, uint8_t* detail);

// --- 서브: 실행기 ---------------------------------------------------------------
// 한 번에 한 채널만 누른다. 단계마다 누름 → press 유지 → 뗌 → wait.
class Executor {
public:
    typedef void (*LineFn)(uint8_t io, bool pressed, void* ctx);           // 채널 누름/뗌
    typedef void (*ReplyFn)(const uint8_t* payload, size_t n, void* ctx);  // KEY_RESULT 송신

    void begin(LineFn line, ReplyFn reply, void* ctx);

    // KEY_CMD 수신. 중복 id 면 실행 없이 상태만 다시 답한다.
    void onCmd(const uint8_t* p, size_t len, uint32_t now);
    // 콘솔 시험용 — 답장 없이 실행. 진행 중이면 BUSY, 검증 실패면 해당 Error.
    Error startLocal(const Step* steps, size_t n, uint32_t now, bool* busy);
    void  update(uint32_t now);
    // 즉시 모두 뗀다(진행 중 시퀀스 중단). 원격 명령이었다면 DONE 은 보내지 않는다.
    void  abort();
    // 중복 판정 기억을 지운다 — 링크가 새로 연결되면 부른다(메인 재부팅 후 id 가 다시 시작).
    void  forget();

    bool    busy() const { return active_; }
    uint8_t pressedIo() const { return pressed_ ? steps_[idx_].io : 0; }
    uint8_t stepIndex() const { return idx_; }
    uint8_t stepCount() const { return n_; }

    struct Stats { uint32_t executed, duplicates, busy, rejected, aborted; };
    const Stats& stats() const { return stats_; }

private:
    void reply(uint16_t id, Status s, uint8_t detail);
    void start(const Step* steps, size_t n, uint32_t now);
    void finish();

    LineFn  line_  = nullptr;
    ReplyFn reply_ = nullptr;
    void*   ctx_   = nullptr;

    Step     steps_[KPKEY_MAX_STEPS];
    uint8_t  n_ = 0, idx_ = 0;
    bool     active_  = false;
    bool     pressed_ = false;
    uint32_t phaseEnd_ = 0;

    bool     remote_  = false;   // 지금 시퀀스가 원격(id 있음)인가
    uint16_t curId_   = 0;
    bool     haveLast_ = false;  // 마지막으로 끝낸 원격 명령 — 중복 재전송에 같은 답을 준다
    uint16_t lastId_   = 0;
    Status   lastStatus_ = Status::DONE;
    uint8_t  lastDetail_ = 0;

    Stats stats_ = {};
};

// --- 메인: 요청기 ---------------------------------------------------------------
class Client {
public:
    // KEY_CMD 송신. 링크 다운이면 false 를 돌려줄 것.
    typedef bool (*SendFn)(const uint8_t* payload, size_t n, void* ctx);

    enum class Start : uint8_t { OK, BUSY, INVALID, LINK_DOWN };
    enum class Fail  : uint8_t { NONE, NO_RESPONSE, SUB_BUSY, SUB_REJECTED, TIMEOUT, LINK_LOST };

    void  begin(SendFn send, void* ctx, uint16_t firstId);
    // 시퀀스 요청. OK 가 아니면 아무것도 보내지 않았다. INVALID 면 *err 에 사유.
    Start start(const Step* steps, size_t n, uint32_t now, Error* err);
    void  onResult(const uint8_t* p, size_t len, uint32_t now);
    void  update(uint32_t now);
    // 링크가 끊겼다 — 진행 중 요청을 실패로 끝낸다(서브가 실행했을 수는 있다).
    void  linkLost();

    bool busy() const { return state_ != State::IDLE && state_ != State::FINISHED; }
    // 끝난 요청의 결과를 한 번 꺼낸다. 꺼낼 것이 없으면 false.
    bool takeOutcome(bool* done, Fail* why, uint16_t* id);

    uint16_t currentId() const { return id_; }
    bool     accepted() const { return state_ == State::RUNNING; }

private:
    enum class State : uint8_t { IDLE, WAIT_ACCEPT, RUNNING, FINISHED };
    void finish(bool done, Fail why);
    bool sendCmd();

    SendFn   send_ = nullptr;
    void*    ctx_  = nullptr;
    State    state_ = State::IDLE;
    uint16_t nextId_ = 1;
    uint16_t id_ = 0;
    uint8_t  buf_[KPKEY_CMD_MAX_LEN];
    size_t   len_ = 0;
    uint32_t total_ = 0;
    uint8_t  tries_ = 0;
    uint32_t deadline_ = 0;
    bool     done_ = false;
    Fail     why_  = Fail::NONE;
};

const char* failName(Client::Fail f);

}  // namespace KpKey
