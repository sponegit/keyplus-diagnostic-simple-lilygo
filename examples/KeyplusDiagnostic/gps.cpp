/**
 * @file      gps.cpp
 * @brief     2단계 GPS 모듈 구현 — GPS_BuiltIn 예제의 내장 GNSS 흐름을 모듈화.
 */
#include "gps.h"
#include "utilities.h"
#include "config.h"

namespace Gps {

bool begin(TinyGsm &modem)
{
    // T-A7670E는 GNSS 전원 제어 GPIO가 없어(MODEM_GPS_ENABLE_GPIO == -1)
    // AT 명령(+CGNSSPWR)만으로 엔진을 켠다. enableGPS()가 내부 처리.
    uint32_t start = millis();
    while (!modem.enableGPS(MODEM_GPS_ENABLE_GPIO, MODEM_GPS_ENABLE_LEVEL)) {
        if (millis() - start > 15000UL) {
            return false;   // 15초 내 활성화 실패 → 호출측에서 재시도/리셋 판단
        }
        delay(500);
    }
    // 모뎀 ↔ ESP32 GNSS UART 속도 확정
    modem.setGPSBaud(GPS_BAUDRATE);
    return true;
}

bool read(TinyGsm &modem, GpsFix &fix)
{
    fix = GpsFix{};   // 전체 0/false 초기화
    bool got = modem.getGPS(&fix.fixMode, &fix.lat, &fix.lon, &fix.speed, &fix.alt,
                            &fix.vsat, &fix.usat, &fix.accuracy,
                            &fix.year, &fix.month, &fix.day,
                            &fix.hour, &fix.minute, &fix.second);
    // fixMode 0 = 아직 측위 못함. 좌표가 있어도 fixMode 0이면 무효 처리.
    fix.valid = got && fix.fixMode != 0;
    return fix.valid;
}

void end(TinyGsm &modem)
{
    modem.disableGPS(MODEM_GPS_ENABLE_GPIO, !MODEM_GPS_ENABLE_LEVEL);
}

void print(const GpsFix &fix, Stream &out)
{
    if (!fix.valid) {
        out.println("[GPS] no fix yet");
        return;
    }
    out.printf("[GPS] fix=%d  lat=%.6f lon=%.6f  spd=%.1fkm/h alt=%.1fm  acc=%.1f  vsat=%d\n",
               fix.fixMode, fix.lat, fix.lon, fix.speed, fix.alt, fix.accuracy, fix.vsat);
    out.printf("[GPS] UTC %04d-%02d-%02d %02d:%02d:%02d\n",
               fix.year, fix.month, fix.day, fix.hour, fix.minute, fix.second);
}

String raw(TinyGsm &modem)
{
    String s = modem.getGPSraw();
    s.trim();
    return s;
}

} // namespace Gps
