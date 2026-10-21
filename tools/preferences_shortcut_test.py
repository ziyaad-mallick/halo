"""Run the actual shortcut-migration block against scratch files, never Start.

Usage: python tools/preferences_shortcut_test.py [--compiler PATH]
Generated fixture, executable and files remain under ignored build/ for inspection.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=os.environ.get("HALO_CXX"))
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    local = repo / "toolchain/llvm-mingw/bin/clang++.exe"
    if not local.exists():
        local = repo / "toolchain/audit-download/llvm-mingw-20250709-ucrt-x86_64/bin/clang++.exe"
    compiler = args.compiler or (str(local) if local.exists() else None) or shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        parser.error("pass --compiler with a Windows C++ compiler")
    source = (repo / "src/settings_app.cpp").read_text(encoding="utf-8")
    ensure = source.index("void EnsureShortcut() {")
    start = source.index("    std::wstring lnk =", ensure)
    end = source.index("    IShellLinkW* link =", start)
    migration = source[start:end]
    build = repo / "build"
    build.mkdir(exist_ok=True)
    scratch = Path(tempfile.mkdtemp(prefix="preferences-shortcut-", dir=build))
    fixture = r'''
#include <windows.h>
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <stdexcept>
namespace fs = std::filesystem;
void Check(bool ok, const char* label) {
    if (!ok) throw std::runtime_error(label);
}
void Write(fs::path p, const char* value) { std::ofstream(p) << value; }
std::string Read(fs::path p) {
    std::ifstream f(p);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
void Migrate(fs::path const& dir) {
    auto path = dir.wstring();
    PWSTR programs = static_cast<PWSTR>(CoTaskMemAlloc((path.size() + 1) * sizeof(wchar_t)));
    Check(programs != nullptr, "allocate scratch path");
    wcscpy(programs, path.c_str());
__MIGRATION__
}
int main(int argc, char** argv) {
    try {
        Check(argc == 2, "scratch root required");
        fs::path root = fs::absolute(argv[1]);
        for (int mode = 0; mode < 4; ++mode) {
            auto dir = root / std::to_string(mode);
            Check(fs::create_directory(dir), "fresh case directory");
            auto old = dir / "Halo Settings.lnk";
            auto current = dir / "Halo Preferences.lnk";
            auto backup = dir / "Halo Settings.lnk.bak";
            if (mode != 3) Write(old, "legacy shortcut bytes");
            if (mode == 1 || mode == 2) Write(current, "current shortcut bytes");
            if (mode == 2) Write(backup, "earlier backup bytes");
            Migrate(dir);
            if (mode == 0) {
                Check(!fs::exists(old) && !fs::exists(backup), "legacy moved only");
                Check(Read(current) == "legacy shortcut bytes", "legacy content preserved");
                std::cout << "PASS legacy-only rename preserves content\n";
            } else if (mode == 1) {
                Check(!fs::exists(old), "legacy moved to backup");
                Check(Read(current) == "current shortcut bytes", "new shortcut preserved");
                Check(Read(backup) == "legacy shortcut bytes", "backup content preserved");
                std::cout << "PASS old + new preserves new and backs up old\n";
            } else if (mode == 2) {
                Check(Read(old) == "legacy shortcut bytes", "collision preserves old");
                Check(Read(current) == "current shortcut bytes", "collision preserves new");
                Check(Read(backup) == "earlier backup bytes", "collision preserves backup");
                std::cout << "PASS backup collision preserves all files\n";
            } else {
                Check(fs::is_empty(dir), "no legacy creates nothing");
                std::cout << "PASS missing legacy does nothing\n";
            }
        }
    } catch (std::exception const& e) { std::cerr << e.what() << '\n'; return 1; }
}
'''.replace("__MIGRATION__", migration)
    cpp = scratch / "migration.cpp"
    exe = scratch / "migration.exe"
    cpp.write_text(fixture, encoding="utf-8")
    subprocess.run([compiler, "-std=c++17", "-static", str(cpp), "-lole32", "-o", str(exe)], check=True)
    subprocess.run([str(exe), str(scratch)], check=True)
    print(f"Artifacts retained: {scratch}")


if __name__ == "__main__":
    main()
