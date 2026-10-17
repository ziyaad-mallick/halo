#pragma once
// Hover window previews using DWM live thumbnails.
// Shows thumbnail cards of an app's windows in MRU order when hovering its dock icon.

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "apps.h"

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

class PreviewManager {
public:
    static constexpr UINT_PTR kTimerId = 2;       // ID for show delay timer (not kHideTimer=1)
    static constexpr UINT kShowDelayMs = 250;     // 250 ms hover delay
    static constexpr UINT_PTR kHideTimerId = 1;   // Owner's hide timer
    static constexpr UINT kHideDelayMs = 500;     // Owner's hide delay

    explicit PreviewManager(HWND owner, WindowTracker const* tracker = nullptr)
        : m_owner(owner), m_tracker(tracker) {
        RegisterWindowClass();
        CreatePreviewWindow();
    }

    ~PreviewManager() {
        Hide();
        if (m_hwnd) {
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
        }
    }

    void SetTracker(WindowTracker const* tracker) {
        m_tracker = tracker;
    }

    void SetScale(float scale) {
        m_scale = scale;
    }

    HWND Hwnd() const { return m_hwnd; }
    bool IsShowing() const { return m_showing; }

    bool IsInCorridor(POINT pt) const {
        if (!m_showing) return false;
        if (PtInRect(&m_windowRect, pt)) return true;
        if (m_iconScreenX != 0) {
            int halfW = static_cast<int>(35.0f * GetScale());
            LONG left = std::min<LONG>(m_windowRect.left, m_iconScreenX - halfW);
            LONG right = std::max<LONG>(m_windowRect.right, m_iconScreenX + halfW);
            LONG top = m_windowRect.top;
            LONG bottom = m_pillTopScreenY + static_cast<LONG>(60.0f * GetScale());
            RECT corridor{left, top, right, bottom};
            if (PtInRect(&corridor, pt)) return true;
        }
        return false;
    }

    bool IsHovered() const {
        if (!m_showing || !m_hwnd) return false;
        POINT pt;
        if (!GetCursorPos(&pt)) return false;
        return IsInCorridor(pt);
    }

    void OnHover(int index, std::wstring const& key, int iconScreenX, int pillTopScreenY) {
        if (index < 0 || key.empty()) {
            if (m_showing) {
                POINT pt;
                if (GetCursorPos(&pt) && IsInCorridor(pt)) {
                    return;
                }
            }
            if (m_owner) KillTimer(m_owner, kTimerId);
            Hide();
            return;
        }

        if (index == m_hoverIndex && key == m_hoverKey) {
            m_iconScreenX = iconScreenX;
            m_pillTopScreenY = pillTopScreenY;
            return;
        }

        // Hover changed to a different icon
        if (m_owner) KillTimer(m_owner, kTimerId);
        Hide();

        m_hoverIndex = index;
        m_hoverKey = key;
        m_iconScreenX = iconScreenX;
        m_pillTopScreenY = pillTopScreenY;

        if (m_owner) {
            SetTimer(m_owner, kTimerId, kShowDelayMs, nullptr);
        }
    }

    void OnPointerLeave() {
        if (m_showing) {
            POINT pt;
            if (GetCursorPos(&pt) && IsInCorridor(pt)) {
                return;
            }
        }
        if (m_owner) KillTimer(m_owner, kTimerId);
        Hide();
    }

    void OnTimer() {
        if (m_owner) KillTimer(m_owner, kTimerId);
        Show();
    }

    void Hide() {
        if (m_owner) KillTimer(m_owner, kTimerId);
        UnregisterAll();
        if (m_hwnd && IsWindowVisible(m_hwnd)) {
            ShowWindow(m_hwnd, SW_HIDE);
        }
        m_showing = false;
        m_hoverIndex = -1;
        m_hoverKey.clear();
    }

