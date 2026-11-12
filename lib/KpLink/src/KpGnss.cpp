#include "KpGnss.h"

#include <math.h>

namespace KpGnss {

static void put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint16_t get16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t encode(const Values& v, uint8_t* out, size_t cap) {
    if (cap < KPGNSS_PAYLOAD_LEN) return 0;
    out[0] = v.flags;
    put32(out + 1, (uint32_t)v.latE7);
    put32(out + 5, (uint32_t)v.lonE7);
    put16(out + 9, (uint16_t)v.altDm);
    put16(out + 11, v.spdX10);
    out[13] = v.hdopX10;
    out[14] = v.sat;
    put16(out + 15, v.year);
    out[17] = v.month;
    out[18] = v.day;
    out[19] = v.hour;
    out[20] = v.minute;
    out[21] = v.second;
    return KPGNSS_PAYLOAD_LEN;
}

bool decode(const uint8_t* p, size_t len, Values* out) {
    if (len < KPGNSS_PAYLOAD_LEN) return false;
    out->flags   = p[0] & (FIX | TIME_OK);   // 예약 비트는 버린다
    out->latE7   = (int32_t)get32(p + 1);
    out->lonE7   = (int32_t)get32(p + 5);
    out->altDm   = (int16_t)get16(p + 9);
    out->spdX10  = get16(p + 11);
    out->hdopX10 = p[13];
    out->sat     = p[14];
    out->year    = get16(p + 15);
    out->month   = p[17];
    out->day     = p[18];
    out->hour    = p[19];
    out->minute  = p[20];
    out->second  = p[21];
    return true;
}

static double clampd(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }

Values make(bool fix, double lat, double lon, float altM, float kmph, float hdop, uint32_t sat,
            uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second) {
    Values v = {};
    if (fix) {
        v.flags |= FIX;
        v.latE7 = (int32_t)lround(clampd(lat, -90.0, 90.0) * 1e7);
        v.lonE7 = (int32_t)lround(clampd(lon, -180.0, 180.0) * 1e7);
        v.altDm = (int16_t)lround(clampd((double)altM * 10.0, -32768.0, 32767.0));
    }
    v.spdX10  = (uint16_t)lround(clampd((double)kmph * 10.0, 0.0, 65535.0));
    v.hdopX10 = (uint8_t)lround(clampd((double)hdop * 10.0, 0.0, 255.0));
    v.sat     = (uint8_t)(sat > 255 ? 255 : sat);
    if (year != 0) {
        v.flags |= TIME_OK;
        v.year   = year;
        v.month  = month;
        v.day    = day;
        v.hour   = hour;
        v.minute = minute;
        v.second = second;
    }
    return v;
}

}  // namespace KpGnss
