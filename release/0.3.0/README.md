# Keyplus Diagnostic 펌웨어 0.3.0 — OTA 배포 패키지

> 작업서: `keyplus-docs/keyplus-project/260706-simple-obd2-diagnostic/firmware-command-reliability.md`
> 서버 기준 커밋: `dc9bb14` (keyplus-diagnostic-backend, main)
> 이전 릴리스: `release/0.2.12/` (오프라인 백필 · 고빈도 샘플)

| 항목 | 값 |
|---|---|
| 파일 | `keyplus-diagnostic-0.3.0.bin` |
| 크기 | 406,272 B |
| MD5 | `c3afd6a57b0c69da9a9f3a51e40ea05a` |
| SHA256 | `6e936394403e19756dc6f68d5defbab59baadd00c27ccb5ff560960f37287ac0` |
| Flash | 20.6% (405,913 / 1,966,080 B) |
| RAM | 8.8% (28,772 / 327,680 B) |

---

## 한 줄 요약

**명령 전달 신뢰성** 4건(F1~F4)입니다. 서버는 이미 반영이 끝나 있고, 이 펌웨어만
올리면 켜집니다. 기능 추가라기보다 **보고 정확도와 유실 제거**입니다.

---

## UART 진단 앱은 그대로 두어도 됩니다

`[STAT]` 한 줄 포맷이 0.2.12 와 **동일합니다**. status 재발행 로그는 `[MQTT]` 태그로
나가므로 앱의 `STAT_RE` 파싱에 닿지 않습니다.

> ⚠️ 단 **0.2.12 를 아직 배포하지 않았다면** UART 앱 `STAT_KEYS` 에 `buf`·`fast` 추가가
> 여전히 선결 조건입니다 — `release/0.2.12/README.md` 참고. 앱이 닫힌 키 목록으로
> 다음 필드 경계를 잡기 때문에, 미등록 토큰은 앞 필드 값에 통째로 흡수됩니다.

---

## 무엇이 바뀌었나

### F1. status 전이 재발행 — **서버 변경 없음**

`(OBD 링크, rpm>0)` 이 **바뀔 때만** status 를 재발행합니다.

```json
{"online":true,"power_mode":"driving","ignition_on":true}
```

| OBD 링크 | rpm | `power_mode` | `ignition_on` | 상황 |
|---|---|---|---|---|
| 있음 | > 0 | `driving` | `true` | 시동 ON |
| 있음 | 0 | `driving` | `false` | 키 ACC/ON, 엔진 정지 |
| 없음 | — | `parked_active` | `false` | 완전 OFF |

**왜**: 계약상 이 둘은 단말이 보고하는 값인데(`StatusPayload`), 펌웨어는 status 로
`{"online":true}` 만 보내 두 값이 DB 기본값에서 한 번도 움직인 적이 없었습니다.
서버가 telemetry 의 `obd`·`rpm` 에서 임시로 파생하고 있었고(30초+ 지연), status 가
접속당 1회뿐이라 그대로 실어도 접속 시점 값에 굳습니다 — 단말은 몇 시간씩 접속을
유지합니다.

- **CAN 링크 유무와 rpm 을 분리**했습니다. 키가 ACC/ON 위치면 시동이 꺼져 있어도 CAN 은
  살아 있고 그때 rpm 은 0 입니다. 링크만으로 시동을 판단하면 이 상태를 시동 켜짐으로
  오인합니다.
- **`parked_deep` 은 쓰지 않습니다.** 그건 "차가 꺼짐"이 아니라 **"단말이 잠듦"**을 뜻하고
  (서버 TTL 300초 · `estimated_delivery: "within_3_min"`), 이 단말은 상시 12V 라 차가
  꺼져도 계속 접속돼 1초 안에 응답합니다. 주차 중인 전 차량이 "3분 내 배달"로 나가면
  그건 거짓말입니다. **실제 단말 절전을 도입할 때까지 비워둡니다.**
- **주기 발행이 아닙니다.** 전이가 없으면 한 건도 나가지 않습니다. `retain=1` 을
  유지하므로 브로커의 retained 값도 최신으로 갱신됩니다.

> 🔜 **배포 후 서버 작업**: `services/ingest.ts` ③-b 의 telemetry 파생 블록을 제거해야
> 합니다. 남겨두면 단말이 먼저 알린 값을 30초 뒤에 서버 파생이 덮어써 지연이 되살아나고,
> 출처가 둘이라 추적이 어려워집니다.

### F2. `ota_start` 에 `received` ack — **서버 변경 없음(수용 대기)**

다운로드 시작 **전에** 수신 사실을 보고합니다.

