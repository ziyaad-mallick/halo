// taskbar_test.exe - runs the real src/taskbar.cpp hide / re-show / restore logic against FAKE windows.
// It never touches the real taskbar: taskbar::SetTestTarget() aims HideAll/RestoreAll at windows of the
// class "HaloFakeTaskbarWnd", a private state file and a fake appbar state (no SHAppBarMessage SETSTATE).
// The fake windows sit at (-30000,-30000), far off every monitor, and take no focus.
//
// Checks:
//   A  hook OFF: a thread keeps calling ShowWindow(SW_SHOW) ("Explorer re-shows it"). The window may be
//      "visible", but its region is always empty, so nothing of it is ever drawn or hit.
//   B  hook ON (the guardian's logic: WinEvent EVENT_OBJECT_SHOW -> ReconcealIfShown): same, and the window
//      ends up hidden again within a short time.
//   C  HideAll is idempotent (the user's original appbar state is not overwritten by our own auto-hide).
//   D  RestoreAll: placement, rect, styles, region, layered alpha, visibility and appbar state equal the originals.
//   E  restore after the in-memory and ini state are lost (crash): the empty region is still found and removed.
//   F  Explorer also clears the region while re-showing: the hook conceals it again.
#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <cstdio>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

#include "../src/taskbar.h"

namespace {

constexpr wchar_t kClass[] = L"HaloFakeTaskbarWnd";
int g_fail = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_fail;
        std::printf("FAIL  %s\n", what);
    } else {
        std::printf("ok    %s\n", what);
    }
}

// ---- the fake "Explorer": its own thread owns the windows --------------------------------------
HWND g_w1 = nullptr, g_w2 = nullptr;
DWORD g_winThread = 0;

DWORD WINAPI WindowThread(LPVOID ready) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassW(&wc);
    const DWORD base = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    const DWORD sty = WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
    g_w1 = CreateWindowExW(base, kClass, L"fake1", sty, -30000, -30000, 400, 48, nullptr, nullptr, wc.hInstance, nullptr);
    // w2 has things a restore must give back exactly: a layered alpha and a region of its own.
    g_w2 = CreateWindowExW(base | WS_EX_LAYERED, kClass, L"fake2", sty, -30000, -29900, 400, 48, nullptr, nullptr, wc.hInstance, nullptr);
    SetLayeredWindowAttributes(g_w2, 0, 200, LWA_ALPHA);
    SetWindowRgn(g_w2, CreateRectRgn(0, 0, 300, 30), FALSE);
    ShowWindow(g_w1, SW_SHOWNA);
    ShowWindow(g_w2, SW_SHOWNA);
    SetEvent(static_cast<HANDLE>(ready));
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    DestroyWindow(g_w1);
    DestroyWindow(g_w2);
    return 0;
}

// ---- snapshot of everything a restore must give back ---------------------------------------------
struct Snap {
    WINDOWPLACEMENT wp{};
    RECT rc{};
    LONG style = 0, ex = 0;
    int rgnType = 0;
    HRGN rgn = nullptr;
    BYTE alpha = 255;
    DWORD flags = 0;
    bool visible = false;
};

Snap Take(HWND h) {
    Snap s;
    s.wp.length = sizeof(s.wp);
    GetWindowPlacement(h, &s.wp);
    GetWindowRect(h, &s.rc);
    s.style = GetWindowLongW(h, GWL_STYLE);
    s.ex = GetWindowLongW(h, GWL_EXSTYLE);
    s.rgn = CreateRectRgn(0, 0, 0, 0);
    s.rgnType = GetWindowRgn(h, s.rgn);
    if (s.ex & WS_EX_LAYERED) GetLayeredWindowAttributes(h, nullptr, &s.alpha, &s.flags);
    s.visible = IsWindowVisible(h) != FALSE;
    return s;
}

bool SameRect(const RECT& a, const RECT& b) { return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom; }

bool Same(const Snap& a, const Snap& b, std::string* why) {
    auto no = [&](const char* w) { *why = w; return false; };
    if (a.wp.showCmd != b.wp.showCmd || a.wp.flags != b.wp.flags || !SameRect(a.wp.rcNormalPosition, b.wp.rcNormalPosition)) return no("placement");
    if (a.wp.ptMinPosition.x != b.wp.ptMinPosition.x || a.wp.ptMinPosition.y != b.wp.ptMinPosition.y) return no("min position");
    if (!SameRect(a.rc, b.rc)) return no("window rect");
    if (a.style != b.style) return no("style");
    if (a.ex != b.ex) return no("exstyle");
    if (a.rgnType != b.rgnType) return no("region type");
    if (a.rgnType > NULLREGION && !EqualRgn(a.rgn, b.rgn)) return no("region shape");
    if (a.alpha != b.alpha || a.flags != b.flags) return no("layered alpha");
    if (a.visible != b.visible) return no("visibility");
    return true;
}

