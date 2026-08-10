/**
 * @file      buffer.cpp
 * @brief     raw 링버퍼 구현. 설계: offline-telemetry-buffer.md §2, §3
 *
 * 섹터 레이아웃 (4096B):
 *   ┌────────────────────────────────────────────┐
 *   │ 헤더 8B │ 레코드 56B × 73 = 4,088B         │  → 4,096B 정확히 채운다(패딩 0)
 *   └────────────────────────────────────────────┘
 *   헤더: magic u32 (0x4B504246 "KPBF") + sector_seq u32 (단조 증가)
 *
 * 절대 인덱스: abs = sector_seq × 73 + slot. sector_seq 가 링 수명 전체에 걸쳐 단조
 * 증가하므로 abs 는 "몇 번째로 적재된 레코드인가"와 같고, 쓰기/읽기 커서를 이 한 축으로
 * 비교할 수 있다(물리 섹터 번호는 s_seqOf[] 역인덱스로 찾는다).
 */
#include "buffer.h"
#include "config.h"
#include "clk.h"
#include "log.h"
#include <esp_partition.h>
#include <Preferences.h>
#include <string.h>
#include <math.h>

namespace Buf {

static const uint32_t kMagic = 0x4B504246UL;   // "KPBF"

struct SectorHeader {
    uint32_t magic;
    uint32_t seq;
};
static_assert(sizeof(SectorHeader) == 8, "섹터 헤더는 8B 고정");

static const esp_partition_t *s_part = nullptr;
static uint32_t s_sectorCount = 0;
static uint32_t s_capacity    = 0;

// 물리 섹터 → sector_seq. 0 = 미기록/무효. 링 랩 시 이 배열만 갱신하면 된다.
// 섹터 수는 런타임 값이지만 상한(현 파티션 표 128KB → 32)에 여유를 둔 정적 배열로 잡는다
// — 힙 할당을 피해 브라운아웃/단편화 위험을 줄인다.
static uint32_t s_seqOf[BUF_MAX_SECTORS] = {0};

static uint32_t s_writeAbs = 0;   // 다음에 쓸 절대 인덱스
static uint32_t s_readAbs  = 0;   // 다음에 읽을 절대 인덱스
static uint32_t s_maxSeq   = 0;   // 현재까지 배정된 최대 sector_seq

static uint32_t s_cStored = 0, s_cUploaded = 0, s_cDropped = 0, s_cDiscarded = 0;
static uint16_t s_boot    = 0;
static uint32_t s_popsSinceSave = 0;

static const char *kNs         = "buf";
static const char *kKeyBoot    = "boot";
static const char *kKeyCursor  = "rd_abs";

// ── CRC-16/CCITT (0xFFFF init, poly 0x1021) ────────────────────────────────
static uint16_t crc16(const uint8_t *d, size_t n)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static uint32_t sectorOffset(uint32_t phys) { return phys * BUF_SECTOR_SIZE; }
static uint32_t slotOffset(uint32_t phys, uint32_t slot)
{
    return sectorOffset(phys) + sizeof(SectorHeader) + slot * BUF_RECORD_SIZE;
}

// sector_seq → 물리 섹터. 없으면 -1(이미 폐기된 구간).
static int physOf(uint32_t seq)
{
    if (seq == 0) return -1;
    for (uint32_t i = 0; i < s_sectorCount; i++) {
        if (s_seqOf[i] == seq) return (int)i;
    }
    return -1;
}

// 현재 살아있는 가장 오래된 절대 인덱스.
static uint32_t tailAbs()
{
    uint32_t minSeq = 0;
    for (uint32_t i = 0; i < s_sectorCount; i++) {
        if (s_seqOf[i] == 0) continue;
        if (minSeq == 0 || s_seqOf[i] < minSeq) minSeq = s_seqOf[i];
    }
    return minSeq ? minSeq * BUF_RECS_PER_SECTOR : 0;
}

// 읽기 커서가 폐기된 구간을 가리키면 살아있는 가장 오래된 위치로 끌어올린다.
static void clampRead()
{
    uint32_t t = tailAbs();
    if (s_readAbs < t) {
        s_readAbs = t;
    }
    if (s_readAbs > s_writeAbs) s_readAbs = s_writeAbs;
}

static void saveCursor()
{
    Preferences p;
    p.begin(kNs, /*readOnly=*/false);
    p.putULong(kKeyCursor, s_readAbs);
    p.end();
    s_popsSinceSave = 0;
}

// 섹터 하나를 erase 하고 새 sector_seq 를 배정 + 헤더 기록.
static bool allocSector(uint32_t phys, Stream &log)
{
    if (esp_partition_erase_range(s_part, sectorOffset(phys), BUF_SECTOR_SIZE) != ESP_OK) {
        LOGE(log, "[BUF] 섹터 %lu erase 실패\n", (unsigned long)phys);
        return false;
    }
    SectorHeader h = { kMagic, ++s_maxSeq };
    if (esp_partition_write(s_part, sectorOffset(phys), &h, sizeof(h)) != ESP_OK) {
        LOGE(log, "[BUF] 섹터 %lu 헤더 쓰기 실패\n", (unsigned long)phys);
        s_maxSeq--;
        return false;
    }
    s_seqOf[phys] = h.seq;
    return true;
}

bool begin(Stream &log)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      ESP_PARTITION_SUBTYPE_ANY, BUF_PARTITION_LABEL);
    if (!s_part) {
        LOGE(log, "[BUF] 파티션 '%s' 없음 — 오프라인 적재 비활성\n", BUF_PARTITION_LABEL);
        return false;
    }

    // ⚠️ 섹터 수를 컴파일 상수로 박지 않는다 — 차후 파티션을 재설계해도 같은 펌웨어가
    //    두 종류 단말을 모두 지원한다(지금은 항상 32).
    s_sectorCount = s_part->size / BUF_SECTOR_SIZE;
    if (s_sectorCount > BUF_MAX_SECTORS) {
        LOGW(log, "[BUF] 파티션이 %luKB — 상한 %d섹터만 사용한다\n",
             (unsigned long)(s_part->size / 1024), BUF_MAX_SECTORS);
        s_sectorCount = BUF_MAX_SECTORS;
    }
    if (s_sectorCount == 0) {
        LOGE(log, "[BUF] 파티션이 너무 작다(%lu B)\n", (unsigned long)s_part->size);
        s_part = nullptr;
        return false;
    }
    s_capacity = s_sectorCount * BUF_RECS_PER_SECTOR;

    // 부팅 카운터 — 시각 소급 복원 가능 여부 판정에 쓴다(§4.2).
    {
        Preferences p;
        p.begin(kNs, /*readOnly=*/false);
        s_boot = (uint16_t)(p.getUShort(kKeyBoot, 0) + 1);
        p.putUShort(kKeyBoot, s_boot);
        s_readAbs = p.getULong(kKeyCursor, 0);
        p.end();
    }

    // ── 섹터 헤더 스캔 → head/tail 복원 (NVS 불필요, 전원 단절에도 위치가 복원된다) ──
    uint32_t maxSeq = 0;
    int      headPhys = -1;
    for (uint32_t i = 0; i < s_sectorCount; i++) {
        SectorHeader h = {0, 0};
        if (esp_partition_read(s_part, sectorOffset(i), &h, sizeof(h)) != ESP_OK) continue;
        if (h.magic != kMagic || h.seq == 0 || h.seq == 0xFFFFFFFFUL) { s_seqOf[i] = 0; continue; }
        s_seqOf[i] = h.seq;
        if (h.seq > maxSeq) { maxSeq = h.seq; headPhys = (int)i; }
    }

    if (headPhys < 0) {
        // 유효 섹터가 하나도 없다 → 전체 초기화. erase 는 첫 쓰기에서 섹터 단위로 한다.
        LOGI(log, "[BUF] 유효 섹터 없음 — 링 초기화 (%lu섹터 / %lu건)\n",
             (unsigned long)s_sectorCount, (unsigned long)s_capacity);
        for (uint32_t i = 0; i < s_sectorCount; i++) s_seqOf[i] = 0;
        s_maxSeq   = 0;
        s_writeAbs = 0;
        s_readAbs  = 0;
        if (!allocSector(0, log)) { s_part = nullptr; return false; }
        s_writeAbs = s_maxSeq * BUF_RECS_PER_SECTOR;
    } else {
        s_maxSeq = maxSeq;
        // head 섹터 안에서 첫 미기록 슬롯(ts == 0xFFFFFFFF)을 찾는다.
        uint32_t slot = 0;
        for (; slot < BUF_RECS_PER_SECTOR; slot++) {
            uint32_t first = 0;
            if (esp_partition_read(s_part, slotOffset(headPhys, slot), &first, 4) != ESP_OK) break;
            if (first == 0xFFFFFFFFUL) break;
        }
        s_writeAbs = maxSeq * BUF_RECS_PER_SECTOR + slot;
    }

    clampRead();

    LOGI(log, "[BUF] 링 준비됨 — %lu섹터 %lu건 수용, 미전송 %lu건 (boot=%u)\n",
         (unsigned long)s_sectorCount, (unsigned long)s_capacity,
         (unsigned long)count(), s_boot);
    return true;
}

