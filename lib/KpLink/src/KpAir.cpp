#include "KpAir.h"

#include <stdio.h>
#include <stdlib.h>

namespace KpAir {

static const uint16_t KNOWN = T | RH | P | CO2 | VOC | NOX | PM | CO | GUARD;

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
    out->mask  = get16(p + 0) & KNOWN;
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
    if (!(v.mask & KNOWN) || cap < 3) return 0;
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
    adv(snprintf(out + n, room(), "}"));

    if (n >= cap) return 0;   // 잘렸다
    return n;
}

}  // namespace KpAir
