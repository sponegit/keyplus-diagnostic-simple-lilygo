/**
 * @file      lte.cpp
 * @brief     증분 A LTE 브링업 구현 — Network/HttpClient 예제의 검증된 흐름을 모듈화.
 *
 * 흐름: setNetworkAPN → getRegistrationStatus 폴링(타임아웃) → gprsConnect(PDP)
 *       → TinyGsmClient + ArduinoHttpClient 로 평문 GET 검증.
 */
#include "lte.h"
#include "config.h"
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

bool begin(TinyGsm &modem, Stream &log)
{
    // APN을 등록 전에 지정 — 일부 통신사는 APN이 없으면 등록을 거부한다.
    log.print("[LTE] set APN: ");
    log.println(LTE_APN);
    if (!modem.setNetworkAPN(LTE_APN)) {
        log.println("[LTE] setNetworkAPN failed (계속 시도)");
    }

    // 망 등록 폴링. 등록 거부(REG_DENIED)는 대개 APN/요금제 문제 → 즉시 실패.
    log.println("[LTE] waiting for network registration...");
    uint32_t start = millis();
    RegStatus status = REG_NO_RESULT;
    while (true) {
        status = modem.getRegistrationStatus();
        if (status == REG_OK_HOME || status == REG_OK_ROAMING) {
            break;
        }
        if (status == REG_DENIED) {
            log.println("[LTE] registration DENIED — APN/요금제/잔액 확인 필요");
            return false;
        }
        if (millis() - start > LTE_REG_TIMEOUT_MS) {
            log.printf("[LTE] registration timeout (%lus) — 마지막 상태: %s\n",
                       LTE_REG_TIMEOUT_MS / 1000, regStr(status));
            return false;
        }
        // 검색 중에는 신호 세기를 함께 찍어 안테나/수신 문제를 조기 진단.
        log.printf("[LTE] [%lus] reg=%s rssi=%d\n",
                   (unsigned long)((millis() - start) / 1000),
                   regStr(status), modem.getSignalQuality());
        delay(1500);
    }
    log.printf("[LTE] registered (%s)\n", regStr(status));

    // PDP(gprs) 컨텍스트 활성 — TinyGsmClient TCP가 사용할 데이터 세션.
    log.print("[LTE] activating data context (PDP)...");
    if (!modem.gprsConnect(LTE_APN, LTE_GPRS_USER, LTE_GPRS_PASS)) {
        log.println(" fail");
        return false;
    }
    log.println(" ok");

    if (!modem.isGprsConnected()) {
        log.println("[LTE] gprs not connected after activate");
        return false;
    }

    // 모뎀 시계 UTC(TZ=0) NTP 동기 — cmd 만료판정/ack ts, telemetry ts용.
    // LG U+가 NITZ 시각을 안 줘서 CCLK가 기본값(1970→+2000=2070)이 되는 문제 보정.
    // 데이터패스(PDP) 위에서 best-effort(실패해도 브링업은 성공 처리).
    modem.NTPServerSync("pool.ntp.org", 0);
    log.println("[LTE] NTP 시계 동기 시도(UTC)");

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
    log.printf("[LTE] reg=%d data=%d rssi=%d oper=%s ip=%s\n",
               s.registered, s.dataUp, s.rssi,
               s.oper.length() ? s.oper.c_str() : "-",
               s.ip.length()   ? s.ip.c_str()   : "-");
}

int httpGetCheck(TinyGsm &modem, Stream &log)
{
    // 데이터 세션 위에 TCP 소켓 → 평문 HTTP GET. 인터넷 실도달 확인.
    TinyGsmClient client(modem);
    HttpClient    http(client, LTE_TEST_HOST, LTE_TEST_PORT);

    log.printf("[LTE] HTTP GET http://%s:%d%s ... ",
               LTE_TEST_HOST, LTE_TEST_PORT, LTE_TEST_PATH);

    int err = http.get(LTE_TEST_PATH);
    if (err != 0) {
        log.printf("connect failed (err=%d)\n", err);
        http.stop();
        return err < 0 ? err : -1000;   // 접속 단계 실패는 음수로 통일
    }

    int statusCode = http.responseStatusCode();
    int len        = http.contentLength();
    log.printf("status=%d, contentLength=%d\n", statusCode, len);

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
