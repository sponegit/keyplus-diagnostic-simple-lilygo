/**
 * @file      config.h
 * @brief     Keyplus 진단기 프로젝트 설정 — 하드웨어 베이스라인(0단계 확정) + 기능 토글
 *
 * 보드: LilyGO T-A7670E (바레, 확장보드 없음) / ESP32-WROVER, Flash 4MB, PSRAM 8MB
 * 모뎀: A7670E (LTE Cat-1) + 내장 GNSS
 *
 * 핀맵 원본: keyplus-docs/.../260706-simple-obd2-diagnostic/hardware-baseline.md
 * 모뎀/SD/GPS 핀은 보드 정의(utilities.h, -DLILYGO_T_A7670)가 담당하므로 여기서 재정의하지 않는다.
 * 아래는 "프로젝트가 추가로 점유하는" 핀(차키·OBD2)과 기능 설정만 확정 기록한다.
 */
#pragma once

// ===========================================================================
// 단계별 기능 토글 (2트랙 진행: 유심 도착 전/후)
//   현재 = 2단계 GPS. 유심 불필요 단계만 활성화한다.
// ===========================================================================
#define FEATURE_GPS         1   // 2단계: 내장 GNSS 위치 조회       [활성]
#define FEATURE_OBD2        1   // 6단계: CAN/OBD2 PID (외장 트랜시버 SN65HVD230, GPIO21/22) [활성]
#define FEATURE_CARKEY      1   // 5b단계: 차키 GPIO 토글 (열림/잠금만, 전원 제어 없음) [활성]
#define FEATURE_BLE         0   // 9단계: BLE 명령/OTA
#define FEATURE_LTE         1   // 1단계: LTE+MQTT (LG U+ 유심)     [활성 — 증분 A]
#define FEATURE_STATUS_LED  1   // 3단계: 상태표시 LED (외장, GPIO23) [활성]
#define FEATURE_OTA         1   // 8단계: LTE OTA(ota_start) + config_update [활성]
#define FEATURE_OFFLINE_BUF 1   // 오프라인 telemetry 적재·백필 (spiffs 파티션 raw 링) [활성]
#define FEATURE_FAST_SAMPLE 1   // 고빈도(1Hz) OBD 샘플 스트림 [활성 — 런타임 fast_ms=0 로 차단 가능]

// ===========================================================================
// 소비전류 계측 모드 (PWR_BENCH) — 차량 배터리 유지시간 최적화 작업용
//   실측 150~200mA. 차량 암전류 예산(장치 몫 10~20mA)의 10배라 어디서 나가는지
//   먼저 분해해야 한다. 기능을 하나씩 빼는 대신 **바닥부터 쌓아** 두 지점만 잰다.
//
//     0 = 정상 동작 (출고 기본 — 절대 0 이 아닌 채로 릴리스하지 말 것)
//     1 = idle : GPS/OBD2/LED/버퍼/고빈도 전부 OFF + LTE 도 OFF.
//                모뎀은 전원인가·미등록 상태로 남는다(PWRKEY 는 눌린다).
//                → ESP32(240MHz 상시 활성) + 보드 정지전류 + 모뎀 유휴
//     2 = mqtt : 1 에 LTE 등록 + MQTT 상시연결 + 30초 telemetry 발행만 추가.
//                = "차키 명령을 즉시 받는다"는 요구를 만족하는 **최소 구성**.
//     3 = floor: 1 에서 PWRKEY 펄스까지 생략 → 모뎀 OFF. ESP32+보드만의 바닥.
//     4 = mqtt+gps : 2 에 **GNSS 만** 추가. (4 − 2) = GNSS 상시 ON 비용.
//     5 = mqtt+obd : 2 에 **OBD2/CAN 만** 추가. (5 − 2) = CAN 트랜시버+TWAI 비용.
//
//   해석: (2 − 1) = LTE 등록·MQTT 상시연결 유지비, (1 − 3) = 모뎀 유휴 비용,
//         (3) = ESP32/보드 바닥. 이 셋으로 어디를 손볼지가 결정된다.
//   4·5 는 GPS·OBD2 를 **따로** 재려고 나중에 추가했다. 둘을 한꺼번에(0 − 2) 재면
//   합계만 알 뿐 어느 쪽을 손봐야 할지 모른다 — GNSS duty cycle 의 값어치를 판단하려면
//   GNSS 몫만 따로 필요하다. 검산: (4 − 2) + (5 − 2) ≈ (0 − 2).
//   ⚠️ 차키(FEATURE_CARKEY)는 모든 모드에서 켜 둔다 — GPIO 출력 LOW 유지라
//      소비가 사실상 0 이고, 최적화 후에도 남아야 하는 유일한 필수 기능이다.
//   ⚠️ 계측 중에도 발행은 살아 있다(모드 2). MQTT 유지 자체가 캐리어 NAT(60초)
//      때문에 30초 발행을 요구하므로, 발행을 뺀 수치는 현실의 유지비가 아니다.
// ===========================================================================
#define PWR_BENCH           0

#if PWR_BENCH
  #undef  FEATURE_FAST_SAMPLE
  #define FEATURE_FAST_SAMPLE 0
  #undef  FEATURE_STATUS_LED
  #define FEATURE_STATUS_LED  0
  #undef  FEATURE_OFFLINE_BUF
  #define FEATURE_OFFLINE_BUF 0

  // GNSS 는 모드 4 에서만 켠다.
  #undef  FEATURE_GPS
  #if (PWR_BENCH == 4)
    #define FEATURE_GPS       1
  #else
    #define FEATURE_GPS       0
  #endif

  // OBD2/CAN 은 모드 5 에서만 켠다.
  #undef  FEATURE_OBD2
  #if (PWR_BENCH == 5)
    #define FEATURE_OBD2      1
  #else
    #define FEATURE_OBD2      0
  #endif

  // LTE 는 2·4·5 에서만 켠다(1·3 은 모뎀 유휴/바닥 측정이라 꺼야 한다).
  #if (PWR_BENCH == 1 || PWR_BENCH == 3)
    #undef  FEATURE_LTE
    #define FEATURE_LTE       0
    #undef  FEATURE_OTA
    #define FEATURE_OTA       0
  #endif
#endif

// ===========================================================================
// 모뎀 브링업 (setup) — 무응답/미식별로 부팅이 멈추지 않게 하는 상한
//   ⚠️ 실측(0.2.6, 유심 미삽입 + 업로드 직후 첫 부팅): AT 는 뜨는데 그 뒤 ATI 가 계속
//      무응답이었다(로그: `+CPIN: SIM REMOVED` → `+CME ERROR: AT command timeout` →
//      `MODEM STRING NO FOUND!` 12초마다 무한). 모델명 조회가 무한 재시도라 setup 이
//      영영 끝나지 않았고, 콘솔·OBD2·차키까지 같이 죽었다.
//      → 아래 상한만큼만 시도하고, 하드 리셋 1회로 회복을 노린 뒤, 그래도 안 되면
//        모뎀 없이 부팅을 계속한다(모뎀은 loop 의 재브링업/리셋 경로가 계속 다룬다).
// ===========================================================================
// ===========================================================================
// 모뎀 유휴 슬립 (AT+CSCLK) — 설계·위험은 modempwr.h 참고
//   실측(2026-08-10 PPK2 5V): 모뎀이 망에 붙어 아무것도 안 하는 동안 47mA.
//   mqtt 상태 120mA 의 40% 다. 같은 측정에서 **30초 주기 발행이 얹는 비용은 10mA 뿐**
//   이었으므로(mqtt 120 − idle 110), 이 47mA 는 발행과 무관한 모듈 자체 상시 소비다.
//   → 주기 발행은 슬립의 장애물이 아니다. 겨냥할 것은 이 baseline 이다.
//   ⚠️ 합격 조건은 소비전류가 아니라 **재우는 동안에도 차키 명령이 제때 도착하는가** 다.
//      런타임 'msleep on|off' 로 재플래싱 없이 A/B 비교할 수 있다.
// ===========================================================================
// ⚠️ 진행 상황(2026-08-11): **1차 판정은 무효였다. 재측정 대기 중.**
//    1차(2026-08-10)에서 "효과 없음"으로 결론냈으나 **그 측정 조건이 틀렸다** —
//    전체 펌웨어 + CPU 240MHz 였다(로그 260810_002_fw: `기능: GPS OBD2 CARKEY LTE LED OTA`,
//    PWR_BENCH/cpu 배너 없음). 총 150~200mA 배경 위에서 20~40mA 짜리 변화를 찾은 셈인데,
//    그 배경 자체가 GNSS 측위 탐색과 LTE 송신 전력 때문에 ±30mA 씩 흔들렸다.
//    → 효과가 있었더라도 보이지 않았을 조건이다. 판정을 다시 한다.
//    재측정 조건: PWR_BENCH=2(mqtt, 총 85~105mA) + CPU 80MHz + quiet-idle.
//    이 조건의 모뎀 유휴 몫이 47mA 라 변화가 배경에 묻히지 않는다. 그리고 판정은
//    반드시 **같은 실행 안의 'msleep on/off' A/B** 로 한다 — 실행 간 비교는 LTE 전파
//    상태 때문에 ±20mA 가 흔들려 이 크기의 결론을 낼 수 없다는 걸 이미 여러 번 겪었다.
//
//    전류와 무관하게 이미 확정된 사실:
//      mode 1(DTR 슬립)은 **이 보드에서 걸리지 않는다.** 'msleep probe' 가 "재운뒤
//      AT=응답"을 냈는데, A7670 HW 매뉴얼 3.3.2 는 "CSCLK=1 에서 DTR 을 올리면
//      串口功能不能正常通讯"(UART 통신 불가)이라고 명시한다 → GPIO25 가 모듈 DTR 에 안 닿았다.
//      RI(GPIO33)도 마찬가지였다(670초간 엣지 0회).
//      → 기본값은 배선이 필요 없는 mode 2(RX 슬립)로 둔다.
//
// ⚠️ 최종 판정(2026-08-11 재측정, 로그 260811_004_batt): **효과 없음 → 껐다.**
//    이번엔 조건이 깨끗했다 — PWR_BENCH=2 / CPU 80MHz / 총 60mA 배경 / **같은 실행 안의 A/B**.
//      msleep on  = 60mA
//      msleep off = 58mA        ← 차이 없음(오히려 off 가 낮다 = 측정 노이즈 범위)
//      msleep probe = 깨어있을때 4ms / 재운뒤 4ms  ← 기상 지연이 없다 = **애초에 안 잔다**
//    수면비율 93% 는 우리 장부상 수치일 뿐 모듈은 실제로 자지 않았다.
//    AT 매뉴얼이 이 명령을 "Control **UART** Sleep"으로 규정한 그대로다.
//
// ⚠️ 게다가 **해롭다는 정황**이 있다. 같은 로그에서 'msleep on' 1분 뒤 세션이 끊겼다
//    (16:29:10 on → 16:30:11 연결 끊김 → 재접속에 76초). RX 슬립 기상은 UART 에
//    "AT\r\n" 을 쏘고 응답을 비우는데(modempwr.cpp wakeByRx), 그 순간 도착한 URC 를
//    잠식하면 세션이 어긋난다 — 설계 당시 적어둔 위험이 실제로 관측된 셈이다.
//    이득이 0 인데 위험만 있으므로 끈다.
//
//    코드는 남긴다: 'msleep probe'(기상 지연 측정)는 계측 도구로 쓸모가 있고,
//    DTR/RTS 배선이 확보되면 mode 1 로 되살릴 여지가 있다.
#undef  FEATURE_MODEM_SLEEP
#define FEATURE_MODEM_SLEEP         0
#if (FEATURE_MODEM_SLEEP && !FEATURE_LTE)
  // 슬립 적용(AT+CSCLK=1)은 LTE 브링업 성공 뒤에 건다 — LTE 가 없으면 걸 자리가 없다.
  #undef  FEATURE_MODEM_SLEEP
  #define FEATURE_MODEM_SLEEP       0
