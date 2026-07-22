/**
 * @file      KeyplusDiagnostic.ino
 * @brief     Keyplus OBD2 진단기 통합 펌웨어 — 엔트리포인트
 *
 * 보드: LilyGO T-A7670E (ESP32-WROVER + A7670E LTE Cat-1 + 내장 GNSS)
 * 진행: 2트랙 단계별. 현재 = 2단계 GPS 위치(유심 불필요).
 *       기능 토글은 config.h(FEATURE_*)에서 관리하며, 이후 단계(OBD2/차키/BLE/LTE)를
 *       모듈로 이 파일에 얹는다.
 *
 * 참고: hardware-baseline.md(핀맵·파티션), FEATURE_MAP.md(예제 매핑)
 */
#include "config.h"
#include "utilities.h"

#define TINY_GSM_RX_BUFFER  1024        // RX 버퍼 1KB
#define SerialMon           Serial
#define TINY_GSM_DEBUG      SerialMon
// #define DUMP_AT_COMMANDS             // 필요 시 AT 로그 (StreamDebugger 필요)

#include <TinyGsmClient.h>

// provisioning.h는 TinyGsmClient.h(및 TINY_GSM_RX_BUFFER)보다 뒤에 포함한다
// — provisioning.h가 HTTP 자동 프로비저닝에 TinyGsm 타입을 참조하기 때문.
#include "provisioning.h"
#include "led.h"

#if FEATURE_CARKEY
#include "carkey.h"
#endif

#if FEATURE_GPS
#include "gps.h"
#endif

#if FEATURE_LTE
#include "lte.h"
#include "mqtt.h"
#endif

#ifdef DUMP_AT_COMMANDS
#include <StreamDebugger.h>
StreamDebugger debugger(SerialAT, SerialMon);
TinyGsm modem(debugger);
#else
TinyGsm modem(SerialAT);
#endif

// ---------------------------------------------------------------------------
// 모뎀 하드웨어 전원 시퀀스 (GPS_BuiltIn 예제 기준)
// ---------------------------------------------------------------------------
static void modemPowerOn()
{
#ifdef BOARD_POWERON_PIN
    // ⚠️ T-A7670 V1.2: 배터리 전원 시 POWERON(IO12)을 HIGH로 유지하지 않으면 리셋됨.
    pinMode(BOARD_POWERON_PIN, OUTPUT);
    digitalWrite(BOARD_POWERON_PIN, HIGH);
#endif

#ifdef MODEM_RESET_PIN
    pinMode(MODEM_RESET_PIN, OUTPUT);
    digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL); delay(100);
    digitalWrite(MODEM_RESET_PIN, MODEM_RESET_LEVEL);  delay(2600);
    digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);
#endif

#ifdef MODEM_DTR_PIN
    // DTR LOW → 모뎀 슬립 방지 (33=RING/25=DTR은 베이스라인에서 예약 유지)
    pinMode(MODEM_DTR_PIN, OUTPUT);
    digitalWrite(MODEM_DTR_PIN, LOW);
#endif

    // PWRKEY 펄스로 모뎀 부팅
    pinMode(BOARD_PWRKEY_PIN, OUTPUT);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);  delay(100);
    digitalWrite(BOARD_PWRKEY_PIN, HIGH); delay(MODEM_POWERON_PULSE_WIDTH_MS);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);
}

// AT 응답이 올 때까지 대기. 30회 실패 시 PWRKEY 재펄스.
static void modemWaitReady()
{
    SerialMon.println("[MODEM] waiting for AT...");
    int retry = 0;
    while (!modem.testAT(1000)) {
        SerialMon.print(".");
        if (retry++ > 30) {
            digitalWrite(BOARD_PWRKEY_PIN, LOW);  delay(100);
            digitalWrite(BOARD_PWRKEY_PIN, HIGH); delay(MODEM_POWERON_PULSE_WIDTH_MS);
            digitalWrite(BOARD_PWRKEY_PIN, LOW);
            retry = 0;
        }
    }
    SerialMon.println("\n[MODEM] AT ready");
}

