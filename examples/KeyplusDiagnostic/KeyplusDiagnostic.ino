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
#include "console.h"      // DbgConsole — U0(USB-C) + 보조 UART 동시 출력/입력
#include <esp_system.h>   // esp_reset_reason() — 부팅 원인 진단

#define TINY_GSM_RX_BUFFER  1024        // RX 버퍼 1KB
// 콘솔은 두 포트(USB-C / 외부 USB-TTL)에 동시에 붙는다 — console.h 참고.
// TinyGSM 디버그 출력도 같은 스트림을 타므로 외부 UART에서도 다 보인다.
#define SerialMon           DbgConsole
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
#include "clk.h"    // 시각 기준(base) — 오프라인 적재/고빈도 창이 공유한다

#if FEATURE_FAST_SAMPLE
#include "fast.h"
#endif
#if FEATURE_OFFLINE_BUF
#include "buffer.h"
#endif

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
// 직전 폴의 측위 성공 여부 — 획득/상실 전이 로그와 telemetry ts 선택에 쓴다.
// g_lastFix는 측위 실패해도 마지막 유효 좌표를 유지하므로(telemetry가 계속 참조)
// "지금 측위 중인가"를 g_lastFix.valid로는 알 수 없다. GPS 미빌드에서도 telemetry가
// 참조하므로(그때는 항상 false → 모뎀 시각 폴백) g_lastFix와 같은 가드에 둔다.
static bool g_gpsFixNow = false;
#endif

// 최신 OBD2 샘플 캐시 — telemetry가 참조. 링크 없으면 valid=false 유지(항상 존재).
static Obd2::Data g_obd;

// ---------------------------------------------------------------------------
// loop 진행 상태 — 원래 loop() 안의 static 이었지만, 콘솔 'status' 명령이 읽어야 해서
// 파일 스코프로 올렸다(진단 시 "다음 발행까지 몇 초?", "백오프 어디까지 갔나"를 봐야 한다).
// ---------------------------------------------------------------------------
#if FEATURE_OBD2
static uint32_t g_lastObdPoll  = 0;
static uint32_t g_lastObdRetry = 0;
#endif

#if FEATURE_LTE
static uint32_t g_seq          = 0;
static bool     g_metaSent     = false;
static uint32_t g_backoff      = 0;
static uint32_t g_lastConnTry  = 0;
static bool     g_wasConnected = false;
static uint32_t g_nextPubAt    = 0;
static uint32_t g_lastStatAt   = 0;   // [STAT] 마지막 출력 시각(유휴 주기 출력용)
// 403 래치 — allowlist 문제. 무한 재시도 금지(재부팅/setid로만 해제).
static bool     g_provRejected = false;
// cmd 구독 페이싱 — 접속 직후 지연 + 실패 시 지수 백오프 재시도.
static uint32_t g_nextSubAt     = 0;
static uint32_t g_subRetryDelay = 0;
// LTE 재브링업 페이싱/복구 상태.
//   연속 실패마다 간격을 늘리고(전원이 약할 때 모뎀이 쉬는 시간을 준다),
//   LTE_FAIL_BEFORE_RESET 회에 닿으면 모뎀 자체를 리셋한다.
static uint32_t g_lteRetryDelay = 0;
static uint32_t g_lastLteTry    = 0;
static int      g_lteFailStreak = 0;
// 접속이 성립한 시각 — 백필 시작 지연(BACKFILL_START_DELAY_MS)의 기준.
// 접속 직후는 구독·OTA ack·첫 telemetry 가 몰리는 구간이라 백필을 끼워넣지 않는다.
static uint32_t g_connectedAt   = 0;
// 모뎀 생존 프로브(R1) — 발행과 무관하게 세션 사망을 잡는다.
static uint32_t g_lastProbeAt     = 0;
static int      g_probeFailStreak = 0;

// --- status 전이 재발행 (F1) + 구독 완료 통지 (F3) --------------------------
// status 는 원래 접속당 1회(connectSession)라 시동 on/off 를 따라가지 못했다. 그래서
// power_mode·ignition_on 이 DB 기본값에서 한 번도 움직이지 않았고, 서버가 telemetry 의
// obd·rpm 에서 파생하는 차선책을 쓰고 있었다(30초 지연 + 출처 이중화).
//   → (OBD 링크, rpm>0) 이 **바뀔 때만** 재발행해 출처를 계약대로 단말로 되돌린다.
// 감지는 OBD 폴 직후에 하고 발행은 아래 발행 체인에서 한다 — 그래야 telemetry·고빈도
// 창·백필과 같은 우선순위/간격 규칙을 그대로 받는다(모뎀은 publish 가 겹치면 깨진다).
static bool     g_stPrevLink   = false;  // 직전 관측 (OBD 링크 있음)
static bool     g_stPrevIgn    = false;  // 직전 관측 (rpm > 0)
static bool     g_stInit       = false;  // 최초 1회는 전이로 친다(retained 값 확정)
static bool     g_stPending    = false;  // 발행 대기
static bool     g_stSubNotify  = false;  // 이번 발행에 sub:true 를 얹는다(F3)
#endif

#if FEATURE_OFFLINE_BUF
// 다음 오프라인 적재 시각. 주행 30초 / 정차 120초로 갈아탄다.
static uint32_t g_nextLogAt     = 0;
// 백필 페이싱 — 틱당 1건, 최소 BACKFILL_MIN_GAP_MS 간격.
static uint32_t g_backfillNextAt = 0;
// 시각 복원 불가로 폐기한 건수(§4.2) — 조용히 사라지지 않게 [STAT]/status 에 노출한다.
static uint32_t g_bfDiscarded    = 0;
// 시각 미확보로 백필을 미룬 횟수. 폐기가 아니라 대기다 — 시각이 오면 그대로 복원된다.
static uint32_t g_bfNoClock      = 0;
#endif

#if FEATURE_FAST_SAMPLE
// 고빈도 창 발행 예약 시각(0 = 예약 없음). 실시간 telemetry 발행 직후에 잡힌다.
static uint32_t g_nextFastAt    = 0;
#endif

#if FEATURE_LTE
// status 전이 재발행(F1/F3)을 지금 내도 되는가.
//   ① 발행 대기 플래그가 서 있어야 한다(전이 없으면 한 건도 안 나간다 — 주기 발행 아님)
//   ② 직전 발행과 PUBLISH_MIN_GAP_MS 이상 벌어져야 한다(발행 체인 공통 가드)
//   ③ 다음 실시간 telemetry 가 STATUS_REPUB_GUARD_MS 안에 있으면 미룬다
// ③ 이 필요한 이유: 발행 체인은 telemetry 를 먼저 보지만, telemetry 예정 시각 직전에
// status 를 끼워넣으면 그 둘이 수십 ms 간격으로 붙는다. 전이가 늦어봐야 그만큼이고
// (합격 기준 5~10초 안), 세션이 깨지는 쪽이 훨씬 비싸다.
static bool statusGateOpen(uint32_t now)
{
    if (!g_stPending) return false;
    if (!Mqtt::publishGapElapsed(now, PUBLISH_MIN_GAP_MS))             return false;
    if ((int32_t)(g_nextPubAt - now) < (int32_t)STATUS_REPUB_GUARD_MS) return false;
    return true;
}

// 계약상 power_mode 는 단말이 보고하는 값이다. OBD 링크 유무로만 정한다
// (rpm 은 ignition_on 쪽 — 키 ACC/ON 이면 CAN 은 살아 있고 rpm 만 0 이다).
// ⚠️ parked_deep 은 쓰지 않는다. 그건 "차가 꺼짐"이 아니라 **"단말이 잠듦"**을 뜻하고
//    (서버 TTL 300초·estimated_delivery "within_3_min"), 이 단말은 상시 12V 라 차가
//    꺼져도 계속 접속돼 1초 안에 응답한다. 주차 중 전 차량이 "3분 내 배달"로 나가면
//    그건 거짓말이다 — 실제 단말 절전을 도입할 때까지 비워둔다.
// ⚠️ 살아 있는 g_obd.valid 가 아니라 전이 감지 시점의 스냅샷(g_stPrevLink)을 쓴다.
//    감지와 발행 사이에 최대 1~2초가 벌어지는데(간격 가드), 그 사이 링크가 바뀌면
//    power_mode 는 새 값, ignition_on 은 옛 값이 되어 한 발행 안에서 어긋난다.
static const char *statusPowerMode()
{
    return g_stPrevLink ? "driving" : "parked_active";
}

// 모뎀이 내부 리셋된 정황을 잡았을 때의 공통 처리 — 펌웨어 플래그를 모뎀 실제 상태와
// 다시 맞추고, 재접속 게이트를 즉시 연다. 감지 경로가 여럿이라(생존 프로브 R1,
// GNSS 꺼짐 R2, 브링업 실패 R4) 한 군데로 모은다.
// ⚠️ Mqtt::resetServiceState 는 s_serviceStarted 도 내린다. 모뎀이 재부팅되면
//    AT+CMQTTSTART 로 띄운 CMQTT 서비스가 함께 사라지므로, 이걸 내려주지 않으면
//    begin() 이 CMQTTSTART 를 건너뛰고 영영 "접속 실패"만 반복한다.
static void noteModemRestarted(const char *why)
{
    LOGW(SerialMon, "[MODEM] 리셋 정황 감지(%s) — 세션 사망 처리·재접속 즉시 시도\n", why);
    Mqtt::resetServiceState();
    Cmd::markUnsubscribed();
    // 쌓인 재접속 백오프(최대 15초)를 그대로 쓰면 이미 확인된 사망을 그만큼 더 기다린다.
    // LTE 재브링업 백오프(g_lteRetryDelay)는 건드리지 않는다 — 그건 "등록이 안 붙는"
    // 전원 마진 상황을 위한 페이싱이라, 여기서 풀면 브라운아웃을 몰아치게 만든다.
    g_backoff = 0;
}
#endif