#endif
// --- 슬립 방식 (AT+CSCLK=<status>) ------------------------------------------
//   1 = DTR 슬립 : DTR 을 HIGH 로 올리면 잔다. **모듈 DTR 핀 배선이 필요하다.**
//   2 = RX 슬립  : 시리얼로 아무 데이터나 보내면 깨어난다. **핀이 필요 없다.**
// ⚠️ 실측(2026-08-10): 이 보드에서 모드 1 이 동작하지 않았다. CSCLK=1 은 OK 를 받는데
//    DTR 을 올려도 UART 가 계속 살아 있었다('msleep probe' → "재운뒤 AT=응답").
//    A7670 하드웨어 매뉴얼 3.3.2 는 "CSCLK=1 에서 DTR 을 올리면 串口功能不能正常通讯"
//    (UART 통신 불가)이라고 명시하므로, 안 죽었다는 건 **GPIO25 가 모듈 DTR 에 닿지
//    않았다**는 뜻이다. → 기본값을 배선이 필요 없는 2 로 둔다.
//    (배선이 확인되면 1 이 더 안전하다 — 모드 2 는 기상용 바이트를 UART 에 쏘므로
//     그 순간 도착한 URC 를 흘릴 여지가 있다. modempwr.cpp wakeByRx 주석 참고)
#define MODEM_SLEEP_MODE            (2)
// 깨운 뒤 모뎀 UART 가 살아날 때까지의 대기. A76xx 문서 최소 50ms + 마진.
// ⚠️ 줄이면 깨우자마자 보낸 AT 가 사라져 "모뎀 무응답" 오판 → 리셋으로 이어진다.
#define MODEM_WAKE_SETTLE_MS        (60)
// 자는 동안 AT 프로브(5초)·전원표본(10초)을 거른다. 이 둘이 유휴 창을 잘게 쪼개서
// 슬립이 성립할 시간 자체를 없앤다 — 실측 118회 깨움 = 평균 4.1초 수면이었다.
// ⚠️ 대가: 죽은 모뎀 발견이 5초 → 최대 (발행주기 + PUB_ACK 타임아웃) 로 늦어진다.
//    30초 발행 + 8초면 최악 38초. 발행 ACK 가 이미 생존 신호라 프로브는 중복이다.
#define MODEM_SLEEP_QUIET_IDLE      (1)
// 마지막 AT 이후 이만큼 조용하면 재운다. 접속·발행처럼 AT 가 연달아 나가는 구간에서
// 매번 재웠다 깨우는(=60ms 씩 까먹는) 일이 없도록 하는 값이다.
// ⚠️ 너무 키우면 프로브(5초)·전원표본(10초) 사이의 짧은 유휴를 통째로 놓친다.
#define MODEM_SLEEP_IDLE_MS         (500UL)

// ===========================================================================
// ESP32 CPU 클럭 낮추기 — floor 63mA 공략 1단계
//   실측(2026-08-10 PPK2 5V) floor(3) = 63mA. 모뎀을 아예 안 켠 상태라 이 값은
//   ESP32 + 보드 정지전류가 전부이고, ESP32 는 240MHz 로 한 번도 자지 않고 돈다.
//   ESP32 는 클럭에 거의 비례해 먹으므로 240 → 80MHz 만으로 20mA 안팎이 빠진다.
//
//   ⚠️ 80MHz 아래로 내리지 말 것. ESP32 는 CPU 가 80MHz **이상**일 때 APB 가 80MHz
//      로 고정되는데, 그 아래로 내리면 APB 가 CPU 를 따라 내려가 **UART 보레이트가
//      전부 어긋난다**(모뎀 통신·콘솔이 통째로 깨진다). 80 이 안전한 바닥이다.
//   ⚠️ 이건 '재우기'가 아니라 '천천히 돌리기'다. 진짜 절감은 light sleep 이지만,
//      그건 모뎀 URC 수신(+CMQTTRX*)을 놓치지 않을 기상 경로가 먼저 필요하다
//      — RI 핀(GPIO33) 배선 확인이 그 전제다. 위험 대비 이득이 좋은 이 단계를 먼저 친다.
// ===========================================================================
#define FEATURE_CPU_SLOWDOWN        1
#define CPU_FREQ_MHZ                (80)

// --- light sleep 기상 경로 진단 (계측 전용) ---------------------------------
// floor 27mA 를 더 줄이려면 light sleep(0.8mA 급) 뿐인데, 자는 동안 APB 가 꺼져
// UART 바이트가 유실된다 — 차키 명령이 그 경로라 기상 경로부터 확보해야 한다.
// 후보 둘(RI 핀 GPIO33 / UART 엣지)을 **하드웨어 작업 없이** 판정한다. wake.h 참고.
// ⚠️ 판정 완료(2026-08-11) → **0 으로 내렸다.**
//    RI(GPIO33)는 670초 동안 엣지 0회(그 사이 URC 20회 이상) → **미배선 확정.**
//    `wake sleep 20` 도 TIMER 기상 + UART 대기 바이트 0 이었고, 자는 동안 차키 명령이
//    도착하지 않았다. 배선 없이는 light sleep 을 쓸 수 없다는 결론이라 진단 코드를 끈다.
//    (RTS/CTS 배선을 확인하게 되면 다시 1 로 올려 같은 방식으로 판정할 수 있다)
#define FEATURE_WAKE_PROBE          0
#if (FEATURE_CPU_SLOWDOWN && CPU_FREQ_MHZ < 80)
  #error "CPU_FREQ_MHZ 는 80 미만이면 APB 가 따라 내려가 UART 가 깨진다(위 주석 참고)"
#endif

#define MODEM_AT_READY_TIMEOUT_MS   (90000UL)  // AT 준비 대기 상한(넘으면 모뎀 없이 진행)
#define MODEM_NAME_RETRY            (3)        // 모델명(ATI) 조회 재시도 횟수(리셋 전/후 각각)
#define MODEM_GPS_ENABLE_RETRY      (3)        // GNSS 활성화(+CGNSSPWR) 재시도 횟수

// ===========================================================================
// GPS 설정 (2단계)
// ===========================================================================
// 주행 중 위치 폴링 주기. telemetry 발행 직전에도 별도로 1회 갱신하므로(loop 참고)
// 이 주기는 "발행 사이에 얼마나 촘촘히 궤적을 갱신할지"를 정한다.
#define GPS_POLL_INTERVAL_MS        (10000UL)
// 정차/주차 중(OBD 링크 없음 또는 rpm·차속 0) 폴링 주기. 좌표가 변하지 않는데
// 폴링만 하면 모뎀 AT 왕복과 전력을 낭비한다. 주차 중 배터리 방전 방지.
#define GPS_POLL_IDLE_MS            (60000UL)
// 첫 fix 실패 시 재시도 간격 (콜드스타트는 수십 초~분 소요될 수 있음)
#define GPS_RETRY_INTERVAL_MS       (5000UL)