// 모뎀이 내장 GPS 지원 모델인지 확인. A7670G(GPS 미지원)면 정지.
static void modemCheckGpsCapable()
{
    String name = "UNKNOWN";
    while (true) {
        name = modem.getModemName();
        if (name == "UNKNOWN") {
            SerialMon.println("[MODEM] name unavailable, retrying");
            delay(1000);
            continue;
        }
        // A7670G 계열은 내장 측위 미지원 → 외장 GPS 예제 필요.
        if (name.startsWith("A7670G")) {
            while (true) {
                SerialMon.println("[MODEM] this modem has NO built-in GPS (A7670G). "
                                  "Use ExternalGPS_A7670G_Only. Halting.");
                delay(5000);
            }
        }
        SerialMon.print("[MODEM] model: ");
        SerialMon.println(name);
        break;
    }
}

// 단말 신원 요약 출력: imei/mac(하드웨어) + device_id/mqtt_pw(NVS). 모뎀 준비 후 호출.
static void printIdentity(TinyGsm &modem, const char *when)
{
    String imei = modem.getIMEI(); imei.trim();
    uint64_t efuse = ESP.getEfuseMac();
    char mac[13];
    snprintf(mac, sizeof(mac), "%012llX", (unsigned long long)efuse);
    SerialMon.println("---------------- 단말 신원 ----------------");
    SerialMon.printf("  시점       : %s\n", when);
    SerialMon.printf("  imei       : %s\n", imei.length() ? imei.c_str() : "(조회 실패)");
    SerialMon.printf("  mac        : %s\n", mac);
    SerialMon.printf("  device_id  : %s (%s)\n", Prov::deviceId().c_str(),
                     Prov::hasValidId() ? "valid" : "none/invalid");
    SerialMon.printf("  mqtt_pw    : %s\n",
                     Prov::mqttPassword().isEmpty() ? "(none)" : Prov::mqttPassword().c_str());
    SerialMon.println("-------------------------------------------");
}

