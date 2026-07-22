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
#define FEATURE_OBD2        0   // 4단계: CAN/OBD2 PID (외장 트랜시버 필요)
#define FEATURE_CARKEY      1   // 5b단계: 차키 GPIO 토글 (열림/잠금만, 전원 제어 없음) [활성]
#define FEATURE_BLE         0   // 7단계: BLE 명령/OTA
#define FEATURE_LTE         1   // 1단계: LTE+MQTT (LG U+ 유심)     [활성 — 증분 A]
#define FEATURE_STATUS_LED  1   // 3단계: 상태표시 LED (외장, GPIO23) [활성]

// ===========================================================================
// GPS 설정 (2단계)
// ===========================================================================
// 위치 폴링 주기. 주차 중 배터리 방전 방지를 위해 과도한 폴링은 피한다.
#define GPS_POLL_INTERVAL_MS        (10000UL)
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
#define MQTT_PUBLISH_INTERVAL_MS    (30000UL) // telemetry 발행 주기(= NAT keep-alive 역할)
#define MQTT_RECONNECT_CAP_MS       (15000UL) // 재접속 백오프 상한 (최악도 20초 내 수렴)
#define MQTT_CA_FILENAME            "emqx_ca.pem"

// ── 브로커 선택 ─────────────────────────────────────────────────────────
// EMQX 기동 전엔 공개 테스트 브로커로 검증. 로컬 EMQX가 뜨면 아래 MQTT_BROKER_SEL
// 한 줄만 LOCAL로 바꾸면 호스트/포트/TLS/인증/CA가 자동 선택된다(코드 불변).
#define MQTT_BROKER_TEST     1   // test.mosquitto.org (공개 TLS, 익명) — 검증됨
#define MQTT_BROKER_LOCAL    2   // 로컬 EMQX (기동 중 — 아래 TODO 값 확정 필요)
#define MQTT_BROKER_PROD     3   // 실 EMQX (mqtt.keyplus.sponeinfra.com, 상용)

#define MQTT_BROKER_SEL      MQTT_BROKER_LOCAL  // ← 로컬 EMQX 테스트 중(61.81.117.174:28883 TLS)

#if   MQTT_BROKER_SEL == MQTT_BROKER_TEST
  #define MQTT_HOST          "test.mosquitto.org"
  #define MQTT_PORT          (8883)
  #define MQTT_USE_TLS       1     // TLS 접속(서버 CA 검증)
  #define MQTT_ANON          1     // 익명(비번 미검증)
  #define MQTT_TLS_VERIFY    1     // 서버 인증서 CA 검증(authmode=1)
  #define MQTT_TLS_RELAX     0     // 정식 cert(호스트명 일치) → 완화 불필요
#elif MQTT_BROKER_SEL == MQTT_BROKER_LOCAL
  // 로컬 EMQX (공인IP:포워딩포트, TLS 자체서명 = certs.h CA_LOCAL_EMQX)
  #define MQTT_HOST          "61.81.117.174"  // 로컬 EMQX 공인 IP(gateway 28081과 동일 호스트)
  #define MQTT_PORT          (28883)          // 내부 8883 → 포워딩 28883 (TLS)
  #define MQTT_USE_TLS       1                // TLS 접속
  #define MQTT_ANON          0                // device_id(username) + 발급 mqtt_pw 인증
  #define MQTT_TLS_VERIFY    1                // CA 검증(authmode=1) — 래퍼가 authmode=0 미지원
  #define MQTT_TLS_RELAX     1                // 자체서명+IP접속: CN·시각 검증 완화(CA 검증은 유지)
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
// 로컬 device-gateway(포워딩된 공인 IP:포트). LTE 단말이 외부에서 닿는 공개 주소여야 함.
// ⚠️ 현재 로컬은 평문 HTTP(TLS 아님) → PROVISION_USE_TLS=0. 상용 전환 시 도메인+443+CA로 교체.
#define PROVISION_HOST              "61.81.117.174"
#define PROVISION_PORT              (28081)
#define PROVISION_PATH              "/internal/provision"
#define PROVISION_USE_TLS           0     // 0=평문 HTTP(로컬), 1=HTTPS(상용, certs.h CA 필요)
// 오프라인(서버 없이) 테스트용: 1이면 NVS 비었을 때 아래 DEVICE_ID로 시드(자동 발급 생략).
// 기본 0 — NVS 비면 미프로비저닝 상태로 두고 부팅 시 /internal/provision 발급을 시도.
#define PROVISION_SEED_FALLBACK     0

// 단말 신원/버전 — TODO(provisioning): NVS 프로비저닝으로 이관. 지금은 검증용 상수.
// 포맷 vt-YYMM-NNNN-XXX (계약 @keyplus-diagnostic/shared deviceId.ts 와 동일).
#define DEVICE_ID                   "vt-2607-0001-x7q"
#define FW_VERSION                  "0.1.0-incrB"

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
#define CARKEY_DRIVE_SEL      CARKEY_DRIVE_DIRECT   // ← 현재: TR 미준비 → 직결(LOW=누름)

// ===========================================================================
// OBD2 CAN 핀맵 (4단계 — 외부 CAN 트랜시버 SN65HVD230 등 추가 필요)  [현재 미사용]
// ===========================================================================
#define PIN_CAN_RX          (21)
#define PIN_CAN_TX          (22)

// ===========================================================================
// 예약 핀 (베이스보드 정의와 중복이나 정책 명시) — 재정의 금지
//   SD: 2,13,14,15 / 모뎀 슬립 DTR: 25 / 원격 웨이크 RING: 33 / PSRAM: 16,17
// ===========================================================================
