/**
 * @file      cmd.cpp
 * @brief     MQTT cmd 다운링크 구현 — 수신 콜백(복사만) + loop 처리(실행/ack).
 */
#include "cmd.h"
#include "config.h"
#include "provisioning.h"
#include "carkey.h"
#include <string.h>
#include <stdlib.h>

namespace Cmd {

static const uint8_t kClientIdx = 0;

// --- 콜백 수신 버퍼 (mqtt_handle 내부에서 채워짐: 복사 + 플래그만) --------------
static volatile bool s_pending = false;
static char     s_payload[256];   // 명령 JSON(작음). null 종단.
static String   s_cmdTopic;       // v1/{id}/cmd
static String   s_ackTopic;       // v1/{id}/cmd/ack

// 단일 cmd 토픽만 구독하므로 topic은 무시하고 payload만 사용.
static void onMessage(const char * /*topic*/, const uint8_t *payload, uint32_t len)
{
    if (s_pending) return;   // 직전 명령 미처리 시 드롭(단순화)
    uint32_t n = (len < sizeof(s_payload) - 1) ? len : sizeof(s_payload) - 1;
    memcpy(s_payload, payload, n);
    s_payload[n] = '\0';
    s_pending = true;
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
    if (y < 2020) return 0;
    uint32_t e = civilToEpoch(y, mo, d, h, mi, s);
    // CCLK 시각은 로컬(tz, 15분 단위) → UTC = local - tz*15min.
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
    s_ackTopic = "v1/" + Prov::deviceId() + "/cmd/ack";
    bool ok = modem.mqtt_subscribe(kClientIdx, s_cmdTopic.c_str(), /*qos=*/1);
    log.printf("[CMD] subscribe %s -> %s\n", s_cmdTopic.c_str(), ok ? "ok" : "FAIL");
}

void handle(TinyGsm &modem, Stream &log)
{
    if (!s_pending) return;
    s_pending = false;

    String cmdId = jStr(s_payload, "command_id");
    String type  = jStr(s_payload, "type");
    long   expA  = jNum(s_payload, "expires_at");
    log.printf("[CMD] recv id=%s type=%s exp=%ld\n", cmdId.c_str(), type.c_str(), expA);

    if (cmdId.isEmpty() || type.isEmpty()) {
        log.println("[CMD] 파싱 실패 — 드롭(ack 없음)");
        return;
    }

    uint32_t now = nowEpoch(modem);
    const char *result;

    if (now > 0 && expA > 0 && (long)now > expA) {
        result = "expired_on_device";     // 수신 시 이미 만료 → 미실행
        log.println("[CMD] 만료됨 — 미실행");
    } else if (type == "door_lock") {
#if FEATURE_CARKEY
        Carkey::lock();  result = "done";
#else
        result = "failed";
#endif
    } else if (type == "door_unlock") {
#if FEATURE_CARKEY
        Carkey::unlock(); result = "done";
#else
        result = "failed";
#endif
    } else {
        // remote_start / config_update / ota_start 미구현
        result = "failed";
        log.printf("[CMD] 미지원 type=%s\n", type.c_str());
    }

    char ack[160];
    int n = snprintf(ack, sizeof(ack),
        "{\"command_id\":\"%s\",\"result\":\"%s\",\"ts\":%u}",
        cmdId.c_str(), result, (unsigned)now);
    bool ok = (n > 0 && n < (int)sizeof(ack)) &&
              modem.mqtt_publish(kClientIdx, s_ackTopic.c_str(), ack, /*qos=*/1);
    log.printf("[CMD] ack id=%s result=%s %s\n", cmdId.c_str(), result,
               ok ? "published" : "FAILED");
}

} // namespace Cmd
