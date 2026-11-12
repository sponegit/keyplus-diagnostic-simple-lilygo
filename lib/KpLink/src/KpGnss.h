#pragma once
// ===========================================================================
// KpGnss — 서브 외장 GNSS(MAX-M10S) 위치 → 메인 (작업서 S7 2단계)
//
// 서브가 TinyGPSPlus 해석 결과를 GNSS 프레임으로 주기 발행한다(최신값만 의미 — ACK 없음,
// 유실 허용 §3.3. fix 를 얻거나 잃으면 주기를 기다리지 않고 바로 보낸다).
// 메인은 최신값을 들고 있다가 telemetry `gps{}`·시각 기준(Clk SRC_GPS)으로 쓴다.
//
// GNSS payload : [flags:1][lat_e7:4 LE 부호][lon_e7:4 LE 부호][alt_dm:2 LE 부호]
//                [spd_x10:2 LE][hdop_x10:1][sat:1]
//                [year:2 LE][month:1][day:1][hour:1][minute:1][second:1]   = 22B
//   flags b0 = 위치 fix(유효·신선), b1 = UTC 시각 유효. 나머지 비트는 예약(받는 쪽이 무시).
//   fix 가 아니면 위치 필드는 0 이어도 된다 — 받는 쪽은 b0 만 보고 판단한다.
//   길이가 더 길면 뒤는 무시한다(필드를 뒤에 덧붙여 늘릴 수 있게).
//
// 단위: lat/lon = 도 × 1e7(≈1.1cm), alt = 0.1m, spd = 0.1km/h, hdop = 0.1(255 = 25.5 이상).
//
// Arduino 의존이 없다 — 호스트에서 단위 테스트한다(test/test_kpgnss).
// ===========================================================================
#include <stddef.h>
#include <stdint.h>

#define KPGNSS_PAYLOAD_LEN   22

namespace KpGnss {

enum Flag : uint8_t {
    FIX     = 0x01,
    TIME_OK = 0x02,
};

struct Values {
    uint8_t  flags;
    int32_t  latE7;
    int32_t  lonE7;
    int16_t  altDm;
    uint16_t spdX10;
    uint8_t  hdopX10;
    uint8_t  sat;
    uint16_t year;
    uint8_t  month, day, hour, minute, second;

    bool fix() const    { return (flags & FIX) != 0; }
    bool timeOk() const { return (flags & TIME_OK) != 0; }
};

size_t encode(const Values& v, uint8_t* out, size_t cap);
bool   decode(const uint8_t* p, size_t len, Values* out);   // 길이가 모자라면 false

// 부동소수 측정값 → Values. 범위를 넘는 값은 표현 가능한 끝값으로 자른다
// (위도 ±90·경도 ±180 밖, 음의 속도·HDOP 는 0, HDOP 25.5 초과는 255).
// 시각은 year 가 0 이면 TIME_OK 를 세우지 않는다.
Values make(bool fix, double lat, double lon, float altM, float kmph, float hdop, uint32_t sat,
            uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second);

}  // namespace KpGnss
