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
// GNSS UART 속도 (모뎀 ↔ ESP32)
#define GPS_BAUDRATE                (115200)

// ===========================================================================
// LTE / MQTT 설정 (1단계)   설계: lte-mqtt-device-design.md
// ===========================================================================
// --- 증분 A: LTE 브링업 (서버 무관, LG U+ 유심 데이터패스 검증) ---
// LG U+ 표준 동적IP APN. 단말은 outbound 접속이라 정적 공인IP APN 불필요.
// M2M 요금제 유심이면 개통 후 AT+CGDCONT? 로 실제 값 확인해 교체할 것.
#define LTE_APN                     "internet.lguplus.co.kr"
#define LTE_GPRS_USER               ""
#define LTE_GPRS_PASS               ""
// 망 등록 대기 상한 (콜드 등록은 수십 초 소요 가능)
#define LTE_REG_TIMEOUT_MS          (90000UL)
// --- 모뎀 무응답 복구 -------------------------------------------------------
// 전원 마진이 부족하면 LTE 등록(최대 출력 송신) 중 전압이 내려앉아 모뎀이 내부 리셋되고,
// 그 뒤로 AT에 응답하지 않는다(CSQ=99, 등록상태 no-result). 이때 AT 재시도만 반복하면
// 회복 경로가 없어 영영 못 붙는다 → 아래 값으로 조기 감지 + 리셋 승격을 한다.
#define LTE_AT_PROBE_MS             (2000)      // 모뎀 생존 확인 AT 응답 대기
#define LTE_MODEM_RESET_WAIT_MS     (20000UL)   // 리셋 후 AT 재개 대기 상한
#define LTE_FAIL_BEFORE_RESET       (2)         // 연속 브링업 실패 몇 회에 모뎀 리셋
// 재브링업 간격 — 등록 시도는 모뎀이 최대 출력으로 송신하는 구간이라, 전원이 약하면
// 몰아칠수록 전압 강하가 심해진다. 실패마다 2배로 늘려 모뎀이 쉬는 시간을 준다.
#define LTE_RETRY_BASE_MS           (30000UL)
#define LTE_RETRY_CAP_MS            (300000UL)  // 5분
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
// Mqtt::handle의 URC 수신 상한. 매 틱 호출이므로 짧아야 한다(래퍼 기본값 100ms는
// 유휴 시에도 통째로 소모되어 루프 주기를 3배로 늘린다 — mqtt.cpp handle 주석 참고).
// UART에 바이트가 있을 때만 진입하므로, 한 URC를 마저 읽을 정도만 주면 충분하다.
#define MQTT_HANDLE_TIMEOUT_MS      (20)
#define MQTT_CA_FILENAME            "emqx_ca.pem"

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
#define FW_VERSION                  "0.2.0-ota"

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
#define CARKEY_PRESS_MS    (120)

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
#define OBD2_LINK_RETRY_MS      (30000UL) // 링크 미확립(응답0) 시 재초기화 시작 간격
// 재초기화 실패마다 간격 2배 → 이 값에서 상한. 주차(시동 OFF) 중엔 재시도가 몇 시간이고
// 이어지는데, begin 1회가 비트레이트 2종 무응답 타임아웃 + 드라이버 install/uninstall 을
// 수반해 수백 ms 블로킹이다. 시동을 걸면 늦어도 이 간격 안에 링크가 다시 잡힌다.
#define OBD2_LINK_RETRY_CAP_MS  (300000UL) // 5분
// 확장 PID(>0x20: 0x2F/0x42/0xA6) 미지원 래치 임계. 지원 마스크(0x00 응답)는 0x01~0x20만
// 커버해서 이들은 무조건 요청하게 되는데, 미지원 차량이면 매 폴 1건당 OBD2_REQ_TIMEOUT_MS를
// 순수 타임아웃으로 버린다. 연속 이 횟수만큼 무응답이면 링크 재확립 전까지 요청을 생략한다.
#define OBD2_EXT_PID_MISS_LIMIT (3)
// 폴 도중 연속 무응답이 이 횟수에 닿고 아직 한 건도 응답이 없으면 링크 끊김으로 보고 폴 중단.
// (시동 OFF 시 남은 PID 전부가 타임아웃 → 폴 1회가 loop를 1초 이상 멈추는 것을 막는다.)
#define OBD2_POLL_ABORT_MISSES  (3)
// 원시 CAN 프레임 시리얼 덤프(디버그). 1이면 요청(TX)/응답(RX)을 [CAN] 헥스로 출력.
#define OBD2_DUMP_RAW           (0)

// ===========================================================================
// 예약 핀 (베이스보드 정의와 중복이나 정책 명시) — 재정의 금지
//   SD: 2,13,14,15 / 모뎀 슬립 DTR: 25 / 원격 웨이크 RING: 33 / PSRAM: 16,17
// ===========================================================================