// ===========================================================================
// GNSS duty cycle (주차 중 전원 껐다 켜기) — 소비전류 최대 항목 공략
//   실측(2026-08-11 PPK2 5V): mqtt+gps(4) 150mA − mqtt(2) 85mA = **GNSS 65mA**.
//   시스템 전체에서 가장 큰 단일 항목이다.
//   ⚠️ **폴링 주기를 늘리는 건 대책이 아니다.** GNSS 엔진은 부팅 때 켠 뒤 한 번도
//      꺼지지 않으므로, 읽는 주기를 10초→60초로 늘려도 수신기는 계속 돌며 65mA 를 먹는다.
//      줄이는 유일한 방법은 AT+CGNSSPWR=0 으로 **엔진을 끄는 것**이다.
//   주차 중엔 좌표가 변하지 않으므로 켜 둘 이유가 없다. 주기적으로 잠깐 켜서 측위하고 끈다.
//
//   ⚠️ 서버는 아직 좌표를 쓰지 않는다(2026-08-11 확인) → 위치 신선도 제약이 없다.
//      주기는 순전히 전력과 재측위 시간(TTFF)만 보고 정하면 된다.
// ===========================================================================
#define FEATURE_GPS_DUTY            1
// 주차 중 한 사이클 주기. duty = FIX_TIMEOUT / CYCLE 이므로 5분·30초면 10% → 약 −58mA.
// 0 으로 두면 **주차 중 완전 OFF**(주행 재개 전까지 다시 안 켠다).
#define GPS_PARKED_CYCLE_MS         (300000UL)   // 5분
// 측위 실패가 이어질 때의 주기 상한(지수 백오프). 지하주차장·실내처럼 **아무리 켜도
// 측위가 안 되는 곳**에서 5분마다 90초씩 켜면 duty 30% → 약 13mA 를 아무 소득 없이 태운다.
// 실패마다 주기를 2배로 늘려 그 낭비를 접고, 한 번 성공하면 즉시 기본 주기로 돌아온다.
//   30분 주기 기준 낭비 = 90/1800 × 45mA ≈ 2mA.
#define GPS_PARKED_CYCLE_MAX_MS     (1800000UL)  // 30분
// 한 사이클에서 측위를 기다리는 상한. 넘으면 포기하고 끈다 —
// ⚠️ 이 상한이 없으면 실내·음영에서 영원히 못 잡고 계속 켜져 있어 duty cycle 이 무의미해진다.
#define GPS_PARKED_FIX_TIMEOUT_MS   (90000UL)
// 측위 성공 후에도 이만큼은 더 켜 둔다(좌표 안정화 + 시각 동기 여유). 0 이면 즉시 끈다.
#define GPS_PARKED_HOLD_MS          (2000UL)
// GNSS UART 속도 (모뎀 ↔ ESP32)
#define GPS_BAUDRATE                (115200)
// 연속 미측위 몇 회마다 GNSS 전원 상태(AT+CGNSSPWR?)를 확인할지.
// ⚠️ 모뎀이 리셋되면(소프트/하드) GNSS 가 꺼진 상태로 돌아간다 — Gps::begin()은 setup
//    에서 한 번뿐이라, 이 확인이 없으면 리셋 이후 영구 미측위가 된다.
#define GPS_ENSURE_AFTER_FAILS      (3)
// 미측위 진행 리포트 주기(INFO). 미측위는 로그가 완전히 침묵해서 폴이 도는지조차
// 알 수 없었다 — 경과 시간 + 원시 +CGNSSINFO 를 이 주기로 한 줄 남긴다.
#define GPS_NOFIX_REPORT_MS         (60000UL)
// AGPS(AT+CAGPS) — 망 보조 데이터로 콜드스타트 단축. LTE(PDP) up 이후 1회 시도.
// 실패해도 측위는 진행되므로 best-effort. 0 이면 시도하지 않는다.
#define GPS_USE_AGPS                (1)
// 모뎀 리셋 후 미뤄둔 GNSS 재활성화(R5)를 실행하려면 다음 실시간 발행까지 이만큼은
// 남아 있어야 한다. Gps::begin 은 enableGPS 를 최대 15초 폴링하므로, 발행 직전에
// 물리면 telemetry 가 그만큼 통째로 밀린다(백필/status 재발행 가드와 같은 규칙).
#define GPS_REENABLE_PUB_GUARD_MS   (16000UL)
// 미접속 구간에서 GNSS 재활성화를 미루는 시간.
// ⚠️ 미접속 = 재접속 창이다. 여기서 15초짜리 Gps::begin 을 물리면 그게 그대로 복구
//    지연이 된다 — 260804 로그2 실측: 09:57:16 AT 복구 → 09:57:23 GNSS(7초) →
//    09:57:27 재브링업. R5 가 리셋 자리에서 뺀 시간이 폴 자리에서 도로 들어왔다.
//    재접속에 먼저 기회를 준다.
// 그렇다고 영영 미룰 수는 없다 — 음영/권외로 오래 못 붙는 동안에도 오프라인 적재에는
//    위치가 필요하다. 정상 복구가 60초대에 끝나므로 그보다 넉넉히 뒤에 연다.
#define GPS_REENABLE_OFFLINE_DELAY_MS (90000UL)

// ===========================================================================
// LTE / MQTT 설정 (1단계)   설계: lte-mqtt-device-design.md
// ===========================================================================
// --- 증분 A: LTE 브링업 (서버 무관, 유심 데이터패스 검증) ---
// LG U+ 표준 동적IP APN. 단말은 outbound 접속이라 정적 공인IP APN 불필요.
// M2M 요금제 유심이면 개통 후 AT+CGDCONT? 로 실제 값 확인해 교체할 것.
// ⚠️ 이 값은 이제 "기본값 겸 1순위 후보"다 — 실제 사용 APN 은 apn.h 가 유심을 보고 정한다.
#define LTE_APN                     "internet.lguplus.co.kr"
#define LTE_GPRS_USER               ""
#define LTE_GPRS_PASS               ""

