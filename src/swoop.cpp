#include "swoop.h"

#include <dwmapi.h>
#define PSAPI_VERSION 1  // psapi.dll's own exports; the K32 names need a newer import library than MinGW has
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <string>
#include <vector>

#include "tokens.h"

namespace halo {
namespace {

constexpr ULONGLONG kModGraceMs = 300;    // the mod asked about a window this recently: it is animating it
constexpr ULONGLONG kModCacheMs = 60000;  // a pid's answer is reused this long (pids are recycled)
constexpr float kRestoreAt = 0.65f;       // the real restore starts this far into the grow, so it lands under the ghost
constexpr float kHoldMaxMs = 150;         // extra time the ghost waits for a slow app to un-minimize
constexpr UINT_PTR kFrameTimer = 1;
constexpr UINT kFrameMs = 8;              // clamped to the system timer; progress is by clock, so jitter is invisible
constexpr wchar_t kClass[] = L"HaloSwoop";

using Clock = std::chrono::steady_clock;

Swoop* g_self = nullptr;  // WinEvent callbacks and the overlay's WndProc carry no context

void Log(std::wstring const& msg) {
    auto line = L"[halo] swoop: " + msg + L"\n";
    OutputDebugStringW(line.c_str());
    fputws(line.c_str(), stderr);
    fflush(stderr);
}

// True if the process has the genie mod loaded, or if we can't tell (elevated, protected, suspended): in both
// cases the stock behaviour is left alone.
bool ModLoaded(DWORD pid) {
#ifdef HALO_SWOOP_TEST
    (void)pid;
    return false;  // tools/swoop_test.cpp: the test window is injected with the real mod on a Windhawk machine
#endif
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) return true;
    bool found = false, ok = false;
    std::vector<HMODULE> mods(512);
    DWORD need = 0;
    for (int tries = 0; tries < 2; tries++) {
        ok = EnumProcessModulesEx(h, mods.data(), static_cast<DWORD>(mods.size() * sizeof(HMODULE)), &need, LIST_MODULES_ALL) != 0;
        if (!ok || need <= mods.size() * sizeof(HMODULE)) break;
        mods.resize(need / sizeof(HMODULE) + 16);  // it loaded more since the first call
    }
    if (ok) {
        mods.resize(std::min<size_t>(mods.size(), need / sizeof(HMODULE)));
        for (HMODULE m : mods) {
            wchar_t name[MAX_PATH];
            DWORD n = GetModuleBaseNameW(h, m, name, ARRAYSIZE(name));
            std::wstring s(name, n);
            for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
            if (s.find(L"halo-genie") != std::wstring::npos) { found = true; break; }
        }
    }
    CloseHandle(h);
    return found || !ok;
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
    HTHUMBNAIL thumb{};
    RECT from{}, to{}, bounds{};
    bool leaving = true;
    bool acted = false;
    Clock::time_point t0;
    std::function<void()> action;  // restore: runs once, near the end of the grow
};

Swoop::Swoop(IconFor iconFor, bool enabled) : m_iconFor(std::move(iconFor)), m_on(enabled) {
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
    ULONGLONG now = GetTickCount64();
    std::erase_if(m_hasMod, [now](auto const& entry) { return now - entry.second.first > kModCacheMs; });
    auto& c = m_hasMod[pid];
    if (c.first == 0 || now - c.first > kModCacheMs) {
        c = {now, ModLoaded(pid)};
        if (c.second) Log(L"pid " + std::to_wstring(pid) + L" left to its own animation (mod loaded, or process unreadable)");
    }
    if (c.second) return;
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
    for (auto& [hwnd, off] : m_wins) {
        if (on) Disable(hwnd);
        else if (off) { Enable(hwnd); off = false; }
    }
}

void Swoop::Track(HWND hwnd) {
    m_wins.emplace(hwnd, false);
    if (m_on) Disable(hwnd);
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
    if (!NormalRect(hwnd, from) || !m_iconFor(hwnd, from, icon)) return;
    Start(hwnd, from, icon, true, nullptr);
}

bool Swoop::Restore(HWND hwnd, std::function<void()> restore) {
    auto it = m_wins.find(hwnd);
    if (!m_on || it == m_wins.end() || !it->second || !IsIconic(hwnd) || IsHungAppWindow(hwnd)) return false;
    End();  // one ghost at a time; a half-played one finishes its restore first
    RECT from, icon;
    if (!NormalRect(hwnd, from) || !m_iconFor(hwnd, from, icon)) return false;
    return Start(hwnd, from, icon, false, std::move(restore));
}

// Builds the overlay over the union of the two rects and plays the first frame. False if DWM won't thumbnail.
bool Swoop::Start(HWND hwnd, RECT const& from, RECT const& icon, bool leaving, std::function<void()> action) {
    auto a = std::make_unique<Anim>();
    a->src = hwnd;
    a->from = from;
    a->to = icon;
    UnionRect(&a->bounds, &from, &icon);
    a->leaving = leaving;
    a->action = std::move(action);
    a->overlay = CreateWindowExW(WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
                                     WS_EX_NOREDIRECTIONBITMAP,
                                 kClass, L"", WS_POPUP, a->bounds.left, a->bounds.top, a->bounds.right - a->bounds.left,
                                 a->bounds.bottom - a->bounds.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!a->overlay) return false;
    SetLayeredWindowAttributes(a->overlay, 0, 255, LWA_ALPHA);
    if (FAILED(DwmRegisterThumbnail(a->overlay, hwnd, &a->thumb))) {
        DestroyWindow(a->overlay);
        return false;
    }
    a->t0 = Clock::now();
    m_anim = std::move(a);
    OnFrame();  // first frame before the overlay is shown: the ghost starts exactly where the window was
    if (!m_anim) return false;
    SetWindowPos(m_anim->overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetTimer(m_anim->overlay, kFrameTimer, kFrameMs, nullptr);
    return true;
}

// One frame, by the clock: leaving accelerates into the icon, entering decelerates out of it.
void Swoop::OnFrame() {
    if (!m_anim) return;
    Anim& a = *m_anim;
    try {
        float ms = std::chrono::duration<float, std::milli>(Clock::now() - a.t0).count();
        float t = std::min(1.0f, ms / ui::kSlowMs);
        if (!a.leaving && !a.acted && t >= kRestoreAt) {
            a.acted = true;
            if (a.action) a.action();
        }
        // done; a restore also waits (briefly) for a slow app to be un-minimized, so the ghost never leaves a gap
        if (t >= 1.0f && (a.leaving || !IsIconic(a.src) || ms > ui::kSlowMs + kHoldMaxMs)) return End();
        float p = a.leaving ? t * t : (1 - t) * (1 - t) * (1 - t);  // 0 = at the window, 1 = in the icon
        float alpha = 1 - std::clamp((p - 0.45f) / 0.55f, 0.0f, 1.0f);
        DWM_THUMBNAIL_PROPERTIES props{};
        props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_OPACITY | DWM_TNP_VISIBLE;
        props.fVisible = TRUE;
        props.opacity = static_cast<BYTE>(std::lround(alpha * 255));
        auto at = [&](LONG f, LONG g, LONG origin) { return static_cast<LONG>(std::lround(f + (g - f) * p)) - origin; };
        props.rcDestination = {at(a.from.left, a.to.left, a.bounds.left), at(a.from.top, a.to.top, a.bounds.top),
                               at(a.from.right, a.to.right, a.bounds.left), at(a.from.bottom, a.to.bottom, a.bounds.top)};
        if (FAILED(DwmUpdateThumbnailProperties(a.thumb, &props))) End();  // the window is gone
    } catch (...) {
        Log(L"frame threw");
        End();
    }
}

// Destroys the overlay whatever state the animation is in. A restore that hadn't run yet runs now.
void Swoop::End() {
    if (!m_anim) return;
    std::unique_ptr<Anim> a = std::move(m_anim);
    KillTimer(a->overlay, kFrameTimer);
    if (a->thumb) DwmUnregisterThumbnail(a->thumb);
    DestroyWindow(a->overlay);
    if (!a->leaving && !a->acted && a->action) {
        try { a->action(); } catch (...) { Log(L"restore threw"); }
    }
}

}  // namespace halo
