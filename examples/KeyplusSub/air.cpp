/**
 * @file      air.cpp
 * @brief     실내 공기질 센서 구현. 설계는 air.h.
 */
#include "air.h"
#include "config.h"
#include "pms.h"
#include "co.h"
#include "link.h"
#include "guard.h"

#include <KpAir.h>
#include <KpLink.h>
#include <Wire.h>
#include <VOCGasIndexAlgorithm.h>
#include <NOxGasIndexAlgorithm.h>

namespace Air {

// --- SHT41 ------------------------------------------------------------------
// 데이터시트: 명령 1바이트 → (변환) → 6바이트 [T msb, T lsb, crc, RH msb, RH lsb, crc].
// CRC-8 다항식 0x31, 초기값 0xFF. 고정밀 측정 변환 최대 8.3ms.
static const uint8_t  SHT41_ADDR        = 0x44;
static const uint8_t  SHT41_CMD_MEASURE = 0xFD;   // 고정밀, 히터 끔
static const uint32_t SHT41_CONV_MS     = 10;

enum class Phase : uint8_t { IDLE, CONVERTING, SGP_CONVERTING };

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
static uint8_t  s_scdSkip     = 0;   // 레일을 켠 뒤 버릴 남은 값 수
static bool     s_scdRunning  = false;   // 주기 측정 중 — 주차 듀티가 쉬면 멈춘다(Guard::airAwake)
static uint32_t s_scdStopAt   = 0;       // 마지막 stop 시각 — 데이터시트: stop 후 500ms 명령 금지
static uint32_t s_scdDutyStops = 0;

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
    s_scdRunning  = true;   // 쉬는 중이면 다음 주기 scdCycle 이 바로 멈춘다
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
    // 주차 공기질 듀티 — 쉬면 주기 측정만 멈춘다(3.3V 레일은 SGP41 때문에 켜 둔다). 깨우면 다시 시작하고
    // 레일 복귀 때와 같이 첫 AIR_SCD_SKIP 값을 버린다 → 첫 값까지 약 20초(5초 × (1 + 3)).
    if (!Guard::airAwake()) {
        s_scdOk = false;
        if (s_scdRunning) {
            if (scdCmd(SCD_CMD_STOP)) {
                s_scdRunning = false;
                s_scdStopAt  = now;
                s_scdDutyStops++;
            } else {
                scdFail(false);
            }
        }
        return;
    }
    if (!s_scdRunning) {
        if ((uint32_t)(now - s_scdStopAt) < 600) return;   // stop 직후 500ms 금지
        if (!scdCmd(SCD_CMD_START)) { scdFail(false); return; }
        s_scdRunning  = true;
        s_scdSkip     = AIR_SCD_SKIP;
        s_scdPresSent = 0;   // 기압을 다시 알린다
        return;              // 첫 값은 5초 뒤
    }
    scdPressure(now);
    uint16_t ready;
    int r = scdReadWords(SCD_CMD_READY, &ready, 1);
    if (r != 0) { scdFail(r == 2); return; }
    if ((ready & 0x07FF) == 0) { s_scdFailRun = 0; return; }   // 아직 새 값 없음(5초 주기)
    uint16_t m[3];
    r = scdReadWords(SCD_CMD_READ, m, 3);
    if (r != 0) { scdFail(r == 2); return; }
    s_scdFailRun = 0;
    if (s_scdSkip) { s_scdSkip--; return; }   // 레일 복귀 직후 — 첫 몇 값은 안정되지 않았다
    s_co2      = m[0];
    s_scdTempC = -45.0f + 175.0f * m[1] / 65535.0f;
    s_scdRh    = 100.0f * m[2] / 65535.0f;
    s_scdOk    = true;
    s_scdAt    = now;
    s_scdFailRun = 0;
    s_scdReads++;
}