    void Show() {
        if (!m_tracker || m_hoverKey.empty() || !m_hwnd) {
            Hide();
            return;
        }

        std::vector<HWND> wins = m_tracker->WindowsOf(m_hoverKey);
        struct SourceInfo {
            HWND hwnd;
            int sw;
            int sh;
        };
        std::vector<SourceInfo> valid;

        for (HWND h : wins) {
            if (!IsWindow(h)) continue;
            DWORD cloaked = 0;
            if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
                continue;
            }
            if (IsIconic(h)) {
                continue;
            }
            RECT cr{};
            if (!GetClientRect(h, &cr)) {
                continue;
            }
            int sw = cr.right - cr.left;
            int sh = cr.bottom - cr.top;
            if (sw <= 0 || sh <= 0) {
                continue;
            }
            valid.push_back({h, sw, sh});
            if (valid.size() == 6) break;
        }

        if (valid.empty()) {
            Hide();
            return;
        }

        const float scale = GetScale();
        const int cardH = static_cast<int>(std::round(160.0f * scale));
        const int minCardW = static_cast<int>(std::round(100.0f * scale));
        const int maxCardW = static_cast<int>(std::round(280.0f * scale));
        const int spacing = static_cast<int>(std::round(10.0f * scale));
        const int padding = static_cast<int>(std::round(12.0f * scale));
        const int anchorGap = static_cast<int>(std::round(10.0f * scale));

        std::vector<int> cardW(valid.size());
        int totalCardsW = 0;
        for (size_t i = 0; i < valid.size(); ++i) {
            int w = cardH * valid[i].sw / valid[i].sh;
            w = std::clamp(w, minCardW, maxCardW);
            cardW[i] = w;
            totalCardsW += w;
        }
        totalCardsW += static_cast<int>(valid.size() - 1) * spacing;

        int windowW = totalCardsW + 2 * padding;
        int windowH = cardH + 2 * padding;

        int windowX = m_iconScreenX - windowW / 2;
        int windowY = m_pillTopScreenY - anchorGap - windowH;

        // Clamp to monitor work area
        POINT anchorPt{m_iconScreenX, m_pillTopScreenY};
        HMONITOR hMon = MonitorFromPoint(anchorPt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(hMon, &mi)) {
            RECT work = mi.rcWork;
            if (windowX + windowW > work.right) windowX = work.right - windowW;
            if (windowX < work.left) windowX = work.left;
            if (windowY < work.top) windowY = work.top;
            if (windowY + windowH > work.bottom) windowY = work.bottom - windowH;
        }

        m_windowRect = {windowX, windowY, windowX + windowW, windowY + windowH};

        // Position window while hidden to eliminate flicker
        SetWindowPos(m_hwnd, HWND_TOPMOST, windowX, windowY, windowW, windowH, SWP_NOACTIVATE);

        UnregisterAll();

        int curX = padding;
        for (size_t i = 0; i < valid.size(); ++i) {
            RECT dest{curX, padding, curX + cardW[i], padding + cardH};
            HTHUMBNAIL th = nullptr;
            HRESULT hr = DwmRegisterThumbnail(m_hwnd, valid[i].hwnd, &th);
            if (SUCCEEDED(hr) && th) {
                DWM_THUMBNAIL_PROPERTIES p{};
                p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_SOURCECLIENTAREAONLY | DWM_TNP_VISIBLE | DWM_TNP_OPACITY;
                p.rcDestination = dest;
                p.fSourceClientAreaOnly = TRUE;
                p.fVisible = TRUE;
                p.opacity = 255;
                DwmUpdateThumbnailProperties(th, &p);
                m_cards.push_back({valid[i].hwnd, dest, th});
            }
            curX += cardW[i] + spacing;
        }

        ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        m_showing = true;
    }

