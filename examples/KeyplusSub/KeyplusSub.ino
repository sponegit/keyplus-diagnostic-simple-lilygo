// ===========================================================================
// Keyplus v2 서브보드 펌웨어 (LilyGO T7-S3)
//   센서 수집 · 온도 가드 · 전원 레일 게이팅 · 저전압 판정 · 차키 시퀀서
//   메인(T-A7670E)과는 UART0(43/44) 한 가닥으로 KpLink 프레임을 주고받는다.
//
// 현재(작업서 S5): 부팅 배너, 레일 OFF 고정, 메인 링크(연결 확인·생존 신호·RESET),
//                  메인 로그 중계([M] 접두) + 'm <명령>' 으로 메인 콘솔 사용,
//                  차키 6채널 시퀀스 실행(KEY_CMD), 메인 중계 OTA 수신(롤백 포함),
//                  USB-CDC 콘솔, loop 워치독.
//                  센서 벤치 1~5단계: SHT41 온·습도, BMP280 기압, SCD41 CO2, SGP41 VOC·NOx(I2C),
//                  PMS5003 미세먼지(UART2).
//                  S6 전원 감시(INA228·저전압 판정·POWER), S7 1단계 GNSS 수신(UART1, 콘솔만).
// ===========================================================================
#include <Arduino.h>
#include <KpLink.h>
#include "config.h"
#include "link.h"
#include "carkey.h"
#include "ota.h"
#include "air.h"
#include "pms.h"
#include "co.h"
#include "guard.h"
#include "pwr.h"
#include "gnss.h"

// arduino-esp32 는 부팅 시 새 OTA 이미지를 자동 확정한다 — 끄고 메인 링크 업 때 직접 확정한다(ota.h).
extern "C" bool verifyRollbackLater() { return true; }

// --- USB-CDC 콘솔 ------------------------------------------------------------
// 한 줄 단위 명령. 'm <명령>' 은 메인 콘솔로 보내고(응답은 [M] 줄로 돌아온다), 나머지는 서브 명령.
static char   s_line[96];
static size_t s_lineLen = 0;

static void printHelp()
{
    Serial.println("[SUB] 명령: m <명령>      메인 콘솔로 전달 (예: m status, m help)");
    Serial.println("            status        메인 링크·차키 상태");
    Serial.println("            key <io> [ms] 차키 채널 io 1~6 누름 (GPIO 4~9, 기본 500ms)");
    Serial.println("            key abort     진행 중 시퀀스 중단(모두 뗌)");
    Serial.println("            i2c           I2C 버스 스캔(센서 주소 확인)");
    Serial.println("            air           센서 현재 값 · air watch 새 값마다 출력(토글)");
    Serial.println("            pms           미세먼지 값·통계 · pms sleep / pms wake");
    Serial.println("            co            일산화탄소 값·통계");
    Serial.println("            pwr           전압·전류(INA228) · pwr reset 최소/최대 초기화");
    Serial.println("            pwr sim <V>   가짜 전압으로 저전압 판정 · pwr sim off 해제");
    Serial.println("            gnss          GNSS fix·위성·통계 · gnss raw NMEA 원문(토글)");
    Serial.println("            guard         온도 가드 상태 · guard sim <°C> 가짜 온도 · guard sim off");
    Serial.println("            reset         서브 재부팅");
    Serial.println("            hang          ⚠️ 시험용 — loop 를 멈춰 워치독 재부팅(5초) 확인");
    Serial.println("            help          이 도움말");
}