void CheckSame(const char* name, const Snap& orig, HWND h) {
    std::string why;
    Snap now = Take(h);
    const bool ok = Same(orig, now, &why);
    Check(ok, ok ? name : (std::string(name) + " (differs: " + why + ")").c_str());
}

// Nothing of the window can be seen or clicked: hidden, or an empty region.
bool Undrawable(HWND h) {
    HRGN r = CreateRectRgn(0, 0, 0, 0);
    const int t = GetWindowRgn(h, r);
    DeleteObject(r);
    return !IsWindowVisible(h) || t == NULLREGION;
}

// ---- the guardian's re-hide, as in src/guardian.cpp: WinEvent SHOW -> ReconcealIfShown -----------
std::atomic<int> g_reconcealed{0}, g_regionGoneAtShow{0};
std::atomic<bool> g_hookOn{true};  // the guardian's hidden_ gate: off while it restores, or it would hide the window again

void CALLBACK OnShow(HWINEVENTHOOK, DWORD, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    if (!g_hookOn || obj != OBJID_WINDOW || child != CHILDID_SELF || !h) return;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 63);
    if (wcscmp(cls, kClass) != 0) return;
    RECT bx{};
    if (GetWindowRgnBox(h, &bx) != NULLREGION) ++g_regionGoneAtShow;  // diagnostic: did the show drop our region?
    if (halo::taskbar::ReconcealIfShown(h)) ++g_reconcealed;
}

void Pump(DWORD ms) {
    const DWORD end = GetTickCount() + ms;
    for (;;) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        LONG left = static_cast<LONG>(end - GetTickCount());
        if (left <= 0) return;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(left < 5 ? left : 5), QS_ALLINPUT);
    }
}

bool WaitConcealed(const std::vector<HWND>& ws, DWORD ms) {
    const DWORD end = GetTickCount() + ms;
    while (static_cast<LONG>(end - GetTickCount()) > 0) {
        bool all = true;
        for (HWND w : ws) all = all && halo::taskbar::IsConcealed(w);
        if (all) return true;
        Pump(5);
    }
    return false;
}

// "Explorer" re-showing the windows from another thread, optionally also clearing their region.
// A sampler thread watches for a window that is visible with a non-empty region.
struct Storm {
    std::atomic<bool> run{true};
    std::atomic<long> samples{0}, visibleSamples{0}, drawableSamples{0};
    std::atomic<int> badType{-1}, badWin{-1};
    std::atomic<int> diagPrinted{0};
    std::atomic<long> noRegionObject{0}, readRetries{0}, unreadable{0};
    std::atomic<long> bad0{0}, bad1{0};  // drawable samples per window (index in the list given to Start)
    std::thread hammer, sampler;

