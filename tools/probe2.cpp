// Throwaway probe 2: when do animations hold a scoped batch?
// H: only animations on visuals connected to a composed (visible) window hold it.
// Cases, each with a 500 ms property-set clock + a sprite bound to it:
//   i1 sprite detached, never attached            -> expect: instant (genie's current bug)
//   i2 sprite attached to a shown window, started after attach -> expect: ~500 ms
//   i3 started detached, attached right before End, window shown  -> hold? (candidate fix)
//   i4 like i3 but the window stays hidden        -> hold? (attach-but-invisible)
#include <windows.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <dispatcherqueue.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <stdio.h>
#include <chrono>
#include <string>
#include "../src/interop.h"

extern "C" HRESULT WINAPI D2D1CreateDevice(IDXGIDevice*, D2D1_CREATION_PROPERTIES const*, ID2D1Device**);

namespace wuc = winrt::Windows::UI::Composition;
using namespace std::chrono_literals;

static void Log(wchar_t const* fmt, ...) {
    wchar_t buf[400];
    va_list args;
    va_start(args, fmt);
    vswprintf(buf, 400, fmt, args);
    va_end(args);
    fputws((std::wstring(L"[probe2] ") + buf + L"\n").c_str(), stdout);
    fflush(stdout);
}

static void Pump(DWORD ms) {
    DWORD start = GetTickCount();
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (GetTickCount() - start >= ms) return;
        DWORD left = ms - (GetTickCount() - start);
        if (left) Sleep(left < 4 ? left : 4);
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"HaloProbe2";
    RegisterClassW(&wc);

    winrt::com_ptr<ID3D11Device> d3d;
    winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                           nullptr, 0, D3D11_SDK_VERSION, d3d.put(), nullptr, nullptr));
    winrt::com_ptr<ID2D1Device> d2d;
    winrt::check_hresult(D2D1CreateDevice(d3d.as<IDXGIDevice>().get(), nullptr, d2d.put()));

    DispatcherQueueOptions opt{sizeof(opt), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
    winrt::Windows::System::DispatcherQueueController dq{nullptr};
    winrt::check_hresult(CreateDispatcherQueueController(
        opt, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(dq))));

    wuc::Compositor comp;
    // A real visible window parked at the bottom-right, 100x100: composition here composes
    // like the genie overlay would.
    HWND vis = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                               wc.lpszClassName, L"probe2 visible", WS_POPUP, 1400, 1000, 100, 100, nullptr, nullptr,
                               inst, nullptr);
    wuc::Desktop::DesktopWindowTarget vtarget{nullptr};
    auto interop = comp.as<ICompositorDesktopInterop>();
    winrt::check_hresult(interop->CreateDesktopWindowTarget(vis, TRUE, winrt::put_abi(vtarget)));
    wuc::ContainerVisual vroot = comp.CreateContainerVisual();
    vroot.RelativeSizeAdjustment({1, 1});
    vtarget.Root(vroot);
    ShowWindow(vis, SW_SHOWNOACTIVATE);

    auto pump = [](DWORD ms) { Pump(ms); };

    auto Case = [&](wchar_t const* label, bool attachBeforeStart, bool attachLate, bool showLate) {
        try {
            wuc::ContainerVisual holder = comp.CreateContainerVisual();  // the "anim root"
            holder.RelativeSizeAdjustment({1, 1});
            wuc::CompositionPropertySet g = comp.CreatePropertySet();
            g.InsertScalar(L"t", 0.0f);

            wuc::CompositionScopedBatch batch = comp.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
            bool fired = false;
            batch.Completed([&fired](auto&&, auto&&) { fired = true; });

            wuc::SpriteVisual s = comp.CreateSpriteVisual();
            s.Size({40, 40});
            s.Brush(comp.CreateColorBrush({0, 200, 80, 255}));
            if (attachBeforeStart) {
                vroot.Children().InsertAtTop(holder);
                holder.Children().InsertAtTop(s);
            }
            auto lin = comp.CreateScalarKeyFrameAnimation();
            lin.Duration(500ms);
            lin.InsertKeyFrame(1.0f, 1.0f, comp.CreateLinearEasingFunction());
            g.StartAnimation(L"t", lin);
            auto expr = comp.CreateExpressionAnimation(L"Vector3(g.t * 300, g.t * 10, 0)");
            expr.SetReferenceParameter(L"g", g);
            s.StartAnimation(L"Offset", expr);
            if (attachLate) {
                vroot.Children().InsertAtTop(holder);
                holder.Children().InsertAtTop(s);
            }
            if (showLate) ShowWindow(vis, SW_SHOWNOACTIVATE);
            batch.End();
            ULONGLONG t0 = GetTickCount64();
            while (!fired && GetTickCount64() - t0 < 1500) pump(20);
            Log(L"%-24ls batch %ls after %llu ms", label, fired ? L"completed" : L"STILL OPEN (>=1500)",
                GetTickCount64() - t0);
            vroot.Children().Remove(holder);
        } catch (winrt::hresult_error const& e) {
            Log(L"%-24ls THREW 0x%08lX %ls", label, (unsigned long)e.code().value, e.message().c_str());
        }
    };

    Case(L"i1 detach, no attach", false, false, false);
    Case(L"i2 attach, start, visible", true, false, false);
    Case(L"i3 start detached, attach, shown", false, true, false);
    Case(L"i4 start detached, attach, hidden->shown", false, true, true);
    Log(L"probe2 done");
    return 0;
}
