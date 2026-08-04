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
#include "clk.h"
#include "log.h"

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
static String s_topicFast;       // v1/{device_id}/telemetry/fast

static void buildTopics()
{
    s_clientId      = Prov::deviceId();
    s_topicStatus    = "v1/" + s_clientId + "/status";
    s_topicTelemetry = "v1/" + s_clientId + "/telemetry";
    // ⚠️ telemetry 하위 4레벨 토픽. 서버 EMQX Rule 은 `v1/+/telemetry` 와 별도로
    //    `v1/+/telemetry/fast` 가 필요하다(MQTT `+` 는 한 레벨만 매칭).
    s_topicFast      = s_topicTelemetry + "/fast";
}

static const uint8_t kClientIdx = 0;

// 접속 상태는 자체 추적한다. 래퍼 mqtt_connected()는 AT+CMQTTDISC? 폴링이라
// publish 직후의 +CMQTTPUB URC와 간섭해 오탐(false)을 내므로 쓰지 않는다.
static bool s_serviceStarted = false;   // mqtt_begin(CMQTTSTART) 완료 — 부팅당 1회만
static bool s_connected      = false;   // 세션 접속 여부

// 마지막 발행 시점의 망 상태. [STAT] 한 줄이 이 값을 재사용해 AT 왕복을 늘리지 않는다.
static int s_lastRssi = 0;
static int s_lastReg  = 0;

// 마지막 발행 시각(millis). 발행 간 간격 가드의 기준 — 헤더 주석 참고.
static uint32_t s_lastPubAt = 0;

void     notePublish()   { s_lastPubAt = millis(); }
uint32_t lastPublishAt() { return s_lastPubAt; }

bool publishGapElapsed(uint32_t now, uint32_t gapMs)
{
    if (s_lastPubAt == 0) return true;   // 아직 한 건도 안 냈다
    return (int32_t)(now - s_lastPubAt) >= (int32_t)gapMs;   // 오버플로우 안전 비교
}

// 사용자/비번이 빈 문자열이면 NULL(익명)로 넘긴다.
static const char *orNull(const char *s) { return (s && s[0]) ? s : nullptr; }

// 발행 직전 생존 게이트(R6). 죽은 모뎀에 mqtt_publish 를 던지면 래퍼가 CMQTTTOPIC 과
// CMQTTPAYLOAD 의 '>' 프롬프트를 각각 10초씩 기다린 뒤에야 실패한다
// (TinyGsmMqttA76xx::mqtt_publish). 그동안 loop 가 통째로 멈춰 LED·차키 해제·OBD 폴이
// 다 밀리고, 세션 사망 인지도 그만큼 늦는다 — 실측 발행 1건에 20초 이상.
//   ⚠️ mqtt_publish 의 timeout 인자(60)는 이것과 무관하다. 그건 브로커 쪽 pub_timeout
//      이고 AT 규격상 하한이 60초라 더 줄일 수 없다(+CMQTTPUB: ...,(60-180),...).
//      게다가 래퍼는 AT+CMQTTPUB 이 OK 를 내면 곧바로 true 를 반환한다 — 그 값은
//      블로킹 시간에 영향을 주지 않는다. 실제로 줄일 수 있는 건 이 사전 확인뿐이다.
//   ⚠️ UART 에 바이트가 있으면 프로브하지 않는다. 대기 중인 URC 를 testAT 가 응답으로
//      먹으면 수신 명령(+CMQTTRX*)이 통째로 사라진다. 바이트를 보내고 있다 = 살아 있다.
static bool modemResponsive(TinyGsm &modem)
{
    if (modem.stream.available()) return true;
    return modem.testAT(MODEM_PROBE_TIMEOUT_MS);
}

bool pubAckOverdue(TinyGsm &modem, uint32_t now)
{
    if (!s_connected) return false;
    if (!modem.mqttPubAckPending()) return false;
    return (int32_t)(now - modem.mqttPubSentAt()) >= (int32_t)MQTT_PUB_ACK_TIMEOUT_MS;
}

int lastPubErr(TinyGsm &modem) { return (int)modem.mqttLastPubErr(); }

