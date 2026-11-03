/**
 * @file      carkey.h
 * @brief     차키 fob 버튼 구동 — 서브 (작업서 S4)
 *
 * GPIO4~9 = 채널 io1~6 (버튼 배정은 설치마다 다르다). 시퀀스 실행·중복 방지·검증은
 * lib/KpLink 의 KpKey::Executor 가 하고, 여기서는 채널을 핀에 잇고 결과를 링크로 돌려보낸다.
 *
 * ⚠️ 부팅 시 차키 핀은 하이임피던스여야 한다(fob 풀업이 "뗌" 유지). begin() 은 출력 레벨을
 *    먼저 "뗌"으로 쓴 뒤 핀 모드를 바꾼다 — 순서를 바꾸면 출력 레지스터 기본값(LOW)이 잠깐
 *    나가 DIRECT 배선에서 버튼이 눌린다.
 *    (arduino-esp32 2.x — 이 프로젝트가 고정한 espressif32@6.12.0 — 는 pinMode 전 digitalWrite 가
 *     출력 레지스터에 그대로 써진다. 프레임워크 3.x 로 올리면 이 동작을 다시 확인할 것.)
 */
#pragma once

#include <Arduino.h>

namespace Carkey {

void begin();
void update();                                    // loop 매 틱
void onCmd(const uint8_t *payload, size_t len);   // KEY_CMD 수신
void forget();                                    // 링크 재연결 — 중복 판정 기억 초기화
void abort();                                     // 즉시 모두 뗌

// 콘솔: key <io 1~6> [ms] | key abort | key
bool tryConsole(const char *line);
void printStatus(Stream &io);

} // namespace Carkey
