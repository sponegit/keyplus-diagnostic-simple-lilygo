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
    bool  has_odometer = false; float    odometer = 0;   // 0xA6  총 주행거리 km (J1979-2, 지원 드묾)

    bool  has_vin = false;      char     vin[18] = {0};  // Mode 09 PID 02 (17자, ISO-TP 다중프레임)
};

// TWAI 드라이버 설치·시작(500k→250k 폴백) + PID 0x00 지원 조회. 성공(링크+응답) 시 true.
// 트랜시버 미배선/차량 미연결이면 응답이 없어 false → loop가 주기 재시도.
bool begin(Stream &log);

// 드라이버 설치 여부(설치는 됐으나 링크 미확립일 수 있음 — 링크는 read 성공으로 판단).
bool isInstalled();

// 진단 콘솔용 링크 요약 — 폴 값(Data)과 달리 "링크 자체가 어떤 상태인가"를 본다.
// 무응답 원인 추적용: 비트레이트가 잡혔는지, 지원 마스크가 뭔지, 확장 PID가 몇 개
// 미지원 확정(래치)됐는지, VIN을 못 읽고 시도 상한에 걸렸는지.
struct LinkInfo {
    bool        installed;      // TWAI 드라이버 설치됨
    int         bitrate;        // 확립된 kbps (0 = 미확립)
    int         lastGoodRate;   // 마지막으로 링크가 잡혔던 kbps (재시도 우선순위)
    uint32_t    supportedPid;   // PID 0x00 응답: 0x01~0x20 지원 비트마스크
    uint32_t    ecuId;          // 고정된 응답 ECU CAN ID (0 = 미확정, 0x7E8 = 엔진)
    bool        hasVin;
    const char *vin;            // hasVin이 false면 ""
    int         vinTries;       // VIN 재시도 횟수(상한 OBD2_VIN_TRY_LIMIT)
    int         extLatched;     // 미지원 확정된 확장 PID 개수
    int         extTotal;       // 확장 PID 총 개수
};
void linkInfo(LinkInfo &out);

// 핵심 PID 폴링 → out 채움.
// 반환값 = "이번 폴에서 ECU 응답이 있었는가"(= CAN 링크 살아있음). false면 호출측이 재초기화.
// ⚠️ out.valid 와 다르다: VIN 캐시가 있으면 out.valid는 링크가 죽어도 true로 남는다.
//    반환값을 out.valid로 두면 링크 끊김을 영영 감지 못해 매 폴 타임아웃 스톨만 반복한다.
bool read(Data &out, Stream &log);

// 고빈도(1Hz) 3 PID 서브셋. 차속(0x0D)·회전수(0x0C)·스로틀(0x11)만 요청한다.
// 전체 폴(read)과 같은 requestPid 를 재사용하며, 같은 loop 에서 순차 실행되므로 재진입 없음.
// 비용: ECU 응답 5~20ms × 3 ≈ 60ms/초 = 듀티 6%.
struct FastSample {
    bool     has_speed = false;  uint8_t  speed = 0;   // 0x0D km/h
    bool     has_rpm = false;    uint16_t rpm = 0;     // 0x0C
    bool     has_throttle = false; uint8_t throttle = 0; // 0x11 %
};

// 반환값 = "이번 요청에서 ECU 응답이 있었는가". read() 와 같은 규약이다.
// false 가 연속되면 호출측(Fast::tick)이 폴을 중단하고 전체 폴의 재확립 경로에 맡긴다 —
// 링크가 죽은 채로 매초 3×타임아웃을 태우면 loop 가 초당 300ms 씩 멈춘다.
bool readFast(FastSample &out, Stream &log);

// 드라이버 정지·제거(재초기화/비트레이트 변경 시).
void end();

// CAN 핀을 열성(recessive)으로 주차시킨다.
//
// TWAI 드라이버를 uninstall 하면 PIN_CAN_TX 가 GPIO 로 돌아가 **뜬 입력**이 된다.
// 트랜시버(SN65HVD230)의 D 입력이 뜬 채 LOW 로 기울면 버스를 계속 우성(dominant)으로
// 물게 되므로, 출력 HIGH 로 고정해 그 가능성을 없앤다.
//
// ⚠️ **이건 절전 대책이 아니다.** 2026-08-11 실측에서 mqtt+obd(5) 115mA 의 원인이
//    이것이라 보고 넣었으나, 주차시킨 뒤에도 **전류가 전혀 변하지 않았다 — 가설은 틀렸다.**
//    그래도 되돌리지 않은 이유는, 출력 핀을 뜬 채로 트랜시버 입력에 물려두는 것이
//    전류와 무관하게 나쁜 상태이기 때문이다(잡음에 따라 버스를 물 수 있다).
//    OBD2 몫의 진짜 원인은 아직 미규명이다 — 트랜시버 실물 연결 여부부터 확인해야 한다.
// ⚠️ FEATURE_OBD2 가 0 인 빌드에서도 부른다 — 트랜시버는 펌웨어 토글과 무관하게
//    배선돼 있으므로 계측 빌드의 기준선 조건을 같게 맞추려면 항상 같은 상태여야 한다.
void parkPins();

// 계측용 — 주차를 풀어 **예전 상태(뜬 입력)로 되돌린다.** 같은 실행 안에서 parkPins 의
// 효과를 A/B 로 재기 위한 것이다(실행 간 비교는 LTE 전파 상태 때문에 ±20mA 흔들려 못 쓴다).
// ⚠️ TWAI 가 설치돼 있으면 아무것도 하지 않는다 — 드라이버가 핀 소유자다.
void unparkPins();

// TWAI 설치 여부(계측 콘솔에서 위 경고를 표시하는 데 쓴다).
bool isInstalledNow();

// 수집 결과를 시리얼에 한 줄로 출력(응답받은 필드만 + VIN). 필드 진단용.
void print(const Data &d, Stream &log);

} // namespace Obd2