// 발행 진입 공통 가드 — 무응답이면 세션을 사망 처리하고 즉시 false.
// s_connected 를 내려야 loop 가 다음 틱에 재접속 경로로 들어간다.
bool publishReady(TinyGsm &modem, const char *what, Stream &log)
{
    if (modemResponsive(modem)) return true;
    s_connected = false;
    LOGW(log, "[MQTT] %s 발행 생략 — 모뎀 무응답(세션 사망 처리)\n", what);
    return false;
}

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
        LOGW(log, "[MQTT] 접속 실패 (CA 검증/인증/네트워크 확인)\n");
        s_connected = false;
        return false;
    }

    // 새 세션 — 이전 세션의 발행 대기 흔적을 지운다(P3). mqtt_publish 는 마지막 단계까지
    // 가야 플래그를 세우므로, 앞 단계에서 실패한 발행의 pending=true 와 낡은 _pubSentAt
    // 이 그대로 남을 수 있다. 안 지우면 접속 직후 곧바로 "결과 URC 미도착" 오판이 난다.
    modem.mqttNotePubAck(-1);

    // 접속 알림: status online, retained.
    // ⚠️ 여기서는 power_mode·ignition_on 을 싣지 않는다 — 이 모듈은 OBD 상태를 모른다.
    //    구독 성공 직후 loop 가 publishStatus(sub=true) 로 실제 차량 상태를 함께 보고한다(F1/F3).
    static const char kOnline[] = "{\"online\":true}";
    modem.mqtt_publish(kClientIdx, s_topicStatus.c_str(), kOnline, /*qos=*/1,
                       /*timeout=*/60, /*retain=*/1);
    notePublish();   // 접속 직후 발행이 몰리는 구간의 간격 가드 기준
    s_connected = true;
    return true;
}

bool publishStatus(TinyGsm &modem, const char *powerMode, bool ignitionOn,
                   bool sub, Stream &log)
{
    if (!s_connected) return false;
    if (!publishReady(modem, "status", log)) return false;

    // 계약 StatusPayload — online 은 항상 true 다(끊김은 LWT 가 대행 발행한다).
    // sub 는 참일 때만 싣는다. 거짓을 실으면 서버가 "구독 실패 통지"로 오해할 여지가 있고,
    // 계약상으로도 없는 키(구식 펌웨어)와 같은 뜻이라 굳이 보낼 이유가 없다.
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
        "{\"online\":true%s,\"power_mode\":\"%s\",\"ignition_on\":%s}",
        sub ? ",\"sub\":true" : "", powerMode, ignitionOn ? "true" : "false");
    if (n <= 0 || n >= (int)sizeof(buf)) {
        LOGE(log, "[MQTT] status 페이로드 오버플로\n");
        return false;
    }

    // retain=1 — connectSession 의 online 이 retained 라, 여기서 0 으로 내면 브로커에는
    // power_mode 없는 낡은 값이 남는다.
    bool ok = modem.mqtt_publish(kClientIdx, s_topicStatus.c_str(), buf, /*qos=*/1,
                                 /*timeout=*/60, /*retain=*/1);
    notePublish();
    if (!ok) s_connected = false;   // 발행 실패 = 세션 끊김 → 다음 루프서 재접속
    // ⚠️ 태그는 [MQTT] 다. [STAT] 를 쓰면 UART 진단 앱이 주기 상태 한 줄로 오인해
    //    key=value 파싱이 깨진다(printStatusLine 주석).
    LOGI(log, "[MQTT] status %s power_mode=%s ignition_on=%d%s\n",
         ok ? "published" : "FAILED", powerMode, ignitionOn ? 1 : 0, sub ? " sub=1" : "");
    return ok;
}

