/**
 * @file      cmd.cpp
 * @brief     MQTT cmd 다운링크 구현 — 수신 콜백(복사만) + loop 처리(실행/ack).
 */
#include "cmd.h"
#include "log.h"
#include "config.h"
#include "provisioning.h"
#include "carkey.h"
#include "cfg.h"
#include "mqtt.h"     // 발행 간격 가드(publishGapElapsed) + ack 발행 기록(notePublish)
#if FEATURE_OTA
#include "ota.h"
#endif
#include <string.h>
#include <stdlib.h>

namespace Cmd {

static const uint8_t kClientIdx = 0;

// --- 콜백 수신 큐 (mqtt_handle 내부에서 채워짐: 복사 + 인덱스 전진만) ------------
// 1칸 버퍼였을 때는 명령이 loop 주기(50ms)보다 촘촘히 도착하면 뒤엣것이 로그도 ack 도
// 없이 사라졌다. 서버 flushPendingForDevice() 는 pending 을 순회하며 연속 발행하므로
// 재접속 직후 밀린 명령이 여러 건이면 첫 건만 살아남았다 — 게다가 조용히.
//
// ⚠️ head/tail/count 를 volatile 로 두지 않는다. 콜백은 별도 스레드가 아니라
//    loop → Mqtt::handle() → mqtt_handle() 안에서 동기적으로 불린다(단일 컨텍스트).
//    handle() 실행 중에는 콜백이 끼어들 수 없으므로 원자성 문제가 없다.
static char     s_q[CMD_RX_QUEUE_N][CMD_PAYLOAD_MAX];   // 명령 JSON. null 종단.
// ⚠️ 넘치면 경고 없이 잘리고, 잘린 JSON 은 뒤쪽 필드를 못 찾아 조용히 오동작한다
//    (ota_start 라면 url 이 빈 값이 되어 "잘못된 URL"로 실패). 크기는 config.h 참고.
static uint8_t  s_qHead  = 0;   // 다음에 쓸 칸
static uint8_t  s_qTail  = 0;   // 다음에 처리할 칸
static uint8_t  s_qCount = 0;
static uint32_t s_qDropped     = 0;   // 큐가 가득 차 버린 건수(누적)
static uint32_t s_qDropLogged  = 0;   // 그중 로그로 알린 건수 — 조용한 유실을 막는다

// 실행 지연 상태(F2 §5) — received ack 를 낸 뒤 실행을 다음 틱 이후로 미룬다.
// 큐에서 빼지 않은 채로 두고, s_execAt 이 지나면 같은 항목을 실행한다.
static bool     s_execDeferred = false;
static uint32_t s_execAt       = 0;

static String   s_cmdTopic;       // v1/{id}/cmd (구독용. ack 토픽은 sendAck가 매번 조립)
static bool     s_subscribed = false;   // 마지막 subscribe 성공 여부(콘솔 진단용)

// 단일 cmd 토픽만 구독하므로 topic은 무시하고 payload만 사용.
static void onMessage(const char * /*topic*/, const uint8_t *payload, uint32_t len)
{
    if (s_qCount >= CMD_RX_QUEUE_N) {
        // 드롭은 세는 것만 하고 로그는 handle() 에서 남긴다 — 콜백은 AT 응답을 파싱하는
        // 중이라 여기서 시간을 쓰면 뒤따르는 URC 단계를 놓칠 수 있다(cmd.h 주석).
        s_qDropped++;
        return;
    }
    uint32_t n = (len < CMD_PAYLOAD_MAX - 1) ? len : CMD_PAYLOAD_MAX - 1;
    memcpy(s_q[s_qHead], payload, n);
    s_q[s_qHead][n] = '\0';
    s_qHead = (uint8_t)((s_qHead + 1) % CMD_RX_QUEUE_N);
    s_qCount++;
}

// 처리 끝난 항목 1건 제거.
static void qPop()
{
    if (!s_qCount) return;
    s_qTail = (uint8_t)((s_qTail + 1) % CMD_RX_QUEUE_N);
    s_qCount--;
    s_execDeferred = false;
}

// --- 간이 JSON 추출 ("key":"str" / "key": num). 평평·단순 페이로드 전제. -------
static String jStr(const char *body, const char *key)
{
    String pat = String("\"") + key + "\"";
    const char *p = strstr(body, pat.c_str());
    if (!p) return "";
    p = strchr(p + pat.length(), ':');
    if (!p) return "";
    p++;
    while (*p == ' ') p++;
    if (*p != '"') return "";
    p++;
    const char *e = strchr(p, '"');
    if (!e) return "";
    return String(p).substring(0, e - p);
}
static long jNum(const char *body, const char *key)
{
    String pat = String("\"") + key + "\"";
    const char *p = strstr(body, pat.c_str());
    if (!p) return 0;
    p = strchr(p + pat.length(), ':');
    if (!p) return 0;
    return atol(p + 1);
}

// UTC 민간시각 → epoch(초). (Howard Hinnant days_from_civil)
static uint32_t civilToEpoch(int y, int m, int d, int hh, int mi, int ss)
{
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;
    return (uint32_t)(days * 86400L + hh * 3600L + mi * 60L + ss);
}

// 모뎀 NITZ(CCLK) → UTC epoch. 실패/미동기 시 0.
static uint32_t nowEpoch(TinyGsm &modem)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    float tz = 0;
    if (!modem.getNetworkTime(&y, &mo, &d, &h, &mi, &s, &tz)) return 0;
    // 유효성 경계: NITZ/NTP 미동기 시 CCLK가 1970→(+2000)→2070 같은 엉뚱한 값이 됨.
    // 그럴 땐 0을 반환해 ts=0(서버가 수신시각 스탬프)·만료판정 스킵으로 처리한다.
    if (y < 2024 || y > 2060) return 0;
    uint32_t e = civilToEpoch(y, mo, d, h, mi, s);
    // CCLK 시각은 로컬(tz, 15분 단위) → UTC = local - tz*15min. (NTP를 TZ=0로 동기하면 tz=0)
    e -= (int32_t)tz * 15 * 60;
    return e;
}

