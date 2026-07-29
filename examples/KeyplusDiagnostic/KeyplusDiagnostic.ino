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
#include <esp_system.h>   // esp_reset_reason() — 부팅 원인 진단

#define TINY_GSM_RX_BUFFER  1024        // RX 버퍼 1KB
#define SerialMon           Serial
#define TINY_GSM_DEBUG      SerialMon
// #define DUMP_AT_COMMANDS             // 필요 시 AT 로그 (StreamDebugger 필요)

#include <TinyGsmClient.h>

// provisioning.h는 TinyGsmClient.h(및 TINY_GSM_RX_BUFFER)보다 뒤에 포함한다
// — provisioning.h가 HTTP 자동 프로비저닝에 TinyGsm 타입을 참조하기 때문.
#include "provisioning.h"
#include "led.h"
#include "cfg.h"
#include "log.h"

#if FEATURE_CARKEY
#include "carkey.h"
#endif

#if FEATURE_GPS
#include "gps.h"
#endif

#include "obd2.h"   // 항상 컴파일(Obd2::Data). 폴링은 FEATURE_OBD2일 때만.

#if FEATURE_LTE
#include "lte.h"
#include "mqtt.h"
#include "cmd.h"
#if FEATURE_OTA
#include "ota.h"
#endif
#endif

#ifdef DUMP_AT_COMMANDS
#include <StreamDebugger.h>
StreamDebugger debugger(SerialAT, SerialMon);
TinyGsm modem(debugger);
#else
TinyGsm modem(SerialAT);
#endif

#if (FEATURE_LTE && FEATURE_OTA)
// 하드 롤백 활성화: arduino-esp32 코어는 기본적으로 부팅 시 esp_ota_mark_app_valid_cancel_rollback()
// 를 자동 호출해 롤백을 무력화한다(esp32-hal-misc.c). 이 weak 심볼을 true로 오버라이드하면
// 자동 확정을 꺼서, 새 OTA 이미지가 PENDING_VERIFY로 남고 우리가 헬스체크(MQTT 접속) 통과 후
// Ota::onHealthy()에서 직접 확정한다. 확정 전 크래시/행(부트 WDT 9s) → 부트로더 자동 롤백.
extern "C" bool verifyRollbackLater() { return true; }
#endif

#if (FEATURE_GPS || FEATURE_LTE)
// 최신 GPS fix 캐시 — telemetry가 참조. 미측위면 valid=false 유지.
static GpsFix g_lastFix;
#endif

// 최신 OBD2 샘플 캐시 — telemetry가 참조. 링크 없으면 valid=false 유지(항상 존재).
static Obd2::Data g_obd;

#if FEATURE_GPS
static uint32_t g_lastGpsPoll = 0;   // 마지막 GPS 폴 시각(주기 폴 / 발행 직전 갱신 공용)
// 직전 폴의 측위 성공 여부 — 획득/상실 전이만 INFO로 남기기 위한 것.
// g_lastFix는 측위 실패해도 마지막 유효 좌표를 유지하므로(telemetry가 계속 참조)
// 전이 판정에 g_lastFix.valid를 쓸 수 없다.
static bool g_gpsFixNow = false;

// GPS 1회 폴 → g_lastFix 갱신. 폴 시각도 같이 갱신하므로, 발행 직전에 호출하면
// 바로 뒤따르는 주기 폴이 중복 실행되지 않는다.
static void pollGps(uint32_t now)
{
    g_lastGpsPoll = now;
    GpsFix fix;
    bool got = Gps::read(modem, fix);

    // 상태 전이만 INFO. 좌표 자체는 [STAT] 한 줄과 DEBUG의 Gps::print에서 본다.
    if (got != g_gpsFixNow) {
        if (got) LOGI(SerialMon, "[GPS] 측위 획득 (fix=%d, %d위성)\n", fix.fixMode, fix.vsat);
        else     LOGI(SerialMon, "[GPS] 측위 상실 — 마지막 좌표 유지\n");
        g_gpsFixNow = got;
    }

    if (got) {
        g_lastFix = fix;                // telemetry용 캐시 갱신
        Gps::print(fix, SerialMon);     // 전체 좌표 덤프는 DEBUG에서만
    } else if (LOG_ON(Log::L_DEBUG)) {
        // 원시 GNSS 정보(+CGNSSINFO)는 미측위일 때만 — 위성 포착 진행상황 확인용이다.
        // fix 확보 후엔 진단 가치가 없는데 모뎀 AT 왕복만 2배가 된다(getGPS + getGPSraw).
        // DEBUG가 아니면 왕복 자체를 생략한다(로그만 끄는 게 아니라 AT도 안 보낸다).
        String rawInfo = Gps::raw(modem);
        SerialMon.print("[GPS] acquiring fix... raw: ");
        SerialMon.println(rawInfo.length() ? rawInfo : "(no data)");
    }
}

