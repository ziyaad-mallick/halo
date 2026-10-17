#pragma once
// Intellihide Window Overlap Detection for Halo.
//
// Keeps the dock visible at the bottom of the screen UNLESS a visible,
// non-minimized window overlaps the dock's screen rectangle, or a fullscreen
// app is active.
//
// Completely event-driven: uses SetWinEventHook for foreground changes, window
// movement/resizing (LOCATIONCHANGE), show/hide, cloak/uncloak, and minimize/restore.
// Also registers for Shell AppBar notifications (ABN_FULLSCREENAPP) for instant
// fullscreen detection.

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

#ifndef EVENT_OBJECT_CLOAKED
#define EVENT_OBJECT_CLOAKED 0x8017
#define EVENT_OBJECT_UNCLOAKED 0x8018
#endif

namespace halo {

class IntellihideDetector;
inline IntellihideDetector* g_intellihide = nullptr;

class IntellihideDetector {
public:
    using StateCallback = std::function<void(bool hasOverlap, bool isFullscreen)>;

    IntellihideDetector(HWND dockHwnd, UINT notifyMsg = 0)
        : m_dockHwnd(dockHwnd), m_notifyMsg(notifyMsg) {
        g_intellihide = this;
        InitHooks();
        RegisterAppBar();
    }

    ~IntellihideDetector() {
        UnregisterAppBar();
        RemoveHooks();
        if (g_intellihide == this) g_intellihide = nullptr;
    }

    // Sets callback invoked when overlap or fullscreen state transitions.
    void SetCallback(StateCallback cb) { m_callback = std::move(cb); }

    // Sets the dock's current bounding rectangle on the screen (in screen coordinates).
    void SetDockRect(RECT const& rc) {
        m_dockRect = rc;
        Evaluate(true);
    }

    // Sets the monitor the dock is currently on.
    void SetMonitor(HMONITOR mon, RECT const& monRect) {
        m_monitor = mon;
        m_monitorRect = monRect;
        Evaluate(true);
    }

    // Current states
    bool HasOverlap() const { return m_hasOverlap; }
    bool IsFullscreen() const { return m_isFullscreen; }
    RECT DockRect() const { return m_dockRect; }
    RECT MonitorRect() const { return m_monitorRect; }

    // Force an immediate evaluation
    void Evaluate(bool force = false) {
        auto now = std::chrono::steady_clock::now();
        if (!force && (now - m_lastCheck < std::chrono::milliseconds(16))) {
            return;  // throttle to ~60Hz during rapid window dragging
        }
        m_lastCheck = now;

        bool hasOverlap = false;
        bool isFullscreen = false;

        // 1. Inspect foreground window (fast path)
        HWND fg = GetForegroundWindow();
        if (fg && IsEligibleAppWindow(fg)) {
            RECT fgRect{};
            if (GetWindowVisualRect(fg, fgRect)) {
                // Check fullscreen on dock's monitor
                if (IsWindowCoveringMonitor(fgRect, m_monitorRect)) {
                    isFullscreen = true;
                    hasOverlap = true;
                } else {
                    RECT inter{};
                    if (IntersectRect(&inter, &fgRect, &m_dockRect)) {
                        hasOverlap = true;
                    }
                }
            }
        }

        // 2. If foreground window doesn't overlap dock or isn't fullscreen,
        // inspect visible top-level windows on the same monitor down the Z-order
        if (!isFullscreen) {
            struct EnumContext {
                IntellihideDetector* self;
                bool overlap;
                bool fullscreen;
                int count;
            } ctx{this, hasOverlap, false, 0};

            EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
                auto* c = reinterpret_cast<EnumContext*>(lp);
                if (c->count++ > 40) return FALSE;  // inspect top 40 windows at most
                if (!c->self->IsEligibleAppWindow(hwnd)) return TRUE;

                RECT wr{};
                if (!c->self->GetWindowVisualRect(hwnd, wr)) return TRUE;

                // Window must be on or touch dock's monitor
                RECT monInter{};
                if (!IntersectRect(&monInter, &wr, &c->self->m_monitorRect)) return TRUE;

                // Check fullscreen
                if (c->self->IsWindowCoveringMonitor(wr, c->self->m_monitorRect)) {
                    c->fullscreen = true;
                    c->overlap = true;
                    return FALSE;  // fullscreen window found, stop
                }

                // Check overlap with dock rect
                RECT dockInter{};
                if (IntersectRect(&dockInter, &wr, &c->self->m_dockRect)) {
                    c->overlap = true;
                    // Keep looking in case a higher window was fullscreen
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&ctx));

            hasOverlap = ctx.overlap;
            isFullscreen = ctx.fullscreen;
        }

        // Account for Shell AppBar fullscreen notification if received
        if (m_appbarFullscreen) {
            isFullscreen = true;
            hasOverlap = true;
        }

        // State change notification
        if (hasOverlap != m_hasOverlap || isFullscreen != m_isFullscreen) {
            m_hasOverlap = hasOverlap;
            m_isFullscreen = isFullscreen;
            if (m_callback) m_callback(m_hasOverlap, m_isFullscreen);
            if (m_notifyMsg && m_dockHwnd) {
                PostMessageW(m_dockHwnd, m_notifyMsg, m_hasOverlap ? 1 : 0, m_isFullscreen ? 1 : 0);
            }
        }
    }

