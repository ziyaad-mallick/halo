// ==WindhawkMod==
// @id              halo-genie
// @name            Halo Genie
// @description     macOS Scale-effect minimize/restore into Halo's dock icons (GPU compositor), plus a zoom for maximize/un-maximize.
// @version         2.1.0
// @author          Ziyaad Mallick (fork of MacOS Minimize Animation by Abdullah Masood)
// @include         *
// @exclude         halo.exe
// @exclude         guardian.exe
// @architecture    x86-64
// @compilerOptions -ldwmapi -lgdi32 -ldcomp -ld3d11 -ldxgi -lole32 -loleaut32 -luuid
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Halo Genie

The macOS "Scale" minimize effect for [Halo](https://github.com/ziyaad-mallick/halo),
the dock that replaces the Windows taskbar. Minimizing shrinks the window
toward the app's dock icon along an ease-in-out path (about 320 ms, fading out
over the last 30%); restoring is the exact reverse with a tiny spring settle.
Maximize and un-maximize get a matching zoom (the window scales between its two
rects, ease-out, no flash).

The animation runs on the GPU compositor: the window is captured once into a
DirectComposition surface, and scale, translate and opacity are
IDCompositionAnimation timelines committed once, so the DWM plays them at the
display refresh rate with no per-frame CPU work in the host process.

If Halo is not running, minimize and restore aim at the Windows taskbar button
instead (simplified from the original mod: the click position, or a name match
through UI Automation). Maximize zoom works either way.

## Halo protocol

Halo's dock is a top-level window of class `HaloDockWindow`. The mod finds it
with EnumWindows and sends the registered message `Halo.DockIconForWindow`
with SendMessageTimeoutW (SMTO_ABORTIFHUNG, 50 ms):

- wParam = the HWND being animated
- lParam = 1 minimize, 2 restore, 3 query only
- LRESULT = 0 if the window has no dock icon, else the icon's rest position
  in physical screen pixels, packed in 48 bits:

      bits  0..15  icon size (square), unsigned; never 0 in a valid reply
      bits 16..31  centre x, signed 16-bit
      bits 32..47  centre y, signed 16-bit

  Unpack: size = r & 0xFFFF; cx = (int16_t)(r >> 16); cy = (int16_t)(r >> 32).
  Negative x (a monitor left of the primary) is plain two's complement.
  The position is where the icon sits when the dock is shown at rest, not
  its hidden, slid-off position. The reply needs a 64-bit LRESULT, so the
  mod is x86-64 only.
- On 1 and 2 Halo also reveals the dock and keeps it up until 300 ms after
  the animation (it assumes the 320 ms animation) so the swoop lands in a visible icon.

Halo replies 0 (no dock icon): the mod lets Windows do its own animation.
No Halo window: the taskbar fallback above.

## Credits and licence

