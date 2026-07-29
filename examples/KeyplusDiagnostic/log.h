/**
 * @file      log.h
 * @brief     UART 로그 레벨 — 평상시 앱 상태만, 진단할 때만 상세.
 *
 * 차량에 장착된 뒤에는 시리얼을 붙여 보는 일이 드물고, 붙이더라도 폴 단위 원시값이
 * 계속 흐르면 정작 상태 전이(접속/끊김/링크 up·down)를 놓친다. 그래서 두 축으로 나눈다.
 *
 *   컴파일 타임 상한 LOG_LEVEL_MAX  — 이보다 상세한 호출은 바이너리에서 제거된다.
 *   런타임 레벨      Log::level()   — 기본 INFO. 콘솔 'log debug'로 현장에서 올린다.
 *
 * 레벨 배정 기준:
 *   ERROR 사람이 조치해야 함(배선/allowlist/플래시 실패). 자동 복구 경로 없음.
 *   WARN  실패했지만 재시도/백오프로 복구를 시도함.
 *   INFO  앱 상태 전이(등록/접속/링크 up·down/명령 수신) + 주기 [STAT] 한 줄.
 *   DEBUG 폴 단위 원시값, 프로토콜 상세(AT 왕복/CAN 프레임/HTTP 본문).
 *
 * 부팅 배너(printBootHeader/printDeviceInfo)는 레벨과 무관하게 항상 출력한다 —
 * "지금 무슨 펌웨어가 어떤 신원으로 돌고 있는가"는 어떤 레벨에서도 알아야 한다.
 */
#pragma once

#include <Arduino.h>
#include "config.h"

namespace Log {

// 낮을수록 중요. 런타임 레벨 이하인 것만 출력된다.
enum Level : uint8_t {
    L_ERROR = 1,
    L_WARN  = 2,
    L_INFO  = 3,
    L_DEBUG = 4,
};

// NVS("cfg"/"log_lv")에서 레벨 로드. 없으면 LOG_DEFAULT_LEVEL. Prov::begin 전에 호출 가능.
void begin();

Level level();

// 즉시 적용 + NVS 저장(재부팅 후에도 유지 — 현장에서 붙였다 떼는 진단용).
void setLevel(Level lv, Stream &io);

// 디버그 콘솔 위임: "log" / "log <error|warn|info|debug>". 처리했으면 true.
bool tryConsole(const String &cmd, const String &arg, Stream &io);

const char *levelName(Level lv);

} // namespace Log

// 컴파일 타임 상한과 런타임 레벨을 함께 만족할 때만 출력.
// LOG_LEVEL_MAX 가 상수라 상한에 걸린 호출은 DCE로 통째로 사라진다.
#define LOG_ON(lv)    ((lv) <= (LOG_LEVEL_MAX) && (lv) <= (uint8_t)Log::level())

#define LOGE(io, ...) do { if (LOG_ON(Log::L_ERROR)) (io).printf(__VA_ARGS__); } while (0)
#define LOGW(io, ...) do { if (LOG_ON(Log::L_WARN))  (io).printf(__VA_ARGS__); } while (0)
#define LOGI(io, ...) do { if (LOG_ON(Log::L_INFO))  (io).printf(__VA_ARGS__); } while (0)
#define LOGD(io, ...) do { if (LOG_ON(Log::L_DEBUG)) (io).printf(__VA_ARGS__); } while (0)
