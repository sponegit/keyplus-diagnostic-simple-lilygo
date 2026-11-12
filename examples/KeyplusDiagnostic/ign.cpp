/**
 * @file      ign.cpp
 * @brief     시동 판정 구현. 설계는 ign.h.
 */
#include "ign.h"
#include "config.h"
#include "cfg.h"
#include "log.h"
#include "console.h"
#include "obd2.h"
#if FEATURE_SUBLINK
#include "sublink.h"
#endif

namespace Ign {

// ── 전압 판정 ───────────────────────────────────────────────────────────────
static bool     s_vKnown  = false;   // 표본을 하나라도 받았는가
static bool     s_vOn     = false;
static bool     s_tRun    = false;   // 전이 타이머 진행 중
static uint32_t s_tAt     = 0;
static uint32_t s_lastAt  = 0;       // 마지막으로 반영한 표본의 수신 시각(같은 표본을 두 번 넣지 않게)
static uint16_t s_lastMv  = 0;

// ── 최종 판정 ───────────────────────────────────────────────────────────────
static bool     s_on      = false;
static Src      s_src     = Src::NONE;
static uint32_t s_changes = 0;
static uint32_t s_changedAt = 0;

static void feedVolt(uint32_t at, uint16_t mv)
{
    // 표본이 오래 끊겼다가 돌아오면 진행 중이던 타이머를 버린다(끊긴 동안을 이어진 것으로 세지 않게).
    if (s_vKnown && (uint32_t)(at - s_lastAt) > IGN_GAP_MS) s_tRun = false;
    s_lastAt = at;
    s_lastMv = mv;

    if (!s_vKnown) {
        // 첫 표본은 바로 판정한다 — 주행 중 재부팅이면 곧장 ON 이어야 한다.
        s_vKnown = true;
        s_vOn    = mv >= IGN_ON_MV;
        s_tRun   = false;
        return;
    }
    const bool cross = s_vOn ? (mv < IGN_OFF_MV) : (mv >= IGN_ON_MV);
    if (!cross) { s_tRun = false; return; }
    if (!s_tRun) { s_tRun = true; s_tAt = at; }
    if ((uint32_t)(at - s_tAt) >= (s_vOn ? IGN_OFF_MS : IGN_ON_MS)) {
        s_vOn  = !s_vOn;
        s_tRun = false;
    }
}

// 회전수를 믿을 수 있는 시간. 고빈도 폴(1초)이 꺼져 있어도 전체 폴(telemetry 주기)이 채우게 둔다.
static uint32_t rpmFreshMs()
{
    const uint32_t full = Cfg::telemetryIntervalMs() + 5000;
    return full > IGN_RPM_FRESH_MS ? full : IGN_RPM_FRESH_MS;
}

bool update(uint32_t now)
{
#if FEATURE_SUBLINK
    uint16_t mv; uint32_t at;
    if (Sublink::pwrSample(mv, at) && (!s_vKnown || at != s_lastAt)) feedVolt(at, mv);
#endif
    // ⚠️ 부호 있는 비교 — 표본 시각(at)은 now 를 잡은 뒤 같은 틱에 들어와 now 보다 늦을 수 있다.
    //    (uint32_t) 로 빼면 그때 거대값이 돼 전압 근거가 순간 "끊김"으로 읽힌다(GNSS 와 같은 버그, 2026-11-12).
    const bool vFresh = s_vKnown && (int32_t)(now - s_lastAt) <= (int32_t)SUBLINK_PWR_STALE_MS;

    uint16_t rpm = 0;
    bool rpmFresh = false;
#if FEATURE_OBD2
    rpmFresh = Obd2::lastRpm(rpmFreshMs(), rpm);
#endif

    bool on;
    Src  src;
    if (rpmFresh) {
        on  = rpm > 0;
        src = Src::RPM;
        if (s_vKnown && s_vOn != on) { s_vOn = on; s_tRun = false; }   // 넘겨받을 때 튀지 않게(ign.h)
    } else if (vFresh) {
        on  = s_vOn;
        src = Src::VOLT;
    } else {
        on  = false;
        src = Src::NONE;
    }

    const bool changed = on != s_on;
    if (changed) {
        s_changes++;
        s_changedAt = now;
        if (src == Src::RPM)
            LOGI(DbgConsole, "[IGN] 시동 %s — 회전수 %u rpm\n", on ? "ON" : "OFF", rpm);
        else if (src == Src::VOLT)
            LOGI(DbgConsole, "[IGN] 시동 %s — 전압 %u.%02uV\n", on ? "ON" : "OFF",
                 s_lastMv / 1000, (s_lastMv % 1000) / 10);
        else
            LOGI(DbgConsole, "[IGN] 시동 OFF — 근거 없음(전압·회전수 둘 다 끊김)\n");
    }
    s_on  = on;
    s_src = src;
    return changed;
}

bool on()      { return s_on; }
Src  source()  { return s_src; }

const char *srcName(Src s)
{
    switch (s) {
    case Src::RPM:  return "OBD 회전수";
    case Src::VOLT: return "전압";
    default:        return "근거 없음";
    }
}

void printStatus(Stream &io)
{
    const uint32_t now = millis();
    io.printf("[IGN] 시동 %s — 근거 %s (전이 %lu회", s_on ? "ON" : "OFF", srcName(s_src),
              (unsigned long)s_changes);
    if (s_changes) io.printf(", 마지막 %lus 전", (unsigned long)((now - s_changedAt) / 1000));
    io.println(")");
    io.printf("  기준         : ON ≥%u.%02uV %lus / OFF <%u.%02uV %lus · 회전수 %lus 안이면 회전수 우선\n",
              IGN_ON_MV / 1000, (IGN_ON_MV % 1000) / 10, (unsigned long)(IGN_ON_MS / 1000),
              IGN_OFF_MV / 1000, (IGN_OFF_MV % 1000) / 10, (unsigned long)(IGN_OFF_MS / 1000),
              (unsigned long)(rpmFreshMs() / 1000));
    if (!s_vKnown)
        io.println("  전압         : 받은 적 없음 (서브 링크·INA228 확인 — status link)");
    else
        io.printf("  전압         : %u.%02uV → %s%s (%lus 전)\n", s_lastMv / 1000, (s_lastMv % 1000) / 10,
                  s_vOn ? "ON" : "OFF", s_tRun ? (s_vOn ? ", OFF 대기 중" : ", ON 대기 중") : "",
                  (unsigned long)((now - s_lastAt) / 1000));
#if FEATURE_OBD2
    uint16_t rpm; uint32_t age = 0;
    const bool fresh = Obd2::lastRpm(rpmFreshMs(), rpm, &age);
    if (!age && !fresh)
        io.println("  회전수       : 읽은 적 없음");
    else
        io.printf("  회전수       : %u rpm (%lus 전%s)\n", rpm, (unsigned long)(age / 1000),
                  fresh ? "" : " — 오래돼 안 씀");
#endif
}

bool tryConsole(const String &cmd, const String &arg, Stream &io)
{
    (void)arg;
    if (cmd != "ign") return false;
    printStatus(io);
    return true;
}

} // namespace Ign