// --- 통신사 자동 판별 (apn.h) ----------------------------------------------
// 0 이면 예전 동작(LTE_APN 고정). 1 이면 유심을 보고 3사 중에서 고른다.
// 통신사가 갈리는 지점은 APN 하나뿐이다 — 망 등록은 유심의 홈망으로 자동으로 된다.
#define LTE_APN_AUTO                (1)
// KT/SKT 후보 APN.
// ⚠️ **실기 검증 안 됨**(KT/SKT 유심 미보유). 소매 LTE 표준값이라 M2M/IoT 요금제에서는
//    사업자가 지정한 별도 APN 일 수 있다. 틀려도 PDP 실패 후 폴백(망 할당 APN → 후보
//    순회)이 받아내고, 그래도 안 되면 콘솔 'apn set <값>' 으로 현장에서 박으면 된다.
#define LTE_APN_SKT                 "lte.sktelecom.com"
#define LTE_APN_KT                  "lte.ktfwing.com"
// IMSI 앞 5자리(MCC+MNC). 한국 MCC=450. 확신 있는 주력 코드만 적는다 — 표에 없으면
// 후보 순회로 흘러가 어차피 3개를 다 시도하므로, 틀린 코드를 넣는 것보다 안전하다.
#define LTE_MNC_LGU                 "45006"
#define LTE_MNC_SKT                 "45005"
#define LTE_MNC_KT                  "45008"
// APN 문자열 버퍼(3GPP 는 최대 100자지만 실사용은 30자 미만).
#define LTE_APN_MAX_LEN             (64)
// PDP 실패 후 후보 APN 순회에 쓸 총 예산(ms). 틀린 APN 은 대개 CGACT 에서 수 초 만에
// 떨어지지만, NETOPEN 이 최대 75초까지 물릴 수 있다 — 여기서 끊어야 재시도 백오프가
// 통째로 밀리지 않는다. 예산이 다하면 남은 후보는 다음 브링업에서 이어서 본다.
#define LTE_APN_SWEEP_BUDGET_MS     (90000UL)
// 망 등록 대기 상한 (콜드 등록은 수십 초 소요 가능)
#define LTE_REG_TIMEOUT_MS          (90000UL)
// 접속 기술 (AT+CNMP). 2=자동, 38=LTE only, 13=GSM only, 51=GSM+LTE.
// 0 이면 아예 건드리지 않는다.
//
// ⚠️ 한때 38(LTE 전용)로 고정했다가 **되돌렸다**. 전원 마진을 지키려고 GSM 의 2A 버스트를
//    없애는 게 목적이었는데, 260804 로그2 에서 부작용이 나왔다:
//      - 모뎀 전원 인가 → 사망까지 약 60초가 **5회 연속 정확히** 반복
//      - 같은 로그의 [PWR] 은 vbat 4270~4284mV(편차 14mV) · temp 30~32°C 로 멀쩡
//    전기적 붕괴는 부하 의존이라 이렇게 규칙적일 수 없다(38 적용 전에는 1분33초~12분으로
//    제각각이었다). 규칙적 주기는 전기 현상이 아니라 결정적 원인을 가리킨다.
//    → 원인 후보에서 지우기 위해 자동으로 복원한다.
//
// ⚠️ 0 이 아니라 2 다. 값이 모뎀 NV 에 남기 때문에 "건드리지 않음"으로 두면 38 이 그대로
//    살아남아 실험이 성립하지 않는다. 능동 복원이어야 펌웨어 교체만으로 되돌아간다.
//    (조회 가드가 있어 실제 쓰기는 복원 1회뿐이다.)
#define LTE_CNMP_MODE               (2)
// --- 모뎀 무응답 복구 -------------------------------------------------------
// 전원 마진이 부족하면 LTE 등록(최대 출력 송신) 중 전압이 내려앉아 모뎀이 내부 리셋되고,
// 그 뒤로 AT에 응답하지 않는다(CSQ=99, 등록상태 no-result). 이때 AT 재시도만 반복하면
// 회복 경로가 없어 영영 못 붙는다 → 아래 값으로 조기 감지 + 리셋 승격을 한다.
#define LTE_AT_PROBE_MS             (2000)      // 모뎀 생존 확인 AT 응답 대기
// SIM 인식 확인(+CPIN?) — 유심이 없으면 망 등록은 100% 실패다. 등록 폴링에
// LTE_REG_TIMEOUT_MS(90초)를 통째로 버리는 대신 여기서 끊고 원인을 정확히 알린다.
// 부팅 직후엔 CPIN 이 잠깐 "NOT READY"일 수 있어 몇 회는 봐준다.
#define LTE_SIM_PROBE_MS            (3000)      // +CPIN? 1회 대기 상한
#define LTE_SIM_PROBE_TRIES         (3)         // 재시도 횟수(1초 간격)
#define LTE_MODEM_RESET_WAIT_MS     (20000UL)   // 리셋 후 AT 재개 대기 상한
#define LTE_FAIL_BEFORE_RESET       (2)         // 연속 브링업 실패 몇 회에 모뎀 리셋
// 재브링업 간격 — 등록 시도는 모뎀이 최대 출력으로 송신하는 구간이라, 전원이 약하면
// 몰아칠수록 전압 강하가 심해진다. 실패마다 2배로 늘려 모뎀이 쉬는 시간을 준다.
#define LTE_RETRY_BASE_MS           (30000UL)
#define LTE_RETRY_CAP_MS            (300000UL)  // 5분
// 리셋으로 모뎀이 되살아난 직후의 재브링업 지연. 쌓인 백오프(실측 60초)를 그대로 쓰면
// 막 부팅한 깨끗한 모뎀을 그만큼 놀린다 — 부팅 URC(+CPIN: READY / +CGEV 등)가 정리될
// 시간만 주고 바로 붙는다. 유심 인식이 덜 됐으면 Lte::begin 의 SIM 재시도가 흡수한다.
#define LTE_POST_RESET_DELAY_MS     (5000UL)
// --- 모뎀 생존 프로브 (R1) --------------------------------------------------
// 세션 사망을 telemetry publish 실패로만 알던 구조라, 모뎀이 내부 리셋돼도 다음 발행
// 주기(30초)가 돌아와 mqtt_publish 가 '>' 프롬프트 대기(10초+10초)를 태우고 나서야
// mqtt=off 가 됐다 — 실측 55~95초. 그동안 LED 는 정상(접속됨)을 표시했다.
//   → 발행과 무관한 짧은 AT 프로브로 감지 지연을 프로브 주기 수준으로 끌어내린다.
// ⚠️ 프로브는 URC 가 스트림에 없을 때만 보낸다. testAT 의 waitResponse 가 대기 중인
//    +CMQTTRX*/+CMQTTPUB 를 응답으로 잘못 먹으면 수신 명령이 통째로 사라진다.
#define MODEM_PROBE_INTERVAL_MS     (5000UL)   // 프로브 주기
#define MODEM_PROBE_TIMEOUT_MS      (500)      // 1회 AT 응답 대기(생존 확인용 — 짧게)
#define MODEM_PROBE_FAILS_TO_DOWN   (2)        // 연속 무응답 몇 회에 세션 사망 판정
// 직전 발행 이후 이만큼 지나야 프로브한다 — 발행 ACK(+CMQTTPUB) URC 가 빠질 시간.
#define MODEM_PROBE_PUB_GUARD_MS    (2000UL)
// --- 전원·발열 계측 (P2) ----------------------------------------------------
// 260804 로그의 반복 모뎀 사망을 "과열"과 "전원 마진" 중 어느 쪽인지 가르기 위한 계측.
// V1.4 회로도 기준 두 값 모두 부품 추가 없이 읽힌다:
//   temp : AT+CPMUTEMP (모뎀 PMU 다이 온도)
//   vbat : IO35 = R7/R9 100K 1% 분압 + C57 10nF → VV_BAT(ORing 이후 모뎀 공급 레일)
// ⚠️ vbat 는 배터리 커넥터가 아니라 **모뎀이 실제로 먹는 레일**이다. 브라운아웃이
//    일어나는 바로 그 노드라 이 용도에 정확하다(ReadBattery 예제의 "USB 연결 시
//    부정확" 경고는 'LiPo 전압을 알고 싶을 때' 이야기라 여기엔 해당하지 않는다).
// ⚠️ R7∥R9(50K) × C57(10nF) = τ 500us 저역통과가 걸려 있다. 마이크로초 단위 순간
//    딥은 뭉개지고, 등록/발행 TX 구간의 지속적 sag 는 보인다.
#define FEATURE_PWR_MONITOR         (1)
#if (FEATURE_PWR_MONITOR && !FEATURE_LTE)
  // 온도는 모뎀 AT 조회다 — LTE 없이는 절반만 남아 의미가 없다. 조용히 끈다.
  #undef  FEATURE_PWR_MONITOR
  #define FEATURE_PWR_MONITOR       (0)
#endif
#define PWR_SAMPLE_INTERVAL_MS      (10000UL)  // 표본 주기(모뎀 AT 1왕복 + ADC)
#define PWR_LOG_INTERVAL_MS         (60000UL)  // [PWR] 한 줄 출력 주기
// ⚠️ 이 값들은 [STAT] 에 얹지 않는다. UART 진단 앱(main.py)의 STAT_KEYS 가 닫힌
//    목록이라 미등록 토큰이 앞 필드 값에 흡수돼 obd/seq 표시가 깨진다(printStatusLine
//    주석). 앱이 확장될 때까지 독립 태그 [PWR] 로 낸다.
#define PWR_VBAT_DIVIDER            (2)        // 하드웨어 분압비(100K/100K → ×2)
// 데이터패스 검증용 평문 HTTP GET 대상 (안정적·경량 엔드포인트)
#define LTE_TEST_HOST               "example.com"
#define LTE_TEST_PORT               (80)
#define LTE_TEST_PATH               "/"