private:
    struct Card {
        HWND hwnd{};
        RECT rect{};
        HTHUMBNAIL thumb{};
    };

    float GetScale() const {
        if (m_scale > 0.0f) return m_scale;
        if (m_owner) {
            UINT dpi = GetDpiForWindow(m_owner);
            if (dpi > 0) return static_cast<float>(dpi) / 96.0f;
        }
        return 1.0f;
    }

    void UnregisterAll() {
        for (auto& c : m_cards) {
            if (c.thumb) {
                DwmUnregisterThumbnail(c.thumb);
                c.thumb = nullptr;
            }
        }
        m_cards.clear();
    }

    static void RegisterWindowClass() {
        static bool s_registered = false;
        if (s_registered) return;
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"HaloPreviewWindow";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        RegisterClassExW(&wc);
        s_registered = true;
    }

    void CreatePreviewWindow() {
        DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
        DWORD style = WS_POPUP;
        m_hwnd = CreateWindowExW(exStyle, L"HaloPreviewWindow", L"", style,
                                 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (m_hwnd) {
            MARGINS margins{-1, -1, -1, -1};
            DwmExtendFrameIntoClientArea(m_hwnd, &margins);
            BOOL on = TRUE;
            DwmSetWindowAttribute(m_hwnd, 17 /* DWMWA_USE_HOSTBACKDROPBRUSH */, &on, sizeof(on));
            DWORD corner = DWMWCP_ROUND;
            DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        }
    }

    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* self = reinterpret_cast<PreviewManager*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        return self ? self->OnMessage(h, m, w, l) : DefWindowProcW(h, m, w, l);
    }

    LRESULT OnMessage(HWND h, UINT m, WPARAM w, LPARAM l) {
        switch (m) {
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;

            case WM_ERASEBKGND:
                return 1;

            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(h, &ps);
                HBRUSH br = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
                FillRect(dc, &ps.rcPaint, br);
                EndPaint(h, &ps);
                return 0;
            }

            case WM_MOUSEMOVE: {
                if (!m_trackingMouse) {
                    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                    m_trackingMouse = TrackMouseEvent(&tme);
                }
                // Hovering over preview window keeps the dock up
                if (m_owner) {
                    KillTimer(m_owner, kHideTimerId);
                }
                POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                bool onCard = false;
                for (auto const& c : m_cards) {
                    if (PtInRect(&c.rect, pt)) {
                        onCard = true;
                        break;
                    }
                }
                SetCursor(LoadCursorW(nullptr, onCard ? IDC_HAND : IDC_ARROW));
                return 0;
            }

            case WM_SETCURSOR:
                if (LOWORD(l) == HTCLIENT) {
                    return TRUE;
                }
                break;

            case WM_MOUSELEAVE: {
                m_trackingMouse = false;
                POINT pt;
                GetCursorPos(&pt);
                HWND wnd = WindowFromPoint(pt);
                if (wnd != m_owner) {
                    Hide();
                    if (m_owner) {
                        SetTimer(m_owner, kHideTimerId, kHideDelayMs, nullptr);
                    }
                }
                return 0;
            }

            case WM_LBUTTONDOWN:
                return 0;

            case WM_LBUTTONUP: {
                POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                for (auto const& c : m_cards) {
                    if (PtInRect(&c.rect, pt)) {
                        HWND target = c.hwnd;
                        if (IsWindow(target)) {
                            if (IsIconic(target)) {
                                SendMessageTimeoutW(target, WM_SYSCOMMAND, SC_RESTORE, 0, SMTO_ABORTIFHUNG, 200, nullptr);
                            }
                            SetForegroundWindow(target);
                        }
                        Hide();
                        break;
                    }
                }
                return 0;
            }
        }
        return DefWindowProcW(h, m, w, l);
    }

    HWND m_owner{};
    WindowTracker const* m_tracker{};
    HWND m_hwnd{};
    float m_scale = 0.0f;
    bool m_showing = false;
    bool m_trackingMouse = false;
    int m_hoverIndex = -1;
    std::wstring m_hoverKey;
    int m_iconScreenX = 0;
    int m_pillTopScreenY = 0;
    RECT m_windowRect{};
    std::vector<Card> m_cards;
};
