// ==WindhawkMod==
// @id              halo-genie
// @name            Halo Genie
// @description     macOS genie minimize/restore into Halo's dock icons, plus a zoom swoop for maximize/un-maximize.
// @version         1.0.0
// @author          Ziyaad Mallick (fork of MacOS Minimize Animation by Abdullah Masood)
// @include         *
// @exclude         halo.exe
// @exclude         guardian.exe
// @architecture    x86-64
// @compilerOptions -ldwmapi -lgdi32 -ld2d1 -lole32 -loleaut32 -luuid
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Halo Genie

The macOS genie effect for [Halo](https://github.com/ziyaad-mallick/halo), the
dock that replaces the Windows taskbar. Minimizing pours the window into the
app's dock icon, restoring pours it back out. Maximize and un-maximize get a
matching zoom (the window scales between its two rects, ease-out, no flash).

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
  the animation (it assumes the 400 ms genie) so the swoop lands in a visible icon.

Halo replies 0 (no dock icon): the mod lets Windows do its own animation.
No Halo window: the taskbar fallback above.

## Credits and licence

Fork of **MacOS Minimize Animation** v3.1.6 (MIT) by **Abdullah Masood**
(<https://github.com/Abdullah-Masood-05>). The Direct2D genie engine (mesh
warp on the macOS lamp curve, window-only capture, translucent-window fix,
frame pacing) was contributed to that mod by **Potassiumuncher**
(<https://github.com/Potassiumuncher>). The maximize zoom was written for this
fork; the way it hooks maximize/restore was studied in "Windows Animations"
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
#include <d2d1.h>
#include <uiautomation.h>
#include <math.h>
#include <atomic>
#include <algorithm>
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

constexpr int kGenieMs = 400;  // Halo keeps the dock up for this + 300 ms
constexpr int kZoomMs = 280;
constexpr int kTiles = 32;     // genie mesh resolution
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
typedef BOOL (WINAPI *ShowWindow_t)(HWND, int);
typedef BOOL (WINAPI *SetWindowPlacement_t)(HWND, const WINDOWPLACEMENT*);
typedef BOOL (WINAPI *CloseWindow_t)(HWND);
DefWindowProcW_t DefWindowProcW_Original;
ShowWindow_t ShowWindow_Original, ShowWindowAsync_Original;
SetWindowPlacement_t SetWindowPlacement_Original;
CloseWindow_t CloseWindow_Original;

ID2D1Factory* g_d2dFactory = nullptr;
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
// Rendering
// ---------------------------------------------------------------------------
// The macOS "lamp" curve (Potassiumuncher): maps a mesh vertex (tx,ty in 0..1)
// at progress p to its warped screen position, for window rect w and icon i.
static void LampVertex(float tx, float ty, float p, const Geometry& w, const Geometry& i, float* outX, float* outY) {
    float split = 0.3f;
    float k = (p <= split) ? (p / split) : 1.0f;
    float j = (p > split) ? ((p - split) / (1.0f - split)) : 0.0f;
    float expandHeight = (i.y - w.y - w.height);
    float fullHeight = (i.y - w.y) - (expandHeight * (1.0f - k));
    float height = fullHeight - (j * fullHeight);
    float y = ty * height;
    float x = tx * (i.width) + tx * (w.width - i.width) * (1.0f - j) * (1.0f - ty) + tx * (w.width - i.width) * (1.0f - k) * ty;
    float offsetX = (i.x - w.x) * (y / (fullHeight + 0.1f)) * k + (i.x - w.x) * j;
    float offsetY = i.y - w.y - height - (expandHeight * (1.0f - k));
    float effectX = sinf(((height - y) / fullHeight) * 2.0f * kPi + kPi) * (w.x + w.width * tx - (i.x + i.width * tx)) / 7.0f * k;
    *outX = w.x + x + offsetX + effectX;
    *outY = w.y + y + offsetY;
}

static D2D1_POINT_2F Bloat(D2D1_POINT_2F p, D2D1_POINT_2F c, float amount) {
    float dx = p.x - c.x, dy = p.y - c.y, len = sqrtf(dx * dx + dy * dy);
    return len < 0.001f ? p : D2D1::Point2F(p.x + dx / len * amount, p.y + dy / len * amount);
}

static ID2D1PathGeometry* QuadGeo(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_POINT_2F c, D2D1_POINT_2F d) {
    ID2D1PathGeometry* geo = nullptr;
    g_d2dFactory->CreatePathGeometry(&geo);
    if (!geo) return nullptr;
    ID2D1GeometrySink* sink = nullptr;
    geo->Open(&sink);
    sink->BeginFigure(a, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(b); sink->AddLine(c); sink->AddLine(d);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    sink->Release();
    return geo;
}

static RECT MonitorOf(const RECT& r) {
    MONITORINFO mi = {sizeof(mi)};
    return GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi) ? mi.rcMonitor : r;
}

static RECT Union(const RECT& a, const RECT& b) {
    return {std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right), std::max(a.bottom, b.bottom)};
}

static DWORD WINAPI Worker(LPVOID param) {
    std::shared_ptr<Anim> a(*(std::shared_ptr<Anim>*)param);
    delete (std::shared_ptr<Anim>*)param;
    DpiScope dpi;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    const bool zoom = a->kind >= Maximize;
    const bool rising = a->kind == Restore;
    const Snap& snap = *a->snap;
    const Geometry wGeom = {(float)a->from.left, (float)a->from.top, (float)snap.w, (float)snap.h};
    const Geometry iGeom = a->icon;

    // Canvas: the box the effect can occupy, clamped to the screens involved but
    // never clipping the window or its destination. The genie's sway pad is bounded
    // by max(|w.x-i.x|, |w.right-i.right|)/7.
    RECT dest = zoom ? a->toEst : RECT{(LONG)iGeom.x, (LONG)iGeom.y, (LONG)(iGeom.x + iGeom.width), (LONG)(iGeom.y + iGeom.height)};
    RECT box = Union(a->from, dest);
    if (!zoom) {
        float sway = std::max(fabsf(wGeom.x - iGeom.x), fabsf(wGeom.x + wGeom.width - iGeom.x - iGeom.width)) / 7.0f;
        int pad = (int)(std::max(wGeom.width / 2, sway) + 1);
        box.left -= pad; box.right += pad;
    }
    RECT mons = Union(MonitorOf(a->from), MonitorOf(dest));
    box = Union({std::max(box.left, mons.left), std::max(box.top, mons.top),
                 std::min(box.right, mons.right), std::min(box.bottom, mons.bottom)},
                Union(a->from, dest));
    const int vLeft = box.left, vTop = box.top;
    const int vW = std::max<int>(1, box.right - box.left), vH = std::max<int>(1, box.bottom - box.top - 1);

    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    BITMAPINFO bmi = {{sizeof(BITMAPINFOHEADER), vW, -vH, 1, 32, BI_RGB}};
    void* targetBits = nullptr;
    HBITMAP target = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &targetBits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(mem, target);
    HWND ghost = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                                 L"STATIC", nullptr, WS_POPUP, vLeft, vTop, vW, vH, nullptr, nullptr, nullptr, nullptr);

    ID2D1DCRenderTarget* rt = nullptr;
    ID2D1Bitmap* bmp = nullptr;
    ID2D1BitmapBrush* brush = nullptr;
    if (g_d2dFactory) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            0, 0, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE, D2D1_FEATURE_LEVEL_DEFAULT);
        g_d2dFactory->CreateDCRenderTarget(&props, &rt);
        if (rt) {
            rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            rt->CreateBitmap(D2D1::SizeU(snap.w, snap.h), snap.bits, snap.w * 4,
                             D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &bmp);
            if (bmp && !zoom)
                rt->CreateBitmapBrush(bmp, D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                                      D2D1_BITMAP_INTERPOLATION_MODE_LINEAR), &brush);
        }
    }

    // Frames are paced at 2x the refresh rate by a high-resolution timer (a rate
    // limiter, not a vsync gate), progress follows real elapsed time.
    double frameMs = 1000.0 / 240.0;
    DWM_TIMING_INFO ti = {sizeof(ti)};
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.rateRefresh.uiDenominator)
        frameMs = 1000.0 / ((double)ti.rateRefresh.uiNumerator / ti.rateRefresh.uiDenominator * 2.0);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_MODIFY_STATE | SYNCHRONIZE);

    LARGE_INTEGER freq, start, now, lastPresent = {};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    const double durMs = zoom ? kZoomMs : kGenieMs;
    int frames = 0;
    double sumGap = 0, maxGap = 0;
    bool first = true, swapped = false;
    std::vector<std::vector<D2D1_POINT_2F>> grid(kTiles + 1, std::vector<D2D1_POINT_2F>(kTiles + 1));

    while (rt && bmp && (zoom || brush)) {
        QueryPerformanceCounter(&now);
        const LARGE_INTEGER frameStart = now;
        double elapsed = (now.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
        bool last = elapsed >= durMs || a->abort.load() || g_unloading.load();
        float raw = (float)std::min(elapsed / durMs, 1.0);

        // Zoom: once the real window has its final size, swap in a fresh capture
        // (sharp at the new size) halfway through; the rects are unchanged so it doesn't jump.
        if (zoom && !swapped && raw > 0.5f && a->toReady.load()) {
            swapped = true;
            RECT ext;
            auto fresh = Capture(a->hwnd, &ext);
            if (fresh && abs((ext.right - ext.left) - (a->toFinal.right - a->toFinal.left)) < 4) {
                ID2D1Bitmap* nb = nullptr;
                rt->CreateBitmap(D2D1::SizeU(fresh->w, fresh->h), fresh->bits, fresh->w * 4,
                                 D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &nb);
                if (nb) { bmp->Release(); bmp = nb; a->snap = fresh; }
            }
        }

        RECT bind = {0, 0, vW, vH};
        rt->BindDC(mem, &bind);
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        bool degenerate = false;
        if (zoom) {
            float e = 1.0f - powf(1.0f - raw, 3.0f);  // ease-out
            const RECT& to = a->toReady.load() ? a->toFinal : a->toEst;
            auto lerp = [&](LONG f, LONG t) { return (float)f + ((float)t - (float)f) * e; };
            rt->DrawBitmap(bmp, D2D1::RectF(lerp(a->from.left, to.left) - vLeft, lerp(a->from.top, to.top) - vTop,
                                            lerp(a->from.right, to.right) - vLeft, lerp(a->from.bottom, to.bottom) - vTop),
                           1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            float eased = raw * raw * (3.0f - 2.0f * raw);
            float t = rising ? 1.0f - eased : eased;  // restore plays the warp in reverse
            for (int y = 0; y <= kTiles; y++)
                for (int x = 0; x <= kTiles; x++) {
                    float px, py;
                    LampVertex((float)x / kTiles, (float)y / kTiles, t, wGeom, iGeom, &px, &py);
                    grid[y][x] = D2D1::Point2F(px - vLeft, py - vTop);
                }
            float l = grid[0][0].x, r = l, tp = grid[0][0].y, b = tp;
            for (auto& row : grid) for (auto& p : row) { l = fminf(l, p.x); r = fmaxf(r, p.x); tp = fminf(tp, p.y); b = fmaxf(b, p.y); }
            degenerate = (r - l) < 2.0f || (b - tp) < 2.0f;  // a collapsed mesh would flash as a line
            ID2D1RoundedRectangleGeometry* mask = nullptr;
            float rad = fminf(12.0f, fminf(r - l, b - tp) / 2.0f);
            g_d2dFactory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2D1::RectF(l, tp, r, b), rad, rad), &mask);
            if (mask) {
                D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters();
                lp.geometricMask = mask;
                lp.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
                rt->PushLayer(&lp, nullptr);
                rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
                for (int y = 0; y < kTiles; y++)
                    for (int x = 0; x < kTiles; x++) {
                        D2D1_POINT_2F p1 = grid[y][x], p2 = grid[y][x + 1], p3 = grid[y + 1][x], p4 = grid[y + 1][x + 1];
                        D2D1_POINT_2F c = D2D1::Point2F((p1.x + p2.x + p3.x + p4.x) / 4, (p1.y + p2.y + p3.y + p4.y) / 4);
                        float qw = fmaxf(fabsf(p2.x - p1.x), fabsf(p4.x - p3.x)), qh = fmaxf(fabsf(p3.y - p1.y), fabsf(p4.y - p2.y));
                        float bl = fmaxf(0.20f, fminf(0.40f, fminf(qw, qh) * 0.05f));  // overlap keeps seams closed
                        ID2D1PathGeometry* quad = QuadGeo(Bloat(p1, c, bl), Bloat(p2, c, bl), Bloat(p4, c, bl), Bloat(p3, c, bl));
                        if (!quad) continue;
                        float sx = (float)x / kTiles * wGeom.width, sy = (float)y / kTiles * wGeom.height;
                        float sw = wGeom.width / kTiles, sh = wGeom.height / kTiles;
                        float m11 = (p2.x - p1.x) / sw, m12 = (p2.y - p1.y) / sw, m21 = (p3.x - p1.x) / sh, m22 = (p3.y - p1.y) / sh;
                        brush->SetTransform(D2D1::Matrix3x2F(m11, m12, m21, m22, p1.x - sx * m11 - sy * m21, p1.y - sx * m12 - sy * m22));
                        rt->FillGeometry(quad, brush);
                        quad->Release();
                    }
                rt->PopLayer();
                mask->Release();
            }
        }
        rt->EndDraw();

        if (!degenerate) {
            POINT dst = {vLeft, vTop}, src = {0, 0};
            SIZE sz = {vW, vH};
            BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            UpdateLayeredWindow(ghost, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
            if (first) ShowWindow(ghost, SW_SHOWNOACTIVATE);
            QueryPerformanceCounter(&now);
            if (lastPresent.QuadPart) {
                double gap = (now.QuadPart - lastPresent.QuadPart) * 1000.0 / freq.QuadPart;
                sumGap += gap; maxGap = std::max(maxGap, gap);
            }
            lastPresent = now;
            frames++;
        }
        if (last) break;
        if (first && !degenerate) {
            DwmFlush();  // the ghost is on screen before the caller changes the real window
            first = false;
            a->firstShown = true;
        }
        QueryPerformanceCounter(&now);
        double wait = frameMs - (now.QuadPart - frameStart.QuadPart) * 1000.0 / freq.QuadPart;
        if (timer && wait > 0.1) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)(wait * 10000.0);
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 100);
        } else {
            SwitchToThread();
        }
    }
    Wh_Log(L"genie kind=%d frames=%d avg=%.2fms max=%.2fms dur=%dms canvas=%dx%d", (int)a->kind, frames,
           frames > 1 ? sumGap / (frames - 1) : 0.0, maxGap, (int)durMs, vW, vH);

    if (timer) CloseHandle(timer);
    if (brush) brush->Release();
    if (bmp) bmp->Release();
    if (rt) rt->Release();

    // Reveal the real window under the ghost, then drop the ghost once DWM has composed it.
    if (a->cloaked) { Cloak(a->hwnd, FALSE); DwmFlush(); }
    SetTransitions(a->hwnd, TRUE);
    a->firstShown = true;
    SelectObject(mem, oldBmp);
    DeleteObject(target);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    DestroyWindow(ghost);
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
    auto a = Begin(h, kind);
    if (!a) return real();
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
    if (msg == WM_SYSCOMMAND) {
        Kind k;
        if (Classify(h, (int)(w & 0xFFF0), true, &k)) return Run(h, k, [&] { return DefWindowProcW_Original(h, msg, w, l); });
    } else if (msg == WM_DESTROY) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_cache.erase(h);
    }
    return DefWindowProcW_Original(h, msg, w, l);
}

BOOL Wh_ModInit() {
    g_haloMsg = RegisterWindowMessageW(L"Halo.DockIconForWindow");
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory), (void**)&g_d2dFactory)))
        g_d2dFactory = nullptr;
    Wh_SetFunctionHook((void*)DefWindowProcW, (void*)DefWindowProcW_Hook, (void**)&DefWindowProcW_Original);
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
    if (g_d2dFactory) { g_d2dFactory->Release(); g_d2dFactory = nullptr; }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cache.clear();
}
