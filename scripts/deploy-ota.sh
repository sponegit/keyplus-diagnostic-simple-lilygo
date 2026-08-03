#!/usr/bin/env bash
#
# OTA 펌웨어 배포 — release/<버전>/*.bin 을 백엔드 firmware-bin/ 으로 전송한다.
#
#   ./scripts/deploy-ota.sh 0.2.12
#   ./scripts/deploy-ota.sh 0.2.12 --dry-run
#
# 환경변수로 대상 서버를 덮어쓸 수 있다:
#   SSH_HOST   (기본 mgr@mqtt-dev.keyplus.sponeinfra.com)
#   SSH_PORT   (기본 30022)
#   REMOTE_DIR (기본 keyplus/apps/keyplus-diagnostic-backend/firmware-bin/)
#
set -euo pipefail

SSH_HOST="${SSH_HOST:-mgr@mqtt-dev.keyplus.sponeinfra.com}"
SSH_PORT="${SSH_PORT:-30022}"
REMOTE_DIR="${REMOTE_DIR:-keyplus/apps/keyplus-diagnostic-backend/firmware-bin/}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RELEASE_ROOT="$REPO_ROOT/release"

die() { printf '오류: %s\n' "$*" >&2; exit 1; }

usage() {
    cat >&2 <<EOF
사용법: $(basename "$0") <버전> [--dry-run]

  <버전>      release/ 아래의 디렉터리 이름 (예: 0.2.12)
  --dry-run   전송하지 않고 실행할 scp 명령만 출력한다
EOF
    exit 2
}

# 사용 가능한 버전을 버전 순으로 나열한다 (0.2.9 < 0.2.10 이 되도록 -V 정렬).
list_versions() {
    find "$RELEASE_ROOT" -mindepth 1 -maxdepth 1 -type d -exec basename {} \; \
        | sort -V
}

VERSION=""
DRY_RUN=0
for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=1 ;;
        -h|--help) usage ;;
        -*)        die "알 수 없는 옵션: $arg" ;;
        *)
            if [ -n "$VERSION" ]; then
                die "버전은 하나만 지정한다 (이미 '$VERSION')"
            fi
            VERSION="$arg"
            ;;
    esac
done

[ -n "$VERSION" ] || usage
[ -d "$RELEASE_ROOT" ] || die "release 디렉터리가 없다: $RELEASE_ROOT"

# 1) 버전 디렉터리
VERSION_DIR="$RELEASE_ROOT/$VERSION"
if [ ! -d "$VERSION_DIR" ]; then
    printf '오류: release/%s 가 없다. 사용 가능한 버전:\n' "$VERSION" >&2
    list_versions | sed 's/^/  /' >&2
    exit 1
fi

# 2) bin 파일 탐색 — *.bin 만 잡고 *.bin.md5 는 글롭에서 자연히 빠진다
#    (bash 3.2 호환 — mapfile 없이 글롭 루프로 센다)
BIN=""
BIN_COUNT=0
for candidate in "$VERSION_DIR"/*.bin; do
    [ -f "$candidate" ] || continue
    BIN="$candidate"
    BIN_COUNT=$((BIN_COUNT + 1))
done
if [ "$BIN_COUNT" -eq 0 ]; then
    die "release/$VERSION 에 .bin 파일이 없다"
elif [ "$BIN_COUNT" -gt 1 ]; then
    printf '오류: release/%s 에 .bin 이 여러 개다 — 무엇을 올릴지 모호하다:\n' "$VERSION" >&2
    ls -1 "$VERSION_DIR"/*.bin >&2
    exit 1
fi
BIN_NAME="$(basename "$BIN")"
BIN_SIZE="$(wc -c < "$BIN" | tr -d ' ')"

# 3) 로컬 무결성 — .bin.md5 가 있으면 전송 전에 대조한다
LOCAL_MD5="$(md5 -q "$BIN" 2>/dev/null || md5sum "$BIN" | cut -d' ' -f1)"
if [ -f "$BIN.md5" ]; then
    EXPECTED_MD5="$(tr -d '[:space:]' < "$BIN.md5" | cut -d' ' -f1)"
    [ "$LOCAL_MD5" = "$EXPECTED_MD5" ] \
        || die "MD5 불일치 — 빌드 산출물이 손상됐다
  파일:   $LOCAL_MD5
  .md5:   $EXPECTED_MD5"
else
    printf '경고: %s.md5 가 없어 로컬 검증을 건너뛴다\n' "$BIN_NAME" >&2
fi

printf '버전   : %s\n' "$VERSION"
printf '파일   : %s (%s bytes)\n' "$BIN_NAME" "$BIN_SIZE"
printf 'MD5    : %s\n' "$LOCAL_MD5"
printf '대상   : %s:%s (port %s)\n\n' "$SSH_HOST" "$REMOTE_DIR" "$SSH_PORT"

if [ "$DRY_RUN" -eq 1 ]; then
    printf '[dry-run] scp -P %s %q %s:%s\n' \
        "$SSH_PORT" "$BIN" "$SSH_HOST" "$REMOTE_DIR"
    exit 0
fi

# 4) 전송
scp -P "$SSH_PORT" "$BIN" "$SSH_HOST:$REMOTE_DIR"

# 5) 원격 검증 — 실패해도 전송 자체는 끝났으므로 경고로만 남긴다
REMOTE_MD5="$(ssh -p "$SSH_PORT" "$SSH_HOST" \
    "md5sum '$REMOTE_DIR$BIN_NAME' 2>/dev/null | cut -d' ' -f1" || true)"
if [ -z "$REMOTE_MD5" ]; then
    printf '\n전송 완료 — 원격 MD5 확인은 건너뛴다(md5sum 없음)\n'
elif [ "$REMOTE_MD5" = "$LOCAL_MD5" ]; then
    printf '\n전송 완료 — 원격 MD5 일치 (%s)\n' "$REMOTE_MD5"
else
    printf '\n경고: 원격 MD5 불일치 — 재전송이 필요하다\n  로컬: %s\n  원격: %s\n' \
        "$LOCAL_MD5" "$REMOTE_MD5" >&2
    exit 1
fi
