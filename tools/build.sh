#!/usr/bin/env bash
# Build Halo with the LLVM-MinGW toolchain. Usage: tools/build.sh [halo|guardian|icons_test|all]
set -euo pipefail
cd "$(dirname "$0")/.."
CXX="${HALO_CXX:-/c/Users/testi/Windhawk/Compiler/bin/clang++.exe}"
mkdir -p build

COMMON=(-std=c++23 -O2 -target x86_64-w64-mingw32 -municode -DUNICODE -D_UNICODE
        -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000008 -static -Wall)
WINLIBS=(-lwindowsapp -lruntimeobject -lcoremessaging -ld3d11 -ldxgi -ld2d1 -ldwrite -ldcomp -ldwmapi
         -lole32 -luuid -loleaut32 -lpowrprof -ldxva2 -luser32 -lgdi32 -lshell32 -lshcore -lcomctl32 -lwindowscodecs -lsecur32 -ladvapi32)

build_halo() {
  echo "== halo.exe"
  "$CXX" "${COMMON[@]}" -mwindows src/halo.cpp src/dock.cpp src/cluster.cpp src/icons.cpp src/apps.cpp src/winkey.cpp src/edge.cpp src/launcher.cpp src/previews.cpp \
    src/control_center.cpp src/notifications.cpp src/services.cpp -lwbemuuid -lwlanapi \
    -o "${HALO_OUT:-build/halo.exe}" "${WINLIBS[@]}"
}

build_guardian() {
  echo "== guardian.exe"
  "$CXX" "${COMMON[@]}" -mwindows src/guardian.cpp src/taskbar.cpp -o build/guardian.exe \
    -lshell32 -luser32 -lole32 -ladvapi32
}

build_icons_test() {
  echo "== icons_test.exe"
  "$CXX" "${COMMON[@]}" src/icons.cpp src/icons_test.cpp -o build/icons_test.exe \
    -lole32 -luuid -lshell32 -lcomctl32 -lwindowscodecs -lgdi32 -luser32
}

build_cluster_test() {
  echo "== cluster_test.exe"
  "$CXX" "${COMMON[@]}" src/cluster.cpp tools/cluster_test.cpp -o build/cluster_test.exe "${WINLIBS[@]}"
}

case "${1:-halo}" in
  halo) build_halo ;;
  guardian) build_guardian ;;
  icons_test) build_icons_test ;;
  cluster_test) build_cluster_test ;;
  all) build_halo; build_guardian; build_icons_test; build_cluster_test ;;
  *) echo "unknown target $1" >&2; exit 2 ;;
esac
