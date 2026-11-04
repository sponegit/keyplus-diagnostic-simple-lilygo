/**
 * @file      pms.h
 * @brief     PMS5003 미세먼지 — 서브 (작업서 S8 5단계)
 *
 * UART2(RX18 · TX21, 9600 8N1). 센서는 기본(active) 모드에서 스스로 약 1초마다 32바이트 프레임을
 * 보낸다 — 여기서는 받아서 체크섬을 보고 대기환경 값(PM1.0·PM2.5·PM10, µg/m³)을 꺼낸다.
 *
 * 슬립: UART 명령(0xE4)과 SET 핀(GPIO47, LOW=슬립)을 함께 쓴다. SET 을 배선하지 않은 벤치에서도
 * UART 명령만으로 팬이 멈춘다. 깨운 뒤 30초(PMS_WARMUP_MS)는 팬·레이저 안정화 — 값은 받되 "안정화 중"으로 표시.
 *
 * 콘솔: pms (현재 값·통계) | pms sleep | pms wake
 */
#pragma once

#include <Arduino.h>

namespace Pms {

void begin();                        // 꺼진 상태로 시작 — Guard 가 첫 판단 후 setPower(true)
void update();                       // loop 매 틱 — 쌓인 바이트를 파싱

// 5V 먼지 레일(GPIO39)에 맞춰 부른다(Guard). 켜면 UART 열고 30초 안정화, 끄면 슬립 명령 후
// UART 를 닫고 TX·SET 을 내린다(꺼진 센서로 back-feed 방지).
void setPower(bool on);

// 발행할 만한 값이면 true — 수신 중이고 슬립·안정화 중이 아닐 때만.
bool get(uint16_t *pm1, uint16_t *pm25, uint16_t *pm10);

bool tryConsole(const char *line);
void printValues(Stream &io);        // 'air' 한 줄 요약
void printStatus(Stream &io);

} // namespace Pms
