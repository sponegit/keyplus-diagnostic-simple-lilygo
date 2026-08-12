/**
 * @file      gps.cpp
 * @brief     2단계 GPS 모듈 구현 — GPS_BuiltIn 예제의 내장 GNSS 흐름을 모듈화.
 */
#include "gps.h"
#include "utilities.h"
#include "config.h"
#include "log.h"

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

    // 위성군 지정(AT+CGNSSMODE). 이걸 안 보내면 모뎀 기본값에 맡기게 되는데, 기본이
    // 단일 위성군이면 포착이 극단적으로 느려진다(9시간 미측위 사례). LilyGO 의 A7670
    // GNSS 예제들이 쓰는 값과 동일하게 GPS+BDS+GALILEO+SBAS+QZSS 를 켠다.
    // 실패해도 측위 자체는 진행되므로 반환값을 막지는 않는다.
    modem.setGPSMode(GNSS_MODE_GPS_BDS_GALILEO_SBAS_QZSS);
    return true;
}

uint8_t mode(TinyGsm &modem)
{
    // AT+CGNSSMODE? — 실제로 어떤 위성군이 켜져 있는지. 진단(status gps)용.
    // 응답 전체를 문자열로 받아서 파싱한다 — TinyGSM 의 stream 파서(streamGetIntBefore)는
    // protected 라 라이브러리 밖에서 못 쓴다.
    String res;
    modem.sendAT("+CGNSSMODE?");
    if (modem.waitResponse(1000L, res) != 1) return 0;
    int i = res.indexOf("+CGNSSMODE:");
    if (i < 0) return 0;
    return (uint8_t)res.substring(i + 11).toInt();
}

// +CGNSSINFO 의 좌표는 SIMCom 규격상 NMEA 형식 ddmm.mmmmmm(경도는 dddmm.mmmmmm)이고,
// TinyGSM 은 이를 변환하지 않고 그대로 넘긴다(getGPSImpl 주석 "Latitude in ddmm.mmmmmm").
// 그대로 telemetry 에 실으면 서울(37.57°)이 3734.2° 로 나가 지도상 좌표가 무의미해진다.
//
// 10진수 도(degree)로 들어오는 펌웨어도 있어(모델/펌웨어별 차이) 값을 보고 판단한다:
// 위도는 10진수면 절대 90을 넘지 못하고, ddmm 형식이면 1° 이상에서 항상 100 이상이다.
// 경도도 같은 논리로 180 기준. 국내(37N/127E)에서는 어느 쪽인지 항상 명확히 갈린다.
static float nmeaToDegrees(float v, float maxDeg)
{
    float a = fabsf(v);
    if (a <= maxDeg) return v;          // 이미 10진수 도 — 그대로
    float deg = floorf(a / 100.0f);     // ddmm.mmmmmm → dd + mm.mmmmmm/60
    float min = a - deg * 100.0f;
    float out = deg + min / 60.0f;
    return v < 0 ? -out : out;
}

bool read(TinyGsm &modem, GpsFix &fix)
{
    fix = GpsFix{};   // 전체 0/false 초기화
    bool got = modem.getGPS(&fix.fixMode, &fix.lat, &fix.lon, &fix.speed, &fix.alt,
                            &fix.vsat, &fix.usat, &fix.accuracy,
                            &fix.year, &fix.month, &fix.day,
                            &fix.hour, &fix.minute, &fix.second);
    fix.lat = nmeaToDegrees(fix.lat, 90.0f);
    fix.lon = nmeaToDegrees(fix.lon, 180.0f);
    // 모뎀이 속도/필드 미제공 시 -9999 센티넬을 준다(fix는 있어도 speed 필드 공란).
    // 음수는 무효 → 0으로 정리(telemetry·표시에 -9999.0km/h 노출 방지). 실속도는 OBD 0x0D가 권위.
    if (fix.speed < 0)    fix.speed = 0;
    if (fix.alt < -1000)  fix.alt = 0;
    if (fix.accuracy < 0) fix.accuracy = 0;
    // fixMode 0 = 아직 측위 못함. 좌표가 있어도 fixMode 0이면 무효 처리.
    fix.valid = got && fix.fixMode != 0;
    return fix.valid;
}

