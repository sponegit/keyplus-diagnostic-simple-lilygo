/**
 * @file      fast.h
 * @brief     고빈도(1Hz) OBD 샘플 창 + 오프라인 창 집계
 *
 * 30초 주기 telemetry 는 순간값 스냅샷이라 그 사이의 가속·제동·공회전이 전부 사라진다.
 * 차속·회전수·스로틀만 1Hz 로 모아 창(window) 단위로 1건 발행한다.
 *   - 온라인: 원본 창을 `v1/{id}/telemetry/fast` 로 발행 → 이벤트 판정은 **서버**가 한다
 *            (임계값 튜닝에 OTA 불필요).
 *   - 오프라인: 원본은 버리고(링 2,336건이 1Hz 면 39분에 소진) **창 집계 8바이트만**
 *            오프라인 레코드에 실어 백필한다.
 *
 * ⚠️ 원본 링과 집계 누산기는 **독립**이다. 오프라인 정차 창은 120초라 최대 120샘플로
 *    FAST_WINDOW_MAX(64)를 넘는다(공회전 중이면 CAN 링크가 살아 실제로 발생한다).
 *    원본 링을 훑어 집계하면 가장 오래된 56샘플이 조용히 빠진다 →
 *    tick() 이 샘플마다 곧바로 누산하고, 원본 링은 온라인 발행 전용으로만 쓴다.
 *    리셋 시점도 분리한다 — 링은 reset()(발행 시), 누산기는 takeAggregate()(적재 시).
 *
 * 계약(SSOT): packages/shared/src/protocol/telemetry.ts
 *   TelemetryFastPayload {t0,dt,n,o[],spd[],rpm[],thr[]} / AggSample {w,n,spd_max,...}
 * 설계: offline-telemetry-buffer.md §6
 */
#pragma once

#include <Arduino.h>
#include "obd2.h"

namespace Fast {

// 창 원본 샘플. 온라인 발행 전용(오프라인에는 싣지 않는다).
struct Sample {
    uint8_t  off;   // 창 시작(t0) 기준 초 오프셋 — loop 지터로 건너뛴 초가 있다
    uint8_t  spd;   // km/h
    uint16_t rpm;
    uint8_t  thr;   // %
};

/**
 * 창 집계 — 오프라인 레코드의 agg_* 8바이트로 그대로 들어간다(buffer.h Record 참조).
 * n = 0 이면 "창에 샘플이 하나도 없었다"(시동 OFF·CAN 링크 없음)는 뜻이고,
 * 이때 백필 페이로드는 agg 오브젝트를 **통째로 생략**한다 — 0 으로 채워 보내면
 * 서버가 "정지·무회전"을 관측값으로 오인한다.
 */
struct Aggregate {
    uint8_t n            = 0;   // 창에서 실제 수집된 샘플 수(신뢰도)
    uint8_t spd_max      = 0;   // km/h
    uint8_t spd_min      = 0;   // km/h
    uint8_t rpm_max_x100 = 0;   // 회전수 ÷ 100 (상한 25,500 rpm)
    int8_t  accel_x10    = 0;   // 창 최대 가속 m/s² × 10 (±12.7)
    int8_t  decel_x10    = 0;   // 창 최대 감속 m/s² × 10 (음수)
    uint8_t ev           = 0;   // b0 급가속 b1 급감속 b2 과속 b3 공회전
    uint8_t ev_n         = 0;   // 창 내 이벤트 건수(상승엣지 기준)
};

// AggSample.ev 비트 — shared/telemetry.ts AggEventBit 와 같은 값이어야 한다.
enum EvBit : uint8_t {
    EV_HARSH_ACCEL = 1 << 0,
    EV_HARSH_BRAKE = 1 << 1,
    EV_OVERSPEED   = 1 << 2,
    EV_IDLING      = 1 << 3,
};

// 1Hz 샘플러. loop 매 틱 호출(내부에서 주기를 스스로 판정한다).
// 접속 여부와 무관하게 돈다 — 오프라인에서도 집계는 만들어야 하고, CAN 은 모뎀과 별개 버스라
// 모뎀 부담이 0 이다. Cfg::fastMs()==0 이면 아무 것도 하지 않는다(원격 비활성화).
// obd 는 직전 전체 폴 결과 — 링크가 살아있을 때만(obd.valid) 고빈도 폴을 돌리는 게이트다.
void tick(uint32_t now, const Obd2::Data &obd, Stream &log);

// 현재 창에 모인 원본 샘플 수.
int count();

// 원본 창 접근 — 발행 직전에 부른다. t0Out 은 창 시작 epoch(Clk::now 기준, 0=시각 미상).
const Sample *window(int &nOut, uint32_t &t0Out);

// 원본 링 비움(온라인 발행 후). ⚠️ 누산기는 건드리지 않는다.
// 발행 실패해도 부른다 — 쌓아두면 다음 창과 섞여 o 오프셋이 무의미해진다.
void reset();

// 누산기 스냅샷 + 리셋(오프라인 적재 시점). ⚠️ 원본 링은 건드리지 않는다.
Aggregate takeAggregate();

/**
 * 누산기만 버린다 — **온라인 창을 발행한 직후**에 부른다.
 *
 * ⚠️ 이걸 부르지 않으면 누산기가 온라인 구간 내내 자라고, 그 뒤 처음 오프라인으로
 *    떨어지는 레코드의 agg 가 "직전 창"이 아니라 "온라인 세션 전체"를 요약하게 된다
 *    (spd_max·ev 가 몇 시간 전 값, n 은 255 포화). 서버는 그것을 30초 창으로 해석한다.
 *    온라인 구간의 이벤트는 서버가 1Hz 원본으로 판정하므로 이 집계는 버려도 무손실이다.
 */
void dropAggregate();

// 진단 콘솔 'fast' — 현재 창/누산 중 집계/마지막 배출 집계/누적 카운터. 처리했으면 true.
// ([STAT] 한 줄은 count() 만 쓴다 — 누적 카운터는 사람이 fast 를 쳤을 때만 본다.)
bool tryConsole(const String &cmd, const String &arg, Stream &io);

// 발행 결과 보고(카운터용). loop 가 publishFast 결과를 넘긴다.
void notePublished(bool ok);
void noteSkipped();

} // namespace Fast