void begin(TinyGsm &modem)
{
    modem.mqtt_set_callback(onMessage);
}

void subscribe(TinyGsm &modem, Stream &log)
{
    s_cmdTopic = "v1/" + Prov::deviceId() + "/cmd";
    bool ok = modem.mqtt_subscribe(kClientIdx, s_cmdTopic.c_str(), /*qos=*/1);
    s_subscribed = ok;
    if (ok) LOGD(log, "[CMD] subscribe %s ok\n", s_cmdTopic.c_str());
    else    LOGW(log, "[CMD] subscribe %s 실패 — 다운링크 수신 불가\n", s_cmdTopic.c_str());
}

void markUnsubscribed()
{
    // clean_session=1 이라 새 MQTT 세션마다 구독이 사라진다. 모뎀 리셋이면 더더욱.
    // 이 플래그가 참으로 남아 있으면 재구독 로직이 "이미 됐다"고 보고 건너뛴다.
    s_subscribed = false;
}

String topic()        { return s_cmdTopic; }
bool   isSubscribed() { return s_subscribed; }
bool   hasPendingRx() { return s_qCount != 0; }
int    pendingRxCount() { return (int)s_qCount; }
uint32_t droppedRxCount() { return s_qDropped; }

void sendAck(TinyGsm &modem, Stream &log, const String &cmdId, const char *result)
{
    // device_id는 런타임(NVS)이라 매번 조립(재부팅 후 지연 ack도 구독 전에 발행 가능).
    String ackTopic = "v1/" + Prov::deviceId() + "/cmd/ack";
    uint32_t now = nowEpoch(modem);
    char ack[160];
    int n = snprintf(ack, sizeof(ack),
        "{\"command_id\":\"%s\",\"result\":\"%s\",\"ts\":%u}",
        cmdId.c_str(), result, (unsigned)now);
    bool ok = false;
    if (n > 0 && n < (int)sizeof(ack)) {
        ok = modem.mqtt_publish(kClientIdx, ackTopic.c_str(), ack, /*qos=*/1);
        // ⚠️ 성공 여부와 무관하게 발행 사실을 Mqtt 모듈에 알린다(바이트는 이미 나갔다).
        //    이걸 빠뜨리면 간격 가드가 이 ack 를 못 보고 곧바로 telemetry·백필을 통과시켜
        //    발행이 겹친다 — 모뎀은 publish 가 겹치면 세션이 깨진다.
        Mqtt::notePublish();
    }
    LOGI(log, "[CMD] ack id=%s result=%s %s\n", cmdId.c_str(), result,
               ok ? "published" : "FAILED");
}

