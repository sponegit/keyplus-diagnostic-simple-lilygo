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

// --- BMP280 -----------------------------------------------------------------
// 데이터시트 BST-BMP280-DS001 §3.11(보정식)·§4(레지스터). BME280(칩 ID 0x60)도 온도·기압은 같다.
static const uint8_t BMP_REG_CALIB = 0x88;   // dig_T1..dig_P9, 24바이트 LE
static const uint8_t BMP_REG_ID    = 0xD0;
static const uint8_t BMP_REG_CTRL  = 0xF4;
static const uint8_t BMP_REG_CONF  = 0xF5;
static const uint8_t BMP_REG_DATA  = 0xF7;   // press[3] temp[3]
// ctrl_meas: osrs_t ×2 · osrs_p ×16 · normal 모드 (데이터시트 표 7 "indoor navigation" 에 가깝게)
static const uint8_t BMP_CTRL_VAL  = (0b010 << 5) | (0b101 << 2) | 0b11;
// config: t_standby 500ms · IIR 필터 ×4 — 1초에 한 번 읽으므로 그 사이 측정이 최소 한 번은 끝난다
static const uint8_t BMP_CONF_VAL  = (0b100 << 5) | (0b010 << 2);

static uint8_t  s_bmpAddr    = 0;      // 0 = 아직 못 찾음
static uint8_t  s_bmpChip    = 0;
static uint32_t s_bmpProbeAt = 0;
static bool     s_bmpOk      = false;
static float    s_bmpHpa     = NAN;
static float    s_bmpTempC   = NAN;
static uint32_t s_bmpAt      = 0;
static uint32_t s_bmpReads   = 0, s_bmpNack = 0;

static uint16_t T1; static int16_t T2, T3;
static uint16_t P1; static int16_t P2, P3, P4, P5, P6, P7, P8, P9;

static bool readRegs(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t n)
{
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(addr, n) != n) return false;
    for (uint8_t i = 0; i < n; i++) buf[i] = (uint8_t)Wire.read();
    return true;
}