bool begin(TinyGsm &modem, Stream &log)
{
    // device_id(런타임)로 토픽/클라이언트ID 조립. 매 접속 갱신(값 동일하면 무해).
    buildTopics();

    // CMQTT 서비스(CMQTTSTART)는 부팅당 1회만. 이미 시작됐으면 세션 접속만 한다.
    // (매번 mqtt_begin 호출 시 CMQTTSTOP이 살아있는 연결을 끊고 재시작에 실패한다.)
    if (!s_serviceStarted) {
        LOGD(log, "[MQTT] begin: %s, broker %s:%d, id=%s\n",
             MQTT_USE_TLS ? "TLS+sni" : "PLAIN", MQTT_HOST, MQTT_PORT, s_clientId.c_str());
        // 래퍼 RX 버퍼는 토픽과 페이로드를 함께 담는다 — 기본 256B 로는 긴 명령이
        // 잘린다(config.h MQTT_RX_BUFFER_SIZE 주석). 서비스 시작 전에 키운다.
        if (!modem.mqtt_set_rx_buffer_size(MQTT_RX_BUFFER_SIZE)) {
            LOGE(log, "[MQTT] RX 버퍼 확장 실패(%dB) — 긴 명령이 잘릴 수 있음\n",
                 MQTT_RX_BUFFER_SIZE);
        }
        if (!modem.mqtt_begin(MQTT_USE_TLS, /*sni=*/MQTT_USE_TLS)) {
            LOGE(log, "[MQTT] mqtt_begin 실패\n");
            return false;
        }
        s_serviceStarted = true;
    }

    bool ok = connectSession(modem, log);
    if (ok) LOGI(log, "[MQTT] 접속됨 (%s)\n", MQTT_USE_TLS ? "TLS, 서버 CA 검증" : "평문");
    return ok;
}

// 자체 추적값 반환 (플래키한 AT+CMQTTDISC? 폴링 회피).
bool isConnected(TinyGsm & /*modem*/) { return s_connected; }

void resetServiceState()
{
    // 모뎀이 리셋되면 CMQTT 서비스(AT+CMQTTSTART)도 함께 사라진다. 그런데 s_serviceStarted
    // 는 펌웨어 쪽 플래그라 그대로 남아, begin()이 mqtt_begin()을 건너뛰고 곧바로
    // mqtt_connect()를 호출한다 → 서비스가 없으니 영영 접속 실패한다.
    // 모뎀을 리셋한 쪽에서 이걸 불러 상태를 모뎀과 다시 맞춘다. AT는 보내지 않는다
    // (갓 리셋된 모뎀에 mqtt_disconnect를 보내면 무의미하게 블로킹된다).
    s_serviceStarted = false;
    s_connected      = false;
}

int lastRssi() { return s_lastRssi; }
int lastReg()  { return s_lastReg; }

// 토픽/ID는 접속 시점에 조립된다 — 접속 전이면 빈 문자열.
String clientId()       { return s_clientId; }
String topicTelemetry() { return s_topicTelemetry; }
bool   serviceStarted() { return s_serviceStarted; }

// 콜백/URC 펌핑. 매 틱 호출되므로 "URC 없을 때 0ms"가 되어야 한다.
//   래퍼 mqtt_handle(timeout)은 내부에서 waitResponse(timeout, "+CMQTTRXSTART:")를 하는데,
//   waitResponse는 매칭이 없으면 timeout을 통째로 spin한다(TinyGsmClientA7670.h do-while).
//   기본값 100ms → cmd URC가 거의 없는 정상 운행 중에도 매 틱 100ms를 버려
//   실 루프 주기가 delay(50)의 3배(≈150ms)가 되고 LED 패턴/OBD 폴 시각이 밀린다.
//   → UART에 실제 바이트가 있을 때만 진입시키고, 그때도 짧은 상한만 준다.
void handle(TinyGsm &modem)
{
    if (!modem.stream.available()) return;   // URC 없음 → 즉시 반환(0ms)
    modem.mqtt_handle(MQTT_HANDLE_TIMEOUT_MS);
}

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

