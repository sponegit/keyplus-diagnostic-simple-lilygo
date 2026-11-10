/**
 * @file      gnss.cpp
 * @brief     MAX-M10S 구현. 설계는 gnss.h.
 */
#include "gnss.h"
#include "config.h"

#include <TinyGPS++.h>

namespace Gnss {

static HardwareSerial s_uart(1);
static TinyGPSPlus    s_gps;

static uint32_t s_powerAt   = 0;      // 레일을 켠 시각 — 첫 fix 소요 시간 기준
static uint32_t s_firstFix  = 0;      // 레일 켠 뒤 첫 fix 까지(ms). 0 = 아직
static uint32_t s_lastByte  = 0;
static bool     s_hadFix    = false;
static bool     s_raw       = false;
static char     s_line[100];          // NMEA 문장은 최대 82자
static uint8_t  s_lineLen   = 0;

void begin()
{
    digitalWrite(PIN_RAIL_GNSS, HIGH);   // 게이트 회로가 있으면 전원 ON. 듀티는 2단계
    s_uart.begin(GNSS_BAUD, SERIAL_8N1, PIN_GNSS_RX, PIN_GNSS_TX);
    s_powerAt = millis();
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
    } else if (c != '\r' && s_lineLen + 1 < sizeof(s_line)) {
        s_line[s_lineLen++] = c;
    }
}

void update()
{
    while (s_uart.available() > 0) {
        const char c = (char)s_uart.read();
        s_lastByte = millis();
        s_gps.encode(c);
        if (s_raw) rawByte(c);
    }
    const bool now = fix();
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

bool tryConsole(const char *line)
{
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
    io.printf("[GNSS] MAX-M10S (UART1 RX%d·TX%d @%d)\n", PIN_GNSS_RX, PIN_GNSS_TX, GNSS_BAUD);
    if (!s_gps.charsProcessed()) {
        io.println("  수신         : 없음 — 모듈 TX→GPIO15 배선·3.3V 전원 확인");
    } else if ((uint32_t)(now - s_lastByte) > GNSS_RX_STALE_MS) {
        io.printf("  수신         : %lus 동안 없음 (끊김)\n", (unsigned long)((now - s_lastByte) / 1000));
    } else if (!s_gps.passedChecksum()) {
        io.println("  수신         : 바이트는 오는데 문장이 없음 — 속도(9600)·잡음 확인, gnss raw 로 원문 보기");
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
}

} // namespace Gnss
