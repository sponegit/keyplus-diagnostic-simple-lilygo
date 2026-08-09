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
// miss 바이트는 rpm(uint16_t) 정렬이 남긴 꼬리 패딩에 들어가 크기가 그대로다.
// config.h 의 "RAM 384B" 주석이 계속 맞는지 컴파일 타임에 못박는다 — 필드를 더 늘리면
// 여기서 먼저 걸린다.
static_assert(sizeof(Sample) == 6, "Sample 크기 변경 — config.h FAST_WINDOW_MAX 주석의 RAM 산정도 같이 고칠 것");
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
// s_nextSampleMs 는 "다음 샘플을 뜰 격자 시각"이다. 마지막 샘플 시각이 아니다 —
// 이 구분이 페이싱의 전부다(tick 의 페이싱 주석 참조).
static uint32_t s_nextSampleMs = 0;
static bool     s_scheduled    = false;   // 격자 기준을 한 번이라도 잡았는가
static int      s_missStreak   = 0;
static bool     s_aborted      = false;   // 연속 무응답으로 중단됨(링크 재확립까지)
static bool     s_prevObdValid = false;

// ── 시동 OFF 게이트 ─────────────────────────────────────────────────────────
// CAN 링크는 살아 있는데 엔진만 꺼진 구간(키 ACC/ON, 또는 시동 OFF 후 버스가 잠들기
// 전)을 걸러낸다. obd.valid 만 보던 0.3.8 까지는 이 구간이 통째로 수집됐다.
static int      s_offStreak    = 0;
static bool     s_engineOff    = false;   // 중지 래치(rpm 회복까지)

// ── 누적 카운터 ─────────────────────────────────────────────────────────────
static uint32_t s_cSampled = 0, s_cMissed = 0, s_cPub = 0, s_cPubFail = 0, s_cSkip = 0;
// 수집은 됐지만 개별 PID 가 비어 있던 초. 링크 품질 지표라 폴 무응답(s_cMissed)과 나눠 센다.
static uint32_t s_cRpmMiss = 0, s_cThrMiss = 0;
static Aggregate s_lastAgg;             // 마지막으로 배출된 집계(콘솔 표시용)
static bool      s_haveLastAgg = false;

/**
 * 슬롯을 잃었을 때(폴 실패·차속 미응답) 다음 격자까지 1초를 통째로 버리지 않는다.
 *
 * ⚠️ 격자를 **당기기만** 한다 — 다음 격자를 넘겨 미루지 않는다. 격자 자체는 그대로
 *    두고 그 안에서 한 번 더 두드리는 것이라, 성공하면 그 초의 샘플을 건지고
 *    실패해도 원래 격자에 복귀한다. 재시도분이 격자보다 500ms 이내로 당겨지므로
 *    o 오프셋(반올림 초)은 그대로다.
 *
 * ⚠️ 시동 OFF 중지 상태에서는 당기지 않는다. 그 구간의 간격은 격자가 아니라 재시동
 *    확인 주기(FAST_ENGINE_OFF_PROBE_MS)라, 여기서 당기면 주차 내내 150ms 폴이 된다 —
 *    특히 차속 미응답 경로는 시동 판정 앞에서 되돌아가므로 래치가 스스로 풀리지도 않는다.
 */
