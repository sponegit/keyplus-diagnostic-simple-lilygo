# Keyplus Diagnostic 펌웨어 0.3.8 — OTA 배포 패키지

> 이전 릴리스: `release/0.3.7/` (통신사 자동 판별)
> **0.3.7 은 실기에 내려가지 못했습니다 — 현장 단말은 0.3.5 입니다.**
> 그 배포를 막은 것이 바로 이 릴리스가 고치는 버그입니다. 0.3.7 패키지는 폐기하고 이것을 쓰세요.

| 항목 | 값 |
|---|---|
| 파일 | `keyplus-diagnostic-0.3.8.bin` |
| 크기 | 425,776 B |
| MD5 | `0f74cbdd93ac18b2befb47d5dd507438` |
| SHA256 | `e724aacb3c734bba5e8055bfad566f18378cac9d67c9e4b3e37e468f767407f5` |
| Flash | 21.6% (425,413 / 1,966,080 B) |
| RAM | 8.9% (29,148 / 327,680 B) |

빌드 디렉터리를 지우고 두 번 빌드해 MD5 동일 재현을 확인했습니다.

---

## 한 줄 요약

**서버가 내린 명령이 조용히 사라지던 것을 고칩니다.** 다른 AT 왕복과 겹쳐 도착한
다운링크 URC 를 통째로 놓쳤고, `ota_start` 는 물론 `door_lock`·`config_update` 도
같은 확률로 유실됐습니다. 서버 변경은 없습니다 — 페이로드·토픽·계약이 그대로입니다.

---

## 무슨 일이 있었나

0.3.5 단말에 `ota_start` 를 내렸는데 아무 반응이 없었습니다. 시리얼에 남은 것:

```
[3159615] ### Unhandled: +CMQTTRXTOPIC: 0,23
v1/vt-2607-0003-kfo/cmd
+CMQTTRXPAYLOAD: 0,259
{"command_id":"019fd13c-...","type":"ota_start",...}
+CMQTTRXEND: 0
```

브로커→모뎀까지는 정상 도착했습니다. `### Unhandled` 는 TinyGSM 이 **알아보지 못한
데이터를 버릴 때** 찍는 로그이고, `[CMD] 수신` 도 ack 도 없었습니다.

**결정적 단서는 이 덤프에 `+CMQTTRXSTART` 가 없다는 것**입니다. 모뎀은 반드시
`RXSTART → RXTOPIC → RXPAYLOAD → RXEND` 순으로 뱉으므로 첫 줄만 따로 사라진 것입니다.

## 왜 사라졌나

수신 URC 는 **아무 때나** 옵니다. 그런데 종전 코드는 `Mqtt::handle()` → `mqtt_handle()`
을 부르는 그 순간에만 `+CMQTTRXSTART:` 를 알아봤습니다. 하필 다른 AT 왕복
(`CGNSSINFO` 10초 · `CSQ`/`CREG` · `CCLK` · CMQTT 발행)이 진행 중이면:

1. 그쪽 `waitResponse` 가 `+CMQTTRXSTART:` 를 "모르는 문자열"로 누적하고,
2. 기다리던 `OK` 를 만나는 순간 **누적분을 통째로 폐기**합니다 — 매칭에 성공한
   경로라 `Unhandled` 로그조차 남지 않습니다.
3. 첫 줄을 잃은 나머지 블록만 스트림에 남고, 뒤이어 도는 `mqtt_handle` 은 시작을
   못 찾아 아무것도 하지 않습니다 → **명령 유실**.

`cmd` 다운링크가 처음 들어간 시점부터 있던 구조적 문제입니다. 유휴 구간에 도착하면
정상 처리되기 때문에 지금까지 검증에서는 통과했습니다.

> 같은 증상을 이전에 두 번 다뤘습니다 — `1dff1a0`(SUBACK 타임아웃·구독 재시도·파싱 창
> 20ms→100ms)과 `a9c977b`(수신 버퍼 231B→1000B). 둘 다 실재하는 버그였지만,
> 파싱 창을 넓힌 처방은 **단일 창구 구조를 그대로 둔 것**이라 잔여 유실이 남았습니다.

## 어떻게 고쳤나

이미 `+CMQTTPUB` 결과 URC 에 쓰던 것과 같은 방식으로, **어느 `waitResponse` 든
`+CMQTTRXSTART:` 를 보는 즉시 그 자리에서 블록을 끝까지 걷어냅니다.**

- `TinyGsmClientA7670.h` — `waitResponse` URC 분기에 `+CMQTTRXSTART:` 추가
- `TinyGsmMqttA76xx.h` — 읽기 본체를 `mqtt_read_rx_urc()` / `readRxBlock()` 으로 분리

