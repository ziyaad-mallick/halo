#!/usr/bin/env bash
# Build Halo with the LLVM-MinGW toolchain. Usage: tools/build.sh [halo|all]
set -euo pipefail
cd "$(dirname "$0")/.."
CXX="${HALO_CXX:-/c/Users/testi/Windhawk/Compiler/bin/clang++.exe}"
mkdir -p build

COMMON=(-std=c++23 -O2 -target x86_64-w64-mingw32 -municode -mwindows -DUNICODE -D_UNICODE
        -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000008 -static -Wall)
WINLIBS=(-lwindowsapp -lruntimeobject -lcoremessaging -ld3d11 -ldxgi -ld2d1 -ldwrite -ldcomp -ldwmapi
         -lole32 -luuid -luser32 -lgdi32 -lshell32 -lshcore)

build_halo() {
  echo "== halo.exe"
  "$CXX" "${COMMON[@]}" src/halo.cpp -o "${HALO_OUT:-build/halo.exe}" "${WINLIBS[@]}"
}
# Add build_guardian / build_icon_test here as those sources land.

case "${1:-halo}" in
  halo | all) build_halo ;;
  *) echo "unknown target $1" >&2; exit 2 ;;
esac
ls -l "${HALO_OUT:-build/halo.exe}"
