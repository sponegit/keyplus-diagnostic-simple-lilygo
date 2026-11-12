/**
 * @file      co.cpp
 * @brief     ZE15-CO 구현. 설계는 co.h.
 */
#include "co.h"
#include "config.h"

#include <SoftwareSerial.h>

namespace Co {

static const uint8_t FRAME_LEN = 9;
static const uint8_t GAS_CO    = 0x04;
static const uint8_t UNIT_PPM  = 0x03;

static EspSoftwareSerial::UART s_uart;

static uint8_t  s_buf[FRAME_LEN];
static uint8_t  s_pos      = 0;
static bool     s_off      = true;
static bool     s_ok       = false;
static bool     s_fault    = false;   // 마지막 프레임의 고장 비트
static uint16_t s_co10     = 0;       // 0.1 ppm
static uint16_t s_range10  = 0;       // 측정 범위(0.1 ppm) — 프레임에 실려 온다
static uint32_t s_at       = 0;
static uint32_t s_onAt     = 0;
static uint32_t s_frames = 0, s_badSum = 0, s_badType = 0, s_faults = 0;
static uint32_t s_bytes    = 0;       // 받은 바이트 전부(프레임 이전) — 배선 진단용
static bool     s_raw      = false;

static uint8_t checksum(const uint8_t *f)
{
    uint8_t s = 0;
    for (uint8_t i = 1; i < FRAME_LEN - 1; i++) s += f[i];
    return (uint8_t)(~s + 1);
}

// 소수 자릿수(dec)를 0.1 단위로 맞춘다. 매뉴얼 예시는 dec=1(값 × 0.1)이다.
static uint16_t to10(uint16_t raw, uint8_t dec)
{
    uint32_t v = raw;
    if (dec == 0)      v *= 10;
    else while (dec-- > 1) v /= 10;
    return v > 0xFFFF ? 0xFFFF : (uint16_t)v;
}

// 버퍼가 다 찼는데 프레임이 아니면 다음 FF 부터 다시 맞춘다 — 중간부터 받아도 다음 프레임에서 맞는다.
static void resync()
{
    uint8_t i = 1;
    while (i < FRAME_LEN && s_buf[i] != 0xFF) i++;
    s_pos = FRAME_LEN - i;
    memmove(s_buf, s_buf + i, s_pos);
}

static void onFrame()
{
    if (checksum(s_buf) != s_buf[8]) { s_badSum++; resync(); return; }
    if (s_buf[1] != GAS_CO || s_buf[2] != UNIT_PPM) { s_badType++; resync(); return; }
    s_pos = 0;
    s_frames++;
    s_fault = (s_buf[4] & 0x80) != 0;
    if (s_fault) {
        s_faults++;
        s_ok = false;
        return;
    }
    const uint8_t dec = s_buf[3];
    s_co10    = to10((uint16_t)((s_buf[4] & 0x1F) << 8 | s_buf[5]), dec);
    s_range10 = to10((uint16_t)(s_buf[6] << 8 | s_buf[7]), dec);
    s_ok      = true;
    s_at      = millis();
}

void begin()
{
    pinMode(PIN_CO_TX, INPUT);   // 레일이 켜질 때까지 신호선을 놓아 둔다
    s_off = true;
}

void setPower(bool on)
{
    if (on == !s_off) return;
    if (on) {
        // 평소에는 보내지 않는다(자동 송신 모드). TX 는 쉬는 레벨 HIGH — 진단 명령 co query 때만 쓴다.
        // RX 버퍼 64B · 엣지 버퍼 640 — loop 가 SCD41 탐색(500ms)으로 막혀도 9바이트/초는 넉넉히 버틴다.
        s_uart.begin(CO_BAUD, EspSoftwareSerial::SWSERIAL_8N1, PIN_CO_RX, PIN_CO_TX, false, 64, 640);
        s_off  = false;
        s_onAt = millis();
        s_pos  = 0;
        s_ok   = false;
        return;
    }
    s_uart.end();
    pinMode(PIN_CO_TX, INPUT);
    s_off = true;
    s_ok  = false;
    s_pos = 0;
}

void update()
{
    if (s_off) return;
    while (s_uart.available() > 0) {
        const uint8_t c = (uint8_t)s_uart.read();
        s_bytes++;
        if (s_raw) Serial.printf("%02X%s", c, c == 0xFF ? "\n[CO] " : " ");
        if (s_pos == 0 && c != 0xFF) continue;
        s_buf[s_pos++] = c;
        if (s_pos == FRAME_LEN) onFrame();
    }
    if (s_ok && (uint32_t)(millis() - s_at) > CO_STALE_MS) s_ok = false;
}

static bool warming() { return !s_off && (uint32_t)(millis() - s_onAt) < CO_WARMUP_MS; }

bool get(uint16_t *co10)
{
    if (s_off || !s_ok || warming()) return false;
    *co10 = s_co10;
    return true;
}

bool fault() { return !s_off && s_fault; }

void printValues(Stream &io)
{
    if (s_off)
        io.println("[AIR] ZE15-CO 꺼짐 (온도 가드·주차 듀티 — guard)");
    else if (s_ok)
        io.printf("[AIR] ZE15-CO %u.%u ppm%s\n", (unsigned)(s_co10 / 10), (unsigned)(s_co10 % 10),
                  warming() ? "  (예열 중)" : "");
    else if (s_fault)
        io.println("[AIR] ZE15-CO ⚠️ 센서 고장 비트 — 값 버림");
    else
        io.printf("[AIR] ZE15-CO 없음 (프레임 수신 없음%s)\n", warming() ? " — 켠 직후" : " — 5V·TXD→GPIO11 확인");
}

// GPIO11 을 1초씩 두 번 훑는다 — 내부 풀업으로 한 번, 풀다운으로 한 번.
// UART 는 쉴 때 HIGH 다. 풀업만 보면 선이 끊겨도 HIGH 라 구분이 안 되므로 풀다운에서도 HIGH 인지 본다.
static void sample(uint32_t *hiPct, uint32_t *edges, uint8_t pin = PIN_CO_RX)
{
    uint32_t hi = 0, n = 0, e = 0;
    int last = digitalRead(pin);
    const uint32_t t0 = millis();
    while ((uint32_t)(millis() - t0) < 1000) {
        const int v = digitalRead(pin);
        hi += v;
        n++;
        if (v != last) e++;
        last = v;
        delayMicroseconds(20);
    }
    *hiPct = hi * 100 / n;
    *edges = e;
}

static void probePin()
{
    const bool wasOn = !s_off;
    if (wasOn) s_uart.end();
    uint32_t upHi, upE, dnHi, dnE;
    pinMode(PIN_CO_RX, INPUT_PULLUP);
    delay(5);
    sample(&upHi, &upE);
    pinMode(PIN_CO_RX, INPUT_PULLDOWN);
    delay(5);
    sample(&dnHi, &dnE);
    pinMode(PIN_CO_RX, INPUT);
    // TX/RX 가 바뀌었으면 센서 TXD 가 GPIO12 로 온다 — 잠깐 입력으로 놓고 본다(1초 송신 주기라 1~2회 잡힌다).
    uint32_t txHi, txE;
    pinMode(PIN_CO_TX, INPUT_PULLDOWN);
    delay(5);
    sample(&txHi, &txE, PIN_CO_TX);
    if (wasOn) s_uart.begin(CO_BAUD, EspSoftwareSerial::SWSERIAL_8N1, PIN_CO_RX, PIN_CO_TX, false, 64, 640);
    Serial.printf("[CO] GPIO%d 풀업: HIGH %lu%% 변화 %lu회 · 풀다운: HIGH %lu%% 변화 %lu회\n", PIN_CO_RX,
                  (unsigned long)upHi, (unsigned long)upE, (unsigned long)dnHi, (unsigned long)dnE);
    const char *why;
    if (upE + dnE > 4)              why = "신호 있음 — 센서가 보내는 중, co raw 로 바이트 확인";
    else if (dnHi > 90)             why = "센서 TX 가 HIGH 로 붙잡고 있음(연결됨) — 그런데 안 보냄: 모드·핀 순서(PIN8=TXD) 확인, co query";
    else if (upHi > 90 && dnHi < 10) why = "떠 있음 — 센서 TX 가 GPIO11 에 안 닿음(선·핀 번호·접촉) 또는 센서 전원 없음";
    else if (upHi < 10)              why = "늘 LOW — 센서 전원 없음 또는 다른 핀(GND 등)에 물림";
    else                             why = "판단 불가 — 멀티미터로 센서 PIN8 ↔ GND 전압 확인(쉴 때 약 3V)";
    Serial.printf("[CO] → %s\n", why);
    Serial.printf("[CO] GPIO%d(입력으로 잠깐): HIGH %lu%% 변화 %lu회%s\n", PIN_CO_TX, (unsigned long)txHi,
                  (unsigned long)txE, txE > 4 ? " → ⚠️ 여기로 데이터가 온다 — TX/RX 가 바뀌었다(11↔12 맞바꿈)" : "");
}

bool tryConsole(const char *line)
{
    if (!strcmp(line, "co")) {
        printValues(Serial);
        printStatus(Serial);
        return true;
    }
    if (!strcmp(line, "co raw")) {
        s_raw = !s_raw;
        Serial.printf("[CO] 수신 바이트 16진 출력 %s\n[CO] ", s_raw ? "켬 (다시 co raw 로 끔)" : "끔");
        return true;
    }
    if (!strcmp(line, "co pin")) {
        probePin();
        return true;
    }
    if (!strcmp(line, "co query")) {
        // 질의 명령(매뉴얼 Q&A 읽기). 응답은 co raw 로 본다. 센서가 질의응답 모드로 바뀔 수 있다.
        if (s_off) {
            Serial.println("[CO] 레일 꺼짐 — 보낼 수 없음");
            return true;
        }
        static const uint8_t q[9] = {0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79};
        s_uart.write(q, sizeof(q));
        Serial.println("[CO] 질의 보냄 FF 01 86 00 00 00 00 00 79 — co raw 로 응답 확인");
        return true;
    }
    return false;
}

void printStatus(Stream &io)
{
    io.println("[CO] ZE15-CO");
    io.printf("  state        : %s\n", s_off ? "꺼짐(가드·주차 듀티)" : warming() ? "예열 중" : s_ok ? "측정" :
                                       s_fault ? "고장 비트" : "수신 없음");
    if (s_ok)
        io.printf("  last         : %u.%u ppm, 범위 %u ppm (%lums 전)\n", (unsigned)(s_co10 / 10),
                  (unsigned)(s_co10 % 10), (unsigned)(s_range10 / 10), (unsigned long)(millis() - s_at));
    io.printf("  stats        : bytes %lu, frames %lu, checksum %lu, type %lu, fault %lu\n", (unsigned long)s_bytes,
              (unsigned long)s_frames, (unsigned long)s_badSum, (unsigned long)s_badType, (unsigned long)s_faults);
}

} // namespace Co
