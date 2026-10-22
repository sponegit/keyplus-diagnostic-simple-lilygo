#pragma once
// ===========================================================================
// KpLink — 메인(T-A7670E) ↔ 서브(T7-S3) UART 링크 공용 정의
//
// 두 펌웨어가 이 파일 하나를 함께 include 한다. 프레임 형식·타입 ID·규약 버전을
// 바꿀 때는 반드시 여기서만 바꾸고, 메인·서브를 같은 커밋으로 올린다.
//
// 프레임 (작업서 §3.1):
//   [0xAA 0x55][type:1][seq:1][len:2 LE][payload:len][crc16:2 LE]
//   crc16 = CRC-16/CCITT-FALSE, 범위 = type ~ payload 끝 (싱크 2바이트 제외)
//
// Arduino 의존이 없다 — 호스트에서 단위 테스트한다(pio test -e native).
// ===========================================================================
#include <stddef.h>
#include <stdint.h>

// 링크 연결 시 HELLO 로 서로 대조한다. 다르면 링크를 올리지 않는다.
// 프레임 형식·타입 의미·payload 배치가 바뀌면 올린다.
#define KPLINK_PROTO_VERSION   2

#define KPLINK_SYNC0           0xAA
#define KPLINK_SYNC1           0x55
#define KPLINK_HEADER_LEN      6      // sync 2 + type 1 + seq 1 + len 2
#define KPLINK_CRC_LEN         2
// OTA 청크(데이터 1024 + 오프셋 등 머리) 가 들어가는 크기. 이보다 긴 len 은 잡음으로 본다.
#define KPLINK_MAX_PAYLOAD     1056
#define KPLINK_MAX_FRAME       (KPLINK_HEADER_LEN + KPLINK_MAX_PAYLOAD + KPLINK_CRC_LEN)

namespace KpLink {

// 타입 ID (작업서 §3.2). 상위 니블이 범주다.
enum Type : uint8_t {
    // 0x0_ 링크 — 양방향
    HELLO       = 0x01,   // 규약 버전·fw·hw
    HEARTBEAT   = 0x02,
    ACK         = 0x03,   // 대상 seq + 결과
    RESET       = 0x04,   // 메인→서브 소프트 재부팅 요청
    // 0x1_ 콘솔
    LOG         = 0x10,   // 메인→서브 로그 텍스트 — 버려도 되는 데이터, 큐가 차면 먼저 드롭
    CONSOLE_IN  = 0x11,   // 서브→메인 콘솔 입력 줄
    // 0x2_ 차키
    KEY_CMD     = 0x20,   // 메인→서브 시퀀스 단계 목록
    KEY_RESULT  = 0x21,   // 서브→메인 accepted·busy·done·failed
    // 0x3_ 서브 OTA
    OTA_BEGIN   = 0x30,
    OTA_CHUNK   = 0x31,
    OTA_END     = 0x32,
    OTA_ACK     = 0x33,
    // 0x4_ 상태
    POWER       = 0x40,   // 서브→메인 전압·전류 (INA228)
    AIR         = 0x41,   // 서브→메인 공기질
    GNSS        = 0x42,   // 서브→메인 위치
    GUARD       = 0x43,   // 서브→메인 온도 가드·저전압 상태
    VEHICLE     = 0x48,   // 메인→서브 시동·주행 상태
};

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF). crc 에 이전 결과를 넘기면 이어서 계산한다.
uint16_t crc16(const uint8_t* data, size_t len, uint16_t crc = 0xFFFF);

// 프레임 하나를 out 에 쓴다. 반환 = 쓴 바이트 수, 실패(len 초과·버퍼 부족) 시 0.
size_t encode(uint8_t type, uint8_t seq, const uint8_t* payload, uint16_t len,
              uint8_t* out, size_t outCap);

// 디코더가 꺼내 준 프레임. payload 는 디코더 내부 버퍼를 가리키며
// **다음 feed()/next() 호출 전까지만** 유효하다.
struct Frame {
    uint8_t        type;
    uint8_t        seq;
    uint16_t       len;
    const uint8_t* payload;
};

// 스트리밍 디코더 — 바이트를 넣고(feed) 프레임을 꺼낸다(next).
//
// 잡음 내성: 서브 부팅 시 ROM 로그, 선 뽑힘, 풀업 없는 RX 의 잡음이 섞여 들어온다.
// 싱크·길이·CRC 중 하나라도 어긋나면 **선두 1바이트만 버리고** 다시 싱크를 찾는다.
// 그래야 가짜 싱크 안에 숨은 진짜 프레임을 놓치지 않는다.
//
// 가짜 헤더가 큰 len 을 주장하면 그 길이만큼 바이트가 올 때까지 기다리게 된다.
// 링크가 한가하면 뒤따르는 진짜 프레임이 그만큼 묶이므로, 전송 계층은
// "미완성 프레임이 있는데 일정 시간 새 바이트가 없다"면 expire() 를 불러 풀어준다.
class Decoder {
public:
    // 넣을 수 있는 만큼 받아들이고 받아들인 바이트 수를 반환한다.
    // n 보다 작으면 버퍼가 찬 것 — next() 로 비운 뒤 나머지를 다시 넣는다.
    size_t feed(const uint8_t* data, size_t n);

    // 완성된 프레임이 있으면 out 에 채우고 true.
    bool next(Frame& out);

    // 미완성 프레임을 포기한다(선두 1바이트를 버리고 다시 싱크를 찾게 함).
    void expire();

    // 버퍼에 남은(아직 프레임이 되지 못한) 바이트 수.
    size_t pending() const { return len_ - consume_; }

    void reset();

    // 진단 카운터 — 콘솔 `status link` 로 노출한다.
    struct Stats {
        uint32_t frames;    // 정상 프레임
        uint32_t crcErr;    // CRC 불일치
        uint32_t lenErr;    // len 이 상한 초과
        uint32_t dropped;   // 싱크를 찾느라 버린 바이트
        uint32_t expired;   // expire() 로 포기한 미완성 프레임
    };
    const Stats& stats() const { return stats_; }

private:
    void compact();
    void dropFront(size_t n);

    uint8_t buf_[KPLINK_MAX_FRAME * 2];
    size_t  len_     = 0;   // 버퍼에 든 바이트
    size_t  consume_ = 0;   // 직전 next() 가 내준 프레임 길이 — 다음 호출 때 지운다
    Stats   stats_   = {};
};

}  // namespace KpLink
