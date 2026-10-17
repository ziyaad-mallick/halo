// Plays the GPU genie end to end in one process, no mod and no live Halo needed.
// It does what HaloStart (mods/halo-genie.wh.cpp) does: writes a GenieHeader and a
// snapshot into the Local\HaloGenie.<id> file mapping, creates the .ready/.done events
// with a null DACL (id = pid<<32|seq, the mod's format), then calls Genie::OnMessage(1, id)
// and waits the way the mod waits. A minimize (kind 0) and a restore (kind 1) run in the
// empty bottom-right corner of the primary monitor, below the live halo.exe's dock in z.
// Nothing is launched and nothing runs on a timer.
//
// Before the animations it draws two markers on the same region so a desktop-capture run
// (tools/genie-shots.sh) can tell "the genie strips never rendered" apart from "the whole
// overlay never rendered": a static red rect (400 ms) and a keyframe-animated blue square
// (300 ms), both known-good composition recipes.
//
//   bash tools/build.sh genie_selftest
//   tools/genie-shots.sh 1440,620,440,560        # the region the markers + animation use
// Or by hand:
//   build/framecap.exe 6000 build\selftest.csv build\selftest-shots <x> <y> <w> <h> 1 &
//   build/genie_selftest.exe
// Expected on stdout: markers ok, then ready/done PASS lines per animation.
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
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <stdio.h>
#include <chrono>
#include <string>
#include <vector>
#include "../src/genie.h"
#include "../src/interop.h"
#include <dwmapi.h>

using namespace std::chrono_literals;

// The interop helper the MinGW d2d1.h headers don't declare for C.
extern "C" HRESULT WINAPI D2D1CreateDevice(IDXGIDevice*, D2D1_CREATION_PROPERTIES const*, ID2D1Device**);

namespace wuc = winrt::Windows::UI::Composition;