bool available() { return s_part != nullptr; }

uint32_t count()
{
    if (!s_part) return 0;
    return (s_writeAbs > s_readAbs) ? (s_writeAbs - s_readAbs) : 0;
}

// 아래에 정의. pushSample/testFill 이 공유하는 링 기록 경로.
static bool writeRecord(Record &r, Stream &log);

bool pushSample(const GpsFix &fix, bool fixFresh, const Obd2::Data &obd,
                uint32_t seq, const Fast::Aggregate &agg, bool parkedWindow,
                int rssi, int reg, Stream &log)
{
    if (!s_part) return false;

    Record r;
    memset(&r, 0, sizeof(r));

    Clk::Src src = Clk::SRC_NONE;
    r.ts   = Clk::now(&src);
    r.up_s = millis() / 1000UL;
    r.boot = s_boot;
    r.seq  = (uint16_t)(seq & 0xFFFF);

    r.flags = (uint8_t)(src & F_TSRC_MASK);
    if (fixFresh)  r.flags |= F_GPS_FIX;
    if (obd.valid) r.flags |= F_OBD_VALID;
    if (parkedWindow) r.flags |= F_PARKED_WIN;

    // GPS — 측위를 놓쳤어도 마지막 유효 좌표를 싣는다(실시간 telemetry 와 같은 규약).
    if (fix.valid) {
        r.lat_e6  = (int32_t)lroundf(fix.lat * 1e6f);
        r.lon_e6  = (int32_t)lroundf(fix.lon * 1e6f);
        r.spd_x10 = (uint16_t)(fix.speed > 0 ? lroundf(fix.speed * 10.0f) : 0);
        r.sat     = (uint8_t)(fix.vsat > 255 ? 255 : (fix.vsat < 0 ? 0 : fix.vsat));
    }

    r.rssi = (int8_t)(rssi > 127 ? 127 : (rssi < -128 ? -128 : rssi));
    r.reg  = (uint8_t)(reg < 0 ? 0 : (reg > 255 ? 255 : reg));

    if (obd.valid) {
        if (obd.has_rpm)      { r.obd_flags |= O_RPM;      r.rpm = (uint16_t)(obd.rpm < 0 ? 0 : (obd.rpm > 65535.0f ? 65535 : lroundf(obd.rpm))); }
        if (obd.has_speed)    { r.obd_flags |= O_SPEED;    r.obd_speed = (uint8_t)(obd.speed < 0 ? 0 : (obd.speed > 255 ? 255 : obd.speed)); }
        if (obd.has_coolant)  { r.obd_flags |= O_COOLANT;  r.coolant = (int8_t)(obd.coolant < -128 ? -128 : (obd.coolant > 127 ? 127 : obd.coolant)); }
        if (obd.has_load)     { r.obd_flags |= O_LOAD;     r.load_x2 = (uint8_t)(obd.load < 0 ? 0 : (obd.load > 127.5f ? 255 : lroundf(obd.load * 2.0f))); }
        if (obd.has_throttle) { r.obd_flags |= O_THR;      r.throttle_x2 = (uint8_t)(obd.throttle < 0 ? 0 : (obd.throttle > 127.5f ? 255 : lroundf(obd.throttle * 2.0f))); }
        if (obd.has_intake)   { r.obd_flags |= O_INTAKE;   r.intake = (int8_t)(obd.intake < -128 ? -128 : (obd.intake > 127 ? 127 : obd.intake)); }
        if (obd.has_maf)      { r.obd_flags |= O_MAF;      r.maf_x10 = (uint16_t)(obd.maf < 0 ? 0 : (obd.maf > 6553.5f ? 65535 : lroundf(obd.maf * 10.0f))); }
        if (obd.has_fuel)     { r.obd_flags |= O_FUEL;     r.fuel_x2 = (uint8_t)(obd.fuel < 0 ? 0 : (obd.fuel > 127.5f ? 255 : lroundf(obd.fuel * 2.0f))); }
        if (obd.has_ctrlv)    { r.obd_flags |= O_CTRLV;    r.ctrl_mv = (uint16_t)(obd.ctrl_v < 0 ? 0 : (obd.ctrl_v > 65.535f ? 65535 : lroundf(obd.ctrl_v * 1000.0f))); }
        if (obd.has_runtime)  { r.obd_flags |= O_RUNTIME;  r.runtime_s = obd.runtime; }
        if (obd.has_odometer) { r.obd_flags |= O_ODOMETER; r.odo_x10 = (uint32_t)(obd.odometer < 0 ? 0 : lroundf(obd.odometer * 10.0f)); }
    }

    // 고빈도 창 집계 8B. agg.n==0 이면 전부 0 → 백필에서 agg 오브젝트를 통째로 생략한다.
    r.agg_spd_max      = agg.spd_max;
    r.agg_spd_min      = agg.spd_min;
    r.agg_rpm_max_x100 = agg.rpm_max_x100;
    r.agg_accel_x10    = agg.accel_x10;
    r.agg_decel_x10    = agg.decel_x10;
    r.agg_ev           = agg.ev;
    r.agg_ev_n         = agg.ev_n;
    r.agg_n            = agg.n;

    return writeRecord(r, log);
}

