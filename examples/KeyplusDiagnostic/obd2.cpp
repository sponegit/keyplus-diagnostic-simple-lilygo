/**
 * @file      obd2.cpp
 * @brief     OBD2/CAN 실시간 PID 구현 — ESP32 TWAI(내장 CAN) + SN65HVD230 트랜시버.
 */
#include "obd2.h"
#include "config.h"
#include "log.h"
#include "console.h"
#include "driver/twai.h"
#include <string.h>

namespace Obd2 {

static bool     s_installed = false;
static uint32_t s_supported = 0;   // PID 0x00 지원 비트마스크(0x01~0x20)
// 응답 ECU 고정(0 = 아직 미확정). 기능 요청(0x7DF)에는 여러 모듈이 각자 답한다 —
// 엔진(0x7E8) 외 모듈이 같은 PID 에 FF(데이터 없음)로 답하거나 자기 기준의 지원
// 마스크를 주는데, 먼저 도착한 프레임을 채택하면 폴마다 값이 뒤바뀐다.
// 실측(0.2.8): 부팅마다 지원마스크가 0x80100001/0x983BA013/0x98188003 로 달라지고
// rpm 이 16384(=0xFFFF/4)로 70초간 발행됐다. 링크 확립 때 정한 ECU 만 채택한다.
static uint32_t s_ecuId     = 0;
static int      s_bitrate   = 0;   // 확립된 비트레이트(kbps)
static int      s_lastGood  = 0;   // 마지막으로 링크가 잡힌 비트레이트(재시도 시 우선 시도)
static char     s_vin[18]   = {0}; // VIN 캐시(차량당 불변, 링크 확립 시 1회 조회)
static bool     s_hasVin    = false;
static int      s_vinTries  = 0;   // VIN 재시도 횟수(미지원 차량 무한요청 방지)
static int      s_vinSkip   = 0;   // VIN 재시도 페이싱 — 남은 스킵 폴 수

// 폴링 대상 PID 표 — 요청 순서와 디코드에 필요한 최소 응답 바이트 수.
// 0x20 초과 PID는 지원 마스크(0x00 응답, 0x01~0x20)로 거를 수 없어 무조건 요청하게 되므로
// 아래 s_extMiss 래치로 미지원 차량에서의 반복 타임아웃을 막는다.
struct PidSpec { uint8_t pid; uint8_t minLen; };
static const PidSpec kPolls[] = {
    { 0x0C, 2 },  // RPM
    { 0x0D, 1 },  // 차속
    { 0x05, 1 },  // 냉각수온
    { 0x04, 1 },  // 엔진부하
    { 0x11, 1 },  // 스로틀
    { 0x0F, 1 },  // 흡기온
    { 0x10, 2 },  // MAF
    { 0x1F, 2 },  // 시동후 경과시간
    { 0x2F, 1 },  // 연료잔량      (>0x20 — 래치 대상)
    { 0x42, 2 },  // 제어모듈 전압 (>0x20 — 래치 대상)
    { 0xA6, 4 },  // 총 주행거리   (>0x20 — 래치 대상, 지원 드묾)
};
static const int kPollCount = (int)(sizeof(kPolls) / sizeof(kPolls[0]));

// 확장 PID(>0x20) 연속 무응답 횟수. 상한에 닿으면 링크 재확립(begin) 전까지 요청하지 않는다.
static uint8_t s_extMiss[kPollCount] = {0};

// 해당 슬롯이 래치 대상(>0x20)인가.
static bool isExtPid(uint8_t pid) { return pid > 0x20; }

// 원시 CAN 프레임 헥스 덤프(디버그, OBD2_DUMP_RAW). 요청/응답을 [CAN] TX/RX로 출력.
static void dumpFrame(const char *dir, const twai_message_t &m)
{
#if OBD2_DUMP_RAW
    if (!LOG_ON(Log::L_DEBUG)) return;
    DbgConsole.printf("[CAN] %s %03X [%d]", dir, (unsigned)m.identifier, m.data_length_code);
    for (int i = 0; i < m.data_length_code; i++) DbgConsole.printf(" %02X", m.data[i]);
    DbgConsole.println();
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
        LOGE(log, "[OBD2] driver_install @%dk 실패\n", kbps);
        return false;
    }
    if (twai_start() != ESP_OK) {
        LOGE(log, "[OBD2] twai_start 실패\n");
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
    s_ecuId  = 0;       // ECU 고정 해제 — 재확립 때 다시 정한다
    s_vinSkip = 0;
    for (int i = 0; i < kPollCount; i++) s_extMiss[i] = 0;   // 확장 PID 래치 해제
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
        // 폴링과 같은 이유로 고정 ECU 응답만 본다 — 타 모듈의 멀티프레임이 섞이면
        // Flow Control 을 엉뚱한 주소로 보내고 재조립이 깨진다.
        if (s_ecuId && m.identifier != s_ecuId) continue;
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
        LOGI(log, "[OBD2] VIN=%s\n", s_vin);
        return true;
    }
    LOGD(log, "[OBD2] VIN 조회 실패 (FF=%d got=%d/%d) — 재시도\n", haveFF ? 1 : 0, got, total);
    return false;
}

