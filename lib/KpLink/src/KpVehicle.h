#pragma once
// ===========================================================================
// KpVehicle — 메인 → 서브 차량 상태 (위치 주기 3단계, 2026-11-12)
//
// 메인이 시동·주행 판정과 "지금 GNSS 가 필요한가"를 VEHICLE(0x48) 프레임으로 서브에 알린다.
// 서브는 GNSS_ON 비트를 그대로 따라 외장 GNSS 전원을 켜고 끈다 — 언제 켤지는 메인이 정한다.
//   · 주행 중: 항상 켬
//   · 주차 중: 다음 주차 telemetry 직전에만 켜서 fix 를 얻고, 메인이 그 fix 로 telemetry 를 낸 뒤 끔
// 서브가 스스로 주기를 세면 메인의 주차 telemetry 가 GNSS 꺼진 순간에 나가 좌표 없이 저장된다
// (서버 vehicle_state 위치가 null 로 덮인다). 그래서 타이밍의 출처를 메인 하나로 둔다.
//
// 최신값만 의미가 있다 — ACK 없음, 유실 허용(§3.3). 메인은 바뀔 때 + 주기(VEHICLE_PUB_MS)로 보낸다.
// 서브는 VEHICLE_STALE_MS 넘게 못 받으면 GNSS 를 켠다(안전 쪽 — 옛 메인·링크 끊김).
//
// VEHICLE payload : [flags:1]   길이가 더 길면 뒤는 무시한다(필드를 뒤에 덧붙여 늘릴 수 있게).
//   b0 IGN_ON  = 시동 ON (메인 ign 판정)
//   b1 DRIVING = 주행 주기(시동 ON 또는 차속·회전수 > 0, 시동 OFF 이동 포함)
//   b2 GNSS_ON = 지금 외장 GNSS 전원을 켜 둘 것
//   b3 AIR_IDLE = 공기질 듀티 쉼 — PMS5003 5V 레일을 끄고 SCD41 주기 측정을 멈춰도 된다
//                (2026-11-12 주차 공기질 듀티). **꺼도 된다는 허락**이라 0 이 안전 쪽이다 —
//                이 비트를 모르는 옛 메인은 늘 0 을 보내 센서가 계속 켜져 있다.
//                SHT41·SGP41·BMP280(3.3V 레일)·ZE15-CO 는 쉬지 않는다(SGP41 지수 학습이 1초 연속 표본을 전제,
//                CO 는 작업서 S8 "주차 중에도 ON").
//                주차 중 메인이 다음 주차 telemetry(미접속이면 다음 오프라인 적재) AIR_WAKE_LEAD_MS 전에 내린다.
//   나머지 비트는 예약 — 받는 쪽이 버린다.
//
// Arduino 의존이 없다 — 호스트에서 단위 테스트한다(test/test_kpvehicle).
// ===========================================================================
#include <stddef.h>
#include <stdint.h>

#define KPVEHICLE_PAYLOAD_LEN 1

namespace KpVehicle {

enum Flag : uint8_t {
    IGN_ON  = 0x01,
    DRIVING = 0x02,
    GNSS_ON = 0x04,
    AIR_IDLE = 0x08,
};

struct Values {
    uint8_t flags;

    bool ignOn() const   { return (flags & IGN_ON) != 0; }
    bool driving() const { return (flags & DRIVING) != 0; }
    bool gnssOn() const  { return (flags & GNSS_ON) != 0; }
    bool airIdle() const { return (flags & AIR_IDLE) != 0; }
};

Values make(bool ignOn, bool driving, bool gnssOn, bool airIdle = false);
size_t encode(const Values& v, uint8_t* out, size_t cap);
bool   decode(const uint8_t* p, size_t len, Values* out);   // 길이가 모자라면 false

}  // namespace KpVehicle
