#pragma once
// App Name Labels on Hover for Halo.
//
// Shows a small glass pill with the app's friendly name (e.g. "Google Chrome",
// "Visual Studio Code") ~10px above the currently hovered icon, with smooth
// compositor-driven fade-in/fade-out and lateral sliding.
//
// App names are resolved from:
// 1. Pinned .lnk shortcuts (IShellItem SIGDN_NORMALDISPLAY or IShellLink description)
// 2. Packaged Store apps (shell:AppsFolder\<AUMID> display name)
// 3. Unpinned Win32 running apps (FileDescription version info, or SHGetFileInfo display name)
//
// Drawn cleanly into a CompositionDrawingSurface using DirectWrite and Direct2D.

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.Graphics.DirectX.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifndef HALO_COMPOSITION_INTEROP_DEFINED
#define HALO_COMPOSITION_INTEROP_DEFINED
struct ICompositionDrawingSurfaceInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE BeginDraw(RECT const* updateRect, REFIID iid, void** updateObject, POINT* offset) = 0;
    virtual HRESULT STDMETHODCALLTYPE EndDraw() = 0;
    virtual HRESULT STDMETHODCALLTYPE Resize(SIZE size) = 0;
    virtual HRESULT STDMETHODCALLTYPE Scroll(RECT const* scrollRect, RECT const* clipRect, int dx, int dy) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResumeDraw() = 0;
    virtual HRESULT STDMETHODCALLTYPE SuspendDraw() = 0;
};
__CRT_UUID_DECL(ICompositionDrawingSurfaceInterop, 0xFD04E6E3, 0xFE0C, 0x4C3C, 0xAB, 0x19, 0xA0, 0x76, 0x01, 0xA5, 0x76, 0xEE)
#endif

namespace halo {

namespace wuc = winrt::Windows::UI::Composition;

// Resolves friendly display names for apps with an in-memory cache.
class AppNameResolver {
public:
    static AppNameResolver& Instance() {
        static AppNameResolver s_instance;
        return s_instance;
    }

    std::wstring Resolve(std::wstring const& pathOrKey, std::wstring const& exePath = {}) {
        if (pathOrKey.empty()) return {};

        auto it = m_cache.find(pathOrKey);
        if (it != m_cache.end()) return it->second;

        std::wstring name;
        if (pathOrKey.starts_with(L"shell:AppsFolder\\")) {
            name = ResolveStoreApp(pathOrKey);
        } else if (pathOrKey.ends_with(L".lnk")) {
            name = ResolveLnk(pathOrKey);
        } else if (!exePath.empty()) {
            name = ResolveExe(exePath);
        } else if (pathOrKey.ends_with(L".exe")) {
            name = ResolveExe(pathOrKey);
        } else {
            name = ResolveFallback(pathOrKey);
        }

        if (name.empty()) name = ResolveFallback(pathOrKey);
        m_cache[pathOrKey] = name;
        return name;
    }

private:
    AppNameResolver() = default;

    // Resolves packaged app names from shell:AppsFolder\<AUMID>
    std::wstring ResolveStoreApp(std::wstring const& parsingName) {
        winrt::com_ptr<IShellItem> item;
        HRESULT hr = SHCreateItemFromParsingName(parsingName.c_str(), nullptr,
                                                 IID_IShellItem, item.put_void());
        if (SUCCEEDED(hr) && item) {
            wchar_t* name = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name) {
                std::wstring result(name);
                CoTaskMemFree(name);
                return result;
            }
        }

        // Fallback: strip "shell:AppsFolder\" and extract the friendly segment of AUMID
        std::wstring aumid = parsingName.substr(17);
        auto bang = aumid.find(L'!');
        if (bang != std::wstring::npos) aumid = aumid.substr(0, bang);
        auto dot = aumid.find(L'.');
        if (dot != std::wstring::npos) aumid = aumid.substr(dot + 1);
        return aumid;
    }

