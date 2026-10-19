// Attributes Halo's private memory (the README's 17.5 MB claim vs the ~36 MB a
// long-lived instance reports) to its init phases, in complete isolation: no
// Halo window, no dock, nothing on screen, no contact with a running session.
//
// It builds Halo's exact graphics stack - dispatcher queue, Compositor, a
// D3D11 device, the D2D1 device, the CompositionGraphicsDevice - then makes the
// icon surfaces of src/halo.cpp's MakeIconBrush, printing the process's private
// bytes after every phase so each cost is attributable.
//
//   tools/memprobe.exe [warp|hw] [none|cubic|linear|downs]
//
//   warp    D3D11_DRIVER_TYPE_WARP, what Halo::InitGraphics uses
//   hw      D3D11_DRIVER_TYPE_HARDWARE, the alternative it documented
//   none    create the 27 icon surfaces, draw nothing
//   cubic   draw each one from the 256px source at 44px (Halo today)
//   linear  the same with D2D1_INTERPOLATION_MODE_LINEAR
//   downs   pre-shrink the source to 44px on the CPU, then draw it 1:1
#include <windows.h>
#include <psapi.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <dispatcherqueue.h>
#include <windows.ui.composition.interop.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.Graphics.DirectX.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace wuc = winrt::Windows::UI::Composition;

// Missing from the MinGW SDK headers; same layout and IID as src/halo.cpp.
struct ICompositionDrawingSurfaceInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE BeginDraw(RECT const* updateRect, REFIID iid, void** updateObject, POINT* offset) = 0;
    virtual HRESULT STDMETHODCALLTYPE EndDraw() = 0;
    virtual HRESULT STDMETHODCALLTYPE Resize(SIZE size) = 0;
    virtual HRESULT STDMETHODCALLTYPE Scroll(RECT const* scrollRect, RECT const* clipRect, int dx, int dy) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResumeDraw() = 0;
    virtual HRESULT STDMETHODCALLTYPE SuspendDraw() = 0;
};
__CRT_UUID_DECL(ICompositionDrawingSurfaceInterop, 0xFD04E6E3, 0xFE0C, 0x4C3C, 0xAB, 0x19, 0xA0, 0x76, 0x01, 0xA5, 0x76, 0xEE)

extern "C" HRESULT WINAPI D2D1CreateDevice(IDXGIDevice*, D2D1_CREATION_PROPERTIES const*, ID2D1Device**);

// Commit charge: the same number as .NET's PrivateMemorySize64 and Task
// Manager's "Commit size" - what the README's 17.5 MB and the 36.5 MB report.
static double Priv() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    return pmc.PrivateUsage / 1048576.0;
}
static double Work() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    return pmc.WorkingSetSize / 1048576.0;
}
static double g_prev = 0;
static void Say(const char* phase) {
    const double now = Priv();
    printf("  %-44s %7.1f MB private  (ws %5.1f, step %+6.1f)\n", phase, now, Work(), g_prev ? now - g_prev : 0.0);
    g_prev = now;
    fflush(stdout);
}

enum class Mode { None, Cubic, Linear, Downs, Clear, Cap, Reuse, Reuse44, Reclaim };

