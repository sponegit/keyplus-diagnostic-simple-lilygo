/**
 * @file      apn.h
 * @brief     통신사 자동 판별 — 3사(LG U+/SKT/KT) 유심에서 APN 을 스스로 찾는다.
 *
 * 배경: APN 이 config.h 상수로 박혀 있어 LG U+ 유심만 데이터가 붙었다. KT/SKT 유심을
 *       꽂으면 망 등록까지는 되고(등록은 유심의 홈망으로 자동이다) PDP 활성에서 실패한다.
 *       → 통신사가 갈리는 지점은 **APN 하나뿐**이므로, 그 한 값만 자동으로 정하면 된다.
 *
 * 결정 순서 (Lte::begin 이 등록 전에 select() 로 묻는다):
 *   ① 수동 고정   — 콘솔 'apn set <값>' 으로 사람이 박은 값. 무조건 최우선.
 *   ② NVS 캐시    — 지난번 PDP 성공값. **같은 유심(ICCID 일치)일 때만** 쓴다.
 *   ③ IMSI 표     — AT+CIMI 앞 5자리(MCC+MNC)로 통신사 판별. AT 1왕복, 실패 시도 0회.
 *   ④ 후보 회전   — 표에 없는 MNC(알뜰폰 등). 브링업 시도마다 후보를 하나씩 돌려본다.
 * 등록 후 PDP 가 실패하면 Lte::begin 이 추가로
 *   ⑤ 망 할당 APN(AT+CGDCONT? 의 CID 1) → ⑥ 나머지 후보 순회
 * 까지 훑고, 붙은 값을 commit() 으로 NVS 에 굳힌다(다음 부팅부터는 ②로 바로 간다).
 *
 * ⚠️ **KT/SKT 유심 실기 검증이 안 됐다**(해당 유심 미보유). 표의 KT/SKT APN 은 소매 LTE
 *    표준값이라 M2M/IoT 요금제에서는 다를 수 있다. 그래서 ⑤⑥ 폴백과 콘솔 수동 고정을
 *    같이 넣었다 — 표가 틀려도 현장에서 재플래싱 없이('apn set …' 후 재부팅) 끝난다.
 *
 * ⚠️ ICCID 로 캐시를 무효화하므로 **유심 교체는 "갈아끼우고 재부팅"이 전부**다.
 *    (핫스왑은 어차피 불가 — 모뎀이 SIM 을 부팅 시 한 번만 읽는다)
 *
 * NVS: 네임스페이스 "apn" / 키 apn·iccid·manual. 쓰기는 값이 바뀔 때만 한다
 *      (재접속마다 쓰면 플래시가 닳는다).
 */
#pragma once

#include "utilities.h"
#include <TinyGsmClient.h>

namespace Apn {

// NVS 로드. setup 에서 Cfg::begin() 옆에 1회. 모뎀 AT 를 쓰지 않는다.
void begin();

// 이번 브링업에 쓸 APN 을 정한다(위 ①~④). 반환값은 내부 정적 버퍼 — 오래 들고 있으려면
// 복사할 것. 모뎀이 SIM 을 읽을 수 있는 상태(SIM_READY)에서 호출해야 한다.
const char *select(TinyGsm &modem, Stream &log);

// 등록/PDP 가 끝내 실패했을 때 호출. 통신사를 못 가린 상태(④)면 다음 브링업이 다른
// 후보로 시작하도록 회전시킨다. 캐시/IMSI 로 정해진 경우엔 아무 일도 하지 않는다.
void noteBringupFailed();

// PDP 가 실제로 붙은 APN 을 확정 저장(ICCID 와 함께). 값이 그대로면 NVS 를 쓰지 않는다.
// 수동 고정 상태에서는 무시한다(사람이 정한 값이 계속 최우선이어야 한다).
void commit(const char *apn, Stream &log);

// 후보 APN 목록(표 순서: LG U+ → SKT → KT). PDP 실패 후 순회에 쓴다.
int         candidateCount();
const char *candidate(int i);

// 망이 attach 때 내려준 APN(AT+CGDCONT? 의 CID 1). 없으면 빈 문자열.
// 표가 틀렸을 때 이 값이 정답인 경우가 많다 — PDP 폴백의 첫 후보.
String networkApn(TinyGsm &modem);

// 현재 선택된 APN 과 그 출처(로그/status 표시용).
const char *current();
const char *sourceStr();

// 콘솔 'apn' / 'apn set <값>' / 'apn clear'.
// ⚠️ AT 왕복이 있다 — 호출측이 URC 를 먼저 비운 뒤 부를 것(.ino drainModemUrc).
void console(const String &arg, TinyGsm &modem, Stream &io);

} // namespace Apn
