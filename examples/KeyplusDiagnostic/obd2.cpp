/**
 * @file      obd2.cpp
 * @brief     OBD2/CAN 실시간 PID 구현 — ESP32 TWAI(내장 CAN) + SN65HVD230 트랜시버.
 */
#include "obd2.h"
#include "config.h"
#include "driver/twai.h"
#include <string.h>

namespace Obd2 {

static bool     s_installed = false;
static uint32_t s_supported = 0;   // PID 0x00 지원 비트마스크(0x01~0x20)
static int      s_bitrate   = 0;   // 확립된 비트레이트(kbps)
static char     s_vin[18]   = {0}; // VIN 캐시(차량당 불변, 링크 확립 시 1회 조회)
static bool     s_hasVin    = false;
static int      s_vinTries  = 0;   // VIN 재시도 횟수(미지원 차량 무한요청 방지)

// 원시 CAN 프레임 헥스 덤프(디버그, OBD2_DUMP_RAW). 요청/응답을 [CAN] TX/RX로 출력.
static void dumpFrame(const char *dir, const twai_message_t &m)
{
#if OBD2_DUMP_RAW
    Serial.printf("[CAN] %s %03X [%d]", dir, (unsigned)m.identifier, m.data_length_code);
    for (int i = 0; i < m.data_length_code; i++) Serial.printf(" %02X", m.data[i]);
    Serial.println();
#else
    (void)dir; (void)m;
#endif
}

// PID N(1~32)이 지원 마스크에 있는가. 0x20 초과 PID는 별도 마스크가 필요하므로 항상 시도(1).
static bool supported(uint8_t pid)
{
    if (pid == 0 || pid > 0x20) return true;
    return (s_supported & (1u << (32 - pid))) != 0;
}

// TWAI 설치·시작(NORMAL 모드 — 요청 송신 필요). 성공 시 s_installed=true.
static bool install(int kbps, Stream &log)
{
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
        (gpio_num_t)PIN_CAN_TX, (gpio_num_t)PIN_CAN_RX, TWAI_MODE_NORMAL);
    // 실차 버스는 프레임이 폭주(수백/초) → 기본 RX 큐(5)로는 VIN ISO-TP 다중프레임
    // 재조립 중 큐 오버플로로 CF를 놓친다. 큐를 키워 멀티프레임 수신을 보장한다.
    g.rx_queue_len = 32;
    g.tx_queue_len = 10;
    // 매크로는 브레이스 이니셜라이저라 삼항 피연산자로 못 씀 → 변수에 담아 복사.
    twai_timing_config_t t500 = TWAI_TIMING_CONFIG_500KBITS();
    twai_timing_config_t t250 = TWAI_TIMING_CONFIG_250KBITS();
    twai_timing_config_t t = (kbps == 250) ? t250 : t500;
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();  // ID 필터는 소프트웨어로
    if (twai_driver_install(&g, &t, &f) != ESP_OK) {
        log.printf("[OBD2] driver_install @%dk 실패\n", kbps);
        return false;
    }
    if (twai_start() != ESP_OK) {
        log.println("[OBD2] twai_start 실패");
        twai_driver_uninstall();
        return false;
    }
    s_installed = true;
    return true;
}

void end()
{
    if (s_installed) {
        twai_stop();
        twai_driver_uninstall();
        s_installed = false;
    }
    s_hasVin = false;   // 재확립 시 VIN 재조회(차량 교체 대비)
    s_vinTries = 0;
}

