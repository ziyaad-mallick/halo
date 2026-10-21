#pragma once
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <cwchar>
#include <string>
#include <thread>

namespace halo::launch {
inline constexpr wchar_t kSettingsAumid[] =
    L"windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel";
inline constexpr wchar_t kSettingsTarget[] =
    L"shell:AppsFolder\\windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel";

// Keep routing testable without launching apps or changing the live desktop.
template <class Backend>
bool Open(std::wstring const& target, Backend& backend) {
    HRESULT hr;
    if (_wcsicmp(target.c_str(), L"ms-settings:") == 0 || _wcsicmp(target.c_str(), kSettingsTarget) == 0) {
        hr = backend.Activate();
        if (SUCCEEDED(hr)) return true;
        hr = backend.Shell(L"ms-settings:");
    } else {
        hr = backend.Shell(target);
    }
    if (SUCCEEDED(hr)) return true;
    backend.Error(target, hr);
    return false;
}

struct Native {
    HRESULT Activate() {
        IApplicationActivationManager* manager = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER,
                                      IID_PPV_ARGS(&manager));
        if (SUCCEEDED(hr)) {
            DWORD pid = 0;
            CoAllowSetForegroundWindow(manager, nullptr);
            hr = manager->ActivateApplication(kSettingsAumid, nullptr, AO_NOERRORUI, &pid);
            manager->Release();
        }
        return hr;
    }
    HRESULT Shell(std::wstring const& target) {
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        // CC also launches from a short-lived worker with no message loop.
        sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
        sei.lpVerb = L"open";
        sei.lpFile = target.c_str();
        sei.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&sei)) return S_OK;
        DWORD error = GetLastError();
        return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
    }
    void Error(std::wstring const& target, HRESULT hr) {
        wchar_t code[16];
        swprintf(code, 16, L"0x%08lX", static_cast<unsigned long>(hr));
        std::wstring text = L"Halo could not open:\n" + target + L"\n\nError " + code + L". Try again from Windows Start (Ctrl+Esc).";
        OutputDebugStringW((text + L"\n").c_str());
        // Never block the shell's input/composition thread on a modal error dialog.
        std::thread([text = std::move(text)] {
            MessageBoxW(nullptr, text.c_str(), L"Halo — could not open", MB_OK | MB_ICONWARNING);
        }).detach();
    }
};

inline bool Open(std::wstring const& target) {
    Native backend;
    return Open(target, backend);
}
}  // namespace halo::launch
