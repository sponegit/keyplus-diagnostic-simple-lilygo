#pragma once
// ===========================================================================
// KpOta — 서브 펌웨어 OTA, 메인 중계 (작업서 S5)
//
//   LTE https_body → 메인 ──OTA_CHUNK──▶ 서브 Update.write
//                       ◀──OTA_ACK────
// 메인은 이미지를 저장하지 않는다. 모뎀에서 읽은 조각을 그대로 보내고, 서브가 받았다고
// 답해야 다음 조각을 읽는다(stop-and-wait). 메인 쪽 버퍼가 조각 하나뿐이라 되감기가 안 되므로,
// 서브는 "다음에 받을 오프셋"을 답하고 메인은 그 오프셋의 조각만 다시 보낼 수 있다.
//
// 흐름:
//   OTA_BEGIN{size, baud, md5, version} → ACK(BEGIN, OK)  — 이후 양쪽이 baud 로 전환
//   OTA_CHUNK{offset, data}             → ACK(CHUNK, OK, next)   (중복 조각은 쓰지 않고 next 만)
//   OTA_END{total}                      → ACK(END, OK)  — 서브는 마커·MD5 검증 후 재부팅
//
// Receiver 는 Update(플래시)를 콜백으로 받는다 — 오프셋·중복·순서 판단을 호스트에서 시험한다.
// Arduino 의존이 없다(test/test_kpota).
// ===========================================================================
#include <stddef.h>
#include <stdint.h>

#define KPOTA_CHUNK_MAX      1024      // 조각 데이터 상한(KPLINK_MAX_PAYLOAD 안에 머리 4바이트와 함께)
#define KPOTA_MD5_LEN        32        // 16진 32자. 빈 값이면 크기만 검증
#define KPOTA_VER_MAX        15
#define KPOTA_IDLE_ABORT_MS  10000     // 서브: OTA 프레임이 이만큼 안 오면 포기·baud 복귀

namespace KpOta {

enum class Phase : uint8_t { BEGIN = 1, CHUNK = 2, END = 3 };

enum class Status : uint8_t {
    OK          = 0,
    NOT_STARTED = 1,   // BEGIN 없이 CHUNK/END
    NO_SPACE    = 2,   // Update.begin 실패(크기 > 파티션)
    WRITE_FAIL  = 3,
    BAD_OFFSET  = 4,   // 앞 오프셋을 건너뛴 조각 — 메인 버그 또는 유실
    SIZE_MISMATCH = 5, // END 의 total 이 받은 바이트와 다름
    FOREIGN_HW  = 6,   // 이미지 마커가 다른 하드웨어(예: 메인 이미지)
    VERIFY_FAIL = 7,   // MD5·이미지 검증 실패(Update.end)
    BAD_FRAME   = 8,
};
const char* statusName(Status s);

struct Begin {
    uint32_t size;
    uint32_t baud;                      // 0 = 전환 안 함
    char     md5[KPOTA_MD5_LEN + 1];    // 빈 문자열 허용
    char     version[KPOTA_VER_MAX + 1];
};

// OTA_BEGIN : [size:4][baud:4][md5Len:1][md5][verLen:1][ver]
// OTA_CHUNK : [offset:4][data]
// OTA_END   : [total:4]
// OTA_ACK   : [phase:1][status:1][next:4]
size_t encodeBegin(const Begin& b, uint8_t* out, size_t cap);
bool   decodeBegin(const uint8_t* p, size_t len, Begin* b);
size_t encodeChunk(uint32_t offset, const uint8_t* data, size_t n, uint8_t* out, size_t cap);
bool   decodeChunk(const uint8_t* p, size_t len, uint32_t* offset, const uint8_t** data, size_t* n);
size_t encodeEnd(uint32_t total, uint8_t* out, size_t cap);
bool   decodeEnd(const uint8_t* p, size_t len, uint32_t* total);
size_t encodeAck(Phase ph, Status st, uint32_t next, uint8_t* out, size_t cap);
bool   decodeAck(const uint8_t* p, size_t len, Phase* ph, Status* st, uint32_t* next);

#define KPOTA_ACK_LEN   6
#define KPOTA_BEGIN_MAX (4 + 4 + 1 + KPOTA_MD5_LEN + 1 + KPOTA_VER_MAX)

// --- 서브: 수신기 ---------------------------------------------------------------
class Receiver {
public:
    struct Flash {
        // size 바이트짜리 이미지 쓰기 시작. md5 가 비어 있지 않으면 끝에서 검증.
        bool   (*begin)(uint32_t size, const char* md5, void* ctx);
        bool   (*write)(const uint8_t* data, size_t n, void* ctx);
        // 마감 — 마커·MD5 검증. OK 면 다음 부팅 이미지로 확정된 것.
        Status (*end)(void* ctx);
        void   (*abort)(void* ctx);
        void*  ctx;
    };
    typedef void (*ReplyFn)(const uint8_t* payload, size_t n, void* ctx);

    void begin(const Flash& flash, ReplyFn reply, void* replyCtx);

    void onBegin(const uint8_t* p, size_t len, uint32_t now);
    void onChunk(const uint8_t* p, size_t len, uint32_t now);
    void onEnd(const uint8_t* p, size_t len, uint32_t now);
    // 무진전 감시. KPOTA_IDLE_ABORT_MS 동안 프레임이 없으면 포기하고 true(=baud 복귀하라).
    bool tick(uint32_t now);

    bool        active() const { return active_; }
    bool        finished() const { return finished_; }   // END OK — 재부팅 대기
    uint32_t    received() const { return next_; }
    const Begin& info() const { return info_; }

private:
    void reply(Phase ph, Status st);
    void fail();

    Flash    flash_ = {};
    ReplyFn  reply_ = nullptr;
    void*    replyCtx_ = nullptr;
    bool     active_ = false;
    bool     finished_ = false;
    Begin    info_ = {};
    uint32_t next_ = 0;
    uint32_t lastAt_ = 0;
};

}  // namespace KpOta
