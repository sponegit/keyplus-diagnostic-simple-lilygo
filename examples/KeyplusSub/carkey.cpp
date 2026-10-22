/**
 * @file      carkey.cpp
 * @brief     차키 구동 구현 — KpKey::Executor ↔ GPIO4~9 ↔ 링크. 설계는 carkey.h.
 */
#include "carkey.h"
#include "config.h"
#include "link.h"

#include <KpKey.h>
#include <KpLink.h>
#include <stdlib.h>

namespace Carkey {

#if CARKEY_DRIVE_SEL == CARKEY_DRIVE_DIRECT
static const int PIN_MODE_SEL  = OUTPUT_OPEN_DRAIN;
static const int PRESS_LEVEL   = LOW;
static const int RELEASE_LEVEL = HIGH;    // 오픈드레인 HIGH = Hi-Z → fob 풀업이 복귀시킨다
#else
static const int PIN_MODE_SEL  = OUTPUT;
static const int PRESS_LEVEL   = HIGH;
static const int RELEASE_LEVEL = LOW;
#endif

static const uint8_t kPins[KPKEY_IO_MAX] = {
    PIN_KEY_LOCK, PIN_KEY_UNLOCK, PIN_KEY_TRUNK, PIN_KEY_START, PIN_KEY_OPT1, PIN_KEY_OPT2,
};

static KpKey::Executor s_exec;

static void setLine(uint8_t io, bool pressed, void *)
{
    if (io < KPKEY_IO_MIN || io > KPKEY_IO_MAX) return;
    digitalWrite(kPins[io - 1], pressed ? PRESS_LEVEL : RELEASE_LEVEL);
    digitalWrite(PIN_STATUS_LED, pressed ? LED_ON_LEVEL : !LED_ON_LEVEL);
    Serial.printf("[KEY] %s %s\n", KpKey::ioName(io), pressed ? "press" : "release");
}

static void reply(const uint8_t *p, size_t n, void *)
{
    // 링크 다운이면 못 보낸다 — 메인은 재전송하거나 실패로 끝낸다.
    Link::sendFrame(KpLink::KEY_RESULT, p, (uint16_t)n);
    uint16_t id; KpKey::Status s; uint8_t d;
    if (KpKey::decodeResult(p, n, &id, &s, &d))
        Serial.printf("[KEY] 결과 id=%u %s%s%s\n", (unsigned)id, KpKey::statusName(s),
                      s == KpKey::Status::REJECTED ? " — " : "",
                      s == KpKey::Status::REJECTED ? KpKey::errorName((KpKey::Error)d) : "");
}

void begin()
{
    for (uint8_t pin : kPins) {
        digitalWrite(pin, RELEASE_LEVEL);   // 레벨 먼저 — carkey.h 주석
        pinMode(pin, PIN_MODE_SEL);
    }
    digitalWrite(PIN_STATUS_LED, !LED_ON_LEVEL);
    pinMode(PIN_STATUS_LED, OUTPUT);
    s_exec.begin(setLine, reply, nullptr);
}

void update() { s_exec.update(millis()); }

void onCmd(const uint8_t *payload, size_t len) { s_exec.onCmd(payload, len, millis()); }

void forget() { s_exec.forget(); }

void abort()
{
    s_exec.abort();
    // 실행기 밖에서도 확실히 — 모든 채널을 뗀다.
    for (uint8_t pin : kPins) digitalWrite(pin, RELEASE_LEVEL);
    digitalWrite(PIN_STATUS_LED, !LED_ON_LEVEL);
}

bool tryConsole(const char *line)
{
    if (strncmp(line, "key", 3) != 0 || (line[3] != '\0' && line[3] != ' ')) return false;
    const char *a = line + 3;
    while (*a == ' ') a++;
    if (!*a) { printStatus(Serial); return true; }
    if (!strcmp(a, "abort")) { abort(); Serial.println("[KEY] 중단 — 모두 뗌"); return true; }

    char *end = nullptr;
    long io = strtol(a, &end, 10);
    long ms = CARKEY_PRESS_MS;
    if (end && *end == ' ') ms = strtol(end, nullptr, 10);
    KpKey::Step st = {(uint8_t)(io < 0 || io > 255 ? 0 : io),
                      (uint16_t)(ms < 0 ? 0 : (ms > 65535 ? 65535 : ms)), 0};
    bool busy = false;
    KpKey::Error e = s_exec.startLocal(&st, 1, millis(), &busy);
    if (e != KpKey::Error::OK) {
        Serial.printf("[KEY] 거절 — %s (사용법: key <io 1~6> [ms] | key abort)\n", KpKey::errorName(e));
    } else if (busy) {
        Serial.println("[KEY] 거절 — 다른 시퀀스 진행 중");
    }
    return true;
}

void printStatus(Stream &io)
{
    const KpKey::Executor::Stats &st = s_exec.stats();
    io.println("[KEY] 차키");
    io.printf("  drive        : %s\n", CARKEY_DRIVE_SEL == CARKEY_DRIVE_DIRECT
                                         ? "DIRECT (open-drain, LOW=press)" : "MOSFET (HIGH=press)");
    if (s_exec.busy())
        io.printf("  state        : 실행 중 — 단계 %u/%u, 누름 %s\n",
                  (unsigned)s_exec.stepIndex() + 1, (unsigned)s_exec.stepCount(),
                  s_exec.pressedIo() ? KpKey::ioName(s_exec.pressedIo()) : "-");
    else
        io.println("  state        : 대기");
    io.printf("  executed %lu, duplicate %lu, busy %lu, rejected %lu, aborted %lu\n",
              (unsigned long)st.executed, (unsigned long)st.duplicates, (unsigned long)st.busy,
              (unsigned long)st.rejected, (unsigned long)st.aborted);
}

} // namespace Carkey
