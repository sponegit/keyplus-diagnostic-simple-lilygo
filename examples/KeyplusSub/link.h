/**
 * @file      link.h
 * @brief     메인(T-A7670E) UART 링크 — 서브 쪽 (작업서 S2)
 *
 * UART0(TX43·RX44)에 lib/KpLink 의 Session 을 얹는다. 메인 sublink.cpp 와 짝이다.
 * 서브는 메인 다운을 이유로 자기 동작을 바꾸지 않는다(작업서 §3.4) — 메인은 LTE 등록 중
 * 수십 초씩 조용해지는 게 정상이다. 링크 상태는 로그·콘솔로만 알린다.
 *
 * RESET 프레임을 받으면 소프트 재부팅한다(메인 콘솔 'sub reset').
 */
#pragma once

#include <Arduino.h>

namespace Link {

void begin();          // setup 에서 1회 — UART0 open + Session 시작
void poll();           // loop 매 틱 — 수신 → 주기 처리
bool up();
void printStatus(Stream &io);

} // namespace Link
