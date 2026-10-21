#pragma once
// ===========================================================================
// KpLink — 메인(T-A7670E) ↔ 서브(T7-S3) UART 링크 공용 정의
//
// 두 펌웨어가 이 파일 하나를 함께 include 한다. 프레임 형식·타입 ID·규약 버전을
// 바꿀 때는 반드시 여기서만 바꾸고, 메인·서브를 같은 커밋으로 올린다.
//
// 프레임 (HARDWARE.md §1):
//   [0xAA 0x55][type:1][len:2 LE][payload:len][crc16:2 LE]
//   crc16 = CRC-16/CCITT-FALSE, 범위 = type ~ payload 끝 (싱크 2바이트 제외)
//
// ⚠️ 아래 타입 ID 는 사양서의 초안 값이다. 프로토콜 설계가 확정되면 갱신하고
//    KPLINK_PROTO_VERSION 을 올린다 — 링크 연결 시 서로 버전을 대조해 어긋나면 거부한다.
// ===========================================================================
#include <stddef.h>
#include <stdint.h>

#define KPLINK_PROTO_VERSION   1

#define KPLINK_SYNC0           0xAA
#define KPLINK_SYNC1           0x55
#define KPLINK_HEADER_LEN      5      // sync 2 + type 1 + len 2
#define KPLINK_CRC_LEN         2
#define KPLINK_MAX_PAYLOAD     1024   // OTA 청크 상한 기준 — 확정 시 조정

namespace KpLink {

enum Type : uint8_t {
    LOG         = 0x01,   // 메인 디버그 로그 텍스트 — 버려도 되는 데이터, 큐가 차면 먼저 드롭
    SENSOR_REQ  = 0x10,
    SENSOR_RESP = 0x11,
    CARKEY_CMD  = 0x20,
    OTA_CHUNK   = 0x30,
    POWER       = 0x40,   // 전압·전류 (INA228)
};

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF). crc 에 이전 결과를 넘기면 이어서 계산한다.
uint16_t crc16(const uint8_t* data, size_t len, uint16_t crc = 0xFFFF);

}  // namespace KpLink
