// Throwaway probe: bisects why the genie's scoped batch completes instantly.
// A. does a deliberately malformed expression throw at StartAnimation?
// B. does a trivial keyframe on a sprite hold a scoped batch open for its duration?
// C. do PropertySet-driven animations hold it open, and does a sprite bound to them start?
#include <windows.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <d3d11.h>
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
    fputws((std::wstring(L"[probe] ") + buf + L"\n").c_str(), stdout);
    fflush(stdout);
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

struct Timer {
    ULONGLONG t0 = GetTickCount64();
    bool fired = false;
};

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"HaloExprProbe";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"probe", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, inst, nullptr);

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
    wuc::Desktop::DesktopWindowTarget target{nullptr};
    auto interop = comp.as<ICompositorDesktopInterop>();
    winrt::check_hresult(interop->CreateDesktopWindowTarget(hwnd, TRUE, winrt::put_abi(target)));
    wuc::ContainerVisual root = comp.CreateContainerVisual();
    root.RelativeSizeAdjustment({1, 1});
    target.Root(root);

    auto pump = [&](DWORD ms) {
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
    };

    // A. malformed expression: throw at start, or silently accepted?
    {
        wuc::SpriteVisual s = comp.CreateSpriteVisual();
        s.Size({50, 50});
        s.Offset({10, 10, 0});
        root.Children().InsertAtTop(s);
        bool threw = false;
        try {
            s.StartAnimation(L"Offset", comp.CreateExpressionAnimation(L"This is (not valid"));
        } catch (winrt::hresult_error const& e) {
            threw = true;
            Log(L"A malformed expression THREW 0x%08lX %ls", (unsigned long)e.code().value, e.message().c_str());
        }
        if (!threw) Log(L"A malformed expression did NOT throw (silent start)");
        pump(50);
    }

    // B. trivial keyframe holds the batch for its duration?
    {
        wuc::CompositionScopedBatch batch = comp.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
        Timer timer;
        batch.Completed([&fired = timer.fired](auto&&, auto&&) { fired = true; });
        wuc::SpriteVisual s = comp.CreateSpriteVisual();
        s.Size({30, 30});
        s.Offset({0, 0, 0});
        root.Children().InsertAtTop(s);
        auto kf = comp.CreateVector3KeyFrameAnimation();
        kf.Duration(500ms);
        kf.InsertKeyFrame(1.0f, {100, 100, 0});
        s.StartAnimation(L"Offset", kf);
        batch.End();
        ULONGLONG t0 = GetTickCount64();
        while (!timer.fired && GetTickCount64() - t0 < 2500) pump(15);
        ULONGLONG ms = GetTickCount64() - t0;
        Log(L"B trivial keyframe batch: %ls after %llu ms (%ls)", timer.fired ? L"completed" : L"STILL OPEN", ms,
            ms >= 400 ? L"ok" : L"failed early");
    }

    // C. property set drives + a sprite bound to them: do they hold the batch and start?
    {
        wuc::CompositionScopedBatch batch = comp.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
        Timer timer;
        batch.Completed([&fired = timer.fired](auto&&, auto&&) { fired = true; });
        auto g = comp.CreatePropertySet();
        g.InsertScalar(L"t", 0.0f);
        g.InsertScalar(L"x", 0.0f);
        auto lin = comp.CreateScalarKeyFrameAnimation();
        lin.Duration(500ms);
        lin.InsertKeyFrame(1.0f, 1.0f, comp.CreateLinearEasingFunction());
        g.StartAnimation(L"t", lin);
        auto drive = comp.CreateExpressionAnimation(L"g.t * 200");
        drive.SetReferenceParameter(L"g", g);
        g.StartAnimation(L"x", drive);
        wuc::SpriteVisual s = comp.CreateSpriteVisual();
        s.Size({40, 40});
        s.Offset({0, 0, 0});
        root.Children().InsertAtTop(s);
        auto offExpr = comp.CreateExpressionAnimation(L"Vector3(g.x, 100, 0)");
        offExpr.SetReferenceParameter(L"g", g);
        bool startThrew = false;
        try {
            s.StartAnimation(L"Offset", offExpr);
        } catch (winrt::hresult_error const& e) {
            startThrew = true;
            Log(L"C sprite binding THREW 0x%08lX %ls", (unsigned long)e.code().value, e.message().c_str());
        }
        batch.End();
        ULONGLONG t0 = GetTickCount64();
        while (!timer.fired && GetTickCount64() - t0 < 2500) pump(15);
        ULONGLONG ms = GetTickCount64() - t0;
        Log(L"C property-set batch: %ls after %llu ms (%ls); sprite binding %ls", timer.fired ? L"completed" : L"STILL OPEN",
            ms, ms >= 400 ? L"ok" : L"failed early", startThrew ? L"THREW" : L"started");
    }

    // E. does a keyframe on a DETACHED visual hold the batch? (genie: anim->root is
    // inserted into the live tree only after batch.End())
    {
        wuc::CompositionScopedBatch batch = comp.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
        Timer timer;
        batch.Completed([&fired = timer.fired](auto&&, auto&&) { fired = true; });
        wuc::SpriteVisual s = comp.CreateSpriteVisual();  // never inserted into the tree
        s.Size({30, 30});
        s.Offset({0, 0, 0});
        auto kf = comp.CreateVector3KeyFrameAnimation();
        kf.Duration(500ms);
        kf.InsertKeyFrame(1.0f, {100, 100, 0});
        s.StartAnimation(L"Offset", kf);
        batch.End();
        ULONGLONG t0 = GetTickCount64();
        while (!timer.fired && GetTickCount64() - t0 < 2500) pump(15);
        ULONGLONG ms = GetTickCount64() - t0;
        Log(L"E detached-sprite batch: %ls after %llu ms (%ls)", timer.fired ? L"completed" : L"STILL OPEN", ms,
            ms >= 400 ? L"ok" : L"failed early");
    }

    // F. genie strip replica: the property-set drives plus the exact strip TransformMatrix
    // expression, on a live sprite. Reports the batch time AND the matrix after ~520 ms: if
    // the expression runs, the matrix is not the identity and its translation has moved.
    {
        wuc::CompositionScopedBatch batch = comp.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
        ULONGLONG batchT0 = GetTickCount64();
        bool fired = false;
        batch.Completed([&fired](auto&&, auto&&) { fired = true; });

        auto g = comp.CreatePropertySet();
        for (auto name : {L"t", L"e", L"k", L"j", L"f", L"hh", L"s0", L"s1", L"u", L"k7"}) g.InsertScalar(name, 0.0f);
        auto drive = [&](wchar_t const* prop, std::wstring const& text) {
            auto expr = comp.CreateExpressionAnimation(text);
            expr.SetReferenceParameter(L"g", g);
            g.StartAnimation(prop, expr);
        };
        auto num = [](float v) { wchar_t b[32]; swprintf(b, 32, L"%.7g", v); return std::wstring(b); };

        // genie_selftest's minimize rects, screen=1920x1200:
        RECT const window{1460, 640, 1860, 1140}, to{1593, 1096, 1637, 1140};
        float W = 400, H = 500, rows = 6, y0 = 0;
        float winx = (float)window.left, winy = (float)window.top;
        float tox = (float)to.left, toy = (float)to.top, tow = (float)(to.right - to.left);
        float expand = toy - winy - H, dw = W - tow, cA = winx - tox, cB = winx + W - tox - tow;
        drive(L"e", L"3*g.t*g.t - 2*g.t*g.t*g.t");
        drive(L"k", L"Clamp(g.e / 0.3, 0, 1)");
        drive(L"j", L"Clamp((g.e - 0.3) / 0.7, 0, 1)");
        drive(L"f", L"(" + num(toy - winy) + L") - (" + num(expand) + L") * (1 - g.k)");
        drive(L"hh", L"g.f * (1 - g.j)");
        drive(L"s0", L"1 - g.j");
        drive(L"s1", L"1 - g.k");
        drive(L"u", L"g.hh / (g.f + 0.1) * g.k");
        drive(L"k7", L"g.k / 7");
        auto lin = comp.CreateScalarKeyFrameAnimation();
        lin.Duration(500ms);
        lin.InsertKeyFrame(1.0f, 1.0f, comp.CreateLinearEasingFunction());
        g.StartAnimation(L"t", lin);

        auto X = [&](int tx, float ty) {
            std::wstring t = num(ty), r = L"(1 - " + t + L")";
            return L"(" + num(winx) + L" + " + std::to_wstring(tx) + L" * (" + num(tow) + L" + " + num(dw) +
                   L" * (g.s0 * " + r + L" + g.s1 * " + t + L")) + (" + num(tox - winx) + L") * (" + t +
                   L" * g.u + g.j) - Sin(6.2831853 * " + r + L" * g.s0) * (" + num(tx ? cB : cA) + L") * g.k7)";
        };
        auto Y = [&](float ty) { return L"(" + num(winy) + L" + g.f - g.hh * (1 - " + num(ty) + L"))"; };
        float t0 = y0 / H, t1 = (y0 + rows) / H;
        std::wstring mat = L"Matrix4x4((" + X(1, t0) + L" - " + X(0, t0) + L") / " + num(W) + L", 0, 0, 0, (" + X(0, t1) +
                           L" - " + X(0, t0) + L") / " + num(rows) + L", (" + Y(t1) + L" - " + Y(t0) + L") / " +
                           num(rows) + L", 0, 0, 0, 0, 1, 0, " + X(0, t0) + L", " + Y(t0) + L", 0, 1)";
        Log(L"F strip expression (%d chars)", (int)mat.size());

        wuc::SpriteVisual s = comp.CreateSpriteVisual();
        s.Size({W, rows + 1});
        s.Offset({0, 0, 0});
        root.Children().InsertAtTop(s);
        auto expr = comp.CreateExpressionAnimation(mat);
        expr.SetReferenceParameter(L"g", g);
        bool stripThrew = false;
        try {
            s.StartAnimation(L"TransformMatrix", expr);
        } catch (winrt::hresult_error const& e) {
            stripThrew = true;
            Log(L"F strip TransformMatrix THREW 0x%08lX %ls", (unsigned long)e.code().value, e.message().c_str());
        }
        batch.End();
        ULONGLONG t0f = GetTickCount64();
        while (!fired && GetTickCount64() - t0f < 2500) pump(15);
        ULONGLONG ms = GetTickCount64() - t0f;
        winrt::Windows::Foundation::Numerics::float4x4 m = s.TransformMatrix();
        Log(L"F full replica batch: %ls after %llu ms (%ls); strip %ls; matrix now m41=%.1f m42=%.1f m11=%.3f m21=%.3f",
            fired ? L"completed" : L"STILL OPEN", ms, ms >= 400 ? L"ok" : L"failed early",
            stripThrew ? L"THREW" : L"started", m.m41, m.m42, m.m11, m.m21);
        ULONGLONG after = GetTickCount64() - batchT0;
        Log(L"F total since batch create: %llu ms", after);
    }

    // G. same as C's sprite, but on a window that is WS_EX_LAYERED like the genie overlay
    // (Genie creates its overlay with WS_EX_LAYERED + WS_EX_TRANSPARENT and
    // SetLayeredWindowAttributes). Does the layered target hold animation batches?
    {
        HWND lw = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                                      WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                  wc.lpszClassName, L"layered", WS_POPUP, 0, 0, 300, 200, nullptr, nullptr, inst,
                                  nullptr);
        SetLayeredWindowAttributes(lw, 0, 255, LWA_ALPHA);
        wuc::Desktop::DesktopWindowTarget ltarget{nullptr};
        auto linterop = comp.as<ICompositorDesktopInterop>();
        winrt::check_hresult(linterop->CreateDesktopWindowTarget(lw, TRUE, winrt::put_abi(ltarget)));
        wuc::ContainerVisual lroot = comp.CreateContainerVisual();
        lroot.RelativeSizeAdjustment({1, 1});
        ltarget.Root(lroot);

        wuc::CompositionScopedBatch batch = comp.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
        Timer timer;
        batch.Completed([&fired = timer.fired](auto&&, auto&&) { fired = true; });
        wuc::SpriteVisual s = comp.CreateSpriteVisual();
        s.Size({40, 40});
        s.Offset({0, 0, 0});
        lroot.Children().InsertAtTop(s);
        auto offExpr = comp.CreateExpressionAnimation(L"Vector3(g2.x, 100, 0)");
        wuc::CompositionPropertySet g2 = comp.CreatePropertySet();
        g2.InsertScalar(L"x", 0.0f);
        offExpr.SetReferenceParameter(L"g2", g2);
        auto lin = comp.CreateScalarKeyFrameAnimation();
        lin.Duration(500ms);
        lin.InsertKeyFrame(1.0f, 1.0f, comp.CreateLinearEasingFunction());
        g2.StartAnimation(L"x", lin);
        s.StartAnimation(L"Offset", offExpr);
        batch.End();
        Log(L"G layered target created over class %ls", L"HaloExprProbe");
        ULONGLONG t0g = GetTickCount64();
        while (!timer.fired && GetTickCount64() - t0g < 2500) pump(15);
        ULONGLONG msg = GetTickCount64() - t0g;
        Log(L"G layered-window batch: %ls after %llu ms (%ls)", timer.fired ? L"completed" : L"STILL OPEN", msg,
            msg >= 400 ? L"ok" : L"failed early");
        DestroyWindow(lw);
    }

    Log(L"probe done");
    return 0;
}
