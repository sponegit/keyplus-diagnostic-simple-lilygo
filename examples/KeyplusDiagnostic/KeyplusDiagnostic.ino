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
#include "provisioning.h"

#define TINY_GSM_RX_BUFFER  1024        // RX 버퍼 1KB
#define SerialMon           Serial
#define TINY_GSM_DEBUG      SerialMon
// #define DUMP_AT_COMMANDS             // 필요 시 AT 로그 (StreamDebugger 필요)

#include <TinyGsmClient.h>

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

// ---------------------------------------------------------------------------
void setup()
{
    SerialMon.begin(115200);
    delay(100);
    SerialMon.println("\n=== Keyplus Diagnostic — GPS + LTE bring-up ===");

    // 단말 신원 로드(NVS). Debug Console에서 'setid vt-...'로 설정 가능('help' 참고).
    Prov::begin();
    SerialMon.printf("[PROV] device_id=%s (%s)\n",
                     Prov::deviceId().c_str(), Prov::hasValidId() ? "valid" : "INVALID");
    Prov::printHelp(SerialMon);

    modemPowerOn();
    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
    delay(3000);

    modemWaitReady();

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

        // 증분 B: MQTT/TLS 접속(서버 CA 검증) + status online/LWT. telemetry는 loop에서 발행.
        if (Mqtt::begin(modem, SerialMon)) {
            SerialMon.println("[MQTT] ✅ 접속 성공 — telemetry 발행 시작");
        } else {
            SerialMon.println("[MQTT] ❌ 접속 실패 — 위 로그(CA/네트워크) 확인");
        }
    } else {
        SerialMon.println("[LTE] ❌ 브링업 실패 — 위 로그(APN/신호/SIM) 확인");
    }
#endif
}

// ---------------------------------------------------------------------------
void loop()
{
    uint32_t now = millis();   // millis() 오버플로우 안전 비교용

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

    // 접속 상승엣지: connectSession의 status 발행 ACK가 빠질 시간을 주려
    // 첫 telemetry를 5초 뒤로 잡는다(발행끼리 충돌 방지 — 모뎀은 publish 1건씩).
    if (connected && !wasConnected) {
        nextPubAt = now + 5000;
    }
    wasConnected = connected;

    if (!connected) {
        // 미접속: 백오프 간격으로 (재)접속. LTE(PDP)가 죽었으면 먼저 살린다.
        if (now - lastConnTry >= backoff) {
            lastConnTry = now;
            if (!Lte::isUp(modem)) {
                SerialMon.println("[LTE] link down — 재브링업");
                Lte::begin(modem, SerialMon);
            }
            if (Lte::isUp(modem) && Mqtt::begin(modem, SerialMon)) {
                backoff = 0;   // 성공 → 백오프 리셋
            } else {
                backoff = backoff ? backoff * 2 : 1000;
                if (backoff > MQTT_RECONNECT_CAP_MS) backoff = MQTT_RECONNECT_CAP_MS;
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
