// Notification Center: a standalone macOS-style panel that slides in from the right edge of the primary
// monitor: a header ("Notifications" + Clear), floating glass cards grouped by app, and a calendar.
// Notifications are read with UserNotificationListener (works unpackaged; see tools/notif_spike.cpp) and
// refreshed when the notification database changes (file change notification, no polling). Everything
// except that one waiting thread exists only while the panel is open.
#pragma once
#include <windows.h>
#include <winrt/Windows.UI.Composition.h>
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

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

}  // namespace halo
