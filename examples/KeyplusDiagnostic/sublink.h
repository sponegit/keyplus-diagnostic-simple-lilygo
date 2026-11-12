/**
 * @file      sublink.h
 * @brief     v2 서브보드(T7-S3) UART 링크 — 메인 쪽 (작업서 S2)
 *
 * UART2(TX13·RX34)에 lib/KpLink 의 Session 을 얹는다. 연결 확인(HELLO)·생존 신호·
 * 끊김 감지·재연결은 Session 이 하고, 여기서는 UART 와 로그·콘솔만 잇는다.
 *
 * ⚠️ poll() 은 loop 매 틱 부른다. setup 이 막혀 있는 동안(모뎀 대기·LTE 등록)은 링크가
 *    올라오지 않는다 — 서브 HELLO 는 RX 버퍼(SUBLINK_RX_BUF)에 쌓였다가 loop 진입 후 처리된다.
 *    loop 가 수십 초 막혀도 같다. 그동안 서브는 먼저 끊고(peer-lost), 풀리면 다시 연결된다.
 *
 * 콘솔 중계(작업서 S3): console() 이 돌려주는 스트림을 console.cpp 의 TeeStream 보조 자리에 꽂는다.
 *   쓰기 — 로그를 줄 단위 LOG 프레임으로 서브에 보낸다(서브 USB-CDC 에 "[M] " 접두로 출력).
 *          로그는 버려도 되는 데이터다: 링크 다운이면 버리고, 송신 버퍼가 차 있으면 버린다
 *          (차키 프레임이 로그 뒤에 밀리지 않게). 혼잡으로 버린 줄 수는 다음 줄 앞에 표시한다.
 *   읽기 — 서브 콘솔의 'm <명령>' 이 CONSOLE_IN 프레임으로 와서 여기서 읽힌다.
 *
 * 콘솔: status link / sub reset
 */
#pragma once

#include <Arduino.h>
#include <KpGnss.h>

namespace Sublink {

// UART2 open + Session 시작. setup 초반에 1회.
void begin();

// 수신 바이트 처리 → 주기 처리(HELLO·HEARTBEAT·타임아웃). loop 매 틱.
void poll();

bool up();
// 서브 펌웨어 버전(HELLO). 아직 모르면 빈 문자열.
const char *peerFw();

// 앱 프레임 송신(차키 KEY_CMD 등). 링크 다운이면 보내지 않고 false.
bool sendFrame(uint8_t type, const uint8_t *payload, uint16_t len);

// 서브 OTA 지원(S5) — 링크 UART 속도 변경, 로그 중계 억제(OTA 조각과 대역을 다투지 않게).
void setBaud(uint32_t baud);
void setLogMuted(bool muted);

// 서브 공기질(AIR, 작업서 S8) telemetry 조각 — {"t":..,...} 를 out 에 쓰고 길이를 돌려준다.
// 받은 지 SUBLINK_AIR_STALE_MS 가 지났거나 담긴 값이 없으면 0(호출자가 air 키째 생략).
size_t airJson(char *out, size_t cap);

// 서브 전원(POWER, 작업서 S6) telemetry 조각 — {"v":..,"i":..,"uv":..}. 오래됐거나 받은 적 없으면 0.
size_t pwrJson(char *out, size_t cap);

// 서브 전원 최신 표본 — 차량 전압(mV)과 받은 시각(millis). 받은 적 없으면 false.
// 신선도는 호출자가 본다(시동 판정 ign.cpp 는 새 표본이 올 때만 반영한다).
bool pwrSample(uint16_t &mv, uint32_t &atMs);

// 서브 GNSS(작업서 S7 2단계) 최신 프레임과 받은 시각(millis). 받은 적 없으면 false.
// 신선도는 호출자가 본다(SUBLINK_GNSS_STALE_MS — 링크가 끊겼는데 마지막 fix 를 "지금"으로 쓰면 안 된다).
bool gnssSample(KpGnss::Values &v, uint32_t &atMs);

// 서브 소프트 재부팅 요청(RESET 프레임). 링크가 업일 때만 보낸다.
bool requestReset();

// 콘솔 중계 스트림 — console.cpp 가 DbgConsole(TeeStream)의 보조 포트로 쓴다.
// begin() 전에 써도 안전하다(링크 다운 취급 → 버림).
Stream &console();

// 콘솔 'status link' 본문.
void printStatus(Stream &io);

// 콘솔 훅: 'sub reset' 이면 처리하고 true.
bool tryConsole(const String &cmd, const String &arg, Stream &io);

} // namespace Sublink
