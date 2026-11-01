#include "swoop.h"

#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <new>
#include <string>
#include <vector>

#include "tokens.h"
#include "lamp_geometry.h"
#include "swoop_compositor.h"

namespace halo {
namespace {

constexpr ULONGLONG kModGraceMs = 300;    // the mod asked about a window this recently: it is animating it
constexpr float kRestoreAt = 1.0f;        // reveal the real window only once the ghost covers its full rectangle
constexpr float kSettleMs = 48;           // ghost stays over the real window this long after it un-minimizes: DWM needs a few frames to compose it
constexpr float kHoldMaxMs = 150;         // extra time the ghost waits for a slow app to un-minimize
constexpr float kLampMs = static_cast<float>(kLampSeconds * 1000);
constexpr int kLampStrips = 32;
constexpr UINT_PTR kFrameTimer = 1;
constexpr UINT kFrameMs = 8;              // high-resolution worker cadence; WM_TIMER is the fallback
constexpr UINT kFrameMessage = WM_APP + 30;
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
constexpr wchar_t kClass[] = L"HaloSwoop";

using Clock = std::chrono::steady_clock;

Swoop* g_self = nullptr;  // WinEvent callbacks and the overlay's WndProc carry no context

void Log(std::wstring const& msg) {
    auto line = L"[halo] swoop: " + msg + L"\n";
    OutputDebugStringW(line.c_str());
    fputws(line.c_str(), stderr);
    fflush(stderr);
}

// The window's rect as it is when restored, in screen px. A window minimized from maximized grew from the
// whole work area. (Workspace coordinates start at the work area; with Halo hiding the taskbar the two agree,
// but the offset is applied anyway.)
bool NormalRect(HWND hwnd, RECT& out) {
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!GetWindowPlacement(hwnd, &wp)) return false;
    RECT r = wp.rcNormalPosition;
    MONITORINFO pmi{sizeof(pmi)};
    if (!(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) &&
        GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &pmi))
        OffsetRect(&r, pmi.rcWork.left - pmi.rcMonitor.left, pmi.rcWork.top - pmi.rcMonitor.top);
    if (wp.flags & WPF_RESTORETOMAXIMIZED) {
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi)) r = mi.rcWork;
    }
    if (r.right - r.left < 16 || r.bottom - r.top < 16) return false;
    out = r;
    return true;
}

LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_ERASEBKGND: return 1;
        case WM_TIMER:
            if (w == kFrameTimer && g_self) g_self->OnFrame();
            return 0;
        case kFrameMessage:
            if (g_self) g_self->OnFrame();
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void CALLBACK HookProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (g_self && hwnd && idObject == OBJID_WINDOW && idChild == CHILDID_SELF) g_self->OnMinimize(hwnd);
}

}  // namespace

struct Swoop::Anim {
    HWND src{};
    HWND overlay{};
    struct Strip { HTHUMBNAIL thumb{}; RECT source{}; };
    std::vector<Strip> strips;
    std::unique_ptr<SwoopCompositor> compositor;
    RECT from{}, to{}, bounds{};
    bool leaving = true;
    bool acted = false;
    bool shown = false;           // restore: the real window is no longer minimized
    bool parked = false;          // restore: the real window is coming back off-screen, then slides home
    POINT home{};                 // where the parked window belongs (its window-rect top-left, screen px)
    Clock::time_point t0, shownAt;
    std::function<void()> action;  // restore: runs once, near the end of the grow
    HANDLE frameTimer{}, frameStop{}, frameThread{};
    std::atomic<bool> frameQueued{false};
    std::atomic<bool> renderFailed{false};
    ~Anim() {
        for (auto& strip : strips) if (strip.thumb) DwmUnregisterThumbnail(strip.thumb);
        if (frameTimer) { CancelWaitableTimer(frameTimer); CloseHandle(frameTimer); }
        if (frameStop) CloseHandle(frameStop);
        if (frameThread) CloseHandle(frameThread);
    }
#ifdef HALO_SWOOP_PROFILE
    Clock::time_point lastFrame{};
    double maxGap = 0, maxWork = 0, totalWork = 0;
    int frames = 0;
#endif
};