// 응답 데이터가 전부 0xFF 인가. OBD2 에서 FF 채움은 "데이터 없음"의 관용 표현이고,
// 그대로 디코드하면 rpm 16384(=0xFFFF/4)처럼 물리적으로 불가능한 값이 telemetry 로 나간다.
static bool allFf(const uint8_t *d, int n)
{
    if (n <= 0) return false;
    for (int i = 0; i < n; i++) if (d[i] != 0xFF) return false;
    return true;
}

// FF 채움을 "데이터 없음"으로 버려도 되는 PID 인가.
// 1바이트 백분율 PID(부하/스로틀/연료잔량)는 0xFF = 100% 가 실제로 가능한 값이라
// (전개 가속·만탱크) 버리면 정상 샘플을 잃는다. 그 외는 FF 가 물리적으로 불가능하다
// — rpm 16383.75, MAF 655.35g/s, 전압 65.5V, 수온 215°C, 차속 255km/h.
static bool ffMeansNoData(uint8_t pid, int n)
{
    if (n >= 2) return true;                                   // 다바이트는 예외 없음
    return !(pid == 0x04 || pid == 0x11 || pid == 0x2F);        // 백분율 3종만 예외
}

// PID 1건 요청 → 매칭 응답의 데이터 바이트(A,B,..)를 resp에 채우고 개수 반환. 무응답 -1.
//   요청 0x7DF [02,01,PID], 응답 0x7E8~0x7EF [len,0x41,PID,A,B,..].
// s_ecuId 가 정해져 있으면 그 ECU 의 응답만 채택한다(타 모듈의 FF 응답/다른 지원마스크 배제).
// responderOut 이 주어지면 채택한 프레임의 응답 ID 를 넘긴다(링크 확립 때 ECU 고정용).
static int requestPid(uint8_t pid, uint8_t *resp, int cap, uint32_t timeoutMs,
                      uint32_t *responderOut = nullptr, bool preferEngine = false)
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

    // preferEngine(링크 확립 시): 창이 끝날 때까지 모아보고 엔진(0x7E8)을 우선 채택한다.
    // 평상시(ECU 고정 후): 첫 매칭 프레임을 바로 채택한다.
    int      bestCount = -1;
    uint32_t bestId    = 0;
    uint8_t  best[8]   = {0};

    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        uint32_t rem = timeoutMs - (millis() - start);
        if (twai_receive(&m, pdMS_TO_TICKS(rem)) != ESP_OK) break;   // 타임아웃
        if (m.extd || m.identifier < 0x7E8 || m.identifier > 0x7EF) continue;
        if (m.data_length_code < 3 || m.data[1] != 0x41 || m.data[2] != pid) continue;
        // ECU 가 고정돼 있으면 그 ECU 만. 타 모듈 응답은 값이 다르거나 FF 다.
        if (s_ecuId && m.identifier != s_ecuId) continue;

        dumpFrame("RX", m);
        int count = (int)m.data[0] - 2;              // len − (mode+pid)
        if (count < 0) count = 0;
        if (count > m.data_length_code - 3) count = m.data_length_code - 3;
        if (count > cap) count = cap;
        if (count > (int)sizeof(best)) count = sizeof(best);

        if (bestCount < 0 || (preferEngine && m.identifier == 0x7E8)) {
            bestCount = count;
            bestId    = m.identifier;
            for (int i = 0; i < count; i++) best[i] = m.data[3 + i];
        }
        // 고정 ECU 응답이거나 엔진 응답을 잡았으면 더 기다릴 이유가 없다.
        if (!preferEngine || m.identifier == 0x7E8) break;
    }

    if (bestCount < 0) return -1;
    // FF 채움 = 데이터 없음. 확장 PID 라면 미지원 신호이므로 무응답과 동일하게 취급한다.
    if (allFf(best, bestCount) && ffMeansNoData(pid, bestCount)) return -1;

    for (int i = 0; i < bestCount; i++) resp[i] = best[i];
    if (responderOut) *responderOut = bestId;
    return bestCount;
}

