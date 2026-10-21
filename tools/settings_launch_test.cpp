#include "../src/shell_launch.h"
#include <cstdio>
#include <vector>

struct Fake {
    HRESULT activation = S_OK, shell = S_OK;
    std::vector<std::wstring> calls;
    int errors = 0;
    HRESULT Activate() { calls.push_back(L"activate"); return activation; }
    HRESULT Shell(std::wstring const& target) { calls.push_back(target); return shell; }
    void Error(std::wstring const&, HRESULT) { ++errors; }
};
int wmain() {
    int failed = 0;
    auto check = [&](bool ok, char const* name) {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
        failed += !ok;
    };
    Fake root;
    check(halo::launch::Open(L"ms-settings:", root) && root.calls == std::vector<std::wstring>{L"activate"},
          "root Settings uses app activation without URI on success");
    Fake fallback; fallback.activation = E_ACCESSDENIED;
    check(halo::launch::Open(halo::launch::kSettingsTarget, fallback) &&
          fallback.calls == std::vector<std::wstring>{L"activate", L"ms-settings:"} && !fallback.errors,
          "Settings app activation failure falls back to URI");
    Fake denied; denied.activation = E_FAIL; denied.shell = E_ACCESSDENIED;
    check(!halo::launch::Open(L"ms-settings:", denied) && denied.errors == 1,
          "both Settings launch failures produce one visible error");
    Fake page;
    check(halo::launch::Open(L"ms-settings:privacy-location", page) &&
          page.calls == std::vector<std::wstring>{L"ms-settings:privacy-location"},
          "deep Settings page retains its URI");
    Fake missing; missing.shell = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    check(!halo::launch::Open(L"missing-file.txt", missing) && missing.errors == 1,
          "ordinary launch failure produces visible error");
    std::printf("%d failure(s)\n", failed);
    return failed ? 1 : 0;
}
