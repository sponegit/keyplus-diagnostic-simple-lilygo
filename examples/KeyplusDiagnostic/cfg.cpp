/**
 * @file      cfg.cpp
 * @brief     config_update 런타임 설정 구현 (NVS "cfg").
 */
#include "cfg.h"
#include "config.h"
#include <Preferences.h>
#include <stdlib.h>
#include <string.h>

namespace Cfg {

static const char *kNs = "cfg";

static uint32_t s_teleMs   = CFG_DEFAULT_TELE_MS;
static int      s_keepaliveS = CFG_DEFAULT_KEEPALIVE_S;

// --- 간이 JSON 숫자 추출 ("key": num). cmd.cpp jNum과 동일 규약. -----------
// -1 반환 = 키 없음(부재와 값 0 구분용).
static long jNumOr(const char *body, const char *key, long missing)
{
    String pat = String("\"") + key + "\"";
    const char *p = strstr(body, pat.c_str());
    if (!p) return missing;
    p = strchr(p + pat.length(), ':');
    if (!p) return missing;
    return atol(p + 1);
}

void begin()
{
    Preferences p;
    p.begin(kNs, /*readOnly=*/true);
    s_teleMs     = p.getULong("tele_ms", CFG_DEFAULT_TELE_MS);
    s_keepaliveS = (int)p.getInt("ka_s", CFG_DEFAULT_KEEPALIVE_S);
    p.end();
}

uint32_t telemetryIntervalMs() { return s_teleMs; }
int      keepaliveS()          { return s_keepaliveS; }

int applyUpdate(const char *jsonBody, Stream &log)
{
    int applied = 0;
    Preferences p;
    p.begin(kNs, /*readOnly=*/false);

    long tele = jNumOr(jsonBody, "telemetry_interval_ms", -1);
    if (tele >= 0) {
        if (tele >= (long)CFG_TELE_MS_MIN && tele <= (long)CFG_TELE_MS_MAX) {
            s_teleMs = (uint32_t)tele;              // 즉시 적용
            p.putULong("tele_ms", s_teleMs);
            log.printf("[CFG] telemetry_interval_ms=%lu (즉시 적용)\n", (unsigned long)s_teleMs);
            applied++;
        } else {
            log.printf("[CFG] telemetry_interval_ms=%ld 범위밖(%lu~%lu) — 무시\n",
                       tele, (unsigned long)CFG_TELE_MS_MIN, (unsigned long)CFG_TELE_MS_MAX);
        }
    }

    long ka = jNumOr(jsonBody, "keepalive_s", -1);
    if (ka >= 0) {
        if (ka >= CFG_KEEPALIVE_S_MIN && ka <= CFG_KEEPALIVE_S_MAX) {
            s_keepaliveS = (int)ka;                 // 다음 접속 반영
            p.putInt("ka_s", s_keepaliveS);
            log.printf("[CFG] keepalive_s=%d (다음 MQTT 접속 반영)\n", s_keepaliveS);
            applied++;
        } else {
            log.printf("[CFG] keepalive_s=%ld 범위밖(%d~%d) — 무시\n",
                       ka, CFG_KEEPALIVE_S_MIN, CFG_KEEPALIVE_S_MAX);
        }
    }

    p.end();
    return applied;
}

} // namespace Cfg