Fork of **MacOS Minimize Animation** v3.1.6 (MIT) by **Abdullah Masood**
(<https://github.com/Abdullah-Masood-05>). The window-only capture and
translucent-window fix come from the Direct2D genie engine contributed to that
mod by **Potassiumuncher** (<https://github.com/Potassiumuncher>); the
DirectComposition Scale effect replaced that engine's CPU mesh warp. The
maximize zoom was written for this fork; the way it hooks maximize/restore was studied in "Windows Animations"
by ReDrag (MIT), no code copied.

Dropped from the original: the Classic style, app-launch animation, settings,
excluded-programs list and auto-hide taskbar handling.

MIT License. Copyright (c) Abdullah Masood and Potassiumuncher (original mod
and engine); copyright (c) 2026 Ziyaad Mallick (Halo changes). Permission is
hereby granted, free of charge, to any person obtaining a copy of this
software and associated documentation files, to deal in the software without
restriction, including without limitation the rights to use, copy, modify,
merge, publish, distribute, sublicense, and/or sell copies of the software,
subject to the above copyright notice and this permission notice being
included in all copies or substantial portions of the software. THE SOFTWARE
IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
*/
// ==/WindhawkModReadme==

#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dcomp.h>
#include <uiautomation.h>
#include <math.h>
#include <atomic>
#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 2
#endif
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

constexpr int kGenieMs = 320;  // Halo keeps the dock up for this + 300 ms
constexpr int kZoomMs = 280;
constexpr float kPi = 3.14159265f;

enum Kind { Minimize, Restore, Maximize, Unmaximize };
struct Geometry { float x, y, width, height; };

// Shared immutable snapshot: premultiplied 32-bit top-down DIB.
struct Snap {
    HBITMAP bmp; void* bits; int w, h;
    ~Snap() { DeleteObject(bmp); }
};

struct Anim {
    HWND hwnd;
    Kind kind;
    Geometry icon{};              // genie: the dock icon, screen px
    RECT from{}, toEst{}, toFinal{};  // window's extended frame; zoom end rect (estimate, then real)
    std::atomic<bool> toReady{false}, abort{false}, firstShown{false};
    bool cloaked = false;
    std::shared_ptr<Snap> snap;
};

typedef LRESULT (WINAPI *DefWindowProcW_t)(HWND, UINT, WPARAM, LPARAM);
typedef LRESULT (WINAPI *DefWindowProcA_t)(HWND, UINT, WPARAM, LPARAM);
typedef BOOL (WINAPI *ShowWindow_t)(HWND, int);
typedef BOOL (WINAPI *SetWindowPlacement_t)(HWND, const WINDOWPLACEMENT*);
typedef BOOL (WINAPI *CloseWindow_t)(HWND);
DefWindowProcW_t DefWindowProcW_Original;
DefWindowProcA_t DefWindowProcA_Original;
ShowWindow_t ShowWindow_Original, ShowWindowAsync_Original;
SetWindowPlacement_t SetWindowPlacement_Original;
CloseWindow_t CloseWindow_Original;

ID3D11Device* g_d3d = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGIDevice* g_dxgi = nullptr;
std::mutex g_d3dMutex;
UINT g_haloMsg = 0;
std::mutex g_mutex;
std::unordered_map<HWND, std::shared_ptr<Snap>> g_cache;  // minimize snapshot, reused by restore
std::unordered_set<HWND> g_active;                        // windows with an animation in flight
std::atomic<bool> g_unloading{false};
std::atomic<int> g_workers{0};

// Physical-pixel coordinates on this thread, whatever the host app's DPI mode.
struct DpiScope {
    DPI_AWARENESS_CONTEXT old = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ~DpiScope() { SetThreadDpiAwarenessContext(old); }
};

static void SetTransitions(HWND h, BOOL on) {
    BOOL off = !on;
    DwmSetWindowAttribute(h, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
}
static void Cloak(HWND h, BOOL on) { DwmSetWindowAttribute(h, DWMWA_CLOAK, &on, sizeof(on)); }

static bool ExtFrame(HWND h, RECT* r) {
    return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, r, sizeof(*r)));
}

// ---------------------------------------------------------------------------
// Where to land
// ---------------------------------------------------------------------------
static BOOL CALLBACK FindHaloProc(HWND h, LPARAM l) {
    wchar_t cls[32];
    if (GetClassNameW(h, cls, 32) && !wcscmp(cls, L"HaloDockWindow")) { *(HWND*)l = h; return FALSE; }
    return TRUE;
}

enum class Ask { NoHalo, Unknown, Ok };

static Ask AskHalo(HWND win, int op, Geometry* icon) {
    HWND halo = nullptr;
    EnumWindows(FindHaloProc, (LPARAM)&halo);
    if (!halo) return Ask::NoHalo;
    DWORD_PTR r = 0;
    if (!SendMessageTimeoutW(halo, g_haloMsg, (WPARAM)win, op, SMTO_ABORTIFHUNG, 50, &r) || !(r & 0xFFFF))
        return Ask::Unknown;
    float size = (float)(r & 0xFFFF);
    float cx = (float)(int16_t)(r >> 16), cy = (float)(int16_t)(r >> 32);
    *icon = {cx - size / 2, cy - size / 2, size, size};
    return Ask::Ok;
}

// No Halo: find the app's button on the Windows taskbar with UI Automation.
static void TaskbarButtonX(HWND tray, HWND win, int* x) {
    wchar_t title[256] = {}, path[MAX_PATH] = {};
    GetWindowTextW(win, title, 256);
    DWORD pid = 0, n = MAX_PATH;
    GetWindowThreadProcessId(win, &pid);
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        QueryFullProcessImageNameW(p, 0, path, &n);
        CloseHandle(p);
    }
    auto lower = [](std::wstring s) { for (auto& c : s) c = towlower(c); return s; };
    std::wstring t = lower(title), stem = lower(wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : path);
    stem = stem.substr(0, stem.find(L'.'));

    bool coInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    IUIAutomation* uia = nullptr;
    IUIAutomationElement* root = nullptr;
    IUIAutomationCondition *btn = nullptr, *item = nullptr, *either = nullptr;
    IUIAutomationElementArray* arr = nullptr;
    VARIANT v; v.vt = VT_I4;
    if (SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), (void**)&uia)) &&
        SUCCEEDED(uia->ElementFromHandle(tray, &root))) {
        v.lVal = UIA_ButtonControlTypeId;   uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &btn);
        v.lVal = UIA_ListItemControlTypeId; uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &item);
        if (btn && item && SUCCEEDED(uia->CreateOrCondition(btn, item, &either)) &&
            SUCCEEDED(root->FindAll(TreeScope_Descendants, either, &arr)) && arr) {
            int len = 0, best = 0;
            arr->get_Length(&len);
            for (int i = 0; i < len; i++) {
                IUIAutomationElement* e = nullptr;
                BSTR name = nullptr;
                RECT br;
                if (SUCCEEDED(arr->GetElement(i, &e)) && e) {
                    if (SUCCEEDED(e->get_CurrentName(&name)) && name && *name) {
                        std::wstring nm = lower(name);
                        int score = (!t.empty() && t.find(nm) != std::wstring::npos ? 2 : 0) +
                                    (!stem.empty() && nm.find(stem) != std::wstring::npos ? 1 : 0);
                        if (score > best && SUCCEEDED(e->get_CurrentBoundingRectangle(&br)) && br.right > br.left) {
                            best = score;
                            *x = (br.left + br.right) / 2;
                        }
                    }
                    SysFreeString(name);
                    e->Release();
                }
            }
        }
    }
    if (arr) arr->Release();
    if (either) either->Release();
    if (item) item->Release();
    if (btn) btn->Release();
    if (root) root->Release();
    if (uia) uia->Release();
    if (coInit) CoUninitialize();
}

