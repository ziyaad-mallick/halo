#include "apps.h"

#include <algorithm>
#include <cwctype>

#include <appmodel.h>
#include <dwmapi.h>
#include <propidl.h>
#include <shlobj.h>

#ifndef EVENT_OBJECT_CLOAKED
#define EVENT_OBJECT_CLOAKED 0x8017
#define EVENT_OBJECT_UNCLOAKED 0x8018
#endif

namespace {

WindowTracker* g_tracker = nullptr;  // WinEvent callbacks carry no context

// PKEY_AppUserModel_ID (the MinGW headers don't define the key).
const PROPERTYKEY kPkeyAumid = {{0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring ExeOf(DWORD pid) {
    std::wstring path;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = ARRAYSIZE(buf);
        if (QueryFullProcessImageNameW(h, 0, buf, &n)) path.assign(buf, n);
        CloseHandle(h);
    }
    return path;
}

std::wstring AumidOfProcess(DWORD pid) {
    std::wstring id;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[130];  // APPLICATION_USER_MODEL_ID_MAX_LENGTH
        UINT32 n = ARRAYSIZE(buf);
        if (GetApplicationUserModelId(h, &n, buf) == ERROR_SUCCESS && n > 1) id.assign(buf, n - 1);
        CloseHandle(h);
    }
    return id;
}

std::wstring AumidOfWindow(HWND hwnd) {
    std::wstring id;
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(hwnd, IID_IPropertyStore, reinterpret_cast<void**>(&ps)))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(kPkeyAumid, &v)) && v.vt == VT_LPWSTR && v.pwszVal) id = v.pwszVal;
        PropVariantClear(&v);
        ps->Release();
    }
    return id;
}

// Packaged apps are hosted by ApplicationFrameHost.exe; the real app owns
// the frame's CoreWindow child.
DWORD CoreWindowPid(HWND frame) {
    DWORD pid = 0;
    EnumChildWindows(frame, [](HWND child, LPARAM l) -> BOOL {
        wchar_t cls[64];
        if (GetClassNameW(child, cls, ARRAYSIZE(cls)) && !wcscmp(cls, L"Windows.UI.Core.CoreWindow")) {
            GetWindowThreadProcessId(child, reinterpret_cast<DWORD*>(l));
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&pid));
    return pid;
}

// Alt+Tab's rules (cheap checks only; terminals retitle themselves constantly).
bool IsAppWindow(HWND hwnd) {
    if (!IsWindowVisible(hwnd)) return false;
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (!(ex & WS_EX_APPWINDOW)) {
        if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return false;
        if (GetWindow(hwnd, GW_OWNER)) return false;
    }
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
    return GetWindowTextLengthW(hwnd) != 0;
}

// Works out which app owns the window; false if it has no identity.
bool Identify(HWND hwnd, RunningApp& app) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return false;
    app.exe = ExeOf(pid);  // (empty for elevated processes)
    app.aumid = AumidOfWindow(hwnd);
    constexpr wchar_t kHost[] = L"ApplicationFrameHost.exe";
    constexpr size_t kHostLen = ARRAYSIZE(kHost) - 1;
    if (app.exe.size() >= kHostLen && !_wcsicmp(app.exe.c_str() + app.exe.size() - kHostLen, kHost)) {
        DWORD real = CoreWindowPid(hwnd);
        if (!real) return false;  // the CoreWindow isn't attached yet; a later event retries
        app.exe = ExeOf(real);
        if (app.aumid.empty()) app.aumid = AumidOfProcess(real);
    }
    // Packaged desktop apps (Claude, Spotify, Terminal) set no window ID, but
    // their process has one, and it is what their pin carries.
    if (app.aumid.empty()) app.aumid = AumidOfProcess(pid);
    // File Explorer windows carry no ID; the pin knows it by this one.
    constexpr wchar_t kExplorer[] = L"\\explorer.exe";
    constexpr size_t kExplorerLen = ARRAYSIZE(kExplorer) - 1;
    if (app.aumid.empty() && app.exe.size() >= kExplorerLen &&
        !_wcsicmp(app.exe.c_str() + app.exe.size() - kExplorerLen, kExplorer))
        app.aumid = L"Microsoft.Windows.Explorer";
    app.key = app.aumid.empty() ? Lower(app.exe) : app.aumid;
    return !app.key.empty();
}

