// Isolated native renderer: synthetic notifications only, no hooks or installed-shell changes.
#include <windows.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <dxgi.h>
#include <dispatcherqueue.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <psapi.h>
#include <cstdio>
#include "../src/notifications.h"
#include "../src/settings.h"

extern "C" HRESULT WINAPI D2D1CreateDevice(IDXGIDevice*, D2D1_CREATION_PROPERTIES const*, ID2D1Device**);
static void Memory(const char* stage) {
    PROCESS_MEMORY_COUNTERS_EX p{sizeof(p)};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&p), sizeof(p));
    std::printf("%s: private=%llu working=%llu\n", stage, static_cast<unsigned long long>(p.PrivateUsage),
                static_cast<unsigned long long>(p.WorkingSetSize));
    std::fflush(stdout);
}
int wmain(int argc, wchar_t** argv) {
    SetEnvironmentVariableW(L"HALO_TEST", L"1");
    if (argc > 1) SetEnvironmentVariableW(L"HALO_SETTINGS_FILE", argv[1]);
    halo::settings::Reload();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    DispatcherQueueOptions opt{sizeof(opt), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
    winrt::Windows::System::DispatcherQueueController dq{nullptr};
    winrt::check_hresult(CreateDispatcherQueueController(opt, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(dq))));
    auto comp = winrt::Windows::UI::Composition::Compositor();
    winrt::com_ptr<ID3D11Device> d3d;
    winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                         nullptr, 0, D3D11_SDK_VERSION, d3d.put(), nullptr, nullptr));
    auto dxgi = d3d.as<IDXGIDevice>();
    winrt::com_ptr<ID2D1Device> d2d;
    winrt::check_hresult(D2D1CreateDevice(dxgi.get(), nullptr, d2d.put()));
    winrt::Windows::UI::Composition::CompositionGraphicsDevice gdev{nullptr};
    auto interop = comp.as<ABI::Windows::UI::Composition::ICompositorInterop>();
    winrt::check_hresult(interop->CreateGraphicsDevice(reinterpret_cast<IUnknown*>(d2d.get()),
        reinterpret_cast<ABI::Windows::UI::Composition::ICompositionGraphicsDevice**>(winrt::put_abi(gdev))));
    Memory("before");
    halo::NotificationCenter panel(GetModuleHandleW(nullptr), nullptr, comp, gdev);
    panel.Open();
    const auto start = GetTickCount64();
    bool measured = false;
    SetTimer(nullptr, 1, 100, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!measured && GetTickCount64() - start > 2000) { Memory("open"); measured = true; }
        if (GetTickCount64() - start > 30000) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    panel.Close();
    const auto closing = GetTickCount64();
    while (GetTickCount64() - closing < 500) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
    }
    Memory("closed");
}