// --- SGP41 ------------------------------------------------------------------
// 데이터시트(Sensirion SGP41 v1.0) §4. 원시값(SRAW)을 Sensirion gas index 알고리즘(라이브러리)에 1초마다
// 넣어 VOC·NOx 지수를 얻는다. 지수는 상대값이다 — 100 = 그 환경의 평소(학습된 기준), 높을수록 나쁨.
// ⚠️ 알고리즘은 정확히 1초 간격을 전제한다(AIR_CYCLE_MS). 주기를 바꾸면 생성자 sampling_interval 도 바꿀 것.
static const uint8_t  SGP41_ADDR       = 0x59;
static const uint16_t SGP_CMD_COND     = 0x2612;   // execute_conditioning — 켠 뒤 처음 10초만
static const uint16_t SGP_CMD_MEASURE  = 0x2619;   // measure_raw_signals
static const uint16_t SGP_CMD_SERIAL   = 0x3682;
static const uint32_t SGP_CONV_MS      = 50;
static const uint8_t  SGP_COND_CYCLES  = 10;       // 데이터시트: 10초 권장, 넘기면 NOx 픽셀 손상

static VOCGasIndexAlgorithm s_vocAlgo;
static NOxGasIndexAlgorithm s_noxAlgo;
static bool     s_sgpFound   = false;
static uint32_t s_sgpProbeAt = 0;
static uint8_t  s_sgpCond    = 0;      // 남은 컨디셔닝 횟수
static bool     s_sgpCondNow = false;  // 지금 변환 중인 것이 컨디셔닝인가
static bool     s_sgpOk      = false;
static uint16_t s_srawVoc    = 0, s_srawNox = 0;
static int32_t  s_vocIndex   = 0, s_noxIndex = 0;
static uint32_t s_sgpAt      = 0;
static uint32_t s_sgpReads   = 0, s_sgpNack = 0, s_sgpCrc = 0;
static uint8_t  s_sgpFailRun = 0;

static bool sgpWriteCmd(uint16_t cmd, const uint16_t *args, uint8_t n)
{
    Wire.beginTransmission(SGP41_ADDR);
    Wire.write((uint8_t)(cmd >> 8));
    Wire.write((uint8_t)cmd);
    for (uint8_t i = 0; i < n; i++) {
        const uint8_t w[2] = {(uint8_t)(args[i] >> 8), (uint8_t)args[i]};
        Wire.write(w, 2);
        Wire.write(crc8(w, 2));
    }
    return Wire.endTransmission() == 0;
}

// n 워드를 읽는다. 0 = 성공, 1 = 응답 없음, 2 = CRC
static int sgpReadWords(uint16_t *out, uint8_t n)
{
    const uint8_t len = (uint8_t)(n * 3);
    if (Wire.requestFrom(SGP41_ADDR, len) != len) return 1;
    for (uint8_t i = 0; i < n; i++) {
        uint8_t b[3];
        for (int k = 0; k < 3; k++) b[k] = (uint8_t)Wire.read();
        if (crc8(b, 2) != b[2]) return 2;
        out[i] = (uint16_t)(b[0] << 8 | b[1]);
    }
    return 0;
}

static void sgpProbe(uint32_t now)
{
    if (s_sgpProbeAt && (uint32_t)(now - s_sgpProbeAt) < AIR_PROBE_MS) return;
    s_sgpProbeAt = now;
    uint16_t sn[3];
    if (!sgpWriteCmd(SGP_CMD_SERIAL, nullptr, 0)) return;
    delay(1);
    if (sgpReadWords(sn, 3) != 0) return;
    s_sgpFound   = true;
    s_sgpFailRun = 0;
    s_sgpCond    = SGP_COND_CYCLES;
    s_vocAlgo.reset();   // 새로 꽂힌 센서 — 학습을 처음부터
    s_noxAlgo.reset();
    Serial.printf("[AIR] SGP41 발견 0x%02X — 시리얼 %04X%04X%04X, 컨디셔닝 %u초\n",
                  SGP41_ADDR, sn[0], sn[1], sn[2], (unsigned)SGP_COND_CYCLES);
}

