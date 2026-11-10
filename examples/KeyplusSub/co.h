/**
 * @file      co.h
 * @brief     ZE15-CO 일산화탄소 — 서브 (작업서 S8 6단계)
 *
 * 소프트웨어 시리얼 RX GPIO11, 9600 8N1. 센서는 기본(자동 송신) 모드에서 1초마다 9바이트를 보낸다
 * (Winsen ZE15-CO 매뉴얼 v1.1 Table 4):
 *   [FF][04 가스=CO][03 단위=ppm][소수 자릿수][농도 H][농도 L][범위 H][범위 L][체크섬]
 *   농도 H 의 bit7 = 센서 고장, 하위 5비트만 값. 체크섬 = ~(Byte1..Byte7 합) + 1.
 * 질의 명령을 보내면 질의응답 모드로 바뀌므로(30초 무질의 시 복귀) 평소에는 보내지 않는다 — TX(GPIO12)는 쉬는 레벨 HIGH.
 * 진단 명령 co query 만 질의를 보낸다.
 * ⚠️ 매뉴얼 Table 6(질의 응답) 예시의 체크섬 0x7A 는 오타다(식대로면 0x70). 검증은 식으로 한다.
 *
 * 전원은 5V CO 레일(GPIO41, Guard)을 따른다. 켠 뒤 CO_WARMUP_MS 는 값을 받되 발행하지 않는다.
 *
 * 콘솔: co (현재 값·통계) | co raw (수신 바이트 16진, 토글) | co pin (GPIO11 레벨 1초 측정)
 *       co query (질의 명령 송신 — 응답은 co raw 로)
 */
#pragma once

#include <Arduino.h>

namespace Co {

void begin();                        // 꺼진 상태로 시작 — Guard 가 첫 판단 후 setPower(true)
void update();                       // loop 매 틱 — 쌓인 바이트를 파싱

// 5V CO 레일(GPIO41)에 맞춰 부른다(Guard). 끄면 수신을 닫고 TX 를 내린다(꺼진 센서로 back-feed 방지).
void setPower(bool on);

// 발행할 만한 값이면 true(0.1ppm 단위) — 수신 중·예열 끝·고장 비트 없음.
bool get(uint16_t *co10);

bool tryConsole(const char *line);
void printValues(Stream &io);        // 'air' 한 줄 요약
void printStatus(Stream &io);

} // namespace Co
