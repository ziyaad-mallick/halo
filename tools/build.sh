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
  "$CXX" "${COMMON[@]}" -mwindows src/halo.cpp src/dock.cpp src/cluster.cpp src/icons.cpp src/apps.cpp src/winkey.cpp src/edge.cpp src/launcher.cpp src/previews.cpp src/menu.cpp src/pins.cpp src/swoop.cpp\
    src/control_center.cpp src/notifications.cpp src/services.cpp src/settings.cpp -lwbemuuid -lwlanapi -lpsapi \
    -o "${HALO_OUT:-build/halo.exe}" "${WINLIBS[@]}"
}

build_guardian() {
  echo "== guardian.exe"
  "$CXX" "${COMMON[@]}" -mwindows src/guardian.cpp src/taskbar.cpp -o build/guardian.exe \
    -lshell32 -luser32 -lole32 -ladvapi32
}

build_settings() {
  echo "== halo-settings.exe"
  "$CXX" "${COMMON[@]}" -Wno-c23-extensions -mwindows src/settings_app.cpp src/settings.cpp res/halo-settings.res -o "${HALO_SETTINGS_OUT:-build/halo-settings.exe}" -lws2_32 -lpdh -lpsapi -lshell32 -luser32 -lole32 -luuid -ladvapi32 -lshlwapi
}

build_icons_test() {
  echo "== icons_test.exe"
  "$CXX" "${COMMON[@]}" src/icons.cpp src/icons_test.cpp -o build/icons_test.exe \
    -lole32 -luuid -lshell32 -lcomctl32 -lwindowscodecs -lgdi32 -luser32
}

build_pins_test() {
  echo "== pins_test.exe"
  "$CXX" "${COMMON[@]}" src/pins.cpp src/icons.cpp tools/pins_test.cpp -o build/pins_test.exe \
    -lole32 -luuid -lshell32 -lcomctl32 -lwindowscodecs -lgdi32 -luser32 -ladvapi32
}

build_cluster_test() {
  echo "== cluster_test.exe"
  "$CXX" "${COMMON[@]}" src/cluster.cpp src/settings.cpp tools/cluster_test.cpp -o build/cluster_test.exe "${WINLIBS[@]}"
}

build_swoop_test() {
  echo "== swoop_test.exe"
  "$CXX" "${COMMON[@]}" -DHALO_SWOOP_TEST src/swoop.cpp tools/swoop_test.cpp -o "${SWOOP_TEST_OUT:-build/swoop_test.exe}" -ldwmapi -lpsapi -luser32 -lgdi32
}

case "${1:-halo}" in
  halo) build_halo ;;
  guardian) build_guardian ;;
  settings) build_settings ;;
  icons_test) build_icons_test ;;
  pins_test) build_pins_test ;;
  cluster_test) build_cluster_test ;;
  swoop_test) build_swoop_test ;;
  all) build_halo; build_guardian; build_settings; build_icons_test; build_cluster_test ;;
  *) echo "unknown target $1" >&2; exit 2 ;;
esac