bool publishTelemetry(TinyGsm &modem, const GpsFix &fix, bool fixFresh, const Obd2::Data &obd,
                      uint32_t seq, bool withMeta, Stream &log)
{
    // ⚠️ 반드시 첫 줄이다. 아래 getSignalQuality/getRegistrationStatus 도 AT 왕복이라
    //    죽은 모뎀에서는 여기서부터 타임아웃을 태우기 시작한다.
    if (!publishReady(modem, "telemetry", log)) return false;

    // 네트워크 상태는 발행 시점에 신선하게 읽는다.
    int rssi = modem.getSignalQuality();
    int reg  = (int)modem.getRegistrationStatus();
    s_lastRssi = rssi;   // [STAT] 한 줄이 재사용(추가 AT 왕복 방지)
    s_lastReg  = reg;

    // ts: 현재 측위 중이면 GPS UTC(권위), 아니면 모뎀 UTC 폴백, 그것도 없으면 0.
    // ⚠️ 조건은 fix.valid 가 아니라 fixFresh 다. fix 는 측위를 놓쳐도 마지막 값을 유지하는
    //    캐시라(loop g_lastFix) fix.valid 는 미측위 중에도 계속 true 이고, 그 시각으로 ts 를
    //    만들면 미측위 구간 내내 ts 가 얼어붙는다. 서버 PK 가 (device_id, ts) 라 같은 ts 는
    //    한 행으로 붕괴한다 — 실측(0.2.10, 07-30): 측위를 놓친 채 주행한 33분치 34건이
    //    한 행이 되어 사라졌고, 하루치로는 발행 2270건 중 323건(14%)이 유실됐다.
    uint32_t ts = 0;
    bool authoritative = false;
    if (fixFresh && fix.valid && fix.year >= 2020) {
        ts = civilToEpoch(fix.year, fix.month, fix.day,
                          fix.hour, fix.minute, fix.second);
        authoritative = true;
        // 시각 기준 갱신 — 오프라인 적재/고빈도 창이 이 base 로 시각을 만든다(§4.1).
        // ts 산출 로직 자체는 건드리지 않는다(0.2.11 동작 유지).
        Clk::note(ts, Clk::SRC_GPS);
    } else {
        ts = modemEpoch(modem);   // 미측위(실내/음영/터널)여도 실시각 확보 → 서버 누적 정상화
        if (ts) Clk::note(ts, Clk::SRC_MODEM);
    }
    uint32_t up_s = millis() / 1000UL;

    // ts 단조 증가 가드 — 폴백 경로 전용. 모뎀 시각이 미동기(0)이거나 NITZ 재동기로
    // 뒷걸음질하면 다시 PK 가 겹쳐 행이 사라진다. 앞선 ts 에 up_s 경과분을 더해 밀어낸다.
    // 측위 중 GPS UTC 는 권위이므로 이 보정을 태우지 않는다(모뎀 시각 오차가 latch 되면
    // 이후 진짜 GPS 시각이 영영 밀리게 된다).
    static uint32_t s_lastTs = 0, s_lastTsUp = 0;
    if (!authoritative && s_lastTs && ts <= s_lastTs) {
        uint32_t adv = up_s - s_lastTsUp;
        ts = s_lastTs + (adv ? adv : 1);
    }
    if (ts) { s_lastTs = ts; s_lastTsUp = up_s; }

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
        LOGE(log, "[MQTT] telemetry 페이로드 오버플로\n");
        return false;
    }

    bool ok = modem.mqtt_publish(kClientIdx, s_topicTelemetry.c_str(), buf, /*qos=*/1);
    notePublish();
    if (!ok) s_connected = false;   // 발행 실패 = 세션 끊김 → 다음 루프서 재접속
    // 발행 1건마다의 상세는 DEBUG. 평상시 발행 결과는 loop의 [STAT] 한 줄에 실린다.
    LOGD(log, "[MQTT] telemetry seq=%u %s (%d B)\n", seq, ok ? "published" : "FAILED", n);
    return ok;
}

bool noteTimeFromModem(TinyGsm &modem)
{
    uint32_t e = modemEpoch(modem);
    if (!e) return false;
    Clk::note(e, Clk::SRC_MODEM);
    return true;
}

