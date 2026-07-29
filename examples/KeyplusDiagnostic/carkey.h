/**
 * @file      carkey.h
 * @brief     5b단계 — 차키(fob) 버튼 GPIO 제어. 현 단계: 열림/잠금만(전원 제어 없음).
 *
 * 하드웨어: 잠금 = GPIO18, 열림 = GPIO19. fob 버튼패드는 active-low(내부 풀업, 누르면 GND).
 *
 * ⚠️ 누름 극성은 배선이 정한다 — config.h CARKEY_DRIVE_SEL 한 줄만 바꾸면 된다:
 *   DIRECT (현재): GPIO를 fob 패드에 직결(오픈드레인).
 *     GPIO LOW → 라인을 GND로 당김 → "눌림" / HIGH → Hi-Z → fob 풀업 복귀 → "뗌"
 *     ⚠️ 반드시 오픈드레인. HIGH를 push하면 꺼진 fob에 역급전(back-feed) 위험.
 *     ⚠️ 공통 GND + fob Vcc=3.3V(상시급전 점퍼) 전제 → 로직 레벨 일치.
 *   MOSFET: GPIO가 2N7002 게이트 구동(드레인=패드, 소스=GND). 핀모드 푸시풀 OUTPUT.
 *     GPIO HIGH → 게이트 ON → 패드를 GND로 → "눌림" / LOW → OFF → "뗌" (직결과 반대)
 *   (car-key-control.md 원리 해설 참조. 직결=LOW누름 / 2N7002=HIGH누름 — 혼동 주의.)
 *
 * config.h CARKEY_ACTIVE_HIGH 는 기본 AUTO(=위 DRIVE_SEL 따름). 실물 배선이 위 가정과
 *   반대로 나온 예외에만 0/1 로 극성을 강제하며, 그때 핀모드는 푸시풀로 고정된다.
 *
 * ⚠️ 부팅 초기: 라인에 외부 풀다운이 없어 begin() 전까지 플로팅될 수 있다.
 *   → setup 초반(가능한 한 빨리) begin()을 호출해 두 핀을 뗌(release) 상태로 확정.
 *
 * 전원(GPIO32 LDO EN)은 이 단계에서 제어하지 않는다 — fob는 상시급전(3.3V 점퍼) 전제.
 * 예비(GPIO23)는 상태표시 LED와 물리핀 공유라 이 단계에서 다루지 않는다.
 *
 * 트리거: 현재는 시리얼 Debug Console(lock/unlock)로 검증. MQTT cmd 다운링크 연동은
 *   다음 증분(persistent 세션 + keepalive 대응 후).
 */
#pragma once
#include <Arduino.h>

namespace Carkey {

enum class Button { LOCK, UNLOCK };

// 잠금/열림 핀을 OUTPUT LOW(버튼 뗌)로 확정. setup 초반에 호출(게이트 조기 해제).
void begin();

// 버튼 1회 누름: 누름 레벨 인가 → holdMs 유지 → 뗌 레벨 복귀. 블로킹(~holdMs).
// LED는 Ticker 구동이라 이 블로킹 동안에도 멈추지 않는다.
void press(Button b, uint16_t holdMs);

// 편의 래퍼 (holdMs 기본값 = config.h CARKEY_PRESS_MS).
void lock();
void unlock();

// Debug Console 훅: cmd가 "lock"/"unlock"이면 실행하고 true 반환(그 외 false).
// 시리얼 단일 소유자(Prov::handleSerial)의 미지 명령 분기에서 호출한다.
bool tryConsole(const String &cmd, const String &arg, Stream &io);

} // namespace Carkey
