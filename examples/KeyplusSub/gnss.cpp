/**
 * @file      gnss.cpp
 * @brief     MAX-M10S 구현. 설계는 gnss.h.
 */
#include "gnss.h"
#include "config.h"
#include "link.h"

#include <KpGnss.h>
#include <KpLink.h>
#include <KpVehicle.h>
#include <TinyGPS++.h>

namespace Gnss {

static HardwareSerial s_uart(1);
static TinyGPSPlus    s_gps;

// 전원 듀티(위치 주기 3단계) — 메인 VEHICLE 프레임의 GNSS_ON 을 따른다.
static bool              s_powerOn = true;   // 레일 상태(부팅 = 켬)
static KpVehicle::Values s_veh     = {};
static uint32_t          s_vehAt   = 0;      // 마지막 VEHICLE 수신 시각. 0 = 받은 적 없음
static uint32_t          s_powerCycles = 0;  // 끈 → 켠 횟수(주차 중 깨운 횟수)

static uint32_t s_pubAt     = 0;      // 마지막 GNSS 프레임 시도 시각
static uint32_t s_pubSent   = 0, s_pubSkip = 0;

static uint32_t s_powerAt   = 0;      // 레일을 켠 시각 — 첫 fix 소요 시간 기준
static uint32_t s_firstFix  = 0;      // 레일 켠 뒤 첫 fix 까지(ms). 0 = 아직
static uint32_t s_lastByte  = 0;
static bool     s_hadFix    = false;
static bool     s_raw       = false;
static char     s_line[100];          // NMEA 문장은 최대 82자
static uint8_t  s_lineLen   = 0;
static uint32_t s_baud      = GNSS_BAUD;

void begin()
{
    // 부팅은 켠다 — 메인 VEHICLE 을 받기 전에는 시동 상태를 모른다(안전 쪽).
    digitalWrite(PIN_RAIL_GNSS, HIGH);
    s_uart.begin(s_baud, SERIAL_8N1, PIN_GNSS_RX, PIN_GNSS_TX);
    s_powerAt = millis();
}

void onVehicle(const uint8_t *payload, size_t len)
{
    KpVehicle::Values v;
    if (!KpVehicle::decode(payload, len, &v)) return;
    s_veh   = v;
    s_vehAt = millis();
    if (!s_vehAt) s_vehAt = 1;   // 0 은 "받은 적 없음"
}

// 레일 켜기/끄기. 켤 때 첫 fix 측정을 다시 시작한다(깨울 때마다 TTFF 를 본다).
// ⚠️ 게이트 회로가 없는 벤치에서는 GPIO 만 바뀌고 모듈은 계속 돈다 — 그래도 동작은 맞다(fix 가 계속 있을 뿐).
static void setPower(bool on, const char *why)
{
    if (on == s_powerOn) return;
    s_powerOn = on;
    digitalWrite(PIN_RAIL_GNSS, on ? HIGH : LOW);
    if (on) {
        s_powerAt  = millis();
        s_firstFix = 0;
        s_hadFix   = false;
        s_powerCycles++;
    }
    Serial.printf("[GNSS] 전원 %s — %s\n", on ? "켬" : "끔", why);
}

// 메인이 원하는 전원 상태. VEHICLE 이 없거나 오래됐으면 켠다(옛 메인·링크 끊김 — 위치를 잃지 않게).
static void applyDuty(uint32_t now)
{
    if (!s_vehAt || (uint32_t)(now - s_vehAt) > VEHICLE_STALE_MS) {
        setPower(true, s_vehAt ? "메인 VEHICLE 끊김(안전 쪽)" : "메인 VEHICLE 받기 전");
        return;
    }
    if (s_veh.gnssOn()) setPower(true, s_veh.driving() ? "주행" : "주차 — 위치 요청(메인)");
    else                setPower(false, "주차 — 메인이 위치를 받음");
}

bool fix()
{
    return s_gps.location.isValid() && s_gps.location.age() < GNSS_FIX_STALE_MS;
}

static void rawByte(char c)
{
    if (c == '\n') {
        s_line[s_lineLen] = '\0';
        Serial.printf("[GNSS] %s\n", s_line);
        s_lineLen = 0;
    } else if (c != '\r') {
        // 속도가 틀리면 줄바꿈이 안 온다 — 쓰레기라도 보이게 넘칠 때 그대로 내보낸다.
        s_line[s_lineLen++] = (c >= 0x20 && c < 0x7F) ? c : '.';
        if (s_lineLen + 1 >= sizeof(s_line)) {
            s_line[s_lineLen] = '\0';
            Serial.printf("[GNSS] %s (줄바꿈 없음 — 속도 확인: gnss scan)\n", s_line);
            s_lineLen = 0;
        }
    }
}

// UTC 가 있고 신선한가. date·time 도 isValid() 가 한 번 서면 계속 true 라 나이를 함께 본다.
// u-blox 는 위성 시각을 받기 전에도 RTC 값을 내보낼 수 있다 — 연도로 한 번 더 거른다.
static bool timeFresh()
{
    return s_gps.date.isValid() && s_gps.time.isValid() && s_gps.date.year() >= 2024 &&
           s_gps.time.age() < GNSS_FIX_STALE_MS;
}

static void publish(uint32_t now, bool force)
{
    if (!force && s_pubAt && (uint32_t)(now - s_pubAt) < GNSS_PUB_MS) return;
    s_pubAt = now;
    const bool tOk = timeFresh();
    const KpGnss::Values v = KpGnss::make(
        fix(), s_gps.location.lat(), s_gps.location.lng(), (float)s_gps.altitude.meters(),
        (float)s_gps.speed.kmph(), (float)s_gps.hdop.hdop(), s_gps.satellites.value(),
        tOk ? s_gps.date.year() : 0, s_gps.date.month(), s_gps.date.day(), s_gps.time.hour(),
        s_gps.time.minute(), s_gps.time.second());
    uint8_t buf[KPGNSS_PAYLOAD_LEN];
    const size_t n = KpGnss::encode(v, buf, sizeof(buf));
    // 링크가 내려가 있으면 버린다 — 위치는 최신값만 의미가 있다(작업서 §3.3).
    if (Link::sendFrame(KpLink::GNSS, buf, (uint16_t)n)) s_pubSent++;
    else                                                s_pubSkip++;
}

void update()
{
    applyDuty(millis());
    while (s_uart.available() > 0) {
        const char c = (char)s_uart.read();
        s_lastByte = millis();
        s_gps.encode(c);
        if (s_raw) rawByte(c);
    }
    const bool now = fix();
    // fix 전이는 주기를 기다리지 않고 바로 알린다(메인 ts·좌표 선택이 이 비트를 따른다).
    publish(millis(), now != s_hadFix);
    if (now != s_hadFix) {
        s_hadFix = now;
        if (now) {
            if (!s_firstFix) s_firstFix = millis() - s_powerAt;
            Serial.printf("[GNSS] fix 잡음 — 위성 %lu, HDOP %.1f (전원 후 %lus)\n",
                          (unsigned long)s_gps.satellites.value(), s_gps.hdop.hdop(),
                          (unsigned long)(s_firstFix / 1000));
        } else {
            Serial.println("[GNSS] fix 잃음");
        }
    }
}

// 속도를 바꿔 가며 체크섬이 맞는 NMEA 문장이 오는 속도를 찾는다. 콘솔 진단용 — 약 1.5초 × 후보 수 동안 막힌다.
static void scan()
{
    static const uint32_t cands[] = {9600, 38400, 115200, 57600, 19200, 4800, 230400};
    uint32_t found = 0;
    for (uint32_t b : cands) {
        s_uart.updateBaudRate(b);
        while (s_uart.available()) s_uart.read();
        TinyGPSPlus probe;
        const uint32_t t0 = millis();
        while ((uint32_t)(millis() - t0) < 1500) {
            while (s_uart.available()) probe.encode((char)s_uart.read());
            feedLoopWDT();
            delay(5);
        }
        Serial.printf("[GNSS] %6lu bps : bytes %lu, 문장 %lu, 체크섬 오류 %lu\n", (unsigned long)b,
                      (unsigned long)probe.charsProcessed(), (unsigned long)probe.passedChecksum(),
                      (unsigned long)probe.failedChecksum());
        if (!found && probe.passedChecksum() >= 2) found = b;
    }
    s_baud = found ? found : s_baud;
    s_uart.updateBaudRate(s_baud);
    if (found)
        Serial.printf("[GNSS] %lu bps 로 맞춤 — 재부팅하면 %d 로 돌아간다(맞으면 GNSS_BAUD 를 바꾼다)\n",
                      (unsigned long)found, GNSS_BAUD);
    else
        Serial.printf("[GNSS] 맞는 속도 없음 — %lu bps 유지. 배선(TX→15)·GND·잡음 확인\n", (unsigned long)s_baud);
}

bool tryConsole(const char *line)
{
    if (!strcmp(line, "gnss scan")) {
        scan();
        return true;
    }
    if (!strcmp(line, "gnss")) {
        printStatus(Serial);
        return true;
    }
    if (!strcmp(line, "gnss raw")) {
        s_raw     = !s_raw;
        s_lineLen = 0;
        Serial.printf("[GNSS] NMEA 원문 출력 %s\n", s_raw ? "켬 (다시 gnss raw 로 끔)" : "끔");
        return true;
    }
    return false;
}

void printStatus(Stream &io)
{
    const uint32_t now = millis();
    io.printf("[GNSS] MAX-M10S (UART1 RX%d·TX%d @%lu)\n", PIN_GNSS_RX, PIN_GNSS_TX, (unsigned long)s_baud);
    if (!s_gps.charsProcessed()) {
        io.println("  수신         : 없음 — 모듈 TX→GPIO15 배선·3.3V 전원 확인");
    } else if ((uint32_t)(now - s_lastByte) > GNSS_RX_STALE_MS) {
        io.printf("  수신         : %lus 동안 없음 (끊김)\n", (unsigned long)((now - s_lastByte) / 1000));
    } else if (!s_gps.passedChecksum()) {
        io.println("  수신         : 바이트는 오는데 문장이 없음 — 속도 불일치 의심, gnss scan 으로 찾기");
    }
    if (fix()) {
        io.printf("  fix          : 있음  %.6f, %.6f  고도 %.1fm\n", s_gps.location.lat(), s_gps.location.lng(),
                  s_gps.altitude.meters());
        io.printf("  품질         : 위성 %lu, HDOP %.1f, 속도 %.1f km/h\n", (unsigned long)s_gps.satellites.value(),
                  s_gps.hdop.hdop(), s_gps.speed.kmph());
    } else {
        io.printf("  fix          : 없음  (위성 %lu%s)\n", (unsigned long)s_gps.satellites.value(),
                  s_gps.location.isValid() ? ", 마지막 위치는 오래됨" : "");
    }
    if (s_gps.date.isValid() && s_gps.time.isValid() && s_gps.date.year() > 2000)
        io.printf("  UTC          : %04u-%02u-%02u %02u:%02u:%02u\n", s_gps.date.year(), s_gps.date.month(),
                  s_gps.date.day(), s_gps.time.hour(), s_gps.time.minute(), s_gps.time.second());
    else
        io.println("  UTC          : 아직 없음");
    if (s_firstFix)
        io.printf("  첫 fix       : 전원 후 %lus\n", (unsigned long)(s_firstFix / 1000));
    else
        io.printf("  첫 fix       : 기다리는 중 (전원 후 %lus)\n", (unsigned long)((now - s_powerAt) / 1000));
    io.printf("  stats        : bytes %lu, 문장 %lu, fix 문장 %lu, 체크섬 오류 %lu\n",
              (unsigned long)s_gps.charsProcessed(), (unsigned long)s_gps.passedChecksum(),
              (unsigned long)s_gps.sentencesWithFix(), (unsigned long)s_gps.failedChecksum());
    io.printf("  GNSS 발행    : 보냄 %lu, 못 보냄(링크 다운) %lu, 주기 %lus\n", (unsigned long)s_pubSent,
              (unsigned long)s_pubSkip, (unsigned long)(GNSS_PUB_MS / 1000));
    if (!s_vehAt)
        io.printf("  전원         : %s (메인 VEHICLE 받기 전 — 켜 둠)\n", s_powerOn ? "켬" : "끔");
    else
        io.printf("  전원         : %s · 메인 요청 %s · 시동 %s · %s (VEHICLE %lus 전) · 주차 중 깨운 횟수 %lu\n",
                  s_powerOn ? "켬" : "끔", s_veh.gnssOn() ? "켬" : "끔", s_veh.ignOn() ? "ON" : "OFF",
                  s_veh.driving() ? "주행" : "주차", (unsigned long)((now - s_vehAt) / 1000),
                  (unsigned long)s_powerCycles);
}

} // namespace Gnss