static bool TaskbarTarget(HWND win, Geometry* icon) {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    RECT tr;
    if (!tray || !GetWindowRect(tray, &tr)) return false;
    POINT pt;
    GetCursorPos(&pt);
    int x = (tr.left + tr.right) / 2;
    if (PtInRect(&tr, pt)) x = pt.x;  // the user clicked this window's own button
    else TaskbarButtonX(tray, win, &x);
    *icon = {(float)x - 11, (float)tr.top, 22, 33.0f * GetDpiForSystem() / 96.0f};
    return true;
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------
// The window alone (PrintWindow, so the taskbar and other windows never bleed in)
// cropped to its extended frame. Translucent regions keep their alpha (the
// Potassiumuncher fix), unless most of the window came back transparent, which
// DWM does on secondary monitors; those are forced opaque.
static std::shared_ptr<Snap> Capture(HWND hwnd, RECT* ext) {
    RECT win;
    if (!GetWindowRect(hwnd, &win)) return nullptr;
    *ext = win;
    ExtFrame(hwnd, ext);
    int w = ext->right - ext->left, h = ext->bottom - ext->top;
    int rawW = win.right - win.left, rawH = win.bottom - win.top;
    if (w <= 0 || h <= 0 || rawW <= 0 || rawH <= 0) return nullptr;

    BITMAPINFO bmi = {{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB}};
    auto s = std::make_shared<Snap>();
    s->w = w; s->h = h;
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    s->bmp = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &s->bits, nullptr, 0);
    bmi.bmiHeader.biWidth = rawW; bmi.bmiHeader.biHeight = -rawH;
    void* raw = nullptr;
    HBITMAP rawBmp = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &raw, nullptr, 0);
    bool ok = s->bmp && rawBmp;
    if (ok) {
        HGDIOBJ old = SelectObject(dc, rawBmp);
        PrintWindow(hwnd, dc, PW_RENDERFULLCONTENT);
        GdiFlush();
        SelectObject(dc, old);
        DWORD *src = (DWORD*)raw, *dst = (DWORD*)s->bits;
        int dx = ext->left - win.left, dy = ext->top - win.top;
        long inBounds = 0, clear = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int sx = x + dx, sy = y + dy;
                bool in = sx >= 0 && sx < rawW && sy >= 0 && sy < rawH;
                DWORD p = in ? src[sy * rawW + sx] : 0;
                inBounds += in;
                clear += in && !(p >> 24);
                dst[y * w + x] = (p >> 24) ? p : 0;
            }
        if (clear * 2 > inBounds)
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    int sx = x + dx, sy = y + dy;
                    if (sx >= 0 && sx < rawW && sy >= 0 && sy < rawH && !(dst[y * w + x] >> 24))
                        dst[y * w + x] = 0xFF000000u | (src[sy * rawW + sx] & 0xFFFFFF);
                }
    }
    if (rawBmp) DeleteObject(rawBmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return ok ? s : nullptr;
}

// ---------------------------------------------------------------------------
// Rendering (DirectComposition: the DWM runs the animation, zero CPU per frame)
// ---------------------------------------------------------------------------
// The captured window is uploaded ONCE into a DComp surface. Scale, translate
// and opacity are IDCompositionAnimation objects (piecewise cubics), committed
// once; the worker then just sleeps until the timeline has played out.

// CSS-style cubic-bezier(x1,y1,x2,y2): y at time u.
static double Bezier(double x1, double y1, double x2, double y2, double u) {
    if (u <= 0) return 0;
    if (u >= 1) return 1;
    double lo = 0, hi = 1, s = u;
    for (int i = 0; i < 40; i++) {
        s = (lo + hi) / 2;
        double x = 3 * (1 - s) * (1 - s) * s * x1 + 3 * (1 - s) * s * s * x2 + s * s * s;
        (x < u ? lo : hi) = s;
    }
    return 3 * (1 - s) * (1 - s) * s * y1 + 3 * (1 - s) * s * s * y2 + s * s * s;
}