#ifdef HALO_SWOOP_TEST
bool Swoop::UsingCompositorForTest() const { return m_anim && m_anim->compositor != nullptr; }
#endif

DWORD WINAPI Swoop::FrameThread(void* context) {
    std::unique_ptr<std::shared_ptr<Anim>> owner(static_cast<std::shared_ptr<Anim>*>(context));
    auto a = std::move(*owner);
    HANDLE waiters[] = {a->frameStop, a->frameTimer};
    while (WaitForMultipleObjects(2, waiters, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        float ms = std::chrono::duration<float, std::milli>(Clock::now() - a->t0).count();
        float t = std::min(1.0f, ms / kLampMs);
        if (!RenderFrame(*a, t)) a->renderFailed = true;
        if (WaitForSingleObject(a->frameStop, 0) == WAIT_OBJECT_0) break;
        // Only lifecycle events need the dock thread. Frame updates remain here
        // even while the dock is processing input or another window is slow.
        if ((a->renderFailed || t >= 1 || (!a->leaving && t >= kRestoreAt)) &&
            !a->frameQueued.exchange(true) && !PostMessageW(a->overlay, kFrameMessage, 0, 0)) a->frameQueued = false;
    }
    return 0;
}

Swoop::Swoop(IconFor iconFor, bool enabled) : m_iconFor(std::move(iconFor)), m_on(enabled) {
    if (enabled) m_device = WarmSwoopDevice();
    g_self = this;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);  // already registered after a rebuild: fine
    m_hook = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZESTART, nullptr, HookProc, 0, 0,
                             WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

Swoop::~Swoop() {
    if (m_hook) UnhookWinEvent(static_cast<HWINEVENTHOOK>(m_hook));
    End();
    for (auto& [hwnd, off] : m_wins)
        if (off) Enable(hwnd);
    g_self = nullptr;
}

void Swoop::Disable(HWND hwnd) {
    auto it = m_wins.find(hwnd);
    if (it == m_wins.end() || it->second || IsHungAppWindow(hwnd)) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    BOOL off = TRUE;
    HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
    if (SUCCEEDED(hr)) it->second = true;
    else Log(L"transitions refused for pid " + std::to_wstring(pid) + L", hr " + std::to_wstring(static_cast<unsigned>(hr)));
}

void Swoop::Enable(HWND hwnd) {
    BOOL off = FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));  // fails harmlessly on a dead window
}

void Swoop::SetEnabled(bool on) {
    if (on == m_on) return;
    m_on = on;
    if (!on) End();
    if (on && !m_device) m_device = WarmSwoopDevice();
    if (!on) m_device.reset();
    for (auto& [hwnd, off] : m_wins) {
        if (on) Disable(hwnd);
        else if (off) { Enable(hwnd); off = false; }
    }
}

void Swoop::Track(HWND hwnd) {
    m_wins.emplace(hwnd, false);
    if (m_on) Disable(hwnd);
    else Enable(hwnd);  // off: clear a stale "transitions disabled" an earlier build may have left on this window
}

void Swoop::Forget(HWND hwnd) {
    auto it = m_wins.find(hwnd);
    if (it == m_wins.end()) return;
    if (m_anim && m_anim->src == hwnd) End();
    if (it->second) Enable(hwnd);
    m_wins.erase(hwnd);
    m_modAsked.erase(hwnd);
}

void Swoop::ModAsked(HWND hwnd) {
    ULONGLONG now = GetTickCount64();
    for (auto it = m_modAsked.begin(); it != m_modAsked.end();)
        it = now - it->second > 2000 ? m_modAsked.erase(it) : std::next(it);
    m_modAsked[hwnd] = now;
}

