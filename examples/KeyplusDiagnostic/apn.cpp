/**
 * @file      apn.cpp
 * @brief     통신사 자동 판별 구현 (NVS "apn").
 */
#include "apn.h"
#include "config.h"
#include "log.h"
#include <Preferences.h>

namespace Apn {

static const char *kNs = "apn";

// IMSI 앞 5자리(MCC+MNC) → APN. 한국 MCC=450.
// ⚠️ 45006(LG U+)만 실기 검증됐다. 45005/45008 은 소매 LTE 표준값이라 M2M 요금제에서는
//    다를 수 있다 — 틀려도 PDP 폴백(망 할당 APN → 후보 순회)이 받아낸다.
// ⚠️ 레거시/부가 MNC(45002·45003·45004 등)는 일부러 넣지 않았다. 확신 없는 코드를 표에
//    넣으면 "잘못된 확신"으로 폴백을 건너뛰게 된다. 표에 없으면 순회로 흘러가 어차피
//    3후보를 다 시도하므로, 모르는 건 비워두는 쪽이 안전하다.
struct Entry {
    const char *mnc;    // IMSI 앞 5자리
    const char *apn;
    const char *name;   // 로그 표시용
};
static const Entry kTable[] = {
    { LTE_MNC_LGU, LTE_APN,     "LG U+" },
    { LTE_MNC_SKT, LTE_APN_SKT, "SKT"   },
    { LTE_MNC_KT,  LTE_APN_KT,  "KT"    },
};
static const int kCount = (int)(sizeof(kTable) / sizeof(kTable[0]));

enum Source : uint8_t {
    SRC_DEFAULT,   // config.h 고정값(자동 판별 off)
    SRC_MANUAL,    // 콘솔 'apn set'
    SRC_CACHED,    // NVS — 같은 유심의 지난번 성공값
    SRC_IMSI,      // IMSI(MCC+MNC) 표 적중
    SRC_PROBE,     // 표에 없는 MNC — 후보 회전 중
    SRC_NETWORK,   // 망이 내려준 APN(CGDCONT)으로 붙음
    SRC_SWEEP,     // 후보 순회 중 붙음
};

static char    s_apn[LTE_APN_MAX_LEN] = LTE_APN;
static Source  s_src     = SRC_DEFAULT;
static uint8_t s_rotate  = 0;       // 후보 회전 인덱스(RAM 전용 — 재부팅하면 0)
static bool    s_manual  = false;
static String  s_savedApn;          // NVS 캐시(마지막 성공값)
static String  s_savedIccid;
static String  s_imsi;              // 마지막 select 에서 읽은 값(콘솔 표시용)
static String  s_iccid;

static void setCurrent(const char *apn, Source src)
{
    snprintf(s_apn, sizeof(s_apn), "%s", apn ? apn : "");
    s_src = src;
}

const char *current()   { return s_apn; }
int         candidateCount()      { return kCount; }
const char *candidate(int i)      { return (i >= 0 && i < kCount) ? kTable[i].apn : ""; }

const char *sourceStr()
{
    switch (s_src) {
    case SRC_MANUAL:  return "수동 고정";
    case SRC_CACHED:  return "NVS 캐시";
    case SRC_IMSI:    return "IMSI 판별";
    case SRC_PROBE:   return "후보 시도";
    case SRC_NETWORK: return "망 할당";
    case SRC_SWEEP:   return "후보 순회 성공";
    default:          return "기본값";
    }
}

// 앞 5자리가 숫자인 IMSI 인지. 모뎀이 URC 를 섞어 뱉으면 엉뚱한 문자열이 온다.
static bool imsiUsable(const String &imsi)
{
    if (imsi.length() < 5) return false;
    for (int i = 0; i < 5; ++i) {
        if (!isdigit((unsigned char)imsi[i])) return false;
    }
    return true;
}

static const Entry *lookup(const String &imsi)
{
    if (!imsiUsable(imsi)) return nullptr;
    String mnc = imsi.substring(0, 5);
    for (int i = 0; i < kCount; ++i) {
        if (mnc == kTable[i].mnc) return &kTable[i];
    }
    return nullptr;
}

static void save(const String &apn, const String &iccid, bool manual)
{
    Preferences p;
    p.begin(kNs, /*readOnly=*/false);
    p.putString("apn", apn);
    p.putString("iccid", iccid);
    p.putUChar("manual", manual ? 1 : 0);
    p.end();
    s_savedApn   = apn;
    s_savedIccid = iccid;
    s_manual     = manual;
}

void begin()
{
    Preferences p;
    p.begin(kNs, /*readOnly=*/true);
    s_savedApn   = p.getString("apn", "");
    s_savedIccid = p.getString("iccid", "");
    s_manual     = p.getUChar("manual", 0) != 0;
    p.end();

    // 부팅 직후엔 모뎀이 아직 안 떴을 수 있다 — 여기서는 표시용으로만 반영하고,
    // 실제 결정은 select() 가 SIM 을 읽은 뒤에 한다.
    if (s_savedApn.length()) setCurrent(s_savedApn.c_str(), s_manual ? SRC_MANUAL : SRC_CACHED);
    else                     setCurrent(LTE_APN, SRC_DEFAULT);
}

const char *select(TinyGsm &modem, Stream &log)
{
#if !LTE_APN_AUTO
    // 자동 판별 off — 예전 동작 그대로(config.h 고정값).
    setCurrent(LTE_APN, SRC_DEFAULT);
    return s_apn;
#else
    // ① 사람이 박은 값이 있으면 아무것도 묻지 않는다.
    if (s_manual && s_savedApn.length()) {
        setCurrent(s_savedApn.c_str(), SRC_MANUAL);
        LOGI(log, "[APN] %s (수동 고정 — 'apn clear' 로 해제)\n", s_apn);
        return s_apn;
    }

    s_iccid = modem.getSimCCID(); s_iccid.trim();
    s_imsi  = modem.getIMSI();    s_imsi.trim();

    // ② 같은 유심이면 지난번 성공값을 그대로. ICCID 를 못 읽었을 때(AT 실패)도 캐시를
    //    쓴다 — 유심이 바뀌었다는 증거가 없는데 검증된 값을 버릴 이유가 없다.
    if (s_savedApn.length()) {
        if (s_iccid.isEmpty() || s_iccid == s_savedIccid) {
            setCurrent(s_savedApn.c_str(), SRC_CACHED);
            LOGI(log, "[APN] %s (지난번 성공값)\n", s_apn);
            return s_apn;
        }
        // ICCID 가 다르다 = 유심 교체. 캐시는 여기서 버리지 않고 무시만 한다 —
        // 새 유심으로 PDP 가 붙는 순간 commit() 이 덮어쓴다(중간에 실패해도 손해 없음).
        LOGI(log, "[APN] 유심 교체 감지(ICCID 변경) — 통신사 재판별\n");
    }

    // ③ IMSI(MCC+MNC) 표.
    const Entry *e = lookup(s_imsi);
    if (e) {
        setCurrent(e->apn, SRC_IMSI);
        LOGI(log, "[APN] %s (IMSI %s → %s)\n", s_apn, e->mnc, e->name);
        return s_apn;
    }

    // ④ 모르는 MNC — 후보를 하나씩 돌려본다. 실패해도 다음 브링업이 다음 후보로 시작한다.
    const Entry &c = kTable[s_rotate % kCount];
    setCurrent(c.apn, SRC_PROBE);
    LOGW(log, "[APN] IMSI '%s' 미등록 MNC — 후보 %d/%d 시도: %s(%s)\n",
         s_imsi.length() ? s_imsi.c_str() : "(조회 실패)",
         (int)(s_rotate % kCount) + 1, kCount, s_apn, c.name);
    return s_apn;
#endif
}

void noteBringupFailed()
{
    // 통신사를 못 가린 상태에서만 회전한다. 캐시/IMSI/수동으로 정해진 값은 브링업 실패의
    // 원인이 APN 이 아닐 가능성이 훨씬 크므로(신호·전원·모뎀) 건드리지 않는다.
    if (s_src == SRC_PROBE) s_rotate++;
}

void commit(const char *apn, Stream &log)
{
#if !LTE_APN_AUTO
    (void)apn; (void)log;                     // 자동 판별 off — 저장할 것도 없다
    return;
#else
    // 빈 APN(망 할당)으로 붙은 경우는 저장하지 않는다 — 저장값이 비면 "캐시 없음"과
    // 구분되지 않는다. 매 브링업 순회를 감수하되, 콘솔 'apn set' 안내가 로그에 남는다.
    if (!apn || !*apn) return;
    if (s_manual) return;                     // 사람이 정한 값이 최우선

    // ICCID 를 못 읽은 부팅(AT 실패)에서 기존 바인딩을 지우지 않는다 — 빈 값으로 덮으면
    // 다음 부팅마다 "유심 교체"로 오판해 재판별을 반복한다.
    String ic = s_iccid.length() ? s_iccid : s_savedIccid;

    if (s_savedApn == apn && s_savedIccid == ic) return;   // 변화 없음 → 플래시 안 씀

    save(String(apn), ic, /*manual=*/false);
    LOGI(log, "[APN] %s 저장됨 — 다음 부팅부터 바로 사용\n", apn);
#endif
}

String networkApn(TinyGsm &modem)
{
    String res;
    modem.sendAT("+CGDCONT?");
    if (modem.waitResponse(3000L, res) != 1) return "";

    // +CGDCONT: 1,"IP","internet.lguplus.co.kr","0.0.0.0",0,0
    int i = res.indexOf("+CGDCONT: 1,");
    if (i < 0) return "";
    int q1 = res.indexOf('"', i);          // "IP"
    int q2 = (q1 < 0) ? -1 : res.indexOf('"', q1 + 1);
    int q3 = (q2 < 0) ? -1 : res.indexOf('"', q2 + 1);   // APN 시작
    int q4 = (q3 < 0) ? -1 : res.indexOf('"', q3 + 1);
    if (q3 < 0 || q4 <= q3 + 1) return "";               // 빈 APN("")도 여기서 걸러진다

    String apn = res.substring(q3 + 1, q4);
    apn.trim();
    return apn;
}

// ---------------------------------------------------------------------------
// 콘솔 'apn' — 재플래싱 없이 현장에서 APN 을 확정하기 위한 창구다.
// (USB 로 붙어 'apn set <값>' 후 재부팅 = OTA 도 안 되는 상황의 최종 복구 수단)
// ---------------------------------------------------------------------------
void console(const String &arg, TinyGsm &modem, Stream &io)
{
    String a = arg;
    a.trim();

    if (a.startsWith("set ")) {
        String v = a.substring(4);
        v.trim();
        if (v.isEmpty() || v.length() >= LTE_APN_MAX_LEN) {
            io.printf("[APN] 값이 비었거나 너무 김(최대 %d자)\n", LTE_APN_MAX_LEN - 1);
            return;
        }
        save(v, s_iccid, /*manual=*/true);
        setCurrent(v.c_str(), SRC_MANUAL);
        io.printf("[APN] '%s' 수동 고정 — **재부팅해야 적용**된다('apn clear' 로 해제)\n",
                  v.c_str());
        return;
    }

    if (a == "clear") {
        save("", "", /*manual=*/false);
        s_rotate = 0;
        setCurrent(LTE_APN, SRC_DEFAULT);
        io.println("[APN] 저장값 삭제 — 재부팅하면 통신사를 다시 판별한다");
        return;
    }

    if (!a.isEmpty()) {
        io.println("[APN] 사용법: apn | apn set <apn> | apn clear");
        return;
    }

    io.println("[APN]");
    io.printf("  %-14s: %s (%s)\n", "현재", s_apn, sourceStr());
    io.printf("  %-14s: %s\n", "IMSI", s_imsi.length() ? s_imsi.c_str() : "(미조회)");
    io.printf("  %-14s: %s\n", "ICCID", s_iccid.length() ? s_iccid.c_str() : "(미조회)");
    io.printf("  %-14s: %s%s\n", "NVS 저장값",
              s_savedApn.length() ? s_savedApn.c_str() : "(없음)",
              s_manual ? " [수동 고정]" : "");
    String net = networkApn(modem);
    io.printf("  %-14s: %s\n", "망 할당(CGDCONT)", net.length() ? net.c_str() : "(없음)");
    io.print("  후보          : ");
    for (int i = 0; i < kCount; ++i) {
        io.printf("%s%s(%s)", i ? ", " : "", kTable[i].apn, kTable[i].name);
    }
    io.println();
    io.println("  사용법: apn set <apn>   (NVS 영구 고정, 재부팅 후 적용) / apn clear");
}

} // namespace Apn