// Minimize motion curves, u in 0..1. X (and scale) is the Material standard
// ease-in-out; Y is a touch more front-loaded so the path bows into a slight arc.
static double CurveX(double u) { return Bezier(0.4, 0.0, 0.2, 1.0, u); }
static double CurveY(double u) { return Bezier(0.5, 0.0, 0.25, 1.0, u); }
static double Fade(double u) {  // opacity 1 -> 0 over the last 30%
    double s = std::min(std::max((u - 0.7) / 0.3, 0.0), 1.0);
    return 1.0 - s * s * (3.0 - 2.0 * s);
}
static double Bump(double u) {  // restore spring: +1.5% overshoot, then settle
    double s = std::min(std::max((u - 0.72) / 0.28, 0.0), 1.0);
    return 0.015 * sin(s * kPi);
}

// One animated property: value(u) over u in 0..1, sampled into cubic Hermite
// segments between `knots` (the DWM evaluates the polynomials itself).
struct Track {
    std::function<double(double)> f;
    std::vector<double> knots{0.0, 0.25, 0.5, 0.75, 1.0};
};

static IDCompositionAnimation* MakeAnim(IDCompositionDevice* dev, const Track& t, double durSec) {
    IDCompositionAnimation* an = nullptr;
    if (FAILED(dev->CreateAnimation(&an)) || !an) return nullptr;
    const double h = 1e-4;
    auto d = [&](double u) {  // derivative wrt u
        double a = std::max(u - h, 0.0), b = std::min(u + h, 1.0);
        return (t.f(b) - t.f(a)) / (b - a);
    };
    bool ok = true;
    for (size_t i = 0; i + 1 < t.knots.size(); i++) {
        double u0 = t.knots[i], u1 = t.knots[i + 1];
        double T = (u1 - u0) * durSec;
        double v0 = t.f(u0), v1 = t.f(u1);
        double d0 = d(u0) / durSec, d1 = d(u1) / durSec;  // per second
        double c = v0, l = d0;
        double q = 3 * (v1 - v0) / (T * T) - (2 * d0 + d1) / T;
        double k = -2 * (v1 - v0) / (T * T * T) + (d0 + d1) / (T * T);
        ok &= SUCCEEDED(an->AddCubic(u0 * durSec, (float)c, (float)l, (float)q, (float)k));
    }
    ok &= SUCCEEDED(an->End(durSec, (float)t.f(1.0)));
    if (!ok) { an->Release(); return nullptr; }
    return an;
}

// One D3D device per process, made on first use (injected into every process,
// so nothing is created until an animation actually runs).
static IDXGIDevice* AcquireDxgiDevice() {
    std::lock_guard<std::mutex> lock(g_d3dMutex);
    if (!g_dxgi) {
        ID3D11Device* d3d = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        D3D_FEATURE_LEVEL fl;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       nullptr, 0, D3D11_SDK_VERSION, &d3d, &fl, &ctx);
        if (FAILED(hr))
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   nullptr, 0, D3D11_SDK_VERSION, &d3d, &fl, &ctx);
        if (SUCCEEDED(hr) && SUCCEEDED(d3d->QueryInterface(__uuidof(IDXGIDevice), (void**)&g_dxgi))) {
            g_d3d = d3d; g_ctx = ctx;
        } else {
            if (d3d) d3d->Release();
            if (ctx) ctx->Release();
            g_dxgi = nullptr;
        }
    }
    if (g_dxgi) g_dxgi->AddRef();
    return g_dxgi;
}

// Creating the D3D device is most of a first minimize's cost (~100-300 ms, held on the app's own thread), so
// it is made in the background as soon as the pointer is over a minimize button: ready by the click.
std::atomic<bool> g_warming{false};
static DWORD WINAPI PrewarmThread(LPVOID) {
    if (IDXGIDevice* d = AcquireDxgiDevice()) d->Release();  // the device stays cached in g_dxgi
    g_workers.fetch_sub(1, std::memory_order_release);
    return 0;
}
static void Prewarm() {
    if (g_dxgi || g_unloading.load() || g_warming.exchange(true)) return;
    g_workers.fetch_add(1, std::memory_order_relaxed);
    HANDLE t = CreateThread(nullptr, 0, PrewarmThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    else g_workers.fetch_sub(1, std::memory_order_release), g_warming = false;
}

static void ReleaseDxgiDevice() {
    std::lock_guard<std::mutex> lock(g_d3dMutex);
    if (g_dxgi) { g_dxgi->Release(); g_dxgi = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_d3d) { g_d3d->Release(); g_d3d = nullptr; }
}

static RECT Union(const RECT& a, const RECT& b) {
    return {std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right), std::max(a.bottom, b.bottom)};
}

template <class T> static void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Waits up to `ms` (or until `done`), dispatching this thread's messages: the ghost window lives on this thread,
// so it must answer whatever the system sends it (a thread that never pumps looks hung and gets the busy cursor).
template <class F> static void PumpUntil(double ms, F done) {
    LARGE_INTEGER freq, t0, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        QueryPerformanceCounter(&now);
        double left = ms - (now.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
        if (left <= 0 || done()) return;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, (DWORD)std::min(left, 4.0) + 1, QS_ALLINPUT);
    }
}

