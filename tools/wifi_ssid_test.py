#!/usr/bin/env python3
"""Compile the production ConnectedSsid() body against headless WinRT fakes."""
from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
CLANG = ROOT / "toolchain/audit-download/llvm-mingw-20250709-ucrt-x86_64/bin/clang++.exe"
OUT = ROOT / "build/wifi-ssid-test"

FAKE = r'''#include <string>
#include <vector>
#include <cassert>
#include <cstdio>
namespace winrt::Windows::Networking::Connectivity {
enum class NetworkConnectivityLevel { None, LocalAccess, ConstrainedInternetAccess, InternetAccess };
struct Details { std::wstring ssid; std::wstring GetConnectedSsid() const { return ssid; } explicit operator bool() const { return true; } };
struct Profile {
    bool wlan = false;
    NetworkConnectivityLevel level = NetworkConnectivityLevel::None;
    std::wstring name;
    Details details;
    bool throwDetails = false;
    bool IsWlanConnectionProfile() const { return wlan; }
    NetworkConnectivityLevel GetNetworkConnectivityLevel() const { return level; }
    Details WlanConnectionProfileDetails() const { if (throwDetails) throw 1; return details; }
    std::wstring ProfileName() const { return name; }
    explicit operator bool() const { return true; }
};
inline std::vector<Profile> profiles;
struct NetworkInformation {
    static std::vector<Profile> GetConnectionProfiles() { return profiles; }
    static Profile GetInternetConnectionProfile() { return profiles.empty() ? Profile{} : profiles.back(); }
};
}
using winrt::Windows::Networking::Connectivity::NetworkConnectivityLevel;
using winrt::Windows::Networking::Connectivity::Profile;
using winrt::Windows::Networking::Connectivity::NetworkInformation;
'''

CASES = r'''
using namespace winrt::Windows::Networking::Connectivity;
static Profile p(bool wlan, NetworkConnectivityLevel level, const wchar_t* name, const wchar_t* ssid) {
    Profile x; x.wlan=wlan; x.level=level; x.name=name; x.details.ssid=ssid; return x;
}
int main() {
    int failures=0;
    auto expect=[&](bool ok,const char* name){printf("%s: %s\n",ok?"PASS":"FAIL",name);failures+=!ok;};
    // Ethernet/VPN is the default route; Wi-Fi only has local access. Its profile name is not its SSID.
    profiles = {p(true, NetworkConnectivityLevel::LocalAccess, L"LocalAccess profile", L"Cafe WiFi"),
                p(false, NetworkConnectivityLevel::InternetAccess, L"VPN default", L"")};
    expect(ConnectedSsid() == L"Cafe WiFi", "offline WLAN SSID with VPN default and renamed profile");
    // No WLAN profile.
    profiles = {p(false, NetworkConnectivityLevel::InternetAccess, L"Ethernet", L"")};
    expect(ConnectedSsid().empty(), "Ethernet only");
    // A disconnected WLAN must not be reported, even when it is listed before an active non-WLAN route.
    profiles = {p(true, NetworkConnectivityLevel::None, L"Saved WiFi", L"Stale SSID"),
                p(false, NetworkConnectivityLevel::InternetAccess, L"VPN", L"")};
    expect(ConnectedSsid().empty(), "disconnected WLAN excluded");
    // Empty details and a per-profile failure must not prevent checking a later active WLAN.
    profiles = {p(true, NetworkConnectivityLevel::LocalAccess, L"No details", L""),
                p(true, NetworkConnectivityLevel::LocalAccess, L"Valid later", L"Later SSID")};
    expect(ConnectedSsid() == L"Later SSID", "empty SSID does not hide later adapter");
    profiles[0].throwDetails = true;
    expect(ConnectedSsid() == L"Later SSID", "failed adapter does not hide later adapter");
    return failures?1:0;
}
'''


def source_for(revision: str | None) -> str:
    if revision:
        return subprocess.check_output(
            ["git", "show", f"{revision}:src/services.cpp"], cwd=ROOT, text=True
        )
    return (ROOT / "src/services.cpp").read_text(encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--revision", help="extract source from a git revision")
    args = parser.parse_args()
    source = source_for(args.revision)
    match = re.search(r"std::wstring ConnectedSsid\(\)\s*\{.*?\n\}", source, re.S)
    if not match:
        print("FAIL: ConnectedSsid() definition not found", file=sys.stderr)
        return 2
    OUT.mkdir(parents=True, exist_ok=True)
    cpp = OUT / "wifi_ssid_fixture.cpp"
    exe = OUT / "wifi_ssid_fixture.exe"
    cpp.write_text(FAKE + "\n" + match.group(0) + "\n" + CASES, encoding="utf-8")
    cmd = [str(CLANG), "-std=c++23", "-O2", "-static", "-target", "x86_64-w64-mingw32", str(cpp), "-o", str(exe)]
    built = subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True)
    if built.returncode:
        print("FAIL: fixture compile\n" + built.stdout + built.stderr, file=sys.stderr)
        return built.returncode
    try:
        run = subprocess.run([str(exe)], cwd=ROOT, text=True, capture_output=True)
    except OSError as exc:
        print(f"UNVERIFIED: fixture compiled, but Windows blocked execution: {exc}", file=sys.stderr)
        return 2
    label = args.revision or "working tree"
    print(run.stdout + run.stderr, end='')
    if run.returncode:
        print(f"FAIL: {label} ConnectedSsid() failed fixture (exit {run.returncode})")
        return 1
    print(f"PASS: {label} ConnectedSsid() passed 5 fixtures")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