// 완성된 Record 를 CRC 봉인 후 링에 기록한다. pushSample 과 testFill 이 공유한다 —
// 링 전진(섹터 폐기·랩·커서 clamp)을 두 벌 두면 반드시 어긋난다.
static bool writeRecord(Record &r, Stream &log)
{
    r.crc16 = crc16((const uint8_t *)&r, sizeof(Record) - 2);

    // ── 쓰기 위치 확보 ──────────────────────────────────────────────────────
    uint32_t seqNo = s_writeAbs / BUF_RECS_PER_SECTOR;
    uint32_t slot  = s_writeAbs % BUF_RECS_PER_SECTOR;
    int      phys  = physOf(seqNo);

    if (phys < 0) {
        // 현재 섹터가 가득 차 다음 섹터로 넘어가야 한다. 가장 오래된 섹터를 통째로 폐기한다.
        uint32_t victimSeq = 0;
        int      victim    = -1;
        for (uint32_t i = 0; i < s_sectorCount; i++) {
            if (s_seqOf[i] == 0) { victim = (int)i; victimSeq = 0; break; }
            if (victimSeq == 0 || s_seqOf[i] < victimSeq) { victimSeq = s_seqOf[i]; victim = (int)i; }
        }
        if (victim < 0) return false;

        bool wrapped = (s_seqOf[victim] != 0);
        if (!allocSector((uint32_t)victim, log)) return false;

        if (wrapped) {
            s_cDropped += BUF_RECS_PER_SECTOR;
            // 조용히 사라지지 않게 한다 — 폐기는 데이터 손실이다.
            LOGW(log, "[BUF] 링 랩 — %d건 폐기(누적 %lu건)\n",
                 BUF_RECS_PER_SECTOR, (unsigned long)s_cDropped);
        }
        s_writeAbs = s_maxSeq * BUF_RECS_PER_SECTOR;
        seqNo = s_maxSeq;
        slot  = 0;
        phys  = victim;
        clampRead();
    }

    if (esp_partition_write(s_part, slotOffset((uint32_t)phys, slot), &r, sizeof(r)) != ESP_OK) {
        LOGE(log, "[BUF] 레코드 쓰기 실패 (섹터 %d 슬롯 %lu)\n", phys, (unsigned long)slot);
        return false;
    }

    s_writeAbs++;
    s_cStored++;
    LOGD(log, "[BUF] 적재 #%lu ts=%lu tsrc=%d agg_n=%u (백로그 %lu)\n",
         (unsigned long)s_cStored, (unsigned long)r.ts, (int)(r.flags & F_TSRC_MASK),
         r.agg_n, (unsigned long)count());
    return true;
}

