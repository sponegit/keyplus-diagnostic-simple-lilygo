# 필요기능 ↔ 예제 매핑 (Feature Map)

LilyGo Modem 시리즈 `examples/` 예제를 기준으로, 프로젝트 필요기능별 매칭 예제를 정리한다.

## 통합 매핑표

| 필요기능 | 매칭 예제 | 상태 |
|---|---|---|
| **위치 조회** (단말기 위치) | [GPS_BuiltIn](GPS_BuiltIn/) / [GPS_BuiltInEx](GPS_BuiltInEx/) / [ModemGpsStream](ModemGpsStream/) / [LBSExample](LBSExample/) | ✅ 있음 |
| **AWS - LTE 통신 서버** | [MqttsBuiltlnAWS](MqttsBuiltlnAWS/) | ✅ 있음 |
| **OBD2 정보 조회** | [SimHatCanBusRecv](SimHatCanBusRecv/) / [SimHatCanBusSend](SimHatCanBusSend/) | ⚠️ CAN(TWAI)만, OBD2 PID 직접 구현 필요 |
| **펌웨어 OTA (LTE)** | [HttpsOTAUpgrade](HttpsOTAUpgrade/) | ✅ 있음 |
| **펌웨어 OTA (BLE)** | ❌ 저장소엔 없음 (ESP32 공식/커뮤니티 예제 이식 가능) | 신규 개발 — `ble-ota.md` 참조 |
| **차키 제어 (LTE)** | [MqttsBuiltlnAWS](MqttsBuiltlnAWS/) + GPIO 토글 | ✅ 명령채널 있음 / 동작은 GPIO |
| **차키 제어 (BLE)** | ❌ 없음 (동작 GPIO는 문서 완결) | BLE 채널만 신규 개발 |
| **정보 서버 전송** (위치·OBD 데이터) | [HttpsBuiltlnPost](HttpsBuiltlnPost/) / [Traccar](Traccar/) | ✅ 있음 |

## 세부 설명

### 위치 조회
- 내장 GPS 좌표 조회: [GPS_BuiltIn](GPS_BuiltIn/), [GPS_BuiltInEx](GPS_BuiltInEx/), [ModemGpsStream](ModemGpsStream/)
- GPS 미수신(실내/음영) 보완: [LBSExample](LBSExample/) — 기지국 기반 위치

### AWS - LTE 통신 서버
- [MqttsBuiltlnAWS](MqttsBuiltlnAWS/): 모뎀 내장 SSL로 AWS IoT Core에 MQTTS 직접 접속. LTE 상에서 가장 효율적인 AWS 접근 방식.

### OBD2 정보 조회
- ⚠️ 순수 OBD2 예제는 **없음**. [SimHatCanBusRecv](SimHatCanBusRecv/) / [SimHatCanBusSend](SimHatCanBusSend/)의 CAN(TWAI) 송수신 위에 OBD2 PID 요청/응답(0x7DF 요청 / 0x7E8 응답) 프로토콜을 직접 구현해야 함.
- 별도 CAN-RS485 확장보드 필요.

### 펌웨어 OTA
- LTE: [HttpsOTAUpgrade](HttpsOTAUpgrade/) — HTTPS로 펌웨어 다운로드 후 OTA 적용.
- BLE: ❌ 저장소엔 예제 없으나, ESP32 공식 컴포넌트(`espressif/ble_ota`) 및 커뮤니티 예제(fbiego 등) 이식으로 구현 가능. 상세: `car-key-control.md`와 동일 폴더의 `ble-ota.md` 참조.

### 차키 제어
차키 제어 = **①명령 수신 채널(BLE/LTE)** + **②GPIO 토글 실행** 으로 분리된다.
- **② GPIO 토글(실제 차키 동작)**: 예제 불필요. `car-key-control.md`의 `OUTPUT_OPEN_DRAIN` 코드가 완결. GPIO 출력 제어 참고용으로만 [SimHatRelay](SimHatRelay/).
- **① 명령 수신 - LTE**: [MqttsBuiltlnAWS](MqttsBuiltlnAWS/) / [MqttClient](MqttClient/) — MQTT subscribe 콜백에서 GPIO 토글 호출. AWS 서버 설계와 동일 예제로 통합 가능.
- **① 명령 수신 - BLE**: ❌ 예제 없음. ESP32 `BLEDevice`(또는 NimBLE) characteristic write 콜백 → GPIO 토글로 신규 구현.

> 참고: 차키 제어 상세 로직 — `keyplus-docs/keyplus-project/260706-simple-obd2-diagnostic/car-key-control.md`

### 정보 서버 전송
- HTTPS POST 전송: [HttpsBuiltlnPost](HttpsBuiltlnPost/), [HttpsBuiltlnPostGetFromMySQL](HttpsBuiltlnPostGetFromMySQL/), [HttpsBuiltlnPostSupabase](HttpsBuiltlnPostSupabase/)
- 위치 전송 특화: [Traccar](Traccar/) — GPS 트래킹 서버 연동.

## 신규 개발 필요 항목 (요약)

1. **BLE 스택 전반** — BLE 펌웨어 OTA + BLE 차키 명령 채널. 저장소에 BLE 예제가 하나도 없음.
2. **OBD2 PID 프로토콜** — CAN 물리계층 예제 위에 OBD2 요청/응답 로직 추가.
3. **차키 GPIO 토글** — 이미 `car-key-control.md`에 코드 존재, 예제 없이 바로 적용 가능.