static DWORD WINAPI Worker(LPVOID param) {
    std::shared_ptr<Anim> a(*(std::shared_ptr<Anim>*)param);
    delete (std::shared_ptr<Anim>*)param;
    DpiScope dpi;

    const bool zoom = a->kind >= Maximize;
    const bool rising = a->kind == Restore;
    const Snap& snap = *a->snap;
    const double sw = snap.w, sh = snap.h;
    const double durMs = zoom ? kZoomMs : kGenieMs;
    const double dur = durMs / 1000.0;

    // Canvas: a transparent window over everything the window can touch. The
    // genie only shrinks toward the icon (plus a 1.5% spring overshoot), so the
    // union of the two rects with a margin is enough.
    RECT dest = zoom ? a->toEst : RECT{(LONG)a->icon.x, (LONG)a->icon.y, (LONG)(a->icon.x + a->icon.width), (LONG)(a->icon.y + a->icon.height)};
    RECT box = Union(a->from, dest);
    int pad = zoom ? 48 : (int)(std::max(sw, sh) * 0.06) + 32;
    box = {box.left - pad, box.top - pad, box.right + pad, box.bottom + pad};
    const int vLeft = box.left, vTop = box.top, vW = box.right - box.left, vH = box.bottom - box.top;

    // Layered + transparent: the ghost is invisible to the mouse, so the pointer keeps the dock (and its hover) and
    // never lands on this thread's window. (WS_EX_TRANSPARENT alone still hit-tests across threads.)
    HWND ghost = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT |
                                     WS_EX_NOACTIVATE,
                                 L"STATIC", nullptr, WS_POPUP, vLeft, vTop, vW, vH, nullptr, nullptr, nullptr, nullptr);
    if (ghost) SetLayeredWindowAttributes(ghost, 0, 255, LWA_ALPHA);

    IDXGIDevice* dxgi = ghost ? AcquireDxgiDevice() : nullptr;
    IDCompositionDevice* dev = nullptr;
    IDCompositionTarget* target = nullptr;
    IDCompositionVisual* visual = nullptr;
    IDCompositionSurface* surface = nullptr;
    IDCompositionEffectGroup* effect = nullptr;
    IDCompositionScaleTransform* scale = nullptr;
    IDCompositionTranslateTransform* move = nullptr;
    IDCompositionTransform* group = nullptr;
    bool built = false;

    if (dxgi && SUCCEEDED(DCompositionCreateDevice(dxgi, __uuidof(IDCompositionDevice), (void**)&dev)) &&
        SUCCEEDED(dev->CreateTargetForHwnd(ghost, TRUE, &target)) && SUCCEEDED(dev->CreateVisual(&visual)) &&
        SUCCEEDED(dev->CreateSurface(snap.w, snap.h, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &surface)) &&
        SUCCEEDED(dev->CreateEffectGroup(&effect)) && SUCCEEDED(dev->CreateScaleTransform(&scale)) &&
        SUCCEEDED(dev->CreateTranslateTransform(&move))) {
        // Upload the capture once.
        ID3D11Texture2D* tex = nullptr;
        POINT off = {};
        if (SUCCEEDED(surface->BeginDraw(nullptr, __uuidof(ID3D11Texture2D), (void**)&tex, &off)) && tex) {
            D3D11_BOX b = {(UINT)off.x, (UINT)off.y, 0, (UINT)(off.x + snap.w), (UINT)(off.y + snap.h), 1};
            {
                std::lock_guard<std::mutex> lock(g_d3dMutex);
                if (g_ctx) g_ctx->UpdateSubresource(tex, 0, &b, snap.bits, snap.w * 4, 0);
            }
            tex->Release();
            built = SUCCEEDED(surface->EndDraw());
        }
    }

    // Motion, as functions of timeline position u (0..1).
    Track sx, sy, tx, ty, op;
    auto zoomTracks = [&](const RECT& to) {
        double kx = (to.right - to.left) / sw, ky = (to.bottom - to.top) / sh;
        double dx = (double)to.left - a->from.left, dy = (double)to.top - a->from.top;
        auto e = [](double u) { return 1.0 - pow(1.0 - u, 3.0); };  // ease-out
        sx.f = [=](double u) { return 1.0 + (kx - 1.0) * e(u); };
        sy.f = [=](double u) { return 1.0 + (ky - 1.0) * e(u); };
        tx.f = [=](double u) { return dx * e(u); };
        ty.f = [=](double u) { return dy * e(u); };
    };
    if (built) {
        if (zoom) {
            zoomTracks(a->toEst);
            op.f = [](double) { return 1.0; };
            sx.knots = sy.knots = tx.knots = ty.knots = {0.0, 0.5, 1.0};
            op.knots = {0.0, 1.0};
            scale->SetCenterX(0.0f); scale->SetCenterY(0.0f);
        } else {
            // Shrink uniformly about the window's centre while that centre travels to the icon's.
            double iconSize = std::max<double>(a->icon.width, 1.0);
            double sEnd = iconSize / std::max(sw, sh);
            double dx = a->icon.x + a->icon.width / 2 - (a->from.left + sw / 2);
            double dy = a->icon.y + a->icon.height / 2 - (a->from.top + sh / 2);
            // Minimize plays p = curve(u); restore is the exact reverse plus the spring settle.
            auto px = [=](double u) { return rising ? CurveX(1.0 - u) - Bump(u) : CurveX(u); };
            auto py = [=](double u) { return rising ? CurveY(1.0 - u) - Bump(u) : CurveY(u); };
            sx.f = sy.f = [=](double u) { return 1.0 + (sEnd - 1.0) * px(u); };
            tx.f = [=](double u) { return dx * px(u); };
            ty.f = [=](double u) { return dy * py(u); };
            op.f = [=](double u) { return rising ? Fade(1.0 - u) : Fade(u); };
            if (rising) {
                sx.knots = sy.knots = tx.knots = ty.knots = {0.0, 0.2, 0.4, 0.6, 0.72, 0.8, 0.86, 0.93, 1.0};
                op.knots = {0.0, 0.3, 1.0};
            } else {
                op.knots = {0.0, 0.7, 1.0};
            }
            scale->SetCenterX((float)(sw / 2)); scale->SetCenterY((float)(sh / 2));
        }

        // Static first pose (the window exactly where it was, or at the icon for a
        // restore) so the ghost is on screen before the caller touches the real window.
        scale->SetScaleX((float)sx.f(0)); scale->SetScaleY((float)sy.f(0));
        move->SetOffsetX((float)tx.f(0)); move->SetOffsetY((float)ty.f(0));
        effect->SetOpacity((float)op.f(0));
        IDCompositionTransform* parts[2] = {scale, move};
        built = SUCCEEDED(dev->CreateTransformGroup(parts, 2, &group)) &&
                SUCCEEDED(visual->SetOffsetX((float)(a->from.left - vLeft))) &&
                SUCCEEDED(visual->SetOffsetY((float)(a->from.top - vTop))) &&
                SUCCEEDED(visual->SetContent(surface)) && SUCCEEDED(visual->SetTransform(group)) &&
                SUCCEEDED(visual->SetEffect(effect)) && SUCCEEDED(target->SetRoot(visual)) && SUCCEEDED(dev->Commit());
    }
    if (built) {
        ShowWindow(ghost, SW_SHOWNOACTIVATE);
        dev->WaitForCommitCompletion();
        DwmFlush();  // the ghost is on screen before the caller changes the real window
    }
    a->firstShown = true;

    if (built && zoom) {  // the real window's final rect: known once the caller has resized it
        PumpUntil(150, [&] { return a->toReady.load() || a->abort.load() || g_unloading.load(); });
        if (a->toReady.load()) zoomTracks(a->toFinal);
    }

    if (built && !a->abort.load() && !g_unloading.load()) {
        // Hand the whole timeline to the compositor: set once, commit once.
        IDCompositionAnimation *asx = MakeAnim(dev, sx, dur), *asy = MakeAnim(dev, sy, dur), *atx = MakeAnim(dev, tx, dur),
                               *aty = MakeAnim(dev, ty, dur), *aop = MakeAnim(dev, op, dur);
        bool ok = asx && asy && atx && aty && aop &&
                  SUCCEEDED(scale->SetScaleX(asx)) && SUCCEEDED(scale->SetScaleY(asy)) &&
                  SUCCEEDED(move->SetOffsetX(atx)) && SUCCEEDED(move->SetOffsetY(aty)) &&
                  SUCCEEDED(effect->SetOpacity(aop)) && SUCCEEDED(dev->Commit());
        Rel(asx); Rel(asy); Rel(atx); Rel(aty); Rel(aop);
        LARGE_INTEGER freq, t0, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        if (ok) {
            // No rendering from here on: just wait out the timeline (plus a frame of slack).
            PumpUntil(durMs + 40.0, [&] { return a->abort.load() || g_unloading.load(); });
            DwmFlush();
        }
        QueryPerformanceCounter(&now);
        Wh_Log(L"genie kind=%d ok=%d dur=%dms waited=%.1fms canvas=%dx%d", (int)a->kind, (int)ok, (int)durMs,
               (now.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart, vW, vH);
    }

    // Reveal the real window under the ghost, then drop the ghost once DWM has composed it.
    if (a->cloaked) { Cloak(a->hwnd, FALSE); DwmFlush(); }
    SetTransitions(a->hwnd, TRUE);
    a->firstShown = true;
    Rel(group); Rel(scale); Rel(move); Rel(effect); Rel(surface); Rel(visual); Rel(target); Rel(dev); Rel(dxgi);
    if (ghost) DestroyWindow(ghost);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_active.erase(a->hwnd);
    }
    a.reset();
    g_workers.fetch_sub(1, std::memory_order_release);
    return 0;
}