bool peek(Record &out, Stream &log)
{
    if (!s_part) return false;
    clampRead();

    // CRC 손상 레코드는 건너뛴다(브라운아웃으로 쓰기가 잘린 경우).
    while (s_readAbs < s_writeAbs) {
        uint32_t seqNo = s_readAbs / BUF_RECS_PER_SECTOR;
        uint32_t slot  = s_readAbs % BUF_RECS_PER_SECTOR;
        int      phys  = physOf(seqNo);
        if (phys < 0) {
            // 커서가 가리키던 섹터가 폐기됐다 → 살아있는 가장 오래된 위치로 끌어올린다.
            // clampRead 가 커서를 못 움직이는 조합(있을 수 없지만)에서도 루프가 돌지
            // 않도록, 전진이 없으면 강제로 1건 넘긴다.
            uint32_t before = s_readAbs;
            clampRead();
            if (s_readAbs == before) { s_readAbs++; s_cDiscarded++; }
            continue;
        }

        if (esp_partition_read(s_part, slotOffset((uint32_t)phys, slot), &out, sizeof(out)) != ESP_OK) {
            s_readAbs++; s_cDiscarded++;
            continue;
        }
        if (out.ts == 0xFFFFFFFFUL) {
            // 미기록 슬롯 — head 섹터의 꼬리. 여기서부터는 읽을 것이 없다.
            return false;
        }
        if (crc16((const uint8_t *)&out, sizeof(Record) - 2) != out.crc16) {
            LOGW(log, "[BUF] CRC 불일치 — 레코드 1건 폐기(누적 %lu건)\n",
                 (unsigned long)(s_cDiscarded + 1));
            s_readAbs++; s_cDiscarded++;
            continue;
        }
        return true;
    }
    return false;
}

