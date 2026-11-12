#include "KpAir.h"

#include <stdio.h>
#include <stdlib.h>

namespace KpAir {


static void put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

size_t encode(const Values& v, uint8_t* out, size_t cap) {
    if (cap < KPAIR_PAYLOAD_LEN) return 0;
    put16(out + 0, v.mask);
    put16(out + 2, (uint16_t)v.t10);
    put16(out + 4, v.rh10);
    put16(out + 6, v.p10);
    put16(out + 8, v.co2);
    put16(out + 10, v.voc);
    put16(out + 12, v.nox);
    put16(out + 14, v.pm1);
    put16(out + 16, v.pm25);
    put16(out + 18, v.pm10);
    put16(out + 20, v.co10);
    out[22] = v.guard;
    return KPAIR_PAYLOAD_LEN;
}

bool decode(const uint8_t* p, size_t len, Values* out) {
    if (len < KPAIR_PAYLOAD_LEN) return false;
    out->mask  = get16(p + 0) & WIRE_BITS;   // MAX 는 메인 쪽 값 — 프레임에 서 있어도 버린다
    out->t10   = (int16_t)get16(p + 2);
    out->rh10  = get16(p + 4);
    out->p10   = get16(p + 6);
    out->co2   = get16(p + 8);
    out->voc   = get16(p + 10);
    out->nox   = get16(p + 12);
    out->pm1   = get16(p + 14);
    out->pm25  = get16(p + 16);
    out->pm10  = get16(p + 18);
    out->co10  = get16(p + 20);
    out->guard = p[22];
    out->co10_max = 0;
    out->pm25_max = 0;
    return true;
}

// 0.1 단위 정수를 "123.4" / "-0.5" 로 쓴다 — 부동소수 서식 없이(반올림 오차·%f 미지원 libc 회피).
static int put10(char* out, size_t cap, const char* sep, const char* key, int32_t v10) {
    const char*    sign = v10 < 0 ? "-" : "";
    const uint32_t a    = (uint32_t)labs((long)v10);
    return snprintf(out, cap, "%s\"%s\":%s%lu.%lu", sep, key, sign,
                    (unsigned long)(a / 10), (unsigned long)(a % 10));
}

size_t formatJson(const Values& v, char* out, size_t cap) {
    if (!(v.mask & WIRE_BITS) || cap < 3) return 0;   // MAX 만 선 건 담을 값이 없다
    size_t n = 0;
    const char* sep = "";
    auto room = [&]() { return n < cap ? cap - n : 0; };
    auto adv = [&](int w) {
        if (w > 0) n += (size_t)w;
        sep = ",";
    };

    adv(snprintf(out, cap, "{"));
    sep = "";
    if (v.mask & T)     adv(put10(out + n, room(), sep, "t", v.t10));
    if (v.mask & RH)    adv(put10(out + n, room(), sep, "rh", v.rh10));
    if (v.mask & P)     adv(put10(out + n, room(), sep, "p", v.p10));
    if (v.mask & CO2)   adv(snprintf(out + n, room(), "%s\"co2\":%u", sep, (unsigned)v.co2));
    if (v.mask & VOC)   adv(snprintf(out + n, room(), "%s\"voc\":%u", sep, (unsigned)v.voc));
    if (v.mask & NOX)   adv(snprintf(out + n, room(), "%s\"nox\":%u", sep, (unsigned)v.nox));
    if (v.mask & PM)    adv(snprintf(out + n, room(), "%s\"pm1\":%u,\"pm25\":%u,\"pm10\":%u", sep,
                                     (unsigned)v.pm1, (unsigned)v.pm25, (unsigned)v.pm10));
    if (v.mask & CO)    adv(put10(out + n, room(), sep, "co", v.co10));
    if (v.mask & GUARD) adv(snprintf(out + n, room(), "%s\"guard\":%u", sep, (unsigned)v.guard));
    if ((v.mask & MAX) && (v.mask & CO)) adv(put10(out + n, room(), sep, "co_max", v.co10_max));
    if ((v.mask & MAX) && (v.mask & PM))
        adv(snprintf(out + n, room(), "%s\"pm25_max\":%u", sep, (unsigned)v.pm25_max));
    adv(snprintf(out + n, room(), "}"));

    if (n >= cap) return 0;   // 잘렸다
    return n;
}

// --- 창 집계 ------------------------------------------------------------------
enum Idx { I_T, I_RH, I_P, I_CO2, I_VOC, I_NOX, I_PM1, I_PM25, I_PM10, I_CO, I_N };

void reset(Window& w) {
    w.n = 0;
    for (int i = 0; i < I_N; i++) { w.cnt[i] = 0; w.sum[i] = 0; }
    w.co10_max = 0;
    w.pm25_max = 0;
    w.guard = 0;
    w.has_guard = false;
}

static void acc(Window& w, int i, int32_t v) {
    if (w.cnt[i] == 0xFFFF) return;   // 창을 오래 안 가져가도 넘치지 않게 — 그 뒤 표본은 버린다
    w.cnt[i]++;
    w.sum[i] += v;
}

void add(Window& w, const Values& v) {
    const uint16_t m = v.mask & WIRE_BITS;
    if (!m) return;
    if (w.n != 0xFFFF) w.n++;
    if (m & T)   acc(w, I_T, v.t10);
    if (m & RH)  acc(w, I_RH, v.rh10);
    if (m & P)   acc(w, I_P, v.p10);
    if (m & CO2) acc(w, I_CO2, v.co2);
    if (m & VOC) acc(w, I_VOC, v.voc);
    if (m & NOX) acc(w, I_NOX, v.nox);
    if (m & PM) {
        acc(w, I_PM1, v.pm1);
        acc(w, I_PM25, v.pm25);
        acc(w, I_PM10, v.pm10);
        if (v.pm25 > w.pm25_max) w.pm25_max = v.pm25;
    }
    if (m & CO) {
        acc(w, I_CO, v.co10);
        if (v.co10 > w.co10_max) w.co10_max = v.co10;
    }
    if (m & GUARD) { w.guard = v.guard; w.has_guard = true; }
}

// 반올림 평균 — 음수는 0 에서 멀어지는 쪽으로(서브 to10 과 같은 규칙).
static int32_t avg(const Window& w, int i) {
    const int64_t s = w.sum[i], c = w.cnt[i];
    return (int32_t)(s >= 0 ? (s + c / 2) / c : -((-s + c / 2) / c));
}

bool take(Window& w, Values* out) {
    Values v = {};
    if (w.cnt[I_T])   { v.mask |= T;   v.t10  = (int16_t)avg(w, I_T); }
    if (w.cnt[I_RH])  { v.mask |= RH;  v.rh10 = (uint16_t)avg(w, I_RH); }
    if (w.cnt[I_P])   { v.mask |= P;   v.p10  = (uint16_t)avg(w, I_P); }
    if (w.cnt[I_CO2]) { v.mask |= CO2; v.co2  = (uint16_t)avg(w, I_CO2); }
    if (w.cnt[I_VOC]) { v.mask |= VOC; v.voc  = (uint16_t)avg(w, I_VOC); }
    if (w.cnt[I_NOX]) { v.mask |= NOX; v.nox  = (uint16_t)avg(w, I_NOX); }
    if (w.cnt[I_PM25]) {
        v.mask |= PM | MAX;
        v.pm1  = (uint16_t)avg(w, I_PM1);
        v.pm25 = (uint16_t)avg(w, I_PM25);
        v.pm10 = (uint16_t)avg(w, I_PM10);
        v.pm25_max = w.pm25_max;
    }
    if (w.cnt[I_CO]) {
        v.mask |= CO | MAX;
        v.co10 = (uint16_t)avg(w, I_CO);
        v.co10_max = w.co10_max;
    }
    if (w.has_guard) { v.mask |= GUARD; v.guard = w.guard; }
    reset(w);
    *out = v;
    return v.mask != 0;
}

}  // namespace KpAir
