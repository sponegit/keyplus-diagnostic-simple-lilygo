# Keyplus Diagnostic 펌웨어 0.2.7 — OTA 배포 패키지

| 항목 | 값 |
|---|---|
| 버전 | **0.2.7** (`FW_VERSION`, `config.h`) |
| 보드 | LILYGO T-A7670E (ESP32-WROVER, Flash 4MB) |
| 파일 | `keyplus-diagnostic-0.2.7.bin` |
| 크기 | **386,288 bytes** |
| MD5 | `ed5405cf669cdbb96189bf0ed41b8e2a` |
| SHA-256 | `f900561f54e7e3e9f20f72da326628c110330cfd404f2e0f504b1b1b6346f9ce` |
| OTA 슬롯 | 1,966,080 bytes (min_spiffs.csv, **19.6% 사용**) |

단말은 **MD5만** 검증합니다(`Update.setMD5`). SHA-256은 배포 경로 무결성 확인용입니다.

> ⚠️ **0.2.6 은 쓰지 마세요.** 이미지 안의 `FW_VERSION` 이 `0.2.5` 로 남아 있어(버전
> 상수 갱신 누락), OTA 로 내리면 플래시는 성공해도 재부팅 후 버전 대조에서 어긋나
> **ack 가 `failed` 로 나갑니다.** 0.2.7 이 이를 포함해 바로잡은 이미지입니다.

## 0.2.6 대비 변경 (실기 로그에서 확인)

- **유심 없이 부팅하면 단말이 통째로 멈추던 문제 수정.** 펌웨어 업로드 직후 유심 없는
  상태로 부팅하면 `MODEM STRING NO FOUND!` 가 12초마다 무한 반복되고 그 뒤가 진행되지
  않았습니다. 원인은 유심 없음 자체가 아니라 **모델명(ATI) 조회를 성공할 때까지
  무한 재시도**하던 구조입니다 — 탈출 조건도 회복 경로도 없어 `setup()` 이 끝나지 않았고,
  콘솔·OBD2·차키·LED 까지 같이 죽었습니다.
  (실측 로그: `+CPIN: SIM REMOVED` → `+CME ERROR: AT command timeout` → 이후 ATI 무응답)

  ```
  [21964] ### Unhandled: +CPIN: SIM REMOVED
  +CME ERROR: AT command timeout
  [21964] MODEM STRING NO FOUND!
  [33965] MODEM STRING NO FOUND!      ← 12초 주기로 영원히
  ```

- 모델명 조회 **3회** 실패 → **하드 리셋 1회**로 회복 시도 → 그래도 실패하면
  `UNKNOWN` 인 채 부팅을 계속합니다. `A7670G`(내장 GPS 없음)로 **확정된** 경우에만
  기존처럼 정지합니다.
- **AT 준비 대기에 90초 상한.** 모뎀이 끝내 안 떠도 GPS/LTE 브링업만 건너뛰고
  콘솔·OBD2·차키는 정상 부팅합니다. 모뎀 회복은 loop 의 재브링업/리셋 경로가 계속 시도합니다.
- **GNSS 활성화 재시도 3회 제한** — 실패해도 부팅이 멈추지 않습니다.
- **유심 없음/PIN 잠김을 등록 전에 감지.** `Lte::begin()` 이 `+CPIN?` 을 먼저 확인해,
  90초 망 등록 폴링을 통째로 버리는 대신 즉시 실패하고 원인을 알립니다.

  ```
  [LTE] SIM 미인식 — 유심 삽입/접촉 확인. 등록 시도 생략
  [LTE] SIM 잠김(PIN) — simUnlock 필요. 등록 시도 생략
  ```

  부팅 직후 CPIN 이 잠깐 `NOT READY` 인 경우는 3회 재시도로 흡수하므로,
  정상 유심이 오탐으로 막히지 않습니다.
- `FW_VERSION` 을 `0.2.5` → `0.2.7` 로 바로잡았습니다(위 경고 참고).

### 검증 범위

빌드 성공(Flash 19.6%), `FEATURE_GPS`/`FEATURE_LTE` 4가지 조합 모두 컴파일 확인,
클린 빌드 MD5 재현 확인. **온디바이스 검증은 아직입니다** — 유심을 뺀 상태로 플래시해
아래가 나오면 이 릴리스의 목적은 달성입니다:

- `[LTE] SIM 미인식 …` 이 뜨고 부팅이 끝까지 진행됨
- 부팅 배너·`info`·`status` 응답이 정상
- OBD2 폴링이 계속 돎(`[STAT]` 한 줄이 주기적으로 나옴)

## 0.2.5 대비 변경 (0.2.6)