```json
{"command_id":"...","result":"received","ts":1785000000}
```

`ota_start` 는 수신 → 다운로드(실측 2분+) → 재부팅 → ack 라 그 구간이 통째로
깜깜했습니다. 서버는 `7e809cc` 이후 **단말 ack 로만** `delivered` 를 올리므로
(브로커 publish 성공은 "구독이 살아 있다"까지만 증명합니다) 이 한 건이 OTA 진행
가시성의 전부입니다.

서버는 `received` 를 `delivered`(비종결)로 매핑합니다 — `completed_at` 을 찍지 않고
`command.completed` 웹훅도 보내지 않습니다. 뒤이어 `done`/`failed` 가 옵니다.

> **도어·설정에는 넣지 않았습니다.** 이들은 수신 즉시 `done` 을 보내므로 `received` 와
> `done` 이 연달아 나가고, 모뎀은 publish 가 겹치면 세션이 깨집니다. `done` 이 이미
> 수신 증거 역할을 하므로 서버는 정확히 동작합니다.

### F3. 구독 완료 통지 `sub:true` — **서버 분기 1개 필요**

`Cmd::subscribe()` 성공 직후 status 에 플래그를 얹습니다.

```json
{"online":true,"sub":true,"power_mode":"...","ignition_on":false}
```

서버는 지금 단말이 언제 구독을 끝냈는지 몰라서 `status online` 을 받고
**`COMMAND_FLUSH_DELAY_MS`(7초)를 추측으로 기다린 뒤** pending 을 flush 합니다.
구독이 실패해 백오프(15초→30초→…)를 타면 그 타이머는 헛돌고, 게이트웨이가 재시작하면
인메모리 타이머 자체가 사라집니다. `sub:true` 가 오면 지연 0 으로 즉시 flush 합니다.

새 토픽도 새 EMQX Rule 도 필요 없습니다 — `fwd_status` 가 이미 잡습니다.

> ⚠️ **차키에는 큰 이득이 없습니다.** TTL 10초라 오프라인 큐잉 자체가 성립하지 않습니다.
> 이득은 `ota_start`(300초)·`config_update`(24시간) 의 큐잉이 정확해지는 것입니다.

### F4. cmd 수신 큐 1칸 → 4칸 — **서버 변경 없음**

종전 `onMessage` 는 직전 명령이 미처리면 **로그도 ack 도 없이** 드롭했습니다.
수신 버퍼가 1칸이고 `Cmd::handle()` 은 loop 당 1건씩 비우므로, 명령이 loop 주기(50ms)
보다 촘촘히 도착하면 뒤엣것이 사라졌습니다. 서버 `flushPendingForDevice()` 는 pending 을
순회하며 연속 발행하고 EMQX REST 왕복이 수십 ms 라 정확히 이 조건입니다.

- 4칸 링(4 × 1024B = 4KB 정적 RAM). 명령은 드물어 실무상 충분합니다.
- **가득 찼을 때만 드롭하고, 그때는 `[CMD]` 로그를 남깁니다.** 조용한 유실이 가장 나쁩니다.
- 누적 드롭 건수는 콘솔 `status` 에도 표시됩니다.
- `handle()` 은 **틱당 1건만** 처리합니다(현행 유지).

### 발행 간격 가드 중앙화 — 이 펌웨어 최대 취약점 대응

모뎀은 **publish 가 겹치면 세션이 깨집니다**(실측 `+CMQTTCONNLOST`). 래퍼
`mqtt_publish()` 는 `AT+CMQTTPUB` 의 `OK` 까지만 기다리고 결과 URC(`+CMQTTPUB: 0,0`)는
비동기로 늦게 옵니다 — 즉 "반환됐다"가 "발행이 끝났다"는 뜻이 아닙니다.

이번에 **모든 발행 경로가 `Mqtt::publishGapElapsed(PUBLISH_MIN_GAP_MS=1초)` 하나를
공유**하도록 했습니다.

```
telemetry(주기) → status 전이 → 고빈도 창 → 백필     (else-if 우선순위)
cmd ack (Cmd::handle, 발행 체인 밖)                   ← 종전에는 이 규칙 밖이었다
```

종전에는 cmd ack 가 가드 밖에 있어 telemetry·백필과 겹칠 수 있었습니다. `ota_start` 는
`received` 발행 후 실행을 `CMD_DEFER_EXEC_MS`(1초) 뒤로 미뤄, `CMQTTSTOP` 이 아직 안 빠진
`+CMQTTPUB` URC 와 겹치지 않게 했습니다.

