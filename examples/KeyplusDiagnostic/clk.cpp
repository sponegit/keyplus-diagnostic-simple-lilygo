/**
 * @file      clk.cpp
 * @brief     시각 기준(base) 구현. 설계: offline-telemetry-buffer.md §4
 */
#include "clk.h"
#include "config.h"

namespace Clk {

// base = "이 millis() 시점에 epoch 는 이 값이었다".
static uint32_t s_baseEpoch = 0;
static uint32_t s_baseMs    = 0;
static Src      s_baseSrc   = SRC_NONE;

// 명백한 오류 시각 차단 — 모뎀 미동기 시 1970/2070 같은 값이 나온다.
static const uint32_t kEpochMin = 1704067200UL;   // 2024-01-01
static const uint32_t kEpochMax = 2871763200UL;   // 2061-01-01

void note(uint32_t epoch, Src src)
{
    if (epoch < kEpochMin || epoch > kEpochMax) return;
    if (src != SRC_GPS && src != SRC_MODEM)     return;   // 파생값으로는 base 를 갱신하지 않는다

    // 모뎀 시각이 신선한 GPS base 를 덮어써 출처가 격하되는 것을 막는다.
    // 두 소스의 오차는 초 단위라 값 자체는 어느 쪽을 써도 무방하지만, 출처(tsrc)는
    // 서버에서 시각 신뢰도 판정에 쓰이므로 더 높은 쪽을 유지한다.
    if (src == SRC_MODEM && s_baseSrc == SRC_GPS
            && (millis() - s_baseMs) < CLK_SRC_FRESH_MS) {
        return;
    }

    s_baseEpoch = epoch;
    s_baseMs    = millis();
    s_baseSrc   = src;
}

uint32_t fromCivil(int y, int m, int d, int hh, int mi, int ss)
{
    // days_from_civil (Howard Hinnant) — mqtt.cpp 의 civilToEpoch 과 같은 식이다.
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;
    return (uint32_t)(days * 86400L + hh * 3600L + mi * 60L + ss);
}

void noteCivil(int y, int mo, int d, int hh, int mi, int ss, Src src)
{
    note(fromCivil(y, mo, d, hh, mi, ss), src);
}

uint32_t now(Src *srcOut)
{
    if (s_baseSrc == SRC_NONE) {
        if (srcOut) *srcOut = SRC_NONE;
        return 0;
    }
    uint32_t elapsed = millis() - s_baseMs;
    if (srcOut) {
        // base 갱신이 최근이면 그 소스를, 오래됐으면 외삽(DERIVED)으로 표시한다.
        *srcOut = (elapsed < CLK_SRC_FRESH_MS) ? s_baseSrc : SRC_DERIVED;
    }
    return s_baseEpoch + elapsed / 1000UL;
}

Src source()
{
    if (s_baseSrc == SRC_NONE) return SRC_NONE;
    return ((millis() - s_baseMs) < CLK_SRC_FRESH_MS) ? s_baseSrc : SRC_DERIVED;
}

const char *srcName(Src s)
{
    switch (s) {
        case SRC_GPS:     return "gps";
        case SRC_MODEM:   return "modem";
        case SRC_DERIVED: return "derived";
        default:          return "none";
    }
}

} // namespace Clk
