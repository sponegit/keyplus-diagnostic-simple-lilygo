#pragma once
// ===========================================================================
// KpLink::Session — 링크 전송 계층 (작업서 §3.3 · S2)
//
// 메인·서브가 같은 클래스를 쓴다. UART 도 millis() 도 직접 만지지 않는다 —
// 바이트는 호출측이 넣어 주고(onBytes), 송신은 콜백으로 내보내고, 시각은 인자로 받는다.
// 그래서 호스트에서 두 인스턴스를 메모리 파이프로 이어 시험할 수 있다(test/test_session).
//
// 연결 확인(HELLO) — 3번 주고받으면 양쪽이 올라온다:
//   A: HELLO(seen=0, up=0) → B          B 는 A 를 알게 됨, 응답
//   B: HELLO(seen=1, up=0) → A          A 는 "B 가 나를 안다" → 링크 업, 응답
//   A: HELLO(seen=1, up=1) → B          B 도 링크 업. 상대가 up 이라 응답하지 않음
// 규칙: 받은 HELLO 의 seen=1 이면 업, 받은 HELLO 의 up=0 이면 즉시 HELLO 로 응답.
// 링크 업 상태에서 seen=0 HELLO 가 오면 상대가 우리를 잊은 것 → 다운 후 다시 연결.
//   boot=1 이면 상대가 재부팅(PEER_RESTART), 아니면 상대 쪽 타임아웃(PEER_LOST — 예: 메인이
//   LTE 등록으로 수십 초 막혀 서브가 먼저 끊은 경우).
//
// 생존: 업 상태에서 HEARTBEAT 를 주기적으로 보낸다. 유효 프레임이 LINK_TIMEOUT 동안
// 하나도 안 오면 다운, 상대를 잊고(seen=0) 처음부터 다시 연결한다.
//
// 규약 버전이 다르면 링크를 올리지 않는다. HELLO payload 의 첫 바이트(규약 버전)는
// 버전이 바뀌어도 **위치를 고정**한다 — 그래야 다른 버전끼리도 불일치를 알아챈다.
// ===========================================================================
#include <stddef.h>
#include <stdint.h>

#include "KpLink.h"

namespace KpLink {

#define KPLINK_HELLO_INTERVAL_MS     1000   // 다운 상태에서 HELLO 재전송 주기
#define KPLINK_HEARTBEAT_MS          1000   // 업 상태에서 HEARTBEAT 주기
#define KPLINK_LINK_TIMEOUT_MS       5000   // 유효 프레임 무수신 → 다운
#define KPLINK_BYTE_GAP_MS           50     // 미완성 프레임 + 새 바이트 공백 → 디코더 expire
#define KPLINK_ID_MAX                15     // fw·hw 문자열 최대 길이

enum class Role : uint8_t { MAIN = 1, SUB = 2 };

class Session {
public:
    // 송신 — 프레임 바이트를 그대로 UART 로. 반환은 쓴 바이트 수(무시해도 됨).
    typedef size_t (*WriteFn)(const uint8_t* data, size_t n, void* ctx);
    // 링크 계층이 소비하지 않은 프레임(HELLO·HEARTBEAT 외 전부)을 앱으로 넘긴다.
    typedef void (*FrameFn)(const Frame& f, void* ctx);
    // 업/다운 전이 — 로그용.
    enum class Reason : uint8_t { HANDSHAKE, TIMEOUT, PEER_RESTART, PEER_LOST };
    typedef void (*StateFn)(bool up, Reason why, void* ctx);

    void begin(Role role, const char* fw, const char* hw,
               WriteFn write, void* ctx, uint32_t now);
    void onFrame(FrameFn fn) { onFrame_ = fn; }
    void onState(StateFn fn) { onState_ = fn; }

    // 수신 바이트 투입 — 안에서 디코드·처리까지 한다. 반드시 tick() 보다 먼저 부를 것
    // (메인 loop 가 오래 막혔다 풀리면 쌓인 프레임을 먼저 처리해야 타임아웃 오판이 없다).
    void onBytes(const uint8_t* data, size_t n, uint32_t now);
    // 주기 처리 — HELLO·HEARTBEAT 송신, 타임아웃, 디코더 expire. loop 매 틱.
    void tick(uint32_t now);

    // 앱 프레임 송신. 반환 = 사용한 seq(0~255), 실패(payload 초과·미시작) 시 -1.
    // 링크 다운이어도 보낸다 — 버릴지는 호출측이 up() 을 보고 정한다.
    int send(uint8_t type, const uint8_t* payload, uint16_t len);

    bool        up() const { return up_; }
    Role        peerRole() const { return peerRole_; }
    const char* peerFw() const { return peerFw_; }
    const char* peerHw() const { return peerHw_; }
    uint8_t     peerProto() const { return peerProto_; }   // 0 = 아직 HELLO 못 받음

    struct Stats {
        uint32_t txFrames, rxFrames;   // rxFrames = 유효 프레임 전부(HELLO·HEARTBEAT 포함)
        uint32_t ups, downs;
        uint32_t timeouts, peerRestarts, peerLost;
        uint32_t protoMismatch;        // 규약 버전이 다른 HELLO 수신
        uint32_t lastUpAt;             // 마지막 업 시각(now 기준)
    };
    const Stats&          stats() const { return stats_; }
    const Decoder::Stats& rxStats() const { return dec_.stats(); }

private:
    void handle(const Frame& f, uint32_t now);
    void handleHello(const Frame& f, uint32_t now);
    void sendHello();
    void setUp(bool up, Reason why, uint32_t now);
    int  sendRaw(uint8_t type, const uint8_t* payload, uint16_t len);

    Role     role_     = Role::MAIN;
    char     fw_[KPLINK_ID_MAX + 1] = {0};
    char     hw_[KPLINK_ID_MAX + 1] = {0};
    WriteFn  write_    = nullptr;
    void*    ctx_      = nullptr;
    FrameFn  onFrame_  = nullptr;
    StateFn  onState_  = nullptr;

    Decoder  dec_;
    bool     started_  = false;
    bool     up_       = false;
    bool     seen_     = false;   // 상대 HELLO 를 받았다(=상대를 안다)
    bool     boot_     = true;    // 부팅 후 아직 한 번도 업된 적 없다 — HELLO boot 플래그
    uint8_t  txSeq_    = 0;
    uint32_t lastRxAt_   = 0;     // 마지막 유효 프레임
    uint32_t lastByteAt_ = 0;     // 마지막 수신 바이트
    uint32_t lastHelloAt_ = 0;
    uint32_t lastBeatAt_  = 0;

    Role     peerRole_  = Role::SUB;
    uint8_t  peerProto_ = 0;
    char     peerFw_[KPLINK_ID_MAX + 1] = {0};
    char     peerHw_[KPLINK_ID_MAX + 1] = {0};

    Stats    stats_ = {};
};

const char* reasonName(Session::Reason r);

// 콘솔 `status link` 본문 — 메인·서브가 같은 형식으로 출력하도록 여기서 만든다.
// 여러 줄(\n 구분)을 buf 에 쓰고 길이를 반환한다. Arduino 의존 없음.
size_t formatStatus(const Session& s, uint32_t now, char* buf, size_t cap);

}  // namespace KpLink