    void Start(const std::vector<HWND>& ws, bool clearRegion, bool toggle = false) {
        hammer = std::thread([this, ws, clearRegion, toggle] {
            unsigned i = 0;
            while (run) {
                for (HWND w : ws) {
                    if (clearRegion && (i % 3) == 0) SetWindowRgn(w, nullptr, FALSE);
                    ShowWindow(w, toggle && (i % 2) ? SW_HIDE : SW_SHOW);
                }
                ++i;
                Sleep(2 + (i * 7) % 15);
            }
        });
        sampler = std::thread([this, ws] {
            while (run) {
                for (HWND w : ws) {
                    HRGN r = CreateRectRgn(0, 0, 0, 0);
                    if (!r) {  // no region object to read into: this sample measured nothing
                        ++noRegionObject;
                        continue;
                    }
                    // GetWindowRgn sometimes fails outright (ERROR with a last-error code, ERROR_INVALID_HANDLE) while
                    // another thread shows the window; that is a failed read, not "no region" (which sets no error).
                    // Read again, up to 3 times, and judge the read that succeeded.
                    int t = ERROR;
                    DWORD le0 = 0;
                    for (int attempt = 0; attempt < 3; ++attempt) {
                        SetLastError(0);
                        t = GetWindowRgn(w, r);
                        le0 = GetLastError();
                        if (!(t == ERROR && le0 != 0)) break;
                        ++readRetries;
                    }
                    DeleteObject(r);
                    if (t == ERROR && le0 != 0) {  // still failing after 3 reads: inconclusive, reported, never a pass or a violation
                        ++unreadable;
                        continue;
                    }
                    const bool vis = IsWindowVisible(w) != FALSE;
                    ++samples;
                    if (vis) ++visibleSamples;
                    if (vis && t != NULLREGION) {
                        // classify: read it again, with GetWindowRgn and with GetWindowRgnBox
                        const DWORD le = le0;
                        RECT bx{};
                        const int box = GetWindowRgnBox(w, &bx);
                        HRGN r2 = CreateRectRgn(0, 0, 0, 0);
                        const int again = GetWindowRgn(w, r2);
                        DeleteObject(r2);
                        const bool vis2 = IsWindowVisible(w) != FALSE;
                        if (diagPrinted++ < 6)
                            std::printf("      diag: t=%d lastErr=%lu box=%d again=%d visNow=%d\n", t, le, box, again, vis2);
                        ++drawableSamples;
                        badType = t;
                        badWin = (w == ws[0]) ? 0 : 1;
                        if (w == ws[0]) ++bad0; else ++bad1;
                    }
                }
                Sleep(0);
            }
        });
    }
    void Stop() {
        run = false;
        hammer.join();
        sampler.join();
    }
};

}  // namespace

