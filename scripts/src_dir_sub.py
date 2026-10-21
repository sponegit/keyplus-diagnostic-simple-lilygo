# PlatformIO pre 스크립트 — env:T7-S3 의 소스 폴더를 서브 펌웨어로 바꾼다.
# src_dir 은 [platformio] 전역 옵션이라 env 마다 다르게 줄 수 없어서, 빌드 시작 전에 덮어쓴다.
Import("env")  # noqa: F821

env.Replace(PROJECT_SRC_DIR="$PROJECT_DIR/examples/KeyplusSub")  # noqa: F821