    // Resolves .lnk shortcut display names
    std::wstring ResolveLnk(std::wstring const& lnkPath) {
        winrt::com_ptr<IShellItem> item;
        HRESULT hr = SHCreateItemFromParsingName(lnkPath.c_str(), nullptr,
                                                 IID_IShellItem, item.put_void());
        if (SUCCEEDED(hr) && item) {
            wchar_t* name = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name) {
                std::wstring result(name);
                CoTaskMemFree(name);
                // Strip .lnk extension if present
                if (result.size() > 4 && !_wcsicmp(result.c_str() + result.size() - 4, L".lnk")) {
                    result.resize(result.size() - 4);
                }
                return result;
            }
        }
        return StripPathAndExt(lnkPath);
    }

    // Resolves win32 executable display names via FileDescription or SHGetFileInfo
    std::wstring ResolveExe(std::wstring const& exePath) {
        // Special-case common system executables
        if (exePath.ends_with(L"explorer.exe")) return L"File Explorer";

        // Try FileDescription from VersionInfo (dynamically loaded from version.dll)
        std::wstring desc = QueryFileDescription(exePath);
        if (!desc.empty()) return desc;

        // Try SHGetFileInfo display name
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW(exePath.c_str(), 0, &sfi, sizeof(sfi), SHGFI_DISPLAYNAME)) {
            if (sfi.szDisplayName[0] != L'\0') {
                std::wstring disp(sfi.szDisplayName);
                if (disp.size() > 4 && !_wcsicmp(disp.c_str() + disp.size() - 4, L".exe")) {
                    disp.resize(disp.size() - 4);
                }
                return disp;
            }
        }

        return StripPathAndExt(exePath);
    }

    std::wstring QueryFileDescription(std::wstring const& path) {
        HMODULE hVer = LoadLibraryW(L"version.dll");
        if (!hVer) return {};

        typedef DWORD(WINAPI * PFN_GetFileVersionInfoSizeW)(LPCWSTR, LPDWORD);
        typedef BOOL(WINAPI * PFN_GetFileVersionInfoW)(LPCWSTR, DWORD, DWORD, LPVOID);
        typedef BOOL(WINAPI * PFN_VerQueryValueW)(LPCVOID, LPCWSTR, LPVOID*, PUINT);

        auto pGetSize = (PFN_GetFileVersionInfoSizeW)GetProcAddress(hVer, "GetFileVersionInfoSizeW");
        auto pGetInfo = (PFN_GetFileVersionInfoW)GetProcAddress(hVer, "GetFileVersionInfoW");
        auto pQuery = (PFN_VerQueryValueW)GetProcAddress(hVer, "VerQueryValueW");

        std::wstring result;
        if (pGetSize && pGetInfo && pQuery) {
            DWORD handle = 0;
            DWORD size = pGetSize(path.c_str(), &handle);
            if (size > 0) {
                std::vector<uint8_t> buf(size);
                if (pGetInfo(path.c_str(), 0, size, buf.data())) {
                    struct LANGANDCODEPAGE {
                        WORD wLanguage;
                        WORD wCodePage;
                    } *translations = nullptr;
                    UINT cbTranslate = 0;

                    if (pQuery(buf.data(), L"\\VarFileInfo\\Translation", (LPVOID*)&translations, &cbTranslate) &&
                        cbTranslate >= sizeof(LANGANDCODEPAGE)) {
                        wchar_t subBlock[64];
                        swprintf(subBlock, 64, L"\\StringFileInfo\\%04x%04x\\FileDescription",
                                 translations[0].wLanguage, translations[0].wCodePage);
                        wchar_t* desc = nullptr;
                        UINT descLen = 0;
                        if (pQuery(buf.data(), subBlock, (LPVOID*)&desc, &descLen) && desc && desc[0]) {
                            result = desc;
                        }
                    }
                    if (result.empty()) {
                        // Fallback to standard US English code page
                        wchar_t* desc = nullptr;
                        UINT descLen = 0;
                        if (pQuery(buf.data(), L"\\StringFileInfo\\040904b0\\FileDescription", (LPVOID*)&desc, &descLen) &&
                            desc && desc[0]) {
                            result = desc;
                        }
                    }
                }
            }
        }
        FreeLibrary(hVer);
        return result;
    }

    std::wstring StripPathAndExt(std::wstring const& path) {
        auto lastSlash = path.find_last_of(L"\\/");
        std::wstring name = (lastSlash == std::wstring::npos) ? path : path.substr(lastSlash + 1);
        auto lastDot = name.rfind(L'.');
        if (lastDot != std::wstring::npos) name.resize(lastDot);
        return name;
    }

    std::wstring ResolveFallback(std::wstring const& s) {
        return StripPathAndExt(s);
    }

    std::map<std::wstring, std::wstring> m_cache;
};

