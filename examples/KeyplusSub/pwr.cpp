/**
 * @file      pwr.cpp
 * @brief     INA228 구현. 설계는 pwr.h.
 */
#include "pwr.h"
#include "config.h"

#include <Wire.h>

namespace Pwr {

// 데이터시트 TI INA228 (SBOSA20) §7.6 레지스터. 모두 MSB 먼저.
static const uint8_t INA_ADDR        = 0x40;
static const uint8_t REG_CONFIG      = 0x00;
static const uint8_t REG_ADC_CONFIG  = 0x01;
static const uint8_t REG_VSHUNT      = 0x04;   // 24비트, 상위 20비트 2의 보수
static const uint8_t REG_VBUS        = 0x05;   // 24비트, 상위 20비트
static const uint8_t REG_DIETEMP     = 0x06;   // 16비트 2의 보수
static const uint8_t REG_MFR_ID      = 0x3E;   // 0x5449 "TI"
static const uint8_t REG_DEVICE_ID   = 0x3F;   // 상위 12비트 0x228

static const uint16_t CONFIG_RST      = 0x8000;
static const uint16_t CONFIG_ADCRANGE = 0x0010;   // ±40.96mV — 2mΩ 에서 ±20.48A
// MODE=0xF(연속 T·VBUS·VSHUNT) · VBUSCT·VSHCT·VTCT=1052µs · AVG=128
static const uint16_t ADC_CONFIG_VAL  = (0xF << 12) | (5 << 9) | (5 << 6) | (5 << 3) | 4;

// LSB (ADCRANGE=1): VSHUNT 78.125nV, VBUS 195.3125µV, 온도 7.8125m°C
static const float VSHUNT_LSB_V = 78.125e-9f;
static const float VBUS_LSB_V   = 195.3125e-6f;
static const float TEMP_LSB_C   = 7.8125e-3f;

static bool     s_found    = false;
static uint32_t s_probeAt  = 0;
static uint32_t s_cycleAt  = 0;
static bool     s_ok       = false;
static float    s_v = NAN, s_a = NAN, s_tempC = NAN;
static float    s_vMin = NAN, s_vMax = NAN, s_aMax = NAN;
static uint32_t s_at       = 0;
static uint32_t s_reads = 0, s_nack = 0;
static uint8_t  s_failRun  = 0;

static bool readReg(uint8_t reg, uint8_t *buf, uint8_t n)
{
    Wire.beginTransmission(INA_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(INA_ADDR, n) != n) return false;
    for (uint8_t i = 0; i < n; i++) buf[i] = (uint8_t)Wire.read();
    return true;
}

static bool writeReg16(uint8_t reg, uint16_t v)
{
    Wire.beginTransmission(INA_ADDR);
    Wire.write(reg);
    Wire.write((uint8_t)(v >> 8));
    Wire.write((uint8_t)v);
    return Wire.endTransmission() == 0;
}

static bool probe()
{
    uint8_t b[2];
    if (!readReg(REG_MFR_ID, b, 2) || (b[0] << 8 | b[1]) != 0x5449) return false;
    if (!readReg(REG_DEVICE_ID, b, 2) || ((b[0] << 8 | b[1]) >> 4) != 0x228) return false;
    // 서브만 리셋되면 이전 설정이 남아 있다 — 리셋 후 다시 쓴다.
    if (!writeReg16(REG_CONFIG, CONFIG_RST)) return false;
    delay(1);
    if (!writeReg16(REG_CONFIG, CONFIG_ADCRANGE) || !writeReg16(REG_ADC_CONFIG, ADC_CONFIG_VAL)) return false;
    s_found   = true;
    s_failRun = 0;
    Serial.printf("[PWR] INA228 발견 0x%02X — 션트 %lu µΩ, 첫 값은 약 0.5초 뒤\n", INA_ADDR,
                  (unsigned long)PWR_SHUNT_UOHM);
    return true;
}

static int32_t raw20(const uint8_t *b, bool sign)
{
    int32_t v = (int32_t)((uint32_t)b[0] << 12 | (uint32_t)b[1] << 4 | b[2] >> 4);
    if (sign && (v & 0x80000)) v -= 0x100000;
    return v;
}

static void read()
{
    uint8_t sh[3], bus[3], t[2];
    if (!readReg(REG_VSHUNT, sh, 3) || !readReg(REG_VBUS, bus, 3) || !readReg(REG_DIETEMP, t, 2)) {
        s_nack++;
        s_ok = false;
        if (++s_failRun >= 5) s_found = false;   // 뽑힌 것으로 보고 다시 찾는다
        return;
    }
    s_failRun = 0;
    s_v     = raw20(bus, false) * VBUS_LSB_V;
    s_a     = raw20(sh, true) * VSHUNT_LSB_V / (PWR_SHUNT_UOHM * 1e-6f);
    s_tempC = (int16_t)(t[0] << 8 | t[1]) * TEMP_LSB_C;
    s_ok    = true;
    s_at    = millis();
    s_reads++;
    if (isnan(s_vMin) || s_v < s_vMin) s_vMin = s_v;
    if (isnan(s_vMax) || s_v > s_vMax) s_vMax = s_v;
    if (isnan(s_aMax) || s_a > s_aMax) s_aMax = s_a;
}

void begin() {}   // Wire 는 Air::begin 이 연다(같은 버스)

void update()
{
    const uint32_t now = millis();
    if ((uint32_t)(now - s_cycleAt) < PWR_CYCLE_MS) return;
    s_cycleAt = now;
    if (!s_found) {
        s_ok = false;
        if (s_probeAt && (uint32_t)(now - s_probeAt) < AIR_PROBE_MS) return;
        s_probeAt = now;
        probe();
        return;   // 방금 설정했다 — 첫 평균이 끝나기 전
    }
    read();
}

bool get(int32_t *mv, int32_t *ma)
{
    if (!s_ok) return false;
    *mv = (int32_t)lroundf(s_v * 1000.0f);
    *ma = (int32_t)lroundf(s_a * 1000.0f);
    return true;
}

bool tryConsole(const char *line)
{
    if (!strcmp(line, "pwr")) {
        printStatus(Serial);
        return true;
    }
    if (!strcmp(line, "pwr reset")) {
        s_vMin = s_vMax = s_aMax = NAN;
        Serial.println("[PWR] 최소/최대 초기화");
        return true;
    }
    return false;
}

void printStatus(Stream &io)
{
    io.println("[PWR] INA228");
    if (s_ok)
        io.printf("  now          : %.3f V  %.1f mA  (칩 %.1f°C, %lums 전)\n", s_v, s_a * 1000.0f, s_tempC,
                  (unsigned long)(millis() - s_at));
    else
        io.printf("  now          : %s\n", s_found ? "읽기 실패" : "없음 (0x40 응답 없음 — VS 3.3V·GND·SDA/SCL 확인)");
    if (!isnan(s_vMin))
        io.printf("  min/max      : %.3f ~ %.3f V, 최대 %.1f mA (pwr reset 으로 초기화)\n", s_vMin, s_vMax,
                  s_aMax * 1000.0f);
    io.printf("  stats        : ok %lu, nack %lu, 션트 %lu µΩ\n", (unsigned long)s_reads, (unsigned long)s_nack,
              (unsigned long)PWR_SHUNT_UOHM);
}

} // namespace Pwr