namespace {

constexpr int kAnimW = 400, kAnimH = 500;  // snapshot size, px
constexpr int kAnimGap = 60;               // distance from the screen edges
constexpr int kIcon = 44;                  // dock-icon-sized target (the handoff's peak-icon size)

RECT ScreenRect() {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    return mi.rcMonitor;
}

// Snapshot: horizontal colour bands + vertical lines, so warp, stretch and shear are all
// visible in framecap's PNGs. Top-down premultiplied BGRA.
void Fill(std::vector<uint8_t>& px, int w, int h, int stride) {
    px.assign((size_t)stride * h, 0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int band = y * 6 / h, line = (x / 40) % 2;
            uint8_t* p = &px[(size_t)y * stride + (size_t)x * 4];
            p[0] = (uint8_t)(30 + band * 40);        // blue: slow gradient down the rows
            p[1] = (uint8_t)(line ? 210 : 30);       // green: every 40 px a bright column
            p[2] = (uint8_t)(220 - band * 25);       // red: bands go pale going down
            p[3] = 255;
        }
}

void Pump(DWORD ms) {
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

// Waits for an event while pumping: the STA compositor and the scoped-batch Completed
// callback only run while this thread pumps. True on WAIT_OBJECT_0 within ms.
bool PumpedWait(HANDLE ev, DWORD ms) {
    DWORD start = GetTickCount();
    for (;;) {
        if (WaitForSingleObject(ev, 15) == WAIT_OBJECT_0) return true;
        if (GetTickCount() - start >= ms) return false;
        Pump(12);
    }
}

void Log(wchar_t const* fmt, ...) {
    wchar_t buf[400];
    va_list args;
    va_start(args, fmt);
    vswprintf(buf, 400, fmt, args);
    va_end(args);
    std::wstring line = L"[halo] selftest: " + std::wstring(buf) + L"\n";
    OutputDebugStringW(line.c_str());
    fputws(line.c_str(), stdout);
    fflush(stdout);
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

// Is DWM actually compositing right now? Counts composition frames over ~ms. When the
// display is asleep (the owner away), animations and batches don't advance and every
// timing below lies, so callers gate on this.
int DwmFrames(DWORD ms) {
    DWM_TIMING_INFO ti{sizeof(ti)}, prev{};
    int frames = 0;
    DWORD start = GetTickCount();
    while (GetTickCount() - start < ms) {
        if (DwmGetCompositionTimingInfo(nullptr, &ti) == 0 && ti.cFrame != prev.cFrame) frames++;
        prev = ti;
        Sleep(16);
    }
    return frames;
}

// Waits until DWM composes steadily (the display is awake). False: it never started.
bool WaitForDwm(DWORD timeoutMs) {
    DWORD start = GetTickCount();
    for (;;) {
        int f = DwmFrames(320);
        if (f >= 8) return true;  // ~25 compositions/s
        if (GetTickCount() - start > timeoutMs) {
            Log(L"display appears idle (%d compositions / 320 ms) - timings would be meaningless", f);
            return false;
        }
    }
}

// Shows a small composition surface over `rect` on its own topmost popup window, pumps for
// `ms`, and closes it. `animate` adds a keyframe-animated square (the known-good recipe).
void Marker(HINSTANCE inst, wuc::Compositor const& comp, RECT rect, bool animate, DWORD ms) {
    static bool classname = [] {
        WNDCLASSW wc{sizeof(wc)};
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = L"HaloSelftestMarker";
        return RegisterClassW(&wc) != 0;
    }();
    (void)classname;
    int w = rect.right - rect.left, h = rect.bottom - rect.top;
    HWND wnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                               L"HaloSelftestMarker", L"", WS_POPUP, rect.left, rect.top, w, h, nullptr, nullptr, inst,
                               nullptr);
    wuc::Desktop::DesktopWindowTarget t{nullptr};
    auto interop = comp.as<ICompositorDesktopInterop>();
    winrt::check_hresult(interop->CreateDesktopWindowTarget(wnd, TRUE, winrt::put_abi(t)));
    wuc::ContainerVisual root = comp.CreateContainerVisual();
    root.RelativeSizeAdjustment({1, 1});
    t.Root(root);
    wuc::SpriteVisual bg = comp.CreateSpriteVisual();
    bg.Size({(float)w * 0.8f, (float)h * 0.8f});
    bg.Offset({(float)w * 0.1f, (float)h * 0.1f, 0});
    bg.Brush(comp.CreateColorBrush({220, 40, 40, 255}));
    root.Children().InsertAtTop(bg);
    if (animate) {
        wuc::SpriteVisual sq = comp.CreateSpriteVisual();
        sq.Size({40, 40});
        sq.Offset({10, 10, 0});
        sq.Brush(comp.CreateColorBrush({80, 200, 255, 255}));
        root.Children().InsertAtTop(sq);
        auto kf = comp.CreateVector3KeyFrameAnimation();
        kf.Duration(300ms);
        kf.InsertKeyFrame(1.0f, {(float)w - 60, (float)h - 60, 0});
        sq.StartAnimation(L"Offset", kf);
    }
    ShowWindow(wnd, SW_SHOWNOACTIVATE);
    Pump(ms);
    DestroyWindow(wnd);
}

// One handshake with the animation, exactly as the mod does it.
bool Play(Genie& genie, int kind, RECT anim, std::vector<uint8_t> const& px) {
    wchar_t const* name = kind == 0 ? L"minimize" : L"restore";
    unsigned long long id = (unsigned long long)GetCurrentProcessId() << 32 | (unsigned)(kind + 1);
    RECT window = anim;
    RECT target = {anim.left + (anim.right - anim.left) / 3, anim.bottom - kIcon,
                   anim.left + (anim.right - anim.left) / 3 + kIcon, anim.bottom};

    SECURITY_DESCRIPTOR sd;
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);  // as HaloStart does
    SECURITY_ATTRIBUTES sa{sizeof(sa), &sd, FALSE};

    wchar_t hex[32];
    swprintf(hex, 32, L"%llx", id);
    std::wstring base = L"Local\\HaloGenie." + std::wstring(hex);  // Equals Genie::Start's NameOf
    size_t bytes = sizeof(GenieHeader) + px.size();
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, (DWORD)(bytes >> 32), (DWORD)bytes,
                                        base.c_str());
    auto* hdr = (GenieHeader*)MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 0);
    HANDLE ready = CreateEventW(&sa, TRUE, FALSE, (base + L".ready").c_str());
    HANDLE done = CreateEventW(&sa, TRUE, FALSE, (base + L".done").c_str());
    if (!hdr || !ready || !done) {
        Log(L"%ls: setup FAILED err=%lu", name, GetLastError());
        return false;
    }
    hdr->magic = kGenieMagic;
    hdr->version = 1;
    hdr->kind = (uint32_t)kind;
    hdr->width = kAnimW;
    hdr->height = kAnimH;
    hdr->stride = kAnimW * 4;
    hdr->window = window;
    hdr->target = target;
    memcpy((uint8_t*)hdr + kGeniePixelOffset, px.data(), px.size());

    Log(L"--- %ls  window %ld,%ld,%ld,%ld  target %ld,%ld,%ld,%ld", name, window.left, window.top, window.right,
        window.bottom, target.left, target.top, target.right, target.bottom);
    ULONGLONG t0 = GetTickCount64();
    genie.OnMessage(1, (LPARAM)id);  // op 1 = start, as the mod posts it

    bool pass = true;
    if (!PumpedWait(ready, 700)) {
        Log(L"%ls: READY FAILED (timeout) - .ready must arrive once the first frame is committed", name);
        pass = false;
    } else {
        ULONGLONG readyMs = GetTickCount64() - t0;
        bool ended = PumpedWait(done, 2000);
        ULONGLONG doneMs = GetTickCount64() - t0;
        bool good = ended && doneMs - readyMs >= 350 && doneMs - readyMs < 1600;
        Log(L"%ls: ready %.0f ms, done %.0f ms (%.0f ms animation) %ls", name, (double)readyMs, (double)doneMs,
            (double)(doneMs - readyMs), ended ? (good ? L"PASS" : L"UNEXPECTED TIMING") : L"FAILED: no .done");
        pass = pass && good;
        if (kind != 0) genie.OnMessage(2, (LPARAM)id);  // op 2 = release
    }
    if (!pass) genie.OnMessage(3, (LPARAM)id);  // op 3 = cancel, drop whatever is left
    UnmapViewOfFile(hdr);
    CloseHandle(mapping);
    CloseHandle(ready);
    CloseHandle(done);
    return pass;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // A hidden window as the Genie's z-order anchor. Never shown, so the live Halo's
    // window tracker never lists it.
    WNDCLASSW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"HaloSelftest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Halo genie selftest", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, inst,
                              nullptr);

    winrt::com_ptr<ID3D11Device> d3d;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, d3d.put(), nullptr, nullptr);
    if (FAILED(hr)) {
        Log(L"WARP device unavailable %08lX, using the GPU", (unsigned long)hr);
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                               d3d.put(), nullptr, nullptr));
    }
    auto dxgi = d3d.as<IDXGIDevice>();
    winrt::com_ptr<ID2D1Device> d2d;
    winrt::check_hresult(D2D1CreateDevice(dxgi.get(), nullptr, d2d.put()));

    DispatcherQueueOptions opt{sizeof(opt), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
    winrt::Windows::System::DispatcherQueueController dq{nullptr};
    winrt::check_hresult(CreateDispatcherQueueController(
        opt, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(dq))));

    wuc::Compositor comp;
    wuc::CompositionGraphicsDevice gdev{nullptr};
    auto interop = comp.as<ABI::Windows::UI::Composition::ICompositorInterop>();
    winrt::check_hresult(interop->CreateGraphicsDevice(
        reinterpret_cast<IUnknown*>(d2d.get()),
        reinterpret_cast<ABI::Windows::UI::Composition::ICompositionGraphicsDevice**>(winrt::put_abi(gdev))));

    Genie genie(comp, gdev, hwnd);
    Log(L"genie constructed over class HaloSelftest");

    RECT screen = ScreenRect();
    RECT anim = {screen.right - kAnimGap - kAnimW, screen.bottom - kAnimGap - kAnimH, screen.right - kAnimGap,
                 screen.bottom - kAnimGap};

    // Markers first (recorded by framecap when tools/genie-shots.sh drives this): a big red
    // rect proves the region and the composition pipeline, then a keyframe-animating blue
    // square proves plain animations run here.
    if (!WaitForDwm(8000)) return 3;  // display asleep: timing can't be trusted, rerun later
    Log(L"marker static");
    Marker(inst, comp, anim, false, 400);
    Log(L"marker animated");
    Marker(inst, comp, anim, true, 500);

    std::vector<uint8_t> px;
    Fill(px, kAnimW, kAnimH, kAnimW * 4);

    int failures = 0;
    failures += Play(genie, 0, anim, px) ? 0 : 1;
    Sleep(400);
    failures += Play(genie, 1, anim, px) ? 0 : 1;
    Log(failures ? L"%d FAILURES" : L"both animations passed", failures);
    return failures ? 1 : 0;
}
