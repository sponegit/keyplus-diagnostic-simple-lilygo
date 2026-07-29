# Keyplus Diagnostic 펌웨어 0.2.1 — OTA 배포 패키지

| 항목 | 값 |
|---|---|
| 버전 | **0.2.1** (`FW_VERSION`, `config.h`) |
| 보드 | LILYGO T-A7670E (ESP32-WROVER, Flash 4MB) |
| 파일 | `keyplus-diagnostic-0.2.1.bin` |
| 크기 | **383,920 bytes** |
| MD5 | `64fb30e80e34e706b933c25943baf857` |
| SHA-256 | `0869013deb0ae1ecc05ba2b2e55025faa8d665867a4bfb4b3cc8486ba7a6ad46` |
| OTA 슬롯 | 1,966,080 bytes (min_spiffs.csv, **19.5% 사용**) |

단말은 **MD5만** 검증합니다(`Update.setMD5`). SHA-256은 배포 경로 무결성 확인용입니다.

---

## 파일 구성

```
keyplus-diagnostic-0.2.1.bin       OTA로 내려보낼 이미지 (app 파티션 전용)
keyplus-diagnostic-0.2.1.bin.md5   MD5 한 줄 (배포 서버 검증용)
ota-manifest.json                  배포 메타 (버전/크기/해시/변경점) — url 채워서 사용
ota_start.json                     MQTT 명령 템플릿 — url/command_id 채워서 발행
```

> ⚠️ 이 `.bin`은 **app 이미지**입니다. USB로 처음 굽는 `0x0` 통합 이미지가 아닙니다.
> 시리얼 최초 플래싱은 `pio run -t upload`을 쓰세요.

---

## 배포 절차

1. `keyplus-diagnostic-0.2.1.bin`을 HTTP(S)로 접근 가능한 곳에 올린다.
2. `ota-manifest.json`의 `url`을 실제 주소로 채워 배포 기록으로 남긴다.
3. `ota_start.json`의 `command_id`(UUID)와 `url`을 채운다.
4. 대상 단말의 **`v1/{device_id}/cmd`** 토픽으로 발행한다 (QoS 1).
   예: `v1/vt-2607-0005-6jn/cmd`
5. 단말이 다운로드 → 플래시 → 재부팅한다.
6. 재부팅 후 MQTT 재접속되면 **`v1/{device_id}/cmd/ack`** 로 결과가 올라온다.

---

## ⚠️ 반드시 지킬 제약

### 1. 명령 JSON은 **255 바이트 미만**이어야 한다

수신 버퍼가 `char s_payload[256]`이고, 넘치면 **경고 없이 잘립니다**
(`cmd.cpp` `onMessage`). 잘리면 뒤쪽 필드(보통 `version`, `expires_at`)를 못 찾아
`url`이 빈 값이 되고 `[OTA] 잘못된 URL`로 실패합니다.

`ota_start.json` 템플릿은 URL을 뺀 뼈대만 **약 150 바이트**입니다.
→ **URL은 100자 이내**로 유지하세요. 짧은 경로(`/fw/kpd-0.2.1.bin`)를 권장합니다.

발행 전 확인:
```bash
python3 -c "import json,sys;d=open('ota_start.json','rb').read();print(len(json.dumps(json.loads(d),separators=(',',':'))),'bytes')"
```

### 2. `version`은 `0.2.1`과 **정확히** 일치해야 한다

재부팅 후 실행 이미지의 `FW_VERSION`과 대조해 ack를 결정합니다(`ota.cpp`).
불일치하면 실제로는 성공했어도 **`failed`로 ack**가 나갑니다.

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
0.2.1에서 브라운아웃이 관측된 개체라면 전원 보강 후에 진행하는 게 안전합니다.

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

USB-C 또는 보조 UART(TX=GPIO13 / RX=GPIO34, 115200)에서:

```
[OTA] 시작 cmd=... ver=0.2.1
  url=https://.../kpd-0.2.1.bin
  md5=64fb30e80e34e706b933c25943baf857
[OTA] 펌웨어 크기 383920 bytes — 플래시 시작
[OTA] 10% (38392/383920)
...
[OTA] 플래시 완료·검증 통과 — 재부팅 예약
```

재부팅 후:

```
============================================================
  Keyplus Diagnostic   fw 0.2.1
  reset      : SW(소프트 리셋/재부팅)
============================================================
[OTA] 새 이미지 확정(mark_valid) ... — 롤백 취소
[OTA] pending ack: cmd=... 기대=0.2.1 실행=0.2.1 → done
```

`info` 명령으로도 버전을 바로 확인할 수 있습니다.

---

## 배포 전 체크리스트

- [ ] 업로드한 파일의 MD5가 위 값과 일치 (`md5 -q <file>`)
- [ ] URL이 단말에서 접근 가능 (사설망/방화벽 확인)
- [ ] `ota_start.json` 직렬화 길이 **< 255 bytes**
- [ ] `version` 필드가 `0.2.1`
- [ ] 대상 단말이 **`cmd` 구독 성공** 상태 — `status server`에서 `[ok]` 확인
      (0.2.1 이전 이미지는 구독 실패 버그가 있어 명령 자체를 못 받습니다)
- [ ] 단말 전원 안정 (브라운아웃 이력 없음 — `info`의 `reset` 확인)
- [ ] 1대 선행 배포 후 확대

> **0.2.0 → 0.2.1 OTA 주의**: 0.2.0에는 cmd 구독 실패 버그가 있어
> `ota_start` 명령이 단말에 **도달하지 않을 수 있습니다.** 구독이 안 되는 개체는
> 시리얼로 0.2.1을 먼저 구운 뒤, 이후 버전부터 OTA를 사용하세요.