부수 효과로 진행 중이던 AT 응답이 URC 로 오염되는 것도 함께 막힙니다.

**두 번째 경로도 닫았습니다.** 로그만으로는 위 (A) "다른 AT 가 삼켰다" 와
(B) "`mqtt_handle` 이 RXSTART 는 잡았지만 다음 단계 100ms 에서 끊겼다" 를 구분할 수
없습니다 — 두 경우 모두 로그가 동일합니다. 그래서 **RXSTART 를 본 뒤에는 짧은 상한으로
끊지 않도록** 바꿨습니다(단계당 500ms). 나머지 블록은 반드시 오게 돼 있으므로 중간에
포기할 이유가 없고, 정상 경로에선 즉시 매칭돼 평상시 비용은 0 입니다.

그 밖에 재진입 가드, `mqtt_begin` 전 버퍼 미할당 가드, 그리고 페이로드가 버퍼 상한에
걸려 잘렸을 때 **실제로 담긴 만큼만** 콜백에 넘기도록(종전에는 모뎀이 알려준 길이를
그대로 넘겨 버퍼 밖을 읽을 수 있었습니다) 함께 손봤습니다.

---

## ⚠️ 이 이미지를 OTA 로 내릴 때

**고치려는 그 경로로 고칠 것을 내려보내는 상황입니다.** 명령을 받는 쪽은 아직
버그가 있는 0.3.5 이므로, `ota_start` 자체가 유실될 수 있습니다.

- 유실은 **상시가 아니라** AT 왕복과 겹칠 때만입니다 → **재발행하면 대체로 통과합니다.**
- 유실됐는지는 시리얼로 즉시 구분됩니다:
  - `[CMD] 수신 id=... type=ota_start` 가 뜨면 → 받았습니다
  - `### Unhandled: +CMQTTRXTOPIC` 만 뜨면 → 유실입니다. 다시 발행하세요
- 확실히 하려면 **USB 플래싱**입니다(`pio run -t upload`). 이 `.bin` 은 OTA 용 app
  이미지이지 `0x0` 통합 이미지가 아닙니다.

## ⚠️ 그다음 관문 — HTTPS 다운로드는 미검증입니다

OTA URL 이 `https://mqtt-dev...:40101` 인데, `https_set_url` 은 CA·authmode 를 따로
세팅하지 않고 **SSL ctx 0 를 MQTT 와 공유**합니다(MQTT 가 올려둔 `cacert`,
`authmode=1` 을 그대로 물려받음). 배포 호스트 인증서가 같은 CA 로 검증되지 않으면
여기서 떨어집니다:

```
[OTA] HTTP GET 실패 code=... size=0
```

이 로그가 나오면 원인은 수신이 아니라 TLS 입니다.

---

## 파일 구성

```
keyplus-diagnostic-0.3.8.bin       OTA로 내려보낼 이미지 (app 파티션 전용)
keyplus-diagnostic-0.3.8.bin.md5   MD5 한 줄 (배포 서버 검증용)
ota-manifest.json                  배포 메타 — url 채워서 사용
ota_start.json                     MQTT 명령 템플릿 — url/command_id 채워서 발행
```

배포는 `scripts/deploy-ota.sh 0.3.8` 로 전송·검증이 자동화됩니다.

---

## 배포 전 체크리스트

- [ ] 업로드한 파일의 MD5 가 위 값과 일치 (`md5 -q <file>`)
- [ ] `ota_start.json` 의 `version` 이 정확히 `0.3.8` — 불일치면 성공해도 ack 가 `failed`
- [ ] `ota_start.json` 직렬화 길이 **< 1,000 bytes**
- [ ] URL 이 `http` 로 시작하고 단말에서 접근 가능
- [ ] 대상 단말이 `cmd` 구독 성공 상태 (`status server` 에서 확인)
- [ ] **명령 발행 후 시리얼에서 `[CMD] 수신` 확인** — 안 뜨면 유실이므로 재발행
- [ ] 단말 전원 안정 — 재부팅 후 10분 안에 MQTT 에 못 붙으면 **자동 롤백**됩니다
- [ ] 1대 선행 배포 후 확대

> 0.3.0 README 의 OTA 제약(페이로드 1,000B, 다운로드 중 MQTT 끊김, PENDING_VERIFY 10분
> 확정, 전파·전원 불안정 환경 회피)은 그대로 유효합니다.

## 그 이전 변경

`release/0.3.7/README.md`(통신사 자동 판별 + 0.3.1~0.3.6),
`release/0.3.0/README.md`(명령 전달 신뢰성 F1~F4) 이하를 참고하세요.