// ---------------------------------------------------------------------------
// Starting animations
// ---------------------------------------------------------------------------
static bool Eligible(HWND h) {
    if (!h || (GetWindowLongPtrW(h, GWL_STYLE) & WS_CHILD)) return false;
    RECT r;
    if (IsIconic(h)) {
        WINDOWPLACEMENT wp = {sizeof(wp)};
        if (!GetWindowPlacement(h, &wp)) return false;
        r = wp.rcNormalPosition;
    } else if (!GetWindowRect(h, &r)) {
        return false;
    }
    return r.right - r.left >= 40 && r.bottom - r.top >= 40;
}

// Decides whether to animate, finds the destination (asking Halo, which also
// reveals the dock) and claims the window. Null: let Windows do its own thing.
static std::shared_ptr<Anim> Begin(HWND h, Kind kind) {
    if (g_unloading.load() || !Eligible(h)) return nullptr;
    DpiScope dpi;
    auto a = std::make_shared<Anim>();
    a->hwnd = h;
    a->kind = kind;
    if (kind <= Restore) {
        Ask r = AskHalo(h, kind == Minimize ? 1 : 2, &a->icon);
        if (r == Ask::Unknown || (r == Ask::NoHalo && !TaskbarTarget(h, &a->icon))) return nullptr;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_active.insert(h).second ? a : nullptr;
}

static void Abandon(const std::shared_ptr<Anim>& a) {
    if (a->cloaked) Cloak(a->hwnd, FALSE);
    SetTransitions(a->hwnd, TRUE);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_active.erase(a->hwnd);
}

// Takes the snapshot (a restore reuses the one taken at minimize) and the rects.
static bool Prepare(const std::shared_ptr<Anim>& a) {
    DpiScope dpi;
    HWND h = a->hwnd;
    if (a->kind == Restore) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_cache.find(h);
        if (it != g_cache.end()) { a->snap = it->second; g_cache.erase(it); }
        RECT cur;
        if (a->snap && (!ExtFrame(h, &cur) || cur.right - cur.left != a->snap->w || cur.bottom - cur.top != a->snap->h))
            a->snap = nullptr;
    }
    if (!a->snap || a->kind != Restore) a->snap = Capture(h, &a->from);
    else ExtFrame(h, &a->from);
    if (!a->snap) return false;
    if (a->kind == Minimize) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_cache[h] = a->snap;
    }
    if (a->kind == Maximize) {
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        a->toEst = mi.rcWork;
    } else if (a->kind == Unmaximize) {
        WINDOWPLACEMENT wp = {sizeof(wp)};
        GetWindowPlacement(h, &wp);
        a->toEst = wp.rcNormalPosition;
    }
    return true;
}