// --- 증분 B: MQTT/TLS (다음 증분에서 사용, 미리 확정 기록) ---
// 도메인 컨벤션 <역할>.keyplus.sponeinfra.com (sponeinfra.com = Route53)
#define MQTT_BROKER_HOST            "mqtt.keyplus.sponeinfra.com"
#define MQTT_BROKER_PORT            (8883)
// MQTT keepalive. 단, 모뎀이 PINGREQ를 자동발송하지 않고 실 드롭은 캐리어 NAT(≈60초)가
// 주도하므로, 실제 연결유지는 publish 주기(아래 MQTT_PUBLISH_INTERVAL_MS)가 담당한다.
// keepalive는 publish 주기보다 크게만 두면 됨.
#define MQTT_KEEPALIVE_S            (120)
// ⚠️ 실측: LG U+ 캐리어 NAT가 유휴 TCP를 약 60초에 끊는다(+CMQTTCONNLOST:0,2).
// 모뎀이 PINGREQ 자동발송을 안 하므로, publish 주기를 그보다 짧게 잡아 publish 자체로
// NAT를 열어둔다(30초 → 여유 2배). cmd 증분(유휴 상시연결) 땐 실 PING으로 재설계.
#define MQTT_NAT_IDLE_TIMEOUT_MS    (60000UL) // 캐리어 NAT 유휴 드롭 시간(실측)
#define MQTT_PUBLISH_INTERVAL_MS    (30000UL) // telemetry 발행 주기(= NAT keep-alive 역할)
#define MQTT_RECONNECT_CAP_MS       (15000UL) // 재접속 백오프 상한 (최악도 20초 내 수렴)
// Mqtt::handle의 URC 수신 상한. 유휴 시 100ms를 통째로 버리던 문제는 "UART에 바이트가
// 있을 때만 진입"하는 가드로 해결했으므로(mqtt.cpp handle 주석), 이 값은 짧을 필요가 없다.
// ⚠️ 오히려 짧으면 안 된다. cmd 수신은 +CMQTTRXSTART → RXTOPIC → RXPAYLOAD → RXEND
//    4단계 URC를 연달아 파싱하는데, 래퍼가 각 단계마다 이 타임아웃을 쓴다.
//    중간에 한 번이라도 끊기면 메시지를 통째로 놓친다 → 래퍼 기본값과 같은 100ms 유지.
#define MQTT_HANDLE_TIMEOUT_MS      (100)
// --- 접속 실패가 loop 를 잡고 있는 시간 --------------------------------------
// ⚠️ 이 세 값은 여기가 아니라 lib/TinyGSM/src/TinyGsmMqttA76xx.h 상단에 있다.
//    래퍼가 config.h 를 include 하지 않고, mqtt.h 가 config.h 보다 먼저 TinyGSM 을
//    끌어오기 때문이다 — 여기에 같은 이름을 두면 래퍼의 #ifndef 기본값이 이미 이겨서
//    조용히 무시된다. 바꾸려면 그 헤더에서 고치거나 빌드 플래그(-D)로 덮을 것.
//      TINY_GSM_MQTT_PROMPT_TIMEOUT_MS       (3000)  CCERTDOWN 의 '>' 프롬프트
//      TINY_GSM_MQTT_CONNECT_ACK_TIMEOUT_MS  (5000)  CMQTTCONNECT 접수 OK
//      TINY_GSM_MQTT_CERT_REUPLOAD_MIN_MS   (60000)  실패 후 CA 재업로드 최소 간격
//    실측(260810 로그2) 재접속 실패 1건이 loop 를 11~44초 멈췄다. 그 구간에 OBD 5초
//    폴·GPS 10초 폴이 통째로 빠지고 차키 응답도 밀린다 — 오프라인 수집의 존재 이유가
//    "못 보내는 동안에도 계속 모은다"인데 정작 그때 수집이 멈추는 셈이었다.
//    ⚠️ TLS 핸드셰이크·CONNACK 대기(+CMQTTCONNECT URC, 30초)는 줄이지 않았다.
//       LTE 위 TLS 는 5~10초가 정상이라 줄이면 멀쩡한 접속이 실패한다.
// --- 발행 완료 확인 (P3) ----------------------------------------------------
// 래퍼 mqtt_publish() 는 AT+CMQTTPUB 이 OK 를 내면 곧바로 true 를 낸다. 실제 전달
// 결과는 비동기 URC `+CMQTTPUB: <idx>,<err>` 에만 있고, 원본 래퍼는 그걸 파싱하지 않아
// "### Unhandled" 로 버렸다 — 260804 로그 전체에 그 줄이 도배돼 있다.
// 즉 지금까지의 "published" 는 전달 완료가 아니라 "모뎀이 요청을 받았다"였다.
//   → 래퍼가 URC 를 기록하고(mqttPubAckPending/mqttLastPubErr), 이 시간 안에 결과가
//     오지 않으면 세션이 죽은 것으로 본다. R1 프로브(5초)보다 빠른 경로가 될 수 있다.
// QoS1 은 브로커 PUBACK 왕복이라 정상 망에서 1초 안쪽이다. 8초면 넉넉하다.
#define MQTT_PUB_ACK_TIMEOUT_MS     (8000UL)
// --- CMQTT 서비스 시작 실패 → 모뎀 리셋 승격 --------------------------------
// mqtt_begin(CMQTTSTART)이 이만큼 연속 실패하면 모뎀을 리셋해 상태를 강제 재동기한다.
// 펌웨어와 모뎀의 상태가 어긋나면(펌웨어 "서비스 꺼짐" vs 모뎀 "켜짐") 재시도는 100%
// 같은 자리에서 실패하는데, LTE(PDP)는 멀쩡해서 재브링업·리셋 경로도 안 탄다 —
// 스스로는 빠져나올 수 없는 유일한 교착이다. 백오프가 1s→2s→4s 이므로 3회면 약 7초에
// 승격한다(실측 교착은 107초였다).
#define MQTT_SERVICE_FAIL_BEFORE_RESET (3)
// --- 세션 접속(CMQTTCONNECT) 연속 실패 → 단계 승격 --------------------------
// 위 SERVICE_FAIL 은 "서비스가 안 뜬다"(CMQTTSTART)만 잡는다. 서비스는 떠 있는데
// 브로커 접속만 계속 실패하는 상태는 아무도 세지 않아 승격 경로가 없었다 —
// Lte::isUp()(등록+PDP)은 참이라 재브링업도 안 타고, 모뎀도 AT 에 응답하니 리셋도 안 탄다.
// 실측(260810 로그): 15초 백오프 포화 상태로 25분 넘게 mqtt=OFF, 백로그 1500건,
// 재부팅해야만 회복됐다(같은 CA·비번·APN 으로 5초 만에 접속 — 모뎀 쪽 상태 문제).
//   1단계: 데이터패스 실검증(HTTP GET) + CMQTT 서비스 재시작(CMQTTSTOP→START)
//   2단계: 모뎀 하드 리셋
// 백오프가 15초에서 포화되므로 4회 ≈ 1분에 1단계, 그 뒤 4회 ≈ 1분 더에 2단계.
#define MQTT_CONNECT_FAIL_BEFORE_RESTART (4)
#define MQTT_CONNECT_FAIL_BEFORE_RESET   (4)
#define MQTT_CA_FILENAME            "emqx_ca.pem"

// --- MQTT 수신 버퍼 ---------------------------------------------------------
// ⚠️ 래퍼(TinyGsmMqttA76xx)는 토픽과 페이로드를 이 버퍼 "하나"에 나눠 담는다
//    (topic + '\0' + payload). 기본 256B 로는 토픽 v1/{device_id}/cmd (24B) 를 빼면
//    페이로드가 231B 밖에 안 남아 ota_start 명령이 경고 없이 잘렸다.
//    Mqtt::begin 이 서비스 시작 전에 mqtt_set_rx_buffer_size() 로 이 값을 적용한다.
#define MQTT_RX_BUFFER_SIZE         (1024)
// cmd.cpp 가 RX 콜백에서 복사해 두는 명령 JSON 버퍼. 위 버퍼에서 토픽을 뺀 만큼이
// 실제 상한이므로 같은 크기로 둔다(실효 상한 ≈ 1000B).
#define CMD_PAYLOAD_MAX             (1024)
// 수신 큐 칸 수(F4). 콜백은 복사만 하고 실행은 loop 가 틱당 1건씩 비우므로, 서버
// flushPendingForDevice() 가 pending 을 연속 발행하면 1칸 버퍼로는 첫 건만 살아남는다
// — 게다가 유실이 조용해서 "가끔 안 먹는다"로만 보고된다. 명령 자체는 드물어 4칸이면
// 실무상 충분하다. RAM 은 4 × CMD_PAYLOAD_MAX = 4KB(정적).
#define CMD_RX_QUEUE_N              (4)
// 명령 실행을 다음 틱으로 미루는 지연(F2 §5). received ack 를 낸 뒤 곧바로 실행하면
// 실행측 발행(done/failed)이나 OTA 의 CMQTTSTOP 이 아직 안 빠진 +CMQTTPUB URC 와
// 겹친다 — 모뎀은 publish 가 겹치면 세션이 깨진다(실측). FAST_PUBLISH_DELAY_MS 와 동일.
#define CMD_DEFER_EXEC_MS           (1000UL)

// 콘솔 AT 조회(info/status) 전에 대기 중인 URC 를 소화시키는 시간 상한.
// info/status 는 getIMEI()·getSimStatus() 같은 AT 를 직접 보내는데, 발행 ACK
// (+CMQTTPUB) 같은 URC 가 스트림에 남아 있으면 그게 응답으로 잘못 읽힌다
// — 실측에서 imei 자리에 "+CMQTTPUB: 0,0" 이 그대로 찍혔다.
#define CONSOLE_URC_DRAIN_MS        (300)

// --- cmd 구독 페이싱 --------------------------------------------------------
// 접속 직후 첫 구독을 미루는 이유: connectSession이 status online을 발행하고 나면
// 그 ACK(+CMQTTPUB URC)가 URC 스트림에 떠 있는데, 그 상태로 SUBACK을 기다리면
// 파싱이 엉킨다. telemetry 첫 발행을 5초 미루는 것과 같은 이유다.
#define CMD_SUB_DELAY_MS            (5000UL)
// 구독 실패 시 재시도 간격(실패마다 2배, 상한까지). 재시도가 없으면 접속이 유지되는 한
// 재접속 상승엣지가 오지 않아 다운링크가 영영 죽은 채로 남는다.
#define CMD_SUB_RETRY_BASE_MS       (15000UL)
#define CMD_SUB_RETRY_CAP_MS        (300000UL)  // 5분

// ── 브로커 선택 ─────────────────────────────────────────────────────────
// EMQX 기동 전엔 공개 테스트 브로커로 검증. 로컬 EMQX가 뜨면 아래 MQTT_BROKER_SEL
// 한 줄만 LOCAL로 바꾸면 호스트/포트/TLS/인증/CA가 자동 선택된다(코드 불변).
#define MQTT_BROKER_TEST     1   // test.mosquitto.org (공개 TLS, 익명) — 검증됨
#define MQTT_BROKER_LOCAL    2   // 로컬 EMQX (기동 중 — 아래 TODO 값 확정 필요)
#define MQTT_BROKER_PROD     3   // 실 EMQX (mqtt.keyplus.sponeinfra.com, 상용)

#define MQTT_BROKER_SEL      MQTT_BROKER_LOCAL  // ← office/dev EMQX (mqtt-dev.keyplus.sponeinfra.com:40102 TLS)

#if   MQTT_BROKER_SEL == MQTT_BROKER_TEST
  #define MQTT_HOST          "test.mosquitto.org"
  #define MQTT_PORT          (8883)
  #define MQTT_USE_TLS       1     // TLS 접속(서버 CA 검증)
  #define MQTT_ANON          1     // 익명(비번 미검증)
  #define MQTT_TLS_VERIFY    1     // 서버 인증서 CA 검증(authmode=1)
  #define MQTT_TLS_RELAX     0     // 정식 cert(호스트명 일치) → 완화 불필요
#elif MQTT_BROKER_SEL == MQTT_BROKER_LOCAL
  // office/dev EMQX (도메인:포워딩포트, TLS 자체서명 = certs.h CA_LOCAL_EMQX)
  #define MQTT_HOST          "mqtt-dev.keyplus.sponeinfra.com"  // dev 도메인(DDNS, gateway 40101과 동일 호스트)
  #define MQTT_PORT          (40102)          // 내부 8883 → 포워딩 40102 (TLS)
  #define MQTT_USE_TLS       1                // TLS 접속
  #define MQTT_ANON          0                // device_id(username) + 발급 mqtt_pw 인증
  #define MQTT_TLS_VERIFY    1                // CA 검증(authmode=1) — 래퍼가 authmode=0 미지원
  #define MQTT_TLS_RELAX     1                // 도메인 접속이라 cert CN 일치 → 0으로 낮춰도 됨(CA 검증은 유지)