> ⚠️ **대가**: 명령 처리에 최대 1초의 페이싱 지연이 생깁니다(직전 발행이 있었을 때만).
> 차키 TTL 10초는 원래 시계 오차·LTE Cat-1 왕복(~1초)·OBD 폴 지연(11 PID × 100ms)을
> 흡수하도록 잡은 값이라 여유 안입니다. 다만 **TTL 을 5초로 낮추자는 논의가 나온다면
> 이 1초를 함께 계산해야 합니다.**

### 콘솔

`status` 출력에 아래가 추가됩니다.

```
  차량 상태(보고)  : driving / ignition_on
  · 미처리 수신 명령 1건 (큐 4칸)
  ⚠️ 수신 큐 넘침으로 드롭 2건        ← 0 이 아니면 명령이 실제로 유실된 것
```

---

## 서버 선행/후속 작업

| 시점 | 작업 | 대상 |
|---|---|---|
| 불필요 | F1·F2·F4 는 서버 무변경으로 동작 | — |
| 배포 후 | `ingestStatus` 에 `p.sub === true` 분기 추가 → 즉시 `flushPendingForDevice` | F3 |
| 배포 확인 후 | `services/ingest.ts` ③-b telemetry 파생 블록 **제거** | F1 |

F3 분기는 **하위호환**입니다 — 구식 펌웨어는 `sub` 를 안 보내므로 기존 7초 경로를 탑니다.
따라서 펌웨어를 먼저 올려도 무해합니다(그동안은 7초 대기가 유지될 뿐).

---

## 실차 검증 항목

| # | 항목 | 방법 | 합격 기준 |
|---|---|---|---|
| 1 | F1 시동 ON | 시동 걸고 `devices` 조회 | **5~10초 내** `power_mode='driving'`, `ignition_on=true` |
| 2 | F1 ACC 구분 | 키를 ACC 에 두고 조회 | `power_mode='driving'`, `ignition_on=`**`false`** |
| 3 | F1 시동 OFF | 시동 끄고 CAN 사망 후 조회 | `parked_active` / `false` |
| 4 | F1 발행 빈도 | 10분 주행 중 `[MQTT] status` 관찰 | **전이 시에만** — 주기 발행 없음 |
| 5 | F1 겹침 | 시동 on/off 를 telemetry 발행 시각에 맞춰 반복 | `+CMQTTCONNLOST` 미발생, telemetry 주기 안 밀림 |
| 6 | F2 OTA | `ota_start` 하달 | 다운로드 시작 전 `delivered`, 재부팅 후 `done` |
| 7 | F3 구독 통지 | 재접속 관찰 | `sub:true` 수신 즉시 flush, 7초 대기 없음 |
| 8 | F4 다건 | `config_update` 2건 연속 하달 | **둘 다** ack |
| 9 | F4 큐 오버플로 | 5건 이상 연속 하달 | 드롭이 **로그로 보인다** — `[CMD] 수신 큐 가득 참` |
| 10 | 회귀 | 도어 열림/잠금 각 3회 | `pending → done`, 거짓 `delivered` 없음 |

### 서버측 확인 쿼리

```sql
-- F1
SELECT device_id, power_mode, ignition_on, last_seen_at FROM devices;

-- F2 (ota_start)
SELECT status, delivered_at, completed_at FROM commands
 WHERE type='ota_start' ORDER BY created_at DESC LIMIT 1;

-- 회귀: 오프라인 단말에 도어 → delivered 가 뜨면 안 된다
SELECT status, last_dispatch_at, delivered_at FROM commands
 ORDER BY created_at DESC LIMIT 5;
```

---

## 파일 구성

```
keyplus-diagnostic-0.3.0.bin       OTA로 내려보낼 이미지 (app 파티션 전용)
keyplus-diagnostic-0.3.0.bin.md5   MD5 한 줄 (배포 서버 검증용)
ota-manifest.json                  배포 메타 (버전/크기/해시/변경점) — url 채워서 사용
ota_start.json                     MQTT 명령 템플릿 — url/command_id 채워서 발행
```

> ⚠️ 이 `.bin`은 **app 이미지**입니다. USB로 처음 굽는 `0x0` 통합 이미지가 아닙니다.
> 시리얼 최초 플래싱은 `pio run -t upload`을 쓰세요.

---

## 배포 절차