// VIN(Mode 09 PID 02) — ISO-TP 다중 프레임 재조립. 성공 시 s_vin/s_hasVin 세팅.
//   요청 0x7DF[02,09,02] → 응답 First Frame(0x1L LL ..) → Flow Control(0x30) 송신 →
//   Consecutive Frame(0x2N ..) 누적. 페이로드 = [49 02 01 <VIN 17>].
static bool readVin(Stream &log)
{
    if (!s_installed) return false;
    twai_message_t m;
    while (twai_receive(&m, 0) == ESP_OK) { /* drain */ }

    twai_message_t tx = {};
    tx.identifier = 0x7DF;
    tx.data_length_code = 8;
    tx.data[0] = 0x02; tx.data[1] = 0x09; tx.data[2] = 0x02;   // Mode 09, PID 02
    if (twai_transmit(&tx, pdMS_TO_TICKS(OBD2_REQ_TIMEOUT_MS)) != ESP_OK) return false;
    dumpFrame("TX", tx);

    uint8_t  payload[64];
    int      total = 0, got = 0;
    uint32_t responderId = 0;
    bool     haveFF = false;

    // First Frame(또는 드물게 Single Frame) 대기. VIN 헤더(49 02)로 검증 →
    // 폴링 중 섞이는 스테일 PID 응답(0x00 41 ..)이나 타 멀티프레임을 배제한다.
    uint32_t start = millis();
    while (millis() - start < (uint32_t)OBD2_REQ_TIMEOUT_MS * 4) {
        uint32_t rem = (uint32_t)OBD2_REQ_TIMEOUT_MS * 4 - (millis() - start);
        if (twai_receive(&m, pdMS_TO_TICKS(rem)) != ESP_OK) break;
        if (m.extd || m.identifier < 0x7E8 || m.identifier > 0x7EF) continue;
        uint8_t pci = m.data[0] & 0xF0;
        if (pci == 0x00 && (m.data[0] & 0x0F) >= 2 &&
            m.data[1] == 0x49 && m.data[2] == 0x02) {          // VIN Single Frame
            dumpFrame("RX", m);
            int len = m.data[0] & 0x0F;
            for (int i = 0; i < len && got < (int)sizeof(payload); i++) payload[got++] = m.data[1 + i];
            total = got;
            break;
        } else if (pci == 0x10 && m.data[2] == 0x49 && m.data[3] == 0x02) {   // VIN First Frame
            dumpFrame("RX", m);
            total = ((m.data[0] & 0x0F) << 8) | m.data[1];
            if (total > (int)sizeof(payload)) total = sizeof(payload);
            for (int i = 0; i < 6 && got < total; i++) payload[got++] = m.data[2 + i];
            responderId = m.identifier;
            haveFF = true;
            break;
        }
        // 그 외(스테일 PID 응답 / 타 멀티프레임) → 무시하고 계속
    }

    if (haveFF) {
        // Flow Control → 응답 ECU의 물리 주소(응답ID−8). CTS, BS=0, STmin=0.
        twai_message_t fc = {};
        fc.identifier = 0x7E0 + (responderId - 0x7E8);
        fc.data_length_code = 8;
        fc.data[0] = 0x30;
        twai_transmit(&fc, pdMS_TO_TICKS(OBD2_REQ_TIMEOUT_MS));
        dumpFrame("TX", fc);

        start = millis();
        while (got < total && millis() - start < (uint32_t)OBD2_REQ_TIMEOUT_MS * 5) {
            uint32_t rem = (uint32_t)OBD2_REQ_TIMEOUT_MS * 5 - (millis() - start);
            if (twai_receive(&m, pdMS_TO_TICKS(rem)) != ESP_OK) break;
            if (m.extd || m.identifier != responderId) continue;
            if ((m.data[0] & 0xF0) == 0x20) {    // Consecutive Frame
                dumpFrame("RX", m);
                for (int i = 1; i < 8 && got < total; i++) payload[got++] = m.data[i];
            }
        }
    }

    // 페이로드 = 49(=09+40) 02 01 <VIN...>. VIN = payload[3..].
    if (got >= 3 + 17 && payload[0] == 0x49 && payload[1] == 0x02) {
        int vlen = got - 3;
        if (vlen > 17) vlen = 17;
        for (int i = 0; i < vlen; i++) s_vin[i] = (char)payload[3 + i];
        s_vin[vlen] = 0;
        s_hasVin = true;
        log.printf("[OBD2] VIN=%s\n", s_vin);
        return true;
    }
    log.printf("[OBD2] VIN 조회 실패 (FF=%d got=%d/%d) — 재시도\n", haveFF ? 1 : 0, got, total);
    return false;
}

