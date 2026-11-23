#include <windows.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <dxgi.h>
#include <dispatcherqueue.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <stdio.h>
#include <cassert>
#include <cmath>

extern "C" HRESULT WINAPI D2D1CreateDevice(IDXGIDevice*, D2D1_CREATION_PROPERTIES const*, ID2D1Device**);

#include "../src/cluster.h"

int wmain() {
    printf("=== DockCluster Test Suite ===\n");
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    DispatcherQueueOptions opt{sizeof(opt), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
    winrt::Windows::System::DispatcherQueueController dq{nullptr};
    HRESULT hr = CreateDispatcherQueueController(opt, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(dq)));
    assert(SUCCEEDED(hr));

    auto comp = winrt::Windows::UI::Composition::Compositor();
    winrt::com_ptr<ID3D11Device> d3d;
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                           nullptr, 0, D3D11_SDK_VERSION, d3d.put(), nullptr, nullptr);
    assert(SUCCEEDED(hr));
    auto dxgi = d3d.as<IDXGIDevice>();
    winrt::com_ptr<ID2D1Device> d2d;
    hr = D2D1CreateDevice(dxgi.get(), nullptr, d2d.put());
    assert(SUCCEEDED(hr));

    winrt::Windows::UI::Composition::CompositionGraphicsDevice gdev{nullptr};
    auto interop = comp.as<ABI::Windows::UI::Composition::ICompositorInterop>();
    hr = interop->CreateGraphicsDevice(reinterpret_cast<IUnknown*>(d2d.get()),
        reinterpret_cast<ABI::Windows::UI::Composition::ICompositionGraphicsDevice**>(winrt::put_abi(gdev)));
    assert(SUCCEEDED(hr));

    // The cluster's width comes from font metrics (time, Wi-Fi, battery %) and the halo2 setting, so the tests check
    // the layout contract (Start is the last slot, the width ends at it, hit tests follow it, it scales with DPI),
    // not a pixel count that changes with the font or a redesign.
    // Test 1: Instantiation and metrics at 1.0x scale
    float pillHeight = 50.0f;
    float scale = 1.0f;
    DockCluster cluster(comp, gdev, nullptr, pillHeight, scale);

    const float w = cluster.Width(), sx = cluster.StartX(), sw = cluster.StartWidth();
    printf("Cluster Width: %.1f\n", w);
    printf("Start X: %.1f, Width: %.1f\n", sx, sw);
    assert(sw == 28.0f || sw == 36.0f);   // one rest-size icon slot (classic / halo2)
    assert(sx > 0.0f && sx + sw <= w);    // Start sits inside the cluster...
    assert(w - (sx + sw) < 1.0f);         // ...as its last slot (the width is ceil'd)
    assert(cluster.Visual() != nullptr);
    printf("PASS: Cluster metrics and visual creation\n");

    // Test 2: Hit testing Start button (edges inclusive)
    const float mid = sx + sw / 2;
    assert(cluster.HitTestStart(sx, 25.0f) == true);
    assert(cluster.HitTestStart(mid, 10.0f) == true);
    assert(cluster.HitTestStart(sx + sw, 40.0f) == true);
    assert(cluster.HitTestStart(sx - 1.0f, 25.0f) == false);       // left of Start
    assert(cluster.HitTestStart(sx + sw + 1.0f, 25.0f) == false);  // right of Start
    assert(cluster.HitTestStart(mid, -5.0f) == false);             // above the pill
    assert(cluster.HitTestStart(mid, 55.0f) == false);             // below the pill
    // The whole cluster is a target too (a click anywhere on it opens Start).
    assert(cluster.HitTestCluster(1.0f, 25.0f) == true);
    assert(cluster.HitTestCluster(w + 1.0f, 25.0f) == false);
    printf("PASS: Start button and cluster hit testing\n");

    // Test 3: Rendering execution into surface
    cluster.Render();
    printf("PASS: Direct2D/DirectWrite rendering\n");

    // Test 4: Event handlers without crash
    cluster.OnTimeChange();
    cluster.OnSettingChange();
    cluster.OnPowerBroadcast(PBT_APMPOWERSTATUSCHANGE, 0);
    cluster.OnNetworkChanged();
    printf("PASS: Event handling\n");

    // Test 5: Metrics at 1.5x scale (DPI scaling). Text widths are ceil'd per slot, so allow a few px of rounding.
    float scale15 = 1.5f;
    DockCluster cluster15(comp, gdev, nullptr, pillHeight * scale15, scale15);
    printf("1.5x Cluster Width: %.1f, StartX: %.1f\n", cluster15.Width(), cluster15.StartX());
    assert(cluster15.StartWidth() == std::round(sw * 1.5f));
    assert(std::abs(cluster15.Width() - w * 1.5f) <= 4.0f);
    cluster15.Render();
    printf("PASS: High-DPI scaling\n");

    printf("\nAll DockCluster unit tests PASSED!\n");
    return 0;
}
