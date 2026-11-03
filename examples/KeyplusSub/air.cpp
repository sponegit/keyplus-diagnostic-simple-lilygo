/**
 * @file      air.cpp
 * @brief     실내 공기질 센서 구현. 설계는 air.h.
 */
#include "air.h"
#include "config.h"

#include <Wire.h>

namespace Air {

// --- SHT41 ------------------------------------------------------------------
// 데이터시트: 명령 1바이트 → (변환) → 6바이트 [T msb, T lsb, crc, RH msb, RH lsb, crc].
// CRC-8 다항식 0x31, 초기값 0xFF. 고정밀 측정 변환 최대 8.3ms.
static const uint8_t  SHT41_ADDR        = 0x44;
static const uint8_t  SHT41_CMD_MEASURE = 0xFD;   // 고정밀, 히터 끔
static const uint32_t SHT41_CONV_MS     = 10;

enum class Phase : uint8_t { IDLE, CONVERTING };

static Phase    s_phase     = Phase::IDLE;
static uint32_t s_cmdAt     = 0;
static uint32_t s_lastCycle = 0;
static bool     s_watch     = false;

static bool     s_shtOk     = false;   // 마지막 측정 성공 여부
static float    s_tempC     = NAN;
static float    s_rh        = NAN;
static uint32_t s_shtAt     = 0;       // 마지막 성공 시각(ms)
static uint32_t s_shtReads  = 0, s_shtNack = 0, s_shtCrc = 0;

static uint8_t crc8(const uint8_t *p, size_t n)
{
    uint8_t c = 0xFF;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x31) : (uint8_t)(c << 1);
    }
    return c;
}

static bool shtStart()
{
    Wire.beginTransmission(SHT41_ADDR);
    Wire.write(SHT41_CMD_MEASURE);
    return Wire.endTransmission() == 0;
}

static void shtFinish()
{
    uint8_t b[6];
    size_t  n = Wire.requestFrom(SHT41_ADDR, (uint8_t)sizeof(b));
    if (n != sizeof(b)) {
        s_shtNack++;
        s_shtOk = false;
        return;
    }
    for (size_t i = 0; i < sizeof(b); i++) b[i] = (uint8_t)Wire.read();
    if (crc8(b, 2) != b[2] || crc8(b + 3, 2) != b[5]) {
        s_shtCrc++;
        s_shtOk = false;
        return;
    }
    const uint16_t rawT  = (uint16_t)(b[0] << 8 | b[1]);
    const uint16_t rawRh = (uint16_t)(b[3] << 8 | b[4]);
    s_tempC = -45.0f + 175.0f * rawT / 65535.0f;
    float rh = -6.0f + 125.0f * rawRh / 65535.0f;   // 데이터시트 권장: 0~100 으로 자른다
    s_rh    = rh < 0 ? 0 : (rh > 100 ? 100 : rh);
    s_shtOk = true;
    s_shtAt = millis();
    s_shtReads++;
}

// --- 공통 -------------------------------------------------------------------

void begin()
{
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
    Wire.setTimeOut(I2C_TIMEOUT_MS);
}

static void printValues(Stream &io)
{
    if (s_shtOk)
        io.printf("[AIR] SHT41  %.2f°C  %.1f%%RH\n", s_tempC, s_rh);
    else
        io.printf("[AIR] SHT41  없음 (응답 없음 %lu · CRC 오류 %lu)\n",
                  (unsigned long)s_shtNack, (unsigned long)s_shtCrc);
}

void update()
{
    const uint32_t now = millis();
    switch (s_phase) {
    case Phase::IDLE:
        if ((uint32_t)(now - s_lastCycle) < AIR_CYCLE_MS) return;
        s_lastCycle = now;
        if (shtStart()) {
            s_cmdAt = now;
            s_phase = Phase::CONVERTING;
        } else {
            s_shtNack++;
            s_shtOk = false;
            if (s_watch) printValues(Serial);
        }
        break;
    case Phase::CONVERTING:
        if ((uint32_t)(now - s_cmdAt) < SHT41_CONV_MS) return;
        shtFinish();
        s_phase = Phase::IDLE;
        if (s_watch) printValues(Serial);
        break;
    }
}

static void scan(Stream &io)
{
    io.printf("[I2C] 스캔 SDA=%d SCL=%d @%luHz\n", PIN_I2C_SDA, PIN_I2C_SCL, (unsigned long)I2C_FREQ_HZ);
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() != 0) continue;
        const char *name = a == 0x44 ? "SHT41"  : a == 0x62 ? "SCD41"  : a == 0x59 ? "SGP41" :
                           a == 0x76 || a == 0x77 ? "BMP280" : a == 0x40 ? "INA228" : "?";
        io.printf("  0x%02X  %s\n", a, name);
        found++;
    }
    io.printf("[I2C] %d개 응답%s\n", found, found ? "" : " — 배선·전원(3V3)·Qwiic 커넥터 확인");
}

bool tryConsole(const char *line)
{
    if (!strcmp(line, "i2c")) {
        scan(Serial);
        return true;
    }
    if (!strcmp(line, "air")) {
        printValues(Serial);
        return true;
    }
    if (!strcmp(line, "air watch")) {
        s_watch = !s_watch;
        Serial.printf("[AIR] watch %s\n", s_watch ? "켬 — 새 값마다 출력" : "끔");
        return true;
    }
    return false;
}

void printStatus(Stream &io)
{
    io.println("[AIR] 공기질 센서");
    if (s_shtOk)
        io.printf("  SHT41        : %.2f°C %.1f%%RH (%lums 전)\n", s_tempC, s_rh,
                  (unsigned long)(millis() - s_shtAt));
    else
        io.println("  SHT41        : 없음");
    io.printf("  SHT41 stats  : ok %lu, nack %lu, crc %lu\n", (unsigned long)s_shtReads,
              (unsigned long)s_shtNack, (unsigned long)s_shtCrc);
}

} // namespace Air