static void retrySoon(uint32_t now)
{
    if (s_engineOff) return;
    const uint32_t r = now + FAST_RETRY_MS;
    if ((int32_t)(r - s_nextSampleMs) < 0) s_nextSampleMs = r;
}

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
    // 시동 래치도 같이 푼다 — 링크가 새로 잡혔다면 재시동일 가능성이 높고,
    // 남겨두면 rpm 을 한 번도 안 읽은 채 중지 상태로 굳는다.
    if (obd.valid && !s_prevObdValid) {
        s_aborted    = false;
        s_missStreak = 0;
        s_engineOff  = false;
        s_offStreak  = 0;
    }
    s_prevObdValid = obd.valid;

    // 게이트 ①: 직전 전체 폴이 valid 였을 때만 돈다(CAN 링크).
    if (!obd.valid || s_aborted) return;

    // ── 고정 격자 페이싱 ────────────────────────────────────────────────────
    // 260806 실측(fast_ms=1000): 주행 중 33샘플/분(1.8초), 정차 중 58샘플/분(1.03초).
    // 서버 harsh_accel 기본 룰이 min_s=2 라 1.8초 간격에서는 임계 지속을 못 채운다 —
    // 주행 이벤트가 한 건도 안 나온 직접 원인이다.
    //
    // 손실이 두 갈래인데 원인이 다르다. 둘 다 고쳐야 1Hz 가 된다.
    //   ① 지터 가산 — 다음 시각을 now 로 되잡으면 실효 주기가 periodMs + 그 틱의 loop
    //      지터가 된다(delay(50) + readFast 60ms ≈ 0.1초). 여기서 고친다.
    //   ② 슬롯 유실 — 폴 실패·차속 미응답이 슬롯을 통째로 쓰고 다음 격자까지 1초를
    //      버린다. 정차 58 vs 주행 33 의 격차는 대부분 이쪽이다(주행 중 ECU 가 전체
    //      폴과 겹쳐 더 자주 빠진다). 아래 retrySoon 이 담당한다.
    // → 격자는 직전 격자 + periodMs 로 전진시킨다. 한 틱이 늦게 와도 다음 격자가
    //   앞당겨져 평균 주기가 periodMs 로 유지된다.
    if (!s_scheduled) {
        s_nextSampleMs = now;
        s_scheduled    = true;
    }
    if ((int32_t)(now - s_nextSampleMs) < 0) return;

    s_nextSampleMs += periodMs;
    // 한 주기 넘게 밀렸으면(Gps::begin 15초 블로킹 등) 격자를 재동기화한다.
    // 그대로 두면 밀린 초만큼 매 loop 마다 몰아치기 폴이 나간다.
    if ((int32_t)(now - s_nextSampleMs) >= 0) s_nextSampleMs = now + periodMs;

    Obd2::FastSample fs;
    if (!Obd2::readFast(fs, log)) {
        s_cMissed++;
        if (++s_missStreak >= FAST_MISS_ABORT) {
            s_aborted = true;
            LOGI(log, "[FAST] 연속 무응답 %d회 — 고빈도 폴 중단(링크 재확립 대기)\n",
                 s_missStreak);
            return;
        }
        retrySoon(now);   // 아직 중단 전 — 이 슬롯을 버리지 않는다
        return;
    }
    s_missStreak = 0;

    // 차속이 없으면 이벤트·가감속을 만들 수 없다. 회전수만 있는 창은 의미가 옅어
    // 샘플로 치지 않는다(집계 n 이 신뢰도 지표라 부풀리면 안 된다).
    // ⚠️ 다만 이 초를 통째로 포기하지는 않는다 — 차속 미응답은 전체 폴과 겹쳤을 때
    //    산발적으로 나므로 짧게 한 번 더 두드리면 대개 건진다.
    if (!fs.has_speed) {
        retrySoon(now);
        return;
    }

    const uint8_t  spd = fs.speed;
    const uint16_t rpm = fs.has_rpm ? fs.rpm : 0;
    const uint8_t  thr = fs.has_throttle ? fs.throttle : 0;

    // ⚠️ 0 을 그대로 싣지 않고 결측 여부를 따로 들고 간다. 미응답 PID 를 0 으로
    //    적으면 DB 에서 "엔진 정지"와 "값 못 읽음"이 구분되지 않는다.
    uint8_t miss = 0;
    if (!fs.has_rpm)      miss |= MISS_RPM;
    if (!fs.has_throttle) miss |= MISS_THR;

    // ── 게이트 ②: 시동 OFF ─────────────────────────────────────────────────
    // ⚠️ **rpm 이 확정 0** 일 때만 시동 OFF 로 읽는다. 결측(has_rpm=false)을 0 으로
    //    보면 링크가 불안정한 주행 구간을 시동 OFF 로 오판해 수집이 멈춘다 —
    //    결측을 0 과 구분하는 위 처리가 이 판정의 전제다.
    // 실측(260806): 13:06 시동 OFF 후 13:34 까지 28분간 spd=0·rpm=0 샘플이 계속
    // 쌓였다(그 시간대 1,942건 중 1,678건 = 86%). 차량 CAN 이 시동 뒤에도 한동안
    // 깨어 있어 obd.valid 게이트로는 걸러지지 않는다.
    const bool engineOffNow = fs.has_rpm && fs.rpm == 0 && spd == 0;
    if (engineOffNow) {
        if (s_offStreak < FAST_ENGINE_OFF_STOP_N) s_offStreak++;
        if (s_offStreak >= FAST_ENGINE_OFF_STOP_N && !s_engineOff) {
            s_engineOff = true;
            LOGI(log, "[FAST] 시동 OFF %d샘플 연속 — 고빈도 수집 중지(rpm 회복 시 재개)\n",
                 s_offStreak);
        }
    } else {
        if (s_engineOff) LOGI(log, "[FAST] rpm 회복 — 고빈도 수집 재개\n");
        s_offStreak = 0;
        s_engineOff = false;
    }
    if (s_engineOff) {
        // 중지 중에도 재시동은 봐야 하므로 폴 자체는 유지하되 간격을 늘린다
        // (전체 폴과 같은 주기 — 그보다 촘촘히 봐야 할 이유가 없다).
        s_nextSampleMs = now + FAST_ENGINE_OFF_PROBE_MS;
        return;
    }

    s_cSampled++;
    if (miss & MISS_RPM) s_cRpmMiss++;
    if (miss & MISS_THR) s_cThrMiss++;

    // ── 원본 링(온라인 발행 전용) ───────────────────────────────────────────
    startWindowIfNeeded(now);
    if (s_n < FAST_WINDOW_MAX) {
        uint32_t offS = (now - s_t0Ms + 500UL) / 1000UL;   // 반올림 — 지터로 밀린 초 보정
        if (offS > 255) offS = 255;
        s_win[s_n].off  = (uint8_t)offS;
        s_win[s_n].spd  = spd;
        s_win[s_n].rpm  = rpm;
        s_win[s_n].thr  = thr;
        s_win[s_n].miss = miss;
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
    // ⚠️ rpm 결측 초는 집계에 넣지 않는다(0 으로 넣어도 max 는 안 흔들리지만,
    //    "읽은 적 없음"이 rpm_max=0 으로 보이면 오프라인 백필에서 시동 상태를 잘못 읽는다).
    if (!(miss & MISS_RPM)) {
        uint8_t rpmX100 = (uint8_t)(rpm / 100 > 255 ? 255 : rpm / 100);
        if (rpmX100 > s_agg.rpm_max_x100) s_agg.rpm_max_x100 = rpmX100;
    }
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
    // 공회전은 rpm 을 실제로 읽은 초에서만 판정한다(결측 0 은 공회전이 아니다).
    if (spd == 0 && !(miss & MISS_RPM) && rpm > FAST_IDLE_RPM_MIN) nowActive |= EV_IDLING;

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
        const Sample &last = s_win[s_n - 1];
        char rpmBuf[8], thrBuf[8];
        if (last.miss & MISS_RPM) snprintf(rpmBuf, sizeof(rpmBuf), "-");
        else                      snprintf(rpmBuf, sizeof(rpmBuf), "%u", last.rpm);
        if (last.miss & MISS_THR) snprintf(thrBuf, sizeof(thrBuf), "-");
        else                      snprintf(thrBuf, sizeof(thrBuf), "%u", last.thr);
        io.printf("  %-14s: off=%u spd=%u rpm=%s thr=%s  (- = 결측)\n", "마지막 샘플",
                  last.off, last.spd, rpmBuf, thrBuf);
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
    io.printf("  %-14s: rpm %lu / thr %lu\n", "PID 결측",
              (unsigned long)s_cRpmMiss, (unsigned long)s_cThrMiss);
    io.printf("  %-14s: ok %lu / 실패 %lu / 생략 %lu\n", "누적 발행",
              (unsigned long)s_cPub, (unsigned long)s_cPubFail, (unsigned long)s_cSkip);
    io.printf("  %-14s: %s\n", "폴 상태",
              s_aborted   ? "중단(연속 무응답 — 링크 재확립 대기)"
              : s_engineOff ? "중지(시동 OFF — rpm 회복 시 재개)"
                            : "정상");
    io.printf("  %-14s: 급가속 %.1f / 급감속 %.1f / 과속 %d / 공회전 %drpm\n", "오프라인 임계",
              FAST_HARSH_ACCEL_MS2_X10 / 10.0f, FAST_HARSH_DECEL_MS2_X10 / 10.0f,
              FAST_OVERSPEED_KMH, FAST_IDLE_RPM_MIN);
    return true;
}

} // namespace Fast
