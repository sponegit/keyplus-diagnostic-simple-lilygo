# Keyplus Diagnostic 펌웨어 0.2.11 — OTA 배포 패키지

| 항목 | 값 |
|---|---|
| 버전 | **0.2.11** (`FW_VERSION`, `config.h`) |
| 보드 | LILYGO T-A7670E (ESP32-WROVER, Flash 4MB) |
| 파일 | `keyplus-diagnostic-0.2.11.bin` |
| 크기 | **390,576 bytes** |
| MD5 | `31adebabb83e94ebffa51f671ed426c9` |
| SHA-256 | `42f856b8a69475a4bf37753a6349567afc771eed226cdfb65048960b4d0d8489` |
| OTA 슬롯 | 1,966,080 bytes (min_spiffs.csv, **19.9% 사용**) |

단말은 **MD5만** 검증합니다(`Update.setMD5`). SHA-256은 배포 경로 무결성 확인용입니다.

0.2.10 대비 **telemetry 유실 수정 한 건**만 담긴 소규모 릴리스입니다.

> ⚠️ **0.2.6 은 쓰지 마세요.** 이미지 안의 `FW_VERSION` 이 `0.2.5` 로 남아 있어 플래시가
> 성공해도 ack 가 `failed` 로 나갑니다(실기 확인).

---

## 주행 구간 telemetry 가 서버에서 사라지던 문제

**증상.** 차량 설치 후 시운전을 했는데 그 구간의 속도·RPM 등 OBD2 값이 서버에 없습니다.
단말 쪽 로그에는 `pub-ok` 만 찍혀 있어 발행은 정상으로 보입니다.

**단말은 정상적으로 보냈습니다.** `g_seq` 는 발행 성공(`pubOk`)일 때만 증가하므로,
서버에 들어온 행의 seq 가 건너뛴다면 그건 "보내지 못한 것"이 아니라 **보냈는데 서버에
안 남은 것**입니다. 2026-07-30 실측(`vt-2607-0003-kfo`):

| 세션 | 발행(seq) | 서버 행 | 유실 |
|---|---|---|---|
| 09:25 ~ 19:21 | 1,043 | 820 | **223건 (21%)** |
| 19:42 ~ 익일 06:18 | 1,227 | 1,127 | **100건 (8%)** |
| 합계 | 2,270 | 1,947 | **323건 (14%)** |

19:42 세션의 소실 분포:

| 소실 구간 | 시각 | 건수 |
|---|---|---|
| seq 3–36 | 19:42:40 → 20:16:06 | **34건 (33분)** ← 설치 직후 시운전 전체 |
| seq 42–50 | 20:18:06 → 20:24:19 | 9건 |
| seq 64–66 / 69–92 / 103–132 | 20:30 → 21:11 | 57건 |
| seq 133–1226 (주차 중) | 21:11 → 06:18 | **0건** |

### 원인 — 캐시된 GPS fix 의 UTC 를 `ts` 로 썼다

```
pollGps()                       측위 실패 시 g_lastFix 를 갱신하지 않는다(마지막 값 유지)
   ↓
publishTelemetry(..., g_lastFix, ...)
   ↓
if (fix.valid) ts = civilToEpoch(fix.year..second)     ← fix.valid 는 캐시값이라 계속 true
```

`fix.valid` 는 **측위를 놓쳐도 true 로 남는 캐시**입니다. 그래서

- 바로 아래 있던 `modemEpoch()` 폴백이 **한 번도 실행된 적이 없고**,
- 측위를 놓친 동안 발행되는 모든 패킷이 **같은 `ts`** 를 달고 나가고,
- 서버 PK 가 `(device_id, ts)` 라 그 구간이 **한 행으로 붕괴**합니다.

`ts=0` 이 반복될 때 같은 일이 벌어진다는 경고 주석은 이미 코드에 있었는데, 실제로는
"얼어붙은 GPS 시각"으로 같은 함정에 빠졌습니다.

**직접 증거.** 각 행의 `ts − up_s`(= 계산상 부팅 시각)는 세션 내내 상수여야 합니다.
19:42 세션에서 이 값은 전 구간 `1785440489` 인데, **대형 소실 구간의 시작 행 seq=2 만
`1785440350`(142초 뒤처짐)** 입니다 — 발행 시점에 142초 묵은 캐시 fix 로 ts 를 만들었다는
뜻이고, 그 뒤 34건이 같은 ts 로 나가 전부 지워졌습니다. 소실 구간 5개 모두
"fresh ts 행 → 갭 → fresh ts 행" 구조로 끝나고, 측위가 안정된 야간 주차 구간
(1,094건, 위성 15~18개)은 유실이 **0** 입니다.

### 수정

- **ts 판정 기준을 `fix.valid` → `fixFresh` 로.** loop 의 `g_gpsFixNow`(= 방금 폴에서
  실제로 측위됐는가)를 `publishTelemetry` 로 넘깁니다. 측위 중이면 GPS UTC(권위),
  아니면 모뎀 UTC(`+CCLK`) 폴백 — 폴백이 이제 실제로 동작합니다.
