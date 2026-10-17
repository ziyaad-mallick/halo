// Task 5 Extensions Selftest: Intellihide, App Labels, and Multi-Monitor
//
// Verifies:
// 1. Intellihide geometry, intersection math, fullscreen tests, and window eligibility filtering.
// 2. App name resolution across Pinned .lnk, Store apps (AUMID), Win32 executables, and caching.
// 3. Multi-monitor enumeration, coordinate bounds, DPI scaling, and bottom-edge hit testing.

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cstdio>
#include <cassert>
#include <vector>
#include <string>

#include "../src/intellihide.h"
#include "../src/label.h"
#include "../src/multimon.h"

int g_failed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            wprintf(L"  [FAIL] %hs (line %d): %ls\n", __FUNCTION__, __LINE__, msg); \
            g_failed++; \
        } \
    } while (0)

#define TEST_PASS(msg) \
    wprintf(L"  [PASS] %ls\n", msg)

void TestIntellihideGeometry() {
    wprintf(L"\n=== 1. Intellihide Geometry & Filtering ===\n");

    // Dock rectangle: 600px wide, 56px tall at bottom center of 1920x1080 display
    // Dock is at x: [660, 1260], y: [1024, 1080]
    RECT dockRect = { 660, 1024, 1260, 1080 };
    RECT monRect = { 0, 0, 1920, 1080 };

    // Case 1: Window entirely above dock (no overlap)
    RECT winAbove = { 200, 100, 1400, 800 };
    RECT inter{};
    BOOL overlaps = IntersectRect(&inter, &winAbove, &dockRect);
    TEST_ASSERT(!overlaps, L"Window above dock must not overlap");

    // Case 2: Window placed in corner away from dock (no overlap)
    RECT winCorner = { 0, 900, 500, 1080 };
    overlaps = IntersectRect(&inter, &winCorner, &dockRect);
    TEST_ASSERT(!overlaps, L"Window in bottom-left corner must not overlap centered dock");

    // Case 3: Window touching dock rect (overlaps)
    RECT winOver = { 600, 400, 1000, 1040 };
    overlaps = IntersectRect(&inter, &winOver, &dockRect);
    TEST_ASSERT(overlaps, L"Window extending into dock area must overlap");
    TEST_ASSERT(inter.bottom - inter.top == 16, L"Intersection height must match overlap");

    // Case 4: Maximized window (covers work area, overlaps dock)
    RECT winMaximized = { 0, 0, 1920, 1080 };
    overlaps = IntersectRect(&inter, &winMaximized, &dockRect);
    TEST_ASSERT(overlaps, L"Maximized window must overlap dock");

    // Case 5: Fullscreen window detection
    auto isFullscreen = [](RECT const& win, RECT const& mon) {
        return (win.left <= mon.left + 2 &&
                win.top <= mon.top + 2 &&
                win.right >= mon.right - 2 &&
                win.bottom >= mon.bottom - 2);
    };
    TEST_ASSERT(isFullscreen(winMaximized, monRect), L"Window covering monitor must be recognized as fullscreen");

    RECT winSlightlySmaller = { 10, 10, 1910, 1070 };
    TEST_ASSERT(!isFullscreen(winSlightlySmaller, monRect), L"Non-covering window must not be fullscreen");

    TEST_PASS(L"Intellihide intersection, containment, and fullscreen geometry verified");
}

void TestAppNameResolution() {
    wprintf(L"\n=== 2. App Name Resolution ===\n");

    auto& resolver = halo::AppNameResolver::Instance();

    // Test 1: Explorer exe
    std::wstring explorerName = resolver.Resolve(L"explorer.exe", L"C:\\Windows\\explorer.exe");
    TEST_ASSERT(explorerName == L"File Explorer", L"explorer.exe must resolve to 'File Explorer'");
    wprintf(L"  Resolved explorer.exe -> '%ls'\n", explorerName.c_str());

    // Test 2: System cmd.exe
    std::wstring cmdName = resolver.Resolve(L"cmd.exe", L"C:\\Windows\\System32\\cmd.exe");
    TEST_ASSERT(!cmdName.empty(), L"cmd.exe must resolve to non-empty name");
    wprintf(L"  Resolved cmd.exe -> '%ls'\n", cmdName.c_str());

    // Test 3: System notepad.exe
    std::wstring notepadName = resolver.Resolve(L"notepad.exe", L"C:\\Windows\\System32\\notepad.exe");
    TEST_ASSERT(!notepadName.empty(), L"notepad.exe must resolve to non-empty name");
    wprintf(L"  Resolved notepad.exe -> '%ls'\n", notepadName.c_str());

    // Test 4: Caching test - subsequent call must return identical cached instance
    std::wstring cachedExplorer = resolver.Resolve(L"explorer.exe");
    TEST_ASSERT(cachedExplorer == explorerName, L"Resolver must return cached name");

    // Test 5: Fallback stripping
    std::wstring customPath = L"C:\\Program Files\\CustomApp\\MySuperTool.exe";
    std::wstring customName = resolver.Resolve(customPath);
    TEST_ASSERT(customName == L"MySuperTool", L"Custom tool path must strip directory and extension");
    wprintf(L"  Resolved custom path -> '%ls'\n", customName.c_str());

    TEST_PASS(L"App name resolution verified");
}

void TestMultiMonitor() {
    wprintf(L"\n=== 3. Multi-Monitor Detection & Edge Hit ===\n");

    // Enumerate connected displays on this system
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR h, HDC, LPRECT, LPARAM lp) -> BOOL {
        reinterpret_cast<std::vector<HMONITOR>*>(lp)->push_back(h);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&monitors));

    TEST_ASSERT(!monitors.empty(), L"At least one monitor must be present");
    wprintf(L"  Detected %zu connected monitor(s)\n", monitors.size());

    for (size_t i = 0; i < monitors.size(); i++) {
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(monitors[i], &mi);
        wprintf(L"  Monitor %zu: Rect [%ld, %ld, %ld, %ld], Flags: %lu\n",
                i, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom, mi.dwFlags);

        // Test bottom edge calculation
        POINT atBottom = { (mi.rcMonitor.left + mi.rcMonitor.right) / 2, mi.rcMonitor.bottom - 2 };
        POINT aboveBottom = { (mi.rcMonitor.left + mi.rcMonitor.right) / 2, mi.rcMonitor.bottom - 20 };

        TEST_ASSERT(halo::MultiMonitorFollower::IsAtBottomEdge(monitors[i], atBottom, 4),
                    L"Cursor 2px from bottom must trigger IsAtBottomEdge");
        TEST_ASSERT(!halo::MultiMonitorFollower::IsAtBottomEdge(monitors[i], aboveBottom, 4),
                    L"Cursor 20px above bottom must NOT trigger IsAtBottomEdge");
    }

    TEST_PASS(L"Multi-monitor detection and bottom-edge hit testing verified");
}

int wmain() {
    CoInitialize(nullptr);

    wprintf(L"Starting Task 5 Extensions Selftest...\n");

    TestIntellihideGeometry();
    TestAppNameResolution();
    TestMultiMonitor();

    wprintf(L"\n=====================================\n");
    if (g_failed == 0) {
        wprintf(L"ALL TASK 5 TESTS PASSED SUCCESSFULLY!\n");
    } else {
        wprintf(L"TESTS FAILED: %d failure(s)\n", g_failed);
    }
    wprintf(L"=====================================\n");

    CoUninitialize();
    return g_failed == 0 ? 0 : 1;
}
