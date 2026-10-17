#pragma once
// Multi-Monitor Cursor Following for Halo.
//
// Automatically detects when the cursor pushes against the bottom edge of any
// connected monitor (using MonitorFromPoint and zero-CPU event-driven sentinel
// hit strips), and smoothly migrates the dock window (SetWindowPos) to that
// monitor's bottom coordinates, adapting DPI and metrics.
//
// Like macOS, the user can summon the dock onto any screen simply by moving
// their cursor to the bottom edge of that display.

#include <windows.h>
#include <shellscalingapi.h>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace halo {

class MultiMonitorFollower;
inline MultiMonitorFollower* g_multimon = nullptr;

class MultiMonitorFollower {
public:
    using MonitorSwitchCallback = std::function<void(HMONITOR newMon, MONITORINFO const& mi, float dpiScale)>;

    MultiMonitorFollower(HWND dockHwnd, UINT notifyMsg = 0)
        : m_dockHwnd(dockHwnd), m_notifyMsg(notifyMsg) {
        g_multimon = this;
        RegisterSentinelClass();
        UpdateMonitors();
    }

    ~MultiMonitorFollower() {
        DestroySentinels();
        UnregisterClassW(kSentinelClassName, GetModuleHandleW(nullptr));
        if (g_multimon == this) g_multimon = nullptr;
    }

    void SetCallback(MonitorSwitchCallback cb) { m_callback = std::move(cb); }

    // Sets the monitor the dock is currently stationed on.
    void SetCurrentMonitor(HMONITOR mon) {
        if (m_currentMon != mon) {
            m_currentMon = mon;
            UpdateSentinels();
        }
    }

    HMONITOR CurrentMonitor() const { return m_currentMon; }

    // Checks if a screen point is within the bottom edge threshold of a monitor
    static bool IsAtBottomEdge(HMONITOR hMon, POINT pt, int thresholdPx = 4) {
        if (!hMon) return false;
        MONITORINFO mi{sizeof(mi)};
        if (!GetMonitorInfoW(hMon, &mi)) return false;
        return (pt.x >= mi.rcMonitor.left &&
                pt.x < mi.rcMonitor.right &&
                pt.y >= mi.rcMonitor.bottom - thresholdPx &&
                pt.y <= mi.rcMonitor.bottom);
    }

    // Called when display layout changes (WM_DISPLAYCHANGE)
    void OnDisplayChange() {
        UpdateMonitors();
    }

    // Evaluates cursor position for monitor switch (called by sentinel hit or manual check)
    bool CheckCursor(POINT pt) {
        HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        if (!hMon || hMon == m_currentMon) return false;

        if (IsAtBottomEdge(hMon, pt, 6)) {
            SwitchToMonitor(hMon);
            return true;
        }
        return false;
    }

    // Handles notification message sent from sentinel windows
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == m_notifyMsg && wp) {
            HMONITOR hMon = reinterpret_cast<HMONITOR>(wp);
            if (hMon != m_currentMon) {
                SwitchToMonitor(hMon);
                return 1;
            }
        }
        return 0;
    }

    // Executes monitor switch: queries new monitor DPI and metrics, notifies listener
    void SwitchToMonitor(HMONITOR targetMon) {
        if (!targetMon || targetMon == m_currentMon) return;

        MONITORINFO mi{sizeof(mi)};
        if (!GetMonitorInfoW(targetMon, &mi)) return;

        UINT dpiX = 96, dpiY = 96;
        GetDpiForMonitor(targetMon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
        float newScale = dpiX / 96.0f;

        m_currentMon = targetMon;
        UpdateSentinels();

        if (m_callback) {
            m_callback(targetMon, mi, newScale);
        }
        if (m_notifyMsg && m_dockHwnd) {
            PostMessageW(m_dockHwnd, m_notifyMsg, reinterpret_cast<WPARAM>(targetMon), 0);
        }
    }

    // Enumerates all connected active monitors
    std::vector<HMONITOR> GetAllMonitors() const {
        std::vector<HMONITOR> result;
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR h, HDC, LPRECT, LPARAM lp) -> BOOL {
            reinterpret_cast<std::vector<HMONITOR>*>(lp)->push_back(h);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&result));
        return result;
    }

private:
    static constexpr wchar_t kSentinelClassName[] = L"HaloMonitorSentinel";
    static constexpr int kSentinelHeightPx = 2;

    void RegisterSentinelClass() {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = SentinelWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kSentinelClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
    }

    void UpdateMonitors() {
        auto all = GetAllMonitors();
        if (m_currentMon == nullptr && !all.empty()) {
            m_currentMon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        }
        UpdateSentinels();
    }

    // Maintains invisible 2px hit-strip windows at the bottom of secondary monitors
    void UpdateSentinels() {
        auto all = GetAllMonitors();
        std::vector<HMONITOR> needed;
        for (auto m : all) {
            if (m != m_currentMon) needed.push_back(m);
        }

        // Destroy sentinels for monitors that are no longer secondary
        for (auto it = m_sentinels.begin(); it != m_sentinels.end();) {
            if (std::find(needed.begin(), needed.end(), it->first) == needed.end()) {
                if (it->second && IsWindow(it->second)) DestroyWindow(it->second);
                it = m_sentinels.erase(it);
            } else {
                ++it;
            }
        }

        // Create or reposition sentinels on all secondary monitors
        for (auto m : needed) {
            MONITORINFO mi{sizeof(mi)};
            if (!GetMonitorInfoW(m, &mi)) continue;

            int x = mi.rcMonitor.left;
            int y = mi.rcMonitor.bottom - kSentinelHeightPx;
            int w = mi.rcMonitor.right - mi.rcMonitor.left;
            int h = kSentinelHeightPx;

            auto it = m_sentinels.find(m);
            if (it == m_sentinels.end() || !IsWindow(it->second)) {
                HWND hwnd = CreateWindowExW(
                    WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                    kSentinelClassName, L"HaloSentinel", WS_POPUP,
                    x, y, w, h, nullptr, nullptr, GetModuleHandleW(nullptr),
                    reinterpret_cast<void*>(m));
                if (hwnd) {
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(m));
                    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                    m_sentinels[m] = hwnd;
                }
            } else {
                SetWindowPos(it->second, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
            }
        }
    }

    void DestroySentinels() {
        for (auto const& pair : m_sentinels) {
            if (pair.second && IsWindow(pair.second)) {
                DestroyWindow(pair.second);
            }
        }
        m_sentinels.clear();
    }

    static LRESULT CALLBACK SentinelWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_MOUSEMOVE) {
            POINT pt;
            GetCursorPos(&pt);
            HMONITOR hMon = reinterpret_cast<HMONITOR>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (g_multimon && hMon) {
                g_multimon->HandleSentinelHit(hMon, pt);
            }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void HandleSentinelHit(HMONITOR hMon, POINT pt) {
        if (hMon && hMon != m_currentMon) {
            SwitchToMonitor(hMon);
        }
    }

    HWND m_dockHwnd = nullptr;
    UINT m_notifyMsg = 0;
    HMONITOR m_currentMon = nullptr;
    std::map<HMONITOR, HWND> m_sentinels;
    MonitorSwitchCallback m_callback;
};

}  // namespace halo
