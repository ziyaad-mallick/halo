// Frame-timing harness for the Start panel (Launcher), hosted in isolation: this process makes its own compositor,
// graphics device and dock-style glass, builds the real halo::Launcher (src/launcher.cpp, linked in) and opens and
// closes it N times. It never starts halo.exe, installs a hook or touches the live shell. The panel window really
// appears on the primary monitor for a moment on each run (it is a real top-level window; that is what is measured).
//
// Per run it reports (QPC clock throughout):
//   show_call        how long Launcher::Show() blocks the UI thread (the work between the Win tap and the first frame)
//   first_frame      request -> first frame DWM presents that changes the panel's window rectangle
//   settled          request -> the first >=100 ms gap with no frame (the open spring is over); "never" if there is none
//   intervals        present-to-present gaps from the first frame to settled: min/avg/p95/max, count over 20 ms
//   idle             frames the compositor still presents for the panel while it is open and nothing moves
//   hover / after    the same with the pointer on the apps, and after it has left again (the icon "wave")
//   close            request -> window hidden, and the close animation's frame intervals
// Frames come from Desktop Duplication METADATA only (present time + dirty rectangles; no pixel is ever read or saved),
// like tools/framegaps.cpp. other-desktop = frames in the same window that did not touch the panel (the noise floor). GPU = the "GPU Engine ... engtype_3D" counters, summed for this
// process and for dwm.exe, as tools/gpu-measure.ps1 does (one sample a second).
//
//   start_perf.exe [--runs N] [--mode h2|classic] [--effect rise|bounce|swipe|fade|swoop|none] [--speed S] [--dim on|off]
//                  [--idle-ms MS] [--gpu-s SECONDS] [--no-hover] [--tag NAME]
// Defaults mirror the owner's own settings (halo2 on, Start effect fade, dim desktop on). Build: tools/build.sh start_perf
#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dispatcherqueue.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <pdh.h>
#include <shellscalingapi.h>
#include <tlhelp32.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../src/icons.h"
#include "../src/launcher.h"
#include "../src/settings.h"
#include "../src/theme.h"

extern "C" HRESULT WINAPI D2D1CreateDevice(IDXGIDevice*, D2D1_CREATION_PROPERTIES const*, ID2D1Device**);

#ifndef HALO_COMPOSITION_INTEROP_DEFINED
#define HALO_COMPOSITION_INTEROP_DEFINED
struct ICompositionDrawingSurfaceInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE BeginDraw(RECT const* updateRect, REFIID iid, void** updateObject, POINT* offset) = 0;
    virtual HRESULT STDMETHODCALLTYPE EndDraw() = 0;
    virtual HRESULT STDMETHODCALLTYPE Resize(SIZE sizePixels) = 0;
    virtual HRESULT STDMETHODCALLTYPE Scroll(RECT const* scrollRect, RECT const* clipRect, int offsetX, int offsetY) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResumeDraw() = 0;
    virtual HRESULT STDMETHODCALLTYPE SuspendDraw() = 0;
};
__CRT_UUID_DECL(ICompositionDrawingSurfaceInterop, 0xFD04E6E3, 0xFE0C, 0x4C3C, 0xAB, 0x19, 0xA0, 0x76, 0x01, 0xA5, 0x76, 0xEE)
#endif

namespace wuc = winrt::Windows::UI::Composition;
using winrt::Windows::Foundation::Size;

namespace {

// ---------------------------------------------------------------------------------------------------- clock
LONGLONG g_qf = 0;
LONGLONG Now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
double Ms(LONGLONG a, LONGLONG b) { return (b - a) * 1000.0 / g_qf; }

// ------------------------------------------------------------------------------------- frame source (Desktop Duplication metadata)
struct Frame {
    LONGLONG at;                // LastPresentTime (QPC)
    std::vector<RECT> dirty;
};

class FrameSource {
public:
    bool Start() {
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, m_dev.put(), nullptr, nullptr));
        auto xd = m_dev.as<IDXGIDevice>();
        winrt::com_ptr<IDXGIAdapter> ad;
        xd->GetAdapter(ad.put());
        winrt::com_ptr<IDXGIOutput> out;
        if (FAILED(ad->EnumOutputs(0, out.put()))) return false;
        auto out1 = out.as<IDXGIOutput1>();
        if (FAILED(out1->DuplicateOutput(m_dev.get(), m_dup.put()))) return false;
        m_thread = std::thread([this] { Loop(); });
        return true;
    }
    ~FrameSource() { m_stop = true; if (m_thread.joinable()) m_thread.join(); }
    // Frames presented in [from, to) whose dirty rectangles touch `roi`; area = fraction of roi they cover (summed, capped at 1).
    struct Hit { LONGLONG at; double cover; RECT box; };
    std::vector<Hit> In(LONGLONG from, LONGLONG to, RECT roi) {
        std::vector<Hit> r;
        std::lock_guard l(m_mu);
        const double area = std::max<LONG>(1, roi.right - roi.left) * double(std::max<LONG>(1, roi.bottom - roi.top));
        for (auto const& f : m_frames) {
            if (f.at < from || f.at >= to) continue;
            double a = 0;
            RECT box{LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN};
            for (auto const& d : f.dirty) {
                RECT x;
                if (IntersectRect(&x, &d, &roi)) {
                    a += double(x.right - x.left) * (x.bottom - x.top);
                    box = {std::min(box.left, x.left), std::min(box.top, x.top), std::max(box.right, x.right), std::max(box.bottom, x.bottom)};
                }
            }
            if (a > 0) r.push_back({f.at, std::min(1.0, a / area), box});
        }
        return r;
    }
    size_t AllCount(LONGLONG from, LONGLONG to) {
        std::lock_guard l(m_mu);
        size_t n = 0;
        for (auto const& f : m_frames) n += f.at >= from && f.at < to;
        return n;
    }