void handle(TinyGsm &modem, Stream &log)
{
    // 큐 오버플로는 조용히 지나가면 안 된다 — 현장에서 "가끔 안 먹는다"로만 보고되고
    // 원인을 찾을 수 없는 형태의 유실이다. 매 틱 확인해 새로 생긴 드롭만 알린다.
    if (s_qDropped != s_qDropLogged) {
        LOGW(log, "[CMD] 수신 큐 가득 참 — 명령 드롭 누적 %lu건(큐 %d칸)\n",
             (unsigned long)s_qDropped, CMD_RX_QUEUE_N);
        s_qDropLogged = s_qDropped;
    }

    if (!s_qCount) return;

    uint32_t nowMs = millis();

    // 실행 지연 중이면 그 시각까지 기다린다(received ack 와 실행측 발행이 겹치지 않게).
    if (s_execDeferred && (int32_t)(nowMs - s_execAt) < 0) return;

    // ⚠️ 틱당 1건만 처리한다. 그리고 직전 발행과 최소 간격을 확보한 뒤에만 진입한다 —
    //    처리 1건마다 ack 를 발행하므로, 큐를 연속으로 비우면 그게 곧 연속 publish 다.
    //    (telemetry·고빈도 창·백필과의 충돌도 이 가드 하나로 함께 막힌다.)
    if (!Mqtt::publishGapElapsed(nowMs, PUBLISH_MIN_GAP_MS)) return;

    const char *body = s_q[s_qTail];
    String cmdId = jStr(body, "command_id");
    String type  = jStr(body, "type");

    // 수신 로그·검증·만료 판정은 항목당 1회만(지연 실행으로 재진입할 때는 건너뛴다).
    if (!s_execDeferred) {
        long expA = jNum(body, "expires_at");
        LOGI(log, "[CMD] 수신 id=%s type=%s exp=%ld (큐 %u건)\n",
             cmdId.c_str(), type.c_str(), expA, (unsigned)s_qCount);

        if (cmdId.isEmpty() || type.isEmpty()) {
            LOGW(log, "[CMD] 파싱 실패 — 드롭(ack 없음)\n");
            qPop();
            return;
        }

        uint32_t now = nowEpoch(modem);
        if (now > 0 && expA > 0 && now > (uint32_t)expA) {   // uint 비교(32bit signed 오버플로우 회피)
            LOGW(log, "[CMD] 만료됨 — 미실행\n");
            qPop();
            sendAck(modem, log, cmdId, "expired_on_device");
            return;
        }

#if FEATURE_OTA
        if (type == "ota_start") {
            // F2 — 다운로드 시작 **전에** 수신 사실을 알린다. ota_start 는
            // 수신 → 다운로드(실측 2분+) → 재부팅 → ack 라 그 구간이 통째로 깜깜하고,
            // 서버는 단말 ack 로만 delivered 를 올리므로(브로커 publish 성공은 증거가
            // 아니다) 이 한 건이 OTA 진행 가시성의 전부다.
            //
            // 발행 직후 Ota::start 로 들어가면 CMQTTSTOP 이 아직 안 빠진 +CMQTTPUB URC 와
            // 겹친다 → 큐에서 빼지 않은 채 실행만 미룬다(§5 해법 1).
            sendAck(modem, log, cmdId, "received");
            s_execDeferred = true;
            s_execAt = millis() + CMD_DEFER_EXEC_MS;
            return;
        }
#endif
    }

    if (type == "door_lock") {
#if FEATURE_CARKEY
        // press()는 논블로킹이라 ack 는 "누름을 시작했다"는 뜻이다(유지 종료를 기다리지 않음).
        // 다른 누름이 진행 중이면 거부되므로 그때는 failed 로 정직하게 알린다.
        sendAck(modem, log, cmdId, Carkey::lock() ? "done" : "failed");
#else
        sendAck(modem, log, cmdId, "failed");
#endif
    } else if (type == "door_unlock") {
#if FEATURE_CARKEY
        sendAck(modem, log, cmdId, Carkey::unlock() ? "done" : "failed");
#else
        sendAck(modem, log, cmdId, "failed");
#endif
    } else if (type == "ota_start") {
#if FEATURE_OTA
        String url = jStr(body, "url");
        String md5 = jStr(body, "md5");
        String ver = jStr(body, "version");
        // ⚠️ 실행 전에 큐에서 뺀다. Ota::start 는 성공 시 재부팅해 돌아오지 않으므로,
        //    나중에 빼는 코드는 실패 경로에서만 도는 셈이 된다(재부팅 시엔 RAM 큐가
        //    통째로 사라져 무해하지만, 순서를 규칙으로 못 박아 두는 편이 안전하다).
        qPop();
        // Ota::start는 성공 시 재부팅(반환 안 함), 실패 시 NVS에 failed 기록 후 false.
        // ⚠️ 어느 경우든 결과 ack는 여기서 발행하지 않는다 — MQTT (재)접속 후 loop의
        //    Ota::flushPendingAck()가 발행(다운로드 중 MQTT 단절로 즉시 ack 유실 방지).
        Ota::start(modem, log, cmdId, url, md5, ver);
        return;
#else
        sendAck(modem, log, cmdId, "failed");
#endif
    } else if (type == "config_update") {
        // 알려진 키(telemetry_interval_ms 즉시 / keepalive_s 다음 접속) 반영. ≥1이면 done.
        int applied = Cfg::applyUpdate(body, log);
        sendAck(modem, log, cmdId, applied > 0 ? "done" : "failed");
    } else {
        // remote_start(예비 IO 없음) 등 미지원.
        LOGW(log, "[CMD] 미지원 type=%s\n", type.c_str());
        sendAck(modem, log, cmdId, "failed");
    }

    // 실행 완료 — 큐에서 제거하고 지연 상태를 내린다. 다음 건은 다음 틱에서,
    // 그것도 PUBLISH_MIN_GAP_MS 가 지난 뒤에 처리된다(위 가드).
    qPop();
}

} // namespace Cmd