int wmain() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // a crash must not swallow the lines printed so far
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring ini = std::wstring(tmp) + L"halo-taskbar-test-" + std::to_wstring(GetCurrentProcessId()) + L".ini";
    DeleteFileW(ini.c_str());

    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE th = CreateThread(nullptr, 0, WindowThread, ready, 0, &g_winThread);
    if (!th || WaitForSingleObject(ready, 5000) != WAIT_OBJECT_0 || !g_w1 || !g_w2) {
        std::printf("FAIL  could not create the fake windows\n");
        return 2;
    }
    const std::vector<HWND> both{g_w1, g_w2};

    halo::taskbar::TestTarget target;
    target.find = [&] { return both; };
    target.stateFile = ini;
    target.appBarState = ABS_ALWAYSONTOP;  // the "user's" original
    halo::taskbar::SetTestTarget(&target);

    const Snap o1 = Take(g_w1), o2 = Take(g_w2);
    Check(o1.visible && o2.visible, "fake windows start visible");
    Check(o1.rgnType == 0 && o2.rgnType == SIMPLEREGION, "originals: w1 has no region, w2 has its own");
    Check(o2.alpha == 200, "original: w2 is layered at alpha 200");

    // ---- hide
    Check(halo::taskbar::HideAll(), "HideAll applies the empty region to every window");
    Check(target.appBarState == ABS_AUTOHIDE, "appbar state set to auto-hide");
    Check(halo::taskbar::IsConcealed(g_w1) && halo::taskbar::IsConcealed(g_w2), "both windows hidden with an empty region");
    Check(GetPrivateProfileIntW(L"taskbar", L"state", 99, ini.c_str()) == ABS_ALWAYSONTOP &&
              GetPrivateProfileIntW(L"taskbar", L"hidden", 0, ini.c_str()) == 1,
          "state file holds the original appbar state and hidden=1");

    // ---- A: hook off, Explorer keeps re-showing
    {
        Storm s;
        s.Start(both, false);
        Pump(1200);
        s.Stop();
        std::printf("      A: %ld samples, %ld with the window marked visible, %ld skipped (CreateRectRgn failed), %ld GetWindowRgn read failures retried, %ld still unreadable after 3 reads (inconclusive)\n", s.samples.load(), s.visibleSamples.load(), s.noRegionObject.load(), s.readRetries.load(), s.unreadable.load());
        Check(s.visibleSamples > 0, "A: the storm really made the windows visible (test is not vacuous)");
        if (s.drawableSamples) std::printf("      A: %ld drawable samples (w1 %ld, w2 %ld), last region type %d\n", s.drawableSamples.load(), s.bad0.load(), s.bad1.load(), s.badType.load());
        Check(s.drawableSamples == 0, "A: never visible with a non-empty region, with no hook running");
    }
    for (HWND w : both) ShowWindow(w, SW_HIDE);

    // ---- B: hook on
    HWINEVENTHOOK hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnShow, GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
    Check(hook != nullptr, "B: show hook installed");
    {
        Storm s;
        s.Start(both, false);
        Pump(1200);
        s.Stop();
        const bool back = WaitConcealed(both, 1000);
        std::printf("      B: %ld samples, %ld visible, %d re-concealed by the hook, %ld read failures retried, %ld unreadable (inconclusive)\n", s.samples.load(), s.visibleSamples.load(), g_reconcealed.load(), s.readRetries.load(), s.unreadable.load());
        std::printf("      B: single unretried GetWindowRgnBox read said not-empty at show time: %d of %d (read noise, see taskbar.cpp RegionState)\n", g_regionGoneAtShow.load(), g_reconcealed.load());
        if (s.drawableSamples) std::printf("      B: %ld drawable samples (w1 %ld, w2 %ld), last region type %d\n", s.drawableSamples.load(), s.bad0.load(), s.bad1.load(), s.badType.load());
        Check(s.drawableSamples == 0, "B: never visible with a non-empty region, hook running");
        Check(g_reconcealed > 0, "B: the hook acted on the re-shows");
        Check(back, "B: both windows hidden again within 1 s of the last re-show");
    }

    // ---- C: HideAll twice keeps the user's original state
    Check(halo::taskbar::HideAll(), "C: second HideAll");
    Check(GetPrivateProfileIntW(L"taskbar", L"state", 99, ini.c_str()) == ABS_ALWAYSONTOP, "C: original appbar state not overwritten by our own auto-hide");

    // ---- D: restore (the guardian clears hidden_ first, so its hooks stand down)
    g_hookOn = false;
    halo::taskbar::RestoreAll();
    Pump(100);
    Check(target.appBarState == ABS_ALWAYSONTOP, "D: appbar state back to the original");
    Check(GetPrivateProfileIntW(L"taskbar", L"hidden", 1, ini.c_str()) == 0, "D: state file says not hidden");
    CheckSame("D: w1 equals its original", o1, g_w1);
    CheckSame("D: w2 (own region, alpha 200) equals its original", o2, g_w2);

    // ---- E: crash. In-memory state and the ini are gone; the empty region alone must be enough to undo.
    target.find = [&] { return std::vector<HWND>{g_w1}; };
    Check(halo::taskbar::HideAll(), "E: hide w1 again");
    DeleteFileW(ini.c_str());
    halo::taskbar::RestoreAll();
    Pump(100);
    g_hookOn = true;
    CheckSame("E: w1 restored with no state file and no memory", o1, g_w1);

    // ---- F: Explorer also removes the region while re-showing
    target.appBarState = ABS_ALWAYSONTOP;
    Check(halo::taskbar::HideAll(), "F: hide w1");
    {
        Storm s;
        s.Start({g_w1}, true);
        Pump(1200);
        s.Stop();
        Pump(500);  // let the hook drain its queue
        std::printf("      F: %ld samples, %ld drawable (region cleared by 'Explorer' and not yet re-emptied: the residual gap)\n", s.samples.load(), s.drawableSamples.load());
        Check(Undrawable(g_w1), "F: w1 not drawable (hidden or empty region) after Explorer cleared its region and re-showed it");
    }
    UnhookWinEvent(hook);
    halo::taskbar::RestoreAll();
    Pump(100);
    CheckSame("F: w1 equals its original after the final restore", o1, g_w1);
    Check(target.appBarState == ABS_ALWAYSONTOP, "F: appbar state back to the original");

    // ---- G: control, no Halo code at all. A window given an empty region by hand and shown and hidden in a loop:
    // if GetWindowRgn reports "no region" for it now and then, that is how Windows behaves, not our code.
    {
        SetWindowRgn(g_w1, CreateRectRgn(0, 0, 0, 0), FALSE);
        Storm s;
        s.Start({g_w1}, false, true);
        Pump(1500);
        s.Stop();
        std::printf("      G (control, plain SetWindowRgn + show/hide loop): %ld samples, %ld visible, %ld conclusive 'visible with no or non-empty region', %ld unreadable\n",
                    s.samples.load(), s.visibleSamples.load(), s.drawableSamples.load(), s.unreadable.load());
        SetWindowRgn(g_w1, nullptr, FALSE);
        ShowWindow(g_w1, SW_SHOWNA);
    }

    halo::taskbar::SetTestTarget(nullptr);
    PostThreadMessageW(g_winThread, WM_QUIT, 0, 0);
    WaitForSingleObject(th, 3000);
    DeleteFileW(ini.c_str());
    std::printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