    // Handles registered AppBar messages (for ABN_FULLSCREENAPP)
    LRESULT HandleAppBarMessage(UINT msg, WPARAM wp, LPARAM lp) {
        if (m_appbarRegistered && wp == ABN_FULLSCREENAPP) {
            m_appbarFullscreen = (lp != FALSE);
            Evaluate(true);
            return 0;
        }
        return 0;
    }

    // WinEvent hook handler
    void OnWinEvent(DWORD event, HWND hwnd) {
        if (!hwnd || hwnd == m_dockHwnd) return;

        // Skip non-top-level windows (controls, child windows)
        if (GetAncestor(hwnd, GA_PARENT) != GetDesktopWindow()) return;

        switch (event) {
            case EVENT_SYSTEM_FOREGROUND:
            case EVENT_OBJECT_SHOW:
            case EVENT_OBJECT_HIDE:
            case EVENT_SYSTEM_MINIMIZESTART:
            case EVENT_SYSTEM_MINIMIZEEND:
            case EVENT_OBJECT_CLOAKED:
            case EVENT_OBJECT_UNCLOAKED:
                Evaluate(true);
                break;

            case EVENT_OBJECT_LOCATIONCHANGE:
                // Window moved or resized
                Evaluate(false);
                break;
        }
    }

private:
    // Retrieves visible window rectangle excluding transparent DWM drop shadows
    bool GetWindowVisualRect(HWND hwnd, RECT& out) const {
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &out, sizeof(out)))) {
            if (out.right > out.left && out.bottom > out.top) return true;
        }
        return GetWindowRect(hwnd, &out) != 0;
    }

    // Checks if window rectangle covers the entire monitor (fullscreen)
    bool IsWindowCoveringMonitor(RECT const& win, RECT const& mon) const {
        // Tolerates 2px margin for borderless windows
        return (win.left <= mon.left + 2 &&
                win.top <= mon.top + 2 &&
                win.right >= mon.right - 2 &&
                win.bottom >= mon.bottom - 2);
    }

    // Filters out desktop windows, tooltips, hidden/iconic/cloaked windows
    bool IsEligibleAppWindow(HWND hwnd) const {
        if (!hwnd || hwnd == m_dockHwnd) return false;
        if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return false;

        // Ignore cloaked windows (other virtual desktops, suspended UWP)
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
            return false;
        }

        // Window style checks
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (ex & WS_EX_TOOLWINDOW && !(ex & WS_EX_APPWINDOW)) return false;
        if (ex & WS_EX_TRANSPARENT) return false;

        // Ignore known desktop and system shell windows
        wchar_t cls[64] = {};
        if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls))) {
            if (!wcscmp(cls, L"Progman") ||
                !wcscmp(cls, L"WorkerW") ||
                !wcscmp(cls, L"Shell_TrayWnd") ||
                !wcscmp(cls, L"Shell_SecondaryTrayWnd") ||
                !wcscmp(cls, L"HaloDockWindow") ||
                !wcscmp(cls, L"HaloGuardianWindow") ||
                !wcscmp(cls, L"HaloMonitorSentinel") ||
                !wcscmp(cls, L"#32768")) {  // popup menus
                return false;
            }
        }

        return true;
    }

    void InitHooks() {
        auto hook = [&](DWORD lo, DWORD hi) {
            HWINEVENTHOOK h = SetWinEventHook(lo, hi, nullptr, StaticWinEventProc, 0, 0,
                                              WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
            if (h) m_hooks.push_back(h);
        };

        hook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND);
        hook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE);
        hook(EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE);
        hook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND);
        hook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED);
    }

    void RemoveHooks() {
        for (auto h : m_hooks) UnhookWinEvent(h);
        m_hooks.clear();
    }

    void RegisterAppBar() {
        if (!m_dockHwnd || !m_notifyMsg) return;
        APPBARDATA abd{sizeof(abd)};
        abd.hWnd = m_dockHwnd;
        abd.uCallbackMessage = m_notifyMsg;
        if (SHAppBarMessage(ABM_NEW, &abd)) {
            m_appbarRegistered = true;
        }
    }

    void UnregisterAppBar() {
        if (m_appbarRegistered && m_dockHwnd) {
            APPBARDATA abd{sizeof(abd)};
            abd.hWnd = m_dockHwnd;
            SHAppBarMessage(ABM_REMOVE, &abd);
            m_appbarRegistered = false;
        }
    }

    static void CALLBACK StaticWinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
        if (g_intellihide && hwnd && idObject == OBJID_WINDOW && idChild == CHILDID_SELF) {
            g_intellihide->OnWinEvent(event, hwnd);
        }
    }

    HWND m_dockHwnd = nullptr;
    UINT m_notifyMsg = 0;
    HMONITOR m_monitor = nullptr;
    RECT m_monitorRect{};
    RECT m_dockRect{};

    bool m_hasOverlap = false;
    bool m_isFullscreen = false;
    bool m_appbarFullscreen = false;
    bool m_appbarRegistered = false;

    StateCallback m_callback;
    std::vector<HWINEVENTHOOK> m_hooks;
    std::chrono::steady_clock::time_point m_lastCheck{};
};

}  // namespace halo
