#pragma once
// ===========================================================================
// KpPower — 전원 측정값 + 저전압 판정 (작업서 S6, HARDWARE §2.3)
//
// 서브가 INA228 값으로 저전압 상태를 판정하고 POWER 프레임으로 주기 발행한다
// (최신값만 의미 — ACK 없음, 유실 허용 §3.3. 상태가 바뀌면 주기를 기다리지 않고 바로 보낸다).
// 메인은 최신값을 들고 있다가 telemetry `pwr{}` 로 싣는다.
//
// POWER payload : [mv:2 LE][ma:2 LE 부호 있음][uv:1]
//                 길이가 더 길면 뒤는 무시한다(필드를 뒤에 덧붙여 늘릴 수 있게).
//
// telemetry     : {"v":12.52,"i":0.045,"uv":0}   v = V(0.01), i = A(0.001, 역류면 음수)
//
// Arduino 의존이 없다 — 호스트에서 단위 테스트한다(test/test_kppower).
// ===========================================================================
#include <stddef.h>
#include <stdint.h>

#define KPPOWER_PAYLOAD_LEN   5
#define KPPOWER_JSON_MAX      48   // {"v":655.35,"i":-32.768,"uv":2} 보다 넉넉히

namespace KpPower {

enum class Uv : uint8_t {
    NORMAL = 0,
    WARN   = 1,   // 경고 — 서버 경고·센서 레일 차단
    CUT    = 2,   // 소프트 차단 — 메인이 MQTT 정상 종료 후 모뎀 off. MCU 는 끄지 않는다
};

struct Values {
    uint16_t mv;   // 차량 쪽 전압(INA228 VBUS = IN+)
    int16_t  ma;   // 장치 소비 전류(12V 쪽)
    uint8_t  uv;   // Uv
};

size_t encode(const Values& v, uint8_t* out, size_t cap);
bool   decode(const uint8_t* p, size_t len, Values* out);   // 길이가 모자라면 false

// telemetry `pwr` 객체(중괄호 포함). 반환 = 쓴 길이(종단 NUL 제외), 잘리면 0.
size_t formatJson(const Values& v, char* out, size_t cap);

const char* uvName(Uv s);

// --- 저전압 2단 상태머신 ----------------------------------------------------
//
//   NORMAL → WARN : warnMv 미만이 warnMs 동안 이어짐
//   (어디서든) → CUT : cutMv 미만이 cutMs 동안 이어짐 — WARN 을 거치지 않고 바로 올 수 있다
//   WARN·CUT → NORMAL : recoverMv 이상이 recoverMs 동안 이어짐(알터네이터 가동 확인)
//
// 크랭킹 강하(1~2초)는 지연이 걸러낸다. 임계 근처 잡음으로 타이머가 매번 리셋되지 않게
// "임계 + bandMv" 를 넘어야 하강 타이머를 리셋한다(복귀 타이머는 "임계 − bandMv" 밑으로 가야 리셋).
// 측정이 gapMs 넘게 끊겼다가 돌아오면 진행 중이던 타이머를 버린다(끊긴 동안을 이어진 것으로 세지 않게).
// 상태는 그대로 둔다.
struct UvParams {
    uint16_t warnMv;
    uint16_t cutMv;
    uint16_t recoverMv;
    uint16_t bandMv;
    uint32_t warnMs;
    uint32_t cutMs;
    uint32_t recoverMs;
    uint32_t gapMs;
};

class UvMachine {
public:
    explicit UvMachine(const UvParams& p) : p_(p) {}

    // 측정 하나를 넣는다. 상태가 바뀌었으면 true.
    bool update(uint32_t now, uint16_t mv);
    Uv   state() const { return state_; }

    // 진행 중인 타이머(콘솔 표시용). 없으면 false.
    // 다음에 일어날 전이와 그때까지 남은 ms 를 돌려준다.
    bool pending(uint32_t now, Uv* next, uint32_t* remainMs) const;

    const UvParams& params() const { return p_; }

private:
    struct Timer {
        bool     run = false;
        uint32_t at  = 0;
        bool elapsed(uint32_t now, uint32_t ms) const { return run && (uint32_t)(now - at) >= ms; }
    };
    static void below(Timer& t, uint32_t now, uint16_t mv, uint16_t th, uint16_t band);

    UvParams p_;
    Uv       state_   = Uv::NORMAL;
    Timer    warn_, cut_, rec_;
    bool     hasLast_ = false;
    uint32_t lastAt_  = 0;
};

}  // namespace KpPower