- **ts 단조 증가 가드(폴백 경로 한정).** 모뎀 시각이 미동기(0)이거나 NITZ 재동기로
  뒷걸음질하면 PK 가 다시 겹칩니다. 직전 ts 에 `up_s` 경과분을 더해 밀어냅니다.
  측위 중 GPS UTC 에는 이 보정을 태우지 않습니다 — 모뎀 시각 오차가 latch 되면
  이후 진짜 GPS 시각이 영영 밀립니다.

```c
uint32_t ts = 0;
bool authoritative = false;
if (fixFresh && fix.valid && fix.year >= 2020) {
    ts = civilToEpoch(fix.year, fix.month, fix.day, fix.hour, fix.minute, fix.second);
    authoritative = true;
} else {
    ts = modemEpoch(modem);
}
static uint32_t s_lastTs = 0, s_lastTsUp = 0;
if (!authoritative && s_lastTs && ts <= s_lastTs) {
    uint32_t adv = up_s - s_lastTsUp;
    ts = s_lastTs + (adv ? adv : 1);
}
if (ts) { s_lastTs = ts; s_lastTsUp = up_s; }
```

변경 파일: `mqtt.cpp` / `mqtt.h` / `KeyplusDiagnostic.ino` / `config.h`(버전).

### 배포 후 확인 방법

측위를 놓친 구간(터널·지하·실내)을 지난 뒤 서버에서 **seq 연속성**을 봅니다.
`ts` 는 30초 간격으로 계속 전진해야 하고, seq 는 건너뛰지 않아야 합니다.

```sql
-- 세션 내 seq 갭 = 유실. 수정 후 0 이어야 한다.
SELECT max(seq) + 1 - count(*) AS lost FROM telemetry
 WHERE device_id = 'vt-2607-0003-kfo' AND ts >= '<부팅시각>';
```

---

## OBD2 는 정상입니다 (0.2.10 수정 확인)

같은 로그로 대조했고 **파싱 문제는 없었습니다.**

| 항목 | 확인 |
|---|---|
| `rpm: 16384`(0xFFFF/4) 오디코드 | 0.2.8 세션(08:12)엔 있었고 **0.2.10 세션엔 없음** → FF 필터 정상 |
| VIN | `KMHR281ABRU663944` 전 구간 일관 |
| 오도미터(0xA6) | 33,117.4 → 33,125.0 km (7.6km 주행분과 일치) |
| 20:24~20:30 `obd` 블록 누락 | **시동 OFF 구간** — 직전 `ctrl_v` 12.48V(배터리), 직후 20:32 `runtime` 99초(재시동). 링크 끊김 처리가 의도대로 동작 |
| `obd.speed=33` vs `gps.spd=0` | GPS spd 가 노트 단위·정지 노이즈. **속도는 OBD PID 0x0D 가 권위** |

---

## 이 릴리스에 담기지 않은 것 (기지 사항)

- **미측위 중에도 `gps.fix=true` + 마지막 좌표가 계속 발행됩니다.** 서버가 터널/음영을
  구분할 수 없습니다. `age_s` 나 실시간 fix 상태 필드를 넣으려면 서버 계약 변경이
  필요해 별건으로 둡니다. 이번 수정으로 **행 자체는 살아나므로**, 지도상으로는
  "그 자리에 머물다 점프"로 보입니다(이전에는 구간이 통째로 없었습니다).
- `gps.spd` 는 노트 단위 무변환(TinyGSM `getGPS`) — km/h 대비 1.852배 작습니다.
- OBD2 링크가 끊기면 `vehicleMoving()=false` 라 주행 중에도 GPS 주기 폴이 60초로
  늘어납니다. 발행 직전 폴이 따로 있어 telemetry 건수엔 영향이 없고 궤적 해상도만
  떨어집니다.

## 그 이전 변경

`release/0.2.10/README.md`(OBD2 응답 ECU 고정 + GPS 첫 fix 15분→27초),
`release/0.2.8/README.md`(모뎀 복구 지연 2분30초→30초), `release/0.2.7/README.md`
(무유심 부팅 멈춤) 이하를 참고하세요.

---

## 파일 구성

```
keyplus-diagnostic-0.2.11.bin       OTA로 내려보낼 이미지 (app 파티션 전용)
keyplus-diagnostic-0.2.11.bin.md5   MD5 한 줄 (배포 서버 검증용)
ota-manifest.json                   배포 메타 (버전/크기/해시/변경점) — url 채워서 사용
ota_start.json                      MQTT 명령 템플릿 — url/command_id 채워서 발행
```

> ⚠️ 이 `.bin`은 **app 이미지**입니다. USB로 처음 굽는 `0x0` 통합 이미지가 아닙니다.
> 시리얼 최초 플래싱은 `pio run -t upload`을 쓰세요.

---

## 배포 절차