// DirectWrite and Direct2D glass pill text label.
class AppLabel {
public:
    AppLabel(wuc::Compositor const& comp,
             wuc::ContainerVisual const& parent,
             wuc::CompositionGraphicsDevice const& gdev,
             float scale = 1.0f)
        : m_comp(comp), m_parent(parent), m_gdev(gdev), m_scale(scale) {
        InitDirectWrite();
        InitVisuals();
    }

    ~AppLabel() {
        if (m_visual && m_parent) {
            m_parent.Children().Remove(m_visual);
        }
    }

    void SetScale(float scale) {
        if (std::abs(m_scale - scale) > 0.001f) {
            m_scale = scale;
            InitDirectWrite();
            m_currentText.clear();  // force redrawing on scale change
        }
    }

    // Displays or updates the label for an icon.
    // iconCenterX: physical X center of the magnified icon in parent visual coordinates.
    // iconTopY: physical Y top edge of the magnified icon.
    void Show(std::wstring const& text, float iconCenterX, float iconTopY) {
        if (text.empty()) {
            Hide();
            return;
        }

        // Text or dimensions changed: re-render surface
        if (text != m_currentText) {
            m_currentText = text;
            UpdateSurface(text);
        }

        // Target placement ~10px above icon top
        float targetW = m_visual.Size().x;
        float targetH = m_visual.Size().y;
        float targetX = iconCenterX - targetW / 2.0f;
        float targetY = iconTopY - m_scale * 10.0f - targetH;

        // Animate or reposition
        if (!m_visible) {
            // Fade in and set initial position
            m_visual.Offset({targetX, targetY, 0.0f});
            AnimateOpacity(1.0f, 120);
            m_visible = true;
        } else {
            // Already visible: smooth slide to new icon X
            AnimateOffsetX(targetX, 90);
            m_visual.Offset({m_visual.Offset().x, targetY, 0.0f});
        }
    }

    // Fades out and hides the label
    void Hide() {
        if (!m_visible) return;
        m_visible = false;
        AnimateOpacity(0.0f, 120);
    }

    bool IsVisible() const { return m_visible; }
    std::wstring CurrentText() const { return m_currentText; }

private:
    void InitDirectWrite() {
        if (!m_dwrite) {
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                reinterpret_cast<IUnknown**>(m_dwrite.put()));
        }
        if (m_dwrite) {
            // Use Segoe UI Variable Display if available, fallback to Segoe UI
            float fontSize = 11.5f * m_scale;
            HRESULT hr = m_dwrite->CreateTextFormat(
                L"Segoe UI Variable Display", nullptr,
                DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, fontSize, L"en-us", m_format.put());
            if (FAILED(hr)) {
                m_dwrite->CreateTextFormat(
                    L"Segoe UI", nullptr,
                    DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, fontSize, L"en-us", m_format.put());
            }
            if (m_format) {
                m_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }
    }

    void InitVisuals() {
        m_visual = m_comp.CreateSpriteVisual();
        m_visual.Opacity(0.0f);
        m_visual.Size({0.0f, 0.0f});
        m_parent.Children().InsertAtTop(m_visual);

        m_easeOut = m_comp.CreateCubicBezierEasingFunction({0.2f, 0.9f}, {0.3f, 1.0f});
    }

