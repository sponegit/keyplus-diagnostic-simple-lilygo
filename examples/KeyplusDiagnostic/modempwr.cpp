/**
 * @file      modempwr.cpp
 * @brief     모뎀 유휴 슬립 구현 — 설계 근거는 modempwr.h 참고.
 */
#include "modempwr.h"

#if FEATURE_MODEM_SLEEP

#include "log.h"

namespace {

bool     g_applied   = false;   // AT+CSCLK=1 이 실제로 걸렸는가
bool     g_asleep    = false;   // DTR HIGH 상태인가
uint32_t g_lastAtMs  = 0;       // 마지막 sendAT 시각 — 재울 시점 판단의 유일한 근거
uint32_t g_sleptMs   = 0;       // 누적 수면시간
uint32_t g_sleepAt   = 0;       // 이번 수면 시작 시각
uint32_t g_wakes     = 0;       // 깨운 횟수
uint32_t g_beganAt   = 0;       // 통계 기준 시각
bool     g_noWake    = false;   // 'msleep probe' 전용 — 훅의 자동 기상을 잠시 끈다
uint8_t  g_mode      = MODEM_SLEEP_MODE;   // 1=DTR 슬립, 2=RX 슬립 (런타임 변경 가능)

TinyGsm *g_modem     = nullptr; // RX 기상 시 스트림에 직접 써야 해서 잡아둔다

// DTR 을 직접 민다. 이 파일 밖에서는 DTR 을 건드리지 않는다 —
// modemPowerOn() 이 부팅 때 LOW 로 초기화하는 것이 유일한 예외다.
inline void dtr(bool high)
{
    digitalWrite(MODEM_DTR_PIN, high ? HIGH : LOW);
}

// RX 슬립(CSCLK=2) 기상 — "시리얼로 아무 데이터나 보내면 깨어난다"(AT 매뉴얼 10.2.7).
//
// 깨우는 바이트 자체는 모듈이 기상하는 데 쓰이고 대개 삼켜지지만, 깨어 있는 상태에서
// 불리면 "OK" 가 돌아온다. 그 "OK" 를 안 치우면 **바로 뒤에 나갈 진짜 명령의 응답으로
// 오인**돼 스트림이 어긋난다(TinyGSM waitResponse 가 첫 OK 에 성공 처리한다).
// → 기상 후 남은 바이트를 비운다. 단 **'+' 로 시작하는 줄은 건드리지 않는다** —
//   이 모뎀의 URC 는 전부 '+' 로 시작하므로(+CMQTTRX*, +CMQTTPUB, +CGNSS...),
//   peek 으로 확인해 URC 앞에서 멈추면 차키 명령을 삼키지 않는다.
// ⚠️ 완벽하지는 않다. 비우는 도중에 URC 가 도착하면 앞부분을 흘릴 수 있다.
//    DTR 배선이 확인되면 모드 1 이 이 위험 자체가 없어 더 낫다.
void wakeByRx()
{
    if (!g_modem) return;
    g_modem->stream.write("AT\r\n");
    g_modem->stream.flush();
    delay(MODEM_WAKE_SETTLE_MS);

    uint32_t t0 = millis();
    while ((millis() - t0) < 200) {
        if (!g_modem->stream.available()) { delay(2); continue; }
        if (g_modem->stream.peek() == '+') break;   // URC 시작 — 다음 handle 에 넘긴다
        g_modem->stream.read();
    }
}

}  // namespace

// TinyGSM sendAT 훅(TinyGsmModem.tpp 의 약한 심볼). **모든 AT 가 여기를 지난다.**
// 자고 있으면 깨우고, 깨어 있으면 시각만 갱신한다.
extern "C" void tinyGsmModemWakeHook(void)
{
    g_lastAtMs = millis();
    if (g_noWake) return;          // 'msleep probe' 가 일부러 자는 모뎀에 쏘는 중이다
    if (g_asleep) ModemPwr::wake();
}

