/**
 * @file      wake.h
 * @brief     light sleep 기상 경로 진단 — RI 핀 / UART 엣지, 그리고 실제 수면 시험
 *
 * 배경: floor 27mA(ESP32 @80MHz)를 더 줄이는 유일한 수단은 light sleep(0.8mA 급)인데,
 * **light sleep 중에는 APB 가 꺼져 UART 가 클럭을 못 받아 들어오는 바이트가 유실된다.**
 * 차키 명령(`+CMQTTRX*`)이 그 경로라 기상 경로를 먼저 확보하지 않으면 재울 수 없다.
 *
 * 후보가 둘이고, **둘 다 하드웨어 작업 없이** 시험할 수 있다.
 *
 *   ① RI 핀 (GPIO33) — A7670 HW 매뉴얼 3.3.2: 모뎀은 URC 를 올리기 전에 RI 를
 *      **60ms LOW** 로 떨어뜨린다(단문은 120ms). 그 엣지로 깨우면 URC 본문을 온전히 받는다.
 *      설계상 정확히 이 용도다. ⚠️ 단 DTR(GPIO25)이 배선돼 있지 않았던 전례가 있으므로
 *      utilities.h 의 핀 정의를 믿지 말고 **엣지가 실제로 잡히는지부터 센다.**
 *
 *   ② UART 엣지 — ESP32 는 light sleep 중 RX 엣지로 깨어날 수 있다(UART0/1, 모뎀은 UART1).
 *      깨우는 데 쓰인 앞쪽 바이트는 유실되지만 **A76xx 의 URC 는 항상 `\r\n` 으로 시작**하므로
 *      그 두 바이트가 희생되고 `+CMQTT...` 본문은 살아남는다.
 *      ⚠️ ESP32 의 UART 기상 회로는 저속 클럭에서 동작하므로 115200 에서 확실히 먹는지는
 *         문서만으로 단정할 수 없다 — 그래서 '실제로 재워보고 무엇이 깨웠는지' 를 본다.
 *
 * 판정 방법
 *   `wake`            RI 엣지 누적 카운터. 발행마다(30초) 1회씩 늘면 RI 배선이 있는 것이다.
 *   `wake sleep [초]`  진짜 light sleep 에 들어갔다가 **무엇이 깨웠는지**를 보고한다.
 *                     PPK2 를 보고 있으면 이 구간이 곧 light sleep 실측 전류다.
 *
 * ⚠️ 이 모듈은 계측 전용이다. 판정이 끝나면 FEATURE_WAKE_PROBE 를 0 으로 내린다.
 */
#pragma once

#include <Arduino.h>
#include "config.h"

#if FEATURE_WAKE_PROBE

namespace Wake {

// RI 핀 인터럽트 부착. modemStream 은 기상 후 남은 바이트를 보고하는 용도이며
// null 이어도 된다(LTE 없는 계측 빌드).
void begin(Stream *modemStream);

// 콘솔 'wake' / 'wake sleep [초]'. 처리했으면 true.
bool tryConsole(const String &cmd, const String &arg, Stream &io);

}  // namespace Wake

#endif  // FEATURE_WAKE_PROBE
