/**
 * @file      provisioning.cpp
 * @brief     device_id NVS 저장 + 시리얼 콘솔 입력 구현.
 */
#include "provisioning.h"
#include "config.h"
#include <Preferences.h>
#include <ArduinoHttpClient.h>   // TinyGsmClient 위 평문 HTTP (lte.cpp와 동일 스택)
#if PROVISION_USE_TLS
#include "certs.h"               // CA_LOCAL_EMQX — HTTPS 서버 인증용 CA(모뎀 업로드)
#endif

#if FEATURE_CARKEY
#include "carkey.h"              // Debug Console lock/unlock 검증 훅
#endif

namespace Prov {

static Preferences s_nvs;
static String s_deviceId;
static String s_mqttPw;
static String s_line;           // 시리얼 라인 누적 버퍼

static const char *NVS_NS      = "prov";
static const char *KEY_DEVICE  = "device_id";
static const char *KEY_MQTT_PW = "mqtt_pw";

// device_id 형식 검증: ^vt-[0-9]{4}-[0-9]{4}-[a-z0-9]{3}$  (계약 deviceId.ts와 동일)
static bool isValidId(const String &s)
{
    if (s.length() != 16) return false;
    if (s[0] != 'v' || s[1] != 't' || s[2] != '-') return false;
    for (int i = 3; i < 7; i++)  if (!isDigit(s[i])) return false;
    if (s[7] != '-') return false;
    for (int i = 8; i < 12; i++) if (!isDigit(s[i])) return false;
    if (s[12] != '-') return false;
    for (int i = 13; i < 16; i++) {
        char c = s[i];
        if (!(isDigit(c) || (c >= 'a' && c <= 'z'))) return false;  // [a-z0-9]
    }
    return true;
}

void begin()
{
    s_nvs.begin(NVS_NS, /*readOnly=*/false);
    s_deviceId = s_nvs.getString(KEY_DEVICE, "");
    s_mqttPw   = s_nvs.getString(KEY_MQTT_PW, "");

#if PROVISION_SEED_FALLBACK
    // 오프라인(서버 없이) 테스트용: NVS 비면 컴파일 기본값(DEVICE_ID)으로 시드.
    if (s_deviceId.isEmpty()) {
        s_deviceId = DEVICE_ID;
        s_nvs.putString(KEY_DEVICE, s_deviceId);
    }
#endif
    // 기본(PROVISION_SEED_FALLBACK=0): NVS 비면 미프로비저닝 상태로 두고,
    // 부팅 시 provisionOverHttp가 서버에서 device_id/mqtt_pw를 발급받는다.
}

bool hasValidId()               { return isValidId(s_deviceId); }
bool hasCredentials()           { return isValidId(s_deviceId) && !s_mqttPw.isEmpty(); }
const String &deviceId()        { return s_deviceId; }
const String &mqttPassword()    { return s_mqttPw; }

// 아주 단순한 JSON 문자열 필드 추출: "key":"value". 중첩/이스케이프 미지원
// (provision 응답이 평평한 문자열 필드뿐이라 충분. 값에 " 포함 시 미지원).
static String jsonStr(const String &body, const char *key)
{
    String pat = String("\"") + key + "\"";
    int k = body.indexOf(pat);
    if (k < 0) return "";
    int colon = body.indexOf(':', k + pat.length());
    if (colon < 0) return "";
    int q1 = body.indexOf('"', colon + 1);
    if (q1 < 0) return "";
    int q2 = body.indexOf('"', q1 + 1);
    if (q2 < 0) return "";
    return body.substring(q1 + 1, q2);
}

#if PROVISION_USE_TLS
// 프로비저닝 HTTPS 서버 CA — gateway-tls 는 EMQX 와 동일 cert 를 쓰므로 CA 선택도 mqtt.cpp 의
// kCaPem 과 동일하게 config.h MQTT_BROKER_SEL 토글을 따른다(프로파일 전환 시 자동 일치).
#if   MQTT_BROKER_SEL == MQTT_BROKER_TEST
static const char *kProvCaPem = CA_MOSQUITTO_ORG;
#elif MQTT_BROKER_SEL == MQTT_BROKER_LOCAL
static const char *kProvCaPem = CA_LOCAL_EMQX;
#else
static const char *kProvCaPem = CA_ISRG_ROOT_X1;
#endif

// CA 를 모뎀 FS 에 업로드(+CCERTDOWN) + SSL 컨텍스트 0 바인딩. A76xx 는 HTTPS·MQTT 가 ctx0 을
// 공유하므로 CA 하나로 둘 다 커버. 부팅 시(MQTT 연결 전) 1회 — mqtt_connect 의 CCERTDOWN 시퀀스와 동일.
// authmode=1(서버 인증). 부팅 직후 모뎀 시각 미동기 대비 ignorelocaltime=1(유효기간 검증만 완화, 체인 검증 유지).
static bool uploadProvCa(TinyGsm &modem, Stream &log)
{
    const char *ca = kProvCaPem;
    // ① CA 파일 다운로드(모뎀 FS 저장). 이미 있으면 ">" 미수신 → 기존 파일 재사용.
    modem.sendAT("+CCERTDOWN=\"ca_cert.pem\",", strlen(ca));
    if (modem.waitResponse(10000UL, ">") == 1) {
        modem.stream.write(ca);
        if (modem.waitResponse() != 1) {
            log.println("[PROV] CA 업로드 실패");
            return false;
        }
        log.println("[PROV] CA 업로드 완료(ca_cert.pem)");
    } else {
        log.println("[PROV] CA 다운로드 프롬프트 없음 — 기존 ca_cert.pem 재사용 가정");
    }
    // ② SSL ctx0: CA 바인딩 + 서버 인증 + 부팅 시각 미동기 대비 유효기간 검증 완화.
    modem.sendAT("+CSSLCFG=\"cacert\",0,\"ca_cert.pem\"");   modem.waitResponse();
    modem.sendAT("+CSSLCFG=\"authmode\",0,1");               modem.waitResponse();  // 1=서버 인증
    modem.sendAT("+CSSLCFG=\"ignorelocaltime\",0,1");        modem.waitResponse();
    return true;
}
#endif

ProvResult provisionOverHttp(TinyGsm &modem, Stream &log)
{
    // 신원 원천: IMEI(모뎀) + efuse MAC(ESP32). 서버 allowlist는 IMEI로 매칭한다.
    String imei = modem.getIMEI();
    imei.trim();
    if (imei.isEmpty()) {
        log.println("[PROV] IMEI 조회 실패 — 네트워크 오류로 처리(재시도)");
        return ProvResult::NETWORK_ERROR;
    }
    uint64_t efuse = ESP.getEfuseMac();
    char macBuf[13];
    // telemetry meta(mqtt.cpp)와 동일 표기(%012llX)로 통일.
    snprintf(macBuf, sizeof(macBuf), "%012llX", (unsigned long long)efuse);

    // 요청 본문(계약: packages/shared provisioning.ts) — 손 조립.
    String body = String("{\"imei\":\"") + imei +
                  "\",\"mac\":\""        + macBuf +
                  "\",\"fw\":\""         + FW_VERSION + "\"}";

    int    status = -1;
    String resp;

#if PROVISION_USE_TLS
    // TLS: 모뎀 내장 HTTPS(ota.cpp 와 동일 서비스). 응답에 mqtt_password 가 실리므로 암호화 필수.
    // CA 를 모뎀 FS 에 올려 SSL ctx0 에 바인딩(authmode=1) → 서버 인증(MITM 방지)까지 적용.
    // A76xx 는 https_begin 에 CA 인자가 없어 CSSLCFG 로 ctx0 을 직접 설정한다(uploadProvCa).
    // 부팅 시(MQTT 연결 전) 1회라 ctx0 경합 없음. MQTT 도 이후 같은 CA 로 ctx0 재설정(멱등).
    uploadProvCa(modem, log);

    String url = String("https://") + PROVISION_HOST + ":" + PROVISION_PORT + PROVISION_PATH;
    log.printf("[PROV] provision POST %s\n", url.c_str());
    log.printf("[PROV]   body=%s\n", body.c_str());

    modem.https_begin();
    if (!modem.https_set_url(url)) {
        log.println("[PROV] https_set_url 실패 — 네트워크 오류");
        modem.https_end();
        return ProvResult::NETWORK_ERROR;
    }
    modem.https_set_content_type("application/json");
    status = modem.https_post(body);     // HTTP 상태코드 반환(<0=연결/전송 실패)
    resp   = modem.https_body();
    modem.https_end();
    if (status < 0) {
        log.printf("[PROV] HTTPS POST 실패 (code=%d) — 네트워크 오류\n", status);
        return ProvResult::NETWORK_ERROR;
    }
#else
    // 평문 HTTP POST (lte.cpp httpGetCheck와 동일 스택).
    log.printf("[PROV] provision POST http://%s:%d%s\n",
               PROVISION_HOST, PROVISION_PORT, PROVISION_PATH);
    log.printf("[PROV]   body=%s\n", body.c_str());

    TinyGsmClient client(modem);
    HttpClient    http(client, PROVISION_HOST, PROVISION_PORT);
    http.setHttpResponseTimeout(20000);   // 응답 대기 상한(ms)

    int err = http.post(PROVISION_PATH, "application/json", body);
    if (err != 0) {
        log.printf("[PROV] connect/POST 실패 (err=%d) — 네트워크 오류\n", err);
        http.stop();
        return ProvResult::NETWORK_ERROR;
    }
    status = http.responseStatusCode();
    resp   = http.responseBody();
    http.stop();
#endif
    log.printf("[PROV] status=%d body=%s\n", status, resp.c_str());

    if (status == 403) {
        log.println("[PROV] 거절(403) — allowlist 미등록/폐기. 재시도 안 함");
        return ProvResult::REJECTED;
    }
    if (status != 200 && status != 201) {
        log.printf("[PROV] 예상외 상태 %d — 네트워크/서버 오류로 처리\n", status);
        return ProvResult::NETWORK_ERROR;
    }

    // 발급 성공 — 크리덴셜 파싱/저장.
    String devId  = jsonStr(resp, "device_id");
    String pw     = jsonStr(resp, "mqtt_password");
    String broker = jsonStr(resp, "broker_domain");  // 참고용(현재 미소비: config MQTT_HOST 사용)

    if (!isValidId(devId) || pw.isEmpty()) {
        log.println("[PROV] 응답 파싱 실패(device_id/mqtt_password 누락) — 재시도");
        return ProvResult::NETWORK_ERROR;
    }

    s_deviceId = devId;
    s_mqttPw   = pw;
    s_nvs.putString(KEY_DEVICE,  s_deviceId);
    s_nvs.putString(KEY_MQTT_PW, s_mqttPw);

    log.printf("[PROV] ✅ 발급 성공 device_id=%s, mqtt_pw 저장됨%s\n",
               s_deviceId.c_str(),
               broker.length() ? (String(" (broker=") + broker + ")").c_str() : "");
    return ProvResult::OK;
}

void printHelp(Stream &io)
{
    io.println("[PROV] 명령: setid vt-YYMM-NNNN-XXX | setpw <pw> | showid | clearid | help");
#if FEATURE_CARKEY
    io.println("[KEY]  명령: lock [ms] | unlock [ms]   (ms 생략 시 기본 펄스폭)");
#endif
}

static void printState(Stream &io)
{
    io.printf("[PROV] device_id=%s (%s), mqtt_pw=%s\n",
              s_deviceId.c_str(), isValidId(s_deviceId) ? "valid" : "INVALID",
              s_mqttPw.isEmpty() ? "(none)" : "(set)");
}

static void processLine(const String &raw, Stream &io)
{
    String line = raw; line.trim();
    if (line.isEmpty()) return;

    int sp = line.indexOf(' ');
    String cmd = (sp < 0) ? line : line.substring(0, sp);
    String arg = (sp < 0) ? ""   : line.substring(sp + 1);
    arg.trim();

    if (cmd == "setid") {
        if (!isValidId(arg)) {
            io.println("[PROV] 거부: 형식은 vt-YYMM-NNNN-XXX (예: vt-2607-0001-x7q)");
            return;
        }
        s_deviceId = arg;
        s_nvs.putString(KEY_DEVICE, s_deviceId);
        io.printf("[PROV] device_id 저장됨: %s — 적용하려면 재부팅(리셋)\n", s_deviceId.c_str());
    } else if (cmd == "setpw") {
        s_mqttPw = arg;
        s_nvs.putString(KEY_MQTT_PW, s_mqttPw);
        io.println("[PROV] mqtt_pw 저장됨 — 적용하려면 재부팅(리셋)");
    } else if (cmd == "showid") {
        printState(io);
    } else if (cmd == "clearid") {
        s_nvs.remove(KEY_DEVICE);
        s_nvs.remove(KEY_MQTT_PW);
        s_deviceId = "";
        s_mqttPw   = "";
        io.println("[PROV] NVS 초기화 완료 → 재부팅 시 자동 재발급(프로비저닝)");
    } else if (cmd == "help") {
        printHelp(io);
    } else {
#if FEATURE_CARKEY
        // 차키 검증 명령(lock/unlock [holdMs])을 이 단일 시리얼 소유자에서 위임.
        if (Carkey::tryConsole(cmd, arg, io)) return;
#endif
        io.printf("[PROV] 알 수 없는 명령: '%s'\n", cmd.c_str());
        printHelp(io);
    }
}

void handleSerial(Stream &io)
{
    // 논블로킹: 들어온 문자를 개행까지 누적 후 처리.
    // CR/LF/CRLF 모두 라인 종료로 처리 → 터미널 줄바꿈 설정과 무관하게 명령이 실행된다.
    while (io.available()) {
        char c = (char)io.read();
        if (c == '\r' || c == '\n') {
            if (s_line.length()) {          // 빈 줄(CRLF의 두 번째 문자 등)은 무시
                processLine(s_line, io);
                s_line = "";
            }
        } else if (s_line.length() < 80) {
            s_line += c;
        }
    }
}

} // namespace Prov
