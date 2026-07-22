/**
 * @file      carkey.h
 * @brief     5b단계 — 차키(fob) 버튼 GPIO 제어. 현 단계: 열림/잠금만(전원 제어 없음).
 *
 * 하드웨어: 잠금 = GPIO18, 열림 = GPIO19. fob 버튼패드는 active-low(내부 풀업, 누르면 GND).
 *
 * ⚠️ 극성은 배선 방식에 따라 반대다 — config.h CARKEY_DRIVE_SEL로 선택한다:
 *   DIRECT(현재, TR 미준비): GPIO를 fob 패드에 직결(오픈드레인).
 *     GPIO LOW → 라인을 GND로 당김 → "버튼 눌림" / HIGH → Hi-Z → fob 풀업 복귀 → "뗌"
 *     ⚠️ 반드시 오픈드레인. HIGH를 push하면 꺼진 fob에 역급전(back-feed) 위험.
 *     ⚠️ 공통 GND + fob Vcc=3.3V(상시급전 점퍼) 전제 → 로직 레벨 일치.
 *   MOSFET(최종): GPIO가 2N7002 게이트 구동(드레인=패드, 소스=GND).
 *     GPIO HIGH → 게이트 ON → 패드를 GND로 → "눌림" / LOW → OFF → "뗌" (직결과 반대)
 *   (car-key-control.md 원리 해설 참조. 직결=LOW누름 / 2N7002=HIGH누름 — 혼동 주의.)
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

// 버튼 1회 누름: 게이트 HIGH(누름) → holdMs 유지 → LOW(뗌). 블로킹(~holdMs).
// LED는 Ticker 구동이라 이 블로킹 동안에도 멈추지 않는다.
void press(Button b, uint16_t holdMs);

// 편의 래퍼 (holdMs 기본값 = config.h CARKEY_PRESS_MS).
void lock();
void unlock();

// Debug Console 훅: cmd가 "lock"/"unlock"이면 실행하고 true 반환(그 외 false).
// 시리얼 단일 소유자(Prov::handleSerial)의 미지 명령 분기에서 호출한다.
bool tryConsole(const String &cmd, const String &arg, Stream &io);

} // namespace Carkey
