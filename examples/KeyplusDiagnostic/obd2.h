/**
 * @file      obd2.h
 * @brief     6단계 — OBD2/CAN 실시간 PID 수집 (ESP32 내장 TWAI + 외부 트랜시버)
 *
 * ISO 15765-4(CAN) 기반 SAE J1979 Mode 01 실시간 PID를 폴링한다.
 *   요청: CAN ID 0x7DF(기능 브로드캐스트), data = [02, 01, PID, 00..]
 *   응답: CAN ID 0x7E8~0x7EF, data = [len, 0x41, PID, A, B, ...]
 * Mode 01 실시간 PID(단일 프레임) + VIN(Mode 09 PID 02, ISO-TP 다중 프레임 재조립)을 지원.
 * DTC(Mode 03) 등 그 외 다중 프레임은 후속 증분.
 *
 * 링크: 500kbps/11-bit(최신 차량 표준) 우선, 무응답 시 250kbps 폴백(begin에서 자동).
 * 배선: 차량 OBD2 CAN-H/L → 트랜시버(SN65HVD230) → GPIO21(RX)/22(TX), 공통 GND.
 * 설계: phase 진행문서 §Phase6 / hardware-baseline.md(CAN 핀)
 */
#pragma once

#include <Arduino.h>

namespace Obd2 {

// 한 폴 주기의 수집 결과. has_* = 이번 폴에서 해당 PID 응답을 받았는가(부분 지원 대비).
struct Data {
    bool     valid = false;      // 하나라도 응답 받음(= CAN 링크 살아있음)
    uint32_t supportedPid = 0;   // PID 0x00 응답: 0x01~0x20 지원 비트마스크

    bool  has_rpm = false;      float    rpm = 0;        // 0x0C  RPM
    bool  has_speed = false;    int      speed = 0;      // 0x0D  km/h
    bool  has_coolant = false;  int      coolant = 0;    // 0x05  °C
    bool  has_load = false;     float    load = 0;       // 0x04  %
    bool  has_throttle = false; float    throttle = 0;   // 0x11  %
    bool  has_intake = false;   int      intake = 0;     // 0x0F  °C
    bool  has_maf = false;      float    maf = 0;        // 0x10  g/s
    bool  has_fuel = false;     float    fuel = 0;       // 0x2F  %
    bool  has_ctrlv = false;    float    ctrl_v = 0;     // 0x42  V
    bool  has_runtime = false;  uint16_t runtime = 0;    // 0x1F  s

    bool  has_vin = false;      char     vin[18] = {0};  // Mode 09 PID 02 (17자, ISO-TP 다중프레임)
};

// TWAI 드라이버 설치·시작(500k→250k 폴백) + PID 0x00 지원 조회. 성공(링크+응답) 시 true.
// 트랜시버 미배선/차량 미연결이면 응답이 없어 false → loop가 주기 재시도.
bool begin(Stream &log);

// 드라이버 설치 여부(설치는 됐으나 링크 미확립일 수 있음 — 링크는 read 성공으로 판단).
bool isInstalled();

// 핵심 PID 폴링 → out 채움. 하나라도 응답 받으면 true(out.valid=true).
// 폴 전체가 무응답이면 false(링크 끊김 → 호출측이 재초기화 고려).
bool read(Data &out, Stream &log);

// 드라이버 정지·제거(재초기화/비트레이트 변경 시).
void end();

} // namespace Obd2