#else // MQTT_BROKER_PROD
  #define MQTT_HOST          MQTT_BROKER_HOST // mqtt.keyplus.sponeinfra.com
  #define MQTT_PORT          MQTT_BROKER_PORT
  #define MQTT_USE_TLS       1
  #define MQTT_ANON          0
  #define MQTT_TLS_VERIFY    1     // 정식 도메인 cert(Let's Encrypt) → CA 검증
  #define MQTT_TLS_RELAX     0     // 호스트명 일치 → 완화 불필요
#endif

// --- 자동 프로비저닝 엔드포인트 (작업1 — /internal/provision) ---
// 단말이 부팅 시 device_id/pw가 없으면 {imei,mac,fw}를 POST해 발급받는다.
// office/dev device-gateway(도메인:포워딩포트). LTE 단말이 외부에서 닿는 공개 주소여야 함.
// gateway-tls(Caddy)가 40101 에서 TLS 종단 → device-gateway:8081. 응답에 mqtt_password 실림 → HTTPS.
#define PROVISION_HOST              "mqtt-dev.keyplus.sponeinfra.com"
#define PROVISION_PORT              (40101)
#define PROVISION_PATH              "/internal/provision"
#define PROVISION_USE_TLS           1     // 0=평문 HTTP, 1=HTTPS(모뎀 내장 HTTPS, OTA와 동일 경로)
// 오프라인(서버 없이) 테스트용: 1이면 NVS 비었을 때 아래 DEVICE_ID로 시드(자동 발급 생략).
// 기본 0 — NVS 비면 미프로비저닝 상태로 두고 부팅 시 /internal/provision 발급을 시도.
#define PROVISION_SEED_FALLBACK     0

// 단말 신원/버전 — TODO(provisioning): NVS 프로비저닝으로 이관. 지금은 검증용 상수.
// 포맷 vt-YYMM-NNNN-XXX (계약 @keyplus-diagnostic/shared deviceId.ts 와 동일).
#define DEVICE_ID                   "vt-2607-0001-x7q"
// 펌웨어 버전 — 부팅 배너/info, telemetry meta, 프로비저닝 요청 body, OTA 결과 검증에
// 모두 이 값이 쓰인다(단일 출처). OTA 로 새 이미지를 내릴 때는 서버가 기대하는 version 과
// 반드시 일치시켜야 한다 — 불일치면 재부팅 후 ota ack 가 failed 로 나간다(ota.cpp).
// ⚠️ 패치 자리를 두 자리로 올리지 않는다(0.3.9 다음은 0.3.10 이 아니라 0.4.0). 이 값은
//    서버 기록·OTA 타깃으로 그대로 나가는데, 버전을 문자열로 정렬하는 소비자가 있으면
//    "0.3.10" < "0.3.9" 가 되어 조용히 역전된다. 단말 쪽은 동등 비교뿐이라 무해하지만
//    (ota.cpp), 바깥까지 확인할 수 없으므로 애초에 그 자리를 만들지 않는다.
#define FW_VERSION                  "0.5.0"

// ===========================================================================
// UART 로그 레벨 (log.h)
//   평상시엔 앱 상태 전이 + 주기 [STAT] 한 줄만, 진단할 때만 폴 단위 상세를 켠다.
//   부팅 배너(펌웨어 버전/단말 신원/설정 요약)는 레벨과 무관하게 항상 출력한다.
// ===========================================================================
// 컴파일 타임 상한. 이보다 상세한 LOGx 호출은 바이너리에서 제거되어 런타임으로도 못 켠다.
// 양산에서 플래시/시리얼 대역을 아끼려면 3(INFO)으로 내린다.
#define LOG_LEVEL_MAX               (4)   // 1=ERROR 2=WARN 3=INFO 4=DEBUG
// 부팅 시 기본 런타임 레벨. NVS("cfg"/"log_lv")에 저장된 값이 있으면 그쪽이 우선한다.
// 콘솔 'log debug' / 'log info' 로 현장에서 바꾸며, 변경은 재부팅 후에도 유지된다.
#define LOG_DEFAULT_LEVEL           (3)   // INFO

// --- 보조 콘솔 UART (외부 USB-TTL 어댑터) ---------------------------------
// 차량 상시전원 장착 상태에서 USB-C 없이 콘솔을 쓰기 위한 두 번째 포트.
// USB-C는 연결 시 브리지의 DTR/RTS 자동리셋 회로가 보드를 재부팅시키고,
// 헤더의 UART0(GPIO1/3)은 그 브리지와 선을 공유해 외부 어댑터 TX가 GPIO3에서 경합한다
// (수신은 되는데 송신만 안 먹는 증상). 상세 배경은 console.h 헤더 주석 참고.
#define FEATURE_AUX_CONSOLE         1
#define AUX_CONSOLE_TX_PIN          (13)  // 헤더 노출, TF Card CS(미사용), 스트랩 아님
#define AUX_CONSOLE_RX_PIN          (34)  // 헤더 노출, 미사용, 입력 전용(RX 전용으로 적합)
#define AUX_CONSOLE_BAUD            (115200)
// [STAT] 한 줄은 telemetry 발행 때마다 나온다. 미접속이라 발행이 없을 땐 이 간격으로
// 따로 남겨, 로그가 조용한 것과 펌웨어가 멈춘 것을 구분할 수 있게 한다.
#define STATUS_LINE_IDLE_MS         (30000UL)

// ===========================================================================
// OTA / config_update 설정 (8단계)   설계: phase8-firmware-ota-lte.md
//   ota_start: 모뎀 HTTP(S) GET 스트리밍 → Update.h(듀얼 OTA 파티션) → 재부팅.
//   2단계 ack(NVS 지속): 재부팅 전 command_id·기대버전 저장 → 부팅 후 검증하여 done/failed.
// ===========================================================================
// 다운로드 스트리밍 청크 크기(모뎀 https_body 1회 read). 예제와 동일 1KB.
#define OTA_CHUNK_SIZE              (1024)
// 다운로드 무진전(타임아웃) 상한 — 이 시간 내 바이트 진전 없으면 실패 처리.
#define OTA_STALL_TIMEOUT_MS        (60000UL)
// OTA 진행률 로그 간격(%).
#define OTA_PROGRESS_STEP_PCT       (10)

// --- 하드 롤백 (부트로더 레벨, CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y + 부트 WDT) ---
// 새 이미지는 부팅 후 PENDING_VERIFY 상태. 헬스체크(MQTT 접속) 통과 시 mark_valid로 확정한다.
// 확정 전 크래시/행(WDT 9s) → 부트로더가 자동으로 직전 정상 이미지로 롤백.
// 아래 期限 내 확정(MQTT 접속) 못 하면 = 부팅은 되나 기능 불능 → 강제 롤백 재부팅.
// (정상 부팅은 ~1~2분 내 접속하므로 넉넉히. 일시적 망 장애 오탐 방지 위해 크게 둔다.)
#define OTA_CONFIRM_DEADLINE_MS     (600000UL)  // 10분

// config_update 런타임 기본값(부팅 시 NVS "cfg"가 있으면 그 값으로 대체).
//   telemetry_interval_ms → 즉시 적용 / keepalive_s → 다음 MQTT 접속 반영.
#define CFG_DEFAULT_TELE_MS         MQTT_PUBLISH_INTERVAL_MS
#define CFG_DEFAULT_KEEPALIVE_S     MQTT_KEEPALIVE_S
// 안전 가드(원격 오설정 방지). 범위 밖 값은 명령 무시.
#define CFG_TELE_MS_MIN             (5000UL)
// ⚠️ 상한은 캐리어 NAT 유휴 드롭(MQTT_NAT_IDLE_TIMEOUT_MS)에 묶인다.
// 모뎀이 PINGREQ를 자동발송하지 않아 publish 자체가 유일한 연결유지 수단이므로,
// 이보다 긴 주기를 허용하면 config_update 한 번으로 단말이 발행→NAT드롭→재접속 루프에
// 빠진다(keepalive_s를 아무리 올려도 PING을 안 보내니 소용없다).
// 더 긴 주기가 필요하면 별도 PINGREQ 태스크를 먼저 도입해야 한다.
#define CFG_TELE_MS_MAX             (MQTT_NAT_IDLE_TIMEOUT_MS * 3 / 4)  // 45초 (NAT의 75%)
#define CFG_KEEPALIVE_S_MIN         (30)
#define CFG_KEEPALIVE_S_MAX         (1800)
// keepalive_s 는 telemetry 주기의 최소 몇 배여야 하는가. publish 사이 유휴 동안 브로커가
// 먼저 세션을 끊으면(MQTT 규격상 keepalive의 1.5배에서 끊는다) 재접속 루프가 된다.
#define CFG_KEEPALIVE_TELE_RATIO    (2)

// 컴파일 타임 정합성 — 기본 발행 주기가 원격 설정 상한을 넘으면 안 된다.
#if (MQTT_PUBLISH_INTERVAL_MS > CFG_TELE_MS_MAX)
#error "MQTT_PUBLISH_INTERVAL_MS가 CFG_TELE_MS_MAX를 초과 — 캐리어 NAT 유휴 드롭에 걸린다"
#endif
#if (MQTT_KEEPALIVE_S * 1000UL < MQTT_PUBLISH_INTERVAL_MS * CFG_KEEPALIVE_TELE_RATIO)
#error "MQTT_KEEPALIVE_S가 발행 주기 대비 너무 짧다 — 브로커가 먼저 세션을 끊는다"
#endif