1. `keyplus-diagnostic-0.2.11.bin`을 HTTP(S)로 접근 가능한 곳에 올린다.
2. `ota-manifest.json`의 `url`을 실제 주소로 채워 배포 기록으로 남긴다.
3. `ota_start.json`의 `command_id`(UUID)와 `url`을 채운다.
4. 대상 단말의 **`v1/{device_id}/cmd`** 토픽으로 발행한다 (QoS 1).
   예: `v1/vt-2607-0003-kfo/cmd`
5. 단말이 다운로드 → 플래시 → 재부팅한다.
6. 재부팅 후 MQTT 재접속되면 **`v1/{device_id}/cmd/ack`** 로 결과가 올라온다.

---

## ⚠️ 반드시 지킬 제약

### 1. 명령 JSON은 **약 1,000 바이트 미만**이어야 한다

래퍼(`TinyGsmMqttA76xx`)가 토픽과 페이로드를 **하나의 버퍼에** 나눠 담기 때문에,
토픽 길이만큼 페이로드 여유가 줄어듭니다(1024B − 토픽 24B ≈ 1,000B). 넘치면
**경고 없이 잘리고**, 잘린 JSON은 `url`이 빈 값이 되어 `[OTA] 잘못된 URL`로 실패합니다.

발행 전 확인:
```bash
python3 -c "import json;print(len(json.dumps(json.load(open('ota_start.json')),separators=(',',':'))),'bytes')"
```

### 2. `version`은 `0.2.11`과 **정확히** 일치해야 한다

재부팅 후 실행 이미지의 `FW_VERSION`과 대조해 ack를 결정합니다(`ota.cpp`).
불일치하면 실제로는 성공했어도 **`failed`로 ack**가 나갑니다.
**서빙하는 bin과 `version` 필드가 같은 릴리스인지 확인하세요** — 0.2.6 bin을
`version: 0.2.7`로 내려 ack가 failed로 나간 사례가 있습니다.

### 3. URL은 `http`로 시작해야 한다

`ota.cpp`가 `url.startsWith("http")`로 1차 검증합니다. HTTPS 권장.

### 4. 다운로드 중에는 MQTT가 끊긴다

모뎀의 SSL 컨텍스트를 HTTP(S)에 양보하려고 `Mqtt::stopService()`를 먼저 호출합니다.
그래서 ack는 즉시 나가지 않고 **재부팅 후 재접속 시점에 발행**됩니다(NVS pending).

### 5. 재부팅 후 10분 안에 MQTT에 붙어야 확정된다

새 이미지는 `PENDING_VERIFY` 상태로 부팅하고, MQTT 접속에 성공해야
`Ota::onHealthy()`가 확정(`mark_valid`)합니다.
`OTA_CONFIRM_DEADLINE_MS`(10분) 안에 못 붙으면 **자동 롤백 재부팅**됩니다.

→ 전파가 약한 곳이나 **전원이 불안정한 차량에서는 OTA를 피하세요.**

---

## 발행 예시

```bash
mosquitto_pub -h mqtt-dev.keyplus.sponeinfra.com -p 40102 \
  --cafile ca.pem \
  -u vt-2607-0003-kfo -P '<mqtt_pw>' \
  -t 'v1/vt-2607-0003-kfo/cmd' -q 1 \
  -f ota_start.json
```

### 진행 확인 (콘솔)

USB-C 또는 보조 UART(UART2, TX=GPIO13 / RX=GPIO34, 115200)에서:

```
[OTA] 시작 cmd=... ver=0.2.11
  url=https://.../kpd-0.2.11.bin
  md5=31adebabb83e94ebffa51f671ed426c9
[OTA] 펌웨어 크기 390576 bytes — 플래시 시작
[OTA] 10% (39057/390576)
...
[OTA] 플래시 완료·검증 통과 — 재부팅 예약
```

재부팅 후:

```
============================================================
  Keyplus Diagnostic   fw 0.2.11
  reset      : SW(소프트 리셋/재부팅)
============================================================
[OTA] 새 이미지 확정(mark_valid) ... — 롤백 취소
[OTA] pending ack: cmd=... 기대=0.2.11 실행=0.2.11 → done
```

`info` 명령으로도 버전을 바로 확인할 수 있습니다.

---

## 배포 전 체크리스트

- [ ] 업로드한 파일의 MD5가 위 값과 일치 (`md5 -q <file>`)
- [ ] 서빙하는 bin이 **0.2.11 릴리스의 것**인지
- [ ] URL이 단말에서 접근 가능 (사설망/방화벽 확인)
- [ ] `ota_start.json` 직렬화 길이 **< 1,000 bytes**
- [ ] `version` 필드가 `0.2.11`
- [ ] 대상 단말이 **`cmd` 구독 성공** 상태 — `status server`에서 `[ok]` 확인
- [ ] 단말 전원 안정 (브라운아웃 이력 없음 — `info`의 `reset` 확인)
- [ ] 1대 선행 배포 후 확대

> **0.2.6 / 0.2.1 은 사용하지 마세요.** 0.2.6은 `FW_VERSION` 누락으로 ack가 항상
> `failed`, 0.2.1은 모뎀 리셋 후 MQTT 영구 접속불가 결함이 있습니다.