int wmain(int argc, wchar_t** argv) {
    const bool hw = argc > 1 && _wcsicmp(argv[1], L"hw") == 0;
    Mode mode = Mode::Cubic;
    if (argc > 2) {
        if (!_wcsicmp(argv[2], L"none")) mode = Mode::None;
        else if (!_wcsicmp(argv[2], L"linear")) mode = Mode::Linear;
        else if (!_wcsicmp(argv[2], L"downs")) mode = Mode::Downs;
        else if (!_wcsicmp(argv[2], L"clear")) mode = Mode::Clear;
        else if (!_wcsicmp(argv[2], L"cap")) mode = Mode::Cap;
        else if (!_wcsicmp(argv[2], L"reuse")) mode = Mode::Reuse;
        else if (!_wcsicmp(argv[2], L"reuse44")) mode = Mode::Reuse44;
        else if (!_wcsicmp(argv[2], L"reclaim")) mode = Mode::Reclaim;
    }
    const char* modeName = mode == Mode::None     ? "surfaces only, no draw"
                           : mode == Mode::Downs   ? "44px source, 1:1 draw"
                           : mode == Mode::Linear  ? "256px source, LINEAR"
                           : mode == Mode::Clear   ? "256px source, CUBIC + ClearResources() per build"
                           : mode == Mode::Cap     ? "256px source, CUBIC + SetMaximumTextureMemory(2MB)"
                           : mode == Mode::Reuse   ? "256px source CUBIC, repaint the SAME surfaces"
                           : mode == Mode::Reclaim ? "256px source CUBIC, then release the whole stack"
                           : mode == Mode::Reuse44 ? "44px source, repaint the SAME surfaces"
                                                   : "256px source, CUBIC (Halo today)";
    printf("== %s | %s\n", hw ? "HARDWARE" : "WARP", modeName);
    Say("process start");

    winrt::init_apartment(winrt::apartment_type::single_threaded);

    winrt::Windows::System::DispatcherQueueController dqc{nullptr};
    DispatcherQueueOptions opt{sizeof(opt), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
    winrt::check_hresult(CreateDispatcherQueueController(
        opt, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(dqc))));
    Say("+ dispatcher queue");

    wuc::Compositor comp;
    Say("+ Compositor");

    winrt::com_ptr<ID3D11Device> d3d;
    winrt::check_hresult(D3D11CreateDevice(nullptr, hw ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr,
                                          D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                          d3d.put(), nullptr, nullptr));
    Say(hw ? "+ D3D11 device (hardware)" : "+ D3D11 device (WARP)");

    auto dxgi = d3d.as<IDXGIDevice>();
    winrt::com_ptr<ID2D1Device> d2d;
    winrt::check_hresult(D2D1CreateDevice(dxgi.get(), nullptr, d2d.put()));
    Say("+ D2D1 device");
    if (mode == Mode::Cap) d2d->SetMaximumTextureMemory(2ull << 20);

    auto cinterop = comp.as<ABI::Windows::UI::Composition::ICompositorInterop>();
    wuc::CompositionGraphicsDevice gdev{nullptr};
    winrt::check_hresult(cinterop->CreateGraphicsDevice(
        reinterpret_cast<IUnknown*>(d2d.get()),
        reinterpret_cast<ABI::Windows::UI::Composition::ICompositionGraphicsDevice**>(winrt::put_abi(gdev))));
    Say("+ CompositionGraphicsDevice");

    constexpr int kIcons = 27, kSide = 44, kSrc = 256;
    // The 256px source, and a box-filtered 44px version of it for the "downs" mode.
    std::vector<uint32_t> src256(kSrc * kSrc, 0xFF204060);
    std::vector<uint32_t> src44(kSide * kSide);
    for (int y = 0; y < kSide; y++)
        for (int x = 0; x < kSide; x++) src44[y * kSide + x] = src256[(y * kSrc / kSide) * kSrc + (x * kSrc / kSide)];

    auto makeSurfaces = [&](int n) {
        std::vector<wuc::CompositionDrawingSurface> keep;
        for (int i = 0; i < n; i++) {
            keep.push_back(gdev.CreateDrawingSurface({kSide, kSide},
                                                     winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                     winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied));
        }
        return keep;
    };
    const bool small = mode == Mode::Downs || mode == Mode::Reuse44;
    auto paint = [&](std::vector<wuc::CompositionDrawingSurface>& keep) {
        if (mode == Mode::None) return;
        for (auto& surf : keep) {
            auto si = surf.as<ICompositionDrawingSurfaceInterop>();
            winrt::com_ptr<ID2D1DeviceContext> dc;
            POINT off{};
            winrt::check_hresult(si->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &off));
            dc->Clear(D2D1::ColorF(0, 0.0f));
            auto& px = small ? src44 : src256;
            const UINT side = small ? kSide : kSrc;
            winrt::com_ptr<ID2D1Bitmap> bmp;
            D2D1_BITMAP_PROPERTIES props =
                D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            winrt::check_hresult(dc->CreateBitmap(D2D1::SizeU(side, side), px.data(), side * 4, &props, bmp.put()));
            D2D1_RECT_F dest = D2D1::RectF(static_cast<float>(off.x), static_cast<float>(off.y),
                                           off.x + static_cast<float>(kSide), off.y + static_cast<float>(kSide));
            dc->DrawBitmap(bmp.get(), &dest, 1.0f,
                           mode == Mode::Linear ? D2D1_INTERPOLATION_MODE_LINEAR
                                                : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                           nullptr, nullptr);
            winrt::check_hresult(si->EndDraw());
        }
    };

    auto surfaces = makeSurfaces(kIcons);
    paint(surfaces);
    Say(mode == Mode::None ? "+ 27 icon surfaces at 44px (nothing drawn)" : "+ 27 icon brushes (MakeIconBrush x27)");
    printf("      => %.1f KB of private commit per icon\n", (Priv() - 3.1) * 1024.0 / kIcons);
    if (mode == Mode::Clear) {
        d2d->ClearResources(0);
        Say("  after ClearResources()");
    }

    if (mode == Mode::Reclaim) {
        surfaces.clear();
        gdev = nullptr;
        d2d = nullptr;
        d3d = nullptr;
        comp = nullptr;
        dqc = nullptr;
        winrt::uninit_apartment();
        Say("  released surfaces, graphics device, devices, compositor");
        printf("== total for this stack: %.1f MB private\n\n", Priv());
        return 0;
    }

    // Ten more dock refreshes. With reuse, the same 27 surfaces are repainted;
    // otherwise every surface is rebuilt, which is what a refresh does today.
    const bool reuse = mode == Mode::Reuse || mode == Mode::Reuse44;
    double before = Priv();
    for (int round = 0; round < 10; round++) {
        if (reuse) {
            paint(surfaces);
        } else {
            surfaces = makeSurfaces(kIcons);
            paint(surfaces);
        }
        if (mode == Mode::Clear) d2d->ClearResources(0);
    }
    Say(reuse ? "+ 10 repaints of the SAME 27 surfaces" : "+ 10 more full refreshes (270 icon surfaces)");
    printf("      => %.1f MB per %s of 27\n", (Priv() - before) / 10.0, reuse ? "repaint" : "refresh");

    printf("== total for this stack: %.1f MB private\n\n", Priv());
    return 0;
}