static void sgpFail(bool crc)
{
    (crc ? s_sgpCrc : s_sgpNack)++;
    s_sgpOk = false;
    if (++s_sgpFailRun >= 5) s_sgpFound = false;   // 뽑힌 것으로 보고 다시 찾는다(컨디셔닝부터 다시)
}

// SHT41 측정 직후에 부른다 — 방금 읽은 온·습도로 보상한다(없으면 데이터시트 기본값 50%RH·25°C).
static bool sgpStart()
{
    uint16_t args[2] = {0x8000, 0x6666};
    if (s_shtOk) {
        args[0] = (uint16_t)(s_rh * 65535.0f / 100.0f);
        args[1] = (uint16_t)((s_tempC + 45.0f) * 65535.0f / 175.0f);
    }
    s_sgpCondNow = s_sgpCond > 0;
    if (!sgpWriteCmd(s_sgpCondNow ? SGP_CMD_COND : SGP_CMD_MEASURE, args, 2)) {
        sgpFail(false);
        return false;
    }
    return true;
}

static void sgpFinish(uint32_t now)
{
    uint16_t w[2];
    int r = sgpReadWords(w, s_sgpCondNow ? 1 : 2);
    if (r != 0) { sgpFail(r == 2); return; }
    s_sgpFailRun = 0;
    if (s_sgpCondNow) {   // 컨디셔닝 중에는 VOC 원시값만 나온다 — 지수 계산에 넣지 않는다
        s_srawVoc = w[0];
        if (--s_sgpCond == 0) Serial.println("[AIR] SGP41 컨디셔닝 끝 — 지수 학습 시작(약 1분 뒤부터 값)");
        return;
    }
    s_srawVoc  = w[0];
    s_srawNox  = w[1];
    s_vocIndex = s_vocAlgo.process(s_srawVoc);
    s_noxIndex = s_noxAlgo.process(s_srawNox);
    s_sgpOk    = true;
    s_sgpAt    = now;
    s_sgpReads++;
}

// --- AIR 발행 ---------------------------------------------------------------
// 지금 믿을 만한 값만 담는다. SGP41 지수 0 = 학습 전, SCD41 은 마지막 값이 오래되면 뺀다.
static uint32_t s_pubAt    = 0;
static uint32_t s_pubSent  = 0, s_pubSkip = 0;
static uint16_t s_pubMask  = 0;

static int16_t to10(float v) { return (int16_t)(v >= 0 ? v * 10.0f + 0.5f : v * 10.0f - 0.5f); }

static void collect(KpAir::Values &v, uint32_t now)
{
    v = {};
    if (s_shtOk) {
        v.mask |= KpAir::T | KpAir::RH;
        v.t10  = to10(s_tempC);
        v.rh10 = (uint16_t)to10(s_rh);
    }
    if (s_bmpOk) {
        v.mask |= KpAir::P;
        v.p10  = (uint16_t)to10(s_bmpHpa);
    }
    if (s_scdOk && (uint32_t)(now - s_scdAt) < AIR_SCD_STALE_MS) {
        v.mask |= KpAir::CO2;
        v.co2  = s_co2;
    }
    if (s_sgpOk && s_vocIndex > 0) {
        v.mask |= KpAir::VOC | KpAir::NOX;
        v.voc  = (uint16_t)s_vocIndex;
        v.nox  = (uint16_t)s_noxIndex;
    }
    if (Pms::get(&v.pm1, &v.pm25, &v.pm10)) v.mask |= KpAir::PM;
    if (Co::get(&v.co10))                    v.mask |= KpAir::CO;
    if (GUARD_TEMP_ENABLE && Guard::known()) {   // 온도 가드를 끄면 guard 키째 생략(config.h)
        v.mask |= KpAir::GUARD;
        v.guard = (uint8_t)Guard::state();
    }
}