namespace ModemPwr {

bool begin(TinyGsm &modem)
{
    g_modem = &modem;
    pinMode(MODEM_DTR_PIN, OUTPUT);
    // 설정 자체가 AT 라 반드시 깨어 있는 상태에서 보낸다.
    // ⚠️ 자던 중에 불릴 수 있다(모뎀 리셋 후 재설정 경로). 진행 중이던 수면시간을
    //    먼저 정산하지 않으면 누적 통계가 그만큼 새고, 수면비율이 실제보다 낮게 보인다.
    if (g_sleepAt) {
        g_sleptMs += (millis() - g_sleepAt);
        g_sleepAt  = 0;
    }
    bool was = g_asleep;
    g_asleep = false;              // 아래 기상/AT 가 훅을 통해 재진입하지 않도록 먼저 내린다
    dtr(false);                    // 모드 2 에서도 무해하다(CSCLK=2 는 DTR 을 안 본다)
    if (was) {
        if (g_mode == 2) wakeByRx();
        else             delay(MODEM_WAKE_SETTLE_MS);
    }

    // AT+CSCLK=<mode>. TinyGSM 의 sleepEnable() 은 0/1 만 보내므로 직접 쏜다.
    // 모뎀이 거부하면(구형 펌웨어, 미지원 모델) 그냥 안 자는 것뿐이라 부팅을 막지 않는다.
    modem.sendAT(GF("+CSCLK="), (int)g_mode);
    g_applied = (modem.waitResponse() == 1);
    if (g_applied) {
        if (g_beganAt == 0) g_beganAt = millis();
        g_lastAtMs = millis();
    }
    return g_applied;
}

bool    enabled() { return g_applied; }
uint8_t mode()    { return g_mode; }

void setEnabled(TinyGsm &modem, bool on)
{
    if (on) {
        begin(modem);
        return;
    }
    // 끌 때는 먼저 깨워야 AT+CSCLK=0 이 모뎀에 닿는다.
    wake();
    modem.sleepEnable(false);      // = AT+CSCLK=0
    g_applied = false;
    dtr(false);            // DTR LOW 고정 = 슬립 이전의 원래 동작
    g_asleep = false;
}

void wake()
{
    if (!g_asleep) return;
    // ⚠️ 기상 경로가 다시 AT 를 쏘므로(모드 2) 훅 재진입을 막으려면 **먼저** 내린다.
    g_asleep = false;

    if (g_mode == 2) {
        wakeByRx();
    } else {
        dtr(false);
        // A76xx 하드웨어 매뉴얼 3.3.2: 자는 모듈은 DTR 을 LOW 로 내려 깨운다.
        // ⚠️ 여기서 블로킹하는 게 맞다. 안 기다리고 AT 를 쏘면 그 명령이 통째로 사라져
        //    "모뎀 무응답" 오판 → 리셋으로 이어진다. 리셋 비용이 이 지연보다 훨씬 크다.
        delay(MODEM_WAKE_SETTLE_MS);
    }

    g_wakes++;
    uint32_t now = millis();
    if (g_sleepAt) g_sleptMs += (now - g_sleepAt);
    g_sleepAt  = 0;
    g_lastAtMs = now;
}

void allowSleepIfIdle(uint32_t now)
{
    if (!g_applied || g_asleep) return;
    if ((int32_t)(now - g_lastAtMs) < (int32_t)MODEM_SLEEP_IDLE_MS) return;
    // 모드 2 는 모듈이 UART 유휴를 스스로 보고 잔다 — 우리가 할 일은 "이제부터 자는
    // 것으로 친다"고 표시해 두는 것뿐이다(기상 시 바이트를 쏴야 하므로 상태는 필요하다).
    if (g_mode == 1) dtr(true);
    g_asleep  = true;
    g_sleepAt = now;
}

bool     asleep()    { return g_asleep; }
uint32_t sleptMs()   { return g_sleptMs + (g_sleepAt ? (millis() - g_sleepAt) : 0); }
uint32_t wakeCount() { return g_wakes; }

uint8_t sleepRatioPct(uint32_t now)
{
    if (g_beganAt == 0) return 0;
    uint32_t span = now - g_beganAt;
    if (span == 0) return 0;
    uint32_t slept = sleptMs();
    if (slept > span) slept = span;
    return (uint8_t)((slept * 100UL) / span);
}

// 'msleep probe' — "슬립이 진짜 걸렸는가"를 소비전류가 아니라 **모뎀 반응**으로 판별한다.
//
// DTR 을 올려 재운 뒤, 훅의 자동 기상을 끈 채 AT 를 한 번 쏜다.
//   무응답 → DTR 배선·CSCLK 둘 다 정상. 모뎀은 실제로 자고 있다.
//             (그런데도 전류가 그대로면 원인은 다른 데 있다 — RF 가 안 쉬는 것이다)
//   응답   → DTR 이 모뎀에 안 닿았거나 CSCLK 가 안 먹었다. 슬립 자체가 성립 안 한 것.
// 이 한 줄로 "빌드/코드 문제"와 "하드웨어·모뎀 동작 문제"가 갈린다.
static void probe(TinyGsm &modem, Stream &io)
{
    if (!g_applied) {
        io.println("[MSLEEP] CSCLK=1 이 안 걸려 있다 — 'msleep on' 먼저 (실패하면 모뎀이 거부한 것)");
        return;
    }

    // ① 깨어 있는 상태의 **응답 지연**을 잰다. 이게 기준점이다.
    //    ⚠️ mode 2 에서는 "응답했다/안 했다"로는 아무것도 판정할 수 없다 —
    //       RX 슬립은 들어오는 데이터 자체가 기상 신호라 자고 있어도 깨어나서 답한다.
    //       구분되는 건 **얼마나 늦게 답하느냐** 뿐이다.
    wake();
    modem.testAT(1000);                       // 첫 발은 버린다(직전 상태의 잔향 제거)
    uint32_t t0 = millis();
    bool awakeOk = modem.testAT(1000);
    uint32_t awakeMs = millis() - t0;

    // ② 강제로 재운다. 훅이 g_lastAtMs 를 방금 갱신했으므로 유휴 조건을 우회해 직접 민다.
    //    모드 2 는 모듈이 UART 유휴를 스스로 재므로 우리가 할 건 '기다리는 것'뿐이다.
    if (g_mode == 1) dtr(true);
    g_asleep  = true;
    g_sleepAt = millis();
    // ⚠️ 진입 여유를 넉넉히 준다. 모드 2 는 모듈이 UART 유휴를 관측해야 하므로
    //    DTR 방식보다 오래 걸린다 — 짧게 주면 "안 잤다"는 잘못된 결론이 나온다.
    delay(g_mode == 2 ? 3000 : 500);

    // ③ 자동 기상을 끄고 쏜다. 지연을 같이 잰다.
    //    mode 1: 자면 UART 가 죽으므로 **무응답**이어야 한다(HW 매뉴얼 3.3.2).
    //    mode 2: 응답은 오되 기상 시간만큼 **늦어야** 한다. 깨어있을 때와 같은 속도로
    //            답하면 안 잔 것이다.
    g_noWake = true;
    uint32_t t1 = millis();
    bool sleepOk = modem.testAT(1000);
    uint32_t sleepMs = millis() - t1;
    g_noWake = false;

    // ④ 복구 — 반드시 깨워 놓고 끝낸다.
    wake();
    bool recovered = modem.testAT(1000);

    io.printf("[MSLEEP] probe(mode=%u): 깨어있을때 AT=%s(%lums) / 재운뒤 AT=%s(%lums) / 복구후 AT=%s\n",
              (unsigned)g_mode,
              awakeOk   ? "응답" : "무응답", (unsigned long)awakeMs,
              sleepOk   ? "응답" : "무응답", (unsigned long)sleepMs,
              recovered ? "응답" : "무응답");

    if (!awakeOk) {
        io.println("         → 깨어있을 때조차 무응답이다. 슬립 문제가 아니라 모뎀/UART 문제다.");
        return;
    }
    if (!recovered) {
        io.println("         → ⚠️ 복구 실패. 모뎀이 깨어나지 못했다 — MODEM_WAKE_SETTLE_MS 를 늘려볼 것.");
        return;
    }

    if (g_mode == 1) {
        // DTR 슬립: 자면 UART 가 통째로 죽는다 — 응답 유무만으로 깔끔하게 갈린다.
        if (!sleepOk) {
            io.println("         → ✅ DTR 슬립 걸림(자는 동안 UART 사망 = 매뉴얼 3.3.2 정상 동작). 이제 전류를 본다.");
        } else {
            io.println("         → ❌ 재웠는데 AT 가 먹혔다. DTR 이 모뎀에 안 닿았거나 CSCLK 무시다.");
            io.printf ("            DTR=GPIO%d 배선 확인. 배선이 없으면 'msleep mode 2'(RX 슬립)로 우회할 것.\n",
                       MODEM_DTR_PIN);
        }
        return;
    }

    // RX 슬립: 들어오는 데이터가 곧 기상 신호라 **자고 있어도 응답한다.**
    // 판정은 오직 지연 차이로 한다 — 기상에 드는 시간이 그대로 응답 지연이 된다.
    if (!sleepOk) {
        io.println("         → ✅ RX 슬립 걸림(기상 시간이 1초를 넘었다). 이제 전류를 본다.");
    } else if (sleepMs >= awakeMs + 30) {
        io.printf ("         → ✅ RX 슬립 걸린 것으로 보인다 — 첫 응답이 %lums 느리다(기상 시간).\n",
                   (unsigned long)(sleepMs - awakeMs));
        io.println("            확정은 PPK2 전류로 한다. 이 지연은 '잤다'의 방증일 뿐이다.");
    } else {
        io.println("         → ❌ 지연 차이가 없다 = 안 잔 것으로 보인다.");
        io.println("            'at+csclk?' 로 값이 2 인지 확인. 2 인데도 이러면 이 모듈이 RX 슬립을 무시한다.");
    }
}

bool tryConsole(const String &cmd, const String &arg, TinyGsm &modem, Stream &io)
{
    if (cmd != "msleep") return false;

    String a = arg; a.trim(); a.toLowerCase();
    if (a == "probe") {
        probe(modem, io);
        return true;
    }
    if (a == "on" || a == "off") {
        setEnabled(modem, a == "on");
        io.printf("[MSLEEP] %s — CSCLK=%u %s\n", a.c_str(),
                  g_applied ? (unsigned)g_mode : 0u,
                  g_applied ? (g_mode == 1 ? "(DTR 슬립)" : "(RX 슬립)") : "(슬립 없음)");
        return true;
    }
    // 'msleep mode 1|2' — DTR 슬립 ↔ RX 슬립 즉시 전환(재플래싱 없이 비교하려고 둔다).
    if (a.startsWith("mode")) {
        String v = a.substring(4); v.trim();
        if (v != "1" && v != "2") {
            io.println("사용법: msleep mode 1|2   (1=DTR 슬립, 2=RX 슬립)");
            return true;
        }
        wake();                       // 방식을 바꾸기 전에 반드시 현재 방식으로 깨워 놓는다
        g_mode = (uint8_t)v.toInt();
        bool ok = begin(modem);
        io.printf("[MSLEEP] mode=%s (%s) — CSCLK 적용 %s\n", v.c_str(),
                  g_mode == 1 ? "DTR 슬립" : "RX 슬립", ok ? "성공" : "실패");
        return true;
    }
    if (a.length()) {
        io.println("사용법: msleep [probe|on|off|mode 1|2]   (인자 없으면 상태 조회)");
        return true;
    }

    uint32_t now = millis();
    io.printf("[MSLEEP] 설정=%s(mode=%u) 현재=%s 수면비율=%u%% 누적수면=%lus 깨움=%lu회\n",
              g_applied ? "on" : "off", (unsigned)g_mode,
              g_asleep  ? "자는중"      : "깨어있음",
              (unsigned)sleepRatioPct(now),
              (unsigned long)(sleptMs() / 1000UL),
              (unsigned long)g_wakes);
    io.printf("         DTR=GPIO%d  깨움대기=%ums  재우기유휴=%ums\n",
              MODEM_DTR_PIN, (unsigned)MODEM_WAKE_SETTLE_MS, (unsigned)MODEM_SLEEP_IDLE_MS);
    return true;
}

}  // namespace ModemPwr

#endif  // FEATURE_MODEM_SLEEP