// PID 1건 요청 → 매칭 응답의 데이터 바이트(A,B,..)를 resp에 채우고 개수 반환. 무응답 -1.
//   요청 0x7DF [02,01,PID], 응답 0x7E8~0x7EF [len,0x41,PID,A,B,..].
static int requestPid(uint8_t pid, uint8_t *resp, int cap, uint32_t timeoutMs)
{
    if (!s_installed) return -1;

    // 직전 폴/타 트래픽의 잔여 RX 비우기(오탐/큐오버플로 완화).
    twai_message_t m;
    while (twai_receive(&m, 0) == ESP_OK) { /* discard */ }

    twai_message_t tx = {};             // 0-init → 표준 프레임(11-bit), non-RTR
    tx.identifier = 0x7DF;              // 기능 브로드캐스트
    tx.data_length_code = 8;
    tx.data[0] = 0x02;                  // 이후 유효 바이트 수(mode+pid)
    tx.data[1] = 0x01;                  // Mode 01
    tx.data[2] = pid;
    if (twai_transmit(&tx, pdMS_TO_TICKS(timeoutMs)) != ESP_OK) return -1;
    dumpFrame("TX", tx);

    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        uint32_t rem = timeoutMs - (millis() - start);
        if (twai_receive(&m, pdMS_TO_TICKS(rem)) != ESP_OK) break;   // 타임아웃
        if (!m.extd && m.identifier >= 0x7E8 && m.identifier <= 0x7EF &&
            m.data_length_code >= 3 && m.data[1] == 0x41 && m.data[2] == pid) {
            dumpFrame("RX", m);
            int count = (int)m.data[0] - 2;              // len − (mode+pid)
            if (count < 0) count = 0;
            if (count > m.data_length_code - 3) count = m.data_length_code - 3;
            if (count > cap) count = cap;
            for (int i = 0; i < count; i++) resp[i] = m.data[3 + i];
            return count;
        }
        // 그 외 CAN 트래픽 → 무시하고 계속 대기
    }
    return -1;
}