// --- 센서 레일(GPIO40) 전이 -----------------------------------------------------
// 꺼지면 레일 위 센서(BMP280·SCD41·SGP41)를 "없음"으로 돌리고 통신하지 않는다. 켜지면 다시 찾는다 —
// 전원이 끊겼다 왔으니 SCD41 은 주기 측정을 새로 시작하고 SGP41 은 컨디셔닝부터 다시 한다.
static bool s_railWasOn = false;

static void railOff()
{
    s_bmpAddr  = 0;     s_bmpOk = false;
    s_scdFound = false; s_scdOk = false; s_scdRunning = false;
    s_sgpFound = false; s_sgpOk = false;
    s_sgpCond  = 0;
}

static void railOn(uint32_t now)
{
    // 전원이 막 들어왔다 — 바로 찾지 말고 AIR_PROBE_MS 뒤에(SCD41 기동 1초 등).
    s_bmpProbeAt = s_scdProbeAt = s_sgpProbeAt = now;
    s_scdSkip = AIR_SCD_SKIP;
}

static void publish(uint32_t now)
{
    if (s_pubAt && (uint32_t)(now - s_pubAt) < AIR_PUB_MS) return;
    s_pubAt = now;
    KpAir::Values v;
    collect(v, now);
    s_pubMask = v.mask;
    uint8_t buf[KPAIR_PAYLOAD_LEN];
    const size_t n = KpAir::encode(v, buf, sizeof(buf));
    // 링크가 내려가 있으면 버린다 — 측정값은 최신값만 의미가 있다(작업서 §3.3).
    if (v.mask && Link::sendFrame(KpLink::AIR, buf, (uint16_t)n)) s_pubSent++;
    else                                                        s_pubSkip++;
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
    if (!Guard::sensorsOn()) {
        io.printf("[AIR] 센서 레일 꺼짐 — %s (BMP280·SCD41·SGP41·PMS5003·ZE15-CO 측정 안 함, guard·pwr)\n",
                  !Guard::known() ? "첫 판단 전" :
                  Guard::state() == KpAir::Guard::HOT ? "고온 차단" :
                  Guard::state() == KpAir::Guard::COLD ? "저온 차단" : "저전압 차단(pwr)");
        return;
    }
    if (s_bmpOk)
        io.printf("[AIR] BMP280 %.2fhPa  %.2f°C\n", s_bmpHpa, s_bmpTempC);
    else
        io.printf("[AIR] BMP280 없음%s\n", s_bmpAddr ? " (측정 대기·읽기 실패)" : " (0x76·0x77 응답 없음)");
    if (s_scdOk)
        io.printf("[AIR] SCD41  %uppm CO2  (%.1f°C %.0f%%RH, %lus 전)\n", (unsigned)s_co2, s_scdTempC, s_scdRh,
                  (unsigned long)((millis() - s_scdAt) / 1000));
    else
        io.printf("[AIR] SCD41  %s\n", !s_scdFound ? "없음 (0x62 응답 없음)" :
                                       !s_scdRunning ? "멈춤 (주차 듀티)" : "첫 측정 대기(5초 주기)");
    if (s_sgpOk && s_vocIndex > 0)
        io.printf("[AIR] SGP41  VOC %ld  NOx %ld  (raw %u/%u)\n", (long)s_vocIndex, (long)s_noxIndex,
                  (unsigned)s_srawVoc, (unsigned)s_srawNox);
    else if (s_sgpOk)
        io.printf("[AIR] SGP41  지수 학습 중 (raw %u/%u)\n", (unsigned)s_srawVoc, (unsigned)s_srawNox);
    else
        io.printf("[AIR] SGP41  %s\n", !s_sgpFound ? "없음 (0x59 응답 없음)" :
                                       s_sgpCond ? "컨디셔닝 중" : "읽기 실패");
    Pms::printValues(io);   // UART 센서지만 'air' 한 화면에서 같이 본다
    Co::printValues(io);
}