// ===========================================================================
// 오프라인 적재·백필 + 고빈도(1Hz) 샘플   설계: offline-telemetry-buffer.md §8.3
// ===========================================================================

// --- 시각 기준(Clk) ---------------------------------------------------------
// base 갱신 이후 이 시간이 지나면 출처를 DERIVED(millis 외삽)로 표시한다.
// telemetry 발행 주기(기본 30초)마다 갱신되므로, 그 2~3배를 넘겼다면 실제로
// 시각 소스가 끊긴 상태다. 드리프트 자체는 ±20ppm(하루 ±1.7초)라 값은 계속 쓸 만하다.
#define CLK_SRC_FRESH_MS            (120000UL)

// --- 오프라인 적재 ----------------------------------------------------------
// 주행 30초 → 19.5시간 / 정차 120초 → 77.9시간(3.2일) 보관.
#define OFFLINE_LOG_MS_DRIVING      (30000UL)
#define OFFLINE_LOG_MS_PARKED       (120000UL)

// ⚠️ SPIFFS 로 마운트하지 않는다 — esp_partition_* raw 전용(buffer.h 헤더 주석).
#define BUF_PARTITION_LABEL         "spiffs"
#define BUF_RECORD_SIZE             (56)
#define BUF_SECTOR_SIZE             (4096)
#define BUF_SECTOR_HEADER_SIZE      (8)         // magic u32 + sector_seq u32
// (4096 − 8헤더) / 56 = 73. 실제로는 4096 − 8 = 4088 = 56×73 정확히 나누어떨어진다.
#define BUF_RECS_PER_SECTOR         (73)
// 섹터 수는 런타임에 part->size 에서 계산한다(파티션 재설계 대비). 이 값은 정적 배열
// 상한일 뿐이며, 현 파티션 표(128KB)에서는 32 가 나온다. 1,408KB 재설계안(352섹터)까지 수용.
#define BUF_MAX_SECTORS             (352)
// NVS 커서 저장 간격 — 전원 단절 시 최대 (이 값−1)건이 재전송된다.
// 서버 telemetry PK 가 (device_id, ts) + ON CONFLICT DO NOTHING 이라 중복은 흡수된다.
#define BUF_CURSOR_SAVE_EVERY       (32)

// --- 발행 간 최소 간격 (모뎀 보호) ------------------------------------------
// 모뎀은 publish 를 1건씩만 처리하고 겹치면 세션이 깨진다(실측 +CMQTTCONNLOST).
// 래퍼 mqtt_publish() 는 AT+CMQTTPUB 의 OK 까지만 기다리고 결과 URC(+CMQTTPUB: 0,0)는
// 비동기로 늦게 온다 — 즉 "반환됐다"가 "발행이 끝났다"는 뜻이 아니다.
// 그래서 telemetry 를 뺀 모든 발행 경로(status 전이·고빈도 창·백필)는 직전 발행으로부터
// 이 간격이 지났을 때만 나간다(Mqtt::publishGapElapsed).
#define PUBLISH_MIN_GAP_MS          (1000UL)

// --- status 전이 재발행 (F1) ------------------------------------------------
// status 는 원래 접속당 1회(connectSession)라 시동 on/off 를 따라가지 못한다.
// (OBD 링크, rpm>0) 이 바뀔 때만 재발행해 power_mode·ignition_on 의 출처를 단말로 되돌린다.
// ⚠️ 주기 발행이 아니다 — 전이가 없으면 한 건도 나가지 않는다.
#define STATUS_REPUB_GUARD_MS       (2000UL)   // 다음 실시간 telemetry 직전 금지 구간

// --- 백필 페이싱 (실시간 발행이 절대 우선) ----------------------------------
#define BACKFILL_START_DELAY_MS     (15000UL)  // 접속 후 백필 시작까지
#define BACKFILL_MIN_GAP_MS         (1000UL)   // 백필 발행 최소 간격(틱당 1건)
#define BACKFILL_GUARD_MS           (2000UL)   // 다음 실시간 발행 직전 금지 구간

// --- 고빈도 샘플 ------------------------------------------------------------
// ⚠️ 런타임 기본값은 0(비활성)이다. 서버 EMQX Rule(`v1/+/telemetry/fast`)과
//    /ingest/telemetry/fast 가 배포되기 전에 발행하면 브로커에서 조용히 버려진다
//    — 무해하지만 데이터·전력 낭비다. 서버 준비 후 config_update {"fast_ms":1000}
//    으로 OTA 없이 켠다. 발행 부하가 의심되면 같은 방법으로 즉시 끌 수 있다.
#define CFG_DEFAULT_FAST_MS         (0UL)
#define FAST_SAMPLE_MS              (1000UL)   // 1Hz — fast_ms 활성화 시 권장값
#define CFG_FAST_MS_MIN             (1000UL)   // 1Hz 보다 빠르게는 올리지 않는다(폴 듀티·발행량)
#define CFG_FAST_MS_MAX             (10000UL)  // 0.1Hz — 이보다 느리면 고빈도의 의미가 없다
#define FAST_WINDOW_MAX             (64)       // 창 최대 원본 샘플 수 (RAM 384B)
#define FAST_PUBLISH_MIN_N          (2)        // 이 미만이면 발행 생략
#define FAST_PUBLISH_DELAY_MS       (1000UL)   // 실시간 telemetry 발행 후 이만큼 뒤 발행
#define FAST_PAYLOAD_BUF            (2048)     // n=64 에서 약 1.2KB (모뎀 상한 10,240B)
#define FAST_MISS_ABORT             (2)        // 연속 무응답 시 고빈도 폴 중단
// 슬롯 유실(폴 실패·차속 미응답) 시 재시도 간격(0.3.9). 격자를 **당기기만** 하므로
// 다음 격자를 넘지 않고, 실패해도 원래 격자로 복귀한다. 반올림 초가 흔들리지 않도록
// 500ms 보다 넉넉히 작게 잡는다. 3 PID 왕복(≈60ms)의 두 배 남짓이면 충분하다.
#define FAST_RETRY_MS               (150UL)

// ── 시동 OFF 종료(0.3.9) ────────────────────────────────────────────────────
// CAN 링크(obd.valid)만 보던 게이트는 "시동은 껐는데 버스는 아직 깨어 있는" 구간을
// 못 거른다. 실측(260806): 13:06 시동 OFF 후 13:34 까지 28분간 spd=0·rpm=0 샘플이
// 계속 쌓여, 그 시간대 1,942건 중 1,678건(86%)이 정차분이었다. 주행 16분을 담으려고
// 37분치를 저장한 셈이고 telemetry_fast 보관은 30일이다.
//
// 5샘플(=5초)로 잡은 이유: 신호대기 정차는 rpm 이 공회전(≈750)이라 애초에 걸리지
// 않는다. 걸리는 건 rpm 이 확정 0 인 구간뿐이라 짧게 잡아도 오종료가 없고, 짧을수록
// 꼬리가 준다. 시동 직후 크랭킹 중 rpm 이 잠깐 0 으로 읽히는 경우를 흡수할 만큼은 된다.
#define FAST_ENGINE_OFF_STOP_N      (5)        // rpm 확정 0 + 정지가 이만큼 연속되면 수집 중지
// 중지 중 재시동 확인 주기. 전체 폴(OBD2_POLL_INTERVAL_MS)과 같은 5초 —
// 그보다 촘촘히 볼 이유가 없고, 주차 중 1Hz 폴은 그 자체가 버스를 깨워 둔다.
#define FAST_ENGINE_OFF_PROBE_MS    (5000UL)
// 가감속 계산 시 인정하는 최대 샘플 간격. 이보다 벌어졌으면(발행 블로킹·폴 지연)
// Δspd 를 그 간격으로 나눠도 의미가 없어 가감속을 계산하지 않는다 — 결측 구간을
// 1초로 가정하면 가짜 급가속이 나온다.
#define FAST_ACCEL_MAX_GAP_MS       (5000UL)

// 오프라인 집계용 이벤트 임계 (온라인 구간은 서버가 1Hz 원본으로 판정한다).
// ⚠️ 이 값들은 단말에 박히므로 변경에 OTA 가 필요하다. 서버측 event_rules 와 어긋날 수
//    있어, 서버는 agg 유래 이벤트를 source='agg' 로 구분해 저장한다.
//
// 지속 조건(*_MIN_S)은 서버 event_rules 기본값(min_s)과 맞춘 것이다. 임계를 스치는
// 1샘플로 이벤트를 세우면 서버 판정과 크게 어긋난다 — 신호등 정차 한 번에 공회전
// 비트가 서고, 노면 요철 한 번이 급제동이 된다.
#define FAST_HARSH_ACCEL_MS2_X10    (30)       // +3.0 m/s²
#define FAST_HARSH_DECEL_MS2_X10    (-35)      // -3.5 m/s²
#define FAST_HARSH_MIN_S            (2)        // 서버 harsh_accel / harsh_brake 의 min_s
#define FAST_OVERSPEED_KMH          (110)
#define FAST_OVERSPEED_MIN_S        (10)       // 서버 overspeed 의 min_s
#define FAST_IDLE_RPM_MIN           (500)
// ⚠️ 서버 idling 기본값은 min_s=180 이지만 여기서는 30 이다. 주행 창이 30초라
//    180초 지속은 창 안에서 관측 자체가 불가능하다(정차 창 120초에서도 못 채운다).
//    → agg 유래 공회전은 서버 판정보다 느슨하다. source='agg' 로 구분되므로 허용한다.
#define FAST_IDLE_MIN_S             (30)