bool begin(Stream &log)
{
    end();
    const int rates[] = { 500, 250 };   // 최신 차량 우선, 무응답 시 폴백
    for (int i = 0; i < 2; i++) {
        if (!install(rates[i], log)) continue;
        // 링크 확인 겸 지원 PID 조회(0x00). 초기엔 여유 있게 대기.
        uint8_t r[4];
        int n = requestPid(0x00, r, 4, OBD2_REQ_TIMEOUT_MS * 3);
        if (n >= 4) {
            s_supported = ((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16) |
                          ((uint32_t)r[2] << 8) | r[3];
            s_bitrate = rates[i];
            log.printf("[OBD2] ✅ 링크 @%dkbps, 지원PID(01-20)=0x%08X\n",
                       s_bitrate, s_supported);
            if (!s_hasVin) readVin(log);   // VIN 1회 조회(ISO-TP), 캐시
            return true;
        }
        log.printf("[OBD2] @%dkbps 무응답 — 폴백\n", rates[i]);
        end();
    }
    log.println("[OBD2] ❌ 링크 미확립 (트랜시버 배선/차량 연결/시동 확인)");
    return false;
}

bool isInstalled() { return s_installed; }

bool read(Data &out, Stream &log)
{
    out = Data();
    if (!s_installed) return false;
    out.supportedPid = s_supported;

    // VIN 미획득이면 폴 때 재시도(begin 실패 복구). 상한까지만 시도(미지원 차량 무한요청 방지).
    if (!s_hasVin && s_vinTries < 5) { readVin(log); s_vinTries++; }
    // VIN(정적)을 매 폴에 실어 telemetry가 참조하게 한다.
    if (s_hasVin) {
        out.has_vin = true;
        strncpy(out.vin, s_vin, sizeof(out.vin) - 1);
        out.vin[sizeof(out.vin) - 1] = 0;
        out.valid = true;
    }

    uint8_t r[6];
    int n;

    // ≤0x20 PID는 지원 마스크로 걸러 불필요한 타임아웃을 줄인다.
    if (supported(0x0C) && (n = requestPid(0x0C, r, 6, OBD2_REQ_TIMEOUT_MS)) >= 2) {
        out.has_rpm = true; out.rpm = ((256 * r[0]) + r[1]) / 4.0f; out.valid = true;
    }
    if (supported(0x0D) && (n = requestPid(0x0D, r, 6, OBD2_REQ_TIMEOUT_MS)) >= 1) {
        out.has_speed = true; out.speed = r[0]; out.valid = true;
    }
    if (supported(0x05) && (n = requestPid(0x05, r, 6, OBD2_REQ_TIMEOUT_MS)) >= 1) {
        out.has_coolant = true; out.coolant = (int)r[0] - 40; out.valid = true;
    }
    if (supported(0x04) && (n = requestPid(0x04, r, 6, OBD2_REQ_TIMEOUT_MS)) >= 1) {
        out.has_load = true; out.load = r[0] * 100.0f / 255.0f; out.valid = true;
    }
    if (supported(0x11) && (n = requestPid(0x11, r, 6, OBD2_REQ_TIMEOUT_MS)) >= 1) {
        out.has_throttle = true; out.throttle = r[0] * 100.0f / 255.0f; out.valid = true;
    }
    if (supported(0x0F) && (n = requestPid(0x0F, r, 6, OBD2_REQ_TIMEOUT_MS)) >= 1) {
        out.has_intake = true; out.intake = (int)r[0] - 40; out.valid = true;
    }
    if (supported(0x10) && (n = requestPid(0x10, r, 6, OBD2_REQ_TIMEOUT_MS)) >= 2) {
        out.has_maf = true; out.maf = ((256 * r[0]) + r[1]) / 100.0f; out.valid = true;
    }
    if (supported(0x1F) && (n = requestPid(0x1F, r, 6, OBD2_REQ_TIMEOUT_MS)) >= 2) {
        out.has_runtime = true; out.runtime = (256 * r[0]) + r[1]; out.valid = true;
    }
    // 0x20 초과 — 마스크 대상 아님, 그냥 시도.
    if ((n = requestPid(0x2F, r, 6, OBD2_REQ_TIMEOUT_MS)) >= 1) {
        out.has_fuel = true; out.fuel = r[0] * 100.0f / 255.0f; out.valid = true;
    }
    if ((n = requestPid(0x42, r, 6, OBD2_REQ_TIMEOUT_MS)) >= 2) {
        out.has_ctrlv = true; out.ctrl_v = ((256 * r[0]) + r[1]) / 1000.0f; out.valid = true;
    }
    // 0xA6 총 주행거리(J1979-2, 4바이트, raw/10 km). 지원 드묾 → 항상 시도, 미지원은 자동 생략.
    if ((n = requestPid(0xA6, r, 6, OBD2_REQ_TIMEOUT_MS)) >= 4) {
        uint32_t raw = ((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16) |
                       ((uint32_t)r[2] << 8) | r[3];
        out.has_odometer = true; out.odometer = raw / 10.0f; out.valid = true;
    }
    (void)log;
    return out.valid;
}

void print(const Data &d, Stream &log)
{
    if (!d.valid) { log.println("[OBD2] (링크 없음/무응답)"); return; }
    log.print("[OBD2]");
    if (d.has_rpm)      log.printf(" rpm=%.0f", d.rpm);
    if (d.has_speed)    log.printf(" spd=%d", d.speed);
    if (d.has_coolant)  log.printf(" cool=%dC", d.coolant);
    if (d.has_load)     log.printf(" load=%.0f%%", d.load);
    if (d.has_throttle) log.printf(" thr=%.0f%%", d.throttle);
    if (d.has_intake)   log.printf(" intake=%dC", d.intake);
    if (d.has_maf)      log.printf(" maf=%.1fg/s", d.maf);
    if (d.has_fuel)     log.printf(" fuel=%.0f%%", d.fuel);
    if (d.has_ctrlv)    log.printf(" batt=%.2fV", d.ctrl_v);
    if (d.has_runtime)  log.printf(" run=%us", d.runtime);
    if (d.has_odometer) log.printf(" odo=%.1fkm", d.odometer);
    log.println();
    if (d.has_vin)      log.printf("[OBD2] VIN=%s\n", d.vin);
}

} // namespace Obd2
