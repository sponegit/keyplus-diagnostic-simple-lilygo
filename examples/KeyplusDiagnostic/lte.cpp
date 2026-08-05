/**
 * @file      lte.cpp
 * @brief     증분 A LTE 브링업 구현 — Network/HttpClient 예제의 검증된 흐름을 모듈화.
 *
 * 흐름: Apn::select → setNetworkAPN → getRegistrationStatus 폴링(타임아웃)
 *       → gprsConnect(PDP, 실패 시 APN 폴백 순회) → TinyGsmClient + ArduinoHttpClient
 *       로 평문 GET 검증.
 */
#include "lte.h"
#include "config.h"
#include "log.h"
#include "apn.h"
#include <ArduinoHttpClient.h>
#include <string.h>

namespace Lte {

// RegStatus → 사람이 읽는 문자열 (로깅용).
static const char *regStr(RegStatus s)
{
    switch (s) {
    case REG_NO_RESULT:    return "no-result";
    case REG_UNREGISTERED: return "unregistered";
    case REG_SEARCHING:    return "searching";
    case REG_DENIED:       return "DENIED";
    case REG_OK_HOME:      return "ok-home";
    case REG_OK_ROAMING:   return "ok-roaming";
    case REG_SMS_ONLY:     return "sms-only";
    default:               return "unknown";
    }
}

bool modemAlive(TinyGsm &modem, uint32_t timeoutMs)
{
    return modem.testAT(timeoutMs);
}

bool softReset(TinyGsm &modem, Stream &log)
{
    LOGW(log, "[LTE] 모뎀 소프트 리셋(AT+CFUN=1,1)\n");
    modem.sendAT("+CFUN=1,1");
    modem.waitResponse(10000UL);

    // 모뎀이 재부팅된다 — AT가 다시 뜰 때까지 제한 시간만 기다린다.
    uint32_t start = millis();
    while (millis() - start < LTE_MODEM_RESET_WAIT_MS) {
        if (modem.testAT(1000)) {
            LOGI(log, "[LTE] 모뎀 리셋 완료 (%lus)\n",
                 (unsigned long)((millis() - start) / 1000UL));
            return true;
        }
        delay(500);
    }
    LOGE(log, "[LTE] 소프트 리셋 후에도 무응답 — 하드 리셋 필요\n");
    return false;
}

// SIM 인식 확인. 유심이 없으면(+CPIN: SIM REMOVED / NOT INSERTED) 등록은 반드시 실패하므로
// 90초 폴링을 태우기 전에 여기서 끊는다. 부팅 직후 CPIN 지연은 재시도로 흡수한다.
static bool simReady(TinyGsm &modem, Stream &log)
{
    SimStatus st = SIM_ERROR;
    for (int i = 1; i <= LTE_SIM_PROBE_TRIES; ++i) {
        st = modem.getSimStatus(LTE_SIM_PROBE_MS);
        if (st == SIM_READY) return true;
        LOGD(log, "[LTE] SIM 확인 %d/%d — status=%d\n", i, LTE_SIM_PROBE_TRIES, (int)st);
        delay(1000);
    }
    if (st == SIM_LOCKED) {
        LOGE(log, "[LTE] SIM 잠김(PIN) — simUnlock 필요. 등록 시도 생략\n");
    } else {
        LOGE(log, "[LTE] SIM 미인식 — 유심 삽입/접촉 확인. 등록 시도 생략\n");
    }
    return false;
}

int modemTempC(TinyGsm &modem)
{
    // TinyGSM 의 getTemperature() = AT+CPMUTEMP. 모뎀 PMU 다이 온도(주변 온도 아님).
    // 실패(무응답/미지원) 시 래퍼가 0 을 돌려주므로 그대로 "미상"으로 흘린다.
    return (int)modem.getTemperature();
}

// CNMP 값 → 사람이 읽는 문자열 (로깅용).
static const char *cnmpStr(int m)
{
    switch (m) {
    case 2:  return "자동";
    case 13: return "GSM 전용";
    case 38: return "LTE 전용";
    case 51: return "GSM+LTE";
    default: return "기타";
    }
}

bool applyNetworkMode(TinyGsm &modem, Stream &log)
{
    if (LTE_CNMP_MODE == 0) return true;   // 건드리지 않음(설정으로 끔)

    // ⚠️ 먼저 조회한다. CNMP 쓰기는 망 재탐색을 유발해 등록을 늦추므로, 이미 원하는
    //    값이면 쓰지 않는다. 값은 모뎀 NV 에 남아 재부팅해도 유지된다 — 그래서 브링업
    //    때마다 불러도 실제 쓰기는 값이 달라진 직후 한 번뿐이다.
    String res;
    modem.sendAT("+CNMP?");
    if (modem.waitResponse(2000L, res) == 1) {
        int i = res.indexOf("+CNMP:");
        if (i >= 0 && res.substring(i + 6).toInt() == LTE_CNMP_MODE) return true;
    }

    modem.sendAT("+CNMP=", LTE_CNMP_MODE);
    bool ok = (modem.waitResponse(10000L) == 1);
    // 실패해도 브링업은 계속한다 — 모뎀에 남아 있는 값으로도 등록 자체는 된다.
    LOGI(log, "[LTE] 접속모드 CNMP=%d(%s) %s\n", LTE_CNMP_MODE, cnmpStr(LTE_CNMP_MODE),
         ok ? "적용됨" : "적용 실패 — 기존 값으로 계속");
    return ok;
}

// PDP(gprs) 활성 1회 시도. gprsConnect 가 내부에서 gprsDisconnect → CGDCONT 재설정까지
// 하므로, 다른 APN 으로 다시 부르는 것만으로 재시도가 성립한다(재등록 불필요).
static bool pdpTry(TinyGsm &modem, const char *apn, Stream &log)
{
    LOGD(log, "[LTE] PDP 활성 시도 (apn=%s)...\n", apn);
    if (!modem.gprsConnect(apn, LTE_GPRS_USER, LTE_GPRS_PASS)) return false;
    if (!modem.isGprsConnected()) {
        LOGD(log, "[LTE] PDP 활성 직후 gprs 미연결 (apn=%s)\n", apn);
        return false;
    }
    return true;
}

// 1순위 APN 으로 PDP 가 안 붙었을 때의 폴백. 성공한 APN 을 out 에 담고 true.
//
// 순서: ① 망이 attach 때 내려준 APN(CGDCONT) — 표가 틀렸을 때 이게 정답인 경우가 많다
//       ② 나머지 후보(LG U+/SKT/KT) 순회
// ⚠️ 등록은 이미 끝났다 — 여기서 다시 90초 등록 폴링을 태우지 않는다. 통신사가 갈리는
//    지점은 APN 뿐이라 PDP 만 돌리면 된다.
// ⚠️ 총 소요를 LTE_APN_SWEEP_BUDGET_MS 로 묶는다. 예산이 다하면 남은 후보는 다음
//    브링업에서 이어서 본다(무한정 붙잡고 있으면 재시도 백오프가 통째로 밀린다).
static bool pdpSweep(TinyGsm &modem, const char *tried, String &out, Stream &log)
{
    uint32_t start = millis();
    String   netApn = Apn::networkApn(modem);

    if (netApn.length() && netApn != tried) {
        LOGI(log, "[LTE] APN 폴백 — 망 할당값 시도: %s\n", netApn.c_str());
        if (pdpTry(modem, netApn.c_str(), log)) { out = netApn; return true; }
    }

    for (int i = 0; i < Apn::candidateCount(); ++i) {
        const char *c = Apn::candidate(i);
        if (!c || !*c) continue;
        if (strcmp(c, tried) == 0)   continue;          // 1순위로 이미 해봤다
        if (netApn == c)             continue;          // 바로 위에서 해봤다
        if (millis() - start > LTE_APN_SWEEP_BUDGET_MS) {
            LOGW(log, "[LTE] APN 순회 예산(%lus) 소진 — 남은 후보는 다음 시도에서\n",
                 (unsigned long)(LTE_APN_SWEEP_BUDGET_MS / 1000UL));
            break;
        }
        LOGI(log, "[LTE] APN 폴백 — 후보 시도: %s\n", c);
        if (pdpTry(modem, c, log)) { out = c; return true; }
    }

    // 최후: 빈 APN(CGDCONT 를 비워 망이 정한 기본 APN 을 쓰게 한다). 요금제 지정 APN 을
    // 모르는 상태에서 유일하게 남은 자동 수단이다.
    // ⚠️ 성공해도 저장하지 않는다(빈 값은 "캐시 없음"과 구분되지 않는다) — 매 브링업마다
    //    여기까지 오게 되므로, 붙었다면 콘솔에서 실제 APN 을 확인해 'apn set' 으로 굳힐 것.
    if (millis() - start <= LTE_APN_SWEEP_BUDGET_MS) {
        LOGI(log, "[LTE] APN 폴백 — 빈 APN(망 기본값) 시도\n");
        if (pdpTry(modem, "", log)) {
            out = "";
            LOGW(log, "[LTE] 빈 APN 으로 접속됨 — 'at+cgdcont?' 로 실제 APN 확인 후 "
                      "'apn set <값>' 으로 고정하면 이후 브링업이 빨라진다\n");
            return true;
        }
    }
    return false;
}

bool begin(TinyGsm &modem, Stream &log)
{
    // 모뎀이 죽어 있으면 등록 폴링(최대 LTE_REG_TIMEOUT_MS)이 통째로 낭비다.
    // 전원 부족으로 모뎀이 내부 리셋된 상태가 정확히 이 경우 — 90초를 태우는 대신
    // 즉시 실패시켜 호출측이 리셋 경로를 타게 한다.
    if (!modemAlive(modem)) {
        LOGE(log, "[LTE] 모뎀 무응답 — 등록 시도 생략(리셋 필요, 전원 마진 확인)\n");
        return false;
    }

    // 유심이 없으면(또는 PIN 잠김) 등록 폴링은 90초를 버리고 실패할 뿐이다 — 조기 차단.
    if (!simReady(modem, log)) return false;

    // 접속 기술 적용 — 등록 폴링 전에. 값이 이미 맞으면 AT 1왕복으로 끝난다.
    applyNetworkMode(modem, log);

    // 이번 브링업에 쓸 APN 결정 — 유심(ICCID/IMSI)을 보고 3사 중에서 고른다(apn.h).
    // ⚠️ SIM 확인 뒤에 부른다(SIM_READY 여야 CIMI/CICCID 가 나온다).
    String apn = Apn::select(modem, log);

    // APN을 등록 전에 지정 — 일부 통신사는 APN이 없으면 등록을 거부한다.
    LOGD(log, "[LTE] set APN: %s\n", apn.c_str());
    if (!modem.setNetworkAPN(apn.c_str())) {
        LOGW(log, "[LTE] setNetworkAPN 실패 (계속 시도)\n");
    }

    // 망 등록 폴링. 등록 거부(REG_DENIED)는 대개 APN/요금제 문제 → 즉시 실패.
    LOGI(log, "[LTE] 망 등록 대기...\n");
    uint32_t start = millis();
    RegStatus status = REG_NO_RESULT;
    while (true) {
        status = modem.getRegistrationStatus();
        if (status == REG_OK_HOME || status == REG_OK_ROAMING) {
            break;
        }
        if (status == REG_DENIED) {
            LOGE(log, "[LTE] 등록 거부(DENIED) — APN/요금제/잔액 확인 필요\n");
            Apn::noteBringupFailed();   // 통신사 미확정이면 다음 시도는 다른 후보로
            return false;
        }
        if (millis() - start > LTE_REG_TIMEOUT_MS) {
            LOGE(log, "[LTE] 등록 타임아웃(%lus) — 마지막 상태 %s\n",
                 LTE_REG_TIMEOUT_MS / 1000, regStr(status));
            Apn::noteBringupFailed();
            return false;
        }
        // 검색 중에는 신호 세기를 함께 찍어 안테나/수신 문제를 조기 진단.
        // 검색 경과는 폴마다 나오므로 DEBUG. 결과(등록/거부/타임아웃)는 위에서 남긴다.
        LOGD(log, "[LTE] [%lus] reg=%s rssi=%d\n",
             (unsigned long)((millis() - start) / 1000),
             regStr(status), modem.getSignalQuality());
        delay(1500);
    }
    LOGI(log, "[LTE] 망 등록됨 (%s, rssi=%d)\n", regStr(status), modem.getSignalQuality());

    // PDP(gprs) 컨텍스트 활성 — TinyGsmClient TCP가 사용할 데이터 세션.
    // 여기가 통신사가 실제로 갈리는 유일한 지점이다: 등록은 유심의 홈망으로 자동이지만,
    // APN 이 틀리면 딱 이 단계에서 떨어진다. → 실패하면 폴백으로 다른 APN 을 훑는다.
    if (!pdpTry(modem, apn.c_str(), log)) {
        String worked;
        if (!pdpSweep(modem, apn.c_str(), worked, log)) {
            LOGE(log, "[LTE] PDP 활성 실패 — 모든 후보 APN 실패(현재 '%s')\n", apn.c_str());
            LOGE(log, "[LTE] 요금제 지정 APN 이면 콘솔 'apn set <값>' 후 재부팅\n");
            Apn::noteBringupFailed();
            return false;
        }
        apn = worked;
    }
    // 붙은 APN 을 NVS 에 굳힌다 — 다음 부팅부터는 첫 시도에 바로 붙는다.
    // (값이 그대로면 쓰지 않는다 — 재접속마다 플래시를 쓰지 않기 위해)
    Apn::commit(apn.c_str(), log);

    // 모뎀 시계 UTC(TZ=0) NTP 동기 — cmd 만료판정/ack ts, telemetry ts용.
    // LG U+가 NITZ 시각을 안 줘서 CCLK가 기본값(1970→+2000=2070)이 되는 문제 보정.
    // 데이터패스(PDP) 위에서 best-effort(실패해도 브링업은 성공 처리).
    modem.NTPServerSync("pool.ntp.org", 0);
    LOGD(log, "[LTE] NTP 시계 동기 시도(UTC)\n");

    return true;
}

bool isUp(TinyGsm &modem)
{
    return modem.isNetworkConnected() && modem.isGprsConnected();
}

void status(TinyGsm &modem, LteStatus &out)
{
    RegStatus s   = modem.getRegistrationStatus();
    out.registered = (s == REG_OK_HOME || s == REG_OK_ROAMING);
    out.dataUp     = modem.isGprsConnected();
    out.rssi       = modem.getSignalQuality();
    out.oper       = modem.getOperator();
    out.ip         = modem.getLocalIP();
}

void printStatus(const LteStatus &s, Stream &log)
{
    LOGD(log, "[LTE] reg=%d data=%d rssi=%d oper=%s ip=%s\n",
         s.registered, s.dataUp, s.rssi,
         s.oper.length() ? s.oper.c_str() : "-",
         s.ip.length()   ? s.ip.c_str()   : "-");
}

int httpGetCheck(TinyGsm &modem, Stream &log)
{
    // 데이터 세션 위에 TCP 소켓 → 평문 HTTP GET. 인터넷 실도달 확인.
    TinyGsmClient client(modem);
    HttpClient    http(client, LTE_TEST_HOST, LTE_TEST_PORT);

    LOGD(log, "[LTE] HTTP GET http://%s:%d%s ...\n",
         LTE_TEST_HOST, LTE_TEST_PORT, LTE_TEST_PATH);

    int err = http.get(LTE_TEST_PATH);
    if (err != 0) {
        LOGW(log, "[LTE] 데이터패스 검증 connect 실패 (err=%d)\n", err);
        http.stop();
        return err < 0 ? err : -1000;   // 접속 단계 실패는 음수로 통일
    }

    int statusCode = http.responseStatusCode();
    int len        = http.contentLength();
    LOGD(log, "[LTE] 데이터패스 검증 status=%d contentLength=%d\n", statusCode, len);

    // 본문은 검증에 불필요하므로 폐기하고 소켓만 닫는다.
    http.responseBody();
    http.stop();
    return statusCode;
}

void end(TinyGsm &modem)
{
    modem.gprsDisconnect();
}

} // namespace Lte
