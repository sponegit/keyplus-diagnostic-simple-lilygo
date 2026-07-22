/**
 * @file      mqtt.cpp
 * @brief     증분 B MQTT/TLS + telemetry 구현.
 *
 * 흐름(MqttsBuiltlnAWS 예제 기준): mqtt_begin(ssl,sni) → mqtt_set_certificate(CA)
 *   → setWillMessage → mqtt_connect(id,user,pass,keepalive) → status online(retain)
 *   → publishTelemetry(QoS1) 주기 발행.
 */
#include "mqtt.h"
#include "config.h"
#include "certs.h"
#include "provisioning.h"
#include "cfg.h"

namespace Mqtt {

// 서버 CA 선택 (config.h MQTT_BROKER_SEL). TLS일 때만 사용.
#if MQTT_USE_TLS
#if   MQTT_BROKER_SEL == MQTT_BROKER_TEST
static const char *kCaPem = CA_MOSQUITTO_ORG;
#elif MQTT_BROKER_SEL == MQTT_BROKER_LOCAL
static const char *kCaPem = CA_LOCAL_EMQX;
#else
static const char *kCaPem = CA_ISRG_ROOT_X1;
#endif
#endif

// 계약 토픽/클라이언트ID — device_id가 런타임(NVS/콘솔)이라 접속 시 조립한다.
static String s_clientId;        // = device_id
static String s_topicStatus;     // v1/{device_id}/status
static String s_topicTelemetry;  // v1/{device_id}/telemetry

static void buildTopics()
{
    s_clientId      = Prov::deviceId();
    s_topicStatus    = "v1/" + s_clientId + "/status";
    s_topicTelemetry = "v1/" + s_clientId + "/telemetry";
}

static const uint8_t kClientIdx = 0;

// 접속 상태는 자체 추적한다. 래퍼 mqtt_connected()는 AT+CMQTTDISC? 폴링이라
// publish 직후의 +CMQTTPUB URC와 간섭해 오탐(false)을 내므로 쓰지 않는다.
static bool s_serviceStarted = false;   // mqtt_begin(CMQTTSTART) 완료 — 부팅당 1회만
static bool s_connected      = false;   // 세션 접속 여부

// 사용자/비번이 빈 문자열이면 NULL(익명)로 넘긴다.
static const char *orNull(const char *s) { return (s && s[0]) ? s : nullptr; }

// UTC 민간시각 → epoch(초). days_from_civil (Howard Hinnant).
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

// 모뎀 NITZ/NTP(CCLK) → UTC epoch. GPS fix 없을 때 telemetry ts 폴백용.
// 미동기/무효 시 0(그럼 서버가 수신시각 스탬프). cmd.cpp nowEpoch와 동일 규약.
static uint32_t modemEpoch(TinyGsm &modem)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    float tz = 0;
    if (!modem.getNetworkTime(&y, &mo, &d, &h, &mi, &s, &tz)) return 0;
    if (y < 2024 || y > 2060) return 0;         // 미동기 시 1970/2070 등 엉뚱값 방어
    uint32_t e = civilToEpoch(y, mo, d, h, mi, s);
    e -= (int32_t)tz * 15 * 60;                  // 로컬(tz 15분 단위) → UTC
    return e;
}

