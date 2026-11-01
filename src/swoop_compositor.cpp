#include "swoop_compositor.h"
#include "lamp_geometry.h"

#include <d3d11.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <winver.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <mutex>
#include <new>
#include <vector>

namespace halo {
namespace {
constexpr int kRows = 48;
constexpr double kDuration = .38;
constexpr DWORD kEnable3D = 0x04000000;
template<class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// This private DWM entry point has no compatibility contract. It is enabled
// only for the exact DLL version tested here; all others use public thumbnails.
// Signature and ordinal described by ADeltaX: https://gist.github.com/ADeltaX/aea6aac248604d0cb7d423a61b06e247
using CreateShared = HRESULT (WINAPI*)(HWND, HWND, DWORD, DWM_THUMBNAIL_PROPERTIES*, void*, void**, HTHUMBNAIL*);
CreateShared TestedSharedVisual() {
    HMODULE dll = GetModuleHandleW(L"dwmapi.dll");
    if (!dll) return nullptr;
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(dll, path, ARRAYSIZE(path))) return nullptr;
    DWORD ignored = 0, size = GetFileVersionInfoSizeW(path, &ignored);
    if (!size) return nullptr;
    std::vector<BYTE> version(size);
    if (!GetFileVersionInfoW(path, 0, size, version.data())) return nullptr;
    VS_FIXEDFILEINFO* info = nullptr; UINT length = 0;
    if (!VerQueryValueW(version.data(), L"\\", reinterpret_cast<void**>(&info), &length) || length < sizeof(*info) || !info) return nullptr;
#ifdef HALO_SWOOP_TEST
    fprintf(stderr, "compositor DWM fixed version: %u.%u.%u.%u\n", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
            HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
#endif
    if (info->dwSignature != 0xFEEF04BD ||
        HIWORD(info->dwFileVersionMS) != 6 || LOWORD(info->dwFileVersionMS) != 2 ||
        HIWORD(info->dwFileVersionLS) != 26100 || LOWORD(info->dwFileVersionLS) != 9549) return nullptr;
    return reinterpret_cast<CreateShared>(GetProcAddress(dll, MAKEINTRESOURCEA(147)));
}

IDCompositionAnimation* Track(IDCompositionDesktopDevice* dev, const std::function<double(double)>& f) {
    IDCompositionAnimation* a = nullptr;
    if (FAILED(dev->CreateAnimation(&a)) || !a) return nullptr;
    constexpr int segments = 20;
    constexpr double step = 1.0 / segments, eps = .0001;
    auto derivative = [&](double u) {
        double lo = std::max(0.0, u-eps), hi = std::min(1.0, u+eps);
        return (f(hi)-f(lo))/(hi-lo)/kDuration;
    };
    for (int i = 0; i < segments; ++i) {
        double u = i*step, v = (i+1)*step, h = step*kDuration;
        double y0 = f(u), y1 = f(v), d0 = derivative(u), d1 = derivative(v);
        double q = 3*(y1-y0)/(h*h)-(2*d0+d1)/h;
        double c = -2*(y1-y0)/(h*h*h)+(d0+d1)/(h*h);
        if (FAILED(a->AddCubic(u*kDuration, (float)y0, (float)d0, (float)q, (float)c))) { Release(a); return nullptr; }
    }
    if (FAILED(a->End(kDuration, (float)f(1)))) Release(a);
    return a;
}

// The matrix elements share the same lamp geometry. Sample all of them at
// once instead of recomputing the curve separately for each property.
bool BuildCurves(IDCompositionDesktopDevice* dev, const std::function<std::array<double, 5>(double)>& value,
                 IDCompositionAnimation* (&out)[5]) {
    static const std::vector<double> knots = [] {
        std::vector<double> k;
        for (int i = 0; i <= 16; ++i) k.push_back(i / 16.0);
        // Times where smoothstep(time) crosses the lamp's two phase edges.
        for (double t : {.21618437233106985, .5333829368376322}) { k.push_back(t); k.push_back(1-t); }
        std::sort(k.begin(), k.end());
        return k;
    }();
    constexpr double eps = .0001;
    std::vector<std::array<double, 5>> y, deriv;
    y.reserve(knots.size()); deriv.reserve(knots.size());
    for (double u : knots) {
        double lo = std::max(0.0, u-eps), hi = std::min(1.0, u+eps);
        auto v = value(u), a = value(lo), b = value(hi);
        std::array<double, 5> d{};
        for (int p = 0; p < 5; ++p) d[p] = (b[p]-a[p])/(hi-lo)/kDuration;
        y.push_back(v); deriv.push_back(d);
    }
    for (auto& a : out) if (FAILED(dev->CreateAnimation(&a)) || !a) return false;
    for (size_t i = 0; i+1 < knots.size(); ++i) {
        double h = (knots[i+1]-knots[i])*kDuration;
        for (int p = 0; p < 5; ++p) {
            double y0 = y[i][p], y1 = y[i+1][p], d0 = deriv[i][p], d1 = deriv[i+1][p];
            double q = 3*(y1-y0)/(h*h)-(2*d0+d1)/h;
            double c = -2*(y1-y0)/(h*h*h)+(d0+d1)/(h*h);
            if (FAILED(out[p]->AddCubic(knots[i]*kDuration, (float)y0, (float)d0, (float)q, (float)c))) return false;
        }
    }
    for (int p = 0; p < 5; ++p) if (FAILED(out[p]->End(kDuration, (float)y.back()[p]))) return false;
    return true;
}
}  // namespace

class SwoopDevice {
public:
    ~SwoopDevice() { Release(m_dxgi); }
    void Prepare() {
        CreateShared create = TestedSharedVisual();
        if (!create) {
#ifdef HALO_SWOOP_TEST
            fprintf(stderr, "compositor warm: unsupported DWM version\n");
#endif
            return;
        }
        ID3D11Device* d3d = nullptr;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
        IDXGIDevice* dxgi = nullptr;
        if (SUCCEEDED(hr)) d3d->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi));
#ifdef HALO_SWOOP_TEST
        fprintf(stderr, "compositor warm: device hr=%08lx dxgi=%d\n", (unsigned long)hr, dxgi != nullptr);
#endif
        Release(d3d);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_dxgi = dxgi;
        m_create = dxgi ? create : nullptr;
    }
    IDXGIDevice* Acquire(CreateShared& create) {
        std::lock_guard<std::mutex> lock(m_mutex);
        create = m_create;
        if (m_dxgi) m_dxgi->AddRef();
        return m_dxgi;
    }