// ---------------------------------------------------------------------------
void setup()
{
    SerialMon.begin(115200);
    delay(100);
    SerialMon.println("\n=== Keyplus Diagnostic — GPS + LTE bring-up ===");

    // 상태표시 LED — 부팅 진입 즉시 solid ON(살아있음 표시).
    Led::begin();

#if FEATURE_CARKEY
    // 차키 게이트를 즉시 OUTPUT LOW(버튼 뗌)로 확정 — 부팅 초기 플로팅/오동작 방지.
    // 콘솔 'lock'/'unlock'으로 검증(Prov 콘솔 미지명령 분기에서 Carkey::tryConsole 호출).
    Carkey::begin();
#endif

    // 단말 신원 로드(NVS). Debug Console에서 'setid vt-...'로 설정 가능('help' 참고).
    Prov::begin();
    SerialMon.printf("[PROV] device_id=%s (%s)\n",
                     Prov::deviceId().c_str(), Prov::hasValidId() ? "valid" : "INVALID");
    Prov::printHelp(SerialMon);
    // 크리덴셜 미확보(device_id/pw 없음) → 미프로비저닝 표시. LTE up 이후 자동 발급(아래).
    if (!Prov::hasCredentials()) Led::set(Led::State::UNPROVISIONED);

    modemPowerOn();
    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
    delay(3000);

    modemWaitReady();

    // 부팅 신원 스냅샷: imei/mac + 현재 NVS의 device_id/pw(발급 전이면 none).
    printIdentity(modem, "부팅");

#if FEATURE_GPS
    modemCheckGpsCapable();
    // GNSS를 먼저 켜서 아래 LTE 등록(최대 수십 초) 대기 동안 위성을 포착하게 한다.
    SerialMon.println("[GPS] enabling GNSS...");
    while (!Gps::begin(modem)) {
        SerialMon.println("[GPS] enable failed, retrying...");
        delay(1000);
    }
    SerialMon.println("[GPS] GNSS enabled");
#endif

#if FEATURE_LTE
    // SIM 상태 먼저 확인 — 미삽입/잠금이면 등록이 무의미하게 타임아웃된다.
    SerialMon.printf("[LTE] SIM status: %d (1=ready, 2=locked)\n", modem.getSimStatus());
    // SIM PIN이 걸려 있으면 아래 주석 해제: modem.simUnlock("0000");

    // 증분 A: 등록 → PDP → 평문 HTTP GET 으로 LG U+ 유심 데이터패스 검증.
    Led::set(Led::State::PROVISIONING);   // 망 등록/접속 시도 중 — 느린 점멸
    if (Lte::begin(modem, SerialMon)) {
        LteStatus st;
        Lte::status(modem, st);
        Lte::printStatus(st, SerialMon);
        int code = Lte::httpGetCheck(modem, SerialMon);
        if (code == 200) {
            SerialMon.println("[LTE] ✅ 데이터패스 검증 성공 (HTTP 200)");
        } else {
            SerialMon.printf("[LTE] ⚠️ HTTP 응답 %d — 데이터는 되나 응답 확인 필요\n", code);
        }

        // 작업1: 크리덴셜 없으면 서버에서 device_id/pw 자동 발급(/internal/provision).
        if (!Prov::hasCredentials()) {
            SerialMon.println("[PROV] 크리덴셜 없음 → 자동 프로비저닝 시도");
            Prov::ProvResult pr = Prov::provisionOverHttp(modem, SerialMon);
            if (pr == Prov::ProvResult::REJECTED) {
                Led::set(Led::State::REJECTED);   // 403 — allowlist 문제, 재시도 안 함
            } else if (pr == Prov::ProvResult::NETWORK_ERROR) {
                Led::set(Led::State::COMM_ERROR); // loop에서 백오프 재시도
            }
        }

        // 증분 B: MQTT/TLS 접속(서버 CA 검증) + status online/LWT. 크리덴셜 확보 시에만 시도.
        if (Prov::hasCredentials()) {
            if (Mqtt::begin(modem, SerialMon)) {
                SerialMon.println("[MQTT] ✅ 접속 성공 — telemetry 발행 시작");
                Led::set(Led::State::MQTT_OK);    // 정상 접속 — heartbeat
            } else {
                SerialMon.println("[MQTT] ❌ 접속 실패 — 위 로그(CA/네트워크) 확인");
                Led::set(Led::State::COMM_ERROR); // 통신 오류 — 3회 버스트
            }
        } else {
            SerialMon.println("[MQTT] ⏸ 크리덴셜 없음 → MQTT 보류(프로비저닝 후 loop에서 접속)");
        }
    } else {
        SerialMon.println("[LTE] ❌ 브링업 실패 — 위 로그(APN/신호/SIM) 확인");
        Led::set(Led::State::COMM_ERROR);     // 통신 오류 — 3회 버스트
    }

    // 프로비저닝/접속 이후 최종 신원(발급된 device_id/pw 확인).
    printIdentity(modem, "프로비저닝 후");
#endif
}

