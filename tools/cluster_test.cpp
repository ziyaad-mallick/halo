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

    // Test 1: Instantiation and metrics at 1.0x scale
    float pillHeight = 50.0f;
    float scale = 1.0f;
    DockCluster cluster(comp, gdev, nullptr, pillHeight, scale);

    printf("Cluster Width: %.1f\n", cluster.Width());
    printf("Start X: %.1f, Width: %.1f\n", cluster.StartX(), cluster.StartWidth());
    assert(cluster.Width() == 192.0f);
    assert(cluster.StartWidth() == 28.0f);
    assert(cluster.StartX() == 192.0f - 28.0f);
    assert(cluster.Visual() != nullptr);
    printf("PASS: Cluster metrics and visual creation\n");

    // Test 2: Hit testing Start button
    assert(cluster.HitTestStart(164.0f, 25.0f) == true);
    assert(cluster.HitTestStart(180.0f, 10.0f) == true);
    assert(cluster.HitTestStart(192.0f, 40.0f) == true);
    assert(cluster.HitTestStart(163.0f, 25.0f) == false);  // Outside Start button (left)
    assert(cluster.HitTestStart(193.0f, 25.0f) == false);  // Outside Start button (right)
    assert(cluster.HitTestStart(170.0f, -5.0f) == false);  // Outside pill vertically (top)
    assert(cluster.HitTestStart(170.0f, 55.0f) == false);  // Outside pill vertically (bottom)
    printf("PASS: Start button hit testing\n");

    // Test 3: Rendering execution into surface
    cluster.Render();
    printf("PASS: Direct2D/DirectWrite rendering\n");

    // Test 4: Event handlers without crash
    cluster.OnTimeChange();
    cluster.OnSettingChange();
    cluster.OnPowerBroadcast(PBT_APMPOWERSTATUSCHANGE, 0);
    cluster.OnNetworkChanged();
    printf("PASS: Event handling\n");

    // Test 5: Metrics at 1.5x scale (DPI scaling)
    float scale15 = 1.5f;
    DockCluster cluster15(comp, gdev, nullptr, pillHeight * scale15, scale15);
    printf("1.5x Cluster Width: %.1f, StartX: %.1f\n", cluster15.Width(), cluster15.StartX());
    assert(cluster15.Width() == std::round(192.0f * 1.5f));
    assert(cluster15.StartWidth() == std::round(28.0f * 1.5f));
    cluster15.Render();
    printf("PASS: High-DPI scaling\n");

    printf("\nAll DockCluster unit tests PASSED!\n");
    return 0;
}