void pop()
{
    if (!s_part) return;
    if (s_readAbs >= s_writeAbs) return;
    s_readAbs++;
    s_cUploaded++;
    // 레코드마다 NVS 에 쓰면 마모와 지연이 늘고 이득은 최대 31건의 중복 회피뿐이다.
    if (++s_popsSinceSave >= BUF_CURSOR_SAVE_EVERY) saveCursor();
}

TsFix resolveTs(Record &rec)
{
    if (rec.ts != 0) return TsFix::OK;

    // 같은 부팅 세션이 아니면 up_s 기준이 달라 복원 불가다(§4.2 — 유일한 폐기 조건).
    if (rec.boot != s_boot) return TsFix::DISCARD;

    // 아직 시각을 모른다 — 폐기하면 안 된다. 접속 직후엔 GPS fix 도 모뎀 NITZ 도
    // 없을 수 있고, 몇 초 뒤 도착하면 이 레코드는 그대로 복원된다.
    uint32_t nowEp = Clk::now();
    if (nowEp == 0) return TsFix::RETRY;

    uint32_t upNow = millis() / 1000UL;
    // 아래 둘은 물리적으로 있을 수 없는 조합이다(up_s 는 단조 증가, back 은 epoch 미만).
    // 레코드가 손상됐다는 뜻이므로 재시도 가치가 없다 → 폐기한다.
    if (upNow < rec.up_s) return TsFix::DISCARD;
    uint32_t back = upNow - rec.up_s;
    if (back > nowEp) return TsFix::DISCARD;

    rec.ts = nowEp - back;
    return TsFix::OK;
}

