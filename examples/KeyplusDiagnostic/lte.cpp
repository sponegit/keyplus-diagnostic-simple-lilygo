/**
 * @file      lte.cpp
 * @brief     증분 A LTE 브링업 구현 — Network/HttpClient 예제의 검증된 흐름을 모듈화.
 *
 * 흐름: setNetworkAPN → getRegistrationStatus 폴링(타임아웃) → gprsConnect(PDP)
 *       → TinyGsmClient + ArduinoHttpClient 로 평문 GET 검증.
 */
#include "lte.h"
#include "config.h"
#include "log.h"
#include <ArduinoHttpClient.h>

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

bool modemAlive(TinyGsm &modem)
{
    return modem.testAT(LTE_AT_PROBE_MS);
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

bool begin(TinyGsm &modem, Stream &log)
{
    // 모뎀이 죽어 있으면 등록 폴링(최대 LTE_REG_TIMEOUT_MS)이 통째로 낭비다.
    // 전원 부족으로 모뎀이 내부 리셋된 상태가 정확히 이 경우 — 90초를 태우는 대신
    // 즉시 실패시켜 호출측이 리셋 경로를 타게 한다.
    if (!modemAlive(modem)) {
        LOGE(log, "[LTE] 모뎀 무응답 — 등록 시도 생략(리셋 필요, 전원 마진 확인)\n");
        return false;
    }

    // APN을 등록 전에 지정 — 일부 통신사는 APN이 없으면 등록을 거부한다.
    LOGD(log, "[LTE] set APN: %s\n", LTE_APN);
    if (!modem.setNetworkAPN(LTE_APN)) {
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
            return false;
        }
        if (millis() - start > LTE_REG_TIMEOUT_MS) {
            LOGE(log, "[LTE] 등록 타임아웃(%lus) — 마지막 상태 %s\n",
                 LTE_REG_TIMEOUT_MS / 1000, regStr(status));
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
    LOGD(log, "[LTE] PDP 활성 시도...\n");
    if (!modem.gprsConnect(LTE_APN, LTE_GPRS_USER, LTE_GPRS_PASS)) {
        LOGE(log, "[LTE] PDP 활성 실패\n");
        return false;
    }

    if (!modem.isGprsConnected()) {
        LOGE(log, "[LTE] PDP 활성 직후 gprs 미연결\n");
        return false;
    }

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
