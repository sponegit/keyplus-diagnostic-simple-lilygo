/**
 * @file      log.cpp
 * @brief     UART 로그 레벨 구현 (NVS "cfg" / 키 "log_lv").
 */
#include "log.h"
#include <Preferences.h>

namespace Log {

static const char *kNs = "cfg";   // cfg.cpp와 같은 네임스페이스(키만 분리)

static Level s_level = (Level)LOG_DEFAULT_LEVEL;

static Level clamp(long v)
{
    if (v < (long)L_ERROR) return L_ERROR;
    if (v > (long)L_DEBUG) return L_DEBUG;
    return (Level)v;
}

void begin()
{
    Preferences p;
    p.begin(kNs, /*readOnly=*/true);
    s_level = clamp((long)p.getUChar("log_lv", (uint8_t)LOG_DEFAULT_LEVEL));
    p.end();
}

Level level() { return s_level; }

const char *levelName(Level lv)
{
    switch (lv) {
        case L_ERROR: return "ERROR";
        case L_WARN:  return "WARN";
        case L_INFO:  return "INFO";
        case L_DEBUG: return "DEBUG";
    }
    return "?";
}

void setLevel(Level lv, Stream &io)
{
    s_level = clamp((long)lv);
    Preferences p;
    p.begin(kNs, /*readOnly=*/false);
    p.putUChar("log_lv", (uint8_t)s_level);
    p.end();
    // 레벨 변경 자체는 레벨과 무관하게 알린다(끈 뒤 왜 조용한지 알 수 있어야 한다).
    io.printf("[LOG] level=%s\n", levelName(s_level));
    if ((uint8_t)s_level > LOG_LEVEL_MAX) {
        io.printf("[LOG] ⚠️ 빌드 상한 %s — 그 이상은 출력되지 않음\n",
                  levelName((Level)LOG_LEVEL_MAX));
    }
}

bool tryConsole(const String &cmd, const String &arg, Stream &io)
{
    if (cmd != "log") return false;

    if (arg.isEmpty()) {   // 현재 레벨 조회
        io.printf("[LOG] level=%s (빌드 상한 %s)\n",
                  levelName(s_level), levelName((Level)LOG_LEVEL_MAX));
        return true;
    }

    String a = arg; a.toLowerCase();
    if      (a == "error" || a == "e" || a == "1") setLevel(L_ERROR, io);
    else if (a == "warn"  || a == "w" || a == "2") setLevel(L_WARN,  io);
    else if (a == "info"  || a == "i" || a == "3") setLevel(L_INFO,  io);
    else if (a == "debug" || a == "d" || a == "4") setLevel(L_DEBUG, io);
    else io.printf("[LOG] 알 수 없는 레벨 '%s' — error|warn|info|debug\n", arg.c_str());
    return true;
}

} // namespace Log
