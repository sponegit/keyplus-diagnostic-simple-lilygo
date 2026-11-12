/**
 * @file      pms.cpp
 * @brief     PMS5003 구현. 설계는 pms.h.
 */
#include "pms.h"
#include "config.h"

namespace Pms {

// 데이터시트(Plantower PMS5003 V2.3) 부록 I — 프레임:
//   [0x42 0x4D][길이 2B = 28][데이터 13워드][체크섬 2B = 앞 30바이트 합]
//   워드 0~2 CF=1 표준입자, 3~5 대기환경(PM1.0·PM2.5·PM10), 6~11 0.1L 당 입자 수(0.3~10µm), 12 예약.
static const uint8_t  FRAME_LEN  = 32;
static const uint16_t BODY_LEN   = 28;

static HardwareSerial s_uart(2);

static uint8_t  s_buf[FRAME_LEN];
static uint8_t  s_pos      = 0;
static bool     s_ok       = false;
static uint16_t s_pm1 = 0, s_pm25 = 0, s_pm10 = 0;
static uint16_t s_cnt03 = 0, s_cnt25 = 0;    // 0.3µm↑ · 2.5µm↑ 입자 수 — 반응 확인용
static uint32_t s_at       = 0;              // 마지막 정상 프레임
static uint32_t s_wakeAt   = 0;              // 마지막으로 깨운 시각(부팅 포함)
static bool     s_sleeping = false;
static bool     s_off      = true;           // 레일 꺼짐(온도 가드·부팅 직후) — UART 도 닫혀 있다
static uint32_t s_frames = 0, s_badSum = 0, s_badLen = 0;

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static void onFrame()
{
    if (be16(s_buf + 2) != BODY_LEN) { s_badLen++; return; }
    uint16_t sum = 0;
    for (uint8_t i = 0; i < FRAME_LEN - 2; i++) sum += s_buf[i];
    if (sum != be16(s_buf + FRAME_LEN - 2)) { s_badSum++; return; }
    const uint8_t *d = s_buf + 4;
    s_pm1   = be16(d + 6);    // 대기환경 값 — 실내·차량 실내 기준(CF=1 은 공장 환경용)
    s_pm25  = be16(d + 8);
    s_pm10  = be16(d + 10);
    s_cnt03 = be16(d + 12);
    s_cnt25 = be16(d + 18);
    s_ok    = true;
    s_at    = millis();
    s_frames++;
}

// 명령 프레임: 42 4D CMD DATAH DATAL 체크섬(앞 5바이트 합)
static void sendCmd(uint8_t cmd, uint16_t data)
{
    uint8_t f[7] = {0x42, 0x4D, cmd, (uint8_t)(data >> 8), (uint8_t)data, 0, 0};
    uint16_t sum = 0;
    for (int i = 0; i < 5; i++) sum += f[i];
    f[5] = (uint8_t)(sum >> 8);
    f[6] = (uint8_t)sum;
    s_uart.write(f, sizeof(f));
}

static void setSleep(bool sleep)
{
    digitalWrite(PIN_PMS_SET, sleep ? LOW : HIGH);
    sendCmd(0xE4, sleep ? 0x0000 : 0x0001);   // sleep / wakeup
    s_sleeping = sleep;
    s_ok = false;
    s_pos = 0;
    if (!sleep) s_wakeAt = millis();
}

void begin()
{
    // 레일을 켤 때까지(Guard 첫 판단) SET LOW · UART 닫음. 배선하지 않은 벤치에서는 SET 이 아무 영향이 없다.
    digitalWrite(PIN_PMS_SET, LOW);
    pinMode(PIN_PMS_SET, OUTPUT);
    s_off = true;
}

void setPower(bool on)
{
    if (on == !s_off) return;
    if (on) {
        digitalWrite(PIN_PMS_SET, HIGH);
        s_uart.begin(PMS_BAUD, SERIAL_8N1, PIN_PMS_RX, PIN_PMS_TX);
        s_off = false;
        // 게이트 회로가 있으면 방금 전원이 들어와 이미 동작 중이다. 벤치(상시 전원)에서는 슬립을 풀어야 한다.
        setSleep(false);
        return;
    }
    // 벤치에서는 전원이 그대로라 팬을 명령으로 세운다(차단이 눈에 보이게). 게이트가 있으면 무해하다.
    sendCmd(0xE4, 0x0000);
    s_uart.flush();
    s_uart.end();
    // 꺼진 센서로 신호선이 전류를 밀어 넣지 않게(back-feed) TX·SET 을 내린다.
    pinMode(PIN_PMS_TX, INPUT);
    digitalWrite(PIN_PMS_SET, LOW);
    s_off      = true;
    s_sleeping = false;
    s_ok       = false;
    s_pos      = 0;
}

void update()
{
    if (s_off) return;
    while (s_uart.available() > 0) {
        const uint8_t c = (uint8_t)s_uart.read();
        // 머리(42 4D)를 맞출 때까지 한 바이트씩 버린다 — 중간부터 받아도 다음 프레임에서 맞는다.
        if (s_pos == 0 && c != 0x42) continue;
        if (s_pos == 1 && c != 0x4D) { s_pos = (c == 0x42) ? 1 : 0; continue; }
        s_buf[s_pos++] = c;
        if (s_pos == FRAME_LEN) {
            onFrame();
            s_pos = 0;
        }
    }
    // 프레임이 끊기면 값을 오래 붙들지 않는다 — active 모드는 최대 2.3초 간격.
    if (s_ok && (uint32_t)(millis() - s_at) > PMS_STALE_MS) s_ok = false;
}

static bool warming() { return !s_off && !s_sleeping && (uint32_t)(millis() - s_wakeAt) < PMS_WARMUP_MS; }

bool get(uint16_t *pm1, uint16_t *pm25, uint16_t *pm10)
{
    if (s_off || s_sleeping || !s_ok || warming()) return false;
    *pm1  = s_pm1;
    *pm25 = s_pm25;
    *pm10 = s_pm10;
    return true;
}

void printValues(Stream &io)
{
    if (s_off)
        io.println("[AIR] PMS5003 꺼짐 (온도 가드·주차 듀티 — guard)");
    else if (s_sleeping)
        io.println("[AIR] PMS5003 슬립 중 (pms wake)");
    else if (s_ok)
        io.printf("[AIR] PMS5003 PM1.0 %u  PM2.5 %u  PM10 %u µg/m³%s\n",
                  (unsigned)s_pm1, (unsigned)s_pm25, (unsigned)s_pm10, warming() ? "  (안정화 중)" : "");
    else
        io.printf("[AIR] PMS5003 없음 (프레임 수신 없음%s)\n", warming() ? " — 깨운 직후" : " — 5V·TX→GPIO18 확인");
}

bool tryConsole(const char *line)
{
    if (!strcmp(line, "pms")) {
        printValues(Serial);
        printStatus(Serial);
        return true;
    }
    if (s_off && (!strcmp(line, "pms sleep") || !strcmp(line, "pms wake"))) {
        Serial.println("[PMS] 레일이 꺼져 있다(온도 가드·주차 듀티) — guard 로 확인");
        return true;
    }
    if (!strcmp(line, "pms sleep")) {
        setSleep(true);
        Serial.println("[PMS] 슬립 — 팬이 멈춰야 한다");
        return true;
    }
    if (!strcmp(line, "pms wake")) {
        setSleep(false);
        Serial.printf("[PMS] 깨움 — 팬 재가동, %lu초 안정화\n", (unsigned long)(PMS_WARMUP_MS / 1000));
        return true;
    }
    return false;
}

void printStatus(Stream &io)
{
    io.println("[PMS] PMS5003");
    io.printf("  state        : %s\n", s_off ? "꺼짐(가드·주차 듀티)" : s_sleeping ? "슬립" : warming() ? "안정화 중" :
                                       s_ok ? "측정" : "수신 없음");
    if (s_ok)
        io.printf("  last         : PM2.5 %u µg/m³, 입자 0.3µm↑ %u · 2.5µm↑ %u /0.1L (%lums 전)\n",
                  (unsigned)s_pm25, (unsigned)s_cnt03, (unsigned)s_cnt25, (unsigned long)(millis() - s_at));
    io.printf("  stats        : frames %lu, checksum %lu, length %lu\n",
              (unsigned long)s_frames, (unsigned long)s_badSum, (unsigned long)s_badLen);
}

} // namespace Pms