void update()
{
    const uint32_t now = millis();
    switch (s_phase) {
    case Phase::IDLE:
        publish(now);    // 측정 사이(버스가 빈 때) — 직전 주기의 결과를 보낸다
        if ((uint32_t)(now - s_lastCycle) < AIR_CYCLE_MS) return;
        s_lastCycle = now;
        if (Guard::sensorsOn() != s_railWasOn) {
            s_railWasOn = Guard::sensorsOn();
            if (s_railWasOn) railOn(now);
            else             railOff();
        }
        if (s_railWasOn) {
            bmpCycle(now);   // 연속 측정 중이라 결과만 읽는다 — SHT41 변환을 기다리는 동안 버스가 비어 있다
            scdCycle(now);   // BMP280 다음 — 방금 읽은 기압으로 보정
            if (!s_sgpFound) sgpProbe(now);
        }
        if (shtStart()) {
            s_cmdAt = now;
            s_phase = Phase::CONVERTING;
        } else {
            s_shtNack++;
            s_shtOk = false;
            Guard::update(now, false, NAN);
            if (s_watch) printValues(Serial);
        }
        break;
    case Phase::CONVERTING:
        if ((uint32_t)(now - s_cmdAt) < SHT41_CONV_MS) return;
        shtFinish();
        Guard::update(now, s_shtOk, s_tempC);   // 레일 전이는 다음 주기 IDLE 에서 센서 쪽에 반영
        if (s_sgpFound && Guard::sensorsOn() && sgpStart()) {
            s_cmdAt = now;
            s_phase = Phase::SGP_CONVERTING;
            return;
        }
        s_phase = Phase::IDLE;
        if (s_watch) printValues(Serial);
        break;
    case Phase::SGP_CONVERTING:
        if ((uint32_t)(now - s_cmdAt) < SGP_CONV_MS) return;
        sgpFinish(now);
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
        io.printf("  SCD41        : %s\n", !s_scdFound ? "없음" : !s_scdRunning ? "멈춤 (주차 듀티)" : "첫 측정 대기");
    io.printf("  SCD41 stats  : ok %lu, nack %lu, crc %lu, 듀티 멈춤 %lu\n", (unsigned long)s_scdReads,
              (unsigned long)s_scdNack, (unsigned long)s_scdCrc, (unsigned long)s_scdDutyStops);
    if (s_scdPresSent) io.printf("  SCD41 기압   : %uhPa 적용\n", (unsigned)s_scdPresSent);
    else               io.println("  SCD41 기압   : 미적용(BMP280 값 대기) — 해발 0m 기본값으로 계산");
    if (s_sgpOk)
        io.printf("  SGP41        : VOC %ld NOx %ld (raw %u/%u, %lus 전)\n", (long)s_vocIndex, (long)s_noxIndex,
                  (unsigned)s_srawVoc, (unsigned)s_srawNox, (unsigned long)((millis() - s_sgpAt) / 1000));
    else
        io.printf("  SGP41        : %s\n", !s_sgpFound ? "없음" : s_sgpCond ? "컨디셔닝 중" : "읽기 실패");
    io.printf("  SGP41 stats  : ok %lu, nack %lu, crc %lu, 보상 %s\n", (unsigned long)s_sgpReads,
              (unsigned long)s_sgpNack, (unsigned long)s_sgpCrc, s_shtOk ? "SHT41" : "기본값(25°C·50%)");
    KpAir::Values v;
    collect(v, millis());
    char json[KPAIR_JSON_MAX];
    io.printf("  AIR 발행     : %lus 마다, 보냄 %lu, 못 보냄 %lu (링크 다운·값 없음)\n",
              (unsigned long)(AIR_PUB_MS / 1000), (unsigned long)s_pubSent, (unsigned long)s_pubSkip);
    io.printf("  AIR 내용     : %s\n", KpAir::formatJson(v, json, sizeof(json)) ? json : "(값 없음)");
}

} // namespace Air