bool isEnabled(TinyGsm &modem)
{
    // AT+CGNSSPWR? — 모뎀이 리셋되면(소프트/하드) GNSS 전원이 0으로 돌아간다.
    // 그 사실을 알 방법이 이 조회뿐이라, 폴 실패가 이어질 때 호출측이 확인한다.
    return modem.isEnableGPS();
}

bool enableAgps(TinyGsm &modem)
{
    // AT+CAGPS — 망에서 보조 데이터(에페메리스 등)를 받아 콜드스타트를 단축한다.
    // PDP 가 올라와 있어야 하고, 실패해도 측위 자체는 진행되므로 best-effort 다.
    return modem.enableAGPS();
}

bool powerOn(TinyGsm &modem)
{
    // begin() 의 비블로킹 판. duty cycle 은 5분마다 도는데 begin() 은 실패 시 최대 15초를
    // 잡아먹어 그동안 차키 명령이 밀린다 — **즉시 응답이 이 제품의 제1 요구다.**
    // 그래서 한 번만 시도하고 실패하면 그대로 돌아간다(호출측이 다음 틱에 다시 부른다).
    if (!modem.enableGPS(MODEM_GPS_ENABLE_GPIO, MODEM_GPS_ENABLE_LEVEL)) return false;
    // ⚠️ 위성군 지정은 전원을 껐다 켜면 기본값으로 돌아갈 수 있다. 여기서 매번 다시
    //    건다 — 이걸 빠뜨리면 duty cycle 을 돌 때마다 단일 위성군으로 떨어져 측위가
    //    극단적으로 느려진다(begin() 의 CGNSSMODE 주석 참고).
    //    setGPSBaud 는 UART 속도라 전원과 무관하므로 다시 걸지 않는다.
    modem.setGPSMode(GNSS_MODE_GPS_BDS_GALILEO_SBAS_QZSS);
    return true;
}

bool end(TinyGsm &modem)
{
    // ⚠️ 반환값을 버리면 안 된다. 끄기가 실패했는데 호출측이 "껐다"고 믿으면
    //    duty cycle 은 그 뒤로 조회를 건너뛰면서(꺼진 줄 알고) 실제로는 GNSS 가 계속
    //    켜진 채 30mA 를 태운다 — 로그만 OFF 라 눈으로는 절대 못 찾는 종류의 손실이다.
    return modem.disableGPS(MODEM_GPS_ENABLE_GPIO, !MODEM_GPS_ENABLE_LEVEL);
}

// 폴 1회분 좌표 덤프 — DEBUG 전용. 평상시 위치는 loop의 [STAT] 한 줄로 충분하고,
// 측위 획득/상실 같은 상태 전이는 호출측이 INFO로 따로 남긴다.
void print(const GpsFix &fix, Stream &out)
{
    if (!fix.valid) {
        LOGD(out, "[GPS] no fix yet\n");
        return;
    }
    LOGD(out, "[GPS] fix=%d  lat=%.6f lon=%.6f  spd=%.1fkm/h alt=%.1fm  acc=%.1f  vsat=%d\n",
         fix.fixMode, fix.lat, fix.lon, fix.speed, fix.alt, fix.accuracy, fix.vsat);
    LOGD(out, "[GPS] UTC %04d-%02d-%02d %02d:%02d:%02d\n",
         fix.year, fix.month, fix.day, fix.hour, fix.minute, fix.second);
}

String raw(TinyGsm &modem)
{
    String s = modem.getGPSraw();
    s.trim();
    return s;
}

} // namespace Gps