// 세션 (재)접속 — CMQTT 서비스(mqtt_begin)는 건드리지 않는다.
// 인증서 지정 → LWT → connect → status online(retained). 재접속도 이 경로만 탄다.
static bool connectSession(TinyGsm &modem, Stream &log)
{
    // 서버 CA 지정 → mqtt_connect가 AT+CCERTDOWN 업로드 + authMethod=1(서버검증).
    // 포인터 저장만 하므로 매 접속 재지정해도 무해. 평문(MQTT_USE_TLS=0)이면 생략.
#if MQTT_USE_TLS
    modem.mqtt_set_certificate(kCaPem);
  #if MQTT_TLS_RELAX
    // 로컬 자체서명 + IP 접속(cert CN=도메인, DNS 미해석) → CN·시각 검증 완화.
    // CA 체인 검증(authmode=1)은 유지 → 여전히 우리 CA로 서명된 서버만 신뢰.
    modem.sendAT("+CSSLCFG=\"ignorecertCN\",0,1");    modem.waitResponse();
    modem.sendAT("+CSSLCFG=\"ignorelocaltime\",0,1");  modem.waitResponse();
  #endif
#endif

    // LWT: 끊김 시 브로커가 {"online":false} 대행 발행 (connect 전에 등록).
    // s_topicStatus는 static String이라 c_str() 포인터가 세션 동안 안정적.
    static const char kWillOffline[] = "{\"online\":false}";
    modem.setWillMessage(s_topicStatus.c_str(), kWillOffline, 1);

    // 인증: 익명(MQTT_ANON=1, 테스트 브로커)이면 NULL, 아니면 device_id + NVS 비번(설계 01 §2).
#if MQTT_ANON
    const char *user = nullptr;
    const char *pass = nullptr;
#else
    const char *user = s_clientId.c_str();                 // username = device_id
    const char *pass = orNull(Prov::mqttPassword().c_str()); // 프로비저닝 발급 비번(NVS)
#endif

    // keepalive는 config_update로 런타임 변경 가능(Cfg). 접속 시점 값 반영.
    if (!modem.mqtt_connect(kClientIdx, MQTT_HOST, MQTT_PORT, s_clientId.c_str(),
                            user, pass, Cfg::keepaliveS())) {
        log.println("[MQTT] connect failed (CA 검증/인증/네트워크 확인)");
        s_connected = false;
        return false;
    }

    // 접속 알림: status online, retained.
    static const char kOnline[] = "{\"online\":true}";
    modem.mqtt_publish(kClientIdx, s_topicStatus.c_str(), kOnline, /*qos=*/1,
                       /*timeout=*/60, /*retain=*/1);
    s_connected = true;
    return true;
}

bool begin(TinyGsm &modem, Stream &log)
{
    // device_id(런타임)로 토픽/클라이언트ID 조립. 매 접속 갱신(값 동일하면 무해).
    buildTopics();

    // CMQTT 서비스(CMQTTSTART)는 부팅당 1회만. 이미 시작됐으면 세션 접속만 한다.
    // (매번 mqtt_begin 호출 시 CMQTTSTOP이 살아있는 연결을 끊고 재시작에 실패한다.)
    if (!s_serviceStarted) {
        log.printf("[MQTT] begin: %s, broker %s:%d, id=%s\n",
                   MQTT_USE_TLS ? "TLS+sni" : "PLAIN", MQTT_HOST, MQTT_PORT, s_clientId.c_str());
        if (!modem.mqtt_begin(MQTT_USE_TLS, /*sni=*/MQTT_USE_TLS)) {
            log.println("[MQTT] mqtt_begin failed");
            return false;
        }
        s_serviceStarted = true;
    }

    bool ok = connectSession(modem, log);
    if (ok) log.println("[MQTT] connected (TLS + server CA verified)");
    return ok;
}

// 자체 추적값 반환 (플래키한 AT+CMQTTDISC? 폴링 회피).
bool isConnected(TinyGsm & /*modem*/) { return s_connected; }

void handle(TinyGsm &modem) { modem.mqtt_handle(); }

bool ensure(TinyGsm &modem, Stream &log)
{
    if (s_connected) return true;
    return begin(modem, log);   // begin이 서비스 시작 여부를 알아서 가드
}

void stopService(TinyGsm &modem)
{
    // 세션 해제 + CMQTTSTOP(래퍼 mqtt_disconnect가 둘 다 수행). 서비스 플래그도 내려
    // 이후 재접속 시 mqtt_begin(CMQTTSTART)부터 다시 타게 한다(OTA 후엔 재부팅이라 무의미하나 안전).
    modem.mqtt_disconnect(kClientIdx);
    s_connected = false;
    s_serviceStarted = false;
}