#if FEATURE_GPS
static uint32_t g_lastGpsPoll = 0;   // 마지막 GPS 폴 시각(주기 폴 / 발행 직전 갱신 공용)
// 연속 미측위 횟수 — GNSS 가 꺼져 있는지 확인할 시점을 잡는 데 쓴다.
// ⚠️ 모뎀이 리셋되면(소프트/하드) AT+CGNSSPWR 이 0으로 돌아가 GNSS 가 꺼진다.
//    Gps::begin()은 setup 에서 한 번뿐이라, 재활성화가 없으면 그 뒤로 영구 미측위다.
static int      g_gpsFailStreak = 0;
static uint32_t g_gpsNofixSince = 0;   // 미측위가 시작된 시각(진행 리포트용)
static uint32_t g_gpsNofixLogAt = 0;   // 마지막 미측위 리포트 시각
// GNSS 재활성화 보류 플래그(R3) — 모뎀이 무응답이라 지금 켤 수 없을 때 세운다.
// Gps::begin 은 최대 15초 블로킹이라, 죽은 모뎀에 던지면 복구 경로를 그만큼 밀어낸다.
static bool     g_gpsReenablePending = false;

// GPS 1회 폴 → g_lastFix 갱신. 폴 시각도 같이 갱신하므로, 발행 직전에 호출하면
// 바로 뒤따르는 주기 폴이 중복 실행되지 않는다.
static void pollGps(uint32_t now)
{
    g_lastGpsPoll = now;

#if FEATURE_LTE
    // 모뎀 무응답으로 미뤄둔 GNSS 재활성화(R3). 모뎀이 살아났으면 여기서 되살린다.
    // 실패하면 플래그를 유지해 다음 폴에 다시 시도한다.
    if (g_gpsReenablePending && Lte::modemAlive(modem, MODEM_PROBE_TIMEOUT_MS)) {
        if (Gps::begin(modem)) {
            g_gpsReenablePending = false;
            LOGI(SerialMon, "[GPS] GNSS 재활성화됨(모뎀 복구 후)\n");
        } else {
            LOGW(SerialMon, "[GPS] GNSS 재활성화 실패 — 다음 폴에 재시도\n");
        }
    }
#endif

    GpsFix fix;
    bool got = Gps::read(modem, fix);

    // 상태 전이만 INFO. 좌표 자체는 [STAT] 한 줄과 DEBUG의 Gps::print에서 본다.
    if (got != g_gpsFixNow) {
        if (got) LOGI(SerialMon, "[GPS] 측위 획득 (fix=%d, %d위성)\n", fix.fixMode, fix.vsat);
        else     LOGI(SerialMon, "[GPS] 측위 상실 — 마지막 좌표 유지\n");
        g_gpsFixNow = got;
        if (!got) { g_gpsNofixSince = now; g_gpsNofixLogAt = now; }
    }

    if (got) {
        g_gpsFailStreak = 0;
        g_lastFix = fix;                // telemetry용 캐시 갱신
        // 시각 기준 갱신 — ⚠️ 오프라인 구간에서는 이 경로가 유일한 권위 시각 공급원이다
        // (telemetry 발행은 접속 중에만 돈다). 여기서 note 하지 않으면 음영 주행 내내
        // base 가 낡아 오프라인 레코드의 ts 가 전부 외삽값이 된다.
        if (fix.year >= 2020) {
            Clk::noteCivil(fix.year, fix.month, fix.day,
                           fix.hour, fix.minute, fix.second, Clk::SRC_GPS);
        }
        Gps::print(fix, SerialMon);     // 전체 좌표 덤프는 DEBUG에서만
        return;
    }

    // ---- 이하 미측위 경로 ----
    if (g_gpsNofixSince == 0) g_gpsNofixSince = now;   // 부팅부터 미측위인 경우

    // GNSS 가 실제로 켜져 있는지 확인. 매 폴마다 AT 를 더 쏘면 낭비라 연속 실패
    // 임계에 닿았을 때만 본다. 꺼져 있으면(모뎀 리셋) 다시 켠다.
    if (++g_gpsFailStreak >= GPS_ENSURE_AFTER_FAILS) {
        g_gpsFailStreak = 0;
        if (!Gps::isEnabled(modem)) {
            LOGW(SerialMon, "[GPS] GNSS 가 꺼져 있음(모뎀 리셋 추정) — 재활성화\n");
#if FEATURE_LTE
            // GNSS 가 꺼졌다 = AT+CGNSSPWR 이 0 으로 돌아갔다 = 모뎀이 리부팅됐다.
            // 260804 로그에서 이 신호는 매 사이클 publish 실패보다 55초 먼저 떴는데,
            // 여기서 GNSS 만 되살리고 끝내는 바람에 그 55초를 그냥 흘려보냈다.
            // MQTT 세션도 같이 죽었다고 보고 재접속 경로를 즉시 연다(R2).
            noteModemRestarted("GNSS 꺼짐");
#endif
            // ⚠️ 모뎀이 살아 있을 때만 켠다(R3). Gps::begin 은 enableGPS 를 최대 15초
            //    폴링하는데(gps.cpp), AT 자체를 못 받는 모뎀에 던지면 그 15초 + AT 대기가
            //    통째로 복구 경로를 밀어낸다 — 실측 사이클당 30초 × 2회를 버렸다.
            //    죽었으면 재활성화는 복구된 뒤로 미룬다.
            if (!Lte::modemAlive(modem, MODEM_PROBE_TIMEOUT_MS)) {
                LOGW(SerialMon, "[GPS] 모뎀 무응답 — GNSS 재활성화는 복구 후로 미룸\n");
                g_gpsReenablePending = true;
            } else if (Gps::begin(modem)) {
                LOGI(SerialMon, "[GPS] GNSS 재활성화됨\n");
            } else {
                LOGE(SerialMon, "[GPS] GNSS 재활성화 실패 — 다음 폴에 재시도\n");
            }
        }
    }

    // 미측위는 INFO 에서 완전히 침묵했다 — 폴이 도는지조차 로그로 알 수 없었다.
    // INFO 는 GPS_NOFIX_REPORT_MS 주기로 한 줄, DEBUG 는 매 폴 남긴다.
    // 원시 +CGNSSINFO 는 미측위일 때만 조회한다 — fix 확보 후엔 진단 가치가 없는데
    // 모뎀 AT 왕복만 2배가 된다(getGPS + getGPSraw).
    bool reportNow = (now - g_gpsNofixLogAt >= GPS_NOFIX_REPORT_MS);
    if (reportNow || LOG_ON(Log::L_DEBUG)) {
        String rawInfo = Gps::raw(modem);
        const char *rawStr = rawInfo.length() ? rawInfo.c_str()
                                             : "(응답 없음 — 안테나/GNSS 확인)";
        if (reportNow) {
            g_gpsNofixLogAt = now;
            LOGI(SerialMon, "[GPS] 미측위 %lus 경과 — raw: %s\n",
                 (unsigned long)((now - g_gpsNofixSince) / 1000UL), rawStr);
        } else {
            LOGD(SerialMon, "[GPS] acquiring fix... raw: %s\n", rawStr);
        }
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
// 부팅 배너 — 로그 레벨과 무관하게 항상 출력한다. 콘솔 'info' 명령도 이걸 재사용한다.
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
    // build 표기는 보류 — __DATE__/__TIME__ 은 이 .ino 가 실제로 컴파일된 시각이라
    // 증분 빌드에서 다른 파일만 고치면 갱신되지 않는다. 낡은 값이 최신 빌드처럼 보여
    // 오히려 오해를 부르므로, 정확한 주입 방법(platformio.ini 타임스탬프)이 정해지기
    // 전까지 끈다. 지금 스냅샷 식별은 fw 버전(FW_VERSION)으로 한다.
    // SerialMon.printf ("  build      : %s %s\n", __DATE__, __TIME__);
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
    SerialMon.printf("  obd poll   : %lums (링크실패 시 %lus 간격 재시도)\n",
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
#if FEATURE_AUX_CONSOLE
    SerialMon.printf("  aux console: TX=GPIO%d RX=GPIO%d @%d (USB-C와 동시 사용 가능)\n",
                     AUX_CONSOLE_TX_PIN, AUX_CONSOLE_RX_PIN, AUX_CONSOLE_BAUD);
#endif
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

    // ⚠️ buf/fast 는 반드시 key=value 토큰이고 위치는 obd 그룹 뒤·seq 앞으로 고정한다.
    //    UART 진단 앱(main.py)의 STAT_KEYS 가 닫힌 목록이라 미등록 토큰은 앞 필드 값에
    //    통째로 흡수된다 — 신규 필드가 안 보이는 정도가 아니라 obd/seq 표시가 깨진다.
    //    앱의 STAT_KEYS 확장(U1)이 이 펌웨어 배포의 선결 조건이다.
    //    비어 있어도 buf=0 을 낸다(키가 사라지면 앱이 축약형과 구분하지 못한다).
#if FEATURE_OFFLINE_BUF
    unsigned long bufN = (unsigned long)Buf::count();
#else
    unsigned long bufN = 0;
#endif
#if FEATURE_FAST_SAMPLE
    int fastN = Fast::count();
#else
    int fastN = 0;
#endif

#if FEATURE_LTE
    // 접속 중엔 마지막 발행 때 읽어둔 값을 재사용해 AT 왕복을 늘리지 않는다.
    // 미접속이면 그 값이 끊길 당시의 낡은 수치(대개 99=측정불가)로 굳어 오해를 부르므로,
    // 30초에 한 번뿐인 유휴 [STAT] 에서는 신선하게 읽는다.
    int rssi = connected ? Mqtt::lastRssi() : modem.getSignalQuality();
    SerialMon.printf("[STAT] up=%lus mqtt=%s rssi=%d gps=%s obd=%s buf=%lu fast=%d seq=%lu %s\n",
                     (unsigned long)(millis() / 1000UL),
                     connected ? "on" : "OFF",
                     rssi, gpsBuf, obdBuf, bufN, fastN,
                     (unsigned long)seq, pubResult);
#else
    (void)connected; (void)seq; (void)pubResult;
    SerialMon.printf("[STAT] up=%lus gps=%s obd=%s buf=%lu fast=%d\n",
                     (unsigned long)(millis() / 1000UL), gpsBuf, obdBuf, bufN, fastN);
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

#if (FEATURE_LTE || FEATURE_GPS)
// 모뎀 하드 리셋 — PWRKEY 전원 사이클 후 AT 재개까지 제한 시간만 대기.
// 소프트 리셋(AT+CFUN=1,1)조차 안 먹는 상태(모뎀이 AT를 아예 못 받음)의 마지막 수단이다.
// 회복이 안 되면 포기하고(false) 호출측이 다음 주기에 다시 시도한다 — setup/loop 어느 쪽에서
// 불러도 여기서 무한 대기하지 않는다.
static bool modemHardReset()
{
    LOGW(SerialMon, "[MODEM] 하드 리셋(PWRKEY 전원 사이클)\n");
    modemPowerOn();
    uint32_t start = millis();
    while (millis() - start < LTE_MODEM_RESET_WAIT_MS) {
        if (modem.testAT(1000)) {
            LOGI(SerialMon, "[MODEM] 하드 리셋 후 AT 준비됨 (%lus)\n",
                 (unsigned long)((millis() - start) / 1000UL));
            return true;
        }
        delay(500);
    }
    LOGE(SerialMon, "[MODEM] 하드 리셋 후에도 무응답 — 전원 마진(5V 공급/배선) 확인 필요\n");
    return false;
}
#endif

// AT 응답이 올 때까지 대기. 30회 실패 시 PWRKEY 재펄스.
// MODEM_AT_READY_TIMEOUT_MS 를 넘기면 포기하고 false — 모뎀이 죽어도 부팅은 끝나야 한다
// (콘솔/OBD2/차키는 모뎀과 무관하게 동작해야 하고, 모뎀 회복은 loop 가 계속 시도한다).
static bool modemWaitReady()
{
    LOGI(SerialMon, "[MODEM] AT 응답 대기...\n");
    uint32_t start = millis();
    int retry = 0;
    while (!modem.testAT(1000)) {
        LOGD(SerialMon, ".");
        if (millis() - start > MODEM_AT_READY_TIMEOUT_MS) {
            LOGE(SerialMon, "\n[MODEM] AT 무응답 %lus — 모뎀 없이 부팅 계속(전원/배선 확인)\n",
                 (unsigned long)(MODEM_AT_READY_TIMEOUT_MS / 1000UL));
            return false;
        }
        if (retry++ > 30) {
            digitalWrite(BOARD_PWRKEY_PIN, LOW);  delay(100);
            digitalWrite(BOARD_PWRKEY_PIN, HIGH); delay(MODEM_POWERON_PULSE_WIDTH_MS);
            digitalWrite(BOARD_PWRKEY_PIN, LOW);
            retry = 0;
        }
    }
    LOGI(SerialMon, "\n[MODEM] AT 준비됨\n");
    return true;
}

// 모뎀이 내장 GPS 지원 모델인지 확인. A7670G(GPS 미지원)면 정지.
static String g_modemName = "UNKNOWN";   // 부팅 배너에 실을 모뎀 모델명

#if FEATURE_GPS
// ATI 로 모델명 1회 조회 — 실패(=UNKNOWN)면 재시도 상한까지만.
static String modemQueryName(int tries)
{
    for (int i = 1; i <= tries; ++i) {
        String name = modem.getModemName();
        if (name != "UNKNOWN") return name;
        // TinyGSM 이 실패 시 "MODEM STRING NO FOUND!" 를 남긴다. 왜 실패했는지(무유심 부팅
        // 직후 모뎀 AT 파서가 잠깐 막힘 등)는 그 앞줄의 URC 를 봐야 한다.
        LOGW(SerialMon, "[MODEM] 모델명 조회 실패 (%d/%d)\n", i, tries);
        delay(1000);
    }
    return String("UNKNOWN");
}

// 모델명 확보 + A7670G(내장 GPS 없음) 차단.
// ⚠️ 여기서 무한 재시도하면 안 된다 — 유심 미삽입 첫 부팅에서 모뎀이 ATI 에 계속 무응답인
//    사례가 실측됐고(config.h 모뎀 브링업 주석), 그때 setup 이 끝나지 않아 단말 전체가 멈췄다.
//    상한까지 실패하면 하드 리셋 1회로 회복을 노리고, 그래도 모르면 UNKNOWN 인 채 진행한다.
//    (모델 미상 = "A7670G 아님"으로 취급 — GPS 를 켜보고 안 되면 그때 실패로 드러난다.)
static void modemCheckGpsCapable()
{
    String name = modemQueryName(MODEM_NAME_RETRY);

    if (name == "UNKNOWN") {
        LOGW(SerialMon, "[MODEM] 모델명 미확보 — 하드 리셋 후 재조회\n");
        if (modemHardReset()) name = modemQueryName(MODEM_NAME_RETRY);
    }

    if (name == "UNKNOWN") {
        LOGE(SerialMon, "[MODEM] 모델명 확보 실패 — UNKNOWN 으로 계속 진행\n");
        return;
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
}
#endif

// 콘솔 핸들러는 loop 상태를 읽어야 해서 아래쪽(loop 직전)에 정의된다.
// .ino 자동 프로토타입 생성은 기능 토글 조합에 따라 어긋나므로 직접 선언한다.
static bool appConsole(const String &cmd, const String &arg, Stream &io);
static void appPrintHelp(Stream &io);

// ---------------------------------------------------------------------------
void setup()
{
    // U0 + 보조 UART 동시 open. 이후 모든 출력이 양쪽 포트에 나간다.
    consoleBegin();
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

#if FEATURE_OFFLINE_BUF
    // 오프라인 링버퍼 — 파티션 탐색 + 섹터 헤더 스캔으로 head/tail 복원.
    // 실패해도(파티션 없음) 부팅은 계속한다 — 적재만 비활성되고 나머지는 정상 동작한다.
    Buf::begin(SerialMon);
#endif

#if (FEATURE_LTE && FEATURE_OTA)
    // OTA pending 플래그 로드 — 재부팅-후-성공/실패면 접속 후 loop가 ack를 flush.
    Ota::begin();
#endif

    // 단말 신원 로드(NVS). Debug Console에서 'setid vt-...'로 설정 가능('help' 참고).
    Prov::begin();
    // 콘솔 위임 등록 — info/status는 모뎀/GPS/OBD 상태를 읽으므로 여기서 처리한다.
    Prov::setConsoleHandler(appConsole);
    Prov::setHelpHandler(appPrintHelp);
    Prov::printHelp(SerialMon);
    // 크리덴셜 미확보(device_id/pw 없음) → 미프로비저닝 표시. LTE up 이후 자동 발급(아래).
    if (!Prov::hasCredentials()) Led::set(Led::State::UNPROVISIONED);

    modemPowerOn();
    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
    delay(3000);

    // 모뎀이 안 뜨면 GPS/LTE 는 못 쓰지만, 콘솔·OBD2·차키는 살아 있어야 한다 → 계속 진행.
    const bool modemReady = modemWaitReady();
    (void)modemReady;   // 기능 토글 조합(GPS/LTE 모두 off)에 따라 미사용일 수 있다

#if FEATURE_GPS
    if (modemReady) {
        modemCheckGpsCapable();   // 모델명 확보(배너에 실림) + A7670G(GPS 미지원) 차단
        // GNSS를 먼저 켜서 아래 LTE 등록(최대 수십 초) 대기 동안 위성을 포착하게 한다.
        LOGD(SerialMon, "[GPS] GNSS 활성화...\n");
        bool gpsOn = false;
        for (int i = 1; i <= MODEM_GPS_ENABLE_RETRY && !gpsOn; ++i) {
            gpsOn = Gps::begin(modem);
            if (!gpsOn) {
                LOGW(SerialMon, "[GPS] GNSS 활성화 실패 (%d/%d)\n", i, MODEM_GPS_ENABLE_RETRY);
                delay(1000);
            }
        }
        // 실패해도 멈추지 않는다 — 폴링은 계속 돌고(미측위로 나감), 모뎀이 회복되면
        // GNSS 도 대개 같이 살아난다. 여기서 막히면 나머지 기능까지 못 쓴다.
        if (gpsOn) LOGI(SerialMon, "[GPS] GNSS 활성화됨\n");
        else       LOGE(SerialMon, "[GPS] GNSS 활성화 실패 — GPS 없이 계속 진행\n");
    } else {
        LOGE(SerialMon, "[GPS] 모뎀 무응답 — GNSS 브링업 생략\n");
    }
#endif

    // 하드웨어 신원 + 접속 대상 + 주기 설정 요약. 레벨과 무관하게 항상 출력한다.
    printDeviceInfo(modem, g_modemName.c_str());

#if FEATURE_LTE
    // SIM 상태는 위 배너에 이미 실렸다. SIM PIN이 걸려 있으면 아래 주석 해제:
    // modem.simUnlock("0000");

    // 증분 A: 등록 → PDP → 평문 HTTP GET 으로 LG U+ 유심 데이터패스 검증.
    Led::set(Led::State::PROVISIONING);   // 망 등록/접속 시도 중 — 느린 점멸
    if (modemReady && Lte::begin(modem, SerialMon)) {
        LteStatus st;
        Lte::status(modem, st);
        Lte::printStatus(st, SerialMon);
#if (FEATURE_GPS && GPS_USE_AGPS)
        // PDP 가 올라온 직후 AGPS 보조 데이터를 받는다 — 콜드스타트(수 분)를 크게 줄인다.
        // 실패해도 측위 자체는 진행되므로 경고만 남기고 넘어간다.
        if (Gps::enableAgps(modem)) LOGI(SerialMon, "[GPS] AGPS 보조 데이터 수신됨\n");
        else                        LOGW(SerialMon, "[GPS] AGPS 실패 — 자력 측위로 진행\n");
#endif

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
                // 구독은 여기서 하지 않는다 — status online 발행 ACK가 URC 스트림에 떠 있어
                // SUBACK 파싱이 엉킨다. loop 상승엣지가 CMD_SUB_DELAY_MS 뒤에 처리한다.
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

// ===========================================================================
// 진단 콘솔 명령 — 'info' / 'status [영역]'
//   로그 레벨과 무관하게 항상 출력한다(요청에 대한 응답이지 로그가 아니다).
//   ⚠️ 모뎀에 AT를 보내는 항목이 있으므로 loop 진행이 잠깐 멈춘다. 사람이 친 명령에
//      한해서만 발생하고, 주기 경로는 캐시값만 쓴다.
// ===========================================================================

// 경과시간을 사람이 읽는 형태로. now 기준 "몇 초 전"(0이면 아직 없음).
static void printAgo(Stream &io, const char *label, uint32_t stamp)
{
    if (stamp == 0) { io.printf("  %-14s: (없음)\n", label); return; }
    io.printf("  %-14s: %lus 전\n", label, (unsigned long)((millis() - stamp) / 1000UL));
}

#if FEATURE_GPS
static void statusGps(Stream &io)
{
    io.println("[GPS]");
    // GNSS 전원(AT+CGNSSPWR?) — 모뎀 리셋 뒤 꺼져 있는 상태를 눈으로 확인하는 항목.
    // 사람이 status 를 쳤을 때만 AT 를 쏜다.
    io.printf("  %-14s: %s\n", "GNSS 엔진",
              Gps::isEnabled(modem) ? "on" : "OFF(측위 불가 — 재활성화 대기)");
    io.printf("  %-14s: %d (4=GPS+BDS+GALILEO+SBAS+QZSS, 0=조회실패)\n",
              "위성군", Gps::mode(modem));
    io.printf("  %-14s: %s\n", "원시 CGNSSINFO", Gps::raw(modem).c_str());
    io.printf("  %-14s: %s\n", "측위", g_gpsFixNow ? "fix" : "no fix");
    if (g_lastFix.valid) {
        // g_lastFix는 측위를 놓쳐도 마지막 값을 유지한다 — telemetry가 이걸 계속 싣는다.
        io.printf("  %-14s: %.6f, %.6f\n", "좌표", g_lastFix.lat, g_lastFix.lon);
        io.printf("  %-14s: mode=%d 위성=%d 정확도=%.1f\n", "품질",
                  g_lastFix.fixMode, g_lastFix.vsat, g_lastFix.accuracy);
        io.printf("  %-14s: %04d-%02d-%02d %02d:%02d:%02d\n", "UTC",
                  g_lastFix.year, g_lastFix.month, g_lastFix.day,
                  g_lastFix.hour, g_lastFix.minute, g_lastFix.second);
        if (!g_gpsFixNow) io.println("  ⚠️ 현재 미측위 — 위 좌표는 마지막 유효값(telemetry도 이 값)");
    } else {
        io.println("  ⚠️ 부팅 후 한 번도 측위 못함 — telemetry gps.fix=false");
    }
    printAgo(io, "마지막 폴", g_lastGpsPoll);
    io.printf("  %-14s: %lums (%s)\n", "폴 주기",
              (unsigned long)(vehicleMoving(g_obd) ? GPS_POLL_INTERVAL_MS : GPS_POLL_IDLE_MS),
              vehicleMoving(g_obd) ? "주행" : "정차");
}
#endif

#if FEATURE_CARKEY
static void statusCarkey(Stream &io)
{
    uint32_t left = Carkey::busyRemainMs();
    io.println("[KEY]");
    io.printf("  %-14s: %s\n", "누름 상태",
              left ? "진행 중" : "대기");
    if (left) io.printf("  %-14s: %lums\n", "남은 시간", (unsigned long)left);
    io.printf("  %-14s: %dms (상한 %dms)\n", "기본 유지시간",
              CARKEY_PRESS_MS, CARKEY_PRESS_MAX_MS);
}
#endif

static void statusModem(Stream &io)
{
    io.println("[MODEM]");
    io.printf("  %-14s: %s\n", "모델", g_modemName.c_str());
#if FEATURE_LTE
    // 아래 3개는 실제 AT 왕복이다 — 사람이 status를 쳤을 때만 발생한다.
    io.printf("  %-14s: %d (1=ready 2=locked)\n", "SIM", modem.getSimStatus());
    LteStatus st;
    Lte::status(modem, st);
    io.printf("  %-14s: %s\n", "망 등록", st.registered ? "등록됨" : "미등록");
    io.printf("  %-14s: %s\n", "PDP(데이터)", st.dataUp ? "up" : "down");
    io.printf("  %-14s: %d %s\n", "신호(rssi)", st.rssi,
              st.rssi == 99 ? "(측정불가)" : st.rssi >= 15 ? "(양호)" : "(약함)");
    io.printf("  %-14s: %s\n", "사업자", st.oper.length() ? st.oper.c_str() : "-");
    io.printf("  %-14s: %s\n", "IP", st.ip.length() ? st.ip.c_str() : "-");
    io.printf("  %-14s: %s\n", "APN", LTE_APN);
    io.printf("  %-14s: %s\n", "모뎀 AT응답", Lte::modemAlive(modem) ? "정상" : "무응답(전원 확인)");
    if (!st.registered) {
        io.printf("  %-14s: %d회 (%d회에 모뎀 리셋)\n", "연속 등록실패",
                  g_lteFailStreak, LTE_FAIL_BEFORE_RESET);
        io.printf("  %-14s: %lus\n", "재브링업 간격", (unsigned long)(g_lteRetryDelay / 1000UL));
    }
#else
    io.println("  (FEATURE_LTE=0 — 모뎀 통신 비활성)");
#endif
}

#if FEATURE_LTE
static void statusServer(Stream &io)
{
    bool connected = Mqtt::isConnected(modem);
    io.println("[SERVER]");
    io.printf("  %-14s: %s:%d (%s)\n", "브로커", MQTT_HOST, MQTT_PORT,
              MQTT_USE_TLS ? "TLS" : "평문");
    io.printf("  %-14s: %s\n", "MQTT 세션", connected ? "접속됨" : "끊김");
    io.printf("  %-14s: %s\n", "CMQTT 서비스", Mqtt::serviceStarted() ? "시작됨" : "미시작");
    io.printf("  %-14s: %s\n", "크리덴셜",
              Prov::hasCredentials() ? "확보" : "없음(프로비저닝 필요)");
    if (g_provRejected) io.println("  ⚠️ 프로비저닝 403 래치 — allowlist 확인 후 재부팅해야 해제");
    io.printf("  %-14s: %s\n", "client_id",
              Mqtt::clientId().length() ? Mqtt::clientId().c_str() : "(접속 전)");
    io.printf("  %-14s: %s\n", "telemetry",
              Mqtt::topicTelemetry().length() ? Mqtt::topicTelemetry().c_str() : "(접속 전)");
    io.printf("  %-14s: %s %s\n", "cmd 구독",
              Cmd::topic().length() ? Cmd::topic().c_str() : "(미구독)",
              Cmd::isSubscribed() ? "[ok]" : "[실패/미구독]");
    if (Cmd::hasPendingRx())
        io.printf("  · 미처리 수신 명령 %d건 (큐 %d칸)\n", Cmd::pendingRxCount(), CMD_RX_QUEUE_N);
    // 0 이 아니면 명령이 실제로 유실됐다는 뜻 — 조용히 넘어가면 안 되는 값이다.
    if (Cmd::droppedRxCount())
        io.printf("  ⚠️ 수신 큐 넘침으로 드롭 %lu건\n", (unsigned long)Cmd::droppedRxCount());
    io.printf("  %-14s: %s / %s%s\n", "차량 상태(보고)",
              statusPowerMode(), g_stPrevIgn ? "ignition_on" : "ignition_off",
              g_stPending ? " [재발행 대기]" : "");
    if (connected && !Cmd::isSubscribed()) {
        int32_t left = (int32_t)(g_nextSubAt - millis());
        io.printf("  %-14s: %lds 후 (간격 %lus)\n", "구독 재시도",
                  (long)(left > 0 ? left / 1000 : 0), (unsigned long)(g_subRetryDelay / 1000UL));
    }
    io.printf("  %-14s: %lums (keepalive %ds)\n", "발행 주기",
              (unsigned long)Cfg::telemetryIntervalMs(), Cfg::keepaliveS());
    io.printf("  %-14s: %lu\n", "발행 seq", (unsigned long)g_seq);
    if (connected) {
        int32_t remain = (int32_t)(g_nextPubAt - millis());
        io.printf("  %-14s: %lds 후\n", "다음 발행", (long)(remain > 0 ? remain / 1000 : 0));
    } else {
        io.printf("  %-14s: %lums\n", "재접속 백오프", (unsigned long)g_backoff);
        printAgo(io, "마지막 시도", g_lastConnTry);
    }
    io.printf("  %-14s: rssi=%d reg=%d (마지막 발행 시점)\n", "발행시 망상태",
              Mqtt::lastRssi(), Mqtt::lastReg());
}
#endif

#if FEATURE_OBD2
static void statusObd(Stream &io)
{
    Obd2::LinkInfo li;
    Obd2::linkInfo(li);
    io.println("[OBD2]");
    io.printf("  %-14s: %s\n", "TWAI 드라이버", li.installed ? "설치됨" : "미설치");
    io.printf("  %-14s: %s\n", "링크", g_obd.valid ? "정상(응답 있음)" : "무응답/끊김");
    if (li.bitrate) io.printf("  %-14s: %dkbps\n", "비트레이트", li.bitrate);
    else            io.printf("  %-14s: 미확립 (직전 성공 %dkbps)\n", "비트레이트", li.lastGoodRate);
    io.printf("  %-14s: %03X %s\n", "응답 ECU", (unsigned)li.ecuId,
              li.ecuId == 0      ? "(미확정)"
            : li.ecuId == 0x7E8  ? "(엔진)"
                                 : "(엔진 아님 — 항목 제한 가능)");
    io.printf("  %-14s: 0x%08X\n", "지원PID(01-20)", li.supportedPid);
    io.printf("  %-14s: %s\n", "VIN", li.hasVin ? li.vin : "(미확보)");
    if (!li.hasVin && li.vinTries >= OBD2_VIN_TRY_LIMIT)
        io.println("  · VIN 재시도 상한 도달 — 미지원 차량으로 판단");
    else if (!li.hasVin && li.vinTries)
        io.printf("  · VIN 재시도 %d/%d\n", li.vinTries, OBD2_VIN_TRY_LIMIT);
    io.printf("  %-14s: %d/%d 미지원 확정\n", "확장PID 래치", li.extLatched, li.extTotal);
    printAgo(io, "마지막 폴", g_lastObdPoll);
    io.printf("  %-14s: %lums\n", "폴 주기", (unsigned long)OBD2_POLL_INTERVAL_MS);
    if (!li.installed) {
        io.printf("  %-14s: %lus (고정)\n", "재확립 간격",
                  (unsigned long)(OBD2_LINK_RETRY_MS / 1000));
        printAgo(io, "마지막 재시도", g_lastObdRetry);
    }
    if (g_obd.valid) {
        io.print("  현재값        :");
        if (g_obd.has_rpm)      io.printf(" rpm=%.0f", g_obd.rpm);
        if (g_obd.has_speed)    io.printf(" spd=%d", g_obd.speed);
        if (g_obd.has_coolant)  io.printf(" cool=%dC", g_obd.coolant);
        if (g_obd.has_ctrlv)    io.printf(" batt=%.2fV", g_obd.ctrl_v);
        if (g_obd.has_fuel)     io.printf(" fuel=%.0f%%", g_obd.fuel);
        if (g_obd.has_odometer) io.printf(" odo=%.1fkm", g_obd.odometer);
        io.println();
    }
}
#endif

// 'status [gps|modem|server|obd]' — 인자 없으면 전체.
static void statusAll(Stream &io, const String &what)
{
    bool all = what.isEmpty() || what == "all";
    bool any = all;

#if FEATURE_GPS
    if (all || what == "gps")    { statusGps(io);    any = true; }
#endif
    if (all || what == "modem")  { statusModem(io);  any = true; }
#if FEATURE_LTE
    if (all || what == "server") { statusServer(io); any = true; }
#endif
#if FEATURE_OBD2
    if (all || what == "obd")    { statusObd(io);    any = true; }
#endif
#if FEATURE_CARKEY
    if (all || what == "key")    { statusCarkey(io); any = true; }
#endif

    if (!any) {
        io.printf("[STATUS] 알 수 없는 영역 '%s' — gps|modem|server|obd|key|all\n", what.c_str());
    }
}

static void appPrintHelp(Stream &io)
{
    io.println("[APP]  명령: info                        단말 정보(부팅 배너와 동일)");
    io.println("             status [gps|modem|server|obd|key]  상태 조회(생략 시 전체)");
    io.println("             at <명령>                    모뎀 AT 직접 전송(예: at+cgnssinfo)");
#if FEATURE_OFFLINE_BUF
    io.println("             buf                          오프라인 백로그/링 상태");
    io.println("             buf clear                    ⚠️ 링 전체 삭제(미전송 주행데이터 소실)");
#endif
#if FEATURE_FAST_SAMPLE
    io.println("             fast                         고빈도 창/집계 상태");
#endif
}

// 콘솔 AT 조회 전에 대기 중인 URC 를 먼저 소화시킨다.
// info/status 는 모뎀에 AT 를 직접 보내는데, 발행 ACK(+CMQTTPUB) 같은 URC 가
// 스트림에 남아 있으면 TinyGSM 이 그 줄을 명령 응답으로 읽어버린다
// (실측: imei 자리에 "+CMQTTPUB: 0,0"). Mqtt::handle 로 흘려보내면 cmd 수신 URC 는
// 정상 처리되고 나머지는 소비되므로, 명령을 잃지 않으면서 스트림이 비워진다.
static void drainModemUrc()
{
#if FEATURE_LTE
    uint32_t t0 = millis();
    while (modem.stream.available() && (millis() - t0) < CONSOLE_URC_DRAIN_MS) {
        Mqtt::handle(modem);
    }
#endif
}

static bool appConsole(const String &cmd, const String &arg, Stream &io)
{
    if (cmd == "info") {
        // 펌웨어 버전·빌드시각·리셋 원인(배너) + 단말 정보 표를 함께 낸다.
        // 부팅 로그를 놓쳤거나 한참 뒤에 붙었을 때, 'info' 한 번으로
        // "무슨 펌웨어가 어떤 신원으로 왜 재부팅해서 돌고 있는가"를 다 볼 수 있어야 한다.
        drainModemUrc();
        printBootHeader();
        printDeviceInfo(modem, g_modemName.c_str());
        return true;
    }
    if (cmd == "status" || cmd == "stat") {
        drainModemUrc();
        String w = arg; w.toLowerCase();
        statusAll(io, w);
        return true;
    }
#if FEATURE_OFFLINE_BUF
    // 'buf' / 'buf clear' — 모뎀 AT 를 쓰지 않으므로 URC 배수가 필요 없다.
    if (Buf::tryConsole(cmd, arg, io)) return true;
#endif
#if FEATURE_FAST_SAMPLE
    if (Fast::tryConsole(cmd, arg, io)) return true;
#endif
    // 임의 AT 명령 패스스루 — 현장에서 재플래싱 없이 모뎀에 직접 물어보기 위한 것이다.
    // (측위 진단: at+cgnssinfo / at+cgnsspwr? / at+cgnssmode? / at+cvauxs=1 등)
    // 'at +cgnssinfo'(띄어쓰기)와 'at+cgnssinfo'(붙여쓰기) 둘 다 받는다 — 콘솔은 첫
    // 공백까지를 명령으로 자르므로 후자는 cmd 자체에 AT 명령이 들어온다.
    String lc = cmd; lc.toLowerCase();
    if (lc == "at" || lc.startsWith("at+")) {
        String body = (lc == "at") ? arg
                                   : cmd.substring(2) + (arg.length() ? " " + arg : String());
        body.trim();
        if (!body.length()) {
            io.println("사용법: at <명령>   예) at +cgnssinfo   at+cgnsspwr?");
            return true;
        }
        // ⚠️ 사람이 친 명령에 한해 동작하고 loop 를 잠깐 멈춘다. 응답을 다른 경로가
        //    먹지 않도록 먼저 대기 URC 를 비운다(info/status 와 같은 이유).
        drainModemUrc();
        String res;
        modem.sendAT(body.c_str());
        int r = modem.waitResponse(5000L, res);
        res.trim();
        io.printf("[AT] AT%s → %s\n", body.c_str(),
                  r == 1 ? "OK" : r == 2 ? "ERROR" : r == 0 ? "타임아웃(무응답)" : "CME/CMS ERROR");
        if (res.length()) io.println(res);
        return true;
    }
    return false;
}

#if (FEATURE_OFFLINE_BUF && FEATURE_LTE)
// 백필을 지금 1건 내보내도 되는가 — **실시간 발행이 절대 우선**이라는 규칙의 구현부.
//   ① 접속 직후 BACKFILL_START_DELAY_MS 동안은 금지(구독·OTA ack·첫 telemetry 가 몰린다)
//   ② 틱당 1건, 최소 BACKFILL_MIN_GAP_MS 간격
//   ③ 다음 실시간 발행 BACKFILL_GUARD_MS 이내면 금지
// 모뎀은 publish 를 1건씩만 처리하고 겹치면 세션이 깨진다 — 가드가 이 펌웨어의 생명줄이다.
static bool backfillGateOpen(uint32_t now)
{
    if (g_connectedAt == 0) return false;
    if ((int32_t)(now - g_connectedAt) < (int32_t)BACKFILL_START_DELAY_MS) return false;
    if (g_backfillNextAt != 0 && (int32_t)(now - g_backfillNextAt) < 0)    return false;
    // 다음 실시간 발행 직전 구간은 비워둔다.
    if ((int32_t)(g_nextPubAt - now) < (int32_t)BACKFILL_GUARD_MS)         return false;
    return true;
}
#endif

// ---------------------------------------------------------------------------
void loop()
{
    uint32_t now = millis();   // millis() 오버플로우 안전 비교용

    // 상태표시 LED 패턴 구동(비블로킹, millis 기반). 매 틱 호출.
    Led::update();

#if FEATURE_CARKEY
    // 차키 누름 해제(비블로킹). 이걸 부르지 않으면 라인이 눌린 채로 남는다.
    Carkey::update();
#endif

    // Debug Console 입력 처리 (setid/setpw/showid/clearid/help) — 논블로킹.
    Prov::handleSerial(SerialMon);

#if FEATURE_OBD2
    // OBD2 폴링: 링크 있으면 주기 수집, 없으면 OBD2_LINK_RETRY_MS 고정 간격으로 재확립 시도.
    if (Obd2::isInstalled()) {
        if (now - g_lastObdPoll >= OBD2_POLL_INTERVAL_MS) {
            g_lastObdPoll = now;
            if (Obd2::read(g_obd, SerialMon)) {
                Obd2::print(g_obd, SerialMon);   // 응답받은 전체 항목 + VIN 출력
            } else {
                // 폴 전체 무응답 → 링크 끊김(시동 off 등) → 재확립 경로로.
                LOGI(SerialMon, "[OBD2] 링크 끊김(폴 무응답) — 재확립 대기\n");
                Obd2::end();
                g_obd = Obd2::Data();   // valid=false → telemetry에서 obd 생략
                g_lastObdRetry = now;
            }
        }
    } else if (now - g_lastObdRetry >= OBD2_LINK_RETRY_MS) {
        // 고정 간격 재시도. 시동을 걸면 늦어도 이 간격 안에 링크가 잡힌다.
        g_lastObdRetry = now;
        Obd2::begin(SerialMon);   // 성공 시 다음 틱부터 폴링
    }
#endif

#if (FEATURE_OBD2 && FEATURE_LTE)
    // ── status 전이 감지 (F1) ────────────────────────────────────────────────
    // ⚠️ CAN 링크 유무와 rpm 을 **분리**한다. 키가 ACC/ON 위치면 시동이 꺼져 있어도 CAN 은
    //    살아 있고 그때 rpm 은 0 이다. 링크만으로 시동을 판단하면 이 상태를 시동 켜짐으로
    //    오인한다 → power_mode 는 링크로, ignition_on 은 rpm 으로 각각 정한다.
    // 여기서는 플래그만 세운다. 실제 발행은 아래 발행 체인에서(간격·우선순위 규칙 공유).
    {
        bool link = g_obd.valid;
        bool ign  = g_obd.has_rpm && g_obd.rpm > 0;
        if (!g_stInit || link != g_stPrevLink || ign != g_stPrevIgn) {
            g_stPrevLink = link;
            g_stPrevIgn  = ign;
            g_stInit     = true;
            g_stPending  = true;
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

    // ── 모뎀 생존 프로브 (R1) ───────────────────────────────────────────────
    // 접속 상태는 자체 추적 플래그(Mqtt::isConnected)이고, 그 플래그를 내리는 유일한
    // 경로가 publish 실패였다 — 모뎀이 죽어도 다음 발행 주기(30초) + 래퍼 프롬프트
    // 대기(10초+10초)가 지나야 알았다. 실측 55~95초 동안 LED 는 정상을 표시했다.
    //   → 접속 중이라고 믿는 동안 짧은 AT 프로브로 실제 생존을 확인한다.
    // ⚠️ 조건 세 개는 전부 필수다.
    //    ① 스트림이 비어 있을 때만 — 대기 중인 URC 를 testAT 가 응답으로 먹으면
    //       수신 명령(+CMQTTRX*)이 통째로 사라진다.
    //    ② 직전 발행 후 가드 — 발행 ACK(+CMQTTPUB)가 빠질 시간을 준다.
    //    ③ 접속 중일 때만 — 미접속 구간은 아래 재접속 경로가 이미 프로브한다.
    if (Mqtt::isConnected(modem)
            && (int32_t)(now - g_lastProbeAt) >= (int32_t)MODEM_PROBE_INTERVAL_MS
            && !modem.stream.available()
            && Mqtt::publishGapElapsed(now, MODEM_PROBE_PUB_GUARD_MS)) {
        g_lastProbeAt = now;
        if (modem.testAT(MODEM_PROBE_TIMEOUT_MS)) {
            g_probeFailStreak = 0;
        } else if (++g_probeFailStreak >= MODEM_PROBE_FAILS_TO_DOWN) {
            g_probeFailStreak = 0;
            noteModemRestarted("AT 프로브 무응답");
        }
    }

    bool connected = Mqtt::isConnected(modem);


    // 접속 상승엣지: connectSession의 status 발행 ACK가 빠질 시간을 주려
    // 첫 telemetry를 5초 뒤로 잡는다(발행끼리 충돌 방지 — 모뎀은 publish 1건씩).
    if (connected && !g_wasConnected) {
        g_nextPubAt = now + 5000;
        g_connectedAt = now;   // 백필 시작 지연의 기준 — 접속 직후는 발행이 몰린다
        // 접속 직후는 status online·구독·첫 telemetry 가 몰린다 — 프로브 시계를 여기서
        // 다시 세워 그 구간에 AT 를 끼워넣지 않는다(R1).
        g_lastProbeAt     = now;
        g_probeFailStreak = 0;
        LOGI(SerialMon, "[MQTT] 재접속됨\n");
        Led::set(Led::State::MQTT_OK);          // 재접속 성공 → 정상(heartbeat)
        // clean_session=1 → 재접속마다 재구독. 단 발행 ACK가 빠질 시간을 주고 시도한다.
        // 이전 세션의 구독 플래그가 남아 있으면 재구독을 건너뛰므로 먼저 내린다.
        Cmd::markUnsubscribed();
        g_nextSubAt     = now + CMD_SUB_DELAY_MS;
        g_subRetryDelay = 0;
    }
    if (!connected && g_wasConnected) {
        // 끊김은 상태 전이 — 조용히 백오프에 들어가면 로그만 보고는 멈춘 것과 구분이 안 된다.
        LOGW(SerialMon, "[MQTT] 연결 끊김 — 백오프 재접속 시작\n");
    }
    g_wasConnected = connected;

    // cmd 구독 — 실패하면 백오프로 재시도한다. 접속이 유지되는 동안에는 재접속
    // 상승엣지가 오지 않으므로, 여기서 재시도하지 않으면 다운링크가 영영 죽은 채로 남는다.
    // ⚠️ 반드시 상승엣지 처리 "뒤"에 둔다. 앞에 두면 이전 세션의 낡은 g_nextSubAt(과거 시각)
    //    으로 즉시 한 번 구독하고, 곧이어 엣지가 플래그를 내려 5초 뒤 또 구독한다(중복 발행).
    if (connected && !Cmd::isSubscribed()
            && g_nextSubAt != 0 && (int32_t)(now - g_nextSubAt) >= 0) {
        Cmd::subscribe(modem, SerialMon);
        if (Cmd::isSubscribed()) {
            g_subRetryDelay = 0;
            // F3 — 구독 완료를 status 로 알린다. 서버는 지금 단말이 언제 구독을 끝냈는지
            // 몰라서 status online 을 받고 7초(COMMAND_FLUSH_DELAY_MS)를 추측으로 기다린 뒤
            // pending 을 flush 한다. 구독이 실패해 백오프(15초→30초)를 타면 그 타이머는
            // 헛돌고, 게이트웨이가 재시작하면 인메모리 타이머 자체가 사라진다.
            // sub:true 가 도착하면 서버는 지연 0 으로 즉시 flush 한다.
            // ⚠️ connectSession 의 online 발행은 power_mode 를 싣지 않으므로(모듈이 OBD 를
            //    모른다) 재접속마다 이 발행이 실제 차량 상태 재보고를 겸한다.
            g_stSubNotify = true;
            g_stPending   = true;
        } else {
            g_subRetryDelay = g_subRetryDelay ? g_subRetryDelay * 2 : CMD_SUB_RETRY_BASE_MS;
            if (g_subRetryDelay > CMD_SUB_RETRY_CAP_MS) g_subRetryDelay = CMD_SUB_RETRY_CAP_MS;
            g_nextSubAt = now + g_subRetryDelay;
            LOGW(SerialMon, "[CMD] 구독 재시도 %lus 후\n",
                 (unsigned long)(g_subRetryDelay / 1000UL));
        }
    }

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

#if FEATURE_FAST_SAMPLE
    // ⚠️ 접속 분기 **밖**이다 — 오프라인에서도 창 집계를 만들어야 하고, 접속 상태와 무관하다.
    //    CAN(TWAI)은 모뎀과 별개 버스라 이 폴이 모뎀에 주는 부담은 0 이다.
    //    Cfg::fastMs()==0 이면 즉시 반환한다(원격 비활성화).
    Fast::tick(now, g_obd, SerialMon);
#endif

    if (!connected) {
#if FEATURE_OFFLINE_BUF
        // ── 오프라인 적재 ────────────────────────────────────────────────────
        // 주행 30초 / 정차 120초. 정차 창이 길수록 주차 보관 시간이 늘어난다(3.2일).
        if (Buf::available() && (g_nextLogAt == 0 || (int32_t)(now - g_nextLogAt) >= 0)) {
            const bool moving = vehicleMoving(g_obd);
            g_nextLogAt = now + (moving ? OFFLINE_LOG_MS_DRIVING : OFFLINE_LOG_MS_PARKED);

            // 시각 기준이 아예 없으면(콜드 부팅이 음영에서 시작) 모뎀 RTC 를 한 번 물어본다.
            // 망이 끊겨도 모뎀 RTC 는 계속 도므로 CCLK 가 유효할 수 있다. base 가 이미
            // 있으면 부르지 않는다 — 적재 1건마다 AT 왕복을 늘릴 이유가 없다.
            if (Clk::source() == Clk::SRC_NONE) Mqtt::noteTimeFromModem(modem);

#if FEATURE_GPS
            // 음영을 벗어난 순간의 좌표를 잡기 위해 적재 직전에도 갱신한다(실시간 경로와 동일).
            pollGps(now);
#endif
            Fast::Aggregate agg;
#if FEATURE_FAST_SAMPLE
            // ⚠️ 원본 링이 아니라 **누산기**를 가져온다. 정차 창은 120초라 최대 120샘플로
            //    FAST_WINDOW_MAX(64)를 넘어, 원본 링을 훑어 집계하면 가장 오래된 56샘플이
            //    조용히 빠진다(fast.h 헤더 주석).
            agg = Fast::takeAggregate();
#endif
            Buf::pushSample(g_lastFix, g_gpsFixNow, g_obd, g_seq, agg,
                            /*parkedWindow=*/!moving,
                            Mqtt::lastRssi(), Mqtt::lastReg(), SerialMon);
        }
#endif
        // 미접속: 백오프 간격으로 (재)접속. LTE(PDP)가 죽었으면 먼저 살린다.
        if (now - g_lastConnTry >= g_backoff) {
            g_lastConnTry = now;

            if (g_provRejected) {
                // allowlist 거절 확정 → 재시도 안 함(LED REJECTED 유지). 백오프만 크게.
                g_backoff = MQTT_RECONNECT_CAP_MS;
            } else {
                Led::set(Led::State::PROVISIONING); // 접속/발급 시도 중 → 느린 점멸
                // 링크 판정은 생존 확인부터 한다(R4). Lte::isUp 은 isNetworkConnected +
                // isGprsConnected 두 AT 왕복인데, 죽은 모뎀에서는 둘 다 타임아웃을 다
                // 태우고 나서야 결론이 난다 — 실측 "연결 끊김" 인지에서 "모뎀 무응답"
                // 판정까지 15초. 짧은 프로브로 그 자리에서 갈라 곧장 리셋 경로로 보낸다.
                bool alive    = Lte::modemAlive(modem, MODEM_PROBE_TIMEOUT_MS);
                bool linkDown = !alive || !Lte::isUp(modem);
                // LTE 재브링업은 자체 백오프로 페이싱한다. 등록 시도는 모뎀이 최대 출력으로
                // 송신하는 구간이라, 전원 마진이 부족하면 몰아칠수록 전압이 더 내려앉는다.
                if (linkDown && (g_lastLteTry == 0 || now - g_lastLteTry >= g_lteRetryDelay)) {
                    g_lastLteTry = now;
                    LOGW(SerialMon, "[LTE] 링크 다운 — 재브링업%s\n",
                         alive ? "" : " (모뎀 무응답 — 리셋 경로)");
                    // 무응답이면 Lte::begin 을 부르지 않는다. begin 도 진입에서 같은 확인을
                    // 하고 false 를 내지만, 그 전에 SIM 폴링/APN 왕복이 또 붙는다.
                    if (alive && Lte::begin(modem, SerialMon)) {
                        g_lteFailStreak = 0;
                        g_lteRetryDelay = 0;
                    } else {
                        g_lteFailStreak++;
                        g_lteRetryDelay = g_lteRetryDelay ? g_lteRetryDelay * 2 : LTE_RETRY_BASE_MS;
                        if (g_lteRetryDelay > LTE_RETRY_CAP_MS) g_lteRetryDelay = LTE_RETRY_CAP_MS;

                        // 모뎀이 AT 자체를 못 받는 상태면 기다릴 이유가 없다 — 다음 시도도
                        // 100% 같은 자리에서 실패한다. 백오프는 "등록이 안 붙는" 상황(전원 마진)
                        // 을 위한 것이지, 죽은 모뎀을 살리지는 못한다 → 즉시 리셋으로 승격.
                        // (실측: 무응답 확인 후 30초 백오프 + 소프트 리셋 30초를 통째로 버렸다.)
                        // 위 게이트에서 이미 무응답으로 갈렸으면 다시 묻지 않는다. 살아 있다고
                        // 봤는데 브링업이 실패한 경우만, 리셋 직전에 넉넉한 기본 타임아웃으로
                        // 한 번 더 확인한다(짧은 프로브의 오탐으로 전원 사이클을 돌리지 않게).
                        bool dead = !alive || !Lte::modemAlive(modem);
                        if (dead || g_lteFailStreak >= LTE_FAIL_BEFORE_RESET) {
                            // AT를 못 받는 모뎀에 AT+CFUN=1,1 은 닿지 않는다 → 소프트 리셋을
                            // 건너뛰고 곧장 PWRKEY 전원 사이클.
                            bool revived = dead ? modemHardReset()
                                                : (Lte::softReset(modem, SerialMon) || modemHardReset());
                            g_lteFailStreak = 0;   // 리셋했으니 다시 센다
                            // ⚠️ 모뎀이 리셋되면 CMQTT 서비스와 구독이 모두 사라진다.
                            //    펌웨어 플래그를 같이 내려주지 않으면 begin()이 CMQTTSTART를
                            //    건너뛰어 "접속 실패"만 무한 반복한다(실측 확인된 증상).
                            Mqtt::resetServiceState();
                            Cmd::markUnsubscribed();

#if FEATURE_GPS
                            // 모뎀 리셋은 GNSS 도 끈다(AT+CGNSSPWR=0 상태로 복귀).
                            // 여기서 되살리지 않으면 다음 폴들이 전부 미측위가 되고,
                            // pollGps 의 연속실패 확인이 뒤늦게 고칠 때까지 위치가 빈다.
                            if (revived) {
                                if (Gps::begin(modem)) LOGI(SerialMon, "[GPS] 모뎀 리셋 후 GNSS 재활성화됨\n");
                                else                   LOGW(SerialMon, "[GPS] 모뎀 리셋 후 GNSS 재활성화 실패 — 폴에서 재시도\n");
                            }
#endif
                            if (revived) {
                                // 모뎀이 방금 재부팅돼 깨끗한 상태다 — 실패 이력으로 쌓인
                                // 백오프를 그대로 쓰면(실측 60초) 살아난 모뎀을 놀린다.
                                // 부팅 URC(+CPIN: READY 등)가 정리될 짧은 시간만 두고 바로 붙는다.
                                g_lastLteTry    = millis();   // 리셋 완료 시점 기준으로 다시 센다
                                g_lteRetryDelay = LTE_POST_RESET_DELAY_MS;
                                // 바깥 재접속 게이트도 같은 시점에 열어준다. 안 그러면 LTE는
                                // 5초 뒤 붙을 준비가 됐는데 MQTT 백오프(최대 15초)가 막는다.
                                g_lastConnTry   = g_lastLteTry;
                                g_backoff       = LTE_POST_RESET_DELAY_MS;
                            }
                        }
                        LOGW(SerialMon, "[LTE] 다음 재브링업 %lus 후\n",
                             (unsigned long)(g_lteRetryDelay / 1000UL));
                    }
                }
                // 크리덴셜 없으면 MQTT 전에 먼저 발급받는다(LTE up 상태에서).
                if (Lte::isUp(modem) && !Prov::hasCredentials()) {
                    Prov::ProvResult pr = Prov::provisionOverHttp(modem, SerialMon);
                    if (pr == Prov::ProvResult::REJECTED) {
                        g_provRejected = true;
                        Led::set(Led::State::REJECTED);
                    }
                }
                if (!g_provRejected && Lte::isUp(modem) && Prov::hasCredentials()
                        && Mqtt::begin(modem, SerialMon)) {
                    g_backoff = 0;   // 성공 → 백오프 리셋 (MQTT_OK는 다음 루프 상승엣지에서 표시)
                } else if (!g_provRejected) {
                    g_backoff = g_backoff ? g_backoff * 2 : 1000;
                    if (g_backoff > MQTT_RECONNECT_CAP_MS) g_backoff = MQTT_RECONNECT_CAP_MS;
                    Led::set(Led::State::COMM_ERROR); // 접속/발급 실패 → 통신오류(3회 버스트)
                }
            }
        }
    } else if ((int32_t)(now - g_nextPubAt) >= 0
               && Mqtt::publishGapElapsed(now, PUBLISH_MIN_GAP_MS)) {
        // ⚠️ 발행 간격 가드는 telemetry 에도 건다. cmd ack 는 발행 체인 밖(Cmd::handle)에서
        //    나가므로, 이게 없으면 ack 직후 주기가 돌아온 telemetry 와 겹칠 수 있다.
        //    g_nextPubAt 은 갱신하지 않고 조건만 막으므로 가드가 풀리는 즉시 발행된다.
        // 접속됨: telemetry 주기 발행(오버플로우 안전 비교). 발행 성공 시 다음 주기 예약,
        // 실패 시 isConnected가 false로 바뀌어 다음 루프에서 재접속 경로를 탄다.
        // 주기는 config_update로 런타임 변경 가능(Cfg::telemetryIntervalMs).
        g_nextPubAt = now + Cfg::telemetryIntervalMs();
#if FEATURE_GPS
        // 발행 직전 좌표 갱신 — 주기 폴에만 의존하면 실을 fix가 최대 폴 주기만큼 묵는다
        // (주행 중 10초 = 시속 100km에서 약 280m 오차). 폴 시각도 갱신되므로 중복 폴 없음.
        pollGps(now);
#endif
        bool withMeta = !g_metaSent;   // 최초 1회만 하드웨어 메타 포함
        // g_gpsFixNow(= 방금 폴에서 실제로 측위됐는가)를 함께 넘긴다. g_lastFix 는 미측위
        // 중에도 마지막 값을 유지하는 캐시라, 이걸 안 넘기면 ts 가 얼어붙어 서버에서
        // 그 구간 telemetry 가 PK 중복으로 사라진다(mqtt.cpp publishTelemetry 주석).
        bool pubOk = Mqtt::publishTelemetry(modem, g_lastFix, g_gpsFixNow, g_obd,
                                            g_seq, withMeta, SerialMon);
        if (pubOk) {
            if (withMeta) g_metaSent = true;
            g_seq++;
        }
#if FEATURE_FAST_SAMPLE
        // 고빈도 창은 실시간 발행 직후 1초 뒤에 낸다 — 모뎀은 publish 를 1건씩만 처리하고
        // 겹치면 세션이 깨진다(실측). 발행 성공 여부와 무관하게 예약한다.
        g_nextFastAt = now + FAST_PUBLISH_DELAY_MS;
#endif
        // 주기 상태 한 줄 — 모듈별 폴 덤프를 DEBUG로 내린 대신 여기서 전체 상태를 요약한다.
        printStatusLine(true, g_seq, pubOk ? "pub-ok" : "pub-FAIL");
        g_lastStatAt = now;
    }
    else if (statusGateOpen(now)) {
        // ── status 전이 재발행 (F1) / 구독 완료 통지 (F3) ────────────────────
        // telemetry 다음 우선순위다 — 지연 30초+ 를 5초대로 줄이는 것이 이 발행의 전부라
        // 고빈도 창·백필보다 먼저 나가야 한다.
        bool sub = g_stSubNotify;
        if (Mqtt::publishStatus(modem, statusPowerMode(), g_stPrevIgn, sub, SerialMon)) {
            g_stPending   = false;
            g_stSubNotify = false;
        }
        // 발행 실패는 세션 끊김이다(publishStatus 가 s_connected 를 내린다) — 플래그를
        // 그대로 두면 재접속 후 다시 나간다. 단 sub 는 재구독 시 어차피 다시 세워진다.
    }
#if FEATURE_FAST_SAMPLE
    else if (g_nextFastAt != 0 && (int32_t)(now - g_nextFastAt) >= 0
             && Mqtt::publishGapElapsed(now, PUBLISH_MIN_GAP_MS)) {
        // ── 고빈도 창 발행 ───────────────────────────────────────────────────
        g_nextFastAt = 0;
        int      n  = 0;
        uint32_t t0 = 0;
        const Fast::Sample *win = Fast::window(n, t0);
        if (n >= FAST_PUBLISH_MIN_N) {
            Fast::notePublished(Mqtt::publishFast(modem, win, n, t0, SerialMon));
        } else if (n > 0 || Cfg::fastMs() != 0) {
            Fast::noteSkipped();   // 시동 OFF·링크 없음 → 샘플 부족
        }
        // ⚠️ 발행 실패해도 창을 비운다. 쌓아두면 다음 창과 섞여 o 오프셋이 무의미해지고,
        //    실시간성이 생명인 데이터라 재전송 가치가 낮다. 실패는 [FAST] 카운터로만 남긴다.
        Fast::reset();
        // 누산기도 함께 버린다 — 온라인 구간 이벤트는 서버가 1Hz 원본으로 판정하므로
        // 무손실이다. 버리지 않으면 온라인이 길어질수록 누산기가 자라, 오프라인으로
        // 떨어진 직후 첫 레코드의 agg 가 "직전 창"이 아니라 "온라인 세션 전체"를 요약한다.
        Fast::dropAggregate();
    }
#endif
#if FEATURE_OFFLINE_BUF
    else if (Buf::count() && backfillGateOpen(now)
             && Mqtt::publishGapElapsed(now, PUBLISH_MIN_GAP_MS)) {
        // ── 백필 1건 ─────────────────────────────────────────────────────────
        // else-if 체인이라 실시간 telemetry → status 전이 → 고빈도 창 → 백필 순으로
        // 자동 우선한다. 공통 간격 가드까지 걸어 cmd ack 와도 겹치지 않게 한다.
        g_backfillNextAt = now + BACKFILL_MIN_GAP_MS;
        Buf::Record r;
        if (Buf::peek(r, SerialMon)) {
            switch (Buf::resolveTs(r)) {
            case Buf::TsFix::OK:
                if (Mqtt::publishBackfill(modem, r, SerialMon)) Buf::pop();
                // 발행 실패는 세션 끊김이다 — pop 하지 않고 다음 접속에서 다시 시도한다.
                break;

            case Buf::TsFix::RETRY:
                // 아직 시각을 모른다(GPS fix·모뎀 NITZ 둘 다 없음). 폐기하지 않는다 —
                // 시각이 도착하면 그대로 복원된다. 링은 FIFO 라 이 레코드를 건너뛸 수
                // 없으므로 백필이 잠시 멈추는데, 접속된 상태에서 시각은 곧 온다.
                // 영영 안 오면 링이 가득 차 오래된 것부터 폐기되지만, 그건 시각을
                // 모르는 단말이 애초에 보낼 수 있는 게 없다는 뜻이다.
                if (++g_bfNoClock % 60 == 1) {
                    LOGW(SerialMon, "[BUF] 시각 미확보 — 백필 대기 중(백로그 %lu건)\n",
                         (unsigned long)Buf::count());
                }
                break;

            case Buf::TsFix::DISCARD:
                // 다른 부팅 세션의 ts=0 레코드 → 복원 불가.
                // ts=0 으로 보내면 서버 PK (device_id,0) 에 여러 건이 한 행으로 붕괴한다.
                Buf::pop();
                if (++g_bfDiscarded % 10 == 1) {
                    LOGW(SerialMon, "[BUF] 시각 복원 불가 — 누적 %lu건 폐기\n",
                         (unsigned long)g_bfDiscarded);
                }
                break;
            }
        }
    }
#endif

    // 미접속이 길어지면 [STAT]가 아예 안 나와 멈춘 것처럼 보인다 → 유휴에도 주기적으로 남긴다.
    if (!connected && now - g_lastStatAt >= STATUS_LINE_IDLE_MS) {
        g_lastStatAt = now;
        printStatusLine(false, g_seq, "-");
    }
#endif

    // TODO: OBD2 / 차키 / BLE 태스크는 각 단계에서 여기에 추가
    delay(50);
}

#ifndef TINY_GSM_FORK_LIBRARY
#error "TinyGSM fork가 필요합니다. lib/ 디렉토리를 arduino libraries에 복사하세요. README 참고"
#endif
