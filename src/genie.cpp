#include "genie.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include "interop.h"

namespace wuc = winrt::Windows::UI::Composition;

namespace {

constexpr int kStripPx = 6;           // source rows per strip
constexpr int kGenieMs = 500;
constexpr int kZoomMs = 300;
constexpr UINT_PTR kReleaseTimer = 1;
constexpr UINT kReleaseTimeoutMs = 1000;

struct Rectf { float x, y, w, h; };

std::wstring NameOf(unsigned long long id, wchar_t const* suffix = L"") {
    wchar_t buf[64];
    swprintf(buf, 64, L"Local\\HaloGenie.%llx%ls", id, suffix);
    return buf;
}

}  // namespace

struct Genie::Anim {
    HANDLE ready{}, done{};
    wuc::ContainerVisual root{nullptr};
    bool needsRelease = false;
    ~Anim() {
        if (ready) CloseHandle(ready);
        if (done) CloseHandle(done);
    }
};

Genie::Genie(wuc::Compositor const& compositor, wuc::CompositionGraphicsDevice const& device, HWND dock)
    : m_comp(compositor), m_device(device), m_dock(dock) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"HaloGenieOverlay";
    RegisterClassExW(&wc);
    // One click-through window over the whole desktop, hidden until an animation runs.
    m_overlay = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                                    WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                wc.lpszClassName, L"Halo Genie", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, this);
    if (!m_overlay) throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));
    SetLayeredWindowAttributes(m_overlay, 0, 255, LWA_ALPHA);
    auto interop = m_comp.as<ICompositorDesktopInterop>();
    winrt::check_hresult(interop->CreateDesktopWindowTarget(m_overlay, TRUE, winrt::put_abi(m_target)));
    m_root = m_comp.CreateContainerVisual();
    m_root.RelativeSizeAdjustment({1, 1});
    m_target.Root(m_root);
}

Genie::~Genie() {
    m_anims.clear();
    if (m_overlay) DestroyWindow(m_overlay);
}

LRESULT CALLBACK Genie::OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE)
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* self = reinterpret_cast<Genie*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_TIMER && self && w == kReleaseTimer) {
        // A release that never came: drop whatever has finished playing.
        KillTimer(h, kReleaseTimer);
        std::vector<unsigned long long> stale;
        for (auto& [id, a] : self->m_anims)
            if (a->needsRelease && WaitForSingleObject(a->done, 0) == WAIT_OBJECT_0) stale.push_back(id);
        for (auto id : stale) self->Finish(id);
        return 0;
    }
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

void Genie::OnMessage(WPARAM op, LPARAM lid) {
    auto id = static_cast<unsigned long long>(lid);
    try {
        if (op == 1) {
            Start(id);
        } else if (op == 2 || op == 3) {
            auto it = m_anims.find(id);
            if (it == m_anims.end()) return;
            if (op == 3) SetEvent(it->second->done);
            Finish(id);
        }
    } catch (winrt::hresult_error const& e) {
        OutputDebugStringW((L"[halo] genie failed: " + std::wstring(e.message().c_str()) + L"\n").c_str());
        Finish(id);  // .ready stays unset: the mod falls back to its CPU animation
    }
}