// 주행 중 판정 — GPS 폴링 주기를 여기에 맞춘다.
// OBD 링크가 살아있고 차속/회전수가 잡히면 주행, 아니면 정차/주차로 본다.
// 링크가 없으면(시동 OFF/미장착) 위치가 변할 일이 없으므로 정차 취급.
// FEATURE_OBD2 미사용 빌드는 판단 근거가 없으니 항상 주행 취급(기존 주기 유지).
static bool vehicleMoving(const Obd2::Data &d)
{
#if FEATURE_OBD2
    if (!d.valid) return false;
    if (d.has_speed && d.speed > 0) return true;
    if (d.has_rpm   && d.rpm   > 0) return true;
    return false;
#else
    (void)d;
    return true;
#endif
}
#endif  // FEATURE_GPS

// ---------------------------------------------------------------------------
// 부팅 원인 진단 — 차량 상시 5V에서 일정시간 후 LED 1/2번(=재부팅) 증상 추적용.
//   BROWNOUT = 전원 마진 부족(LTE TX 버스트 ~2A 딥). PANIC/WDT = 펌웨어 행/크래시.
//   POWERON/SW = 정상 전원인가/소프트리셋. 매 부팅 첫 줄로 남겨 재부팅 이력을 판별한다.
// ---------------------------------------------------------------------------
static const char *resetReasonStr()
{
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "POWERON(정상 전원인가)";
        case ESP_RST_SW:        return "SW(소프트 리셋/재부팅)";
        case ESP_RST_PANIC:     return "PANIC(예외/크래시)";
        case ESP_RST_INT_WDT:   return "INT_WDT(인터럽트 워치독)";
        case ESP_RST_TASK_WDT:  return "TASK_WDT(태스크 워치독)";
        case ESP_RST_WDT:       return "WDT(기타 워치독)";
        case ESP_RST_BROWNOUT:  return "BROWNOUT(전원 딥 — 전원 마진 부족)";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP(딥슬립 복귀)";
        case ESP_RST_EXT:       return "EXT(외부 리셋핀)";
        default:                return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// 부팅 배너 — 로그 레벨과 무관하게 항상 출력한다.
// "지금 무슨 펌웨어가, 어떤 신원으로, 어떤 설정으로 돌고 있는가"는 ERROR 레벨로
// 조여놓은 단말에서도 알아야 진단이 시작된다.
//
// 2부로 나눈 이유: 모뎀 초기화가 수십 초 걸리거나 아예 실패할 수 있어서,
// 펌웨어 버전/리셋 원인은 모뎀을 건드리기 전에 먼저 남긴다.
// ---------------------------------------------------------------------------
static void printBootHeader()
{
    SerialMon.println();
    SerialMon.println("============================================================");
    SerialMon.printf ("  Keyplus Diagnostic   fw %s\n", FW_VERSION);
    SerialMon.printf ("  build      : %s %s\n", __DATE__, __TIME__);
    SerialMon.printf ("  reset      : %s\n", resetReasonStr());
    SerialMon.println("============================================================");
}

static void printFeatureLine()
{
    SerialMon.print("  기능       :");
#if FEATURE_GPS
    SerialMon.print(" GPS");
#endif
#if FEATURE_OBD2
    SerialMon.print(" OBD2");
#endif
#if FEATURE_CARKEY
    SerialMon.print(" CARKEY");
#endif
#if FEATURE_LTE
    SerialMon.print(" LTE");
#endif
#if FEATURE_STATUS_LED
    SerialMon.print(" LED");
#endif
#if FEATURE_OTA
    SerialMon.print(" OTA");
#endif
#if FEATURE_BLE
    SerialMon.print(" BLE");
#endif
    SerialMon.println();
}

// 모뎀 준비 + 프로비저닝 이후 호출. 하드웨어 신원 + 접속 대상 + 주기 설정 요약.
static void printDeviceInfo(TinyGsm &modem, const char *modemName)
{
    String imei = modem.getIMEI(); imei.trim();
    char mac[13];
    snprintf(mac, sizeof(mac), "%012llX", (unsigned long long)ESP.getEfuseMac());

    SerialMon.println("------------------ 단말 정보 -------------------------------");
    SerialMon.printf("  device_id  : %s (%s)\n", Prov::deviceId().c_str(),
                     Prov::hasValidId() ? "valid" : "none/invalid");
    // 비밀번호 원문은 DEBUG에서만 — 평상시 로그에 크리덴셜을 남기지 않는다.
    if (LOG_ON(Log::L_DEBUG)) {
        SerialMon.printf("  mqtt_pw    : %s\n",
                         Prov::mqttPassword().isEmpty() ? "(none)" : Prov::mqttPassword().c_str());
    } else {
        SerialMon.printf("  mqtt_pw    : %s\n",
                         Prov::mqttPassword().isEmpty() ? "(none)" : "(set)");
    }
    SerialMon.printf("  imei       : %s\n", imei.length() ? imei.c_str() : "(조회 실패)");
    SerialMon.printf("  mac        : %s\n", mac);
    SerialMon.printf("  modem      : %s\n", modemName);
#if FEATURE_LTE
    SerialMon.printf("  sim        : %d (1=ready 2=locked)\n", modem.getSimStatus());
    SerialMon.printf("  broker     : %s:%d (%s)\n", MQTT_HOST, MQTT_PORT,
                     MQTT_USE_TLS ? "TLS" : "평문");
    SerialMon.printf("  telemetry  : %lums   keepalive %ds\n",
                     (unsigned long)Cfg::telemetryIntervalMs(), Cfg::keepaliveS());
#endif
#if FEATURE_OBD2
    SerialMon.printf("  obd poll   : %lums (링크실패 시 %lus부터 백오프)\n",
                     (unsigned long)OBD2_POLL_INTERVAL_MS,
                     (unsigned long)(OBD2_LINK_RETRY_MS / 1000));
#endif
#if FEATURE_GPS
    SerialMon.printf("  gps poll   : %lums(주행) / %lums(정차)\n",
                     (unsigned long)GPS_POLL_INTERVAL_MS, (unsigned long)GPS_POLL_IDLE_MS);
#endif
    printFeatureLine();
    SerialMon.printf("  log level  : %s — 'log debug' 로 상세 출력\n",
                     Log::levelName(Log::level()));
    SerialMon.println("------------------------------------------------------------");
}

// ---------------------------------------------------------------------------
// 주기 상태 한 줄 — INFO 레벨의 기본 출력.
// 모듈별 폴 덤프를 DEBUG로 내린 대신, 이 한 줄만 보고 "지금 정상인가"를 판단할 수 있게 한다.
// ---------------------------------------------------------------------------
static void printStatusLine(bool connected, uint32_t seq, const char *pubResult)
{
    if (!LOG_ON(Log::L_INFO)) return;

    char gpsBuf[24];
#if FEATURE_GPS
    // 측위 중이면 위성수, 놓쳤지만 캐시가 있으면 그 사실까지 구분해 보여준다
    // (telemetry에는 캐시 좌표가 실리므로 "왜 좌표가 안 변하나"를 여기서 알 수 있어야 한다).
    if (g_gpsFixNow)          snprintf(gpsBuf, sizeof(gpsBuf), "fix/%dsat", g_lastFix.vsat);
    else if (g_lastFix.valid) snprintf(gpsBuf, sizeof(gpsBuf), "nofix(캐시)");
    else                      snprintf(gpsBuf, sizeof(gpsBuf), "nofix");
#else
    snprintf(gpsBuf, sizeof(gpsBuf), "off");
#endif

    char obdBuf[40];
    if (!g_obd.valid) {
        snprintf(obdBuf, sizeof(obdBuf), "down");
    } else {
        int n = snprintf(obdBuf, sizeof(obdBuf), "up");
        if (g_obd.has_rpm)   n += snprintf(obdBuf + n, sizeof(obdBuf) - n, " rpm=%.0f", g_obd.rpm);
        if (g_obd.has_speed) n += snprintf(obdBuf + n, sizeof(obdBuf) - n, " spd=%d", g_obd.speed);
    }

#if FEATURE_LTE
    // rssi는 마지막 발행 때 읽어둔 값 — 상태 출력 때문에 AT 왕복이 늘지 않게 한다.
    SerialMon.printf("[STAT] up=%lus mqtt=%s rssi=%d gps=%s obd=%s seq=%lu %s\n",
                     (unsigned long)(millis() / 1000UL),
                     connected ? "on" : "OFF",
                     Mqtt::lastRssi(), gpsBuf, obdBuf,
                     (unsigned long)seq, pubResult);
#else
    (void)connected; (void)seq; (void)pubResult;
    SerialMon.printf("[STAT] up=%lus gps=%s obd=%s\n",
                     (unsigned long)(millis() / 1000UL), gpsBuf, obdBuf);
#endif
}

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
    LOGI(SerialMon, "[MODEM] AT 응답 대기...\n");
    int retry = 0;
    while (!modem.testAT(1000)) {
        LOGD(SerialMon, ".");
        if (retry++ > 30) {
            digitalWrite(BOARD_PWRKEY_PIN, LOW);  delay(100);
            digitalWrite(BOARD_PWRKEY_PIN, HIGH); delay(MODEM_POWERON_PULSE_WIDTH_MS);
            digitalWrite(BOARD_PWRKEY_PIN, LOW);
            retry = 0;
        }
    }
    LOGI(SerialMon, "\n[MODEM] AT 준비됨\n");
}