    void UpdateSurface(std::wstring const& text) {
        if (!m_dwrite || !m_format || !m_gdev) return;

        // Measure text
        winrt::com_ptr<IDWriteTextLayout> layout;
        winrt::check_hresult(m_dwrite->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.length()), m_format.get(),
            800.0f * m_scale, 200.0f * m_scale, layout.put()));

        DWRITE_TEXT_METRICS tm{};
        layout->GetMetrics(&tm);

        // Padding: 10px horizontal, 4px vertical
        float padX = 10.0f * m_scale;
        float padY = 4.0f * m_scale;
        float pillW = std::ceil(tm.widthIncludingTrailingWhitespace + 2 * padX);
        float pillH = std::ceil(tm.height + 2 * padY);
        float radius = pillH / 2.0f;

        // Drawing surface
        auto surface = m_gdev.CreateDrawingSurface(
            {pillW, pillH},
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);

        auto interop = surface.as<ICompositionDrawingSurfaceInterop>();
        winrt::com_ptr<ID2D1DeviceContext> dc;
        POINT offset{};
        winrt::check_hresult(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext),
                                                dc.put_void(), &offset));
        try {
            dc->Clear(D2D1::ColorF(0, 0.0f));

            // Rounded glass pill background
            D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(
                D2D1::RectF(offset.x + 0.5f, offset.y + 0.5f,
                            offset.x + pillW - 0.5f, offset.y + pillH - 0.5f),
                radius, radius);

            // Dark frosted glass fill: #16161A at 88% alpha
            winrt::com_ptr<ID2D1SolidColorBrush> bgBrush;
            dc->CreateSolidColorBrush(D2D1::ColorF(0x16161A, 0.88f), bgBrush.put());
            dc->FillRoundedRectangle(rrect, bgBrush.get());

            // 1px rim stroke: white at 18% alpha
            winrt::com_ptr<ID2D1SolidColorBrush> rimBrush;
            dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f), rimBrush.put());
            dc->DrawRoundedRectangle(rrect, rimBrush.get(), 1.0f);

            // Text fill: crisp white at 95% alpha
            winrt::com_ptr<ID2D1SolidColorBrush> textBrush;
            dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f), textBrush.put());
            D2D1_POINT_2F textOrigin = D2D1::Point2F(static_cast<float>(offset.x) + padX,
                                                    static_cast<float>(offset.y) + padY);
            dc->DrawTextLayout(textOrigin, layout.get(), textBrush.get());
        } catch (...) {
            interop->EndDraw();
            throw;
        }
        winrt::check_hresult(interop->EndDraw());

        auto brush = m_comp.CreateSurfaceBrush(surface);
        brush.Stretch(wuc::CompositionStretch::None);
        m_visual.Brush(brush);
        m_visual.Size({pillW, pillH});
    }

    void AnimateOpacity(float target, int ms) {
        auto a = m_comp.CreateScalarKeyFrameAnimation();
        a.InsertKeyFrame(1.0f, target, m_easeOut);
        a.Duration(std::chrono::milliseconds(ms));
        m_visual.StartAnimation(L"Opacity", a);
    }

    void AnimateOffsetX(float targetX, int ms) {
        auto a = m_comp.CreateScalarKeyFrameAnimation();
        a.InsertKeyFrame(1.0f, targetX, m_easeOut);
        a.Duration(std::chrono::milliseconds(ms));
        m_visual.StartAnimation(L"Offset.X", a);
    }

    wuc::Compositor m_comp{nullptr};
    wuc::ContainerVisual m_parent{nullptr};
    wuc::CompositionGraphicsDevice m_gdev{nullptr};
    wuc::SpriteVisual m_visual{nullptr};
    wuc::CubicBezierEasingFunction m_easeOut{nullptr};

    winrt::com_ptr<IDWriteFactory> m_dwrite;
    winrt::com_ptr<IDWriteTextFormat> m_format;

    float m_scale = 1.0f;
    bool m_visible = false;
    std::wstring m_currentText;
};

}  // namespace halo