void clear(Stream &log)
{
    if (!s_part) return;
    esp_partition_erase_range(s_part, 0, s_sectorCount * BUF_SECTOR_SIZE);
    for (uint32_t i = 0; i < s_sectorCount; i++) s_seqOf[i] = 0;
    s_maxSeq   = 0;
    s_writeAbs = 0;
    s_readAbs  = 0;
    allocSector(0, log);
    s_writeAbs = s_maxSeq * BUF_RECS_PER_SECTOR;
    s_readAbs  = s_writeAbs;
    saveCursor();
    LOGI(log, "[BUF] 링 전체 초기화 완료 (%lu섹터)\n", (unsigned long)s_sectorCount);
}

#if FEATURE_BUF_TESTFILL
uint32_t testFill(uint32_t n, uint32_t gapS, Stream &log)
{
    if (!s_part) { LOGE(log, "[BUF] 파티션 없음 — 주입 불가\n"); return 0; }
    if (n == 0)    return 0;
    if (gapS == 0) gapS = 30;

    // Clk base 가 없으면 ts=0 이 되고, 그건 publishBackfill 이 발행을 거부하는 값이다.
    // 여기서 막지 않으면 링만 채우고 드레인은 한 건도 안 나가는 상태가 된다.
    Clk::Src src   = Clk::SRC_NONE;
    const uint32_t nowTs = Clk::now(&src);
    if (nowTs == 0) {
        LOGE(log, "[BUF] 시각 기준 없음 — 접속·측위 후 다시 시도할 것(주입 취소)\n");
        return 0;
    }

    // 용량을 넘기면 링이 랩하면서 방금 넣은 앞부분을 스스로 지운다. 조용히 그러느니 자른다.
    if (n > s_capacity) {
        LOGW(log, "[BUF] 요청 %lu건이 용량 %lu건 초과 — %lu건으로 자른다\n",
             (unsigned long)n, (unsigned long)s_capacity, (unsigned long)s_capacity);
        n = s_capacity;
    }
    // 가장 오래된 ts 가 과거로 얼마나 가는지 미리 알린다(서버에서 대조할 기준).
    LOGI(log, "[BUF] 시험 주입 시작 — %lu건, %lu초 간격 (ts %lu ~ %lu)\n",
         (unsigned long)n, (unsigned long)gapS,
         (unsigned long)(nowTs - (uint32_t)n * gapS), (unsigned long)(nowTs - gapS));

    uint32_t made = 0;
    for (uint32_t i = 0; i < n; i++) {
        Record r;
        memset(&r, 0, sizeof(r));

        // ⚠️ 핵심 — ts 를 과거로 흩는다. 뭉치면 서버 PK 에서 붕괴한다(buffer.h 주석).
        r.ts   = nowTs - (uint32_t)(n - i) * gapS;
        r.up_s = (uint32_t)(millis() / 1000UL);
        r.boot = s_boot;
        r.seq  = (uint16_t)(i & 0xFFFF);

        // 정차 창으로 표시 — publishBackfill 이 agg.w 를 OFFLINE_LOG_MS_PARKED 로 채운다.
        r.flags = (uint8_t)((src & F_TSRC_MASK) | F_GPS_FIX | F_OBD_VALID | F_PARKED_WIN);

        // 좌표는 매 건 미세하게 흔든다 — 완전히 같은 값이면 서버 파생 계산이 한 점으로 본다.
        r.lat_e6  = 37500000 + (int32_t)(i % 500);
        r.lon_e6  = 127000000 + (int32_t)(i % 500);
        r.spd_x10 = 0;
        r.sat     = 9;
        r.rssi    = -75;
        r.reg     = 1;

        // OBD 11필드를 모두 채운다 — 필드 유무가 페이로드 길이를 가르므로 실물과 맞춘다.
        r.obd_flags   = O_RPM | O_SPEED | O_COOLANT | O_LOAD | O_THR | O_INTAKE |
                        O_MAF | O_FUEL | O_CTRLV | O_RUNTIME | O_ODOMETER;
        r.rpm         = 800;
        r.obd_speed   = 0;
        r.coolant     = 85;
        r.load_x2     = 40;      // 20.0 %
        r.throttle_x2 = 30;      // 15.0 %
        r.intake      = 30;
        r.maf_x10     = 25;      // 2.5 g/s
        r.fuel_x2     = 100;     // 50.0 %
        r.ctrl_mv     = 14000;
        r.runtime_s   = (uint16_t)((i * gapS) & 0xFFFF);
        r.odo_x10     = 1000000UL + i;

        // agg 는 5건에 1건만 — 실측 292/1320 ≈ 22%. 크기 분포를 실물에 맞추기 위한 것이다.
        // ev/ev_n 은 0 으로 둔다: aggToEvents 가 세운 비트만 driving_events 로 만드는데,
        // source='agg' 이벤트는 원본이 없어 **서버에서 지우지 않는다**(015 주석). 시험용
        // 가짜 이벤트를 영구히 남기지 않으려는 것이다.
        if (i % 5 == 0) {
            r.agg_n            = 30;
            r.agg_rpm_max_x100 = 8;   // 800 rpm
        }

        if (!writeRecord(r, log)) {
            LOGE(log, "[BUF] %lu건째에서 쓰기 실패 — 중단\n", (unsigned long)(i + 1));
            break;
        }
        made++;

        // 73건마다 섹터 erase(수십 ms)가 끼어든다. 태스크 워치독에 여유를 준다.
        if ((i & 0x0F) == 0) delay(0);
    }

    LOGI(log, "[BUF] 시험 주입 완료 — %lu건 기록, 백로그 %lu건\n",
         (unsigned long)made, (unsigned long)count());
    return made;
}
#endif  // FEATURE_BUF_TESTFILL

