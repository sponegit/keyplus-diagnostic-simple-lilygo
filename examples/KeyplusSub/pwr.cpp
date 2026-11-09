/**
 * @file      pwr.cpp
 * @brief     INA228 구현. 설계는 pwr.h.
 */
#include "pwr.h"
#include "config.h"
#include "guard.h"
#include "link.h"

#include <KpLink.h>
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

static KpPower::UvMachine s_uv({UV_WARN_MV, UV_CUT_MV, UV_RECOVER_MV, UV_BAND_MV, UV_WARN_MS, UV_CUT_MS,
                                UV_RECOVER_MS, UV_GAP_MS});
static bool     s_simOn    = false;
static float    s_simV     = 0;
static uint32_t s_uvAt     = 0;   // 마지막 전이 시각
static uint32_t s_uvTrips  = 0;
static uint32_t s_pubAt    = 0;
static uint32_t s_pubSent = 0, s_pubSkip = 0;

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

bool get(int32_t *mv, int32_t *ma)
{
    if (!s_ok && !s_simOn) return false;
    *mv = (int32_t)lroundf((s_simOn ? s_simV : s_v) * 1000.0f);
    *ma = s_ok ? (int32_t)lroundf(s_a * 1000.0f) : 0;
    return true;
}

KpPower::Uv uv() { return s_uv.state(); }

static void publish(uint32_t now, int32_t mv, int32_t ma, bool force)
{
    if (!force && s_pubAt && (uint32_t)(now - s_pubAt) < PWR_PUB_MS) return;
    s_pubAt = now;
    KpPower::Values v;
    v.mv = (uint16_t)constrain(mv, 0, 65535);
    v.ma = (int16_t)constrain(ma, -32768, 32767);
    v.uv = (uint8_t)s_uv.state();
    uint8_t buf[KPPOWER_PAYLOAD_LEN];
    const size_t n = KpPower::encode(v, buf, sizeof(buf));
    // 링크가 내려가 있으면 버린다 — 측정값은 최신값만 의미가 있다(작업서 §3.3).
    if (Link::sendFrame(KpLink::POWER, buf, (uint16_t)n)) s_pubSent++;
    else                                                 s_pubSkip++;
}

// 이번 주기 값으로 저전압을 판정하고 발행한다. 값이 없으면 판정을 멈춘다(상태 유지).
static void judge(uint32_t now)
{
    int32_t mv, ma;
    if (!get(&mv, &ma)) return;
    const bool changed = s_uv.update(now, (uint16_t)constrain(mv, 0, 65535));
    if (changed) {
        const KpPower::Uv st = s_uv.state();
        s_uvAt = now;
        if (st != KpPower::Uv::NORMAL) s_uvTrips++;
        Serial.printf("[PWR] %s — %.2f V%s\n", KpPower::uvName(st), mv / 1000.0f,
                      st == KpPower::Uv::NORMAL ? ", 센서 레일 복귀 허용" : ", 센서 레일 차단");
        Guard::setPowerBlock(st != KpPower::Uv::NORMAL);
    }
    publish(now, mv, ma, changed);
}

void update()
{
    const uint32_t now = millis();
    if ((uint32_t)(now - s_cycleAt) < PWR_CYCLE_MS) return;
    s_cycleAt = now;
    if (!s_found) {
        s_ok = false;
        if (!s_probeAt || (uint32_t)(now - s_probeAt) >= AIR_PROBE_MS) {
            s_probeAt = now;
            probe();   // 찾았어도 이번 주기는 값 없음 — 첫 평균이 끝나기 전
        }
    } else {
        read();
    }
    judge(now);
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
    if (!strcmp(line, "pwr sim off")) {
        s_simOn = false;
        Serial.println("[PWR] 가짜 전압 끔 — INA228 값으로 판정");
        return true;
    }
    if (!strncmp(line, "pwr sim ", 8)) {
        char *end;
        const float v = strtof(line + 8, &end);
        if (end == line + 8 || v < 0 || v > 60) {
            Serial.println("[PWR] 사용법: pwr sim <V> (0~60) | pwr sim off");
            return true;
        }
        s_simOn = true;
        s_simV  = v;
        Serial.printf("[PWR] 가짜 전압 %.2f V — 판정·POWER 프레임에 이 값을 쓴다(전류는 실측)\n", v);
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
    if (s_simOn) io.printf("  sim          : %.2f V (가짜 — pwr sim off 로 해제)\n", s_simV);

    const uint32_t now = millis();
    io.printf("  저전압       : %s (%lus 전 바뀜, 경고·차단 %lu회)\n", KpPower::uvName(s_uv.state()),
              (unsigned long)((now - s_uvAt) / 1000), (unsigned long)s_uvTrips);
    KpPower::Uv next;
    uint32_t    left;
    if (s_uv.pending(now, &next, &left))
        io.printf("  진행 중      : %s 까지 %lus\n", KpPower::uvName(next), (unsigned long)((left + 999) / 1000));
    io.printf("  기준         : 경고 <%.1fV %lus / 차단 <%.1fV %lus / 복귀 ≥%.1fV %lus\n", UV_WARN_MV / 1000.0f,
              (unsigned long)(UV_WARN_MS / 1000), UV_CUT_MV / 1000.0f, (unsigned long)(UV_CUT_MS / 1000),
              UV_RECOVER_MV / 1000.0f, (unsigned long)(UV_RECOVER_MS / 1000));
    io.printf("  POWER 발행   : 보냄 %lu, 못 보냄(링크 다운) %lu, 주기 %lus\n", (unsigned long)s_pubSent,
              (unsigned long)s_pubSkip, (unsigned long)(PWR_PUB_MS / 1000));
}

} // namespace Pwr