// Starts the render thread; if `wait`, returns once the ghost is on screen.
static bool Launch(const std::shared_ptr<Anim>& a, bool wait) {
    g_workers.fetch_add(1, std::memory_order_relaxed);
    auto* copy = new std::shared_ptr<Anim>(a);
    HANDLE t = CreateThread(nullptr, 0, Worker, copy, 0, nullptr);
    if (!t) {
        g_workers.fetch_sub(1, std::memory_order_release);
        delete copy;
        Abandon(a);
        return false;
    }
    CloseHandle(t);
    for (int i = 0; wait && i < 200 && !a->firstShown.load(); i++) Sleep(1);
    return true;
}

// Runs `real` (the call that changes the window) under the animation `kind`.
template <class F>
static auto Run(HWND h, Kind kind, F real) -> decltype(real()) {
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    auto a = Begin(h, kind);
    if (!a) return real();
    struct Stall {  // how long the app's own thread is held before the real call (logged)
        LARGE_INTEGER &f, &t0, &t1; Kind k;
        ~Stall() { QueryPerformanceCounter(&t1); Wh_Log(L"genie kind=%d caller stall %.1f ms", (int)k, (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart); }
    } stall{freq, t0, t1, kind};
    SetTransitions(h, FALSE);
    if (kind == Restore) {  // hide the restored window, then play out of the icon
        a->cloaked = true;
        Cloak(h, TRUE);
        auto r = real();
        if (!Prepare(a) || !Launch(a, false)) Abandon(a);
        return r;
    }
    if (!Prepare(a)) { Abandon(a); return real(); }
    if (!Launch(a, true)) return real();
    if (kind == Minimize) return real();
    a->cloaked = true;  // zoom: swap the real window under the ghost
    Cloak(h, TRUE);
    bool wasZoomed = IsZoomed(h);
    auto r = real();
    if (IsZoomed(h) == wasZoomed) a->abort = true;  // nothing changed
    else if (ExtFrame(h, &a->toFinal)) a->toReady = true;
    else a->abort = true;
    return r;
}

static bool IsMin(int c) { return c == SW_MINIMIZE || c == SW_SHOWMINIMIZED || c == SW_SHOWMINNOACTIVE; }

// Which animation (if any) a show command or system command means for this window.
static bool Classify(HWND h, int cmd, bool sys, Kind* kind) {
    if (sys ? cmd == SC_MINIMIZE : IsMin(cmd)) {
        if (IsIconic(h) || !IsWindowVisible(h)) return false;
        *kind = Minimize;
    } else if (sys ? cmd == SC_MAXIMIZE : (cmd == SW_MAXIMIZE)) {
        if (IsIconic(h) || IsZoomed(h) || !IsWindowVisible(h)) return false;
        *kind = Maximize;
    } else if (sys ? cmd == SC_RESTORE : (cmd == SW_RESTORE || cmd == SW_SHOWNORMAL)) {
        if (!IsWindowVisible(h)) return false;
        if (IsIconic(h)) *kind = Restore;
        else if (IsZoomed(h)) *kind = Unmaximize;
        else return false;
    } else {
        return false;
    }
    return true;
}

BOOL WINAPI ShowWindow_Hook(HWND h, int cmd) {
    Kind k;
    if (Classify(h, cmd, false, &k)) return Run(h, k, [&] { return ShowWindow_Original(h, cmd); });
    return ShowWindow_Original(h, cmd);
}

BOOL WINAPI ShowWindowAsync_Hook(HWND h, int cmd) {
    Kind k;  // async calls only get the minimize genie (custom title bar apps)
    if (IsMin(cmd) && Classify(h, cmd, false, &k)) return Run(h, k, [&] { return ShowWindowAsync_Original(h, cmd); });
    return ShowWindowAsync_Original(h, cmd);
}

BOOL WINAPI SetWindowPlacement_Hook(HWND h, const WINDOWPLACEMENT* wp) {
    Kind k;
    if (wp && IsMin(wp->showCmd) && Classify(h, wp->showCmd, false, &k))
        return Run(h, k, [&] { return SetWindowPlacement_Original(h, wp); });
    return SetWindowPlacement_Original(h, wp);
}

BOOL WINAPI CloseWindow_Hook(HWND h) {
    Kind k;
    if (Classify(h, SW_MINIMIZE, false, &k)) return Run(h, k, [&] { return CloseWindow_Original(h); });
    return CloseWindow_Original(h);
}

LRESULT WINAPI DefWindowProcW_Hook(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_NCMOUSEMOVE && w == HTMINBUTTON) {
        Prewarm();
    } else if (msg == WM_SYSCOMMAND) {
        Kind k;
        if (Classify(h, (int)(w & 0xFFF0), true, &k)) return Run(h, k, [&] { return DefWindowProcW_Original(h, msg, w, l); });
    } else if (msg == WM_DESTROY) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_cache.erase(h);
    }
    return DefWindowProcW_Original(h, msg, w, l);
}