bool publishBackfill(TinyGsm &modem, const Buf::Record &rec, Stream &log)
{
    if (!s_connected) return false;
    if (!publishReady(modem, "백필", log)) return false;
    if (rec.ts == 0) {
        // 호출측이 resolveTs 로 확정했어야 한다. ts=0 을 보내면 서버 PK (device_id,0) 에
        // 여러 건이 한 행으로 붕괴한다 — 보내지 않는 편이 정직하다.
        LOGW(log, "[BUF] ts 미확정 레코드 — 발행 생략\n");
        return false;
    }

    const bool  parked = (rec.flags & Buf::F_PARKED_WIN) != 0;
    const int   aggW   = parked ? (int)(OFFLINE_LOG_MS_PARKED / 1000UL)
                                : (int)(OFFLINE_LOG_MS_DRIVING / 1000UL);
    const bool  hasFix = (rec.flags & Buf::F_GPS_FIX) != 0;
    const bool  hasPos = (rec.lat_e6 != 0 || rec.lon_e6 != 0);

    char buf[640];
    int n = snprintf(buf, sizeof(buf),
        "{\"ts\":%u,\"seq\":%u,\"gps\":{\"fix\":%s",
        rec.ts, (unsigned)rec.seq, hasFix ? "true" : "false");

    if (hasPos) {
        n += snprintf(buf + n, sizeof(buf) - n,
            ",\"lat\":%.6f,\"lon\":%.6f,\"spd\":%.1f,\"sat\":%d",
            rec.lat_e6 / 1e6, rec.lon_e6 / 1e6, rec.spd_x10 / 10.0, (int)rec.sat);
    }

    // sys.bf=1 이 백필 표시자다. ingest 가 sys 를 통째로 data JSONB 에 넣으므로
    // 게이트웨이 코드 변경 없이 저장된다.
    n += snprintf(buf + n, sizeof(buf) - n,
        "},\"net\":{\"rssi\":%d,\"reg\":%d},\"sys\":{\"up_s\":%u,\"bf\":1}",
        (int)rec.rssi, (int)rec.reg, rec.up_s);

    if (rec.obd_flags) {
        n += snprintf(buf + n, sizeof(buf) - n, ",\"obd\":{");
        int o = 0;
        if (rec.obd_flags & Buf::O_RPM)      { n += snprintf(buf+n, sizeof(buf)-n, "%s\"rpm\":%u",        o++?",":"", rec.rpm); }
        if (rec.obd_flags & Buf::O_SPEED)    { n += snprintf(buf+n, sizeof(buf)-n, "%s\"speed\":%u",      o++?",":"", rec.obd_speed); }
        if (rec.obd_flags & Buf::O_COOLANT)  { n += snprintf(buf+n, sizeof(buf)-n, "%s\"coolant\":%d",    o++?",":"", (int)rec.coolant); }
        if (rec.obd_flags & Buf::O_LOAD)     { n += snprintf(buf+n, sizeof(buf)-n, "%s\"load\":%.1f",     o++?",":"", rec.load_x2 / 2.0); }
        if (rec.obd_flags & Buf::O_THR)      { n += snprintf(buf+n, sizeof(buf)-n, "%s\"throttle\":%.1f", o++?",":"", rec.throttle_x2 / 2.0); }
        if (rec.obd_flags & Buf::O_INTAKE)   { n += snprintf(buf+n, sizeof(buf)-n, "%s\"intake\":%d",     o++?",":"", (int)rec.intake); }
        if (rec.obd_flags & Buf::O_MAF)      { n += snprintf(buf+n, sizeof(buf)-n, "%s\"maf\":%.2f",      o++?",":"", rec.maf_x10 / 10.0); }
        if (rec.obd_flags & Buf::O_FUEL)     { n += snprintf(buf+n, sizeof(buf)-n, "%s\"fuel\":%.1f",     o++?",":"", rec.fuel_x2 / 2.0); }
        if (rec.obd_flags & Buf::O_CTRLV)    { n += snprintf(buf+n, sizeof(buf)-n, "%s\"ctrl_v\":%.2f",   o++?",":"", rec.ctrl_mv / 1000.0); }
        if (rec.obd_flags & Buf::O_RUNTIME)  { n += snprintf(buf+n, sizeof(buf)-n, "%s\"runtime\":%u",    o++?",":"", rec.runtime_s); }
        if (rec.obd_flags & Buf::O_ODOMETER) { n += snprintf(buf+n, sizeof(buf)-n, "%s\"odometer\":%.1f", o++?",":"", rec.odo_x10 / 10.0); }
        n += snprintf(buf + n, sizeof(buf) - n, "}");
    }

    // ⚠️ agg 는 창에 샘플이 있을 때만 싣는다. 0 으로 채운 agg 를 보내면 서버가
    //    "정지·무회전"을 관측값으로 오인한다(shared/telemetry.ts AggSample 주석).
    if (rec.agg_n > 0) {
        n += snprintf(buf + n, sizeof(buf) - n,
            ",\"agg\":{\"w\":%d,\"n\":%u,\"spd_max\":%u,\"spd_min\":%u,\"rpm_max\":%u,"
            "\"accel\":%.1f,\"decel\":%.1f,\"ev\":%u,\"ev_n\":%u}",
            aggW, rec.agg_n, rec.agg_spd_max, rec.agg_spd_min,
            (unsigned)rec.agg_rpm_max_x100 * 100,
            rec.agg_accel_x10 / 10.0, rec.agg_decel_x10 / 10.0,
            rec.agg_ev, rec.agg_ev_n);
    }
    n += snprintf(buf + n, sizeof(buf) - n, "}");

    if (n <= 0 || n >= (int)sizeof(buf)) {
        LOGE(log, "[BUF] 백필 페이로드 오버플로\n");
        return false;
    }

    bool ok = modem.mqtt_publish(kClientIdx, s_topicTelemetry.c_str(), buf, /*qos=*/1);
    notePublish();
    if (!ok) s_connected = false;   // 발행 실패 = 세션 끊김 → 다음 루프서 재접속
    LOGD(log, "[BUF] 백필 ts=%u %s (%d B)\n", rec.ts, ok ? "published" : "FAILED", n);
    return ok;
}

