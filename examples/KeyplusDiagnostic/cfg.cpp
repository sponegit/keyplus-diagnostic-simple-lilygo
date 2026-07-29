/**
 * @file      cfg.cpp
 * @brief     config_update 런타임 설정 구현 (NVS "cfg").
 */
#include "cfg.h"
#include "config.h"
#include "log.h"
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

    // NVS 값은 상한이 더 느슨했던 이전 펌웨어가 써놨을 수 있다(예: tele_ms=600000).
    // 그대로 쓰면 부팅하자마자 발행→NAT드롭→재접속 루프에 빠지므로 로드 시점에 클램프한다.
    // 여기서 고친 값은 다음 config_update가 오기 전까지 메모리에만 반영된다(NVS는 그대로).
    if (s_teleMs < CFG_TELE_MS_MIN || s_teleMs > CFG_TELE_MS_MAX) {
        s_teleMs = CFG_DEFAULT_TELE_MS;
    }
    if (s_keepaliveS < CFG_KEEPALIVE_S_MIN || s_keepaliveS > CFG_KEEPALIVE_S_MAX) {
        s_keepaliveS = CFG_DEFAULT_KEEPALIVE_S;
    }
    if ((uint32_t)s_keepaliveS * 1000UL < s_teleMs * CFG_KEEPALIVE_TELE_RATIO) {
        s_keepaliveS = CFG_DEFAULT_KEEPALIVE_S;
    }
}

uint32_t telemetryIntervalMs() { return s_teleMs; }
int      keepaliveS()          { return s_keepaliveS; }

int applyUpdate(const char *jsonBody, Stream &log)
{
    // 두 값이 한 번에 올 수 있으므로 후보값을 먼저 모은 뒤 개별 범위검사 → 쌍 교차검증 →
    // 커밋 순으로 처리한다. 필드별로 즉시 커밋하면 "telemetry만 늘리는" 명령이 keepalive와의
    // 관계를 깨뜨린 채로 반영된다.
    uint32_t teleMs  = s_teleMs;
    int      kaS     = s_keepaliveS;
    bool     teleReq = false, kaReq = false;

    long tele = jNumOr(jsonBody, "telemetry_interval_ms", -1);
    if (tele >= 0) {
        if (tele >= (long)CFG_TELE_MS_MIN && tele <= (long)CFG_TELE_MS_MAX) {
            teleMs = (uint32_t)tele;
            teleReq = true;
        } else {
            LOGW(log, "[CFG] telemetry_interval_ms=%ld 범위밖(%lu~%lu) — 무시\n",
                       tele, (unsigned long)CFG_TELE_MS_MIN, (unsigned long)CFG_TELE_MS_MAX);
            if (tele > (long)CFG_TELE_MS_MAX) {
                // 상한이 자의적 값이 아니라 물리 제약이라는 걸 로그로 남긴다.
                LOGW(log, "[CFG]   ⚠️ publish가 유일한 연결유지 수단(모뎀 PINGREQ 미발송) — "
                           "캐리어 NAT 유휴 드롭 %lums 보다 짧아야 함\n",
                           (unsigned long)MQTT_NAT_IDLE_TIMEOUT_MS);
            }
        }
    }

    long ka = jNumOr(jsonBody, "keepalive_s", -1);
    if (ka >= 0) {
        if (ka >= CFG_KEEPALIVE_S_MIN && ka <= CFG_KEEPALIVE_S_MAX) {
            kaS = (int)ka;
            kaReq = true;
        } else {
            LOGW(log, "[CFG] keepalive_s=%ld 범위밖(%d~%d) — 무시\n",
                       ka, CFG_KEEPALIVE_S_MIN, CFG_KEEPALIVE_S_MAX);
        }
    }

    if (!teleReq && !kaReq) return 0;

    // 교차검증: keepalive가 발행 주기 대비 너무 짧으면 publish 사이 유휴 동안 브로커가
    // 먼저 세션을 끊어(MQTT 규격상 keepalive의 1.5배) 재접속 루프가 된다. 한쪽만 바꾸는
    // 명령도 결과 조합으로 검사하므로, 부분 반영으로 관계가 깨지는 일이 없다.
    if ((uint32_t)kaS * 1000UL < teleMs * CFG_KEEPALIVE_TELE_RATIO) {
        LOGW(log, "[CFG] 조합 거부 — keepalive_s=%d 는 telemetry %lums 의 %d배 미만\n",
                   kaS, (unsigned long)teleMs, CFG_KEEPALIVE_TELE_RATIO);
        return 0;
    }

    int applied = 0;
    Preferences p;
    p.begin(kNs, /*readOnly=*/false);
    if (teleReq) {
        s_teleMs = teleMs;                          // 즉시 적용
        p.putULong("tele_ms", s_teleMs);
        LOGI(log, "[CFG] telemetry_interval_ms=%lu (즉시 적용)\n", (unsigned long)s_teleMs);
        applied++;
    }
    if (kaReq) {
        s_keepaliveS = kaS;                         // 다음 접속 반영
        p.putInt("ka_s", s_keepaliveS);
        LOGI(log, "[CFG] keepalive_s=%d (다음 MQTT 접속 반영)\n", s_keepaliveS);
        applied++;
    }
    p.end();
    return applied;
}

} // namespace Cfg