bool begin(Stream &log)
{
    end();
    // 기본은 최신 차량(2008+) 표준 500k 우선, 무응답 시 250k 폴백.
    // 단 한 번이라도 링크가 잡혔던 비트레이트가 있으면 그것부터 시도한다 — 250k 차량에서
    // 주차 중 재시도할 때마다 500k 무응답 타임아웃을 먼저 먹는 것을 피한다.
    const int rates[2] = { s_lastGood == 250 ? 250 : 500,
                           s_lastGood == 250 ? 500 : 250 };
    for (int i = 0; i < 2; i++) {
        if (!install(rates[i], log)) continue;
        // 링크 확인 겸 지원 PID 조회(0x00). 초기엔 여유 있게 대기.
        // 여러 모듈이 답하므로 창이 끝날 때까지 모아 엔진(0x7E8)을 우선 채택하고,
        // 채택한 ECU 를 이후 폴링에 고정한다 — 이걸 안 하면 지원마스크와 값이 폴마다 섞인다.
        uint8_t r[4];
        uint32_t responder = 0;
        int n = requestPid(0x00, r, 4, OBD2_REQ_TIMEOUT_MS * 3, &responder, true);
        if (n >= 4) {
            s_supported = ((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16) |
                          ((uint32_t)r[2] << 8) | r[3];
            s_bitrate = rates[i];
            s_lastGood = rates[i];   // 다음 재시도에서 이 속도부터
            s_ecuId    = responder;  // 이후 이 ECU 응답만 채택
            LOGI(log, "[OBD2] 링크 확립 @%dkbps ECU=%03X 지원PID(01-20)=0x%08X\n",
                 s_bitrate, (unsigned)s_ecuId, s_supported);
            if (s_ecuId != 0x7E8) {
                // 엔진 ECU 가 답하지 않았다 — rpm/차속 같은 항목이 없을 수 있다.
                LOGW(log, "[OBD2] 엔진 ECU(7E8)가 응답하지 않음 — 항목이 제한될 수 있다\n");
            }
            if (!s_hasVin) readVin(log);   // VIN 1회 조회(ISO-TP), 캐시
            return true;
        }
        LOGD(log, "[OBD2] @%dkbps 무응답 — 폴백\n", rates[i]);
        end();
    }
    LOGW(log, "[OBD2] 링크 미확립 (트랜시버 배선/차량 연결/시동 확인)\n");
    return false;
}

bool isInstalled() { return s_installed; }

void linkInfo(LinkInfo &out)
{
    out.installed    = s_installed;
    out.bitrate      = s_bitrate;
    out.lastGoodRate = s_lastGood;
    out.supportedPid = s_supported;
    out.ecuId        = s_ecuId;
    out.hasVin       = s_hasVin;
    out.vin          = s_hasVin ? s_vin : "";
    out.vinTries     = s_vinTries;
    out.extLatched   = 0;
    out.extTotal     = 0;
    for (int i = 0; i < kPollCount; i++) {
        if (!isExtPid(kPolls[i].pid)) continue;
        out.extTotal++;
        if (s_extMiss[i] >= OBD2_EXT_PID_MISS_LIMIT) out.extLatched++;
    }
}

bool read(Data &out, Stream &log)
{
    out = Data();
    if (!s_installed) return false;
    out.supportedPid = s_supported;

    uint8_t r[6];
    int  consecMiss = 0;     // 연속 무응답(요청을 실제로 보낸 것만 카운트)
    bool anyAnswer  = false; // 이번 폴에서 ECU가 한 번이라도 응답 = 링크 살아있음

    for (int i = 0; i < kPollCount; i++) {
        const uint8_t pid    = kPolls[i].pid;
        const uint8_t minLen = kPolls[i].minLen;

        // ≤0x20 PID는 지원 마스크로 걸러 불필요한 타임아웃을 줄인다.
        if (!supported(pid)) continue;
        // >0x20 PID 중 미지원 확정된 것은 요청 자체를 생략(래치). 0ms.
        if (isExtPid(pid) && s_extMiss[i] >= OBD2_EXT_PID_MISS_LIMIT) continue;

        int n = requestPid(pid, r, 6, OBD2_REQ_TIMEOUT_MS);

        if (n < 0) {
            // 무응답 — 타임아웃을 통째로 소모했다.
            consecMiss++;
            // ⚠️ 래치는 "이번 폴에서 고정 ECU 가 다른 PID 에 답하고 있다"는 전제에서만
            //    센다. 시동 직후/버스 폭주 구간의 무응답으로 래치하면 지원 PID 를
            //    영구 포기한다(실측: 0xA6 지원 차량에서 미지원 오래치).
            if (isExtPid(pid) && anyAnswer && s_extMiss[i] < OBD2_EXT_PID_MISS_LIMIT) {
                if (++s_extMiss[i] >= OBD2_EXT_PID_MISS_LIMIT) {
                    LOGD(log, "[OBD2] PID 0x%02X 미지원 확정 — 링크 재확립까지 요청 생략\n", pid);
                }
            }
            // 링크가 끊기면(시동 OFF 등) 남은 PID도 전부 타임아웃이라 폴 하나가
            // 최대 11 × OBD2_REQ_TIMEOUT_MS 동안 loop 를 멈춘다. 초반 연속 무응답이면
            // 링크 끊김으로 보고 즉시 중단 → 호출측이 재확립 경로를 탄다.
            if (consecMiss >= OBD2_POLL_ABORT_MISSES && !anyAnswer) break;
            continue;
        }

        // 응답은 왔다(= 링크 정상). 길이가 모자라면 디코드만 건너뛴다.
        anyAnswer  = true;
        consecMiss = 0;
        if (isExtPid(pid)) s_extMiss[i] = 0;
        if (n < minLen) continue;

        switch (pid) {
        case 0x0C: out.has_rpm = true;      out.rpm = ((256 * r[0]) + r[1]) / 4.0f;      break;
        case 0x0D: out.has_speed = true;    out.speed = r[0];                            break;
        case 0x05: out.has_coolant = true;  out.coolant = (int)r[0] - 40;                break;
        case 0x04: out.has_load = true;     out.load = r[0] * 100.0f / 255.0f;           break;
        case 0x11: out.has_throttle = true; out.throttle = r[0] * 100.0f / 255.0f;       break;
        case 0x0F: out.has_intake = true;   out.intake = (int)r[0] - 40;                 break;
        case 0x10: out.has_maf = true;      out.maf = ((256 * r[0]) + r[1]) / 100.0f;    break;
        case 0x1F: out.has_runtime = true;  out.runtime = (256 * r[0]) + r[1];           break;
        case 0x2F: out.has_fuel = true;     out.fuel = r[0] * 100.0f / 255.0f;           break;
        case 0x42: out.has_ctrlv = true;    out.ctrl_v = ((256 * r[0]) + r[1]) / 1000.0f; break;
        case 0xA6: {   // 총 주행거리(J1979-2, 4바이트, raw/10 km)
            uint32_t raw = ((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16) |
                           ((uint32_t)r[2] << 8) | r[3];
            out.has_odometer = true; out.odometer = raw / 10.0f;
            break;
        }
        default: break;
        }
        out.valid = true;
    }

    // 링크가 죽었으면 VIN 재시도(최대 REQ_TIMEOUT×9)도 낭비이자 추가 스톨 → 응답이 있을 때만.
    // 상한까지만 시도(미지원 차량 무한요청 방지).
    // ⚠️ 페이싱 없이 매 폴 재시도하면 상한이 링크 확립 직후 수십 초 안에 소진된다
    //    (실측: 시동 직후 버스 폭주 구간에서 5회를 다 쓰고 세션 내내 VIN 미확보).
    //    폴 몇 번에 한 번만 시도해 상한을 분 단위로 늘린다.
    if (anyAnswer && !s_hasVin && s_vinTries < OBD2_VIN_TRY_LIMIT) {
        if (s_vinSkip == 0) {
            readVin(log);
            s_vinTries++;
            s_vinSkip = OBD2_VIN_RETRY_POLLS;
        } else {
            s_vinSkip--;
        }
    }

    // VIN(정적)을 매 폴에 실어 telemetry가 참조하게 한다.
    if (s_hasVin) {
        out.has_vin = true;
        strncpy(out.vin, s_vin, sizeof(out.vin) - 1);
        out.vin[sizeof(out.vin) - 1] = 0;
        out.valid = true;
    }

    // 반환값은 "링크 살아있음"이다 — out.valid 가 아니다.
    // VIN 캐시가 있으면 out.valid 는 링크가 죽어도 계속 true 라서, 그것으로 판단하면
    // 호출측이 링크 끊김을 영영 감지하지 못하고 매 폴 타임아웃 스톨만 반복한다.
    return anyAnswer;
}

bool readFast(FastSample &out, Stream &log)
{
    out = FastSample();
    if (!s_installed) return false;

    // 0x0D 차속 / 0x0C 회전수 / 0x11 스로틀. 전부 ≤0x20 이라 지원 마스크로 거를 수 있다.
    static const uint8_t kFastPids[3] = { 0x0D, 0x0C, 0x11 };

    uint8_t r[6];
    bool anyAnswer  = false;
    int  consecMiss = 0;

    for (int i = 0; i < 3; i++) {
        const uint8_t pid = kFastPids[i];
        if (!supported(pid)) continue;

        int n = requestPid(pid, r, 6, OBD2_REQ_TIMEOUT_MS);
        if (n < 0) {
            // 링크가 죽었으면 남은 PID 도 전부 타임아웃이다 — 1Hz 로 도는 경로라
            // 전체 폴보다 더 공격적으로 끊는다(초당 스톨 누적 방지).
            if (++consecMiss >= OBD2_POLL_ABORT_MISSES && !anyAnswer) break;
            continue;
        }
        anyAnswer  = true;
        consecMiss = 0;

        switch (pid) {
        case 0x0D:
            if (n >= 1) { out.has_speed = true; out.speed = r[0]; }
            break;
        case 0x0C:
            if (n >= 2) {
                uint32_t raw = ((uint32_t)r[0] << 8) | r[1];
                out.has_rpm = true; out.rpm = (uint16_t)(raw / 4);
            }
            break;
        case 0x11:
            // %  = A × 100/255. 창 집계·이벤트 판정에 소수점은 필요 없어 정수로 둔다.
            if (n >= 1) { out.has_throttle = true; out.throttle = (uint8_t)((r[0] * 100 + 127) / 255); }
            break;
        default: break;
        }
    }

    // 이 경로는 초당 1회라 성공 로그를 남기면 콘솔이 도배된다 — 창 요약은 'fast' 명령에서 본다.
    if (!anyAnswer) LOGD(log, "[FAST] OBD 무응답 — 링크 확인\n");
    return anyAnswer;
}

void print(const Data &d, Stream &log)
{
    // 폴 1회분 전체 항목 덤프 — DEBUG 전용. 평상시 주요 값은 loop의 [STAT] 한 줄에 실린다.
    if (!LOG_ON(Log::L_DEBUG)) return;

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
    // VIN 은 차량당 불변이라 매 폴 찍으면 DEBUG 로그가 두 배가 된다(실측: 5초마다 한 줄).
    // 값이 바뀔 때(= 최초 확보 / 차량 교체)만 남긴다. 현재값은 'status obd' 에서 본다.
    static char lastVin[sizeof(d.vin)] = {0};
    if (d.has_vin && strncmp(lastVin, d.vin, sizeof(lastVin)) != 0) {
        strncpy(lastVin, d.vin, sizeof(lastVin) - 1);
        lastVin[sizeof(lastVin) - 1] = 0;
        log.printf("[OBD2] VIN=%s\n", d.vin);
    }
}

} // namespace Obd2