void stats(Stats &out)
{
    out.available   = (s_part != nullptr);
    out.capacity    = s_capacity;
    out.sectorCount = s_sectorCount;
    out.backlog     = count();
    out.stored      = s_cStored;
    out.uploaded    = s_cUploaded;
    out.dropped     = s_cDropped;
    out.discarded   = s_cDiscarded;
    out.headSeq     = s_maxSeq;
    out.tailSeq     = s_sectorCount ? (tailAbs() / BUF_RECS_PER_SECTOR) : 0;
    out.writeAbs    = s_writeAbs;
    out.readAbs     = s_readAbs;
    out.bootCount   = s_boot;
}

bool tryConsole(const String &cmd, const String &arg, Stream &io)
{
    if (cmd != "buf") return false;

    String a = arg; a.trim(); a.toLowerCase();
    if (a == "clear") {
        if (!s_part) { io.println("[BUF] 파티션 없음 — 초기화할 것이 없다"); return true; }
        uint32_t lost = count();
        clear(io);
        io.printf("[BUF] 링 초기화됨 — 미전송 %lu건이 삭제되었다\n", (unsigned long)lost);
        return true;
    }

    // 'buf fill <건수> [간격초]' — 시험 주입. 플래그가 꺼진 빌드에서도 분기는 남겨
    // "왜 안 되는지"를 답하게 한다(명령이 통째로 사라지면 오타로 오인한다).
    if (a == "fill" || a.startsWith("fill ")) {
#if FEATURE_BUF_TESTFILL
        String rest = a.substring(4); rest.trim();
        uint32_t n = 0, gap = 30;
        int sp = rest.indexOf(' ');
        if (sp < 0) {
            n = (uint32_t)rest.toInt();
        } else {
            n = (uint32_t)rest.substring(0, sp).toInt();
            String g = rest.substring(sp + 1); g.trim();
            if (g.length()) gap = (uint32_t)g.toInt();
        }
        if (n == 0) {
            io.println("사용법: buf fill <건수> [간격초=30]");
            io.println("  예) buf fill 1000 30   — 30초 간격 1000건(과거 8시간20분치)");
            io.println("  · 접속된 상태에서 실행할 것 — 시각 기준이 있어야 ts 가 유효하다");
            io.println("  · 'buf clear' 를 먼저 해 실데이터와 섞이지 않게 할 것");
            return true;
        }
        uint32_t made = testFill(n, gap, io);
        if (made) {
            // 실측 드레인 속도 0.56건/초(2026-08-10) 기준 소요 추정.
            unsigned long mins = (unsigned long)((made * 100UL / 56UL) / 60UL);
            io.printf("[BUF] %lu건 주입됨 — 접속 %lus 뒤 드레인 시작, 약 %lu분 소요 예상\n",
                      (unsigned long)made,
                      (unsigned long)(BACKFILL_START_DELAY_MS / 1000UL), mins);
        }
#else
        io.println("[BUF] 'buf fill' 은 FEATURE_BUF_TESTFILL=1 빌드에서만 동작한다.");
        io.println("      플릿 빌드는 0 이다 — 현장 단말에 임의 레코드를 주입하지 않기 위한 것.");
#endif
        return true;
    }

    Stats s; stats(s);
    io.println("[BUF]");
    if (!s.available) {
        io.printf("  파티션 '%s' 없음 — 오프라인 적재 비활성\n", BUF_PARTITION_LABEL);
        return true;
    }
    uint32_t used = s.capacity ? (s.backlog * 100UL / s.capacity) : 0;
    io.printf("  %-14s: %lu건 (%lu%% of %lu)\n", "미전송", (unsigned long)s.backlog,
              (unsigned long)used, (unsigned long)s.capacity);
    io.printf("  %-14s: %lu섹터 × %d건 (%luKB)\n", "링 크기",
              (unsigned long)s.sectorCount, BUF_RECS_PER_SECTOR,
              (unsigned long)(s.sectorCount * BUF_SECTOR_SIZE / 1024));
    io.printf("  %-14s: 적재 %lu / 업로드 %lu\n", "누적",
              (unsigned long)s.stored, (unsigned long)s.uploaded);
    io.printf("  %-14s: 링랩 %lu / 손상·시각불명 %lu\n", "누적 폐기",
              (unsigned long)s.dropped, (unsigned long)s.discarded);
    io.printf("  %-14s: head_seq=%lu tail_seq=%lu\n", "섹터",
              (unsigned long)s.headSeq, (unsigned long)s.tailSeq);
    io.printf("  %-14s: write=%lu read=%lu\n", "절대 커서",
              (unsigned long)s.writeAbs, (unsigned long)s.readAbs);
    io.printf("  %-14s: %u\n", "부팅 카운터", s.bootCount);
    io.printf("  %-14s: 주행 %lus / 정차 %lus\n", "적재 주기",
              (unsigned long)(OFFLINE_LOG_MS_DRIVING / 1000UL),
              (unsigned long)(OFFLINE_LOG_MS_PARKED / 1000UL));
    io.printf("  %-14s: %s (%s)\n", "시각 기준", Clk::srcName(Clk::source()),
              Clk::source() == Clk::SRC_NONE ? "미확보 — ts=0 적재 중"
                                             : "적재 ts 유효");
    io.println("  · 'buf clear' 는 미전송 주행 데이터를 통째로 지운다");
#if FEATURE_BUF_TESTFILL
    io.println("  ⚠️ 시험 빌드(FEATURE_BUF_TESTFILL=1) — 'buf fill <건수> [간격초]' 사용 가능.");
    io.println("     이 펌웨어를 플릿에 OTA 하지 말 것.");
#endif
    return true;
}

} // namespace Buf