private:
    void Loop() {
        std::vector<BYTE> buf;
        while (!m_stop) {
            DXGI_OUTDUPL_FRAME_INFO fi{};
            winrt::com_ptr<IDXGIResource> res;
            HRESULT hr = m_dup->AcquireNextFrame(20, &fi, res.put());
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
            if (FAILED(hr)) { m_lost = true; return; }
            if (fi.LastPresentTime.QuadPart != 0) {
                Frame f{fi.LastPresentTime.QuadPart, {}};
                if (fi.TotalMetadataBufferSize) {
                    buf.resize(fi.TotalMetadataBufferSize);
                    UINT got = 0;
                    if (SUCCEEDED(m_dup->GetFrameDirtyRects(static_cast<UINT>(buf.size()), reinterpret_cast<RECT*>(buf.data()), &got)))
                        f.dirty.assign(reinterpret_cast<RECT*>(buf.data()), reinterpret_cast<RECT*>(buf.data()) + got / sizeof(RECT));
                }
                std::lock_guard l(m_mu);
                m_frames.push_back(std::move(f));
            }
            res = nullptr;
            m_dup->ReleaseFrame();
        }
    }
    winrt::com_ptr<ID3D11Device> m_dev;
    winrt::com_ptr<IDXGIOutputDuplication> m_dup;
    std::thread m_thread;
    std::mutex m_mu;
    std::vector<Frame> m_frames;
    std::atomic<bool> m_stop{false}, m_lost{false};
};

// ------------------------------------------------------------------------------------------------- GPU counters
DWORD PidOf(wchar_t const* exe) {
    DWORD pid = 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W e{sizeof(e)};
    for (BOOL ok = Process32FirstW(s, &e); ok; ok = Process32NextW(s, &e))
        if (!_wcsicmp(e.szExeFile, exe)) { pid = e.th32ProcessID; break; }
    CloseHandle(s);
    return pid;
}

