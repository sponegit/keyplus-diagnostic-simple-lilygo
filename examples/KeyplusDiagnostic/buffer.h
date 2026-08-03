/**
 * @file      buffer.h
 * @brief     오프라인 telemetry 적재 — `spiffs` 파티션을 raw 링버퍼로 전용
 *
 * 통신 음영(지하주차장·터널·기지국 장애) 구간의 telemetry 를 단말 플래시에 적재해 두었다가,
 * 재접속 후 실시간 발행을 방해하지 않는 속도로 뒤늦게 올린다(백필).
 *
 * ⚠️ **파일시스템이 아니라 raw 다.** `spiffs` 파티션을 SPIFFS 로 마운트하지 않고
 *    esp_partition_* 로 직접 읽고 쓴다:
 *      - SPIFFS 는 128KB 에서 메타데이터·GC 로 실사용이 ~100KB 로 줄고 GC 가 수백 ms 스톨을 낸다.
 *      - 차량 상시전원은 크랭킹·브라운아웃으로 **쓰기 도중 전원이 끊긴다**. SPIFFS 는 이때
 *        파일시스템 전체가 손상돼 마운트 실패로 갈 수 있으나, raw 링은 손상 섹터 하나만 버리면 된다.
 *    → 이 결정 이후 SPIFFS/LittleFS 를 쓰면 파티션이 충돌한다. 파일시스템이 필요해지면
 *      파티션 표 재설계와 함께 다뤄야 한다(현재 이 펌웨어는 파일시스템을 전혀 쓰지 않는다).
 *
 * ⚠️ **OTA 는 파티션 표를 바꿀 수 없다.** 현장 단말에서 확보 가능한 공간은 현재 파티션 표가
 *    정한 128KB 가 상한이다. 그래도 섹터 수를 컴파일 상수로 박지 않고 `part->size` 에서
 *    매 부팅 계산한다 — 차후 파티션을 재설계해도 **같은 펌웨어가 두 종류 단말을 모두 지원**한다.
 *
 * 용량: 56B × 73레코드/섹터 × 32섹터 = 2,336건
 *       → 주행(30초 주기) 19.5시간 / 정차(120초 주기) 77.9시간(3.2일)
 *
 * 계약(SSOT): packages/shared/src/protocol/telemetry.ts — 백필은 기존 telemetry 스키마
 *   그대로 발행하고 `sys.bf=1` + 과거 `ts` 만 다르다(**서버 변경 없음**).
 * 설계: offline-telemetry-buffer.md §2, §3, §5
 */
#pragma once

#include <Arduino.h>
#include "gps.h"
#include "obd2.h"
#include "fast.h"