- OBD2 링크 재확립을 **30초 고정 간격**으로 되돌렸습니다(지수 백오프 → 상한 5분 제거).
  주차 중 백오프가 상한에 닿으면 시동을 걸어도 최대 5분을 기다려야 했습니다.

## 그 이전 변경

`release/0.2.5/README.md` 의 "0.2.4 대비 변경" 이하를 참고하세요.

---

## 파일 구성

```
keyplus-diagnostic-0.2.7.bin       OTA로 내려보낼 이미지 (app 파티션 전용)
keyplus-diagnostic-0.2.7.bin.md5   MD5 한 줄 (배포 서버 검증용)
ota-manifest.json                  배포 메타 (버전/크기/해시/변경점) — url 채워서 사용
ota_start.json                     MQTT 명령 템플릿 — url/command_id 채워서 발행
```

> ⚠️ 이 `.bin`은 **app 이미지**입니다. USB로 처음 굽는 `0x0` 통합 이미지가 아닙니다.
> 시리얼 최초 플래싱은 `pio run -t upload`을 쓰세요.

---

## 배포 절차

1. `keyplus-diagnostic-0.2.7.bin`을 HTTP(S)로 접근 가능한 곳에 올린다.
2. `ota-manifest.json`의 `url`을 실제 주소로 채워 배포 기록으로 남긴다.
3. `ota_start.json`의 `command_id`(UUID)와 `url`을 채운다.
4. 대상 단말의 **`v1/{device_id}/cmd`** 토픽으로 발행한다 (QoS 1).
   예: `v1/vt-2607-0005-6jn/cmd`
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

### 2. `version`은 `0.2.7`과 **정확히** 일치해야 한다

재부팅 후 실행 이미지의 `FW_VERSION`과 대조해 ack를 결정합니다(`ota.cpp`).
불일치하면 실제로는 성공했어도 **`failed`로 ack**가 나갑니다.
(0.2.6 이미지가 바로 이 경우라 배포 대상에서 제외했습니다.)

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

> 참고: 0.2.7의 부팅 견고화(무응답 모뎀에도 setup 완료)는 **OTA 확정과는 무관**합니다.
> 확정 조건은 여전히 "10분 내 MQTT 접속"이라, 유심이 없거나 전파가 없으면
> 새 이미지는 롤백됩니다. 유심 없는 상태의 검증은 **시리얼 플래싱**으로 하세요.

---

## 발행 예시

```bash
mosquitto_pub -h mqtt-dev.keyplus.sponeinfra.com -p 40102 \
  --cafile ca.pem \
  -u vt-2607-0005-6jn -P '<mqtt_pw>' \
  -t 'v1/vt-2607-0005-6jn/cmd' -q 1 \
  -f ota_start.json
```

### 진행 확인 (콘솔)

USB-C 또는 보조 UART(UART2, TX=GPIO13 / RX=GPIO34, 115200)에서:

```
[OTA] 시작 cmd=... ver=0.2.7
  url=https://.../kpd-0.2.7.bin
  md5=ed5405cf669cdbb96189bf0ed41b8e2a
[OTA] 펌웨어 크기 386288 bytes — 플래시 시작
[OTA] 10% (38628/386288)
...
[OTA] 플래시 완료·검증 통과 — 재부팅 예약
```

재부팅 후:

```
============================================================
  Keyplus Diagnostic   fw 0.2.7
  reset      : SW(소프트 리셋/재부팅)
============================================================
[OTA] 새 이미지 확정(mark_valid) ... — 롤백 취소
[OTA] pending ack: cmd=... 기대=0.2.7 실행=0.2.7 → done
```

`info` 명령으로도 버전을 바로 확인할 수 있습니다.

---

## 배포 전 체크리스트

- [ ] 업로드한 파일의 MD5가 위 값과 일치 (`md5 -q <file>`)
- [ ] URL이 단말에서 접근 가능 (사설망/방화벽 확인)
- [ ] `ota_start.json` 직렬화 길이 **< 1,000 bytes**
- [ ] `version` 필드가 `0.2.7`
- [ ] 대상 단말이 **`cmd` 구독 성공** 상태 — `status server`에서 `[ok]` 확인
- [ ] 단말 전원 안정 (브라운아웃 이력 없음 — `info`의 `reset` 확인)
- [ ] 1대 선행 배포 후 확대

> **0.2.6 / 0.2.1 은 사용하지 마세요.** 0.2.6은 `FW_VERSION` 누락으로 ack가 항상
> `failed`, 0.2.1은 모뎀 리셋 후 MQTT 영구 접속불가 결함이 있습니다.
