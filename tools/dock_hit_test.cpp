// Headless test of the dock's magnetic hit-testing (Dock::HitTarget). Not part of build.sh; build ad hoc:
//   clang++ <COMMON flags> src/dock.cpp src/cluster.cpp src/settings.cpp tools/dock_hit_test.cpp -o build/dock_hit_test.exe <WINLIBS>
// Run with no argument for HALO 2.0 off, with `h2` for HALO 2.0 on (it writes a temp settings file).
#include <windows.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <dxgi.h>
#include <dispatcherqueue.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

extern "C" HRESULT WINAPI D2D1CreateDevice(IDXGIDevice*, D2D1_CREATION_PROPERTIES const*, ID2D1Device**);

#include "../src/cluster.h"
#include "../src/dock.h"
#include "../src/settings.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

int wmain(int argc, wchar_t** argv) {
    const bool h2 = argc > 1 && !wcscmp(argv[1], L"h2");
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring ini = std::wstring(tmp) + L"halo_dock_hit_test.ini";
    if (FILE* f = _wfopen(ini.c_str(), L"wb")) {
        fprintf(f, "[general]\r\nhalo2 = %s\r\n", h2 ? "on" : "off");
        fclose(f);
    }
    SetEnvironmentVariableW(L"HALO_SETTINGS_FILE", ini.c_str());
    halo::settings::Reload();
    CHECK(halo::settings::Get().halo2 == h2);
    printf("=== Dock hit-testing, HALO 2.0 %s ===\n", h2 ? "ON" : "off");

    // Magnetic snapping on one axis: spans touch or leave a gap, and the ends reach `tol` past their edge.
    {
        std::vector<halo::hit::Span> spans = {{10.0f, 30.0f}, {40.0f, 60.0f}, {60.0f, 80.0f}};
        CHECK(halo::hit::Nearest(spans, 20.0f, 5.0f) == 0);
        CHECK(halo::hit::Nearest(spans, 31.0f, 5.0f) == 0);  // in the gap, nearer the first
        CHECK(halo::hit::Nearest(spans, 39.0f, 5.0f) == 1);  // ... nearer the second
        CHECK(halo::hit::Nearest(spans, 61.0f, 5.0f) == 2);
        CHECK(halo::hit::Nearest(spans, 6.0f, 5.0f) == 0);   // past the left end, within tol
        CHECK(halo::hit::Nearest(spans, 4.0f, 5.0f) == -1);  // beyond tol
        CHECK(halo::hit::Nearest(spans, 84.0f, 5.0f) == 2);
        CHECK(halo::hit::Nearest(spans, 86.0f, 5.0f) == -1);
        CHECK(halo::hit::Nearest({}, 10.0f, 5.0f) == -1);
        CHECK(halo::hit::BelowTop(55.0f, 60.0f, 14.0f) && !halo::hit::BelowTop(45.0f, 60.0f, 14.0f));
    }

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    DispatcherQueueOptions opt{sizeof(opt), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
    winrt::Windows::System::DispatcherQueueController dq{nullptr};
    CreateDispatcherQueueController(opt, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(dq)));
    auto comp = winrt::Windows::UI::Composition::Compositor();
    winrt::com_ptr<ID3D11Device> d3d;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, d3d.put(), nullptr, nullptr);
    auto dxgi = d3d.as<IDXGIDevice>();
    winrt::com_ptr<ID2D1Device> d2d;
    D2D1CreateDevice(dxgi.get(), nullptr, d2d.put());
    winrt::Windows::UI::Composition::CompositionGraphicsDevice gdev{nullptr};
    auto interop = comp.as<ABI::Windows::UI::Composition::ICompositorInterop>();
    interop->CreateGraphicsDevice(reinterpret_cast<IUnknown*>(d2d.get()),
        reinterpret_cast<ABI::Windows::UI::Composition::ICompositionGraphicsDevice**>(winrt::put_abi(gdev)));

    DockMetrics m;  // 28 px icons, 10 px gap, 10 px padding, 50 px pill
    m.bottom = 100.0f;
    m.centerX = 600.0f;
    auto root = comp.CreateContainerVisual();
    auto pill = comp.CreateContainerVisual();
    DockCluster cluster(comp, gdev, nullptr, m.pillHeight, 1.0f);
    DockWidgets widgets(comp, gdev, m.pillHeight, 1.0f);
    Dock dock(comp, root, pill, m, &cluster, h2 ? &widgets : nullptr);
    std::vector<Dock::Item> items;
    for (int i = 0; i < 5; i++)
        items.push_back({L"app" + std::to_wstring(i), comp.CreateColorBrush(), false, i >= 4});
    dock.SetItems(items);

    // Pill geometry at rest: cluster 192 + gap 10, 5 slots of 38 + a 10 divider (one unpinned), the measured widget width + 10.
    const float cW = cluster.Width();  // font-dependent (192 with the stock Segoe metrics)
    const float right = h2 ? widgets.Width() + 10.0f : 0.0f;
    const float pillW = (cW + 10.0f) + 5 * 38.0f - 10.0f + 10.0f + right + 20.0f;
    const float pillL = m.centerX - pillW / 2, pillR = pillL + pillW;
    const float midY = m.bottom - m.pillHeight / 2;
    printf("pill x %.1f..%.1f\n", pillL, pillR);

    // Every icon: its centre hits it, and so do the bottom of the pill and the points between neighbours.
    for (int i = 0; i < 5; i++) {
        auto [cx, cy] = dock.RestIconCenter(i);
        auto t = dock.HitTarget(cx, midY);
        CHECK(t.kind == Dock::Kind::App && t.index == i);
        CHECK(dock.HitTest(cx, cy) == i);
        CHECK(dock.HitTest(cx, midY + 22.0f) == i);
    }
    // Between two neighbours there is never a miss, and the point goes to the nearer one.
    for (int i = 0; i + 1 < 5; i++) {
        float a = dock.RestIconCenter(i).first, b = dock.RestIconCenter(i + 1).first;
        for (float x = a; x <= b; x += 1.0f) {
            int hit = dock.HitTest(x, midY);
            CHECK(hit == i || hit == i + 1);
        }
        CHECK(dock.HitTest(a + 6.0f, midY) == i);
        CHECK(dock.HitTest(b - 6.0f, midY) == i + 1);
    }
    // Vertical tolerance: a little above the pill still counts, far above does not.
    float x2 = dock.RestIconCenter(2).first;
    CHECK(dock.HitTest(x2, m.bottom - m.pillHeight - 10.0f) == 2);
    CHECK(dock.HitTest(x2, m.bottom - m.pillHeight - 40.0f) == -1);
    // Beside the last icon, and far outside the pill.
    float last = dock.RestIconCenter(4).first;
    CHECK(dock.HitTest(last + 19.0f + 2.0f, midY) == 4 || h2);
    CHECK(dock.HitTest(pillL - 40.0f, midY) == -1 && dock.HitTest(pillR + 40.0f, midY) == -1);
    CHECK(dock.HitTarget(pillL + 3.0f, midY).kind == (h2 ? Dock::Kind::Start : Dock::Kind::None));  // the pill edge: the cluster is Start only with HALO 2.0

    const float cl = pillL + 10.0f;
    if (!h2) {
        // Only the Start logo (the last 28 px of the cluster) is a target.
        CHECK(dock.HitStart(cl + cW - 14.0f, midY));
        CHECK(dock.HitStart(cl + cW - 30.0f, midY));  // just left of the logo: snapped
        CHECK(!dock.HitStart(cl + 20.0f, midY));          // the time: not clickable in 1.x
        CHECK(dock.HitTarget(cl + cW + 2.0f, midY).kind == Dock::Kind::Start);  // the gap before the first icon
        CHECK(dock.HitTarget(pillR - 3.0f, midY).kind == Dock::Kind::App);
    } else {
        // The whole cluster is Start: time, battery, logo and the gaps between.
        for (float x = cl; x <= cl + cW; x += 4.0f) CHECK(dock.HitStart(x, midY));
        CHECK(dock.HitStart(cl + 5.0f, midY - 20.0f));
        // The widgets on the right: Wi-Fi, Bluetooth, media; every point across them hits one, none is dead.
        float wl = pillR - 10.0f - widgets.Width();
        for (float x = wl; x < wl + widgets.Width(); x += 2.0f) {
            auto t = dock.HitTarget(x, midY);
            CHECK(t.kind == (x <= wl + 28.0f ? Dock::Kind::Wifi : x <= wl + 56.0f ? Dock::Kind::Bluetooth : Dock::Kind::Media));
        }
        CHECK(dock.HitTarget(pillR - 2.0f, midY).kind == Dock::Kind::Media);  // the pill's right padding
        CHECK(dock.HitTarget(wl - 4.0f, midY).kind != Dock::Kind::None);       // the divider before them
        CHECK(!dock.HitStart(wl + 10.0f, midY));
    }

    // Magnification moves slots under the pointer: hit-testing follows what is drawn.
    dock.OnPointerMove(dock.RestIconCenter(2).first);
    CHECK(dock.HitTest(dock.RestIconCenter(2).first, midY) == 2);
    dock.OnPointerLeave();
    dock.SetMagnet(2);  // must not throw
    dock.SetMagnet(-1);

    printf(g_fail ? "FAILED: %d\n" : "All dock hit-test checks PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
