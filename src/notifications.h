// Notification Center: a standalone macOS-style panel that slides in from the right edge of the primary
// monitor: a header ("Notifications" + Clear), floating glass cards grouped by app, and a calendar.
// Notifications are read with UserNotificationListener (works unpackaged; see tools/notif_spike.cpp) and
// refreshed when the notification database changes (file change notification, no polling). Everything
// except that one waiting thread exists only while the panel is open.
#pragma once
#include <windows.h>
#include <winrt/Windows.UI.Composition.h>
#include <functional>
#include <memory>

namespace halo {

class NotificationCenter {
public:
    NotificationCenter(HINSTANCE inst, HWND owner, winrt::Windows::UI::Composition::Compositor comp,
                       winrt::Windows::UI::Composition::CompositionGraphicsDevice gdev);
    ~NotificationCenter();
    void Toggle();
    void Open();
    void Close();
    bool IsOpen() const;
    HWND Hwnd() const;  // null while closed
    // Attached mode: the Control Center's panel on screen (physical px). The column then sits right under
    // it with the same width and left edge, shows at most 3 cards and a compact calendar, takes no focus,
    // and moves with the CC's motion. Call before Open(), and again when the CC resizes.
    void SetAnchor(RECT ccPanel);
    // Esc / click outside: when set, called instead of closing only this panel (Halo closes both).
    std::function<void()> onDismiss;
    // Focus moving to this window must not dismiss the panel.
    std::function<HWND()> siblingHwnd;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

}  // namespace halo