static void runCommand(const char *cmd)
{
    if (cmd[0] == 'm' && cmd[1] == ' ') {
        const char *rest = cmd + 2;
        while (*rest == ' ') rest++;
        if (!*rest) return;
        if (!Link::sendConsoleLine(rest)) Serial.println("[SUB] 메인 링크 다운 — 전달 못 함");
        return;
    }
    if (Carkey::tryConsole(cmd)) return;
    if (Air::tryConsole(cmd)) return;
    if (Pms::tryConsole(cmd)) return;
    if (Co::tryConsole(cmd)) return;
    if (Guard::tryConsole(cmd)) return;
    if (Pwr::tryConsole(cmd)) return;
    if (Gnss::tryConsole(cmd)) return;
    if (!strcmp(cmd, "status") || !strcmp(cmd, "status link")) {
        Link::printStatus(Serial);
        Carkey::printStatus(Serial);
        Ota::printStatus(Serial);
        Air::printStatus(Serial);
        Pms::printStatus(Serial);
        Co::printStatus(Serial);
        Guard::printStatus(Serial);
        Pwr::printStatus(Serial);
        Gnss::printStatus(Serial);
    } else if (!strcmp(cmd, "reset")) {
        Serial.println("[SUB] 재부팅");
        Serial.flush();
        delay(50);
        ESP.restart();
    } else if (!strcmp(cmd, "hang")) {
        // 리셋 전용선이 없으므로 워치독이 유일한 행 복구 수단이다 — 실제로 도는지 확인한다.
        Serial.println("[SUB] loop 정지 — 5초 안에 워치독이 재부팅해야 한다");
        Serial.flush();
        for (;;) { }
    } else if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
        printHelp();
    } else if (cmd[0]) {
        Serial.printf("[SUB] 알 수 없는 명령 '%s' — help\n", cmd);
    }
}

static void handleConsole()
{
    while (Serial.available() > 0) {
        int c = Serial.read();
        if (c == '\r' || c == '\n') {
            s_line[s_lineLen] = '\0';
            runCommand(s_line);
            s_lineLen = 0;
        } else if (c >= 0x20 && c < 0x7F && s_lineLen + 1 < sizeof(s_line)) {
            s_line[s_lineLen++] = (char)c;   // 비인쇄 문자는 버린다(잡음 방어)
        }
    }
}

void setup() {
    // 레일 게이트는 온도 가드가 판정하기 전까지 OFF 가 안전한 기본값이다.
    const uint8_t rails[] = {PIN_RAIL_GNSS, PIN_RAIL_PMS_5V, PIN_RAIL_SENSOR_3V3, PIN_RAIL_CO_5V};
    for (uint8_t pin : rails) {
        digitalWrite(pin, LOW);
        pinMode(pin, OUTPUT);
    }
    // 차키 핀(4~9)을 "뗌"으로 확정 — DIRECT 배선이면 Hi-Z(fob 풀업 유지). 레벨을 먼저 쓴다.
    Carkey::begin();
    Ota::begin();
    Air::begin();
    Pms::begin();
    Co::begin();
    Pwr::begin();
    Gnss::begin();    // GPIO38 레일 ON — 온도 가드 대상 아님
    Guard::begin();   // 레일은 위에서 LOW — SHT41 첫 값을 보고 켠다

    Serial.begin(115200);
    // USB 를 안 꽂은 채(차량 장착) 돌 때 콘솔 출력이 loop 를 막지 않게 한다 —
    // 호스트가 없으면 버퍼가 차는 즉시 버린다.
    Serial.setTxTimeoutMs(0);

    Link::begin();

    Serial.printf("\n[BOOT] KeyplusSub fw=%s hw=%s kplink=v%d\n",
                  FW_VERSION, HW_MODEL, KPLINK_PROTO_VERSION);

    // loop 가 멈추면(CONFIG_ESP_TASK_WDT_TIMEOUT_S = 5초) 패닉 → 재부팅.
    // 리셋 전용선이 없으므로(작업서 D7) 서브 행의 1차 복구 수단이다.
    enableLoopWDT();
}

void loop() {
    Link::poll();
    Carkey::update();
    Ota::tick();
    Air::update();
    Pms::update();
    Co::update();
    Pwr::update();
    Gnss::update();
    handleConsole();
    delay(1);
}
