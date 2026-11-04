#pragma once
// ===========================================================================
// Keyplus v2 서브보드 (LilyGO T7-S3) 설정 — 핀맵은 HARDWARE.md §4 가 단일 출처
// ===========================================================================

#define FW_VERSION                  "0.1.0"
// 하드웨어 세대 — 서브 OTA 가드 기준. 메인("v2")과 다른 값이어야 메인 이미지가 서브로,
// 서브 이미지가 메인으로 잘못 들어가는 것을 막을 수 있다.
#define HW_MODEL                    "v2-sub"

// ---------------------------------------------------------------------------
// 메인 링크 — UART0 (콘솔은 USB-CDC 가 담당하므로 UART0 를 링크에 쓴다)
// ---------------------------------------------------------------------------
#define PIN_LINK_TX                 43
#define PIN_LINK_RX                 44
#define LINK_BAUD                   115200   // OTA 중에만 921600 으로 올린다
#define LINK_RX_BUF                 4096     // OTA 청크(1KB) 여러 개가 쌓여도 넘치지 않게
#define LINK_TX_BUF                 1024
// 링크 상대의 HW_MODEL. 다르면 연결은 하되 경고한다(배선·보드 확인).
#define LINK_PEER_HW                "v2"

// --- 서브 OTA (작업서 S5) ---------------------------------------------------
// OTA_BEGIN 이 baud 를 요청하면 ACK 를 보낸 뒤 전환한다. ACK 가 유실되면 메인은 옛 속도로 BEGIN 을
// 다시 보내므로, 전환 후 이 시간 안에 OTA 프레임이 안 오면 옛 속도로 되돌아가 기다린다.
#define OTA_BAUD_FALLBACK_MS        1500
// 새 이미지로 부팅한 뒤 이 시간 안에 메인과 링크가 안 올라오면 직전 이미지로 강제 롤백.
// 링크 업 = 헬스체크 통과 = 확정(mark_valid). 메인이 LTE 등록 중이면 setup 이 길어지므로 넉넉히.
#define OTA_CONFIRM_DEADLINE_MS     (300000UL)   // 5분

// ---------------------------------------------------------------------------
// 차키 fob — 오픈드레인, 직렬 1kΩ. ⚠️ 부팅 시 하이임피던스 유지(내부 풀 금지)
// ---------------------------------------------------------------------------
// 채널 번호(io 1~6) 순서. 버튼 배정은 설치마다 다르다(KpKey.h).
#define PIN_KEY_IO1                 4
#define PIN_KEY_IO2                 5
#define PIN_KEY_IO3                 6
#define PIN_KEY_IO4                 7
#define PIN_KEY_IO5                 8
#define PIN_KEY_IO6                 9

// 구동 방식 (v1 config.h 에서 이전) — 배선이 곧 극성이다. HARDWARE.md §5.3 은 DIRECT.
//   DIRECT = fob 패드 직결, 오픈드레인. LOW=누름 / Hi-Z=뗌. 공통 GND + fob 3.3V 상시급전 전제.
//            ⚠️ 반드시 오픈드레인 — HIGH 를 push 하면 꺼진 fob 에 역급전.
//   MOSFET = 2N7002 게이트 구동(푸시풀). HIGH=누름 / LOW=뗌. 직결과 극성이 반대다.
#define CARKEY_DRIVE_DIRECT         1
#define CARKEY_DRIVE_MOSFET         2
#define CARKEY_DRIVE_SEL            CARKEY_DRIVE_DIRECT
// 콘솔 'key <io>' 에서 ms 를 생략했을 때의 누름 시간. 상한·시퀀스 한도는 KpKey.h(KPKEY_*).
#define CARKEY_PRESS_MS             500

#define PIN_STATUS_LED              10       // 서브 전용 외장 LED (D4) — 지금은 차키 누름 중 점등
#define LED_ON_LEVEL                HIGH

// ---------------------------------------------------------------------------
// 센서 버스
// ---------------------------------------------------------------------------
#define PIN_CO_RX                   11       // ZE15-CO — SW serial
#define PIN_CO_TX                   12
#define PIN_I2C_SDA                 13       // 온보드 Qwiic — SHT41·SGP41·SCD41·BMP280·INA228
#define PIN_I2C_SCL                 14
#define PIN_GNSS_RX                 15       // MAX-M10S — UART1
#define PIN_GNSS_TX                 16
#define PIN_PMS_RX                  18       // PMS5003 — UART2
#define PIN_PMS_TX                  21
#define PIN_PMS_SET                 47       // HIGH·미연결 = 동작, LOW = 슬립
#define PMS_BAUD                    9600
#define PMS_WARMUP_MS               30000UL  // 깨운 뒤 팬·레이저 안정화(데이터시트 30초)
#define PMS_STALE_MS                5000UL   // 이 시간 프레임이 없으면 값 무효(active 모드 최대 간격 2.3초)
#define I2C_FREQ_HZ                 100000UL // 센서 5종 모두 100kHz 지원. 체인이 길어 400kHz 는 쓰지 않는다
#define I2C_TIMEOUT_MS              20       // 버스가 붙잡혀도 loop 워치독(5초)까지 가지 않게
#define AIR_CYCLE_MS                1000UL   // 센서 측정 주기
#define AIR_PROBE_MS                5000UL   // 없는 센서를 다시 찾는 주기(벤치에서 나중에 꽂아도 잡히게)

// ---------------------------------------------------------------------------
// 전원 레일 게이트 — HIGH = ON. 외부 10kΩ 풀다운 전제, 부팅 기본값은 전부 OFF
// ---------------------------------------------------------------------------
#define PIN_RAIL_GNSS               38
#define PIN_RAIL_PMS_5V             39
#define PIN_RAIL_SENSOR_3V3         40
#define PIN_RAIL_CO_5V              41       // 주차 중에도 ON
