/**
 * @file      led.h
 * @brief     서브 상태 LED (GPIO10, HARDWARE §5.4) — 센싱·전원 표시
 *
 * 메인 LED(23)는 통신, 서브 LED 는 센싱·전원만 표시한다. 여러 사유가 겹치면 위가 이긴다:
 *
 *   우선  상태                 패턴 (2초 주기 반복)
 *   1    차키 누름 중          계속 켬
 *   2    저전압 차단(CUT)      빠른 점멸 (100ms 켬 / 100ms 끔)
 *   3    저전압 경고(WARN)     짧게 3번 + 쉼
 *   4    온도 가드(고온·저온)   짧게 2번 + 쉼
 *   5    센서 고장             짧게 1번 + 쉼
 *   -    정상                 꺼짐 (주차 중 야간 눈부심·전력)
 *
 * 센서 고장: SHT41 없음(온도 감시 불가) · INA228 없음(저전압 감시 불가) · ZE15-CO 고장 비트.
 *   부팅 후 LED_FAULT_GRACE_MS 동안은 보지 않는다(첫 탐색 전).
 *   다른 센서(SCD41·SGP41·BMP280·PMS)는 벤치에서 빠져 있을 수 있어 넣지 않았다 — 값 누락은 서버에서 본다.
 *
 * 콘솔: led (지금 패턴·사유) | led test (패턴을 차례로 3초씩 보여 줌, 다시 치면 끔)
 */
#pragma once

#include <Arduino.h>

namespace Led {

void begin();                // 핀을 끈 상태로 출력 설정 — Carkey::begin 보다 먼저
void update();               // loop 매 틱
void setKey(uint8_t io, bool pressed);   // 차키 채널(1~6) 누름/뗌 (carkey.cpp)
void clearKeys();                        // 모두 뗌 (key abort)

bool tryConsole(const char *line);
void printStatus(Stream &io);

} // namespace Led
