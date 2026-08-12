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
// 좌표는 NMEA ddmm.mmmmmm 로 오면 10진수 도로 변환해서 채운다(gps.cpp nmeaToDegrees).
bool read(TinyGsm &modem, GpsFix &fix);

// GNSS 엔진이 켜져 있는지(AT+CGNSSPWR?). 모뎀이 리셋되면 꺼진 상태로 돌아가므로,
// 측위 실패가 이어질 때 이걸로 확인하고 begin()을 다시 불러야 한다.
bool isEnabled(TinyGsm &modem);

// AGPS(AT+CAGPS) — 망 보조 데이터로 콜드스타트 단축. PDP up 상태에서 best-effort.
bool enableAgps(TinyGsm &modem);

// 현재 위성군 설정(AT+CGNSSMODE?). 0 = 조회 실패. 4 = GPS+BDS+GALILEO+SBAS+QZSS.
uint8_t mode(TinyGsm &modem);

// GNSS 비활성화 (저전력 진입 시).
// GNSS 전원만 1회 시도로 켠다(AT+CGNSSPWR=1 + 위성군 재설정). **블로킹하지 않는다.**
// duty cycle 처럼 주기적으로 켜야 하는 경로는 begin() 대신 이걸 쓴다 —
// begin() 은 실패 시 최대 15초를 잡아 그동안 차키 명령이 밀린다.
bool powerOn(TinyGsm &modem);

// GNSS 전원 OFF(AT+CGNSSPWR=0). **반환값을 반드시 확인할 것** — 실패했는데 껐다고
// 믿으면 조회만 건너뛴 채 GNSS 가 계속 돌아 45mA 를 태운다(로그는 OFF 라 눈에 안 띈다).
bool end(TinyGsm &modem);

// GpsFix를 사람이 읽는 형태로 SerialMon에 출력 (디버그용).
void print(const GpsFix &fix, Stream &out);

// 모뎀의 원시 GNSS 정보(+CGNSSINFO 형식) 문자열. 측위 전에도 위성 포착 진행상황
// 확인용으로 유용. 데이터 없으면 빈 문자열.
String raw(TinyGsm &modem);

} // namespace Gps
