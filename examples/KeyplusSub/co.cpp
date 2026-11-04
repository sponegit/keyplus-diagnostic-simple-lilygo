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
        // TX 는 라이브러리에 넘기지 않는다(보낼 일이 없다). 쉬는 레벨 HIGH 로 고정 — 잡음이 명령으로 읽히지 않게.
        digitalWrite(PIN_CO_TX, HIGH);
        pinMode(PIN_CO_TX, OUTPUT);
        // RX 버퍼 64B · 엣지 버퍼 640 — loop 가 SCD41 탐색(500ms)으로 막혀도 9바이트/초는 넉넉히 버틴다.
        s_uart.begin(CO_BAUD, EspSoftwareSerial::SWSERIAL_8N1, PIN_CO_RX, -1, false, 64, 640);
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

void printValues(Stream &io)
{
    if (s_off)
        io.println("[AIR] ZE15-CO 꺼짐 (온도 가드 — guard)");
    else if (s_ok)
        io.printf("[AIR] ZE15-CO %u.%u ppm%s\n", (unsigned)(s_co10 / 10), (unsigned)(s_co10 % 10),
                  warming() ? "  (예열 중)" : "");
    else if (s_fault)
        io.println("[AIR] ZE15-CO ⚠️ 센서 고장 비트 — 값 버림");
    else
        io.printf("[AIR] ZE15-CO 없음 (프레임 수신 없음%s)\n", warming() ? " — 켠 직후" : " — 5V·TXD→GPIO11 확인");
}

bool tryConsole(const char *line)
{
    if (!strcmp(line, "co")) {
        printValues(Serial);
        printStatus(Serial);
        return true;
    }
    return false;
}

void printStatus(Stream &io)
{
    io.println("[CO] ZE15-CO");
    io.printf("  state        : %s\n", s_off ? "꺼짐(가드)" : warming() ? "예열 중" : s_ok ? "측정" :
                                       s_fault ? "고장 비트" : "수신 없음");
    if (s_ok)
        io.printf("  last         : %u.%u ppm, 범위 %u ppm (%lums 전)\n", (unsigned)(s_co10 / 10),
                  (unsigned)(s_co10 % 10), (unsigned)(s_range10 / 10), (unsigned long)(millis() - s_at));
    io.printf("  stats        : frames %lu, checksum %lu, type %lu, fault %lu\n", (unsigned long)s_frames,
              (unsigned long)s_badSum, (unsigned long)s_badType, (unsigned long)s_faults);
}

} // namespace Co
