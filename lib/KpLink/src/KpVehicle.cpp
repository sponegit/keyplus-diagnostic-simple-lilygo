#include "KpVehicle.h"

namespace KpVehicle {

static const uint8_t KNOWN = IGN_ON | DRIVING | GNSS_ON | AIR_IDLE;

Values make(bool ignOn, bool driving, bool gnssOn, bool airIdle) {
    Values v = {};
    if (ignOn)   v.flags |= IGN_ON;
    if (driving) v.flags |= DRIVING;
    if (gnssOn)  v.flags |= GNSS_ON;
    if (airIdle) v.flags |= AIR_IDLE;
    return v;
}

size_t encode(const Values& v, uint8_t* out, size_t cap) {
    if (cap < KPVEHICLE_PAYLOAD_LEN) return 0;
    out[0] = v.flags & KNOWN;
    return KPVEHICLE_PAYLOAD_LEN;
}

bool decode(const uint8_t* p, size_t len, Values* out) {
    if (len < KPVEHICLE_PAYLOAD_LEN) return false;
    out->flags = p[0] & KNOWN;   // 예약 비트는 버린다
    return true;
}

}  // namespace KpVehicle