// 컴파일 타임 정합성 — 레코드/섹터 산술이 어긋나면 링 전체가 조용히 깨진다.
#if (BUF_RECS_PER_SECTOR * BUF_RECORD_SIZE + 8 > BUF_SECTOR_SIZE)
#error "BUF_RECS_PER_SECTOR × BUF_RECORD_SIZE + 헤더 8B 가 섹터 크기를 넘는다"
#endif
#if (BUF_RECORD_SIZE % 4 != 0)
#error "BUF_RECORD_SIZE 는 4의 배수여야 한다 (esp_partition_write 정렬 요구)"
#endif

// ===========================================================================
// 상태표시 LED 핀 (3단계 — 외장 LED, active-high)  설계: phase3-firmware-provisioning-led.md §4
//   ⚠️ bare T-A7670E는 SW 제어 가능한 온보드 LED가 없다: utilities.h의 LILYGO_T_A7670
//      섹션에 BOARD_LED_PIN 미정의, GPIO12는 BOARD_POWERON_PIN(모뎀 전원 유지)로 점유됨.
//      → 외장 LED를 GPIO23에 배선.  GPIO23 ──[1kΩ]──▶|── GND  (3mm 빨강, HIGH=점등)
//   ⚠️ GPIO23은 아래 PIN_KEY_SPARE(5b단계 차키 예비)와 물리핀 공유. 차키 단계 도입 시
//      FEATURE_STATUS_LED=0 으로 끄거나 LED 핀을 재배치할 것.
// ===========================================================================
#define PIN_STATUS_LED     (23)
#define LED_ON_LEVEL       HIGH

// ===========================================================================
// 차키 제어 핀맵 (5b단계 — 확정, 4 GPIO 한정)  [현재 미사용]
//   전원=스위처블 LDO(MIC5504-3.3 EN, active-high), 버튼=2N7002 오픈드레인(active-low)
// ===========================================================================
#define PIN_KEY_PWR_EN      (32)  // fob 전용 LDO EN (active-high) [현 단계 미제어 — fob 상시급전]
#define PIN_KEY_LOCK        (18)  // 잠금  (2N7002 게이트 → GPIO HIGH=누름, 패드 active-low)
#define PIN_KEY_UNLOCK      (19)  // 열림  (2N7002 게이트 → GPIO HIGH=누름, 패드 active-low)
#define PIN_KEY_SPARE       (23)  // 예비/원격시동 (2N7002 게이트) [현 단계 미사용 — LED와 공유]

// 버튼 1회 누름 유지시간(ms). fob가 인식하는 최소 펄스 폭 이상으로 둔다.
// press()는 논블로킹이다 — 누름만 걸어두고 Carkey::update()가 시간이 되면 뗀다.
// 따라서 이 값을 늘려도 loop 는 멈추지 않는다(LED·콘솔·MQTT 정상 동작).
// 콘솔 'lock 2000' / 'unlock 2000' 처럼 인자로 일시 변경 가능.
#define CARKEY_PRESS_MS    (500)
// 누름 유지시간 상한. 트렁크/파노라마처럼 길게 눌러야 하는 기능이 있어 여유를 둔다.
// 상한을 두는 이유는 오조작(오타로 999999 입력) 시 라인이 계속 눌린 채로 남는 것을
// 막기 위함이다 — 논블로킹이라 그 사이 펌웨어는 멀쩡히 도는 만큼 더 위험하다.
#define CARKEY_PRESS_MAX_MS (10000)

// ── 차키 버튼 구동 방식 선택 ──────────────────────────────────────────────
//   DIRECT = fob 버튼패드에 GPIO 직결(오픈드레인). LOW=누름 / HIGH=Hi-Z(뗌).
//            트랜지스터 반전이 없어 fob 패드 극성(active-low) 그대로. MOSFET/TR 미준비 시.
//            ⚠️ 전제: 공통 GND + fob Vcc=3.3V(상시급전 점퍼) → 로직 레벨 일치.
//            ⚠️ 반드시 오픈드레인 — HIGH를 push하면 꺼진 fob에 역급전(back-feed) 위험.
//   MOSFET = 2N7002 게이트 구동(푸시풀). HIGH=누름 / LOW=뗌. 서브보드 실장 후 최종 배선.
//   전환은 아래 CARKEY_DRIVE_SEL 한 줄만 바꾸면 됨(핀맵·로직 자동 선택, 코드 불변).
#define CARKEY_DRIVE_DIRECT   1
#define CARKEY_DRIVE_MOSFET   2
#define CARKEY_DRIVE_SEL      CARKEY_DRIVE_DIRECT   // ← 현재: TR 미준비 → 직결

// ── 버튼 "누름" 활성 레벨 오버라이드 ──────────────────────────────────────
//   AUTO(기본) = 위 CARKEY_DRIVE_SEL 이 극성까지 정한다. DIRECT→LOW누름(오픈드레인),
//                MOSFET→HIGH누름(푸시풀). 평소엔 이 값을 건드리지 말 것.
//   0 / 1 = 극성 강제(실물 배선이 위 가정과 반대로 나온 예외 상황 전용). 이때 핀모드는
//           항상 푸시풀 OUTPUT 이 된다 — 오픈드레인은 HIGH 를 source 하지 못하므로.
//   ⚠️ DIRECT 배선 + 강제 1 조합은 금물: 뗌 상태가 푸시풀 LOW(=GND 고정)라 fob 버튼이
//      상시 눌린 것과 같아지고, fob 전원이 꺼지면 역급전(back-feed) 경로도 생긴다.
//      → 그 조합은 carkey.cpp 에서 컴파일 경고로 잡는다.
#define CARKEY_ACTIVE_AUTO    (-1)
#define CARKEY_ACTIVE_HIGH    CARKEY_ACTIVE_AUTO   // ← 현재: DRIVE_SEL 따름

// ===========================================================================
// OBD2 CAN 핀맵/설정 (6단계 — 외부 CAN 트랜시버 SN65HVD230 등, ESP32 내장 TWAI)
//   차량 OBD2 포트: CAN-H/CAN-L → 트랜시버 → GPIO21(RX)/22(TX). 공통 GND 필수.
//   프로토콜: ISO 15765-4 (CAN). 요청 0x7DF, 응답 0x7E8~0x7EF. Mode 01 실시간 PID(단일 프레임).
// ===========================================================================
#define PIN_CAN_RX          (21)
#define PIN_CAN_TX          (22)
// 최신 차량(2008+) 표준 500kbps/11-bit. 응답 없으면 250kbps로 폴백 시도(begin에서 자동).
#define OBD2_POLL_INTERVAL_MS   (5000UL)  // 실시간 PID 폴링 주기(주차 중 과폴 방지)
#define OBD2_REQ_TIMEOUT_MS     (100)     // PID 1건 응답 대기 상한
// 링크 미확립(응답0) 시 재초기화 간격 — 고정. 시동을 걸면 늦어도 이 간격 안에 잡힌다.
// begin 1회는 비트레이트 2종 무응답 타임아웃 + 드라이버 install/uninstall 로 수백 ms
// 블로킹이지만, 30초 간격이면 그 비용이 무시할 수준(약 2%)이라 백오프를 두지 않는다.
#define OBD2_LINK_RETRY_MS      (30000UL)
// 확장 PID(>0x20: 0x2F/0x42/0xA6) 미지원 래치 임계. 지원 마스크(0x00 응답)는 0x01~0x20만
// 커버해서 이들은 무조건 요청하게 되는데, 미지원 차량이면 매 폴 1건당 OBD2_REQ_TIMEOUT_MS를
// 순수 타임아웃으로 버린다. 연속 이 횟수만큼 무응답이면 링크 재확립 전까지 요청을 생략한다.
// ⚠️ 3 → 5: 시동 직후 버스가 폭주하는 구간에서 응답을 놓쳐 지원 PID 를 미지원으로
//    오래치한 사례가 있다(0xA6 지원 확정 차량에서 발생). 래치는 "고정 ECU 가 같은 폴에서
//    다른 PID 에 답하고 있을 때"만 세지만(obd2.cpp), 임계에도 여유를 준다.
#define OBD2_EXT_PID_MISS_LIMIT (5)
// VIN(Mode09) 재시도 상한과 페이싱. 매 폴 재시도하면 상한이 링크 확립 직후 수십 초에
// 소진돼(실측: 25초에 5회 전부 소모) 세션 내내 VIN 을 못 얻는다. 폴 몇 번에 한 번만
// 시도해 상한을 분 단위로 늘린다 — 8회 × (5+1)폴 × 5초 ≈ 4분에 걸쳐 시도.
#define OBD2_VIN_TRY_LIMIT      (8)
#define OBD2_VIN_RETRY_POLLS    (5)
// 폴 도중 연속 무응답이 이 횟수에 닿고 아직 한 건도 응답이 없으면 링크 끊김으로 보고 폴 중단.
// (시동 OFF 시 남은 PID 전부가 타임아웃 → 폴 1회가 loop를 1초 이상 멈추는 것을 막는다.)
#define OBD2_POLL_ABORT_MISSES  (3)
// 원시 CAN 프레임 시리얼 덤프(디버그). 1이면 요청(TX)/응답(RX)을 [CAN] 헥스로 출력.
#define OBD2_DUMP_RAW           (0)

// ===========================================================================
// 예약 핀 (베이스보드 정의와 중복이나 정책 명시) — 재정의 금지
//   SD: 2,13,14,15 / 모뎀 슬립 DTR: 25 / 원격 웨이크 RING: 33 / PSRAM: 16,17
// ===========================================================================