1. `keyplus-diagnostic-0.3.0.bin`을 HTTP(S)로 접근 가능한 곳에 올린다.
2. `ota-manifest.json`의 `url`을 실제 주소로 채워 배포 기록으로 남긴다.
3. `ota_start.json`의 `command_id`(UUID)와 `url`을 채운다.
4. 대상 단말의 **`v1/{device_id}/cmd`** 토픽으로 발행한다 (QoS 1).
5. 단말이 **`received` ack 를 먼저 보내고**(F2), 다운로드 → 플래시 → 재부팅한다.
6. 재부팅 후 MQTT 재접속되면 **`v1/{device_id}/cmd/ack`** 로 결과가 올라온다.

`scripts/deploy-ota.sh` 를 쓰면 1~4 가 자동화됩니다.

---

## ⚠️ 반드시 지킬 제약

### 1. 명령 JSON은 **약 1,000 바이트 미만**이어야 한다

래퍼(`TinyGsmMqttA76xx`)가 토픽과 페이로드를 **하나의 버퍼에** 나눠 담기 때문에,
토픽 길이만큼 페이로드 여유가 줄어듭니다(1024B − 토픽 24B ≈ 1,000B). 넘치면
**경고 없이 잘리고**, 잘린 JSON은 `url`이 빈 값이 되어 `[OTA] 잘못된 URL`로 실패합니다.

```bash
python3 -c "import json;print(len(json.dumps(json.load(open('ota_start.json')),separators=(',',':'))),'bytes')"
```

### 2. `version`은 `0.3.0`와 **정확히** 일치해야 한다

재부팅 후 실행 이미지의 `FW_VERSION`과 대조해 ack를 결정합니다(`ota.cpp`).
불일치하면 실제로는 성공했어도 **`failed`로 ack**가 나갑니다.

### 3. URL은 `http`로 시작해야 한다

`ota.cpp`가 `url.startsWith("http")`로 1차 검증합니다. HTTPS 권장.

### 4. 다운로드 중에는 MQTT가 끊긴다

모뎀의 SSL 컨텍스트를 HTTP(S)에 양보하려고 `Mqtt::stopService()`를 먼저 호출합니다.
그래서 **결과** ack는 재부팅 후 재접속 시점에 발행됩니다(NVS pending). `received` 는 그
이전에 나가므로 영향받지 않습니다.

### 5. 재부팅 후 10분 안에 MQTT에 붙어야 확정된다

새 이미지는 `PENDING_VERIFY` 상태로 부팅하고, MQTT 접속에 성공해야
`Ota::onHealthy()`가 확정(`mark_valid`)합니다. `OTA_CONFIRM_DEADLINE_MS`(10분) 안에
못 붙으면 **자동 롤백 재부팅**됩니다.

→ 전파가 약한 곳이나 **전원이 불안정한 차량에서는 OTA를 피하세요.**

### 6. 차키 지연 전달은 이 릴리스로도 해결되지 않는다

차키 TTL 은 10초이고 **재시도가 없는 전제의 값**입니다(스위퍼 30초·재발행 백오프 60초는
둘 다 만료 뒤에 돕니다). 차키는 생성 시점의 즉시 발행 1회가 전부입니다.
F3·F4 는 이걸 살리지 못합니다 — **TTL 정책의 결과이지 버그가 아닙니다.**

---

## 배포 전 체크리스트

- [ ] 0.2.12 미배포 상태라면 **UART 앱 `STAT_KEYS` 갱신이 먼저 배포됐는가**
- [ ] 업로드한 파일의 MD5가 위 값과 일치 (`md5 -q <file>`)
- [ ] 서빙하는 bin이 **0.3.0 릴리스의 것**인지
- [ ] URL이 단말에서 접근 가능 (사설망/방화벽 확인)
- [ ] `ota_start.json` 직렬화 길이 **< 1,000 bytes**
- [ ] `version` 필드가 `0.3.0`
- [ ] 대상 단말이 **`cmd` 구독 성공** 상태 — `status server`에서 `[ok]` 확인
- [ ] 단말 전원 안정 (브라운아웃 이력 없음 — `info`의 `reset` 확인)
- [ ] 1대 선행 배포 후 확대
- [ ] 배포 후 시동 ON/OFF 1회로 F1 전이 확인, `config_update` 2건 연속으로 F4 확인

> **0.2.6 / 0.2.1 은 사용하지 마세요.** 0.2.6은 `FW_VERSION` 누락으로 ack가 항상
> `failed`, 0.2.1은 모뎀 리셋 후 MQTT 영구 접속불가 결함이 있습니다.

## 그 이전 변경

`release/0.2.12/README.md`(오프라인 백필 · 고빈도 1Hz 샘플),
`release/0.2.11/README.md`(미측위 구간 telemetry 유실 수정),
`release/0.2.10/README.md`(OBD2 응답 ECU 고정 + GPS 첫 fix 15분→27초) 이하를 참고하세요.
