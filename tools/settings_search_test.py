"""Compile actual launcher search code against deterministic, headless fixtures.

python tools/settings_search_test.py --revision HEAD  # historical regression
python tools/settings_search_test.py                 # working tree
No GUI, installed applications, or user files are accessed.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def extract(source, signature):
    match = re.search(re.escape(signature) + r"\s*\{.*?^\}", source, re.M | re.S)
    if not match:
        raise RuntimeError(f"Cannot extract production function: {signature}")
    return match.group()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revision", help="Read launcher.cpp from a git revision")
    compiler = ROOT / "toolchain/llvm-mingw/bin/clang++.exe"
    if not compiler.exists():
        compiler = ROOT / "toolchain/audit-download/llvm-mingw-20250709-ucrt-x86_64/bin/clang++.exe"
    parser.add_argument("--compiler", type=Path, default=os.environ.get("HALO_CXX", compiler))
    args = parser.parse_args()
    source = (subprocess.check_output(["git", "show", f"{args.revision}:src/launcher.cpp"], cwd=ROOT).decode("utf-8")
              if args.revision else (ROOT / "src/launcher.cpp").read_text(encoding="utf-8"))
    pages = re.search(r"struct SettingsPage\s*\{.*?^\};\s*constexpr SettingsPage kSettingsPages\[\] = \{.*?^\};", source, re.M | re.S)
    if not pages:
        raise RuntimeError("Cannot extract production Settings pages")
    code = r'''
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <iostream>
#include <string>
#include <vector>
'''
    code += extract(source, "std::wstring Lower(std::wstring s)") + "\n"
    code += extract(source, "int MatchRank(std::wstring const& name, std::wstring const& q)") + "\n"
    code += pages.group() + "\n"
    code += r'''
struct Launcher {
    struct Result {
        enum class Type { App, Setting, File } type;
        std::wstring name, path;
    };
    struct AppEntry { std::wstring name, lname, path; };
    struct File { std::wstring name, path; };
    std::wstring m_query;
    bool m_allApps = false;
    std::vector<AppEntry> m_index;
    std::vector<File> m_allFiles;
    std::vector<Result> m_matches;
    void ShowMatches() {} // Pagination is outside this ranking test.
    void RunSearch();
};
'''
    code += extract(source, "void Launcher::RunSearch()") + "\n"
    code += r'''
int main() {
    Launcher l;
    // Include an exact-name app: prior tie-breaking puts apps before Settings pages.
    for (auto name : {L"Halo Settings", L"Halo Preferences", L"Settings", L"Calculator"})
        l.m_index.push_back({name, Lower(name), L"fixture-app"});
    l.m_allFiles = {{L"Settings", L"fixture-file"}, {L"Settings notes", L"fixture-notes"}};
    int failures = 0, checks = 0;
    auto check = [&](bool ok, const char* name) {
        ++checks;
        std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
        if (!ok) ++failures;
    };
    for (auto query : {L"settings", L"SeTTings", L"settings   "}) {
        l.m_query = query;
        l.RunSearch();
        check(!l.m_matches.empty() && l.m_matches[0].type == Launcher::Result::Type::Setting &&
              l.m_matches[0].path == L"ms-settings:", "canonical Settings ranks first (case/trailing spaces)");
        check(std::any_of(l.m_matches.begin(), l.m_matches.end(), [](auto& r) { return r.name == L"Halo Settings"; }),
              "Halo Settings remains discoverable");
    }
    l.m_query = L"halo";
    l.RunSearch();
    check(l.m_matches.size() == 2 && l.m_matches[0].name == L"Halo Settings" &&
          l.m_matches[1].name == L"Halo Preferences", "Halo names retain normal ranking");
    l.m_query = L"calculator";
    l.RunSearch();
    check(l.m_matches.size() == 1 && l.m_matches[0].name == L"Calculator", "unrelated exact app search unchanged");
    l.m_query.clear();
    l.RunSearch();
    check(l.m_matches.empty(), "empty home query has no search results");
    l.m_allApps = true;
    l.RunSearch();
    check(l.m_matches.size() == l.m_index.size(), "All apps preserves every indexed app");
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="halo-settings-search-") as tmp:
        cpp, exe = Path(tmp) / "search.cpp", Path(tmp) / "search.exe"
        cpp.write_text(code, encoding="utf-8")
        subprocess.run([str(args.compiler), "-std=c++20", "-static", str(cpp), "-o", str(exe)], check=True)
        print(f"Source: {args.revision or 'working tree'}", flush=True)
        return subprocess.run([str(exe)]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
