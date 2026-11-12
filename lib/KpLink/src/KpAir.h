#pragma once
// ===========================================================================
// KpAir — 공기질 측정값 묶음 (작업서 S8)
//
// 서브가 AIR 프레임으로 주기 발행하고(최신값만 의미 — ACK 없음, 유실 허용 §3.3),
// 메인이 telemetry 창 동안 받은 AIR 를 Window 로 모아 창 평균 + CO·PM2.5 최대를 telemetry `air{}` 로 싣는다
// (2026-11-12 — 마지막 값 하나만 실으면 창 중간의 CO·PM 순간 상승을 서버가 못 본다).
//
// 값이 없는 항목(센서 없음·안정화 중·온도 가드 차단)은 mask 비트를 내리고 생략한다.
// 0 을 실어 보내면 서버가 관측값으로 받는다(v1 OBD has_* 와 같은 규칙).
//
// AIR payload : [mask:2 LE][t:2][rh:2][p:2][co2:2][voc:2][nox:2][pm1:2][pm25:2][pm10:2][co:2][guard:1]
//               숫자는 모두 LE. 길이가 더 길면 뒤는 무시한다(필드를 뒤에 덧붙여 늘릴 수 있게).
//
// telemetry   : {"t":23.5,"rh":45.6,"p":1013.2,"co2":612,"voc":100,"nox":1,
//                "pm1":3,"pm25":5,"pm10":6,"co":1.2,"guard":0,"co_max":3.4,"pm25_max":9}
//               co_max·pm25_max 는 MAX 비트(메인 창 집계 결과에만 선다 — AIR 프레임에는 없다)일 때만.
//
// Arduino 의존이 없다 — 호스트에서 단위 테스트한다(test/test_kpair).
// ===========================================================================
#include <stddef.h>
#include <stdint.h>

#define KPAIR_PAYLOAD_LEN   23
#define KPAIR_JSON_MAX      192   // 모든 항목(+ co_max·pm25_max)이 최대 자릿수일 때도 들어가는 크기

namespace KpAir {

enum Bit : uint16_t {
    T     = 1 << 0,   // SHT41 온도
    RH    = 1 << 1,   // SHT41 습도
    P     = 1 << 2,   // BMP280 기압
    CO2   = 1 << 3,   // SCD41
    VOC   = 1 << 4,   // SGP41 지수 — 학습 전(0)이면 생략
    NOX   = 1 << 5,
    PM    = 1 << 6,   // PMS5003 pm1·pm25·pm10 셋을 함께
    CO    = 1 << 7,   // ZE15-CO
    GUARD = 1 << 8,   // 온도 가드 상태(작업서 S8 8단계)
    // ── 여기부터는 메인 쪽 값이다 — AIR 프레임으로 오가지 않는다(decode 가 지운다) ──
    MAX   = 1 << 9,   // co10_max·pm25_max 유효(창 집계). CO·PM 비트가 선 항목만 뜻이 있다
};

// AIR 프레임으로 오가는 비트.
static const uint16_t WIRE_BITS = T | RH | P | CO2 | VOC | NOX | PM | CO | GUARD;

enum class Guard : uint8_t {
    NORMAL = 0,
    HOT    = 1,       // 상한 초과로 센서 레일 차단 중
    COLD   = 2,       // 하한 미만으로 차단 중
};

struct Values {
    uint16_t mask;
    int16_t  t10;     // 0.1 °C
    uint16_t rh10;    // 0.1 %RH
    uint16_t p10;     // 0.1 hPa
    uint16_t co2;     // ppm
    uint16_t voc;     // 지수 1~500 (100 = 학습된 평소)
    uint16_t nox;     // 지수 1~500 (1 = 학습된 평소)
    uint16_t pm1, pm25, pm10;   // µg/m³ (대기환경 값)
    uint16_t co10;    // 0.1 ppm
    uint8_t  guard;   // Guard
    uint16_t co10_max;  // MAX: 창 안 CO 최대 (0.1 ppm)
    uint16_t pm25_max;  // MAX: 창 안 PM2.5 최대 (µg/m³)
};

// telemetry 창 집계 — 창 안에 받은 AIR 프레임의 항목별 평균(반올림) + CO·PM2.5 최대 + 마지막 guard.
// 항목마다 따로 센다: 주차 듀티로 PMS·CO 가 창 끝 30초만 켜졌다면 그 30초의 평균이 된다.
struct Window {
    uint16_t n;          // 받은 프레임 수
    uint16_t cnt[10];    // 항목별 표본 수 (t, rh, p, co2, voc, nox, pm1, pm25, pm10, co)
    int64_t  sum[10];
    uint16_t co10_max, pm25_max;
    uint8_t  guard;
    bool     has_guard;
};

void reset(Window& w);
void add(Window& w, const Values& v);
// 창 결과를 out 에 쓰고 창을 비운다. 담을 값이 없으면(프레임 0 · 전부 빈 mask) false — out.mask = 0.
bool take(Window& w, Values* out);

size_t encode(const Values& v, uint8_t* out, size_t cap);
// 길이가 모자라면 false. 알 수 없는 mask 비트는 지운다.
bool   decode(const uint8_t* p, size_t len, Values* out);

// telemetry `air` 객체(중괄호 포함)를 쓴다. 반환 = 쓴 길이(종단 NUL 제외), 실패 시 0.
// mask 가 0 이면 "{}" 가 아니라 0 을 돌려준다 — 호출자가 키째 생략한다.
size_t formatJson(const Values& v, char* out, size_t cap);

}  // namespace KpAir