struct GpuSample { LONGLONG at; double self, dwm; };
class Gpu {
public:
    bool Start() {
        if (PdhOpenQueryW(nullptr, 0, &m_q) != ERROR_SUCCESS) return false;
        if (PdhAddEnglishCounterW(m_q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &m_c) != ERROR_SUCCESS) return false;
        m_dwm = PidOf(L"dwm.exe");
        PdhCollectQueryData(m_q);
        m_thread = std::thread([this] {
            while (!m_stop) {
                for (int i = 0; i < 10 && !m_stop; i++) Sleep(100);
                if (m_stop) break;
                Sample();
            }
        });
        return true;
    }
    ~Gpu() { m_stop = true; if (m_thread.joinable()) m_thread.join(); }
    std::vector<GpuSample> In(LONGLONG from, LONGLONG to) {
        std::lock_guard l(m_mu);
        std::vector<GpuSample> r;
        for (auto const& s : m_s) if (s.at >= from && s.at < to) r.push_back(s);
        return r;
    }

private:
    void Sample() {
        if (PdhCollectQueryData(m_q) != ERROR_SUCCESS) return;
        DWORD bytes = 0, count = 0;
        PdhGetFormattedCounterArrayW(m_c, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
        if (!bytes) return;
        std::vector<BYTE> buf(bytes);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
        if (PdhGetFormattedCounterArrayW(m_c, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) return;
        const std::wstring mine = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_", dwm = L"pid_" + std::to_wstring(m_dwm) + L"_";
        GpuSample g{Now(), 0, 0};
        for (DWORD i = 0; i < count; i++) {
            std::wstring n = items[i].szName;
            if (n.find(L"engtype_3D") == std::wstring::npos) continue;
            if (n.find(mine) != std::wstring::npos) g.self += items[i].FmtValue.doubleValue;
            if (n.find(dwm) != std::wstring::npos) g.dwm += items[i].FmtValue.doubleValue;
        }
        std::lock_guard l(m_mu);
        m_s.push_back(g);
    }
    PDH_HQUERY m_q{};
    PDH_HCOUNTER m_c{};
    DWORD m_dwm = 0;
    std::thread m_thread;
    std::mutex m_mu;
    std::vector<GpuSample> m_s;
    std::atomic<bool> m_stop{false};
};


// ---------------------------------------------------------------------------------------------------- host
struct Env {
    winrt::Windows::System::DispatcherQueueController dq{nullptr};
    wuc::Compositor comp{nullptr};
    winrt::com_ptr<ID3D11Device> d3d;
    winrt::com_ptr<ID2D1Device> d2d;
    wuc::CompositionGraphicsDevice gdev{nullptr};
    float scale = 1.0f;
    RECT monitor{};
};

wuc::CompositionDrawingSurface MakeSurface(Env& e, Size size) {
    return e.gdev.CreateDrawingSurface(size, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                       winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
}

// Draws `bgra` (premultiplied, w x h) into a new surface scaled to px x px, or the xorshift noise when bgra is null (halo.cpp does both).
wuc::CompositionBrush BrushOf(Env& e, uint8_t const* bgra, int w, int h, float px, bool noise) {
    auto surface = MakeSurface(e, {px, px});
    if (noise) surface = MakeSurface(e, {static_cast<float>(w), static_cast<float>(h)});
    auto interop = surface.as<ICompositionDrawingSurfaceInterop>();
    winrt::com_ptr<ID2D1DeviceContext> dc;
    POINT offset{};
    winrt::check_hresult(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &offset));
    try {
        std::vector<uint32_t> px32;
        if (noise) {
            px32.resize(static_cast<size_t>(w) * h);
            uint32_t seed = 0x2545F491;
            for (auto& p : px32) {
                seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
                uint32_t g = seed & 0xFF;
                p = 0xFF000000 | g << 16 | g << 8 | g;
            }
            bgra = reinterpret_cast<uint8_t const*>(px32.data());
        } else {
            dc->Clear(D2D1::ColorF(0, 0.0f));
        }
        winrt::com_ptr<ID2D1Bitmap> bitmap;
        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        winrt::check_hresult(dc->CreateBitmap(D2D1::SizeU(w, h), bgra, w * 4, &props, bitmap.put()));
        D2D1_RECT_F dest = noise ? D2D1::RectF(float(offset.x), float(offset.y), float(offset.x + w), float(offset.y + h))
                                 : D2D1::RectF(float(offset.x), float(offset.y), offset.x + px, offset.y + px);
        dc->DrawBitmap(bitmap.get(), &dest, 1.0f, noise ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    } catch (...) {
        interop->EndDraw();
        throw;
    }
    winrt::check_hresult(interop->EndDraw());
    auto brush = e.comp.CreateSurfaceBrush(surface);
    brush.Stretch(noise ? wuc::CompositionStretch::None : wuc::CompositionStretch::Uniform);
    return brush;
}

// The dock's glass, as Halo::BuildPill builds it (halo.cpp): blurred host backdrop, tint, grain, gradient rim, outer hairline.
wuc::ContainerVisual BuildGlass(Env& e, int noiseHeight, halo::settings::Surface const& cfg, int noiseWidth, float radius) {
    auto& comp = e.comp;
    auto pill = comp.CreateContainerVisual();
    auto glass = comp.CreateSpriteVisual();
    glass.RelativeSizeAdjustment({1, 1});
    glass.Brush(comp.CreateHostBackdropBrush());
    pill.Children().InsertAtTop(glass);
    auto tint = comp.CreateSpriteVisual();
    tint.RelativeSizeAdjustment({1, 1});
    tint.Brush(comp.CreateColorBrush(halo::theme::C(halo::theme::Glass(cfg).tint)));
    pill.Children().InsertAtTop(tint);
    auto noise = comp.CreateSpriteVisual();
    noise.RelativeSizeAdjustment({1, 1});
    noise.Opacity(halo::theme::P().noise);
    noise.Brush(BrushOf(e, nullptr, noiseWidth ? noiseWidth : e.monitor.right - e.monitor.left, noiseHeight ? noiseHeight : 80, 0, true));
    pill.Children().InsertAtTop(noise);

    const float t = std::max(1.0f, std::round(e.scale));
    const float r = (radius > 0 ? radius : 20 * e.scale) - t / 2;
    auto rimShape = comp.CreateRoundedRectangleGeometry();
    auto const& pal = halo::theme::P();
    const bool hasEdge = pal.edge.a != 0;
    const float ri = hasEdge ? t : 0.0f;
    rimShape.Offset({t / 2 + ri, t / 2 + ri});
    rimShape.CornerRadius({r - ri, r - ri});
    auto rimSize = comp.CreateExpressionAnimation(L"Vector2(pill.Size.X - " + std::to_wstring(t + 2 * ri) + L", pill.Size.Y - " + std::to_wstring(t + 2 * ri) + L")");
    rimSize.SetReferenceParameter(L"pill", pill);
    rimShape.StartAnimation(L"Size", rimSize);
    auto gradient = comp.CreateLinearGradientBrush();
    gradient.MappingMode(wuc::CompositionMappingMode::Relative);
    gradient.StartPoint({0, 0});
    gradient.EndPoint({0, 1});
    auto stop = [&](float at, halo::theme::Argb c) { gradient.ColorStops().Append(comp.CreateColorGradientStop(at, halo::theme::C(c))); };
    stop(0.0f, pal.rimTop); stop(0.35f, pal.rimMid); stop(0.7f, pal.rimLow); stop(1.0f, pal.rimBottom);
    auto rimSprite = comp.CreateSpriteShape(rimShape);
    rimSprite.StrokeBrush(gradient);
    rimSprite.StrokeThickness(t);
    auto rim = comp.CreateShapeVisual();
    rim.RelativeSizeAdjustment({1, 1});
    rim.Shapes().Append(rimSprite);
    pill.Children().InsertAtTop(rim);
    if (hasEdge) {
        auto edgeShape = comp.CreateRoundedRectangleGeometry();
        edgeShape.Offset({t / 2, t / 2});
        edgeShape.CornerRadius({r, r});
        auto edgeSize = comp.CreateExpressionAnimation(L"Vector2(pill.Size.X - " + std::to_wstring(t) + L", pill.Size.Y - " + std::to_wstring(t) + L")");
        edgeSize.SetReferenceParameter(L"pill", pill);
        edgeShape.StartAnimation(L"Size", edgeSize);
        auto edgeSprite = comp.CreateSpriteShape(edgeShape);
        edgeSprite.StrokeBrush(comp.CreateColorBrush(halo::theme::C(pal.edge)));
        edgeSprite.StrokeThickness(t);
        auto edge = comp.CreateShapeVisual();
        edge.RelativeSizeAdjustment({1, 1});
        edge.Shapes().Append(edgeSprite);
        pill.Children().InsertAtTop(edge);
    }
    return pill;
}

// ---------------------------------------------------------------------------------------------------- helpers
void Pump(LONGLONG until) {  // runs this thread's message loop (timers, compositor callbacks) until the QPC time
    while (true) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        LONGLONG left = until - Now();
        if (left <= 0) return;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(std::min<LONGLONG>(left * 1000 / g_qf, 4) + 0), QS_ALLINPUT);
    }
}
LONGLONG After(LONGLONG t, double ms) { return t + static_cast<LONGLONG>(ms * g_qf / 1000.0); }

HWND FindPanel() {
    HWND found = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND h, LPARAM l) -> BOOL {
        wchar_t cls[64];
        GetClassNameW(h, cls, 64);
        if (!wcscmp(cls, L"HaloLauncherWindow")) { *reinterpret_cast<HWND*>(l) = h; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}

struct Stats { double min = 0, avg = 0, p95 = 0, max = 0; int over20 = 0, n = 0; };
Stats Intervals(std::vector<FrameSource::Hit> const& f) {
    Stats s;
    std::vector<double> g;
    for (size_t i = 1; i < f.size(); i++) g.push_back(Ms(f[i - 1].at, f[i].at));
    s.n = static_cast<int>(g.size());
    if (g.empty()) return s;
    double sum = 0;
    for (double x : g) { sum += x; s.over20 += x > 20.0; }
    std::sort(g.begin(), g.end());
    s.min = g.front(); s.max = g.back(); s.avg = sum / g.size();
    s.p95 = g[std::min(g.size() - 1, static_cast<size_t>(std::ceil(0.95 * g.size())) - 1)];
    return s;
}
std::string Fmt(Stats const& s) {
    char b[160];
    std::snprintf(b, sizeof b, "n=%d min/avg/p95/max=%.1f/%.1f/%.1f/%.1f over20=%d", s.n, s.min, s.avg, s.p95, s.max, s.over20);
    return b;
}
double Median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

struct Window {  // one measured window: frames in the panel rectangle, other-desktop frames and GPU over it
    int frames = 0;
    double perSec = 0, cover = 0, otherPerSec = 0, gpuSelf = 0, gpuDwm = 0;
};

}  // namespace

int wmain(int argc, wchar_t** argv) {
    QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&g_qf));
    int runs = 8, idleMs = 2000, gpuSecs = 6;
    std::wstring mode = L"h2", effect = L"fade", dim = L"on", tag = L"start_perf", speed = L"55";
    bool hover = true;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--runs") runs = _wtoi(next().c_str());
        else if (a == L"--mode") mode = next();
        else if (a == L"--effect") effect = next();
        else if (a == L"--speed") speed = next();
        else if (a == L"--dim") dim = next();
        else if (a == L"--idle-ms") idleMs = _wtoi(next().c_str());
        else if (a == L"--gpu-s") gpuSecs = _wtoi(next().c_str());
        else if (a == L"--no-hover") hover = false;
        else if (a == L"--tag") tag = next();
    }
    const bool h2 = mode == L"h2";

    // Scratch settings (never the live ones), scratch MRU, and every test guard the existing drivers use.
    wchar_t tmp[MAX_PATH];
    GetEnvironmentVariableW(L"TEMP", tmp, MAX_PATH);
    std::wstring dir = std::wstring(tmp) + L"\\halo-start-perf-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring ini = dir + L"\\settings.ini", recent = dir + L"\\recent.txt", logPath = dir + L"\\launcher.log";
    {
        FILE* f = _wfopen(ini.c_str(), L"wb");
        std::string s = "[general]\ntheme = dark\naccent = #004570\nreduce_motion = off\nhalo2 = " + std::string(h2 ? "on" : "off") +
                        "\n[taskbar]\nopacity = 71\n[start]\neffect = " + std::string(effect.begin(), effect.end()) + "\nspeed = " + std::string(speed.begin(), speed.end()) +
                        "\nmotion = 200\nopacity = 100\ntint = #0F2A2E\nblur = 40\nrecent_apps = 6\nrecent_files = 12\ndim_desktop = " + std::string(dim.begin(), dim.end()) + "\n";
        fwrite(s.data(), 1, s.size(), f);
        fclose(f);
        f = _wfopen(recent.c_str(), L"wb");
        fclose(f);
    }
    SetEnvironmentVariableW(L"HALO_SETTINGS_FILE", ini.c_str());
    SetEnvironmentVariableW(L"HALO_TEST_RECENT", recent.c_str());
    for (auto n : {L"HALO_TEST", L"HALO_TEST_STAY", L"HALO_TEST_NOFOCUS", L"HALO_TEST_POWER_DRYRUN", L"HALO_TEST_CC_DRYRUN"}) SetEnvironmentVariableW(n, L"1");
    FILE* lg = nullptr;
    _wfreopen_s(&lg, logPath.c_str(), L"w", stderr);  // the Launcher logs "test points" and timings here

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    halo::settings::Reload();
    halo::theme::Refresh();
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    Env e;
    DispatcherQueueOptions opt{sizeof(opt), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
    winrt::check_hresult(CreateDispatcherQueueController(opt, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(e.dq))));
    e.comp = wuc::Compositor();
    // Halo draws its surfaces on a WARP device (InitGraphics in halo.cpp); the compositor itself is on the GPU either way.
    winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, e.d3d.put(), nullptr, nullptr));
    winrt::check_hresult(D2D1CreateDevice(e.d3d.as<IDXGIDevice>().get(), nullptr, e.d2d.put()));
    auto interop = e.comp.as<ABI::Windows::UI::Composition::ICompositorInterop>();
    winrt::check_hresult(interop->CreateGraphicsDevice(reinterpret_cast<IUnknown*>(e.d2d.get()),
        reinterpret_cast<ABI::Windows::UI::Composition::ICompositionGraphicsDevice**>(winrt::put_abi(e.gdev))));

    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    e.monitor = mi.rcMonitor;
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    e.scale = dx / 96.0f;

    halo::LauncherHost host;
    host.glass = [&](int nh, halo::settings::Surface const& g, int nw, float radius) { return BuildGlass(e, nh, g, nw, radius); };
    std::map<std::wstring, wuc::CompositionBrush> brushes;
    host.appIcon = [&](std::wstring const& target) -> wuc::CompositionBrush {
        auto it = brushes.find(target);
        if (it != brushes.end()) return it->second;
        wuc::CompositionBrush b{nullptr};
        IconImage icon;
        const float px = std::ceil(48 * e.scale * 1.5f);  // the dock's icon size * magnification
        if (LoadJumboIcon(target, icon) && icon.width != 0) b = BrushOf(e, icon.bgra.data(), icon.width, icon.height, px, false);
        brushes.insert_or_assign(target, b);
        return b;
    };
    host.launch = [](std::wstring const&) {};  // a click never launches anything here
    host.closed = [] {};

    FrameSource frames;
    const bool haveFrames = frames.Start();
    Gpu gpu;
    const bool haveGpu = gpu.Start();
    std::printf("start_perf tag=%ls mode=%ls effect=%ls speed=%ls dim=%ls runs=%d idle=%d ms scale=%.2f monitor=%ldx%ld frames=%s gpu=%s\n", tag.c_str(),
                mode.c_str(), effect.c_str(), speed.c_str(), dim.c_str(), runs, idleMs, e.scale, e.monitor.right - e.monitor.left,
                e.monitor.bottom - e.monitor.top, haveFrames ? "desktop-duplication-metadata" : "UNAVAILABLE", haveGpu ? "pdh" : "UNAVAILABLE");
    std::fflush(stdout);

    halo::Launcher launcher(GetModuleHandleW(nullptr), e.comp, e.gdev, std::move(host));
    Pump(After(Now(), 1500));  // let the prefetch worker deliver its first snapshot

    // Above a dock pill that is 68 logical px tall and floats 6 px up, ten px clear of it (Halo::LauncherAnchorNow).
    halo::LauncherAnchor anchor;
    anchor.centerX = (e.monitor.left + e.monitor.right) / 2;
    anchor.bottom = e.monitor.bottom - static_cast<int>(std::lround((6 + 68 + 10) * e.scale));
    anchor.dockWidth = 600 * e.scale;
    anchor.scale = e.scale;
    anchor.monitor = e.monitor;

    auto windowOf = [&](double fromMs, double toMs, LONGLONG t0, RECT roi) {
        Window w;
        auto hits = frames.In(After(t0, fromMs), After(t0, toMs), roi);
        w.frames = static_cast<int>(hits.size());
        const double sec = (toMs - fromMs) / 1000.0;
        w.perSec = hits.size() / sec;
        for (auto const& h : hits) w.cover += h.cover;
        w.cover = hits.empty() ? 0 : w.cover / hits.size();
        w.otherPerSec = (frames.AllCount(After(t0, fromMs), After(t0, toMs)) - hits.size()) / sec;  // desktop frames that did not touch the panel
        auto g = gpu.In(After(t0, fromMs), After(t0, toMs));
        for (auto const& s : g) { w.gpuSelf += s.self; w.gpuDwm += s.dwm; }
        if (!g.empty()) { w.gpuSelf /= g.size(); w.gpuDwm /= g.size(); }
        return w;
    };

    RECT primed{};
    // ---- closed baseline: what DWM does with Start hidden (the floor for every "dwm" number below).
    {
        // A priming open/close first (cold caches, and it tells us the panel's rectangle); it is not one of the numbered runs.
        launcher.Show(anchor);
        Pump(After(Now(), 2500));
        if (HWND ph = FindPanel()) GetWindowRect(ph, &primed);
        launcher.Hide();
        Pump(After(Now(), 1500));
        const LONGLONG t0 = Now();
        Pump(After(t0, 3000));
        Window w = windowOf(0, 3000, t0, primed);
        {
            RECT u{LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN};
            auto hs = frames.In(t0, After(t0, 3000), primed);
            for (auto const& h : hs) u = {std::min(u.left, h.box.left), std::min(u.top, h.box.top), std::max(u.right, h.box.right), std::max(u.bottom, h.box.bottom)};
            std::printf("panel rect %ld,%ld..%ld,%ld; closed-state frames land in %ld,%ld..%ld,%ld (%zu frames)\n", primed.left, primed.top, primed.right, primed.bottom,
                        hs.empty() ? 0 : u.left, hs.empty() ? 0 : u.top, hs.empty() ? 0 : u.right, hs.empty() ? 0 : u.bottom, hs.size());
        }
        std::printf("closed baseline (3 s): panel-rect frames/s=%.1f other-desktop frames/s=%.1f gpu%% 3D self=%.1f dwm=%.1f\n", w.perSec, w.otherPerSec, w.gpuSelf, w.gpuDwm);
        std::fflush(stdout);
    }

    std::vector<double> mShow, mFirst, mSettle, mAvg, mP95, mMax, mOver, mIdle, mIdleCover, mIdleDwm, mHover, mAfter, mClose, mCloseMax, mCloseOver, mCloseHide;
    int neverSettled = 0;
    int clean = 0;
    for (int run = 1; clean < runs && run <= runs * 3; run++) {
        // The noise floor for this run: frames the owner's own screen activity puts in the panel rectangle while Start is closed.
        const LONGLONG f0 = Now();
        Pump(After(f0, 1500));
        const double floorHz = windowOf(0, 1500, f0, primed).perSec;
        const bool noisy = floorHz > 3.0;  // discarded from the medians (still printed); another run is added
        const LONGLONG t0 = Now();
        launcher.Show(anchor);
        const LONGLONG shown = Now();
        HWND panel = FindPanel();
        RECT roi{};
        if (panel) GetWindowRect(panel, &roi);
        const double idleFrom = 2300, idleTo = idleFrom + idleMs;
        Pump(After(t0, idleFrom));
        Pump(After(t0, idleTo));
        Window idle = windowOf(idleFrom, idleTo, t0, roi);

        // open burst
        auto hits = frames.In(t0, After(t0, idleFrom), roi);
        double first = hits.empty() ? -1 : Ms(t0, hits.front().at), settled = -1;
        std::vector<FrameSource::Hit> burst;
        for (size_t i = 0; i < hits.size(); i++) {
            burst.push_back(hits[i]);
            if (i + 1 < hits.size() && Ms(hits[i].at, hits[i + 1].at) >= 100.0) { settled = Ms(t0, hits[i].at); break; }
            if (i + 1 == hits.size() && Ms(hits[i].at, After(t0, idleFrom)) >= 100.0) settled = Ms(t0, hits[i].at);
        }
        if (settled < 0) neverSettled++;
        Stats st = Intervals(burst);

        // hover: pointer on the first app icon, then away
        Window hov{}, aft{};
        if (hover && panel && h2) {
            int px = 0, py = 0;
            if (FILE* f = _wfopen(logPath.c_str(), L"rb")) {
                std::string all;
                char buf[4096];
                size_t n;
                while ((n = fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n);
                fclose(f);
                auto at = all.rfind("app0=");
                if (at != std::string::npos) std::sscanf(all.c_str() + at, "app0=%d,%d", &px, &py);
            }
            const LONGLONG h0 = Now();
            PostMessageW(panel, WM_MOUSEMOVE, 0, MAKELPARAM(px, py));
            Pump(After(h0, 200));
            PostMessageW(panel, WM_MOUSEMOVE, 0, MAKELPARAM(px + 3, py + 2));
            Pump(After(h0, 1200));
            hov = windowOf(0, 1200, h0, roi);
            PostMessageW(panel, WM_MOUSEMOVE, 0, MAKELPARAM(4, 4));  // off the panel
            const LONGLONG l0 = Now();
            Pump(After(l0, 900));  // 360 ms relax + 420 ms unbind
            const LONGLONG q0 = Now();
            Pump(After(q0, 1500));
            aft = windowOf(0, 1500, q0, roi);
        }

        // close
        const LONGLONG c0 = Now();
        launcher.Hide();
        double hidden = -1;
        while (Ms(c0, Now()) < 1500) {
            Pump(After(Now(), 5));
            if (panel && !IsWindowVisible(panel)) { hidden = Ms(c0, Now()); break; }
        }
        Pump(After(Now(), 150));
        auto chits = frames.In(c0, After(c0, 1500), roi);
        std::vector<FrameSource::Hit> cburst;
        for (size_t i = 0; i < chits.size(); i++) {
            cburst.push_back(chits[i]);
            if (i + 1 < chits.size() && Ms(chits[i].at, chits[i + 1].at) >= 100.0) break;
        }
        Stats cs = Intervals(cburst);
        const double cFirst = chits.empty() ? -1 : Ms(c0, chits.front().at), cLast = cburst.empty() ? -1 : Ms(c0, cburst.back().at);

        std::printf("run %d%s: closed-floor=%.1f frames/s\n", run, noisy ? " NOISY, discarded" : "", floorHz);
        std::printf("       show_call=%.1f ms first_frame=%.1f ms settled=%s%.1f ms open_frames=%zu intervals[%s]\n", Ms(t0, shown), first,
                    settled < 0 ? "never " : "", settled < 0 ? 0.0 : settled, burst.size(), Fmt(st).c_str());
        std::printf("       idle(%d ms): panel frames=%d (%.1f/s, dirty cover %.0f%% of panel) other-desktop frames/s=%.1f gpu%% self=%.1f dwm=%.1f\n", idleMs, idle.frames, idle.perSec,
                    idle.cover * 100, idle.otherPerSec, idle.gpuSelf, idle.gpuDwm);
        if (hover && h2)
            std::printf("       hover(1.2 s): frames=%d (%.1f/s)   after pointer left (1.5 s idle): frames=%d (%.1f/s, dirty cover %.0f%%)\n", hov.frames, hov.perSec,
                        aft.frames, aft.perSec, aft.cover * 100);
        std::printf("       close: request->hidden=%.1f ms first_frame=%.1f last_frame=%.1f frames=%zu intervals[%s]\n", hidden, cFirst, cLast, cburst.size(), Fmt(cs).c_str());
        std::fflush(stdout);
        if (noisy) { Pump(After(Now(), 700)); continue; }
        clean++;
        mShow.push_back(Ms(t0, shown)); mFirst.push_back(first); if (settled >= 0) mSettle.push_back(settled);
        mAvg.push_back(st.avg); mP95.push_back(st.p95); mMax.push_back(st.max); mOver.push_back(st.over20);
        mIdle.push_back(idle.perSec); mIdleCover.push_back(idle.cover * 100); mIdleDwm.push_back(idle.otherPerSec);
        mHover.push_back(hov.perSec); mAfter.push_back(aft.perSec);
        mClose.push_back(cs.avg); mCloseMax.push_back(cs.max); mCloseOver.push_back(cs.over20); mCloseHide.push_back(hidden);
        Pump(After(Now(), 700));
    }
    std::printf("MEDIAN over %d clean runs: show_call=%.1f first_frame=%.1f settled=%.1f (never-settled runs: %d) open_interval avg=%.1f p95=%.1f max=%.1f over20=%.0f | idle frames/s=%.1f other-desktop frames/s=%.1f"
                " | hover frames/s=%.1f after-leave frames/s=%.1f | close hidden=%.1f ms avg=%.1f max=%.1f over20=%.0f\n",
                clean, Median(mShow), Median(mFirst), Median(mSettle), neverSettled, Median(mAvg), Median(mP95), Median(mMax), Median(mOver), Median(mIdle), Median(mIdleDwm),
                Median(mHover), Median(mAfter), Median(mCloseHide), Median(mClose), Median(mCloseMax), Median(mCloseOver));

    // ---- GPU hold: open once, leave it idle, and read the counters a second at a time.
    if (gpuSecs > 0) {
        const LONGLONG t0 = Now();
        launcher.Show(anchor);
        Pump(After(t0, 2500));  // past the open motion, the refresh at 0.9 s and the settle at 1 s
        const LONGLONG g0 = Now();
        Pump(After(g0, gpuSecs * 1000.0));
        const LONGLONG g1 = Now();
        auto s = gpu.In(g0, g1);
        double ms = 0, md = 0, ps = 0, pd = 0;
        for (auto const& x : s) { ms += x.self; md += x.dwm; ps = std::max(ps, x.self); pd = std::max(pd, x.dwm); }
        const size_t n = std::max<size_t>(1, s.size());
        std::printf("GPU open+idle (%d s, %zu samples): self 3D mean=%.1f peak=%.1f   dwm 3D mean=%.1f peak=%.1f\n", gpuSecs, s.size(), ms / n, ps, md / n, pd);
        const LONGLONG c0 = Now();
        launcher.Hide();
        Pump(After(c0, 1200));
        const LONGLONG k0 = Now();
        Pump(After(k0, 3000));
        auto k = gpu.In(k0, After(k0, 3000));
        double kd = 0, ks = 0;
        for (auto const& x : k) { kd += x.dwm; ks += x.self; }
        const size_t kn = std::max<size_t>(1, k.size());
        std::printf("GPU closed (3 s, %zu samples): self 3D mean=%.1f   dwm 3D mean=%.1f\n", k.size(), ks / kn, kd / kn);
        // cycles: N open/close pairs back to back, GPU over the whole stretch (the "peak at open/close" case)
        const LONGLONG y0 = Now();
        for (int i = 0; i < 4; i++) {
            launcher.Show(anchor);
            Pump(After(Now(), 1500));
            launcher.Hide();
            Pump(After(Now(), 700));
        }
        const LONGLONG y1 = Now();
        auto y = gpu.In(y0, y1);
        double yd = 0, ys = 0, ypd = 0, yps = 0;
        for (auto const& x : y) { yd += x.dwm; ys += x.self; ypd = std::max(ypd, x.dwm); yps = std::max(yps, x.self); }
        const size_t yn = std::max<size_t>(1, y.size());
        std::printf("GPU 4 open/close cycles (%.1f s, %zu samples): self 3D mean=%.1f peak=%.1f   dwm 3D mean=%.1f peak=%.1f\n", Ms(y0, y1) / 1000.0, y.size(), ys / yn, yps, yd / yn, ypd);
    }
    launcher.Hide();
    Pump(After(Now(), 600));
    std::printf("launcher log: %ls\n", logPath.c_str());
    std::fflush(stdout);
    return 0;
}