bool publishTelemetry(TinyGsm &modem, const GpsFix &fix, const Obd2::Data &obd,
                      uint32_t seq, bool withMeta, Stream &log)
{
    // 네트워크 상태는 발행 시점에 신선하게 읽는다.
    int rssi = modem.getSignalQuality();
    int reg  = (int)modem.getRegistrationStatus();

    // ts: GPS UTC fix 우선, 없으면 모뎀 UTC 폴백, 그것도 없으면 0(서버가 수신시각 스탬프).
    // ⚠️ ts=0이 계속 오면 서버 PK (device_id,ts) 중복제거로 1행만 쌓임 → 모뎀 시각 폴백으로 회피.
    uint32_t ts = 0;
    if (fix.valid && fix.year >= 2020) {
        ts = civilToEpoch(fix.year, fix.month, fix.day,
                          fix.hour, fix.minute, fix.second);
    } else {
        ts = modemEpoch(modem);   // GPS fix 없어도(실내/음영) 실시각 확보 → 서버 누적 정상화
    }
    uint32_t up_s = millis() / 1000UL;

    // 계약 스키마(§3)대로 손 조립. gps는 fix일 때만 좌표 포함.
    char buf[640];
    int n = snprintf(buf, sizeof(buf),
        "{\"ts\":%u,\"seq\":%u,\"gps\":{\"fix\":%s",
        ts, seq, fix.valid ? "true" : "false");

    if (fix.valid) {
        n += snprintf(buf + n, sizeof(buf) - n,
            ",\"lat\":%.6f,\"lon\":%.6f,\"spd\":%.1f,\"sat\":%d",
            fix.lat, fix.lon, fix.speed, fix.vsat);
    }
    n += snprintf(buf + n, sizeof(buf) - n,
        "},\"net\":{\"rssi\":%d,\"reg\":%d},\"sys\":{\"up_s\":%u}",
        rssi, reg, up_s);

    // OBD2 실시간 PID — 링크 확립 시에만, 지원(응답 받은) 필드만 포함(6단계).
    if (obd.valid) {
        n += snprintf(buf + n, sizeof(buf) - n, ",\"obd\":{");
        int o = 0;   // 콤마 제어
        if (obd.has_rpm)      { n += snprintf(buf+n, sizeof(buf)-n, "%s\"rpm\":%.0f",     o++?",":"", obd.rpm); }
        if (obd.has_speed)    { n += snprintf(buf+n, sizeof(buf)-n, "%s\"speed\":%d",     o++?",":"", obd.speed); }
        if (obd.has_coolant)  { n += snprintf(buf+n, sizeof(buf)-n, "%s\"coolant\":%d",   o++?",":"", obd.coolant); }
        if (obd.has_load)     { n += snprintf(buf+n, sizeof(buf)-n, "%s\"load\":%.1f",    o++?",":"", obd.load); }
        if (obd.has_throttle) { n += snprintf(buf+n, sizeof(buf)-n, "%s\"throttle\":%.1f",o++?",":"", obd.throttle); }
        if (obd.has_intake)   { n += snprintf(buf+n, sizeof(buf)-n, "%s\"intake\":%d",    o++?",":"", obd.intake); }
        if (obd.has_maf)      { n += snprintf(buf+n, sizeof(buf)-n, "%s\"maf\":%.2f",     o++?",":"", obd.maf); }
        if (obd.has_fuel)     { n += snprintf(buf+n, sizeof(buf)-n, "%s\"fuel\":%.1f",    o++?",":"", obd.fuel); }
        if (obd.has_ctrlv)    { n += snprintf(buf+n, sizeof(buf)-n, "%s\"ctrl_v\":%.2f",  o++?",":"", obd.ctrl_v); }
        if (obd.has_runtime)  { n += snprintf(buf+n, sizeof(buf)-n, "%s\"runtime\":%u",   o++?",":"", obd.runtime); }
        if (obd.has_odometer) { n += snprintf(buf+n, sizeof(buf)-n, "%s\"odometer\":%.1f",o++?",":"", obd.odometer); }
        if (obd.has_vin)      { n += snprintf(buf+n, sizeof(buf)-n, "%s\"vin\":\"%s\"",   o++?",":"", obd.vin); }
        n += snprintf(buf + n, sizeof(buf) - n, "}");
    }

    if (withMeta) {
        // 최초 발행: device_id ↔ 하드웨어 매핑용 메타. imei/mac은 신원 아님.
        String imei = modem.getIMEI();
        uint64_t mac = ESP.getEfuseMac();
        char macStr[13];
        snprintf(macStr, sizeof(macStr), "%012llX", mac);
        n += snprintf(buf + n, sizeof(buf) - n,
            ",\"meta\":{\"imei\":\"%s\",\"mac\":\"%s\",\"fw\":\"%s\"}",
            imei.c_str(), macStr, FW_VERSION);
    }
    n += snprintf(buf + n, sizeof(buf) - n, "}");

    if (n <= 0 || n >= (int)sizeof(buf)) {
        log.println("[MQTT] telemetry payload overflow");
        return false;
    }

    bool ok = modem.mqtt_publish(kClientIdx, s_topicTelemetry.c_str(), buf, /*qos=*/1);
    if (!ok) s_connected = false;   // 발행 실패 = 세션 끊김 → 다음 루프서 재접속
    log.printf("[MQTT] telemetry seq=%u %s (%d B)\n", seq, ok ? "published" : "FAILED", n);
    return ok;
}

} // namespace Mqtt
