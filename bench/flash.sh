#!/usr/bin/env bash
#
# 소비전류 계측 빌드 플래시 — bench/README.md 참고.
#
#   ./bench/flash.sh 3            # floor    (모뎀 OFF)
#   ./bench/flash.sh 1            # idle     (모뎀 유휴, LTE off)
#   ./bench/flash.sh 2            # mqtt     (LTE+MQTT 상시연결)
#   ./bench/flash.sh 4            # mqtt+gps (2 에 GNSS 만)
#   ./bench/flash.sh 5            # mqtt+obd (2 에 OBD2/CAN 만)
#   ./bench/flash.sh 0            # 정상     (전체 기능, 검산용)
#   PORT=/dev/cu.usbserial-XXXX ./bench/flash.sh 2
#
# ⚠️ 모드 1·3 은 LTE 가 꺼져 있어 OTA 로 원복할 수 없다. USB 가 닿는 곳에서만 쓸 것.
# 원복(정상 동작): ~/.platformio/penv/bin/pio run -e T-A7670X -t upload
#
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ESPTOOL="${ESPTOOL:-$HOME/.platformio/packages/tool-esptoolpy/esptool.py}"
PYTHON="${PYTHON:-$HOME/.platformio/penv/bin/python}"   # esptool 은 pio 의 venv 파이썬으로 돌린다
APP_OFFSET=0x10000          # min_spiffs.csv 의 app0 시작 주소

die() { printf '오류: %s\n' "$*" >&2; exit 1; }

DRY_RUN=0
[ "${2:-}" = "--dry-run" ] && DRY_RUN=1

MODE="${1:-}"
case "$MODE" in
    3) BIN="$BENCH_DIR/bench-3-floor.bin"; DESC="floor — ESP32+보드만(모뎀 OFF)" ;;
    1) BIN="$BENCH_DIR/bench-1-idle.bin";  DESC="idle  — 모뎀 유휴, LTE off" ;;
    2) BIN="$BENCH_DIR/bench-2-mqtt.bin";  DESC="mqtt  — LTE 등록 + MQTT 상시연결" ;;
    4) BIN="$BENCH_DIR/bench-4-mqtt-gps.bin"; DESC="mqtt+gps — 2 에 GNSS 만 추가" ;;
    5) BIN="$BENCH_DIR/bench-5-mqtt-obd.bin"; DESC="mqtt+obd — 2 에 OBD2/CAN 만 추가" ;;
    0) BIN="$BENCH_DIR/bench-0-full.bin";  DESC="정상 — 전체 기능(검산용)" ;;
    *) die "사용법: $(basename "$0") <3|1|2|4|5|0>
   3=floor  1=idle  2=mqtt  4=mqtt+gps  5=mqtt+obd  0=정상(전체)" ;;
esac

[ -f "$BIN" ] || die "$BIN 이 없다. 저장소 루트에서 아래로 다시 만든다:
  sed -i '' 's/^#define PWR_BENCH .*/#define PWR_BENCH           $MODE/' examples/KeyplusDiagnostic/config.h
  ~/.platformio/penv/bin/pio run -e T-A7670X && cp .pio/build/T-A7670X/firmware.bin $BIN"
[ -f "$ESPTOOL" ] || die "esptool 을 못 찾았다: $ESPTOOL (ESPTOOL 환경변수로 지정)"
[ -x "$PYTHON" ]  || die "파이썬을 못 찾았다: $PYTHON (PYTHON 환경변수로 지정)"

# 포트 자동 탐색 — USB-Serial 브리지 하나만 꽂혀 있다는 전제.
# ⚠️ macOS 기본 bash 는 3.2 라 mapfile 이 없다. 포터블하게 센다.
if [ -z "${PORT:-}" ]; then
    PORTS=""
    NPORTS=0
    for p in /dev/cu.usbserial-* /dev/cu.SLAB_USBtoUART* /dev/cu.wchusbserial*; do
        [ -e "$p" ] || continue
        PORTS="$PORTS $p"
        NPORTS=$((NPORTS + 1))
    done
    [ "$NPORTS" -eq 1 ] || die "포트를 특정할 수 없다(${NPORTS}개 발견:${PORTS:- 없음}). PORT=... 로 지정할 것"
    PORT="${PORTS# }"
fi

printf '대상 : %s\n포트 : %s\n펌웨어: %s\n\n' "$DESC" "$PORT" "$(basename "$BIN")"

# ⚠️ 포트 자동탐색은 "USB-Serial 어댑터가 하나뿐"이라는 전제다. 보조 콘솔용 USB-TTL
#    어댑터(console.h 의 GPIO13/34)가 꽂혀 있으면 그쪽이 잡혀 엉뚱한 포트로 간다.
#    자동리셋 배선이 없어 esptool 이 "No serial data received" 로 끝난다 —
#    그 메시지가 뜨면 PORT= 로 보드의 USB-C 포트를 직접 지정할 것.
if [ "$DRY_RUN" = "1" ]; then
    printf '[--dry-run] 실행할 명령:\n  %s %s --chip esp32 --port %s --baud 921600 write_flash -z %s %s\n' \
        "$PYTHON" "$ESPTOOL" "$PORT" "$APP_OFFSET" "$BIN"
    exit 0
fi

"$PYTHON" "$ESPTOOL" --chip esp32 --port "$PORT" --baud 921600 \
    write_flash -z "$APP_OFFSET" "$BIN"

cat <<EOF

플래시 완료. 이어서 확인할 것:
  1) 시리얼(115200)에 '⚠️ PWR_BENCH: $MODE' 배너가 찍히는지 — 안 찍히면 계측값 무효
  2) 안정화 대기 (floor/idle 1분, mqtt 는 접속 완료 후 3분)
  3) 12V 입력 / 5V 출력 두 지점 평균을 bench/README.md 기록표에 적기
EOF