bool publishFast(TinyGsm &modem, const Fast::Sample *win, int n, uint32_t t0, Stream &log)
{
    if (!s_connected || !win || n <= 0) return false;
    if (!publishReady(modem, "고빈도 창", log)) return false;
    if (t0 == 0) {
        // 시각 기준이 없으면 서버가 t0+o[i] 로 샘플 시각을 만들 수 없다.
        LOGD(log, "[FAST] t0 미확정 — 창 발행 생략\n");
        return false;
    }

    // 페이로드는 평탄(flat) 형태다 — 단말이 snprintf 로 손조립하므로 중첩을 피한다.
    // n=30 기준 약 545B, n=64 기준 약 1.2KB (모뎀 +CMQTTPAYLOAD 상한 10,240B).
    char buf[FAST_PAYLOAD_BUF];
    int  p = snprintf(buf, sizeof(buf), "{\"t0\":%u,\"dt\":%u,\"n\":%d",
                      t0, (unsigned)(Cfg::fastMs() / 1000UL), n);

    // o = t0 기준 초 오프셋. loop 가 MQTT publish(~0.5초)나 전체 OBD 폴로 초를 건너뛰므로
    // 고정 격자를 가정하면 시계열이 조용히 어긋난다 → 오프셋을 명시적으로 싣는다.
    p += snprintf(buf + p, sizeof(buf) - p, ",\"o\":[");
    for (int i = 0; i < n && p < (int)sizeof(buf); i++)
        p += snprintf(buf + p, sizeof(buf) - p, "%s%u", i ? "," : "", win[i].off);

    p += snprintf(buf + p, sizeof(buf) - p, "],\"spd\":[");
    for (int i = 0; i < n && p < (int)sizeof(buf); i++)
        p += snprintf(buf + p, sizeof(buf) - p, "%s%u", i ? "," : "", win[i].spd);

    p += snprintf(buf + p, sizeof(buf) - p, "],\"rpm\":[");
    for (int i = 0; i < n && p < (int)sizeof(buf); i++)
        p += snprintf(buf + p, sizeof(buf) - p, "%s%u", i ? "," : "", win[i].rpm);

    p += snprintf(buf + p, sizeof(buf) - p, "],\"thr\":[");
    for (int i = 0; i < n && p < (int)sizeof(buf); i++)
        p += snprintf(buf + p, sizeof(buf) - p, "%s%u", i ? "," : "", win[i].thr);

    p += snprintf(buf + p, sizeof(buf) - p, "]}");

    if (p <= 0 || p >= (int)sizeof(buf)) {
        // 잘린 JSON 을 보내면 서버가 배치 전체를 버린다 — 보내지 않는다.
        LOGE(log, "[FAST] 창 페이로드 오버플로(n=%d) — 발행 생략\n", n);
        return false;
    }

    bool ok = modem.mqtt_publish(kClientIdx, s_topicFast.c_str(), buf, /*qos=*/1);
    notePublish();
    if (!ok) s_connected = false;
    LOGD(log, "[FAST] 창 t0=%u n=%d %s (%d B)\n", t0, n, ok ? "published" : "FAILED", p);
    return ok;
}

} // namespace Mqtt
