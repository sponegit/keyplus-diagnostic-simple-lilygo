/**
 * @file      ota.h
 * @brief     서브 펌웨어 OTA 수신 — 메인 중계 (작업서 S5)
 *
 * 메인이 LTE 로 받은 이미지를 OTA_* 프레임으로 넘기면 Update 에 쓴다. 오프셋·중복 판단은
 * lib/KpLink 의 KpOta::Receiver, 하드웨어 세대 검사는 hwmark.h(HW_MODEL "v2-sub")가 한다 —
 * 메인 이미지("v2")가 들어오면 확정 전에 거부한다.
 *
 * 롤백: 새 이미지는 PENDING_VERIFY 로 부팅한다(.ino 의 verifyRollbackLater() 가 자동 확정을 끈다).
 * 메인과 링크가 올라오면 onLinkUp() 이 확정한다. 확정 전 크래시는 부트로더가, 링크가 끝내 안
 * 올라오면 tick() 이 OTA_CONFIRM_DEADLINE_MS 뒤 직전 이미지로 되돌린다. 메인은 서브가 다시
 * 연결될 때 HELLO 의 fw 버전으로 성공/실패를 판정한다.
 */
#pragma once

#include <Arduino.h>
#include <KpLink.h>

namespace Ota {

void begin();                          // setup — 실행 이미지 상태(PENDING_VERIFY) 판정
void onFrame(const KpLink::Frame &f);  // OTA_BEGIN·OTA_CHUNK·OTA_END
void tick();                           // loop 매 틱 — 무진전 포기, baud 복귀, 재부팅, 롤백 期限
void onLinkUp();                       // 링크 업 — 새 이미지 확정
bool active();
void printStatus(Stream &io);

} // namespace Ota