// 모뎀이 내장 GPS 지원 모델인지 확인. A7670G(GPS 미지원)면 정지.
static String g_modemName = "UNKNOWN";   // 부팅 배너에 실을 모뎀 모델명

static void modemCheckGpsCapable()
{
    String name = "UNKNOWN";
    while (true) {
        name = modem.getModemName();
        if (name == "UNKNOWN") {
            LOGD(SerialMon, "[MODEM] 모델명 조회 실패 — 재시도\n");
            delay(1000);
            continue;
        }
        // A7670G 계열은 내장 측위 미지원 → 외장 GPS 예제 필요.
        if (name.startsWith("A7670G")) {
            while (true) {
                // 하드웨어 자체가 요구사항을 못 맞추는 상황 → 레벨과 무관하게 계속 알린다.
                SerialMon.println("[MODEM] 이 모뎀은 내장 GPS가 없습니다(A7670G). "
                                  "ExternalGPS_A7670G_Only 예제를 쓰세요. 정지.");
                delay(5000);
            }
        }
        g_modemName = name;
        LOGD(SerialMon, "[MODEM] model: %s\n", name.c_str());
        break;
    }
}

// ---------------------------------------------------------------------------
void setup()
{
    SerialMon.begin(115200);
    delay(100);
    // 로그 레벨을 먼저 로드해야 이후 LOGx 호출이 의도한 레벨로 걸러진다.
    Log::begin();
    // 펌웨어 버전과 리셋 원인은 모뎀을 건드리기 전에 남긴다 — 모뎀 초기화에서 멈춰도
    // "무엇이 왜 재부팅했는가"는 알 수 있어야 한다(차량 5V BROWNOUT/WDT 추적).
    printBootHeader();

    // 상태표시 LED — 부팅 진입 즉시 solid ON(살아있음 표시).
    Led::begin();

#if FEATURE_CARKEY
    // 차키 게이트를 즉시 OUTPUT LOW(버튼 뗌)로 확정 — 부팅 초기 플로팅/오동작 방지.
    // 콘솔 'lock'/'unlock'으로 검증(Prov 콘솔 미지명령 분기에서 Carkey::tryConsole 호출).
    Carkey::begin();
#endif

#if FEATURE_OBD2
    // OBD2/CAN 링크 초기화(모뎀 무관, TWAI). 차량 미연결/시동 꺼짐이면 실패 → loop 재시도.
    LOGD(SerialMon, "[OBD2] CAN 링크 초기화...\n");
    Obd2::begin(SerialMon);
#endif

    // 런타임 설정 로드(NVS "cfg") — config_update로 변경된 telemetry 주기/keepalive 복원.
    Cfg::begin();

#if (FEATURE_LTE && FEATURE_OTA)
    // OTA pending 플래그 로드 — 재부팅-후-성공/실패면 접속 후 loop가 ack를 flush.
    Ota::begin();
#endif

    // 단말 신원 로드(NVS). Debug Console에서 'setid vt-...'로 설정 가능('help' 참고).
    Prov::begin();
    Prov::printHelp(SerialMon);
    // 크리덴셜 미확보(device_id/pw 없음) → 미프로비저닝 표시. LTE up 이후 자동 발급(아래).
    if (!Prov::hasCredentials()) Led::set(Led::State::UNPROVISIONED);

    modemPowerOn();
    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
    delay(3000);

    modemWaitReady();

#if FEATURE_GPS
    modemCheckGpsCapable();   // 모델명 확보(배너에 실림) + A7670G(GPS 미지원) 차단
    // GNSS를 먼저 켜서 아래 LTE 등록(최대 수십 초) 대기 동안 위성을 포착하게 한다.
    LOGD(SerialMon, "[GPS] GNSS 활성화...\n");
    while (!Gps::begin(modem)) {
        LOGW(SerialMon, "[GPS] GNSS 활성화 실패 — 재시도\n");
        delay(1000);
    }
    LOGI(SerialMon, "[GPS] GNSS 활성화됨\n");
#endif

    // 하드웨어 신원 + 접속 대상 + 주기 설정 요약. 레벨과 무관하게 항상 출력한다.
    printDeviceInfo(modem, g_modemName.c_str());

#if FEATURE_LTE
    // SIM 상태는 위 배너에 이미 실렸다. SIM PIN이 걸려 있으면 아래 주석 해제:
    // modem.simUnlock("0000");

    // 증분 A: 등록 → PDP → 평문 HTTP GET 으로 LG U+ 유심 데이터패스 검증.
    Led::set(Led::State::PROVISIONING);   // 망 등록/접속 시도 중 — 느린 점멸
    if (Lte::begin(modem, SerialMon)) {
        LteStatus st;
        Lte::status(modem, st);
        Lte::printStatus(st, SerialMon);
        int code = Lte::httpGetCheck(modem, SerialMon);
        if (code == 200) {
            LOGI(SerialMon, "[LTE] 데이터패스 검증 성공 (HTTP 200)\n");
        } else {
            LOGW(SerialMon, "[LTE] HTTP 응답 %d — 데이터는 되나 응답 확인 필요\n", code);
        }

        // 작업1: 크리덴셜 없으면 서버에서 device_id/pw 자동 발급(/internal/provision).
        if (!Prov::hasCredentials()) {
            LOGI(SerialMon, "[PROV] 크리덴셜 없음 → 자동 프로비저닝 시도\n");
            Prov::ProvResult pr = Prov::provisionOverHttp(modem, SerialMon);
            if (pr == Prov::ProvResult::REJECTED) {
                Led::set(Led::State::REJECTED);   // 403 — allowlist 문제, 재시도 안 함
            } else if (pr == Prov::ProvResult::NETWORK_ERROR) {
                Led::set(Led::State::COMM_ERROR); // loop에서 백오프 재시도
            }
        }

        // cmd 다운링크 RX 콜백 등록(부팅당 1회). 구독은 접속 성공 뒤에 한다.
        Cmd::begin(modem);

        // 증분 B: MQTT/TLS 접속(서버 CA 검증) + status online/LWT. 크리덴셜 확보 시에만 시도.
        if (Prov::hasCredentials()) {
            if (Mqtt::begin(modem, SerialMon)) {
                LOGI(SerialMon, "[MQTT] telemetry 발행 시작\n");
                Led::set(Led::State::MQTT_OK);    // 정상 접속 — heartbeat
                Cmd::subscribe(modem, SerialMon); // v1/{id}/cmd 구독
                // OTA pending ack는 loop에서 hasPending() 가드로 flush(접속 확인 후).
            } else {
                LOGW(SerialMon, "[MQTT] 접속 실패 — 위 로그(CA/네트워크) 확인\n");
                Led::set(Led::State::COMM_ERROR); // 통신 오류 — 3회 버스트
            }
        } else {
            LOGI(SerialMon, "[MQTT] 크리덴셜 없음 → 보류(프로비저닝 후 loop에서 접속)\n");
        }
    } else {
        LOGE(SerialMon, "[LTE] 브링업 실패 — 위 로그(APN/신호/SIM) 확인\n");
        Led::set(Led::State::COMM_ERROR);     // 통신 오류 — 3회 버스트
    }

    // 발급이 일어났으면 최종 device_id를 한 줄로 확인(전체 표는 위 배너에 이미 있음).
    LOGI(SerialMon, "[PROV] device_id=%s (%s)\n", Prov::deviceId().c_str(),
         Prov::hasValidId() ? "valid" : "INVALID");
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

#if FEATURE_OBD2
    // OBD2 폴링: 링크 있으면 주기 수집, 없으면 백오프 간격으로 재확립 시도.
    static uint32_t lastObdPoll = 0, lastObdRetry = 0;
    // 재확립 시도는 실패할 때마다 간격을 2배로 늘린다. Obd2::begin은 비트레이트 2종을
    // 무응답 타임아웃까지 시도하므로 1회에 수백 ms 블로킹 + 드라이버 install/uninstall 이
    // 따른다. 주차 중(시동 OFF)엔 이게 몇 시간이고 반복되므로 고정 30초는 과하다.
    static uint32_t obdRetryDelay = OBD2_LINK_RETRY_MS;
    if (Obd2::isInstalled()) {
        if (now - lastObdPoll >= OBD2_POLL_INTERVAL_MS) {
            lastObdPoll = now;
            if (Obd2::read(g_obd, SerialMon)) {
                Obd2::print(g_obd, SerialMon);   // 응답받은 전체 항목 + VIN 출력
            } else {
                // 폴 전체 무응답 → 링크 끊김(시동 off 등) → 재확립 경로로.
                LOGI(SerialMon, "[OBD2] 링크 끊김(폴 무응답) — 재확립 대기\n");
                Obd2::end();
                g_obd = Obd2::Data();   // valid=false → telemetry에서 obd 생략
                lastObdRetry = now;
                // 방금까지 살아있던 링크다. 일시적 글리치일 수 있으니 기본 간격부터 다시.
                obdRetryDelay = OBD2_LINK_RETRY_MS;
            }
        }
    } else if (now - lastObdRetry >= obdRetryDelay) {
        lastObdRetry = now;
        if (Obd2::begin(SerialMon)) {   // 성공 시 다음 틱부터 폴링
            obdRetryDelay = OBD2_LINK_RETRY_MS;
        } else {
            obdRetryDelay *= 2;
            if (obdRetryDelay > OBD2_LINK_RETRY_CAP_MS) obdRetryDelay = OBD2_LINK_RETRY_CAP_MS;
            LOGD(SerialMon, "[OBD2] 다음 재시도 %lus 후\n", (unsigned long)(obdRetryDelay / 1000));
        }
    }
#endif

#if FEATURE_GPS
    // 주기 폴 — 주행 중엔 GPS_POLL_INTERVAL_MS, 정차/주차 중엔 GPS_POLL_IDLE_MS.
    // 발행 직전에도 갱신하므로(아래 telemetry 분기) 이 주기 폴은 측위 유지와
    // 콜드스타트 진행 확인이 목적이다. 주차 중엔 좌표가 변하지 않으니 느슨하게 둔다.
    uint32_t gpsInterval = vehicleMoving(g_obd) ? GPS_POLL_INTERVAL_MS : GPS_POLL_IDLE_MS;
    if (now - g_lastGpsPoll >= gpsInterval) pollGps(now);
#endif

#if FEATURE_LTE
    // MQTT 콜백/keepalive 펌핑 (매 틱). publish 후 +CMQTTPUB ACK URC를 비운다.
    Mqtt::handle(modem);
    Cmd::handle(modem, SerialMon);   // 수신 명령 처리 + ack 발행

    static uint32_t seq = 0;
    static bool     metaSent = false;
    static uint32_t backoff = 0;
    static uint32_t lastConnTry = 0;
    static bool     wasConnected = false;
    static uint32_t nextPubAt = 0;
    static uint32_t lastStatAt = 0;   // [STAT] 마지막 출력 시각(유휴 주기 출력용)

    bool connected = Mqtt::isConnected(modem);

    static bool provRejected = false;   // 403 래치 — allowlist 문제. 무한 재시도 금지(재부팅/setid로만 해제)

    // 접속 상승엣지: connectSession의 status 발행 ACK가 빠질 시간을 주려
    // 첫 telemetry를 5초 뒤로 잡는다(발행끼리 충돌 방지 — 모뎀은 publish 1건씩).
    if (connected && !wasConnected) {
        nextPubAt = now + 5000;
        LOGI(SerialMon, "[MQTT] 재접속됨\n");
        Led::set(Led::State::MQTT_OK);          // 재접속 성공 → 정상(heartbeat)
        Cmd::subscribe(modem, SerialMon);       // clean_session=1 → 재접속마다 재구독
    }
    if (!connected && wasConnected) {
        // 끊김은 상태 전이 — 조용히 백오프에 들어가면 로그만 보고는 멈춘 것과 구분이 안 된다.
        LOGW(SerialMon, "[MQTT] 연결 끊김 — 백오프 재접속 시작\n");
    }
    wasConnected = connected;

#if FEATURE_OTA
    // 하드 롤백 期限 감시(매 틱): PENDING_VERIFY인데 접속 못 하고 期限 초과면 강제 롤백 재부팅.
    Ota::tick(SerialMon);

    if (connected) {
        // 헬스체크 통과 → 새 이미지 확정(mark_valid, 1회). 부트로더 롤백 취소.
        Ota::onHealthy(SerialMon);
        // OTA 결과 ack: pending이 있으면 발행(성공=버전검증 done / 실패=failed) 후 소거.
        // 롤백된 경우엔 구 이미지가 버전 불일치로 failed를 낸다.
        if (Ota::hasPending()) Ota::flushPendingAck(modem, SerialMon);
    }
#endif

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
                    LOGW(SerialMon, "[LTE] 링크 다운 — 재브링업\n");
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
        // 주기는 config_update로 런타임 변경 가능(Cfg::telemetryIntervalMs).
        nextPubAt = now + Cfg::telemetryIntervalMs();
#if FEATURE_GPS
        // 발행 직전 좌표 갱신 — 주기 폴에만 의존하면 실을 fix가 최대 폴 주기만큼 묵는다
        // (주행 중 10초 = 시속 100km에서 약 280m 오차). 폴 시각도 갱신되므로 중복 폴 없음.
        pollGps(now);
#endif
        bool withMeta = !metaSent;   // 최초 1회만 하드웨어 메타 포함
        bool pubOk = Mqtt::publishTelemetry(modem, g_lastFix, g_obd, seq, withMeta, SerialMon);
        if (pubOk) {
            if (withMeta) metaSent = true;
            seq++;
        }
        // 주기 상태 한 줄 — 모듈별 폴 덤프를 DEBUG로 내린 대신 여기서 전체 상태를 요약한다.
        printStatusLine(true, seq, pubOk ? "pub-ok" : "pub-FAIL");
        lastStatAt = now;
    }

    // 미접속이 길어지면 [STAT]가 아예 안 나와 멈춘 것처럼 보인다 → 유휴에도 주기적으로 남긴다.
    if (!connected && now - lastStatAt >= STATUS_LINE_IDLE_MS) {
        lastStatAt = now;
        printStatusLine(false, seq, "-");
    }
#endif

    // TODO: OBD2 / 차키 / BLE 태스크는 각 단계에서 여기에 추가
    delay(50);
}

#ifndef TINY_GSM_FORK_LIBRARY
#error "TinyGSM fork가 필요합니다. lib/ 디렉토리를 arduino libraries에 복사하세요. README 참고"
#endif
