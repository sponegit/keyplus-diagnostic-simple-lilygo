/**
 * @file      ign.h
 * @brief     시동 판정 — 차량 전압(서브 INA228) 기준, OBD 회전수로 보정
 *
 * 시동이 걸려 알터네이터가 돌면 배터리 단자가 13V 이상(보통 13.5~14.5V), 시동을 끄면 12V대로 내려온다.
 * 이 차이로 판정한다. 전압은 서브가 POWER 프레임(1초)으로 보낸다.
 *
 *   OFF → ON  : IGN_ON_MV 이상이 IGN_ON_MS 이어짐
 *   ON  → OFF : IGN_OFF_MV 미만이 IGN_OFF_MS 이어짐   (크랭킹 순간 강하·전장 부하 흔들림은 지연이 거른다)
 *
 * 전압만으로 틀리는 두 경우를 OBD 회전수가 바로잡는다. 회전수를 최근 IGN_RPM_FRESH_MS 안에
 * 읽었으면 회전수가 정답이다(rpm > 0 = ON, 0 = OFF).
 *   · 충전 제어 차량(현대·기아 AMS 등)은 주행 중에도 12.5~12.8V 로 내려 충전을 쉰다 → 전압만 보면 OFF.
 *   · 시동 직후 끈 배터리(표면 전하)·주차 중 충전기 연결은 엔진이 꺼져도 13V 근처를 유지한다 → 전압만 보면 ON.
 * 회전수가 판정할 때는 전압 쪽 상태도 그 값으로 맞춰 둔다 — 주차 뒤 CAN 이 잠들어 전압으로 넘어갈 때
 * 남은 표면 전하로 다시 ON 이 되지 않게.
 *
 * 둘 다 없으면(서브 링크 끊김·OBD 없음) OFF 로 본다(src = NONE).
 *
 * 콘솔: ign
 */
#pragma once

#include <Arduino.h>

namespace Ign {

enum class Src : uint8_t { NONE, VOLT, RPM };

// loop 매 틱. 상태가 바뀌었으면 true.
bool update(uint32_t now);

bool on();
Src  source();
const char *srcName(Src s);

bool tryConsole(const String &cmd, const String &arg, Stream &io);
void printStatus(Stream &io);

} // namespace Ign