// ANSI windows (older apps, some games' launchers) minimize through DefWindowProcA.
LRESULT WINAPI DefWindowProcA_Hook(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_NCMOUSEMOVE && w == HTMINBUTTON) {
        Prewarm();
    } else if (msg == WM_SYSCOMMAND) {
        Kind k;
        if (Classify(h, (int)(w & 0xFFF0), true, &k)) return Run(h, k, [&] { return DefWindowProcA_Original(h, msg, w, l); });
    } else if (msg == WM_DESTROY) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_cache.erase(h);
    }
    return DefWindowProcA_Original(h, msg, w, l);
}

BOOL Wh_ModInit() {
    g_haloMsg = RegisterWindowMessageW(L"Halo.DockIconForWindow");
    Wh_SetFunctionHook((void*)DefWindowProcW, (void*)DefWindowProcW_Hook, (void**)&DefWindowProcW_Original);
    Wh_SetFunctionHook((void*)DefWindowProcA, (void*)DefWindowProcA_Hook, (void**)&DefWindowProcA_Original);
    Wh_SetFunctionHook((void*)ShowWindow, (void*)ShowWindow_Hook, (void**)&ShowWindow_Original);
    Wh_SetFunctionHook((void*)ShowWindowAsync, (void*)ShowWindowAsync_Hook, (void**)&ShowWindowAsync_Original);
    Wh_SetFunctionHook((void*)SetWindowPlacement, (void*)SetWindowPlacement_Hook, (void**)&SetWindowPlacement_Original);
    Wh_SetFunctionHook((void*)CloseWindow, (void*)CloseWindow_Hook, (void**)&CloseWindow_Original);
    return TRUE;
}

// Windhawk unmaps the DLL right after uninit, so workers must be gone first.
void Wh_ModBeforeUninit() {
    g_unloading = true;
    for (int i = 0; i < 300 && g_workers.load(std::memory_order_acquire) > 0; i++) Sleep(10);
}

void Wh_ModUninit() {
    ReleaseDxgiDevice();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cache.clear();
}
