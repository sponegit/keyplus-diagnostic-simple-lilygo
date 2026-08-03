/**
 * @file      fast.cpp
 * @brief     고빈도 창 + 집계 구현. 설계: offline-telemetry-buffer.md §6
 */
#include "fast.h"
#include "config.h"
#include "clk.h"
#include "cfg.h"
#include "log.h"

namespace Fast {

// ── 원본 창(온라인 발행 전용) ───────────────────────────────────────────────
static Sample   s_win[FAST_WINDOW_MAX];
static int      s_n      = 0;
static uint32_t s_t0Ms   = 0;   // 창 시작 millis (오프셋 기준)
static uint32_t s_t0Ep   = 0;   // 창 시작 epoch (0 = 시각 미상)

// ── 집계 누산기(원본 링과 독립) ─────────────────────────────────────────────
static Aggregate s_agg;
static bool      s_aggAny = false;   // 누산기에 샘플이 들어간 적 있는가(min 초기화용)

// 직전 샘플 — 가감속 계산에 쓴다. 창 경계와 무관하게 이어진다(가감속은 창을 넘나든다).
static bool     s_havePrev = false;
static uint8_t  s_prevSpd  = 0;
static uint32_t s_prevMs   = 0;

// 이벤트 상승엣지 추적 — 조건이 유지되는 동안 매 샘플 세면 ev_n 이 부풀려진다.
static uint8_t  s_evActive = 0;

// ── 폴 페이싱/게이트 ────────────────────────────────────────────────────────
static uint32_t s_lastSampleMs = 0;
static int      s_missStreak   = 0;
static bool     s_aborted      = false;   // 연속 무응답으로 중단됨(링크 재확립까지)
static bool     s_prevObdValid = false;

// ── 누적 카운터 ─────────────────────────────────────────────────────────────
static uint32_t s_cSampled = 0, s_cMissed = 0, s_cPub = 0, s_cPubFail = 0, s_cSkip = 0;
static Aggregate s_lastAgg;             // 마지막으로 배출된 집계(콘솔 표시용)
static bool      s_haveLastAgg = false;

// 창 시작 시각을 잡는다. 첫 샘플이 들어갈 때만.
static void startWindowIfNeeded(uint32_t now)
{
    if (s_n > 0) return;
    s_t0Ms = now;
    s_t0Ep = Clk::now();
}

void tick(uint32_t now, const Obd2::Data &obd, Stream &log)
{
    const uint32_t periodMs = Cfg::fastMs();
    if (periodMs == 0) return;          // 원격 비활성화(fast_ms=0) — 롤아웃 안전판

    // 링크가 죽었다 살아나면 중단 래치를 푼다. 전체 폴이 링크 끊김을 감지하면
    // loop 가 g_obd 를 초기화하므로(valid=false), 그 하강엣지가 신호가 된다.
    if (obd.valid && !s_prevObdValid) {
        s_aborted    = false;
        s_missStreak = 0;
    }
    s_prevObdValid = obd.valid;

    // 게이트: 직전 전체 폴이 valid 였을 때만 돈다. 시동 OFF → CAN 잠김 →
    // 고빈도 수집 없음 → 발행도 없음(데이터량 절약).
    if (!obd.valid || s_aborted) return;

    if (s_lastSampleMs != 0 && (uint32_t)(now - s_lastSampleMs) < periodMs) return;

    // 첫 샘플이면 기준만 잡고 바로 수집한다.
    uint32_t prevSampleMs = s_lastSampleMs;
    s_lastSampleMs = now;

    Obd2::FastSample fs;
    if (!Obd2::readFast(fs, log)) {
        s_cMissed++;
        if (++s_missStreak >= FAST_MISS_ABORT) {
            s_aborted = true;
            LOGI(log, "[FAST] 연속 무응답 %d회 — 고빈도 폴 중단(링크 재확립 대기)\n",
                 s_missStreak);
        }
        return;
    }
    s_missStreak = 0;
    (void)prevSampleMs;

    // 차속이 없으면 이벤트·가감속을 만들 수 없다. 회전수만 있는 창은 의미가 옅어
    // 샘플로 치지 않는다(집계 n 이 신뢰도 지표라 부풀리면 안 된다).
    if (!fs.has_speed) return;

    const uint8_t  spd = fs.speed;
    const uint16_t rpm = fs.has_rpm ? fs.rpm : 0;
    const uint8_t  thr = fs.has_throttle ? fs.throttle : 0;

    s_cSampled++;

    // ── 원본 링(온라인 발행 전용) ───────────────────────────────────────────
    startWindowIfNeeded(now);
    if (s_n < FAST_WINDOW_MAX) {
        uint32_t offS = (now - s_t0Ms + 500UL) / 1000UL;   // 반올림 — 지터로 밀린 초 보정
        if (offS > 255) offS = 255;
        s_win[s_n].off = (uint8_t)offS;
        s_win[s_n].spd = spd;
        s_win[s_n].rpm = rpm;
        s_win[s_n].thr = thr;
        s_n++;
    }

    // ── 집계 누산기(원본 링과 독립, 창 길이 무관) ──────────────────────────
    if (!s_aggAny) {
        s_agg.spd_max = spd;
        s_agg.spd_min = spd;
        s_aggAny = true;
    } else {
        if (spd > s_agg.spd_max) s_agg.spd_max = spd;
        if (spd < s_agg.spd_min) s_agg.spd_min = spd;
    }
    uint8_t rpmX100 = (uint8_t)(rpm / 100 > 255 ? 255 : rpm / 100);
    if (rpmX100 > s_agg.rpm_max_x100) s_agg.rpm_max_x100 = rpmX100;
    if (s_agg.n < 255) s_agg.n++;

    // 가감속 — ⚠️ 실제 경과 초로 나눈다. 결측 구간을 1초로 가정하면 가짜 급가속이 나온다.
    int accelX10 = 0;
    if (s_havePrev) {
        uint32_t dtMs = now - s_prevMs;
        if (dtMs >= 500UL && dtMs <= FAST_ACCEL_MAX_GAP_MS) {
            // (Δkm/h ÷ 3.6) ÷ (Δms/1000) = m/s² → ×10 정수화 = Δkm/h × 100000 ÷ (36 × Δms)
            // 오버플로 여유: |Δspd| ≤ 255 → 최대 2.55e7 (long 범위 내).
            long dSpd = (long)spd - (long)s_prevSpd;
            accelX10 = (int)((dSpd * 100000L) / (36L * (long)dtMs));
            if (accelX10 >  127) accelX10 =  127;
            if (accelX10 < -128) accelX10 = -128;
            if (accelX10 > s_agg.accel_x10) s_agg.accel_x10 = (int8_t)accelX10;
            if (accelX10 < s_agg.decel_x10) s_agg.decel_x10 = (int8_t)accelX10;
        }
    }
    s_havePrev = true;
    s_prevSpd  = spd;
    s_prevMs   = now;

    // 이벤트 판정(오프라인 구간 한정 — 온라인은 서버가 원본으로 판정한다).
    // 상승엣지에서만 ev_n 을 센다.
    uint8_t nowActive = 0;
    if (accelX10 >= FAST_HARSH_ACCEL_MS2_X10) nowActive |= EV_HARSH_ACCEL;
    if (accelX10 <= FAST_HARSH_DECEL_MS2_X10) nowActive |= EV_HARSH_BRAKE;
    if (spd >= FAST_OVERSPEED_KMH)            nowActive |= EV_OVERSPEED;
    if (spd == 0 && rpm > FAST_IDLE_RPM_MIN)  nowActive |= EV_IDLING;

    uint8_t rising = (uint8_t)(nowActive & ~s_evActive);
    if (rising) {
        s_agg.ev |= rising;
        for (int b = 0; b < 4; b++) {
            if ((rising >> b) & 1) { if (s_agg.ev_n < 255) s_agg.ev_n++; }
        }
    }
    s_evActive = nowActive;
}

int count() { return s_n; }

const Sample *window(int &nOut, uint32_t &t0Out)
{
    nOut  = s_n;
    // 창 시작 당시 시각을 못 잡았으면(콜드 부팅 음영) 발행 시점에 소급한다.
    if (s_t0Ep == 0 && s_n > 0) {
        uint32_t nowEp = Clk::now();
        if (nowEp) s_t0Ep = nowEp - (millis() - s_t0Ms) / 1000UL;
    }
    t0Out = s_t0Ep;
    return s_win;
}

void reset()
{
    s_n    = 0;
    s_t0Ms = 0;
    s_t0Ep = 0;
}

Aggregate takeAggregate()
{
    Aggregate out = s_agg;
    if (!s_aggAny) out = Aggregate();   // 샘플 0 → 전 필드 0 (백필에서 agg 생략 신호)
    s_lastAgg     = out;
    s_haveLastAgg = true;

    // 누산기만 리셋 — 원본 링은 건드리지 않는다(리셋 시점이 다르다).
    s_agg      = Aggregate();
    s_aggAny   = false;
    s_evActive = 0;
    return out;
}

void dropAggregate()
{
    // takeAggregate 와 같은 리셋이되 스냅샷을 남기지 않는다(콘솔의 "마지막 배출"은
    // 오프라인 적재분만 보여주는 편이 진단에 유용하다).
    s_agg      = Aggregate();
    s_aggAny   = false;
    s_evActive = 0;
}

void notePublished(bool ok) { if (ok) s_cPub++; else s_cPubFail++; }
void noteSkipped()          { s_cSkip++; }

bool tryConsole(const String &cmd, const String &arg, Stream &io)
{
    if (cmd != "fast") return false;
    (void)arg;

    const uint32_t periodMs = Cfg::fastMs();
    io.println("[FAST]");
    io.printf("  %-14s: %s", "샘플 주기",
              periodMs ? "" : "0 (비활성 — config_update fast_ms 로 활성화)");
    if (periodMs) io.printf("%lums (%.1fHz)", (unsigned long)periodMs, 1000.0f / periodMs);
    io.println();
    io.printf("  %-14s: %d / %d\n", "현재 창 샘플", s_n, FAST_WINDOW_MAX);
    if (s_n > 0) {
        io.printf("  %-14s: t0=%lu (경과 %lus)\n", "창 시작",
                  (unsigned long)s_t0Ep, (unsigned long)((millis() - s_t0Ms) / 1000UL));
        io.printf("  %-14s: off=%u spd=%u rpm=%u thr=%u\n", "마지막 샘플",
                  s_win[s_n - 1].off, s_win[s_n - 1].spd, s_win[s_n - 1].rpm, s_win[s_n - 1].thr);
    }
    io.printf("  %-14s: n=%u spd=%u~%u rpm_max=%u accel=%.1f decel=%.1f ev=0x%X ev_n=%u\n",
              "누산 중", s_agg.n, s_agg.spd_min, s_agg.spd_max,
              (unsigned)s_agg.rpm_max_x100 * 100,
              s_agg.accel_x10 / 10.0f, s_agg.decel_x10 / 10.0f, s_agg.ev, s_agg.ev_n);
    if (s_haveLastAgg) {
        io.printf("  %-14s: n=%u spd=%u~%u rpm_max=%u accel=%.1f decel=%.1f ev=0x%X ev_n=%u\n",
                  "마지막 배출", s_lastAgg.n, s_lastAgg.spd_min, s_lastAgg.spd_max,
                  (unsigned)s_lastAgg.rpm_max_x100 * 100,
                  s_lastAgg.accel_x10 / 10.0f, s_lastAgg.decel_x10 / 10.0f,
                  s_lastAgg.ev, s_lastAgg.ev_n);
    }
    io.printf("  %-14s: 수집 %lu / 무응답 %lu\n", "누적 폴",
              (unsigned long)s_cSampled, (unsigned long)s_cMissed);
    io.printf("  %-14s: ok %lu / 실패 %lu / 생략 %lu\n", "누적 발행",
              (unsigned long)s_cPub, (unsigned long)s_cPubFail, (unsigned long)s_cSkip);
    io.printf("  %-14s: %s\n", "폴 상태",
              s_aborted ? "중단(연속 무응답 — 링크 재확립 대기)" : "정상");
    io.printf("  %-14s: 급가속 %.1f / 급감속 %.1f / 과속 %d / 공회전 %drpm\n", "오프라인 임계",
              FAST_HARSH_ACCEL_MS2_X10 / 10.0f, FAST_HARSH_DECEL_MS2_X10 / 10.0f,
              FAST_OVERSPEED_KMH, FAST_IDLE_RPM_MIN);
    return true;
}

} // namespace Fast
