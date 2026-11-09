#include "KpPower.h"

#include <stdio.h>
#include <stdlib.h>

namespace KpPower {

static void put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

size_t encode(const Values& v, uint8_t* out, size_t cap) {
    if (cap < KPPOWER_PAYLOAD_LEN) return 0;
    put16(out + 0, v.mv);
    put16(out + 2, (uint16_t)v.ma);
    out[4] = v.uv;
    return KPPOWER_PAYLOAD_LEN;
}

bool decode(const uint8_t* p, size_t len, Values* out) {
    if (len < KPPOWER_PAYLOAD_LEN) return false;
    out->mv = get16(p + 0);
    out->ma = (int16_t)get16(p + 2);
    out->uv = p[4];
    return true;
}

size_t formatJson(const Values& v, char* out, size_t cap) {
    // 부동소수 서식 없이(KpAir 와 같은 이유). 전압은 10mV 로 반올림한다.
    const uint32_t cv   = ((uint32_t)v.mv + 5) / 10;
    const char*    sign = v.ma < 0 ? "-" : "";
    const uint32_t a    = (uint32_t)labs((long)v.ma);
    const int w = snprintf(out, cap, "{\"v\":%lu.%02lu,\"i\":%s%lu.%03lu,\"uv\":%u}",
                           (unsigned long)(cv / 100), (unsigned long)(cv % 100), sign,
                           (unsigned long)(a / 1000), (unsigned long)(a % 1000), (unsigned)v.uv);
    if (w <= 0 || (size_t)w >= cap) return 0;
    return (size_t)w;
}

const char* uvName(Uv s) {
    switch (s) {
    case Uv::WARN: return "저전압 경고";
    case Uv::CUT:  return "소프트 차단";
    default:       return "정상";
    }
}

// 아래로 내려가면 타이머를 시작하고, 임계 + band 위로 올라와야 리셋한다.
void UvMachine::below(Timer& t, uint32_t now, uint16_t mv, uint16_t th, uint16_t band) {
    if (mv < th) {
        if (!t.run) {
            t.run = true;
            t.at  = now;
        }
    } else if ((uint32_t)mv >= (uint32_t)th + band) {
        t.run = false;
    }
}

bool UvMachine::update(uint32_t now, uint16_t mv) {
    if (hasLast_ && (uint32_t)(now - lastAt_) > p_.gapMs) warn_.run = cut_.run = rec_.run = false;
    hasLast_ = true;
    lastAt_  = now;

    below(warn_, now, mv, p_.warnMv, p_.bandMv);
    below(cut_, now, mv, p_.cutMv, p_.bandMv);
    if (mv >= p_.recoverMv) {
        if (!rec_.run) {
            rec_.run = true;
            rec_.at  = now;
        }
    } else if ((uint32_t)mv + p_.bandMv <= p_.recoverMv) {
        rec_.run = false;
    }

    Uv next = state_;
    if (state_ != Uv::CUT && cut_.elapsed(now, p_.cutMs))                next = Uv::CUT;
    else if (state_ == Uv::NORMAL && warn_.elapsed(now, p_.warnMs))      next = Uv::WARN;
    else if (state_ != Uv::NORMAL && rec_.elapsed(now, p_.recoverMs))    next = Uv::NORMAL;
    if (next == state_) return false;
    state_ = next;
    return true;
}

bool UvMachine::pending(uint32_t now, Uv* next, uint32_t* remainMs) const {
    auto left = [&](const Timer& t, uint32_t ms) {
        const uint32_t e = (uint32_t)(now - t.at);
        return e >= ms ? 0 : ms - e;
    };
    if (state_ != Uv::CUT && cut_.run) {
        *next     = Uv::CUT;
        *remainMs = left(cut_, p_.cutMs);
        return true;
    }
    if (state_ == Uv::NORMAL && warn_.run) {
        *next     = Uv::WARN;
        *remainMs = left(warn_, p_.warnMs);
        return true;
    }
    if (state_ != Uv::NORMAL && rec_.run) {
        *next     = Uv::NORMAL;
        *remainMs = left(rec_, p_.recoverMs);
        return true;
    }
    return false;
}

}  // namespace KpPower
