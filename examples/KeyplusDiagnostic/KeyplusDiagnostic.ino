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

#if FEATURE_GPS
#include "gps.h"
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
    SerialMon.println("\n=== Keyplus Diagnostic — phase 2 (GPS) ===");

    modemPowerOn();
    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
    delay(3000);

    modemWaitReady();
    modemCheckGpsCapable();

#if FEATURE_GPS
    SerialMon.println("[GPS] enabling GNSS...");
    while (!Gps::begin(modem)) {
        SerialMon.println("[GPS] enable failed, retrying...");
        delay(1000);
    }
    SerialMon.println("[GPS] GNSS enabled");
#endif
}

// ---------------------------------------------------------------------------
void loop()
{
#if FEATURE_GPS
    static uint32_t lastPoll = 0;
    uint32_t now = millis();
    // millis() 오버플로우 안전 비교
    if (now - lastPoll >= GPS_POLL_INTERVAL_MS) {
        lastPoll = now;
        GpsFix fix;
        bool got = Gps::read(modem, fix);

        // 원시 GNSS 정보(+CGNSSINFO) — 측위 전에도 위성 포착 진행상황 확인.
        String rawInfo = Gps::raw(modem);
        SerialMon.print("[GPS] raw: ");
        SerialMon.println(rawInfo.length() ? rawInfo : "(no data)");

        if (got) {
            Gps::print(fix, SerialMon);
            // TODO(3단계): fix를 서버 전송 큐 / SD 오프라인 버퍼로 전달
        } else {
            SerialMon.println("[GPS] acquiring fix... (cold start may take a while)");
        }
    }
#endif

    // TODO: OBD2 / 차키 / BLE 태스크는 각 단계에서 여기에 추가
    delay(50);
}

#ifndef TINY_GSM_FORK_LIBRARY
#error "TinyGSM fork가 필요합니다. lib/ 디렉토리를 arduino libraries에 복사하세요. README 참고"
#endif
