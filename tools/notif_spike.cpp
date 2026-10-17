// Feasibility spike: can an unpackaged exe use UserNotificationListener? (read-only: never removes anything)
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <winrt/Windows.ApplicationModel.h>
#include <cstdio>
using namespace winrt;
using namespace Windows::UI::Notifications;
using namespace Windows::UI::Notifications::Management;
int wmain() {
    try {
        init_apartment();
        auto l = UserNotificationListener::Current();
        printf("GetAccessStatus (before) = %d\n", (int)l.GetAccessStatus());
        try { auto s = l.RequestAccessAsync().get(); printf("RequestAccessAsync = %d (0 Unspecified,1 Allowed,2 Denied)\n", (int)s); }
        catch (hresult_error const& e) { printf("RequestAccessAsync threw 0x%08X %ls\n", (unsigned)e.code().value, e.message().c_str()); }
        printf("GetAccessStatus (after) = %d\n", (int)l.GetAccessStatus());
        try {
            auto n = l.GetNotificationsAsync(NotificationKinds::Toast).get();
            printf("GetNotificationsAsync ok, count=%u\n", n.Size());
            int k = 0;
            for (auto const& un : n) {
                if (k++ >= 5) break;
                auto b = un.Notification().Visual().GetBinding(L"ToastGeneric");
                auto t = b ? b.GetTextElements() : nullptr;
                printf("id=%u app=%ls t0=%ls t1=%ls\n", un.Id(), un.AppInfo().DisplayInfo().DisplayName().c_str(),
                       t && t.Size() > 0 ? t.GetAt(0).Text().c_str() : L"-", t && t.Size() > 1 ? t.GetAt(1).Text().c_str() : L"-");
            }
        } catch (hresult_error const& e) { printf("GetNotificationsAsync threw 0x%08X %ls\n", (unsigned)e.code().value, e.message().c_str()); }
        try {
            auto tok = l.NotificationChanged([](auto&&, auto&&) { printf("EVENT\n"); });
            printf("NotificationChanged subscribe ok\n");
            Sleep(100);
            l.NotificationChanged(tok);
        } catch (hresult_error const& e) { printf("NotificationChanged threw 0x%08X %ls\n", (unsigned)e.code().value, e.message().c_str()); }
    } catch (hresult_error const& e) { printf("fatal 0x%08X %ls\n", (unsigned)e.code().value, e.message().c_str()); }
}
