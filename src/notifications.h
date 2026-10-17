// Notification stack: Windows toast notifications as floating glass cards under the Control Center.
// Reads them with UserNotificationListener (works unpackaged; see tools/notif_spike.cpp), refreshes when
// the notification database changes (file change notification, no polling), lives only while shown.
#pragma once
#include <windows.h>
#include <winrt/Windows.UI.Composition.h>
#include <memory>

namespace halo {

class NotificationStack {
public:
    NotificationStack(HINSTANCE inst, winrt::Windows::UI::Composition::Compositor comp,
                      winrt::Windows::UI::Composition::CompositionGraphicsDevice gdev);
    ~NotificationStack();
    void Show(RECT panelScreen);  // idempotent; places the stack under the panel
    void Hide();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

}  // namespace halo