// ---------------------------------------------------------------------------
void loop()
{
    uint32_t now = millis();   // millis() 오버플로우 안전 비교용

    // 상태표시 LED 패턴 구동(비블로킹, millis 기반). 매 틱 호출.
    Led::update();

    // Debug Console 입력 처리 (setid/setpw/showid/clearid/help) — 논블로킹.
    Prov::handleSerial(SerialMon);

#if (FEATURE_GPS || FEATURE_LTE)
    // 최신 GPS fix 캐시 — telemetry가 참조. 미측위면 valid=false 유지.
    static GpsFix g_lastFix;
#endif

#if FEATURE_GPS
    static uint32_t lastPoll = 0;
    if (now - lastPoll >= GPS_POLL_INTERVAL_MS) {
        lastPoll = now;
        GpsFix fix;
        bool got = Gps::read(modem, fix);

        // 원시 GNSS 정보(+CGNSSINFO) — 측위 전에도 위성 포착 진행상황 확인.
        String rawInfo = Gps::raw(modem);
        SerialMon.print("[GPS] raw: ");
        SerialMon.println(rawInfo.length() ? rawInfo : "(no data)");

        if (got) {
            g_lastFix = fix;                // telemetry용 캐시 갱신
            Gps::print(fix, SerialMon);
        } else {
            SerialMon.println("[GPS] acquiring fix... (cold start may take a while)");
        }
    }
#endif

#if FEATURE_LTE
    // MQTT 콜백/keepalive 펌핑 (매 틱). publish 후 +CMQTTPUB ACK URC를 비운다.
    Mqtt::handle(modem);

    static uint32_t seq = 0;
    static bool     metaSent = false;
    static uint32_t backoff = 0;
    static uint32_t lastConnTry = 0;
    static bool     wasConnected = false;
    static uint32_t nextPubAt = 0;

    bool connected = Mqtt::isConnected(modem);

    static bool provRejected = false;   // 403 래치 — allowlist 문제. 무한 재시도 금지(재부팅/setid로만 해제)

    // 접속 상승엣지: connectSession의 status 발행 ACK가 빠질 시간을 주려
    // 첫 telemetry를 5초 뒤로 잡는다(발행끼리 충돌 방지 — 모뎀은 publish 1건씩).
    if (connected && !wasConnected) {
        nextPubAt = now + 5000;
        Led::set(Led::State::MQTT_OK);          // 재접속 성공 → 정상(heartbeat)
    }
    wasConnected = connected;

    if (!connected) {
        // 미접속: 백오프 간격으로 (재)접속. LTE(PDP)가 죽었으면 먼저 살린다.
        if (now - lastConnTry >= backoff) {
            lastConnTry = now;

            if (provRejected) {
                // allowlist 거절 확정 → 재시도 안 함(LED REJECTED 유지). backoff만 크게.
                backoff = MQTT_RECONNECT_CAP_MS;
            } else {
                Led::set(Led::State::PROVISIONING); // 접속/발급 시도 중 → 느린 점멸
                if (!Lte::isUp(modem)) {
                    SerialMon.println("[LTE] link down — 재브링업");
                    Lte::begin(modem, SerialMon);
                }
                // 크리덴셜 없으면 MQTT 전에 먼저 발급받는다(LTE up 상태에서).
                if (Lte::isUp(modem) && !Prov::hasCredentials()) {
                    Prov::ProvResult pr = Prov::provisionOverHttp(modem, SerialMon);
                    if (pr == Prov::ProvResult::REJECTED) {
                        provRejected = true;
                        Led::set(Led::State::REJECTED);
                    }
                }
                if (!provRejected && Lte::isUp(modem) && Prov::hasCredentials()
                        && Mqtt::begin(modem, SerialMon)) {
                    backoff = 0;   // 성공 → 백오프 리셋 (MQTT_OK는 다음 루프 상승엣지에서 표시)
                } else if (!provRejected) {
                    backoff = backoff ? backoff * 2 : 1000;
                    if (backoff > MQTT_RECONNECT_CAP_MS) backoff = MQTT_RECONNECT_CAP_MS;
                    Led::set(Led::State::COMM_ERROR); // 접속/발급 실패 → 통신오류(3회 버스트)
                }
            }
        }
    } else if ((int32_t)(now - nextPubAt) >= 0) {
        // 접속됨: telemetry 주기 발행(오버플로우 안전 비교). 발행 성공 시 다음 주기 예약,
        // 실패 시 isConnected가 false로 바뀌어 다음 루프에서 재접속 경로를 탄다.
        nextPubAt = now + MQTT_PUBLISH_INTERVAL_MS;
        bool withMeta = !metaSent;   // 최초 1회만 하드웨어 메타 포함
        if (Mqtt::publishTelemetry(modem, g_lastFix, seq, withMeta, SerialMon)) {
            if (withMeta) metaSent = true;
            seq++;
        }
    }
#endif

    // TODO: OBD2 / 차키 / BLE 태스크는 각 단계에서 여기에 추가
    delay(50);
}

#ifndef TINY_GSM_FORK_LIBRARY
#error "TinyGSM fork가 필요합니다. lib/ 디렉토리를 arduino libraries에 복사하세요. README 참고"
#endif
