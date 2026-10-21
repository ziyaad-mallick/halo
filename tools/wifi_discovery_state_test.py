#!/usr/bin/env python3
"""Compile the production FinishWifi() body against a small headless state fixture.

Run: python tools/wifi_discovery_state_test.py
Includes partial multi-adapter success, permission failures, and cancellation.
"""
from __future__ import annotations

import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
CLANG = ROOT / "toolchain/audit-download/llvm-mingw-20250709-ucrt-x86_64/bin/clang++.exe"
OUT = ROOT / "build/wifi-discovery-state-test"

FIXTURE = r'''#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../../src/radio_policy.h"

namespace halo::svc {
using radio_policy::DiscoveryPhase;

struct WifiNet { bool connected = false; };
struct DiscoveryStatus {
    DiscoveryPhase phase = DiscoveryPhase::Idle;
    DWORD error = ERROR_SUCCESS;
};
struct ScanSignal {};

std::atomic<unsigned> g_wifiGen{0};
std::mutex g_wifiMutex;
std::vector<WifiNet> g_wifiNets;
DiscoveryStatus g_wifiStatus;
std::weak_ptr<ScanSignal> g_activeWifiScan;
int g_posts = 0;
}

#define PostMessageW(hwnd, msg, wparam, lparam) (++halo::svc::g_posts)
'''

CASES = r'''
int main() {
    using namespace halo::svc;
    int failures = 0;
    auto expect = [&](bool ok, const char* label) {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    auto run = [&](unsigned gen, std::vector<WifiNet> nets, DWORD error, bool hadAdapter,
                   bool hadSuccess = false) {
        g_activeWifiScan = std::make_shared<ScanSignal>();
        FinishWifi(gen, reinterpret_cast<HWND>(1), 7, std::move(nets), error, hadAdapter{had_success_arg});
    };

    // A completion from an older scan must not replace the current snapshot or notify the UI.
    g_wifiGen = 8;
    g_wifiNets = {{true}};
    g_wifiStatus = {DiscoveryPhase::Loading, ERROR_SUCCESS};
    g_posts = 0;
    run(7, {{false}}, ERROR_GEN_FAILURE, true);
    expect(g_wifiNets.size() == 1 && g_wifiNets[0].connected &&
           g_wifiStatus.phase == DiscoveryPhase::Loading && g_posts == 0,
           "stale scan generation ignored");

    g_wifiGen = 1;
    run(1, {}, ERROR_ACCESS_DENIED, true);
    expect(g_wifiStatus.phase == DiscoveryPhase::AccessDenied && g_wifiStatus.error == ERROR_ACCESS_DENIED,
           "access denied status");

    run(1, {}, ERROR_NOT_FOUND, false);
    expect(g_wifiStatus.phase == DiscoveryPhase::Unavailable && g_wifiStatus.error == ERROR_NOT_FOUND,
           "no adapters unavailable");

    run(1, {{false}, {true}}, ERROR_SUCCESS, true);
    expect(g_wifiStatus.phase == DiscoveryPhase::Ready && g_wifiStatus.error == ERROR_SUCCESS &&
           g_wifiNets.size() == 2 && g_wifiNets.front().connected,
           "successful scan ready and connected network sorted first");

    // At least one adapter succeeded, so retain the useful rows and report Ready while preserving
    // the other adapter's error DWORD. This expectation fails against the pre-fix six-arg body.
    run(1, {{false}, {true}}, ERROR_GEN_FAILURE, true, true);
    expect(g_wifiStatus.phase == DiscoveryPhase::Ready && g_wifiStatus.error == ERROR_GEN_FAILURE &&
           g_wifiNets.size() == 2,
           "partial multi-adapter scan ready with diagnostic error retained");
    return failures ? 1 : 0;
}
'''


def main() -> int:
    source = (ROOT / "src/services.cpp").read_text(encoding="utf-8")
    match = re.search(r"void\s+FinishWifi\((?P<signature>[^)]*)\)\s*\{.*?\n\}", source, re.S)
    if not match:
        print("FAIL: FinishWifi() definition not found", file=sys.stderr)
        return 2

    signature = match.group("signature")
    has_success_arg = bool(re.search(r"\bbool\s+hadSuccess\b", signature))
    had_success_arg = ", hadSuccess" if has_success_arg else ""
    cases = CASES.replace("{had_success_arg}", had_success_arg)

    OUT.mkdir(parents=True, exist_ok=True)
    cpp = OUT / "wifi_discovery_state_fixture.cpp"
    exe = OUT / "wifi_discovery_state_fixture.exe"
    # Keep the actual production signature and body. Test calls account for a future optional
    # hadSuccess final parameter without requiring any production-source edits in this revision.
    # The mock macro must be visible before the actual production function definition.
    marker = "int g_posts = 0;\n}\n\n#define PostMessageW(hwnd, msg, wparam, lparam) (++halo::svc::g_posts)\n"
    insertion = marker + "namespace halo::svc {\n" + match.group(0) + "\n}\n"
    fixture = FIXTURE.replace(marker, insertion)
    cpp.write_text(fixture + "\n" + cases, encoding="utf-8")

    command = [str(CLANG), "-std=c++23", "-O2", "-static", "-target", "x86_64-w64-mingw32",
               str(cpp), "-o", str(exe)]
    built = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
    if built.returncode:
        print("FAIL: fixture compile\n" + built.stdout + built.stderr, file=sys.stderr)
        return built.returncode
    try:
        run_result = subprocess.run([str(exe)], cwd=ROOT, text=True, capture_output=True)
    except OSError as exc:
        print(f"UNVERIFIED: fixture compiled, but Windows blocked execution: {exc}", file=sys.stderr)
        return 2
    print(run_result.stdout + run_result.stderr, end="")
    if run_result.returncode:
        print("FAIL: FinishWifi() discovery state fixture exposed a failing expectation")
        return 1
    print("PASS: FinishWifi() discovery state fixture passed 5 cases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
