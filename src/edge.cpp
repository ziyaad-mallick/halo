#include "edge.h"

#include <shellscalingapi.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>

namespace halo::edge {
namespace {

constexpr UINT kCornerDwellMs = 120;   // resting in the hot zone this long opens the Control Center
constexpr float kZone = 0.2f;          // the hot zone: this fraction of the top edge (from the right) and of the right edge (from the top)
constexpr DWORD kPushMs = 140;        // how long the pointer must keep pushing into the right edge
constexpr int kPushEvents = 6;        // ... and in how many move events
constexpr DWORD kPushGapMs = 70;      // a pause longer than this means the hand stopped: start over
constexpr DWORD kCooldownMs = 700;    // after a gesture fires
constexpr float kNearBand = 160;      // logical px from an edge where the strips turn click-through
constexpr float kStrip = 4;           // touch strip thickness, logical px
constexpr float kSwipe = 36;          // touch travel that counts as a swipe, logical px
constexpr ULONG_PTR kTouchSignatureMask = 0xFFFFFF00, kTouchSignature = 0xFF515700;  // MI_WP_SIGNATURE
constexpr UINT kMsgNear = WM_APP + 1;  // to the top strip: wParam = 1 mouse near an edge, 0 far
constexpr wchar_t kStripClass[] = L"HaloEdgeStrip";

HWND g_target{}, g_top{}, g_right{};
UINT g_msg = 0;
HINSTANCE g_inst{};
HANDLE g_thread{};
DWORD g_threadId = 0;
HHOOK g_hook{};
std::atomic<long> g_left{0}, g_topY{0}, g_rightX{0}, g_bottom{0}, g_band{160};
std::atomic<bool> g_near{false};
POINT g_touchStart{-1, -1};
float g_scale = 1.0f;

void Fire(WPARAM which) { PostMessageW(g_target, g_msg, which, 0); }

// ---- mouse (hook thread) -------------------------------------------------------------------

struct Mouse {
    bool inCorner = false;
    UINT_PTR cornerTimer = 0;
    POINT last{-1, -1};
    DWORD pushStart = 0, lastPush = 0, cooldownUntil = 0;
    int pushEvents = 0, pushDy = 0;
} g_m;

// The right fifth of the top edge, or the top fifth of the right edge.
bool InCorner(POINT p) {
    const long w = g_rightX - g_left, h = g_bottom - g_topY;
    return (p.y <= g_topY + 1 && p.x >= g_rightX - static_cast<long>(w * kZone)) ||
           (p.x >= g_rightX - 1 && p.y <= g_topY + static_cast<long>(h * kZone));
}

void CALLBACK CornerTimer(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    g_m.cornerTimer = 0;
    POINT p;
    if (g_m.inCorner && GetCursorPos(&p) && InCorner(p) && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
        g_m.cooldownUntil = GetTickCount() + kCooldownMs;
        Fire(kTop);
    }
}

void CancelCorner() {
    g_m.inCorner = false;
    if (g_m.cornerTimer) KillTimer(nullptr, g_m.cornerTimer);
    g_m.cornerTimer = 0;
}

void OnMove(POINT p, DWORD t) {
    const long band = g_band;
    bool nearEdge = p.x >= g_rightX - band || p.y <= g_topY + band;
    if (nearEdge != g_near.exchange(nearEdge) && g_top) PostMessageW(g_top, kMsgNear, nearEdge, 0);

    const bool cooling = static_cast<LONG>(t - g_m.cooldownUntil) < 0;
    if (InCorner(p)) {
        if (!g_m.inCorner && !cooling) {
            g_m.inCorner = true;
            g_m.cornerTimer = SetTimer(nullptr, 0, kCornerDwellMs, &CornerTimer);
        }
    } else if (g_m.inCorner) {
        CancelCorner();
    }

    // The hook sees every move report, even once the cursor is pinned against the edge, so a hand
    // that keeps pushing right produces a run of reports at the edge with almost no vertical travel.
    const bool atEdge = p.x >= g_rightX - 1 && !InCorner(p);
    const bool wasAtEdge = g_m.last.x >= g_rightX - 1;
    if (atEdge && wasAtEdge && !cooling) {
        if (!g_m.pushEvents || t - g_m.lastPush > kPushGapMs) { g_m.pushEvents = 0; g_m.pushDy = 0; g_m.pushStart = t; }
        g_m.pushEvents++;
        g_m.pushDy += std::abs(p.y - g_m.last.y);
        g_m.lastPush = t;
        if (g_m.pushDy > 24) {
            g_m.pushEvents = 0;  // sliding along the edge (a scrollbar drag), not pushing into it
        } else if (g_m.pushEvents >= kPushEvents && t - g_m.pushStart >= kPushMs) {
            g_m.pushEvents = 0;
            g_m.cooldownUntil = t + kCooldownMs;
            Fire(kRight);
        }
    } else if (!atEdge) {
        g_m.pushEvents = 0;
    }
    g_m.last = p;
}

LRESULT CALLBACK MouseHook(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) {
        auto* m = reinterpret_cast<MSLLHOOKSTRUCT*>(l);
        if ((m->dwExtraInfo & kTouchSignatureMask) != kTouchSignature) {  // touch and pen: the strips
            if (w == WM_MOUSEMOVE) OnMove(m->pt, m->time);
            else if (w == WM_LBUTTONDOWN || w == WM_RBUTTONDOWN) { CancelCorner(); g_m.pushEvents = 0; }
        }
    }
    return CallNextHookEx(nullptr, code, w, l);
}

DWORD WINAPI HookThread(LPVOID ready) {
    MSG m;
    PeekMessageW(&m, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // make the queue before signalling
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, &MouseHook, GetModuleHandleW(nullptr), 0);
    SetEvent(static_cast<HANDLE>(ready));
    while (GetMessageW(&m, nullptr, 0, 0) > 0) DispatchMessageW(&m);  // also runs the corner timer
    if (g_hook) UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
    return 0;
}

// ---- touch strips (UI thread) --------------------------------------------------------------

void SetClickThrough(HWND h, bool through) {
    if (!h) return;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    LONG_PTR want = through ? (ex | WS_EX_TRANSPARENT) : (ex & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
    if (want != ex) SetWindowLongPtrW(h, GWL_EXSTYLE, want);
}

LRESULT CALLBACK StripProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case kMsgNear:
            SetClickThrough(g_top, w != 0);
            SetClickThrough(g_right, w != 0);
            return 0;
        case WM_MOUSEMOVE:  // a real mouse got here before the hook's note did: let it through
            if ((GetMessageExtraInfo() & kTouchSignatureMask) != kTouchSignature) {
                SetClickThrough(g_top, true);
                SetClickThrough(g_right, true);
            }
            return 0;
        case WM_POINTERDOWN:
        case WM_POINTERUPDATE: {
            POINTER_INFO pi{};
            if (!GetPointerInfo(GET_POINTERID_WPARAM(w), &pi) || pi.pointerType == PT_MOUSE) break;
            if (m == WM_POINTERDOWN) { g_touchStart = pi.ptPixelLocation; return 0; }
            if (g_touchStart.x < 0 || !IS_POINTER_INCONTACT_WPARAM(w)) return 0;
            const long dx = pi.ptPixelLocation.x - g_touchStart.x, dy = pi.ptPixelLocation.y - g_touchStart.y;
            const float need = kSwipe * g_scale;
            if (h == g_top && dy >= need && dy > std::abs(dx)) { g_touchStart = {-1, -1}; Fire(kTop); }
            else if (h == g_right && -dx >= need && -dx > std::abs(dy)) { g_touchStart = {-1, -1}; Fire(kRight); }
            return 0;
        }
        case WM_POINTERUP:
        case WM_POINTERCAPTURECHANGED:
            g_touchStart = {-1, -1};
            break;
    }
    return DefWindowProcW(h, m, w, l);
}

