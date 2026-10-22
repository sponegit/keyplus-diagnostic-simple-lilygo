/**
 * @file      carkey.h
 * @brief     차키 — v2 메인 쪽 대리 모듈 (작업서 S4)
 *
 * v2 에서 fob 버튼 GPIO 는 서브보드(T7-S3 GPIO4~9)에 있다. 메인은 시퀀스를 KEY_CMD 프레임으로
 * 보내고 서브의 KEY_RESULT 로 결과를 받는다. 재전송·중복 방지·타임아웃은 lib/KpLink 의
 * KpKey::Client(메인) / KpKey::Executor(서브)가 한다 — 실행은 최대 1회.
 *
 * 결과는 비동기다(원격시동 시퀀스는 수 초). 시작한 쪽이 결과를 가져간다:
 *   REMOTE  — 서버 명령(cmd.cpp). takeRemoteOutcome() 으로 꺼내 ack 를 낸다.
 *   CONSOLE — 콘솔 lock/unlock/trunk/key. update() 가 결과를 로그로 남긴다.
 * 한 번에 하나만 진행한다. 진행 중이면 새 요청은 BUSY 로 거절된다(v1 과 같은 정책).
 *
 * 링크가 끊기면(Sublink) 진행 중 요청은 즉시 실패로 끝난다 — 서브가 이미 실행했을 수는 있다.
 * DONE 이 유실돼도 같다(timeout). 둘 다 "실행 여부를 확인 못 함"이라 failed 로 정직하게 낸다.
 */
#pragma once

#include <Arduino.h>
#include <KpKey.h>

namespace Carkey {

enum class Owner : uint8_t { REMOTE, CONSOLE };

// 요청 id 시작값을 난수로 잡는다(메인 재부팅 후 서브의 중복 판정 기억과 겹치지 않게 — 서브도
// 링크 재연결 때 기억을 지우지만 이중으로 막는다). setup 에서 1회.
void begin();

// loop 매 틱 — 재전송·타임아웃, 콘솔 요청 결과 로그.
void update();

// 시퀀스 요청. OK 가 아니면 아무것도 보내지 않았다(INVALID 면 *err 에 사유).
KpKey::Client::Start start(const KpKey::Step *steps, size_t n, Owner owner, KpKey::Error *err);

// 서버 명령 요청의 결과를 한 번 꺼낸다. 없으면 false.
bool takeRemoteOutcome(bool *done, KpKey::Client::Fail *why);

bool busy();

// Sublink 에서 호출 — KEY_RESULT 수신 / 링크 끊김.
void onResult(const uint8_t *payload, size_t len);
void linkLost();

// 콘솔: lock [ms] | unlock [ms] | trunk [ms] | key <io 1~6> [ms]
bool tryConsole(const String &cmd, const String &arg, Stream &io);
void printStatus(Stream &io);

} // namespace Carkey