namespace Buf {

/**
 * 오프라인 레코드 — 56바이트 고정, 리틀엔디언.
 * 넓은 형부터 배치해 모든 필드가 자연 정렬된다(u32는 4정렬, u16은 2정렬).
 * esp_partition_write 도 4바이트 정렬을 요구하므로 섹터 헤더 8B + 56B×n 은 항상 4정렬이다.
 *
 * **VIN 은 넣지 않는다** — 17자를 넣으면 레코드가 73B 로 커져 보관량이 23% 줄어든다.
 * VIN 은 차량마다 불변이고 실시간 telemetry 로 이미 서버에 저장되어 있다.
 */
struct Record {
    uint32_t ts;            //  0  최선의 epoch(초). 0 = 미상(업로드 시 소급 복원)
    uint32_t up_s;          //  4  부팅 후 경과 초 (시각 소급 복원용)
    int32_t  lat_e6;        //  8  위도 × 1e6 (약 0.11m — GPS 실측 정확도보다 세밀)
    int32_t  lon_e6;        // 12  경도 × 1e6
    uint32_t odo_x10;       // 16  총 주행거리 km × 10
    uint16_t boot;          // 20  부팅 카운터(NVS) — 시각 소급 복원 가능 여부 판정
    uint16_t seq;           // 22  캡처 당시 g_seq 하위 16비트
    uint16_t spd_x10;       // 24  GPS 속도 × 10 (km/h)
    uint16_t obd_flags;     // 26  has_* 11비트 (아래 ObdFlag)
    uint16_t rpm;           // 28  엔진 회전수
    uint16_t maf_x10;       // 30  g/s × 10
    uint16_t ctrl_mv;       // 32  제어모듈 전압 mV
    uint16_t runtime_s;     // 34  시동 후 경과 초
    uint8_t  sat;           // 36  가시 위성 수
    int8_t   rssi;          // 37  getSignalQuality()
    uint8_t  reg;           // 38  등록 상태
    uint8_t  flags;         // 39  아래 Flag 비트
    uint8_t  obd_speed;     // 40  차속 km/h
    int8_t   coolant;       // 41  냉각수온 °C
    uint8_t  load_x2;       // 42  엔진부하 % × 2 (0.5% 해상도)
    uint8_t  throttle_x2;   // 43  스로틀 % × 2
    int8_t   intake;        // 44  흡기온 °C
    uint8_t  fuel_x2;       // 45  연료 잔량 % × 2
    uint8_t  agg_spd_max;   // 46  ── 이하 고빈도 창 집계 8B (Fast::Aggregate)
    uint8_t  agg_spd_min;   // 47
    uint8_t  agg_rpm_max_x100; // 48
    int8_t   agg_accel_x10; // 49
    int8_t   agg_decel_x10; // 50
    uint8_t  agg_ev;        // 51  b0 급가속 b1 급감속 b2 과속 b3 공회전
    uint8_t  agg_ev_n;      // 52
    uint8_t  agg_n;         // 53  창에서 실제 수집된 1Hz 샘플 수(0 = agg 생략)
    uint16_t crc16;         // 54  앞 54B CRC-16/CCITT
};
static_assert(sizeof(Record) == 56, "오프라인 레코드는 56B 고정이어야 한다(패딩 발생)");

// Record.flags 비트
enum Flag : uint8_t {
    F_TSRC_MASK  = 0x03,   // b0-1 시각 출처 (Clk::Src — 0 없음 / 1 파생 / 2 모뎀 / 3 GPS)
    F_GPS_FIX    = 0x04,   // b2 적재 시점 측위 중
    F_OBD_VALID  = 0x08,   // b3 OBD 링크 유효
    F_PARKED_WIN = 0x10,   // b4 정차 창(1 = 120초 / 0 = 주행 30초) — agg.w 산출용
};

// Record.obd_flags 비트 — Obd2::Data 의 has_* 를 그대로 옮긴다.
enum ObdFlag : uint16_t {
    O_RPM = 1 << 0,  O_SPEED = 1 << 1,  O_COOLANT  = 1 << 2, O_LOAD = 1 << 3,
    O_THR = 1 << 4,  O_INTAKE = 1 << 5, O_MAF      = 1 << 6, O_FUEL = 1 << 7,
    O_CTRLV = 1 << 8, O_RUNTIME = 1 << 9, O_ODOMETER = 1 << 10,
};

// 파티션 탐색 + 링 상태 복원(NVS 불필요 — 섹터 헤더 스캔). setup 에서 1회.
// 파티션이 없으면 false(이후 모든 호출이 무해하게 no-op).
bool begin(Stream &log);

bool available();          // begin 성공 여부

/**
 * 현재 상태를 1건 적재. 오프라인 경로(미접속)에서만 부른다.
 * agg 는 Fast::takeAggregate() 결과 — agg.n==0 이면 백필에서 agg 오브젝트를 생략한다.
 * parkedWindow 는 이 창이 정차(120초) 창이었는지 — flags b4 로 저장돼 서버가 duration_s 를 채운다.
 */
bool pushSample(const GpsFix &fix, bool fixFresh, const Obd2::Data &obd,
                uint32_t seq, const Fast::Aggregate &agg, bool parkedWindow,
                int rssi, int reg, Stream &log);

// 미전송 백로그 건수.
uint32_t count();

// 가장 오래된 미전송 레코드를 읽는다(커서는 움직이지 않는다). 없으면 false.
// CRC 불일치 레코드는 건너뛰며 커서를 전진시킨다(손상 섹터 방어).
bool peek(Record &out, Stream &log);

// peek 한 레코드를 소비(커서 전진). 발행 성공 시에만 부른다.
// 커서는 BUF_CURSOR_SAVE_EVERY 건마다 NVS 에 저장한다 — 전원 단절 시 최대 31건이
// 재전송되지만, 서버 telemetry PK 가 (device_id, ts) + ON CONFLICT DO NOTHING 이라 무해하다.
void pop();

// resolveTs 의 결과. **RETRY 와 DISCARD 를 반드시 구분해야 한다** —
// 둘을 묶어 폐기하면 아직 복원 가능한 레코드를 버리게 된다.
enum class TsFix : uint8_t {
    OK,        // ts 확정 — 발행 가능
    RETRY,     // 아직 시각을 모른다(Clk base 없음) — 폐기하지 말고 다음 기회에
    DISCARD,   // 복원 불가(다른 부팅 세션의 ts=0) — 폐기
};

/**
 * 업로드 직전 ts 소급 복원 — 플래시 레코드는 고치지 않고 **RAM 에서 계산**한다.
 *   rec.ts != 0                        → OK (그대로 사용)
 *   rec.boot == 현재 boot && Clk 있음  → OK (ts = Clk::now() - (현재 up_s - rec.up_s))
 *   rec.boot == 현재 boot && Clk 없음  → RETRY
 *   rec.boot != 현재 boot              → DISCARD
 *
 * ts=0 으로 보내면 서버 PK 가 (device_id, 0) 이라 여러 건이 한 행으로 붕괴하므로
 * (0.2.10 에서 겪은 것과 같은 실패), 보내지 않고 폐기하는 편이 정직하다.
 *
 * ⚠️ RETRY 는 "지금은 모르지만 곧 안다"는 뜻이다. 접속된 직후엔 아직 GPS fix 도
 *    모뎀 NITZ 도 없을 수 있는데(지하주차장에서 LTE 만 붙은 구간), 그 순간
 *    폐기해 버리면 몇 초 뒤 시각이 도착했을 때 복원할 수 있었던 데이터가 사라진다.
 *    설계 §4.2 의 폐기 조건은 "다른 부팅 세션" 하나뿐이다.
 */
TsFix resolveTs(Record &rec);

// 링 전체 erase(현장 초기화). ⚠️ 미전송 주행 데이터가 사라진다.
void clear(Stream &log);

struct Stats {
    bool     available;
    uint32_t capacity;      // 총 레코드 수용량 (섹터수 × 73)
    uint32_t sectorCount;
    uint32_t backlog;       // 미전송 건수
    uint32_t stored;        // 누적 적재
    uint32_t uploaded;      // 누적 업로드(pop)
    uint32_t dropped;       // 누적 폐기(링 랩)
    uint32_t discarded;     // 누적 폐기(시각 복원 불가 / CRC 손상)
    uint32_t headSeq;       // head 섹터의 sector_seq
    uint32_t tailSeq;
    uint32_t writeAbs;      // 절대 쓰기 인덱스
    uint32_t readAbs;       // 절대 읽기 커서
    uint16_t bootCount;
};
void stats(Stats &out);

// 진단 콘솔 'buf' / 'buf clear'. 처리했으면 true.
bool tryConsole(const String &cmd, const String &arg, Stream &io);

} // namespace Buf
