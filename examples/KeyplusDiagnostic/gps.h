/**
 * @file      gps.h
 * @brief     2단계 — A7670E 내장 GNSS 위치 조회 모듈
 *
 * TinyGSM의 modem.enableGPS()/getGPS()/disableGPS()를 감싸 진단기 펌웨어에서
 * 쓰기 좋은 형태(구조체 + 상태 폴링)로 노출한다. 유심 불필요.
 */
#pragma once

// utilities.h가 TINY_GSM_MODEM_A7670를 정의하므로 TinyGsmClient.h보다 먼저 포함해야
// 모든 translation unit에서 올바른 모뎀 드라이버가 컴파일된다.
#include "utilities.h"
#include <TinyGsmClient.h>

// 한 번의 위치 측정 결과. valid=false면 나머지 필드는 무의미.
struct GpsFix {
    bool    valid;
    uint8_t fixMode;    // 0=미측위, 2=2D, 3=3D
    float   lat;
    float   lon;
    float   speed;      // km/h
    float   alt;        // m
    float   accuracy;   // HDOP 기반 추정
    int     vsat;       // 가시 위성 수
    int     usat;       // 사용 위성 수 (내장 GPS는 항상 0 반환)
    // UTC 시각
    int     year, month, day, hour, minute, second;
};

namespace Gps {

// GNSS 전원/엔진 활성화. 성공까지 재시도한다. 성공 시 true.
bool begin(TinyGsm &modem);

// 위치 1회 폴링. fix 획득 시 true, 아직 측위 전이면 false.
bool read(TinyGsm &modem, GpsFix &fix);

// GNSS 비활성화 (저전력 진입 시).
void end(TinyGsm &modem);

// GpsFix를 사람이 읽는 형태로 SerialMon에 출력 (디버그용).
void print(const GpsFix &fix, Stream &out);

// 모뎀의 원시 GNSS 정보(+CGNSSINFO 형식) 문자열. 측위 전에도 위성 포착 진행상황
// 확인용으로 유용. 데이터 없으면 빈 문자열.
String raw(TinyGsm &modem);

} // namespace Gps