void Swoop::OnMinimize(HWND hwnd) {
    auto it = m_wins.find(hwnd);
    if (!m_on || it == m_wins.end() || !it->second || m_anim) return;  // a second window of a minimize-all just goes
    auto asked = m_modAsked.find(hwnd);
    if (asked != m_modAsked.end() && GetTickCount64() - asked->second < kModGraceMs) return;
    RECT from, icon;
    if (!NormalRect(hwnd, from) || !m_iconFor(hwnd, from, icon) || !Start(hwnd, from, icon, true, nullptr)) {
        // If there is no target or DWM refuses the overlay, retain the native
        // transition for future gestures instead of leaving this window stuck.
        Enable(hwnd);
        it->second = false;
    }
}

// A live thumbnail goes blank for a frame when its window leaves the minimized state. Done at the start of the grow
// that frame lands where the ghost is still transparent, so the window is restored early and kept off-screen
// (invisible, but still composed, so the ghost shows its live pixels) until the ghost is full size.
// A window that restores maximized can't be moved, so it keeps the late restore.
bool ParkMinimized(HWND hwnd, RECT const& from) {
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!GetWindowPlacement(hwnd, &wp) || wp.showCmd != SW_SHOWMINIMIZED || (wp.flags & WPF_RESTORETOMAXIMIZED)) return false;
    int width = from.right - from.left;
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN) - width - 200;
    OffsetRect(&wp.rcNormalPosition, x - from.left, 0);
    return SetWindowPlacement(hwnd, &wp) != 0;
}