static bool writeReg(uint8_t addr, uint8_t reg, uint8_t val)
{
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool bmpProbe()
{
    static const uint8_t addrs[] = {0x76, 0x77};
    for (uint8_t a : addrs) {
        uint8_t id;
        if (!readRegs(a, BMP_REG_ID, &id, 1) || (id != 0x58 && id != 0x60)) continue;
        uint8_t c[24];
        if (!readRegs(a, BMP_REG_CALIB, c, sizeof(c))) continue;
        auto u16 = [&](int i) { return (uint16_t)(c[i] | c[i + 1] << 8); };
        T1 = u16(0);           T2 = (int16_t)u16(2);  T3 = (int16_t)u16(4);
        P1 = u16(6);           P2 = (int16_t)u16(8);  P3 = (int16_t)u16(10);
        P4 = (int16_t)u16(12); P5 = (int16_t)u16(14); P6 = (int16_t)u16(16);
        P7 = (int16_t)u16(18); P8 = (int16_t)u16(20); P9 = (int16_t)u16(22);
        // config 는 sleep 모드에서만 확실히 먹는다 — 모드 비트 0 으로 내린 뒤 쓴다.
        if (!writeReg(a, BMP_REG_CTRL, 0x00) || !writeReg(a, BMP_REG_CONF, BMP_CONF_VAL) ||
            !writeReg(a, BMP_REG_CTRL, BMP_CTRL_VAL)) continue;
        s_bmpAddr = a;
        s_bmpChip = id;
        Serial.printf("[AIR] %s 발견 0x%02X\n", id == 0x58 ? "BMP280" : "BME280", a);
        return true;
    }
    return false;
}

static void bmpRead()
{
    uint8_t d[6];
    if (!readRegs(s_bmpAddr, BMP_REG_DATA, d, sizeof(d))) {
        s_bmpNack++;
        s_bmpOk = false;
        // 연속 실패면 뽑힌 것으로 보고 다시 찾는다(다시 꽂으면 재초기화가 필요하다 — 전원이 끊겼다 왔다).
        if (s_bmpNack % 5 == 0) s_bmpAddr = 0;
        return;
    }
    const int32_t adcP = (int32_t)((uint32_t)d[0] << 12 | (uint32_t)d[1] << 4 | d[2] >> 4);
    const int32_t adcT = (int32_t)((uint32_t)d[3] << 12 | (uint32_t)d[4] << 4 | d[5] >> 4);
    if (adcT == 0x80000) { s_bmpOk = false; return; }   // 첫 측정 전(리셋값) — 다음 주기에 다시

    // 데이터시트 §3.11.3 — 32비트 온도, 64비트 기압. 결과: 0.01°C, Pa/256.
    int32_t v1 = ((((adcT >> 3) - ((int32_t)T1 << 1))) * T2) >> 11;
    int32_t v2 = (((((adcT >> 4) - (int32_t)T1) * ((adcT >> 4) - (int32_t)T1)) >> 12) * T3) >> 14;
    const int32_t tFine = v1 + v2;
    s_bmpTempC = ((tFine * 5 + 128) >> 8) / 100.0f;

    int64_t p1 = (int64_t)tFine - 128000;
    int64_t p2 = p1 * p1 * P6;
    p2 = p2 + ((p1 * P5) << 17);
    p2 = p2 + ((int64_t)P4 << 35);
    p1 = ((p1 * p1 * P3) >> 8) + ((p1 * P2) << 12);
    p1 = ((((int64_t)1) << 47) + p1) * P1 >> 33;
    if (p1 == 0) { s_bmpOk = false; return; }           // 0 으로 나누기 방지(데이터시트 그대로)
    int64_t p = 1048576 - adcP;
    p = (((p << 31) - p2) * 3125) / p1;
    p1 = ((int64_t)P9 * (p >> 13) * (p >> 13)) >> 25;
    p2 = ((int64_t)P8 * p) >> 19;
    p = ((p + p1 + p2) >> 8) + ((int64_t)P7 << 4);
    s_bmpHpa = (float)p / 256.0f / 100.0f;

    s_bmpOk = true;
    s_bmpAt = millis();
    s_bmpReads++;
}

static void bmpCycle(uint32_t now)
{
    if (!s_bmpAddr) {
        s_bmpOk = false;
        if (s_bmpProbeAt && (uint32_t)(now - s_bmpProbeAt) < AIR_PROBE_MS) return;
        s_bmpProbeAt = now;
        if (!bmpProbe()) return;
    }
    bmpRead();
}

// --- SCD41 ------------------------------------------------------------------
// 데이터시트(Sensirion SCD4x v1.6) §3. 명령은 16비트, 데이터는 [msb, lsb, crc] 워드 단위(CRC 는 SHT41 과 같다).
// 주기 측정 모드에서 5초마다 새 값이 나온다. ASC·재보정은 건드리지 않는다(작업서 D5 — 공장 기본값 유지).
static const uint8_t  SCD41_ADDR           = 0x62;
static const uint16_t SCD_CMD_START        = 0x21B1;   // start_periodic_measurement
static const uint16_t SCD_CMD_STOP         = 0x3F86;   // stop_periodic_measurement (끝나는 데 500ms)
static const uint16_t SCD_CMD_READY        = 0xE4B8;   // get_data_ready_status
static const uint16_t SCD_CMD_READ         = 0xEC05;   // read_measurement
static const uint16_t SCD_CMD_PRESSURE     = 0xE000;   // set_ambient_pressure (hPa, 측정 중에도 허용)
static const uint16_t SCD_CMD_SERIAL       = 0x3682;   // get_serial_number (idle 에서만)

static bool     s_scdFound    = false;
static uint32_t s_scdProbeAt  = 0;
static bool     s_scdOk       = false;
static uint16_t s_co2         = 0;
static float    s_scdTempC    = NAN;
static float    s_scdRh       = NAN;
static uint32_t s_scdAt       = 0;
static uint32_t s_scdReads    = 0, s_scdNack = 0, s_scdCrc = 0;
static uint8_t  s_scdFailRun  = 0;
static uint16_t s_scdPresSent = 0;   // 마지막으로 보낸 기압(hPa). 0 = 보낸 적 없음
static uint32_t s_scdPresAt   = 0;

static bool scdCmd(uint16_t cmd)
{
    Wire.beginTransmission(SCD41_ADDR);
    Wire.write((uint8_t)(cmd >> 8));
    Wire.write((uint8_t)cmd);
    return Wire.endTransmission() == 0;
}

static bool scdCmdArg(uint16_t cmd, uint16_t arg)
{
    const uint8_t w[2] = {(uint8_t)(arg >> 8), (uint8_t)arg};
    Wire.beginTransmission(SCD41_ADDR);
    Wire.write((uint8_t)(cmd >> 8));
    Wire.write((uint8_t)cmd);
    Wire.write(w, 2);
    Wire.write(crc8(w, 2));
    return Wire.endTransmission() == 0;
}

// 명령 후 1ms(데이터시트 실행 시간) 기다렸다가 n 워드를 읽는다. 0 = 성공, 1 = 응답 없음, 2 = CRC
static int scdReadWords(uint16_t cmd, uint16_t *out, uint8_t n)
{
    if (!scdCmd(cmd)) return 1;
    delay(1);
    const uint8_t len = (uint8_t)(n * 3);
    if (Wire.requestFrom(SCD41_ADDR, len) != len) return 1;
    for (uint8_t i = 0; i < n; i++) {
        uint8_t b[3];
        for (int k = 0; k < 3; k++) b[k] = (uint8_t)Wire.read();
        if (crc8(b, 2) != b[2]) return 2;
        out[i] = (uint16_t)(b[0] << 8 | b[1]);
    }
    return 0;
}

static bool scdProbe()
{
    // 서브만 리셋되면 센서는 전원이 유지된 채 주기 측정 중이다 — 먼저 멈춰야 idle 명령이 먹는다.
    if (!scdCmd(SCD_CMD_STOP)) return false;   // 응답 없음 = 센서 없음, 기다리지 않는다
    delay(500);                                // 데이터시트: stop 후 500ms 동안 명령 금지 (찾았을 때 한 번만)
    uint16_t sn[3];
    if (scdReadWords(SCD_CMD_SERIAL, sn, 3) != 0) return false;
    if (!scdCmd(SCD_CMD_START)) return false;
    s_scdFound    = true;
    s_scdFailRun  = 0;
    s_scdPresSent = 0;
    Serial.printf("[AIR] SCD41 발견 0x%02X — 시리얼 %04X%04X%04X, 첫 값은 약 5초 뒤\n",
                  SCD41_ADDR, sn[0], sn[1], sn[2]);
    return true;
}

static void scdFail(bool crc)
{
    (crc ? s_scdCrc : s_scdNack)++;
    s_scdOk = false;
    // 연속 실패면 뽑힌 것으로 보고 다시 찾는다(다시 꽂으면 주기 측정을 새로 시작해야 한다).
    if (++s_scdFailRun >= 5) s_scdFound = false;
}

// BMP280 기압을 SCD41 에 알린다 — 기압 1hPa 차이가 CO2 약 0.1% 오차. 바뀌었을 때(≥1hPa)나 1분마다.
static void scdPressure(uint32_t now)
{
    if (!s_bmpOk) return;
    const uint16_t hpa = (uint16_t)(s_bmpHpa + 0.5f);
    if (hpa < 700 || hpa > 1200) return;   // 데이터시트 허용 범위
    const bool changed = !s_scdPresSent || (hpa > s_scdPresSent ? hpa - s_scdPresSent : s_scdPresSent - hpa) >= 1;
    if (!changed && (uint32_t)(now - s_scdPresAt) < 60000UL) return;
    if (scdCmdArg(SCD_CMD_PRESSURE, hpa)) {
        s_scdPresSent = hpa;
        s_scdPresAt   = now;
    }
}

static void scdCycle(uint32_t now)
{
    if (!s_scdFound) {
        s_scdOk = false;
        if (s_scdProbeAt && (uint32_t)(now - s_scdProbeAt) < AIR_PROBE_MS) return;
        s_scdProbeAt = now;
        scdProbe();
        return;   // 찾았어도 방금 시작했다 — 첫 값은 5초 뒤
    }
    scdPressure(now);
    uint16_t ready;
    int r = scdReadWords(SCD_CMD_READY, &ready, 1);
    if (r != 0) { scdFail(r == 2); return; }
    if ((ready & 0x07FF) == 0) { s_scdFailRun = 0; return; }   // 아직 새 값 없음(5초 주기)
    uint16_t m[3];
    r = scdReadWords(SCD_CMD_READ, m, 3);
    if (r != 0) { scdFail(r == 2); return; }
    s_co2      = m[0];
    s_scdTempC = -45.0f + 175.0f * m[1] / 65535.0f;
    s_scdRh    = 100.0f * m[2] / 65535.0f;
    s_scdOk    = true;
    s_scdAt    = now;
    s_scdFailRun = 0;
    s_scdReads++;
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
    if (s_bmpOk)
        io.printf("[AIR] BMP280 %.2fhPa  %.2f°C\n", s_bmpHpa, s_bmpTempC);
    else
        io.printf("[AIR] BMP280 없음%s\n", s_bmpAddr ? " (측정 대기·읽기 실패)" : " (0x76·0x77 응답 없음)");
    if (s_scdOk)
        io.printf("[AIR] SCD41  %uppm CO2  (%.1f°C %.0f%%RH, %lus 전)\n", (unsigned)s_co2, s_scdTempC, s_scdRh,
                  (unsigned long)((millis() - s_scdAt) / 1000));
    else
        io.printf("[AIR] SCD41  %s\n", s_scdFound ? "첫 측정 대기(5초 주기)" : "없음 (0x62 응답 없음)");
}

void update()
{
    const uint32_t now = millis();
    switch (s_phase) {
    case Phase::IDLE:
        if ((uint32_t)(now - s_lastCycle) < AIR_CYCLE_MS) return;
        s_lastCycle = now;
        bmpCycle(now);   // 연속 측정 중이라 결과만 읽는다 — SHT41 변환을 기다리는 동안 버스가 비어 있다
        scdCycle(now);   // BMP280 다음 — 방금 읽은 기압으로 보정
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
    if (s_bmpOk)
        io.printf("  BMP280       : %.2fhPa %.2f°C (0x%02X, %lums 전)\n", s_bmpHpa, s_bmpTempC,
                  s_bmpAddr, (unsigned long)(millis() - s_bmpAt));
    else
        io.printf("  BMP280       : 없음%s\n", s_bmpAddr ? " (읽기 실패)" : "");
    io.printf("  BMP280 stats : ok %lu, nack %lu\n", (unsigned long)s_bmpReads, (unsigned long)s_bmpNack);
    if (s_scdOk)
        io.printf("  SCD41        : %uppm %.1f°C %.0f%%RH (%lus 전)\n", (unsigned)s_co2, s_scdTempC, s_scdRh,
                  (unsigned long)((millis() - s_scdAt) / 1000));
    else
        io.printf("  SCD41        : %s\n", s_scdFound ? "첫 측정 대기" : "없음");
    io.printf("  SCD41 stats  : ok %lu, nack %lu, crc %lu\n", (unsigned long)s_scdReads,
              (unsigned long)s_scdNack, (unsigned long)s_scdCrc);
    if (s_scdPresSent) io.printf("  SCD41 기압   : %uhPa 적용\n", (unsigned)s_scdPresSent);
    else               io.println("  SCD41 기압   : 미적용(BMP280 값 대기) — 해발 0m 기본값으로 계산");
}

} // namespace Air
