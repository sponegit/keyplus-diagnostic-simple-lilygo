/**
 * @file      carkey.cpp
 * @brief     차키 대리 모듈 구현 — KpKey::Client 를 서브 링크에 잇는다. 설계는 carkey.h.
 */
#include "carkey.h"
#include "config.h"

#if FEATURE_CARKEY

#if !FEATURE_SUBLINK
#error "v2 차키는 서브 링크로만 동작한다 — FEATURE_SUBLINK=1 필요"
#endif

#include <KpLink.h>
#include <esp_random.h>
#include "console.h"
#include "log.h"
#include "sublink.h"

namespace Carkey {

static KpKey::Client s_client;
static Owner         s_owner = Owner::REMOTE;

// 서버 명령 결과 — cmd.cpp 가 ack 를 낼 때까지 보관한다.
static bool               s_remoteReady = false;
static bool               s_remoteDone  = false;
static KpKey::Client::Fail s_remoteWhy  = KpKey::Client::Fail::NONE;

// 마지막 결과(콘솔 status 용).
static bool               s_haveLast = false;
static bool               s_lastDone = false;
static KpKey::Client::Fail s_lastWhy = KpKey::Client::Fail::NONE;
static uint32_t           s_lastAt   = 0;
static uint16_t           s_lastId   = 0;

static bool sendCmd(const uint8_t *p, size_t n, void *)
{
    return Sublink::sendFrame(KpLink::KEY_CMD, p, (uint16_t)n);
}

void begin()
{
    uint16_t first = (uint16_t)(esp_random() & 0xFFFF);
    s_client.begin(sendCmd, nullptr, first ? first : 1);
}

KpKey::Client::Start start(const KpKey::Step *steps, size_t n, Owner owner, KpKey::Error *err)
{
    KpKey::Client::Start r = s_client.start(steps, n, millis(), err);
    if (r == KpKey::Client::Start::OK) {
        s_owner = owner;
        // 실제 차키 동작은 앱 상태 전이 — 원격 명령 결과를 추적하려면 평상시에도 남아야 한다.
        LOGI(DbgConsole, "[KEY] 요청 id=%u — %u단계 %lums (첫 단계 %s %ums)\n",
             (unsigned)s_client.currentId(), (unsigned)n,
             (unsigned long)KpKey::totalMs(steps, n),
             KpKey::ioName(steps[0].io), (unsigned)steps[0].pressMs);
    }
    return r;
}

void update()
{
    s_client.update(millis());

    bool done;
    KpKey::Client::Fail why;
    uint16_t id;
    if (!s_client.takeOutcome(&done, &why, &id)) return;

    s_haveLast = true;
    s_lastDone = done;
    s_lastWhy  = why;
    s_lastAt   = millis();
    s_lastId   = id;
    if (done) LOGI(DbgConsole, "[KEY] 완료 id=%u\n", (unsigned)id);
    else      LOGW(DbgConsole, "[KEY] 실패 id=%u (%s)\n", (unsigned)id, KpKey::failName(why));

    if (s_owner == Owner::REMOTE) {
        s_remoteReady = true;
        s_remoteDone  = done;
        s_remoteWhy   = why;
    }
}

bool takeRemoteOutcome(bool *done, KpKey::Client::Fail *why)
{
    if (!s_remoteReady) return false;
    s_remoteReady = false;
    if (done) *done = s_remoteDone;
    if (why)  *why  = s_remoteWhy;
    return true;
}

bool busy() { return s_client.busy(); }

void onResult(const uint8_t *payload, size_t len)
{
    s_client.onResult(payload, len, millis());
}

void linkLost()
{
    s_client.linkLost();
}

static const char *startName(KpKey::Client::Start r)
{
    switch (r) {
        case KpKey::Client::Start::OK:        return "ok";
        case KpKey::Client::Start::BUSY:      return "다른 요청 진행 중";
        case KpKey::Client::Start::INVALID:   return "범위 밖";
        case KpKey::Client::Start::LINK_DOWN: return "서브 링크 다운";
    }
    return "?";
}

bool tryConsole(const String &cmd, const String &arg, Stream &io)
{
    uint8_t  ioNo = 0;
    String   rest = arg;
    rest.trim();

    if      (cmd == "lock")   ioNo = KPKEY_IO_LOCK;
    else if (cmd == "unlock") ioNo = KPKEY_IO_UNLOCK;
    else if (cmd == "key") {
        // key <io> [ms]
        int sp = rest.indexOf(' ');
        String first = sp < 0 ? rest : rest.substring(0, sp);
        rest = sp < 0 ? String() : rest.substring(sp + 1);
        rest.trim();
        long v = first.toInt();
        if (v < KPKEY_IO_MIN || v > KPKEY_IO_MAX) {
            io.println("[KEY] 사용법: key <io 1~6> [ms]   (1 UNLOCK 2 LOCK 3 CH3 4 START 5 OPT1 6 OPT2)");
            return true;
        }
        ioNo = (uint8_t)v;
    } else {
        return false;
    }

    long ms = rest.length() ? rest.toInt() : CARKEY_PRESS_MS;
    KpKey::Step st = {ioNo, (uint16_t)(ms < 0 ? 0 : (ms > 65535 ? 65535 : ms)), 0};
    KpKey::Error err = KpKey::Error::OK;
    KpKey::Client::Start r = start(&st, 1, Owner::CONSOLE, &err);
    if (r != KpKey::Client::Start::OK) {
        io.printf("[KEY] 거절 — %s%s%s\n", startName(r),
                  r == KpKey::Client::Start::INVALID ? ": " : "",
                  r == KpKey::Client::Start::INVALID ? KpKey::errorName(err) : "");
    }
    return true;
}

void printStatus(Stream &io)
{
    io.println("[KEY] 차키 (서브 경유)");
    io.printf("  %-14s: %s\n", "서브 링크", Sublink::up() ? "UP" : "DOWN — 요청 즉시 실패");
    if (s_client.busy()) {
        io.printf("  %-14s: id=%u %s (%s)\n", "진행", (unsigned)s_client.currentId(),
                  s_client.accepted() ? "실행 중" : "수락 대기",
                  s_owner == Owner::REMOTE ? "서버 명령" : "콘솔");
    } else {
        io.printf("  %-14s: 대기\n", "진행");
    }
    if (s_haveLast) {
        io.printf("  %-14s: id=%u %s%s%s, %lus 전\n", "마지막 결과", (unsigned)s_lastId,
                  s_lastDone ? "done" : "failed",
                  s_lastDone ? "" : " — ", s_lastDone ? "" : KpKey::failName(s_lastWhy),
                  (unsigned long)((millis() - s_lastAt) / 1000));
    }
    io.printf("  %-14s: 누름 %dms (상한 %dms, 시퀀스 %d단계·%dms)\n", "기본값",
              CARKEY_PRESS_MS, KPKEY_PRESS_MAX_MS,
              KPKEY_MAX_STEPS, KPKEY_TOTAL_MAX_MS);
}

} // namespace Carkey

#endif  // FEATURE_CARKEY
