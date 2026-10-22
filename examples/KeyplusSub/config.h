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

// ---------------------------------------------------------------------------
// 차키 fob — 오픈드레인, 직렬 1kΩ. ⚠️ 부팅 시 하이임피던스 유지(내부 풀 금지)
// ---------------------------------------------------------------------------
#define PIN_KEY_LOCK                4
#define PIN_KEY_UNLOCK              5
#define PIN_KEY_TRUNK               6
#define PIN_KEY_START               7
#define PIN_KEY_OPT1                8
#define PIN_KEY_OPT2                9

#define PIN_STATUS_LED              10       // 서브 전용 외장 LED

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
#define PIN_PMS_SET                 47

// ---------------------------------------------------------------------------
// 전원 레일 게이트 — HIGH = ON. 외부 10kΩ 풀다운 전제, 부팅 기본값은 전부 OFF
// ---------------------------------------------------------------------------
#define PIN_RAIL_GNSS               38
#define PIN_RAIL_PMS_5V             39
#define PIN_RAIL_SENSOR_3V3         40
#define PIN_RAIL_CO_5V              41       // 주차 중에도 ON