HWND MakeStrip() {
    HWND h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kStripClass,
                             L"HaloEdgeStrip", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_inst, nullptr);
    if (h) SetLayeredWindowAttributes(h, 0, 1, LWA_ALPHA);  // invisible but still hit-testable
    return h;
}

void Place() {
    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpi = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi, &dy);
    g_scale = dpi / 96.0f;
    const RECT r = mi.rcMonitor;
    g_left = r.left; g_topY = r.top; g_rightX = r.right - 1; g_bottom = r.bottom - 1;
    g_band = static_cast<long>(kNearBand * g_scale);
    const int t = std::max(2, static_cast<int>(kStrip * g_scale));
    // The top strip stops short of the right strip; the right strip skips the top-right corner.
    if (g_top) SetWindowPos(g_top, HWND_TOPMOST, r.left, r.top, r.right - r.left - t, t, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (g_right) SetWindowPos(g_right, HWND_TOPMOST, r.right - t, r.top + t, t, r.bottom - r.top - t, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

}  // namespace

bool Start(HINSTANCE inst, HWND target, UINT msg) {
    g_inst = inst; g_target = target; g_msg = msg;
    WNDCLASSW wc{};
    wc.lpfnWndProc = &StripProc;
    wc.hInstance = inst;
    wc.lpszClassName = kStripClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_top = MakeStrip();
    g_right = MakeStrip();
    Place();
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, &HookThread, ready, 0, &g_threadId);
    if (g_thread) WaitForSingleObject(ready, 2000);
    CloseHandle(ready);
    return g_hook != nullptr;
}

void Stop() {
    if (g_thread) {
        PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
        WaitForSingleObject(g_thread, 1000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_top) DestroyWindow(g_top);
    if (g_right) DestroyWindow(g_right);
    g_top = g_right = nullptr;
}

void OnDisplayChange() { Place(); }

}  // namespace halo::edge
