/**
 * @file      link.h
 * @brief     메인(T-A7670E) UART 링크 — 서브 쪽 (작업서 S2)
 *
 * UART0(TX43·RX44)에 lib/KpLink 의 Session 을 얹는다. 메인 sublink.cpp 와 짝이다.
 * 서브는 메인 다운을 이유로 자기 동작을 바꾸지 않는다(작업서 §3.4) — 메인은 LTE 등록 중
 * 수십 초씩 조용해지는 게 정상이다. 링크 상태는 로그·콘솔로만 알린다.
 *
 * RESET 프레임을 받으면 소프트 재부팅한다(메인 콘솔 'sub reset').
 *
 * 콘솔 중계(작업서 S3): 메인 LOG 프레임을 USB-CDC 에 "[M] " 접두로 출력하고,
 * 서브 콘솔의 'm <명령>' 을 CONSOLE_IN 프레임으로 메인에 보낸다.
 */
#pragma once

#include <Arduino.h>

namespace Link {

void begin();          // setup 에서 1회 — UART0 open + Session 시작
void poll();           // loop 매 틱 — 수신 → 주기 처리
bool up();
// 앱 프레임 송신(KEY_RESULT 등). 링크 다운이면 false.
bool sendFrame(uint8_t type, const uint8_t *payload, uint16_t len);
// 메인 콘솔에 한 줄 입력을 보낸다(줄바꿈은 여기서 붙인다). 링크 다운이면 false.
bool sendConsoleLine(const char *line);
void printStatus(Stream &io);

} // namespace Link