void CALLBACK HookProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (g_tracker && hwnd && idObject == OBJID_WINDOW && idChild == CHILDID_SELF) g_tracker->OnEvent(event, hwnd);
}

}  // namespace

WindowTracker::WindowTracker(HWND notify, UINT msg) : m_notify(notify), m_msg(msg) {
    g_tracker = this;
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        reinterpret_cast<WindowTracker*>(l)->Update(h);
        return TRUE;
    }, reinterpret_cast<LPARAM>(this));  // top of the z-order first, a fair guess at recency
    m_seeding = false;
    if (HWND fg = GetForegroundWindow()) OnEvent(EVENT_SYSTEM_FOREGROUND, fg);

    auto hook = [&](DWORD lo, DWORD hi) {
        m_hooks.push_back(SetWinEventHook(lo, hi, nullptr, HookProc, 0, 0,
                                          WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS));
    };
    hook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE);  // create, destroy, show, hide
    hook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE);
    hook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED);
    hook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND);
}

WindowTracker::~WindowTracker() {
    for (auto h : m_hooks) UnhookWinEvent(h);
    g_tracker = nullptr;
}

std::vector<RunningApp> WindowTracker::Apps() const { return m_apps; }

HWND WindowTracker::Front(std::wstring const& key) const {
    for (auto const& w : m_wins)
        if (w.app.key == key) return w.hwnd;
    return nullptr;
}

std::wstring WindowTracker::KeyOf(HWND hwnd) const {
    for (auto const& w : m_wins)
        if (w.hwnd == hwnd) return w.app.key;
    return {};
}

void WindowTracker::OnEvent(DWORD event, HWND hwnd) {
    if (event == EVENT_OBJECT_DESTROY) return Remove(hwnd);
    if (GetAncestor(hwnd, GA_PARENT) != GetDesktopWindow()) return;  // controls and other children
    Update(hwnd);
    if (event == EVENT_SYSTEM_FOREGROUND) {
        auto it = std::find_if(m_wins.begin(), m_wins.end(), [&](Win const& w) { return w.hwnd == hwnd; });
        if (it != m_wins.end()) {
            std::rotate(m_wins.begin(), it, it + 1);
            if (onForeground) onForeground(m_wins.front().app);
        }
    }
}

void WindowTracker::Update(HWND hwnd) {
    auto it = std::find_if(m_wins.begin(), m_wins.end(), [&](Win const& w) { return w.hwnd == hwnd; });
    bool is = IsAppWindow(hwnd);
    if (it != m_wins.end()) {
        if (!is) Remove(hwnd);  // a tracked window keeps its identity
        return;
    }
    Win w{hwnd, {}};
    if (is && Identify(hwnd, w.app)) {
        m_wins.push_back(w);
        Changed();
    }
}

void WindowTracker::Remove(HWND hwnd) {
    auto it = std::find_if(m_wins.begin(), m_wins.end(), [&](Win const& w) { return w.hwnd == hwnd; });
    if (it == m_wins.end()) return;
    m_wins.erase(it);
    Changed();
}

// Recomputes the app list, keeping the apps' first-appearance order; tells
// the owner only if the set of apps changed.
void WindowTracker::Changed() {
    std::vector<RunningApp> apps;
    for (auto const& a : m_apps)
        if (std::any_of(m_wins.begin(), m_wins.end(), [&](Win const& w) { return w.app.key == a.key; }))
            apps.push_back(a);
    for (auto it = m_wins.rbegin(); it != m_wins.rend(); ++it)
        if (std::none_of(apps.begin(), apps.end(), [&](RunningApp const& a) { return a.key == it->app.key; }))
            apps.push_back(it->app);
    bool same = apps.size() == m_apps.size() &&
                std::equal(apps.begin(), apps.end(), m_apps.begin(),
                           [](RunningApp const& a, RunningApp const& b) { return a.key == b.key; });
    m_apps = std::move(apps);
    if (!same && !m_seeding) PostMessageW(m_notify, m_msg, 0, 0);
}
