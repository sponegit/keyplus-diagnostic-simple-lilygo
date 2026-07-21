/**
 * @file      provisioning.cpp
 * @brief     device_id NVS 저장 + 시리얼 콘솔 입력 구현.
 */
#include "provisioning.h"
#include "config.h"
#include <Preferences.h>

namespace Prov {

static Preferences s_nvs;
static String s_deviceId;
static String s_mqttPw;
static String s_line;           // 시리얼 라인 누적 버퍼

static const char *NVS_NS      = "prov";
static const char *KEY_DEVICE  = "device_id";
static const char *KEY_MQTT_PW = "mqtt_pw";

// device_id 형식 검증: ^vt-[0-9]{4}-[0-9]{4}-[a-z0-9]{3}$  (계약 deviceId.ts와 동일)
static bool isValidId(const String &s)
{
    if (s.length() != 16) return false;
    if (s[0] != 'v' || s[1] != 't' || s[2] != '-') return false;
    for (int i = 3; i < 7; i++)  if (!isDigit(s[i])) return false;
    if (s[7] != '-') return false;
    for (int i = 8; i < 12; i++) if (!isDigit(s[i])) return false;
    if (s[12] != '-') return false;
    for (int i = 13; i < 16; i++) {
        char c = s[i];
        if (!(isDigit(c) || (c >= 'a' && c <= 'z'))) return false;  // [a-z0-9]
    }
    return true;
}

void begin()
{
    s_nvs.begin(NVS_NS, /*readOnly=*/false);
    s_deviceId = s_nvs.getString(KEY_DEVICE, "");
    s_mqttPw   = s_nvs.getString(KEY_MQTT_PW, "");

    // NVS 비어 있으면 컴파일 기본값으로 시드 (기존 동작 유지).
    if (s_deviceId.isEmpty()) {
        s_deviceId = DEVICE_ID;
        s_nvs.putString(KEY_DEVICE, s_deviceId);
    }
}

bool hasValidId()               { return isValidId(s_deviceId); }
const String &deviceId()        { return s_deviceId; }
const String &mqttPassword()    { return s_mqttPw; }

void printHelp(Stream &io)
{
    io.println("[PROV] 명령: setid vt-YYMM-NNNN-XXX | setpw <pw> | showid | clearid | help");
}

static void printState(Stream &io)
{
    io.printf("[PROV] device_id=%s (%s), mqtt_pw=%s\n",
              s_deviceId.c_str(), isValidId(s_deviceId) ? "valid" : "INVALID",
              s_mqttPw.isEmpty() ? "(none)" : "(set)");
}

static void processLine(const String &raw, Stream &io)
{
    String line = raw; line.trim();
    if (line.isEmpty()) return;

    int sp = line.indexOf(' ');
    String cmd = (sp < 0) ? line : line.substring(0, sp);
    String arg = (sp < 0) ? ""   : line.substring(sp + 1);
    arg.trim();

    if (cmd == "setid") {
        if (!isValidId(arg)) {
            io.println("[PROV] 거부: 형식은 vt-YYMM-NNNN-XXX (예: vt-2607-0001-x7q)");
            return;
        }
        s_deviceId = arg;
        s_nvs.putString(KEY_DEVICE, s_deviceId);
        io.printf("[PROV] device_id 저장됨: %s — 적용하려면 재부팅(리셋)\n", s_deviceId.c_str());
    } else if (cmd == "setpw") {
        s_mqttPw = arg;
        s_nvs.putString(KEY_MQTT_PW, s_mqttPw);
        io.println("[PROV] mqtt_pw 저장됨 — 적용하려면 재부팅(리셋)");
    } else if (cmd == "showid") {
        printState(io);
    } else if (cmd == "clearid") {
        s_nvs.remove(KEY_DEVICE);
        s_nvs.remove(KEY_MQTT_PW);
        s_deviceId = DEVICE_ID;      // 기본값 복귀
        s_mqttPw = "";
        s_nvs.putString(KEY_DEVICE, s_deviceId);
        io.printf("[PROV] 초기화 → 기본값 %s. 재부팅 권장\n", s_deviceId.c_str());
    } else if (cmd == "help") {
        printHelp(io);
    } else {
        io.printf("[PROV] 알 수 없는 명령: '%s'\n", cmd.c_str());
        printHelp(io);
    }
}

void handleSerial(Stream &io)
{
    // 논블로킹: 들어온 문자를 개행까지 누적 후 처리. send_on_enter 유무 무관하게 동작.
    while (io.available()) {
        char c = (char)io.read();
        if (c == '\r') continue;
        if (c == '\n') {
            processLine(s_line, io);
            s_line = "";
        } else if (s_line.length() < 80) {
            s_line += c;
        }
    }
}

} // namespace Prov