private:
    std::mutex m_mutex;
    IDXGIDevice* m_dxgi = nullptr;
    CreateShared m_create = nullptr;
};

static DWORD WINAPI WarmThread(void* raw) {
    std::unique_ptr<std::shared_ptr<SwoopDevice>> owner(static_cast<std::shared_ptr<SwoopDevice>*>(raw));
    (*owner)->Prepare();
    return 0;
}

std::shared_ptr<SwoopDevice> WarmSwoopDevice() {
    auto device = std::make_shared<SwoopDevice>();
    auto* owner = new (std::nothrow) std::shared_ptr<SwoopDevice>(device);
    if (owner) {
        HANDLE thread = CreateThread(nullptr, 0, WarmThread, owner, 0, nullptr);
        if (thread) CloseHandle(thread); else delete owner;
    }
    return device;
}

struct SwoopCompositor::Impl {
    struct Strip {
        IDCompositionVisual2* visual{};
        IDCompositionMatrixTransform* transform{};
        IDCompositionAnimation* curves[5]{};
        HTHUMBNAIL thumb{};
    };
    IDCompositionDesktopDevice* device{};
    IDCompositionTarget* target{};
    IDCompositionVisual2* root{};
    IDCompositionEffectGroup* effect{};
    IDCompositionAnimation* opacity{};
    std::vector<Strip> strips;
    ~Impl() {
        for (auto& s : strips) {
            if (s.thumb) DwmUnregisterThumbnail(s.thumb);
            for (auto& a : s.curves) Release(a);
            Release(s.transform); Release(s.visual);
        }
        Release(opacity); Release(effect); Release(root); Release(target); Release(device);
    }
};