void Genie::Start(unsigned long long id) {
    // The mod's snapshot: header + pixels in a named file mapping.
    HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, NameOf(id).c_str());
    if (!map) return;
    auto* base = static_cast<uint8_t const*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0));
    CloseHandle(map);
    if (!base) return;
    struct Unmap { uint8_t const* p; ~Unmap() { UnmapViewOfFile(p); } } unmap{base};
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(base, &mbi, sizeof(mbi));
    auto const& h = *reinterpret_cast<GenieHeader const*>(base);
    if (h.magic != kGenieMagic || h.version != 1 || h.kind > 3 || !h.width || !h.height || h.stride < h.width * 4 ||
        kGeniePixelOffset + static_cast<size_t>(h.stride) * h.height > mbi.RegionSize)
        return;
    auto anim = std::make_unique<Anim>();
    anim->ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, NameOf(id, L".ready").c_str());
    anim->done = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, NameOf(id, L".done").c_str());
    if (!anim->ready || !anim->done) return;

    // Snapshot -> one drawing surface that every strip samples.
    float W = static_cast<float>(h.width), H = static_cast<float>(h.height);
    auto surface = m_device.CreateDrawingSurface({W, H}, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                 winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
    {
        auto sinterop = surface.as<ICompositionDrawingSurfaceInterop>();
        winrt::com_ptr<ID2D1DeviceContext> dc;
        POINT off{};
        winrt::check_hresult(sinterop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &off));
        try {
            dc->Clear(D2D1::ColorF(0, 0.0f));
            winrt::com_ptr<ID2D1Bitmap> bitmap;
            D2D1_BITMAP_PROPERTIES props =
                D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            winrt::check_hresult(dc->CreateBitmap(D2D1::SizeU(h.width, h.height), base + kGeniePixelOffset, h.stride, &props,
                                                  bitmap.put()));
            D2D1_RECT_F dest = D2D1::RectF(static_cast<float>(off.x), static_cast<float>(off.y), off.x + W, off.y + H);
            dc->DrawBitmap(bitmap.get(), &dest, 1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr, nullptr);
        } catch (...) {
            sinterop->EndDraw();
            throw;
        }
        winrt::check_hresult(sinterop->EndDraw());
    }

    // The overlay covers the virtual desktop as it is now.
    m_virtual.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    m_virtual.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    m_virtual.right = m_virtual.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    m_virtual.bottom = m_virtual.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    float ox = static_cast<float>(m_virtual.left), oy = static_cast<float>(m_virtual.top);

    auto kind = static_cast<GenieKind>(h.kind);
    bool zoom = kind == GenieKind::Maximize || kind == GenieKind::Unmaximize;
    bool rising = kind == GenieKind::Restore;
    Rectf win{h.window.left - ox, h.window.top - oy, W, H};
    Rectf to{h.target.left - ox, h.target.top - oy, static_cast<float>(h.target.right - h.target.left),
             static_cast<float>(h.target.bottom - h.target.top)};
    int ms = zoom ? kZoomMs : kGenieMs;

    anim->root = m_comp.CreateContainerVisual();
    auto strips = anim->root.Children();
    auto batch = m_comp.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
    if (zoom) {
        // One visual: the snapshot swoops from the window's rect to the target rect.
        auto sprite = m_comp.CreateSpriteVisual();
        sprite.Size({W, H});
        auto brush = m_comp.CreateSurfaceBrush(surface);
        brush.Stretch(wuc::CompositionStretch::None);
        sprite.Brush(brush);
        sprite.Offset({win.x, win.y, 0});
        strips.InsertAtTop(sprite);
        auto ease = m_comp.CreateCubicBezierEasingFunction({0.2f, 0.7f}, {0.3f, 1.0f});
        auto move = m_comp.CreateVector3KeyFrameAnimation();
        move.Duration(std::chrono::milliseconds(ms));
        move.InsertKeyFrame(1.0f, {to.x, to.y, 0}, ease);
        sprite.StartAnimation(L"Offset", move);
        auto grow = m_comp.CreateVector3KeyFrameAnimation();
        grow.Duration(std::chrono::milliseconds(ms));
        grow.InsertKeyFrame(1.0f, {to.w / W, to.h / H, 1}, ease);
        sprite.StartAnimation(L"Scale", grow);
    } else {
        // The macOS lamp curve (Potassiumuncher's Direct2D genie engine in MacOS Minimize Animation,
        // MIT; the mod's CPU fallback has it as LampVertex), as strips of kStripPx rows of the snapshot (plus one row of overlap
        // so no seam opens). A strip's TransformMatrix is the affine map through its top-left,
        // top-right and bottom-left corners under the warp, so neighbours join exactly. DWM
        // evaluates the matrices every frame from one linearly animated time value g.t.
        auto g = m_comp.CreatePropertySet();
        for (auto name : {L"t", L"e", L"k", L"j", L"f", L"hh", L"s0", L"s1", L"u", L"k7"}) g.InsertScalar(name, 0.0f);
        auto drive = [&](wchar_t const* prop, std::wstring const& text) {
            auto expr = m_comp.CreateExpressionAnimation(text);
            expr.SetReferenceParameter(L"g", g);
            g.StartAnimation(prop, expr);
        };
        auto num = [](float v) { wchar_t b[32]; swprintf(b, 32, L"%.7g", v); return std::wstring(b); };
        float expand = to.y - win.y - win.h, dw = win.w - to.w;
        float cA = win.x - to.x, cB = win.x + win.w - to.x - to.w;
        drive(L"e", rising ? L"1 - (3*g.t*g.t - 2*g.t*g.t*g.t)" : L"3*g.t*g.t - 2*g.t*g.t*g.t");  // smoothstep
        drive(L"k", L"Clamp(g.e / 0.3, 0, 1)");
        drive(L"j", L"Clamp((g.e - 0.3) / 0.7, 0, 1)");
        drive(L"f", L"(" + num(to.y - win.y) + L") - (" + num(expand) + L") * (1 - g.k)");  // fullHeight
        drive(L"hh", L"g.f * (1 - g.j)");                                                   // height
        drive(L"s0", L"1 - g.j");
        drive(L"s1", L"1 - g.k");
        drive(L"u", L"g.hh / (g.f + 0.1) * g.k");
        drive(L"k7", L"g.k / 7");
        auto lin = m_comp.CreateScalarKeyFrameAnimation();
        lin.Duration(std::chrono::milliseconds(ms));
        lin.InsertKeyFrame(1.0f, 1.0f, m_comp.CreateLinearEasingFunction());
        g.StartAnimation(L"t", lin);

        auto X = [&](int tx, float ty) {  // Lamp()'s x for (tx, ty) as an expression over g
            std::wstring t = num(ty), r = L"(1 - " + t + L")";
            return L"(" + num(win.x) + L" + " + std::to_wstring(tx) + L" * (" + num(to.w) + L" + " + num(dw) +
                   L" * (g.s0 * " + r + L" + g.s1 * " + t + L")) + (" + num(to.x - win.x) + L") * (" + t +
                   L" * g.u + g.j) - Sin(6.2831853 * " + r + L" * g.s0) * (" + num(tx ? cB : cA) + L") * g.k7)";
        };
        auto Y = [&](float ty) { return L"(" + num(win.y) + L" + g.f - g.hh * (1 - " + num(ty) + L"))"; };
        for (int y0 = 0; y0 < static_cast<int>(h.height); y0 += kStripPx) {
            float rows = static_cast<float>(std::min<int>(kStripPx, h.height - y0));
            float overlap = (y0 + kStripPx < static_cast<int>(h.height)) ? 1.0f : 0.0f;
            auto sprite = m_comp.CreateSpriteVisual();
            sprite.Size({W, rows + overlap});
            auto brush = m_comp.CreateSurfaceBrush(surface);
            brush.Stretch(wuc::CompositionStretch::None);
            brush.HorizontalAlignmentRatio(0.0f);
            brush.VerticalAlignmentRatio(0.0f);
            brush.Offset({0.0f, -static_cast<float>(y0)});
            sprite.Brush(brush);
            strips.InsertAtTop(sprite);
            float t0 = y0 / H, t1 = (y0 + rows) / H;
            auto expr = m_comp.CreateExpressionAnimation(
                L"Matrix4x4((" + X(1, t0) + L" - " + X(0, t0) + L") / " + num(W) + L", 0, 0, 0, (" + X(0, t1) + L" - " +
                X(0, t0) + L") / " + num(rows) + L", (" + Y(t1) + L" - " + Y(t0) + L") / " + num(rows) +
                L", 0, 0, 0, 0, 1, 0, " + X(0, t0) + L", " + Y(t0) + L", 0, 1)");
            expr.SetReferenceParameter(L"g", g);
            sprite.StartAnimation(L"TransformMatrix", expr);
        }
    }
    batch.End();
    anim->needsRelease = kind != GenieKind::Minimize;
    batch.Completed([this, id](auto&&, auto&&) {
        // Back on the UI thread: tell the mod, and drop a minimize at once (its window is already gone).
        auto it = m_anims.find(id);
        if (it == m_anims.end()) return;
        SetEvent(it->second->done);
        if (it->second->needsRelease) SetTimer(m_overlay, kReleaseTimer, kReleaseTimeoutMs, nullptr);
        else Finish(id);
    });

    m_root.Children().InsertAtTop(anim->root);
    SetWindowPos(m_overlay, m_dock, m_virtual.left, m_virtual.top, m_virtual.right - m_virtual.left,
                 m_virtual.bottom - m_virtual.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    HANDLE ready = nullptr;
    DuplicateHandle(GetCurrentProcess(), anim->ready, GetCurrentProcess(), &ready, 0, FALSE, DUPLICATE_SAME_ACCESS);
    m_anims[id] = std::move(anim);
    // The first frame is on screen once DWM has processed the commit and composed it.
    m_comp.RequestCommitAsync().Completed([ready](auto&&, auto&&) {
        DwmFlush();
        SetEvent(ready);
        CloseHandle(ready);
    });
}

void Genie::Finish(unsigned long long id) {
    auto it = m_anims.find(id);
    if (it == m_anims.end()) return;
    if (it->second->root) m_root.Children().Remove(it->second->root);
    m_anims.erase(it);
    if (m_anims.empty()) {
        KillTimer(m_overlay, kReleaseTimer);
        ShowWindow(m_overlay, SW_HIDE);
    }
}
