#pragma once
// Which apps are running, judged by the windows Alt+Tab would show.
//
// Event-driven: WinEvent hooks (out of context, delivered through the message
// loop of the thread that installs them) add and remove windows as they
// appear and disappear. There is no polling.

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

struct RunningApp {
    std::wstring key;    // the AUMID if the app has one, else the lower-cased exe path
    std::wstring aumid;  // may be empty
    std::wstring exe;    // full path of the app's process; may be empty
};

class WindowTracker {
public:
    // Posts `msg` to `notify` whenever an app starts or its last window goes away.
    WindowTracker(HWND notify, UINT msg);
    ~WindowTracker();

    std::vector<RunningApp> Apps() const;       // in the order the apps first appeared
    std::vector<HWND> WindowsOf(std::wstring const& key) const;  // the app's windows, most recently active first
    HWND Front(std::wstring const& key) const;  // the app's most recently active window, or null
    std::wstring KeyOf(HWND hwnd) const;        // the app a tracked window belongs to, or empty

    void OnEvent(DWORD event, HWND hwnd);  // WinEvent callback target

    // Called with the app of each tracked window that comes to the front.
    std::function<void(RunningApp const&)> onForeground;

private:
    struct Win {
        HWND hwnd;
        RunningApp app;
    };
    void Update(HWND hwnd);  // adds or drops hwnd depending on whether it is an app window now
    void Remove(HWND hwnd);
    void Changed();

    HWND m_notify;
    UINT m_msg;
    bool m_seeding = true;
    std::vector<Win> m_wins;  // most recently active first
    std::vector<RunningApp> m_apps;
    std::vector<HWINEVENTHOOK> m_hooks;
};