SwoopCompositor::SwoopCompositor(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
SwoopCompositor::~SwoopCompositor() = default;

static DWORD WINAPI DisposeCompositor(void* raw) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    delete static_cast<SwoopCompositor*>(raw);
    if (SUCCEEDED(hr)) CoUninitialize();
    return 0;
}

void SwoopCompositor::DisposeAsync(std::unique_ptr<SwoopCompositor> compositor) {
    if (!compositor) return;
    auto* raw = compositor.release();
    if (!QueueUserWorkItem(DisposeCompositor, raw, WT_EXECUTEDEFAULT)) delete raw;
}

std::unique_ptr<SwoopCompositor> SwoopCompositor::Create(const std::shared_ptr<SwoopDevice>& cache,
                                                          HWND overlay, HWND source, RECT from, RECT icon,
                                                          RECT bounds, bool leaving) {
    if (!cache) return nullptr;
    CreateShared create = nullptr;
    IDXGIDevice* dxgi = cache->Acquire(create);
    if (!dxgi) {
#ifdef HALO_SWOOP_TEST
        fprintf(stderr, "compositor: device not ready\n");
#endif
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    HRESULT hr = DCompositionCreateDevice3(dxgi, __uuidof(IDCompositionDesktopDevice), reinterpret_cast<void**>(&impl->device));
    Release(dxgi);
    if (SUCCEEDED(hr)) hr = impl->device->CreateTargetForHwnd(overlay, TRUE, &impl->target);
    if (SUCCEEDED(hr)) hr = impl->device->CreateVisual(&impl->root);
    if (SUCCEEDED(hr)) hr = impl->device->CreateEffectGroup(&impl->effect);
    if (SUCCEEDED(hr)) hr = impl->root->SetEffect(impl->effect);
    if (FAILED(hr)) {
#ifdef HALO_SWOOP_TEST
        fprintf(stderr, "compositor: device/target hr=%08lx\n", (unsigned long)hr);
#endif
        return nullptr;
    }

    HTHUMBNAIL probe = nullptr; SIZE size{};
    hr = DwmRegisterThumbnail(overlay, source, &probe);
    if (SUCCEEDED(hr)) hr = DwmQueryThumbnailSourceSize(probe, &size);
    if (probe) DwmUnregisterThumbnail(probe);
    if (FAILED(hr) || size.cx <= 0 || size.cy <= 0) {
#ifdef HALO_SWOOP_TEST
        fprintf(stderr, "compositor: source hr=%08lx size=%ldx%ld\n", (unsigned long)hr, size.cx, size.cy);
#endif
        return nullptr;
    }
    LampRect w{(double)from.left, (double)from.top, (double)(from.right-from.left), (double)(from.bottom-from.top)};
    LampRect dock{(double)icon.left, (double)icon.top, (double)(icon.right-icon.left), (double)(icon.bottom-icon.top)};
    if (w.width <= 0 || w.height <= 0) return nullptr;
    impl->strips.reserve(kRows);
    for (int row = 0; row < kRows && SUCCEEDED(hr); ++row) {
        impl->strips.emplace_back(); auto& s = impl->strips.back();
        DWM_THUMBNAIL_PROPERTIES props{};
        props.dwFlags = DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE |
                        DWM_TNP_SOURCECLIENTAREAONLY | kEnable3D;
        props.opacity = 255; props.fVisible = TRUE;
        props.rcSource = {0, 0, size.cx, size.cy}; props.rcDestination = props.rcSource;
        hr = create(overlay, source, 2, &props, impl->device, reinterpret_cast<void**>(&s.visual), &s.thumb);
        IDCompositionRectangleClip* clip = nullptr;
        if (SUCCEEDED(hr)) hr = impl->device->CreateRectangleClip(&clip);
        if (SUCCEEDED(hr)) hr = clip->SetLeft(0.f);
        if (SUCCEEDED(hr)) hr = clip->SetRight((float)size.cx);
        if (SUCCEEDED(hr)) hr = clip->SetTop((float)(size.cy * row / (double)kRows));
        if (SUCCEEDED(hr)) hr = clip->SetBottom((float)(size.cy * (row+1) / (double)kRows));
        if (SUCCEEDED(hr)) hr = s.visual->SetClip(clip);
        Release(clip);
        if (SUCCEEDED(hr)) hr = impl->device->CreateMatrixTransform(&s.transform);
        if (SUCCEEDED(hr)) hr = s.visual->SetTransform(s.transform);
        if (SUCCEEDED(hr)) hr = impl->root->AddVisual(s.visual, TRUE, nullptr);
#ifdef HALO_SWOOP_TEST_COMPOSITOR_FAIL
        if (row == 5) hr = E_FAIL;  // exercise cleanup and public-thumbnail fallback
#endif
        auto value = [=](double u) -> std::array<double, 5> {
            double time = leaving ? u : 1-u, p = time*time*(3-2*time);
            double y0 = row/(double)kRows, y1 = (row+1)/(double)kRows;
            auto m = LampStripAffine(y0, y1, p, w, dock, size.cx, size.cy);
            return {m.sx, m.shear, m.tx, m.sy, m.ty};
        };
        auto initial = value(0);
        constexpr int indices[5][2] = {{0,0},{1,0},{2,0},{1,1},{2,1}};
        for (int i = 0; i < 5 && SUCCEEDED(hr); ++i) hr = s.transform->SetMatrixElement(indices[i][0], indices[i][1], (float)initial[i]);
        if (SUCCEEDED(hr) && !BuildCurves(impl->device, value, s.curves)) hr = E_FAIL;
    }
    auto fade = [=](double u) {
        double time = leaving ? u : 1-u, p = time*time*(3-2*time);
        double f = std::clamp((p-.7)/.3, 0.0, 1.0);
        return 1-f*f*(3-2*f);
    };
    if (SUCCEEDED(hr)) hr = impl->effect->SetOpacity((float)fade(0));
    if (SUCCEEDED(hr)) { impl->opacity = Track(impl->device, fade); if (!impl->opacity) hr = E_FAIL; }
    if (SUCCEEDED(hr)) hr = impl->root->SetOffsetX((float)(from.left-bounds.left));
    if (SUCCEEDED(hr)) hr = impl->root->SetOffsetY((float)(from.top-bounds.top));
    if (SUCCEEDED(hr)) hr = impl->target->SetRoot(impl->root);
    if (SUCCEEDED(hr)) hr = impl->device->Commit();
    if (FAILED(hr)) {
#ifdef HALO_SWOOP_TEST
        fprintf(stderr, "compositor: scene hr=%08lx rows=%zu\n", (unsigned long)hr, impl->strips.size());
#endif
        return nullptr;
    }
    return std::unique_ptr<SwoopCompositor>(new SwoopCompositor(std::move(impl)));
}

bool SwoopCompositor::Start() {
    HRESULT hr = S_OK;
    constexpr int indices[5][2] = {{0,0},{1,0},{2,0},{1,1},{2,1}};
    for (auto& s : m_impl->strips) {
        for (int i = 0; i < 5 && SUCCEEDED(hr); ++i) hr = s.transform->SetMatrixElement(indices[i][0], indices[i][1], s.curves[i]);
    }
    if (SUCCEEDED(hr)) hr = m_impl->effect->SetOpacity(m_impl->opacity);
    return SUCCEEDED(hr) && SUCCEEDED(m_impl->device->Commit());
}
}  // namespace halo