void Swoop::MoveHome(Anim& a) {
    if (!a.parked) return;
    a.parked = false;
    RECT r{};
    // Only a window still sitting where we parked it: one the app moved itself stays where the app put it.
    if (GetWindowRect(a.src, &r) && r.right < GetSystemMetrics(SM_XVIRTUALSCREEN) + 1)
        SetWindowPos(a.src, nullptr, a.home.x, a.home.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool Swoop::Restore(HWND hwnd, std::function<void()> restore) {
    auto it = m_wins.find(hwnd);
    if (!m_on || it == m_wins.end() || !it->second || !IsIconic(hwnd) || IsHungAppWindow(hwnd)) return false;
    End();  // one ghost at a time; a half-played one finishes its restore first
    RECT from, icon;
    if (!NormalRect(hwnd, from) || !m_iconFor(hwnd, from, icon)) return false;
    WINDOWPLACEMENT original{sizeof(original)};
    GetWindowPlacement(hwnd, &original);
    bool park = ParkMinimized(hwnd, from);
    if (!Start(hwnd, from, icon, false, std::move(restore), park ? POINT{from.left, from.top} : POINT{LONG_MIN, 0})) {
        if (park) SetWindowPlacement(hwnd, &original);
        return false;
    }
    return true;
}

// Builds the overlay over the union of the two rects and plays the first frame. False if DWM won't thumbnail.
bool Swoop::Start(HWND hwnd, RECT const& from, RECT const& icon, bool leaving, std::function<void()> action, POINT home) {
    auto a = std::make_shared<Anim>();
    a->src = hwnd;
    a->from = from;
    a->to = icon;
    UnionRect(&a->bounds, &from, &icon);
    int pad = std::max(from.right - from.left, from.bottom - from.top) / 4 + 32;
    InflateRect(&a->bounds, pad, pad);
    a->leaving = leaving;
    a->overlay = CreateWindowExW(WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
                                     WS_EX_NOREDIRECTIONBITMAP,
                                 kClass, L"", WS_POPUP, a->bounds.left, a->bounds.top, a->bounds.right - a->bounds.left,
                                 a->bounds.bottom - a->bounds.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!a->overlay) return false;
    SetLayeredWindowAttributes(a->overlay, 0, 255, LWA_ALPHA);
#ifndef HALO_SWOOP_FORCE_THUMBNAILS
#ifdef HALO_SWOOP_TEST
    auto setupStarted = Clock::now();
#endif
    a->compositor = SwoopCompositor::Create(m_device, a->overlay, hwnd, from, icon, a->bounds, leaving);
#ifdef HALO_SWOOP_TEST
    fprintf(stderr, "compositor setup: active=%d elapsed=%.1fms\n", a->compositor != nullptr,
            std::chrono::duration<double, std::milli>(Clock::now()-setupStarted).count());
#endif
#endif
    if (!a->compositor) {
        HTHUMBNAIL first{};
        SIZE source{};
        if (FAILED(DwmRegisterThumbnail(a->overlay, hwnd, &first))) {
            DestroyWindow(a->overlay);
            return false;
        }
        if (FAILED(DwmQueryThumbnailSourceSize(first, &source)) || source.cx < 1 || source.cy < 1) {
            DwmUnregisterThumbnail(first);
            DestroyWindow(a->overlay);
            return false;
        }
        const int count = std::min(kLampStrips, (int)source.cy);
        a->strips.reserve(count);
        for (int i = 0; i < count; ++i) {
            HTHUMBNAIL thumb = i == 0 ? first : nullptr;
            if (i && FAILED(DwmRegisterThumbnail(a->overlay, hwnd, &thumb))) {
                for (auto& strip : a->strips) DwmUnregisterThumbnail(strip.thumb);
                a->strips.clear();
                DestroyWindow(a->overlay);
                return false;
            }
            a->strips.push_back({thumb, {0, source.cy * i / count, source.cx, source.cy * (i + 1) / count}});
        }
    }
    a->t0 = Clock::now();
    m_anim = std::move(a);
    OnFrame();  // first frame before the overlay is shown: the ghost starts exactly where the window was
    if (!m_anim) return false;
    if (!SetWindowPos(m_anim->overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
        End();
        return false;
    }
    Anim& started = *m_anim;
    if (started.compositor) {
        started.t0 = Clock::now();
        if (!started.compositor->Start()) { End(); return false; }
    }
#ifndef HALO_SWOOP_FORCE_LOWRES
    started.frameTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
#endif
    started.frameStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    LARGE_INTEGER due{}; due.QuadPart = -static_cast<LONGLONG>(started.compositor ? kLampMs : kFrameMs) * 10000;
    bool armed = started.frameTimer && SetWaitableTimer(started.frameTimer, &due, kFrameMs, nullptr, nullptr, FALSE);
    if (armed && started.frameStop) {
        auto* context = new (std::nothrow) std::shared_ptr<Anim>(m_anim);
        if (context) {
            started.frameThread = CreateThread(nullptr, 0, FrameThread, context, 0, nullptr);
            if (!started.frameThread) delete context;
        }
    }
    if (!started.frameThread && !SetTimer(started.overlay, kFrameTimer, kFrameMs, nullptr)) { End(); return false; }
    m_anim->action = std::move(action);
    if (home.x != LONG_MIN) {  // parked restore: switch the window to its live state now, while the ghost is still invisible
        m_anim->parked = true;
        m_anim->home = home;
        m_anim->acted = true;
        auto restore = std::move(m_anim->action);
        if (restore) restore();
    }
    return true;
}

bool Swoop::RenderFrame(Anim& a, float t) {
    if (a.compositor) return true;  // the compositor owns the complete timeline
#ifdef HALO_SWOOP_TEST_STALL
    if (t > .02f && t < 1) Sleep(200);  // model a delayed DWM call on the render worker
#endif
#ifdef HALO_SWOOP_PROFILE
    auto frameStart = Clock::now();
    if (a.frames) a.maxGap = std::max(a.maxGap, std::chrono::duration<double, std::milli>(frameStart - a.lastFrame).count());
    a.lastFrame = frameStart;
#endif
    try {
        double p = a.leaving ? t : 1 - t;
        BYTE opacity = static_cast<BYTE>(std::lround(255 * LampOpacity(p)));
        LampRect w{(double)a.from.left, (double)a.from.top, (double)(a.from.right-a.from.left), (double)(a.from.bottom-a.from.top)};
        LampRect icon{(double)a.to.left, (double)a.to.top, (double)(a.to.right-a.to.left), (double)(a.to.bottom-a.to.top)};
        int sourceHeight = a.strips.back().source.bottom;
        for (auto const& strip : a.strips) {
            double top = (double)strip.source.top / sourceHeight, bottom = (double)strip.source.bottom / sourceHeight;
            auto left = LampVertex(0, top, p, w, icon);
            auto right = LampVertex(1, top, p, w, icon);
            auto lower = LampVertex(0, bottom, p, w, icon);
            DWM_THUMBNAIL_PROPERTIES props{};
            props.dwFlags = DWM_TNP_RECTSOURCE | DWM_TNP_RECTDESTINATION | DWM_TNP_OPACITY | DWM_TNP_VISIBLE;
            props.fVisible = TRUE;
            props.opacity = opacity;
            props.rcSource = strip.source;
            props.rcDestination = {(LONG)std::lround(left.x)-a.bounds.left, (LONG)std::lround(left.y)-a.bounds.top,
                                   (LONG)std::lround(right.x)-a.bounds.left, (LONG)std::lround(lower.y)-a.bounds.top};
            // DWM rejects collapsed rectangles at the icon; the fade has reached zero there.
            if (props.rcDestination.right <= props.rcDestination.left || props.rcDestination.bottom <= props.rcDestination.top) {
                props.fVisible = FALSE;
                props.dwFlags = DWM_TNP_VISIBLE;
            }
            if (FAILED(DwmUpdateThumbnailProperties(strip.thumb, &props))) return false;
        }
#ifdef HALO_SWOOP_PROFILE
        double work = std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count();
        a.maxWork = std::max(a.maxWork, work);
        a.totalWork += work;
        ++a.frames;
#endif
        return true;
    } catch (...) { return false; }
}

// The dock thread only performs restore and cleanup; the timer thread renders.
void Swoop::OnFrame() {
    if (!m_anim) return;
    Anim& a = *m_anim;
    a.frameQueued = false;
    float ms = std::chrono::duration<float, std::milli>(Clock::now() - a.t0).count();
    float t = std::min(1.0f, ms / kLampMs);
    if (a.renderFailed) return End();
    if (!a.leaving && !a.acted && t >= kRestoreAt) {
        a.acted = true;
        if (a.action) a.action();
        if (!m_anim || m_anim.get() != &a) return;
    }
    if (t >= 1.0f && a.leaving) return End();
    if (t >= 1.0f && !a.shown && (!IsIconic(a.src) || ms > kLampMs + kHoldMaxMs)) {
        MoveHome(a);  // the ghost covers the full rectangle and shows this window's own pixels: the move is invisible
        a.shown = true;
        a.shownAt = Clock::now();
    }
    // The ghost is a live thumbnail of the same pixels, so keeping it a few frames costs nothing and hides the compose gap.
    if (a.shown && std::chrono::duration<float, std::milli>(Clock::now() - a.shownAt).count() >= kSettleMs) return End();
    if (!a.frameThread && !RenderFrame(a, t)) End();
}

// Destroys the overlay whatever state the animation is in. A restore that hadn't run yet runs now.
void Swoop::End() {
    if (!m_anim) return;
    std::shared_ptr<Anim> a = std::move(m_anim);
    KillTimer(a->overlay, kFrameTimer);
    if (a->frameStop) SetEvent(a->frameStop);
    DWORD stopped = a->frameThread ? WaitForSingleObject(a->frameThread, 50) : WAIT_OBJECT_0;
#ifdef HALO_SWOOP_PROFILE
    if (stopped == WAIT_OBJECT_0)
        fprintf(stderr, "swoop profile: frames=%d max-gap=%.2fms mean-work=%.2fms max-work=%.2fms\n", a->frames, a->maxGap,
                a->frames ? a->totalWork / a->frames : 0, a->maxWork);
#endif
    if (stopped != WAIT_OBJECT_0) Log(L"frame worker is still finishing; cleanup deferred");
    MoveHome(*a);
    DestroyWindow(a->overlay);
    // When the frame worker has stopped, transfer potentially expensive DWM
    // visual cleanup to a thread-pool worker. If it is still running, its shared
    // animation owns the compositor until it exits.
    if (stopped == WAIT_OBJECT_0 && a->compositor) SwoopCompositor::DisposeAsync(std::move(a->compositor));
    if (!a->leaving && !a->acted && a->action) {
        try { a->action(); } catch (...) { Log(L"restore threw"); }
    }
}

}  // namespace halo
