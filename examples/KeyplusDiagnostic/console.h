/**
 * @file      console.h
 * @brief     디버그 콘솔 스트림 — U0(USB-C) + 보조 UART 동시 사용.
 *
 * 진단기는 차량 상시전원에 물려 동작하므로, USB-C를 꽂으면 온보드 USB-Serial 브리지의
 * DTR/RTS 자동리셋 회로가 보드를 재부팅시켜 현장 디버깅이 어렵다. 그렇다고 헤더의
 * UART0(GPIO1/3)에 외부 USB-TTL을 물리면 그 선은 온보드 브리지와 영구히 공유돼
 * 브리지 TX와 어댑터 TX가 GPIO3에서 경합한다(수신은 되는데 송신만 안 먹는 증상).
 *
 * → 외부 어댑터 전용 UART를 따로 열고, 로그 출력과 명령 입력을 두 포트에 동시에 태운다.
 *   USB-C를 쓰든 외부 UART를 쓰든 동작이 완전히 같다.
 *
 * 핀 선정 근거 (LILYGO T-A7670E R2 핀맵):
 *   TX = GPIO13 — 헤더 노출, TF Card CS(이 펌웨어는 SD 미사용), 부트 스트랩 아님
 *   RX = GPIO34 — 헤더 노출, 미사용, 입력 전용이라 RX 전용으로 적합
 *   ⚠️ GPIO34는 내부 풀업이 없다. 어댑터를 빼두면 라인이 떠서 잡음이 문자로 들어온다.
 *      10kΩ 풀업을 3V3로 달아두거나, 어댑터를 상시 연결해 둘 것.
 *      (풀업이 없어도 handleSerial이 비인쇄 문자를 버리므로 오명령 실행은 막힌다)
 */
#pragma once

#include <Arduino.h>
#include "config.h"

// 두 Stream에 동시에 쓰고, 둘 중 아무 쪽에서나 읽는 어댑터.
//   write : 양쪽 모두에 출력 (로그가 두 포트에 다 보인다)
//   read  : primary 우선, 비면 aux에서 (명령을 두 포트 중 아무 데서나 받는다)
// ⚠️ 두 포트에서 동시에 타이핑하면 한 줄 버퍼에 섞인다. 디버그 용도라 허용.
class TeeStream : public Stream {
public:
    TeeStream(Stream &primary, Stream &aux) : _p(primary), _a(aux) {}

    size_t write(uint8_t c) override { _a.write(c); return _p.write(c); }
    size_t write(const uint8_t *buf, size_t n) override { _a.write(buf, n); return _p.write(buf, n); }
    int    available() override { return _p.available() + _a.available(); }
    int    read() override { int c = _p.read(); return (c >= 0) ? c : _a.read(); }
    int    peek() override { int c = _p.peek(); return (c >= 0) ? c : _a.peek(); }
    void   flush() override { _p.flush(); _a.flush(); }

private:
    Stream &_p;
    Stream &_a;
};

// 콘솔 스트림. FEATURE_AUX_CONSOLE=0이면 Serial 그대로다.
// 로그·배너·명령 입력이 모두 이 스트림을 지난다(.ino의 SerialMon).
extern Stream &DbgConsole;

// 두 포트 open. setup() 최초에 호출 — 이후 모든 출력이 양쪽에 나간다.
void consoleBegin();
