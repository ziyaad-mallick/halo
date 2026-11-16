#define SECURITY_WIN32
#include "launcher.h"
#include "motion.h"
#include "theme.h"
#include "tokens.h"
#include "shell_launch.h"

#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.UI.h>
#include <dwmapi.h>
#include <powrprof.h>
#include <shlobj.h>
#include <shellapi.h>
#include <sddl.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <memory>
#include <thread>

// <security.h> would declare this, but its ntsecapi.h redefines an enumerator (Network) that devicetopology.h (via
// services.h) already has, so the one function is declared here. It lives in secur32, which build.sh links.
extern "C" BOOLEAN WINAPI GetUserNameExW(int format, LPWSTR name, PULONG size);
constexpr int kNameDisplay = 3;  // EXTENDED_NAME_FORMAT::NameDisplay

// Interop interfaces missing from the MinGW SDK headers (layouts per the Windows SDK).
struct ICompositorDesktopInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateDesktopWindowTarget(HWND hwnd, BOOL isTopmost, void** target) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnsureOnThread(DWORD threadId) = 0;
};
__CRT_UUID_DECL(ICompositorDesktopInterop, 0x29E691FA, 0x4567, 0x4DCA, 0xB3, 0x19, 0xD0, 0xF2, 0x07, 0xEB, 0x68, 0x07)
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
namespace {

// Soft drop shadow for a rounded rect. The sprite has no brush of its own (only the shadow draws), so a
// translucent glass above it never shows the white mask.
wuc::SpriteVisual MakeShadow(wuc::Compositor const& comp, wuc::CompositionGraphicsDevice const& gdev, float wPx, float hPx,
                             float radPx, float blur, float dy, theme::Argb col) {
    if (col.a == 0 || blur <= 0) return nullptr;
    try {
        auto surface = gdev.CreateDrawingSurface({wPx, hPx}, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                 winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
        auto interop = surface.as<ICompositionDrawingSurfaceInterop>();
        winrt::com_ptr<ID2D1DeviceContext> dc;
        POINT off{};
        if (FAILED(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &off))) return nullptr;
        dc->Clear(D2D1::ColorF(0, 0.0f));
        winrt::com_ptr<ID2D1SolidColorBrush> b;
        dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), b.put());
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(off.x + 0.0f, off.y + 0.0f, off.x + wPx, off.y + hPx), radPx, radPx), b.get());
        interop->EndDraw();
        auto mask = comp.CreateSurfaceBrush(surface);
        mask.Stretch(wuc::CompositionStretch::Fill);
        auto sh = comp.CreateDropShadow();
        sh.BlurRadius(blur);
        sh.Color(theme::C(col));
        sh.Offset({0, dy, 0});
        sh.Mask(mask);
        auto v = comp.CreateSpriteVisual();
        v.Size({wPx, hPx});
        v.Shadow(sh);
        return v;
    } catch (...) { return nullptr; }
}

// The panel's shadow for HALO 2.0, baked once: concentric rounded rects at a quarter of the size, stretched up (the
// stretch is the blur). Moving it each frame is a plain texture draw; a CompositionDropShadow is re-blurred by the GPU.
wuc::SpriteVisual MakeBakedShadow(wuc::Compositor const& comp, wuc::CompositionGraphicsDevice const& gdev, float wPx, float hPx,
                                  float radPx, float blur, float dy, theme::Argb col) {
    if (col.a == 0 || blur <= 0) return nullptr;
    try {
        constexpr float q = 0.25f;
        const float m = std::ceil(blur + dy), fw = wPx + 2 * m, fh = hPx + 2 * m;  // the panel plus room for the blur on every side
        auto surface = gdev.CreateDrawingSurface({std::ceil(fw * q), std::ceil(fh * q)},
                                                 winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                 winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
        auto interop = surface.as<ICompositionDrawingSurfaceInterop>();
        winrt::com_ptr<ID2D1DeviceContext> dc;
        POINT off{};
        if (FAILED(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &off))) return nullptr;
        dc->Clear(D2D1::ColorF(0, 0.0f));
        winrt::com_ptr<ID2D1SolidColorBrush> b;
        dc->CreateSolidColorBrush(D2D1::ColorF(col.r / 255.0f, col.g / 255.0f, col.b / 255.0f, 1), b.put());
        constexpr int layers = 16;
        const float a = col.a / 255.0f, each = 1 - std::pow(1 - a, 1.0f / layers);  // the layers add up to `a` in the middle
        b->SetOpacity(each);
        for (int i = 0; i < layers; i++) {
            const float e = blur * 0.85f * (1 - static_cast<float>(i) / layers);
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(off.x + (m - e) * q, off.y + (m + dy - e) * q,
                                                                   off.x + (m + wPx + e) * q, off.y + (m + dy + hPx + e) * q),
                                                       (radPx + e) * q, (radPx + e) * q), b.get());
        }
        interop->EndDraw();
        auto brush = comp.CreateSurfaceBrush(surface);
        brush.Stretch(wuc::CompositionStretch::Fill);
        brush.BitmapInterpolationMode(wuc::CompositionBitmapInterpolationMode::Linear);
        auto v = comp.CreateSpriteVisual();
        v.Size({fw, fh});
        v.Offset({-m, -m, 0});
        v.Brush(brush);
        return v;
    } catch (...) { return nullptr; }
}

// The slow reads of the right column (the hotspot), posted back from a thread of their own.
struct LateRead {
    bool hotspotAvail = false, hotspotOn = false;
};

// Muted text: theme::Muted() (the palette's `dim` is 3.6:1 on the light glass, so light uses the soft ink). Both modes use it now.
D2D1_COLOR_F MutedOf(bool) { return theme::Muted(); }

// Layout, logical px.
constexpr float kPad = ui::kS6;
constexpr float kLabelH = 16;
constexpr float kIconSize = ui::kAppIcon;
constexpr float kSearchTop = ui::kS6, kSearchH = ui::kSearchH;
constexpr float kSearchLeft = ui::kS4 + ui::kIcon + ui::kS2, kSearchRight = ui::kS4;  // the field's text: after the glyph, and its right padding
constexpr int kCaretBlinkMs = 530;
constexpr float kMenuDim = 0.45f;  // how far the panel dims behind the open Power menu
constexpr float kAppRowH = start::tk::kAppCellH;  // an app cell: icon and name (the same grid as 2.0)
constexpr float kResultRowH = 36, kResultIcon = 24;
constexpr float kBottomH = 64;  // classic's footer: the hairline, then 12, the 36 px buttons and 16
constexpr float kAvatar = ui::kAvatar;
constexpr float kMenuW = 240;  // wide enough for "Use Windows taskbar" and its hint
constexpr float kMenuRowH = ui::kMenuRowH;
constexpr float kMenuPad = ui::kS2;
constexpr float kMinWidth = 560, kMaxWidth = 720, kWidth = 640, kScreenMargin = 24;
constexpr float kTop = 8;         // room above the panel for the spring's overshoot
constexpr int kH2Apps = 12, kH2Files = 6, kH2AppCols = 6;  // the 2.0 middle pane: 6 x 2 apps, 2 x 3 files
constexpr int kMaxFiles = 12, kFileCols = 6, kKeepApps = 12, kKeepFiles = 60, kMaxResults = 8, kMaxQuery = 120;
constexpr UINT_PTR kRefreshTimer = 1, kClockTimer = 2, kCaretTimer = 3, kServicesTimer = 4, kOpenTimer = 5, kRehoverTimer = 6, kWaveOffTimer = 7, kSettleTimer = 8;  // kClockTimer: halo2, re-armed to each minute boundary
constexpr int kQuickTiles = 10;  // Airplane, Hotspot, Do not disturb, Night light, Dark mode, Microphone, Battery saver, Screenshot, Cast, Project
constexpr int kListBuffer = 4, kScrollMs = 180, kScrollSpringMs = 85;  // rows drawn past each end of the list; the scroll slide
constexpr UINT kMsgDeferred = WM_APP + 3;  // run the queued service calls (after an optimistic redraw)
constexpr UINT kMsgAppsFolder = WM_APP + 1;  // lParam: a Launcher::Found* from the Apps folder thread, owned by the panel
constexpr UINT kMsgRecent = WM_APP + 2;  // result stays in shared state; no owned message payload
// halo2: service notifications for the right column (the Control Center's own numbering is its own window's business).
constexpr UINT kMsgAudio = WM_APP + 10, kMsgRadios = WM_APP + 11, kMsgBright = WM_APP + 12, kMsgMedia = WM_APP + 13,
               kMsgWifi = WM_APP + 14, kMsgBt = WM_APP + 15, kMsgUsage = WM_APP + 16, kMsgMic = WM_APP + 17, kMsgLate = WM_APP + 18, kMsgOutputs = WM_APP + 19;
// Icon motion, matched to the dock: hover swells and lifts on a spring, a press squashes, a click bounces.
constexpr float kHoverScale = 1.25f, kHoverLift = 6, kPressScale = 0.94f, kBounceRise = 8;  // (an icon's own squash; a plate or tile uses ui::kPressScale)
constexpr float kHoverDamping = 0.6f;
constexpr int kHoverPeriodMs = 45, kBounceMs = 420;

inline winrt::Windows::UI::Color Col(D2D1_COLOR_F c) {
    auto b = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return winrt::Windows::UI::ColorHelper::FromArgb(b(c.a), b(c.r), b(c.g), b(c.b));
}

constexpr wchar_t const* kMenuItems[] = {L"Sleep", L"Restart", L"Shut down", L"Lock", L"Sign out", L"Use Windows taskbar"};
constexpr int kMenuCount = 6;
constexpr int kMenuTaskbar = 5;                      // not a power action: hands the screen back to Windows' own taskbar
constexpr wchar_t kMenuTaskbarHint[] = L"Win+Alt+T";  // the guardian's toggle

void Log(std::wstring const& msg) {
    auto line = L"[halo] launcher: " + msg + L"\n";
    OutputDebugStringW(line.c_str());
    fputws(line.c_str(), stderr);
    fflush(stderr);
}

bool EnvIs(wchar_t const* name) {
    wchar_t v[8] = {};
    return GetEnvironmentVariableW(name, v, 8) && v[0] == L'1';
}

D2D1_RECT_F R(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }

// When a recent file was last used, as a row's caption: Today, Yesterday, the weekday within a week, else the date.
std::wstring FileCaption(ULONGLONG ft) {
    if (!ft) return {};
    FILETIME f{static_cast<DWORD>(ft & 0xFFFFFFFF), static_cast<DWORD>(ft >> 32)};
    SYSTEMTIME utc{}, loc{}, now{};
    if (!FileTimeToSystemTime(&f, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &loc)) return {};
    GetLocalTime(&now);
    const long days = start::DaysFromCivil(now.wYear, now.wMonth, now.wDay) - start::DaysFromCivil(loc.wYear, loc.wMonth, loc.wDay);
    if (days <= 0) return L"Today";
    if (days == 1) return L"Yesterday";
    wchar_t buf[48] = {};
    if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &loc, days < 7 ? L"ddd" : L"d MMM", buf, 48, nullptr)) return {};
    return buf;
}
bool In(D2D1_RECT_F const& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring Utf16(std::string const& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Utf8(std::wstring const& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring RecentFile() {
    wchar_t over[MAX_PATH];  // HALO_TEST_RECENT: a stand-in file, so tests leave the real list alone
    if (GetEnvironmentVariableW(L"HALO_TEST_RECENT", over, MAX_PATH)) return over;
    wchar_t dir[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return {};
    std::wstring d = std::wstring(dir) + L"\\Halo";
    CreateDirectoryW(d.c_str(), nullptr);
    return d + L"\\recent.txt";
}
// A picture file as premultiplied BGRA, via WIC.
bool LoadImageFile(std::wstring const& path, std::vector<uint8_t>& bgra, int& px) {
    winrt::com_ptr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                                factory.put_void())))
        return false;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    winrt::com_ptr<IWICFormatConverter> conv;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                  decoder.put())) ||
        FAILED(decoder->GetFrame(0, frame.put())) || FAILED(factory->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (!w || w != h) return false;
    bgra.resize(static_cast<size_t>(w) * h * 4);
    px = static_cast<int>(w);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(bgra.size()), bgra.data()));
}

// The account picture Windows keeps under C:\Users\Public\AccountPictures\<SID>\ (the per-user
// %APPDATA%\Microsoft\Windows\AccountPictures only holds a .accountpicture-ms blob). Picks the
// smallest size that is at least `want` px, else the largest.
std::wstring AccountPicture(int want) {
    std::wstring sid;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        DWORD n = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &n);
        std::vector<uint8_t> buf(n);
        if (n && GetTokenInformation(token, TokenUser, buf.data(), n, &n)) {
            wchar_t* s = nullptr;
            if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &s)) {
                sid = s;
                LocalFree(s);
            }
        }
        CloseHandle(token);
    }
    wchar_t* pub = nullptr;
    if (sid.empty() || FAILED(SHGetKnownFolderPath(FOLDERID_Public, 0, nullptr, &pub))) return {};
    std::wstring dir = std::wstring(pub) + L"\\AccountPictures\\" + sid + L"\\";
    CoTaskMemFree(pub);
    std::wstring best;
    int bestPx = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*Image*.*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    do {
        std::wstring name = fd.cFileName;
        auto at = name.find(L"-Image");
        if (at == std::wstring::npos) continue;
        int px = _wtoi(name.c_str() + at + 6);
        if (px <= 0) continue;
        bool better = !bestPx || (bestPx < want ? px > bestPx : (px >= want && px < bestPx));
        if (better) {
            best = dir + name;
            bestPx = px;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best;
}

bool EnableShutdownPrivilege() {
    HANDLE t = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &t)) return false;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid) &&
              AdjustTokenPrivileges(t, FALSE, &tp, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
    CloseHandle(t);
    return ok;
}

using winrt::Windows::Foundation::IReference;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

// How a lower-cased name matches a lower-cased query: 0 starts with it, 1 a word in it does, 2 it contains it, -1 no match.
int MatchRank(std::wstring const& name, std::wstring const& q) {
    size_t at = name.find(q);
    if (at == std::wstring::npos) return -1;
    if (at == 0) return 0;
    for (; at != std::wstring::npos; at = name.find(q, at + 1))
        if (!std::iswalnum(name[at - 1])) return 1;
    return 2;
}

// Every .lnk under a Start-menu Programs folder, as (name without extension, path).
void ScanLinks(std::wstring const& dir, std::vector<std::pair<std::wstring, std::wstring>>& out, int depth) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (name != L"." && name != L".." && depth < 4 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                ScanLinks(dir + L"\\" + name, out, depth + 1);
        } else if (name.size() > 4 && !_wcsicmp(name.c_str() + name.size() - 4, L".lnk")) {
            out.emplace_back(name.substr(0, name.size() - 4), dir + L"\\" + name);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Every entry of the Apps folder, which is what Windows' own All apps list shows (Store apps included), as
// (name, shell:AppsFolder\<id>). Documents and web links that installers put in the Start menu are left out.
void ScanAppsFolder(Launcher::Found& out) {
    winrt::com_ptr<IShellItem> folder;
    winrt::com_ptr<IEnumShellItems> items;
    if (FAILED(SHCreateItemFromParsingName(L"shell:AppsFolder", nullptr, IID_PPV_ARGS(folder.put()))) ||
        FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(items.put()))))
        return;
    for (winrt::com_ptr<IShellItem> item; items->Next(1, item.put(), nullptr) == S_OK; item = nullptr) {
        wchar_t *name = nullptr, *id = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) &&
            SUCCEEDED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &id)) && name[0] && id[0]) {
            std::wstring lid = Lower(id);
            bool doc = lid.starts_with(L"http");
            for (auto ext : {L".txt", L".pdf", L".chm", L".htm", L".html", L".url", L".rtf"}) doc = doc || lid.ends_with(ext);
            if (!doc) out.emplace_back(name, std::wstring(L"shell:AppsFolder\\") + id);
        }
        CoTaskMemFree(name);
        CoTaskMemFree(id);
    }
}

// The Settings pages search finds: the name Settings gives the page, other words people look for it by, its URI.
struct SettingsPage {
    const wchar_t* name;
    const wchar_t* words;
    const wchar_t* uri;
};
constexpr SettingsPage kSettingsPages[] = {
    {L"Settings", L"windows system settings", L"ms-settings:"},
    {L"Wi-Fi", L"wifi wireless internet network", L"ms-settings:network-wifi"},
    {L"Ethernet", L"wired network cable", L"ms-settings:network-ethernet"},
    {L"VPN", L"network", L"ms-settings:network-vpn"},
    {L"Mobile hotspot", L"tethering share internet network", L"ms-settings:network-mobilehotspot"},
    {L"Airplane mode", L"flight mode", L"ms-settings:network-airplanemode"},
    {L"Proxy", L"network", L"ms-settings:network-proxy"},
    {L"Bluetooth & devices", L"bluetooth pair headphones earbuds devices", L"ms-settings:bluetooth"},
    {L"Printers & scanners", L"printer scanner print", L"ms-settings:printers"},
    {L"Mouse", L"pointer cursor scroll", L"ms-settings:mousetouchpad"},
    {L"Touchpad", L"trackpad gestures scroll", L"ms-settings:devices-touchpad"},
    {L"Display", L"screen brightness resolution scale monitor", L"ms-settings:display"},
    {L"Night light", L"blue light warm", L"ms-settings:nightlight"},
    {L"Graphics", L"gpu", L"ms-settings:display-advancedgraphics"},
    {L"Sound", L"audio volume speakers microphone output input", L"ms-settings:sound"},
    {L"Notifications", L"do not disturb alerts", L"ms-settings:notifications"},
    {L"Power & battery", L"battery sleep energy saver", L"ms-settings:powersleep"},
    {L"Storage", L"disk space drive cleanup", L"ms-settings:storagesense"},
    {L"Multitasking", L"snap windows alt tab", L"ms-settings:multitasking"},
    {L"Clipboard", L"copy paste history", L"ms-settings:clipboard"},
    {L"About", L"pc name specs system info windows version", L"ms-settings:about"},
    {L"Recovery", L"reset this pc restore", L"ms-settings:recovery"},
    {L"Activation", L"product key license", L"ms-settings:activation"},
    {L"Background", L"wallpaper desktop picture", L"ms-settings:personalization-background"},
    {L"Colors", L"dark mode light mode accent color theme", L"ms-settings:colors"},
    {L"Themes", L"theme", L"ms-settings:themes"},
    {L"Lock screen", L"lockscreen", L"ms-settings:lockscreen"},
    {L"Fonts", L"font typeface", L"ms-settings:fonts"},
    {L"Taskbar", L"tray", L"ms-settings:taskbar"},
    {L"Installed apps", L"uninstall programs apps features remove", L"ms-settings:appsfeatures"},
    {L"Default apps", L"open with browser default", L"ms-settings:defaultapps"},
    {L"Startup apps", L"startup login programs", L"ms-settings:startupapps"},
    {L"Optional features", L"features", L"ms-settings:optionalfeatures"},
    {L"Your info", L"account profile picture", L"ms-settings:yourinfo"},
    {L"Sign-in options", L"password pin windows hello fingerprint face", L"ms-settings:signinoptions"},
    {L"Date & time", L"clock time zone timezone", L"ms-settings:dateandtime"},
    {L"Language & region", L"language region locale", L"ms-settings:regionlanguage"},
    {L"Typing", L"keyboard autocorrect", L"ms-settings:typing"},
    {L"Gaming", L"game mode", L"ms-settings:gaming-gamemode"},
    {L"Accessibility", L"ease of access", L"ms-settings:easeofaccess"},
    {L"Text size", L"font size bigger text", L"ms-settings:easeofaccess-display"},
    {L"Privacy & security", L"privacy permissions", L"ms-settings:privacy"},
    {L"Camera privacy", L"webcam permissions", L"ms-settings:privacy-webcam"},
    {L"Microphone privacy", L"mic permissions", L"ms-settings:privacy-microphone"},
    {L"Location", L"gps location services privacy", L"ms-settings:privacy-location"},
    {L"Windows Update", L"updates update upgrade", L"ms-settings:windowsupdate"},
};
// The Settings app: the icon of every Settings page in the results.
constexpr wchar_t kSettingsApp[] =
    L"shell:AppsFolder\\windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel";

// Launch errors are visible without blocking the shell's one UI thread.
void OpenQuietly(std::wstring const& target) {
    launch::Open(target);
}

IReference<float3> Ref3(float3 v) { return v; }  // spring animations take IReference<float3>


}  // namespace

Launcher::Launcher(HINSTANCE inst, wuc::Compositor const& comp, wuc::CompositionGraphicsDevice const& gdev,
                   LauncherHost host)
    : m_comp(comp), m_gdev(gdev), m_host(std::move(host)) {
    m_dryRun = EnvIs(L"HALO_TEST_POWER_DRYRUN") || EnvIs(L"HALO_TEST");
    winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                             reinterpret_cast<IUnknown**>(m_dwrite.put())));
    m_easeOut = m_comp.CreateCubicBezierEasingFunction({0.2f, 0.0f}, {0.0f, 1.0f});
    LoadMru();
    CreateHwnd(inst);
    Prefetch();
    Log(L"ready, " + std::to_wstring(m_mru.size()) + L" recent apps, " + std::to_wstring(m_index.size()) + L" start-menu apps" +
        (m_dryRun ? L", power actions are DRY RUN" : L""));
}

Launcher::~Launcher() {
    {
        std::lock_guard lock(m_recentRefresh->mutex);
        m_recentRefresh->notify = nullptr;
    }
    if (m_dim) DestroyWindow(m_dim);
    if (m_hwnd) DestroyWindow(m_hwnd);
}

// [start] dim_desktop: a click-through black sheet over the monitor, under the panel (which Place() raises above it).
void Launcher::ShowDim(bool on) {
    if (!on) {
        if (m_dim) ShowWindow(m_dim, SW_HIDE);
        return;
    }
    if (!m_dim) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"HaloLauncherDim";
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        RegisterClassExW(&wc);
        m_dim = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                                wc.lpszClassName, L"Halo Launcher Dim", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
        if (!m_dim) return;
        SetLayeredWindowAttributes(m_dim, 0, theme::Dark() ? 64 : 40, LWA_ALPHA);
    }
    const RECT& r = m_anchor.monitor;
    SetWindowPos(m_dim, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void Launcher::CreateHwnd(HINSTANCE inst) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"HaloLauncherWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    // Activatable: the panel is the search UI and takes the keyboard.
    m_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"Halo Launcher",
                             WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, inst, this);
    if (!m_hwnd) throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));
    BOOL on = TRUE;
    DwmSetWindowAttribute(m_hwnd, 17 /* DWMWA_USE_HOSTBACKDROPBRUSH */, &on, sizeof(on));
    auto interop = m_comp.as<ICompositorDesktopInterop>();
    winrt::check_hresult(interop->CreateDesktopWindowTarget(m_hwnd, TRUE, winrt::put_abi(m_target)));
    m_root = m_comp.CreateContainerVisual();
    m_root.RelativeSizeAdjustment({1, 1});
    m_target.Root(m_root);
}

// ---- Content --------------------------------------------------------------------------------

void Launcher::LoadMru() {
    m_mru.clear();
    auto path = RecentFile();
    if (path.empty()) return;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string data(GetFileSize(f, nullptr), '\0');
    DWORD got = 0;
    ReadFile(f, data.data(), static_cast<DWORD>(data.size()), &got, nullptr);
    CloseHandle(f);
    data.resize(got);
    size_t at = 0;
    while (at < data.size() && m_mru.size() < kKeepApps) {
        size_t end = data.find('\n', at);
        if (end == std::string::npos) end = data.size();
        std::string line = data.substr(at, end - at);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) m_mru.push_back(Utf16(line));
        at = end + 1;
    }
}

void Launcher::SaveMru() const {
    auto path = RecentFile();
    if (path.empty()) return;
    std::string data;
    for (auto const& t : m_mru) data += Utf8(t) + "\n";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &put, nullptr);
    CloseHandle(f);
}

void Launcher::Note(std::wstring const& target) {
    if (target.empty()) return;
    if (!m_mru.empty() && !_wcsicmp(m_mru.front().c_str(), target.c_str())) return;
    std::erase_if(m_mru, [&](std::wstring const& t) { return !_wcsicmp(t.c_str(), target.c_str()); });
    m_mru.insert(m_mru.begin(), target);
    if (m_mru.size() > kKeepApps) m_mru.resize(kKeepApps);
    SaveMru();
}

// The newest non-folder files in the shell's Recent folder, from their shortcuts: the panel shows the
// first kMaxFiles, search looks through all kKeepFiles.
std::vector<Launcher::FileEntry> Launcher::LoadFiles() {
    std::vector<FileEntry> files;
    std::wstring folder;
    if (wchar_t over[MAX_PATH]; GetEnvironmentVariableW(L"HALO_TEST_RECENT_DIR", over, MAX_PATH)) {  // tools/start_preview: a folder of stand-in shortcuts, never the real Recent
        folder = std::wstring(over) + L"\\";
    } else {
        wchar_t* dir = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Recent, 0, nullptr, &dir))) return files;
        folder = std::wstring(dir) + L"\\";
        CoTaskMemFree(dir);
    }

    struct Link {
        std::wstring path;
        ULONGLONG time;
    };
    std::vector<Link> links;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((folder + L"*.lnk").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            links.push_back({folder + fd.cFileName,
                             (static_cast<ULONGLONG>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime});
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(links.begin(), links.end(), [](Link const& a, Link const& b) { return a.time > b.time; });

    winrt::com_ptr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, link.put_void()))) return files;
    auto file = link.as<IPersistFile>();
    size_t looked = 0;
    for (auto const& l : links) {
        if (files.size() >= kKeepFiles || ++looked > 160) break;
        wchar_t target[MAX_PATH] = {};
        if (FAILED(file->Load(l.path.c_str(), STGM_READ)) ||
            FAILED(link->GetPath(target, MAX_PATH, nullptr, SLGP_RAWPATH)) || !target[0])
            continue;
        if (target[0] == L'\\' && target[1] == L'\\') continue;  // network: stat could hang
        if (target[1] == L':') {  // so could a mapped, removable or optical drive
            const wchar_t root[4] = {target[0], L':', L'\\', 0};
            if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        }
        DWORD attrs = GetFileAttributesW(target);
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring path = target;
        if (std::any_of(files.begin(), files.end(), [&](FileEntry const& e) { return !_wcsicmp(e.path.c_str(), path.c_str()); }))
            continue;
        files.push_back({path.substr(path.find_last_of(L'\\') + 1), path, l.time});
    }
    return files;
}

// Every app, once: the Start-menu shortcuts (per user, then all users) now, then what the Apps folder has that
// they don't (Store apps such as Settings). What search and All apps look through.
void Launcher::BuildIndex() {
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    Found found;
    if (wchar_t over[MAX_PATH]; GetEnvironmentVariableW(L"HALO_TEST_APPS", over, MAX_PATH)) {  // tools/start_preview: stand-in apps, not the Start menu
        if (HANDLE f = CreateFileW(over, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr); f != INVALID_HANDLE_VALUE) {
            std::string data(GetFileSize(f, nullptr), '\0');
            DWORD got = 0;
            ReadFile(f, data.data(), static_cast<DWORD>(data.size()), &got, nullptr);
            CloseHandle(f);
            data.resize(got);
            size_t at = 0;
            while (at < data.size()) {
                size_t end = data.find('\n', at);
                if (end == std::string::npos) end = data.size();
                std::wstring line = Utf16(data.substr(at, end - at));
                while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
                if (auto bar = line.find(L'|'); bar != std::wstring::npos) found.push_back({line.substr(0, bar), line.substr(bar + 1)});
                at = end + 1;
            }
        }
        m_index.clear();
        AddToIndex(found);
        m_indexed = true;
        return;
    }
    for (auto const& id : {FOLDERID_Programs, FOLDERID_CommonPrograms}) {
        wchar_t* dir = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &dir))) ScanLinks(dir, found, 0);
        CoTaskMemFree(dir);
    }
    m_index.clear();
    AddToIndex(found);
    m_indexed = true;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    Log(L"app index: " + std::to_wstring(m_index.size()) + L" start-menu apps in " +
        std::to_wstring((t1.QuadPart - t0.QuadPart) * 10000 / fq.QuadPart / 10.0) + L" ms; reading the Apps folder");
    // The Apps folder takes the best part of a second to list, so it is read on a thread of its own and merged
    // when it posts back (kMsgAppsFolder).
    std::thread([hwnd = m_hwnd] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto* more = new Found;
        ScanAppsFolder(*more);
        if (SUCCEEDED(co)) CoUninitialize();
        if (!PostMessageW(hwnd, kMsgAppsFolder, 0, reinterpret_cast<LPARAM>(more))) delete more;
    }).detach();
}

// Halo runs for days, so the index built at startup goes stale: an app installed since is missing, one removed
// still listed. This rescans every source on a thread (Start-menu shortcuts first, so they win a name over the
// Apps folder, as in BuildIndex) and posts the whole result for kMsgAppsFolder to swap in (wParam 1).
void Launcher::RefreshIndex() {
    if (GetEnvironmentVariableW(L"HALO_TEST_APPS", nullptr, 0)) return;  // tools/start_preview owns the index
    const ULONGLONG now = GetTickCount64();
    if (m_rescanAt && now - m_rescanAt < 10000) return;                 // opening Start twice in a row
    if (m_rescanning.exchange(true)) return;
    m_rescanAt = now;
    std::thread([hwnd = m_hwnd] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto* all = new Found;
        for (auto const& id : {FOLDERID_Programs, FOLDERID_CommonPrograms}) {
            wchar_t* dir = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &dir))) ScanLinks(dir, *all, 0);
            CoTaskMemFree(dir);
        }
        ScanAppsFolder(*all);
        if (SUCCEEDED(co)) CoUninitialize();
        if (!PostMessageW(hwnd, kMsgAppsFolder, 1, reinterpret_cast<LPARAM>(all))) delete all;
    }).detach();
}

// Adds the apps whose name is not in the index yet (the first entry of a name wins), keeping it A to Z.
void Launcher::AddToIndex(Found const& found) {
    for (auto const& [name, path] : found) {
        auto lname = Lower(name);
        if (lname.find(L"uninstall") != std::wstring::npos) continue;
        if (std::any_of(m_index.begin(), m_index.end(), [&](AppEntry const& e) { return e.lname == lname; })) continue;
        m_index.push_back({name, lname, path});
    }
    std::sort(m_index.begin(), m_index.end(), [](AppEntry const& a, AppEntry const& b) { return a.lname < b.lname; });
}

// ---- Drawing --------------------------------------------------------------------------------

winrt::Windows::Foundation::Numerics::float3 Launcher::PanelHome() const { return {m_shadowM, kTop * m_s, 0}; }

wuc::CompositionDrawingSurface Launcher::MakeSurface(float w, float h) {
    return m_gdev.CreateDrawingSurface({std::ceil(w), std::ceil(h)},
                                       winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                       winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
}

wuc::SpriteVisual Launcher::MakeSurfaceVisual(wuc::CompositionDrawingSurface const& s, float w, float h) {
    auto brush = m_comp.CreateSurfaceBrush(s);
    brush.Stretch(wuc::CompositionStretch::None);
    auto v = m_comp.CreateSpriteVisual();
    v.Size({w, h});
    v.Brush(brush);
    return v;
}

// Draws a surface with one of the Draw* functions (each clears it first).
void Launcher::Redraw(wuc::CompositionDrawingSurface const& surface,
                      void (Launcher::*draw)(ID2D1DeviceContext*, float, float)) {
    auto interop = surface.as<ICompositionDrawingSurfaceInterop>();
    winrt::com_ptr<ID2D1DeviceContext> dc;
    POINT offset{};
    winrt::check_hresult(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &offset));
    try {
        (this->*draw)(dc.get(), static_cast<float>(offset.x), static_cast<float>(offset.y));
    } catch (...) {
        interop->EndDraw();
        throw;
    }
    interop->EndDraw();
}

// A file's shell icon at `logicalPx`, as a brush for its own sprite (cached by icon type and size).
wuc::CompositionBrush Launcher::FileBrush(std::wstring const& path, float logicalPx) {
    const int px = static_cast<int>(std::round(logicalPx * m_s));
    const auto key = IconKey(path);
    const auto bk = key + L"|" + std::to_wstring(px);
    if (auto it = m_fileBrushes.find(bk); it != m_fileBrushes.end()) return it->second;
    auto img = m_fileIcons.find(key);
    if (img == m_fileIcons.end()) return nullptr;  // worker supplies pixels; never block input for a shell icon
    if (!img->second.width) return nullptr;
    try {
        auto surface = MakeSurface(static_cast<float>(px), static_cast<float>(px));
        auto interop = surface.as<ICompositionDrawingSurfaceInterop>();
        winrt::com_ptr<ID2D1DeviceContext> dc;
        POINT off{};
        winrt::check_hresult(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &off));
        dc->Clear(D2D1::ColorF(0, 0.0f));
        winrt::com_ptr<ID2D1Bitmap> bmp;
        D2D1_BITMAP_PROPERTIES props =
            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(img->second.width, img->second.height), img->second.bgra.data(),
                                       img->second.width * 4, &props, bmp.put()))) {
            D2D1_RECT_F dest = R(static_cast<float>(off.x), static_cast<float>(off.y), static_cast<float>(off.x + px),
                                 static_cast<float>(off.y + px));
            dc->DrawBitmap(bmp.get(), &dest, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
        }
        interop->EndDraw();
        auto brush = m_comp.CreateSurfaceBrush(surface);
        brush.Stretch(wuc::CompositionStretch::Fill);
        m_fileBrushes.insert_or_assign(bk, brush);
        return brush;
    } catch (...) { return nullptr; }
}

namespace {

struct Text {
    winrt::com_ptr<IDWriteFactory> f;
    winrt::com_ptr<IDWriteTextFormat> Format(wchar_t const* family, float px, DWRITE_FONT_WEIGHT weight) const {
        winrt::com_ptr<IDWriteTextFormat> fmt;
        f->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"en-us",
                            fmt.put());
        if (fmt) {
            fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        return fmt;
    }
    winrt::com_ptr<IDWriteTextLayout> Layout(std::wstring const& t, IDWriteTextFormat* fmt, float w, float h) const {
        winrt::com_ptr<IDWriteTextLayout> l;
        f->CreateTextLayout(t.c_str(), static_cast<UINT32>(t.size()), fmt, std::max(w, 1.0f), std::max(h, 1.0f), l.put());
        return l;
    }
    float Width(std::wstring const& t, IDWriteTextFormat* fmt) const {
        DWRITE_TEXT_METRICS tm{};
        if (auto l = Layout(t, fmt, 2000, 100)) l->GetMetrics(&tm);
        return std::ceil(tm.widthIncludingTrailingWhitespace);
    }
    void Draw(ID2D1DeviceContext* dc, std::wstring const& t, IDWriteTextFormat* fmt, D2D1_RECT_F r, ID2D1Brush* b,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, bool ellipsis = false, bool tabular = false) const {
        auto l = Layout(t, fmt, r.right - r.left, r.bottom - r.top);
        if (!l) return;
        l->SetTextAlignment(align);
        if (tabular) {  // tabular numerals: digits of one width, so a column of numbers lines up
            winrt::com_ptr<IDWriteTypography> typo;
            if (SUCCEEDED(f->CreateTypography(typo.put()))) {
                typo->AddFontFeature({static_cast<DWRITE_FONT_FEATURE_TAG>(0x6D756E74), 1});  // 'tnum'
                l->SetTypography(typo.get(), {0, static_cast<UINT32>(t.size())});
            }
        }
        if (ellipsis) {
            winrt::com_ptr<IDWriteInlineObject> sign;
            f->CreateEllipsisTrimmingSign(fmt, sign.put());
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            l->SetTrimming(&trim, sign.get());
        }
        dc->DrawTextLayout({r.left, r.top}, l.get(), b);
    }
};

}  // namespace

// Lays out the panel for the current content and builds its visual tree.
void Launcher::Build() {
    const float s = m_s;

    // HALO 2.0: the panel is a left column (this Start) plus a right column (the control centre); without halo2 the
    // left column is the whole panel and nothing below changes.
    m_h2 = settings::Get().halo2;
    float wl, rightW = 0;  // the left column's width, and the right column's (logical px)
    if (m_h2) {
        auto const widths = start::PlanWidths3((m_anchor.monitor.right - m_anchor.monitor.left) / s);
        wl = widths.mid;
        rightW = widths.right;
        m_gap = std::round(start::kPanelGap * s);
        m_notifW = std::round(widths.notif * s);
        m_nw = m_notifW > 0 ? m_notifW + m_gap : 0;  // the middle card starts here
        m_w = std::round(wl * s) + m_gap + std::round(rightW * s);
    } else {
        wl = std::min(kWidth, (m_anchor.monitor.right - m_anchor.monitor.left) / s - 2 * kScreenMargin);
        wl = std::clamp(wl, std::min(kMinWidth, wl), kMaxWidth);
        m_nw = m_notifW = m_gap = 0;
        m_w = std::round(wl * s);
    }
    m_lw = m_h2 ? std::round(wl * s) + m_gap : m_w;  // where the right card starts
    m_rightW = rightW;

    // HALO 2.0 keeps twelve slots per section; classic Start follows its preferences.
    auto const& cfg = settings::Get().start;
    const size_t maxApps = m_h2 ? kH2Apps : std::min(cfg.recentApps, 6);
    size_t maxFiles = m_h2 ? kH2Files : std::min(cfg.recentFiles, kMaxFiles);
    if (m_h2) {  // Keep every recent item; the home body scrolls on smaller displays.
        m_plan = start::PlanCenter(std::min(start::AvailHeight(static_cast<float>(m_anchor.monitor.top), static_cast<float>(m_anchor.bottom), s),
                                            (m_anchor.monitor.bottom - m_anchor.monitor.top) / s * 0.78f),
                                   maxApps > 0, maxFiles > 0);
        m_homeScroll = 0;
        m_rightScroll = 0;
    }
    m_showApps = maxApps > 0;
    m_showFiles = maxFiles > 0;
    m_files.assign(m_allFiles.begin(), m_allFiles.begin() + std::min(m_allFiles.size(), maxFiles));

    // Recent apps: those that still have an icon.
    m_apps.clear();
    for (auto const& t : m_mru) {
        if (m_apps.size() >= maxApps) break;
        if (t.find(L"LockApp") != std::wstring::npos) continue;  // the lock screen is not an app to relaunch
        if (m_host.appIcon && m_host.appIcon(t)) m_apps.push_back(t);
    }

    // Top to bottom. Classic: search bar, recent apps (one row), recent files (2 x 6), the dock bar under a hairline.
    // HALO 2.0 (the middle card, no footer): greeting and date, three widget cards, search, recent apps (6 x 2), recent files (2 x 3).
    const int appCols = m_h2 ? kH2AppCols : kFileCols;
    const float innerW = wl - 2 * kPad, colW = innerW / appCols;
    const float searchTop = m_h2 ? m_plan.searchY : kSearchTop;
    const float searchBot = searchTop + (m_h2 ? start::kSearchH : kSearchH);
    float appsLabelY, appsTop, filesLabelY, filesTop, dividerY, panelH;
    if (m_h2) {
        // Never taller than 78 % of the screen, and at least tall enough for the control column (its footer is pinned).
        const float availH = start::AvailHeight(static_cast<float>(m_anchor.monitor.top), static_cast<float>(m_anchor.bottom), s);
        const float screenH = (m_anchor.monitor.bottom - m_anchor.monitor.top) / s;
        const float capH = std::min(availH, screenH * 0.78f);
        panelH = std::min(capH, std::max({m_plan.height, start::kRightMinH, start::PlanRight(0, kQuickTiles, true).bottom + start::kRightFootH}));
        appsLabelY = m_plan.appsLabelY;
        appsTop = m_plan.appsTop;
        filesLabelY = m_plan.filesLabelY;
        filesTop = m_plan.filesTop;
        dividerY = panelH;  // no footer in the middle card: its body runs to the bottom edge
    } else {
        appsLabelY = searchBot + ui::kS6;
        appsTop = appsLabelY + kLabelH + ui::kS2;
        filesLabelY = m_showApps ? appsTop + kAppRowH + ui::kS4 : appsLabelY;
        filesTop = filesLabelY + kLabelH + ui::kS2;
        const int fileRows = std::max<int>(1, (static_cast<int>(m_files.size()) + 1) / 2);  // 2 columns of 40 px rows
        const float contentEnd = m_showFiles ? filesTop + fileRows * start::tk::kFileRowH
                                             : m_showApps ? appsTop + kAppRowH : searchBot;
        dividerY = std::max(contentEnd + ui::kS4,
                            searchBot + 8 + 4 + kMaxResults * kResultRowH + 4);  // a page of results always fits
        panelH = dividerY + 1 + kBottomH;
    }
    const float iconSz = kIconSize;
    m_iconPx = iconSz;
    m_h = std::round(panelH * s);
    m_bodyTop = (searchBot + ui::kS2) * s;
    m_bodyBottom = dividerY * s;

    m_searchRect = R(kPad * s, searchTop * s, (kPad + innerW) * s, searchBot * s);
    m_appsLabel = R(kPad * s, appsLabelY * s, (kPad + innerW) * s, (appsLabelY + kLabelH) * s);
    m_filesLabel = R(kPad * s, filesLabelY * s, (kPad + innerW) * s, (filesLabelY + kLabelH) * s);
    // "All apps" at the right end of the first label's line (its text, not its plate, on the margin).
    m_allAppsRect = R((kPad + innerW + 8 - 104) * s, (appsLabelY - 6) * s, (kPad + innerW + 8) * s, (appsLabelY + kLabelH + 6) * s);
    m_divider = R(kPad * s, dividerY * s, (wl - kPad) * s, (dividerY + 1) * s);
    m_appCells.clear();
    m_fileCells.clear();
    for (size_t i = 0; i < m_apps.size(); i++) {
        float x = kPad + (i % appCols + 0.5f) * colW - iconSz / 2, y = appsTop + (i / appCols) * kAppRowH + start::tk::kAppPadTop;
        m_appCells.push_back(R(x * s, y * s, (x + iconSz) * s, (y + iconSz) * s));
    }
    const float fileColW = (innerW - start::tk::kPad) / 2;  // HALO 2.0: recent files are a two-column list of rows
    for (size_t i = 0; i < m_files.size(); i++) {  // two columns of 40 px rows, in both modes
        float x = kPad + (i % 2) * (fileColW + start::tk::kPad), y = filesTop + (i / 2) * start::tk::kFileRowH;
        m_fileCells.push_back(R(x * s, y * s, (x + fileColW) * s, (y + start::tk::kFileRowH) * s));
    }

    m_staticHits.clear();
    if (m_h2) {
        // Footer of the control column: battery on the left; Lock, Settings and Power (36 px icon buttons) on the right.
        const float pad = start::kRightPad * s, btn = ui::kButton * s, gap = ui::kS2 * s, y0 = m_h - pad - btn;
        m_powerRect = R(m_w - pad - btn, y0, m_w - pad, y0 + btn);
        m_settingsRect = R(m_powerRect.left - gap - btn, y0, m_powerRect.left - gap, y0 + btn);
        m_lockRect = R(m_settingsRect.left - gap - btn, y0, m_settingsRect.left - gap, y0 + btn);
        m_avatarRect = m_nameRect = R(0, 0, 0, 0);
        m_staticHits.push_back({m_lockRect, Kind::Lock, 0});
    } else {
        // Footer: the picture and name, and Settings and Power from the right as 36 px icon buttons.
        const float bottomY = dividerY + 1, btn = ui::kButton, gap = ui::kS2, by = bottomY + ui::kS3;
        const float rowMid = by + btn / 2, powerX = wl - kPad - btn, settingsX = powerX - gap - btn;
        m_avatarRect = R(kPad * s, (rowMid - kAvatar / 2) * s, (kPad + kAvatar) * s, (rowMid + kAvatar / 2) * s);
        m_powerRect = R(powerX * s, by * s, (powerX + btn) * s, (by + btn) * s);
        m_settingsRect = R(settingsX * s, by * s, (settingsX + btn) * s, (by + btn) * s);
        m_nameRect = R((kPad + kAvatar + ui::kS3) * s, (rowMid - ui::kS3) * s, (settingsX - ui::kS2) * s, (rowMid + ui::kS3) * s);
    }
    m_staticHits.push_back({m_settingsRect, Kind::Settings, 0});
    m_staticHits.push_back({m_powerRect, Kind::Power, 0});

    if (m_h2) {  // the greeting and its date, and three widget cards (battery, power mode, this week)
        m_greetRect = R(kPad * s, m_plan.greetY * s, (kPad + innerW) * s, (m_plan.greetY + m_plan.greetH) * s);
        m_calRect = R(0, 0, 0, 0);
        m_battery = svc::ReadBattery();
        m_mode = svc::ReadPowerMode();
        const float wgap = start::tk::kPad * s, cw = (innerW * s - 2 * wgap) / 3, wy = m_plan.widgetsY * s;
        m_modeCard = R(kPad * s + cw + wgap, wy, kPad * s + 2 * cw + wgap, wy + m_plan.widgetsH * s);
        if (m_mode != svc::PowerMode::Unknown) {  // a click steps Efficiency -> Balanced -> Performance -> Efficiency
            const int now = m_mode == svc::PowerMode::Efficiency ? 0 : m_mode == svc::PowerMode::Balanced ? 1 : 2;
            m_staticHits.push_back({m_modeCard, Kind::PowerMode, (now + 1) % 3});
        }
    }

    // The Power menu opens above the Power button, right-aligned with it.
    m_menuW = std::round(kMenuW * s);
    m_menuH = std::round((kMenuCount * kMenuRowH + 2 * kMenuPad) * s);
    m_menuX = std::round(m_powerRect.right - m_menuW);
    m_menuY = std::round(m_powerRect.top - 6 * s - m_menuH);
    m_menuRows.clear();
    m_menuHits.clear();
    for (int i = 0; i < kMenuCount; i++) {
        float top = (kMenuPad + i * kMenuRowH) * s;
        m_menuRows.push_back(R(kMenuPad * s, top, m_menuW - kMenuPad * s, top + kMenuRowH * s));
        m_menuHits.push_back({R(m_menuX + kMenuPad * s, m_menuY + top, m_menuX + m_menuW - kMenuPad * s,
                                m_menuY + top + kMenuRowH * s), Kind::MenuItem, i});
    }

    // Visual tree: [glass] [content: hover plate, bottom bar, search bar, body (text + icon sprites)] [menu].
    m_root.Children().RemoveAll();
    {   // transparent margin around the panel that the drop shadow lives in (left, right, bottom)
        auto const& pal = theme::P();
        m_shadowM = pal.shadow.a ? std::ceil(pal.shadowBlur * s) : 0.0f;
        m_shadowB = pal.shadow.a ? std::ceil((pal.shadowBlur + pal.shadowY) * s) : 0.0f;
    }
    m_shadowHold = nullptr;
    // HALO 2.0 is three cards (notifications, middle, control centre), each its own glass with its own edge, shadow and
    // rounded corners; classic Start is one. `boxes` are the cards' x and width in panel px.
    const float panelR = ui::kPanelRadius * s;  // 36: a 20 px card plus its 16 px inset, so inner corners stay concentric
    struct Box { float x, w; };
    std::vector<Box> boxes;
    if (m_h2) {
        if (m_nw > 0) boxes.push_back({0, m_notifW});
        boxes.push_back({m_nw, m_lw - m_gap});
        boxes.push_back({m_nw + m_lw, m_w - m_lw});
    } else {
        boxes.push_back({0, PanelW()});
    }
    // The cards' glass is the dock's (same tint, same opacity): Start reads as part of the same surface family.
    settings::Surface const& glassCfg = m_h2 ? static_cast<settings::Surface const&>(settings::Get().taskbar) : settings::Get().start;
    if (m_shadowM > 0) {
        auto hold = m_comp.CreateContainerVisual();
        hold.Size({PanelW(), m_h});
        hold.Offset(PanelHome());
        for (auto const& b : boxes) {
            auto sh = m_h2 ? MakeBakedShadow(m_comp, m_gdev, std::ceil(b.w), std::ceil(m_h), panelR, theme::P().shadowBlur * s,
                                             theme::P().shadowY * s, theme::P().shadow)
                           : MakeShadow(m_comp, m_gdev, std::ceil(b.w), std::ceil(m_h), panelR, theme::P().shadowBlur * s,
                                        theme::P().shadowY * s, theme::P().shadow);
            if (!sh) continue;
            if (!m_h2) sh.Size({b.w, m_h});
            auto at = m_comp.CreateContainerVisual();  // the shadow is drawn around (0, 0): put it under its card
            at.Size({b.w, m_h});
            at.Offset({b.x, 0, 0});
            at.Children().InsertAtTop(sh);
            hold.Children().InsertAtTop(at);
        }
        if (hold.Children().Count()) {
            m_shadowHold = hold;
            m_root.Children().InsertAtTop(m_shadowHold);
        }
    }
    // The stage clips the panel to the old window rect, so the rise/exit still happens behind the window bottom edge.
    auto stage = m_comp.CreateContainerVisual();
    stage.Offset({m_shadowM, 0, 0});
    stage.Size({PanelW(), kTop * s + m_h});
    {
        auto sc = m_comp.CreateRectangleClip();
        sc.Left(0); sc.Top(0); sc.Right(PanelW()); sc.Bottom(kTop * s + m_h);
        stage.Clip(sc);
    }
    m_root.Children().InsertAtTop(stage);
    m_panel = m_comp.CreateContainerVisual();
    m_panel.Size({PanelW(), m_h});
    m_panel.Offset({0, kTop * s, 0});
    if (!m_h2) {
        auto clip = m_comp.CreateRoundedRectangleGeometry();
        clip.CornerRadius({panelR, panelR});
        clip.Size({PanelW(), m_h});
        m_panel.Clip(m_comp.CreateGeometricClip(clip));
    }
    stage.Children().InsertAtTop(m_panel);

    for (auto const& b : boxes) {  // glass: backdrop, tint, grain, gradient rim and the 1 px outer hairline (all from the host's BuildPill)
        auto card = m_comp.CreateContainerVisual();
        card.Size({b.w, m_h});
        card.Offset({b.x, 0, 0});
        if (m_h2) {
            auto clip = m_comp.CreateRoundedRectangleGeometry();
            clip.CornerRadius({panelR, panelR});
            clip.Size({b.w, m_h});
            card.Clip(m_comp.CreateGeometricClip(clip));
        }
        if (m_host.glass) {
            auto glass = m_host.glass(static_cast<int>(std::ceil(m_h)), glassCfg, static_cast<int>(std::ceil(b.w)), panelR);
            glass.Size({b.w, m_h});
            card.Children().InsertAtTop(glass);
        }
        m_panel.Children().InsertAtTop(card);
    }
    m_content = m_comp.CreateContainerVisual();  // the middle and the control centre: x = 0 is the middle card's left edge
    m_content.Size({m_w, m_h});
    m_content.Offset({m_nw, 0, 0});
    m_panel.Children().InsertAtTop(m_content);

    m_notifRoot = nullptr;
    if (m_h2 && m_nw > 0) {  // the notification + calendar card
        if (!m_notifCol) {
            m_notifCol = std::make_unique<NotifColumn>(GetModuleHandleW(nullptr), m_comp, m_gdev);
            m_notifCol->onOpened = [this] { Hide(); };
        }
        m_notifRoot = m_notifCol->Build(m_notifW, m_h, s);
        m_panel.Children().InsertAtTop(m_notifRoot);
    }

    auto plate = [&](wuc::CompositionRoundedRectangleGeometry& shape, bool content) {
        shape = m_comp.CreateRoundedRectangleGeometry();
        auto sprite = m_comp.CreateSpriteShape(shape);
        auto brush = m_comp.CreateColorBrush(Col(theme::P().fillHover));  // the one hover treatment; PressFeel recolours it on mouse-down
        sprite.FillBrush(brush);
        (content ? m_hoverSprite : m_menuHoverSprite) = sprite;
        (content ? m_hoverBrush : m_menuHoverBrush) = brush;
        auto v = m_comp.CreateShapeVisual();
        v.Size({m_w, m_h});
        v.Shapes().Append(sprite);
        v.Opacity(0.0f);
        return v;
    };
    m_hover = plate(m_hoverShape, true);
    m_content.Children().InsertAtTop(m_hover);
    m_wave = m_comp.CreatePropertySet();
    m_wave.InsertScalar(L"PointerX", -10000.0f);
    m_wave.InsertScalar(L"PointerY", -10000.0f);
    m_wave.InsertScalar(L"Intensity", 0.0f);
    m_waveOn = false;
    m_hoverIndex = -1;
    m_hoverOn = m_menuHoverOn = false;

    if (!m_h2) {  // the dock bar: divider, user, Settings, Power (classic; 2.0's footer is the control column's)
        float top = m_divider.top, h = m_h - top;
        const float barPx = m_lw;
        auto surface = MakeSurface(barPx, h);
        Redraw(surface, &Launcher::DrawStatic);
        auto v = MakeSurfaceVisual(surface, barPx, h);
        v.Offset({0, top, 0});
        m_content.Children().InsertAtTop(v);
    }
    {   // the search bar's surface and its caret (the blink runs on the compositor)
        float w = m_searchRect.right - m_searchRect.left, h = m_searchRect.bottom - m_searchRect.top;
        m_searchSurface = MakeSurface(w, h);
        auto v = MakeSurfaceVisual(m_searchSurface, w, h);
        v.Offset({m_searchRect.left, m_searchRect.top, 0});
        m_content.Children().InsertAtTop(v);
        m_caret = m_comp.CreateSpriteVisual();
        m_caret.Brush(m_comp.CreateColorBrush(Col(theme::P().text)));
        m_content.Children().InsertAtTop(m_caret);
    }
    if (m_h2) {
        m_headerSurface = MakeSurface(m_lw, searchTop * s);
        auto header = MakeSurfaceVisual(m_headerSurface, m_lw, searchTop * s);
        m_content.Children().InsertAtTop(header);
        Redraw(m_headerSurface, &Launcher::DrawHeader);
    }
    {   // the body: one text surface and the icon sprites on top of it, in a holder the scroll slides (no rebuild)
        float h = m_bodyBottom - m_bodyTop;
        if (m_h2) h = std::max(h, m_plan.contentBottom * s - m_bodyTop);
        m_bodySurface = MakeSurface(m_lw, h);
        auto v = MakeSurfaceVisual(m_bodySurface, m_lw, h);
        m_homeHold = m_comp.CreateContainerVisual();
        m_homeHold.Size({m_lw, h});
        m_homeHold.Offset({0, m_bodyTop, 0});
        m_homeHold.Children().InsertAtTop(v);
        m_bodyIcons = m_comp.CreateContainerVisual();
        m_bodyIcons.Size({m_lw, h});
        m_homeHold.Children().InsertAtTop(m_bodyIcons);
        auto homeClip = m_comp.CreateContainerVisual();
        homeClip.Size({m_lw, m_h});
        auto inset = m_comp.CreateInsetClip();
        inset.TopInset(m_bodyTop);
        inset.BottomInset(m_h - m_bodyBottom);
        homeClip.Clip(inset);
        homeClip.Children().InsertAtTop(m_homeHold);
        m_content.Children().InsertAtTop(homeClip);
    }
    {   // the result rows
        const float h = (std::ceil((m_bodyBottom - m_bodyTop) / (kResultRowH * s)) + 2 * kListBuffer) * kResultRowH * s;
        m_listSurface = MakeSurface(m_lw, h);
        m_listClip = m_comp.CreateContainerVisual();
        m_listClip.Size({m_lw, m_h});
        m_listInset = m_comp.CreateInsetClip();
        m_listClip.Clip(m_listInset);
        m_listHold = m_comp.CreateContainerVisual();
        m_listHold.Size({m_lw, h});
        m_listHold.Children().InsertAtTop(MakeSurfaceVisual(m_listSurface, m_lw, h));
        m_listIcons = m_comp.CreateContainerVisual();
        m_listIcons.Size({m_lw, h});
        m_listHold.Children().InsertAtTop(m_listIcons);
        m_listClip.Children().InsertAtTop(m_listHold);
        m_content.Children().InsertAtTop(m_listClip);
    }

    if (m_h2) {
        BuildRight(rightW);
        const float fw = m_w - m_lw, fh = start::kRightFootH * s;  // the control column's footer: battery, Lock, Settings, Power
        m_footSurface = MakeSurface(fw, fh);
        auto foot = MakeSurfaceVisual(m_footSurface, fw, fh);
        foot.Offset({m_lw, m_h - fh, 0});
        m_content.Children().InsertAtTop(foot);
        Redraw(m_footSurface, &Launcher::DrawFoot);
    } else {
        m_ccHits.clear();
    }

    // The menu: its own glass, dimming the panel behind it while it is open.
    m_menu = m_comp.CreateContainerVisual();
    m_menu.Size({m_menuW, m_menuH});
    m_menuOuter = nullptr;
    if (m_h2) {  // HALO 2.0: the menu is the one raised surface: a shadow under it (baked, so it costs no GPU blur), animated with it
        m_menuOuter = m_comp.CreateContainerVisual();
        m_menuOuter.Size({m_menuW, m_menuH});
        m_menuOuter.Offset({m_menuX + m_nw, m_menuY, 0});
        m_menuOuter.Opacity(0.0f);
        m_menuOuter.IsVisible(false);
        if (auto sh = MakeBakedShadow(m_comp, m_gdev, m_menuW, m_menuH, start::tk::kRCard * s, start::tk::kMenuShadowBlur * s,
                                      start::tk::kMenuShadowDy * s, theme::P().shadow.a ? theme::Argb{theme::MenuShadowAlpha(), 0, 0, 0} : theme::Argb{0, 0, 0, 0}))
            m_menuOuter.Children().InsertAtTop(sh);
        m_menuOuter.Children().InsertAtTop(m_menu);
    } else {
        m_menu.Offset({m_menuX + m_nw, m_menuY, 0});
        m_menu.Opacity(0.0f);
        m_menu.IsVisible(false);
    }
    auto menuClip = m_comp.CreateRoundedRectangleGeometry();
    const float menuR = ui::kRadiusLarge * s;
    menuClip.CornerRadius({menuR, menuR});
    menuClip.Size({m_menuW, m_menuH});
    m_menu.Clip(m_comp.CreateGeometricClip(menuClip));
    // The glass behind the menu is built the first time it opens (ToggleMenu): a closed menu costs no host backdrop.
    m_menuGlassHold = m_comp.CreateContainerVisual();
    m_menuGlassHold.Size({m_menuW, m_menuH});
    m_menu.Children().InsertAtTop(m_menuGlassHold);
    m_menuGlassBuilt = false;
    if (m_host.glass) {
        // Over the panel's own text the dock's 50% tint is not enough; the backdrop is the desktop, not the panel.
        auto veil = m_comp.CreateSpriteVisual();
        veil.Size({m_menuW, m_menuH});
        const auto tint = theme::Glass(glassCfg).tint;  // the frost follows the panel's own glass section
        veil.Brush(m_comp.CreateColorBrush(theme::C({theme::MenuVeilAlpha(), tint.r, tint.g, tint.b})));
        m_menu.Children().InsertAtTop(veil);
        if (m_h2) {  // raised: a lighter sheet than the panel's glass
            auto raise = m_comp.CreateSpriteVisual();
            raise.Size({m_menuW, m_menuH});
            raise.Brush(m_comp.CreateColorBrush(theme::C(theme::MenuSheet())));
            m_menu.Children().InsertAtTop(raise);
        }
    }
    m_menuHover = plate(m_menuHoverShape, false);
    m_menuHover.Size({m_menuW, m_menuH});
    m_menu.Children().InsertAtTop(m_menuHover);
    auto menuSurface = MakeSurface(m_menuW, m_menuH);
    Redraw(menuSurface, &Launcher::DrawMenu);
    m_menu.Children().InsertAtTop(MakeSurfaceVisual(menuSurface, m_menuW, m_menuH));
    m_panel.Children().InsertAtTop(m_h2 ? m_menuOuter : m_menu);
    m_menuOpen = false;

    SearchDraw();
    BuildBody();
}

// The part between the search bar and the dock bar: the home sections, or the results while there is a query.
// Rebuilt on every keystroke; the glass, shadow and bars are not.
// The dock's wave for one icon: scale = 1 + 0.28 * (cosine falloff of the distance to the pointer, radius ~90 px), times
// Intensity (0 until the pointer is near the apps). `k` lives on the sprite so Scale and the small lift share one value.
// The icon's centre is baked; the home holder's offset is referenced live, so the wave stays right while the list scrolls.
void Launcher::AttachWave(IconV const& ic, float cx, float cy, float h) {
    if (!m_wave || !m_homeHold) return;
    const float R = 92.0f * m_s;
    auto n = [](float v) { return std::to_wstring(v); };
    auto k = m_comp.CreateExpressionAnimation(
        L"1 + 0.28 * w.Intensity * (Cos(Clamp(Sqrt(Pow(w.PointerX - " + n(cx) + L", 2) + Pow(w.PointerY - (" + n(cy) + L" + hold.Offset.Y - " + n(m_bodyTop) +
        L"), 2)) / " + n(R) + L", 0, 1) * 3.14159265) + 1) / 2");
    k.SetReferenceParameter(L"w", m_wave);
    k.SetReferenceParameter(L"hold", m_homeHold);
    ic.v.Properties().InsertScalar(L"k", 1.0f);
    ic.v.Properties().StartAnimation(L"k", k);
    auto scale = m_comp.CreateExpressionAnimation(L"Vector3(Max(me.k, 0.001), Max(me.k, 0.001), 1)");
    scale.SetReferenceParameter(L"me", ic.v.Properties());
    ic.v.StartAnimation(L"Scale", scale);
    auto lift = m_comp.CreateExpressionAnimation(n(ic.restY) + L" - (me.k - 1) * " + n(h * 0.45f));
    lift.SetReferenceParameter(L"me", ic.v.Properties());
    ic.v.StartAnimation(L"Offset.Y", lift);
}

void Launcher::WaveStart() {
    KillTimer(m_hwnd, kWaveOffTimer);
    if (m_waveLive) return;
    m_waveLive = true;
    for (auto const& w : m_waveItems) AttachWave(w.ic, w.cx, w.cy, w.h);
}

// Unbind everything and put each icon back at rest.
void Launcher::WaveStop() {
    KillTimer(m_hwnd, kWaveOffTimer);
    if (!m_waveLive) return;
    m_waveLive = false;
    for (auto const& w : m_waveItems) {
        if (!w.ic.v) continue;
        w.ic.v.Properties().StopAnimation(L"k");
        w.ic.v.StopAnimation(L"Scale");
        w.ic.v.StopAnimation(L"Offset.Y");
        w.ic.v.Scale({1, 1, 1});
        auto o = w.ic.v.Offset();
        w.ic.v.Offset({o.x, w.ic.restY, o.z});
    }
}

// Pointer in content coordinates. The wave is on while the pointer is within a radius of the apps.
void Launcher::MoveWave(float x, float panelY) {
    if (!m_wave) return;
    m_wave.InsertScalar(L"PointerX", x);
    m_wave.InsertScalar(L"PointerY", panelY);
    bool closeEnough = false;
    if (!m_appCells.empty() && !Listing()) {
        const float R = 92.0f * m_s;
        const float top = m_appCells.front().top - m_homeScroll - R, bottom = m_appCells.back().bottom - m_homeScroll + R;
        closeEnough = x >= 0 && x < m_lw && panelY >= top && panelY <= bottom;
    }
    if (closeEnough != m_waveOn) {
        m_waveOn = closeEnough;
        if (closeEnough) WaveStart();
        else SetTimer(m_hwnd, kWaveOffTimer, 420, nullptr);  // after the 360 ms relax
        Animate(m_wave, L"Intensity", closeEnough ? 1.0f : 0.0f, closeEnough ? 140 : 360);  // eases in fast, relaxes slowly: the dock's feel
    }
}

void Launcher::BuildBody() {
    const float s = m_s;
    const bool results = Listing();
    m_bodyIcons.Children().RemoveAll();
    m_appSprites.clear();
    m_waveItems.clear();
    m_fileSprites.clear();
    m_resultRows.clear();
    m_hoverIcon = nullptr;
    m_hoverIndex = -1;
    m_hits.clear();

    // An icon on its own sprite, so it can scale and lift without redrawing anything.
    auto sprite = [&](wuc::CompositionBrush const& brush, D2D1_RECT_F r) {
        IconV ic;
        if (!brush) return ic;
        float w = r.right - r.left, h = r.bottom - r.top;
        ic.v = m_comp.CreateSpriteVisual();
        ic.v.Size({w, h});
        r.top -= m_bodyTop;  // sprites live in the home holder, which carries the scroll
        ic.v.Offset({r.left, r.top, 0});
        ic.v.CenterPoint({w / 2, h / 2, 0});
        ic.v.Brush(brush);
        ic.restY = r.top;
        m_bodyIcons.Children().InsertAtTop(ic.v);
        return ic;
    };

    m_homeHold.StopAnimation(L"Offset.Y");  // the home view sits at its scroll; a list or search result at the top
    m_homeHold.Offset({0, m_bodyTop - (m_h2 && !results ? m_homeScroll : 0.0f), 0});
    m_listIcons.Children().RemoveAll();
    m_listHold.StopAnimation(L"Offset.Y");
    m_scrollFrom = 0;
    m_listClip.IsVisible(results);
    if (results) {
        const float rowPx = kResultRowH * s;
        for (size_t i = 0; i < m_results.size(); i++) {
            float top = ListTop() + i * rowPx;
            auto row = R((kPad - 8) * s, top, m_lw - (kPad - 8) * s, top + rowPx);
            m_resultRows.push_back(row);
            m_hits.push_back({row, Kind::Result, static_cast<int>(i)});
        }
        // The rows on screen and kListBuffer past each end, so a scroll slides real rows in.
        const int total = static_cast<int>(m_matches.size()), rows = ListRows();
        m_listInset.TopInset(ListTop());
        m_listInset.BottomInset(m_h - (ListTop() + rows * rowPx));  // whole rows only: a cut-off one could not be clicked
        m_listHold.Offset({0, ListTop() - kListBuffer * rowPx, 0});
        for (int j = -kListBuffer; j < rows + kListBuffer; j++) {
            const int k = m_scroll + j;
            if (k < 0 || k >= total) continue;
            auto const& r = m_matches[k];
            auto brush = r.type == Result::Type::File ? FileBrush(r.path, kResultIcon)
                         : m_host.appIcon ? m_host.appIcon(r.type == Result::Type::App ? r.path : kSettingsApp)
                                          : nullptr;
            if (!brush) continue;
            auto v = m_comp.CreateSpriteVisual();
            v.Size({kResultIcon * s, kResultIcon * s});
            v.Offset({(kPad - 8 + 12) * s, (j + kListBuffer) * rowPx + (kResultRowH - kResultIcon) / 2 * s, 0});
            v.Brush(brush);
            m_listIcons.Children().InsertAtTop(v);
        }
        Redraw(m_listSurface, &Launcher::DrawList);
    } else {
        for (size_t i = 0; i < m_apps.size(); i++) {
            auto const& c = m_appCells[i];
            const float cw = (m_lw - m_gap - 2 * kPad * s) / kH2AppCols;  // the whole cell, icon and name, is one target
            const float inset = ui::kS1 / 2 * s, top = c.top - start::tk::kAppPadTop * s;
            m_hits.push_back({R((c.left + c.right) / 2 - cw / 2 + inset, top, (c.left + c.right) / 2 + cw / 2 - inset, top + kAppRowH * s), Kind::App, static_cast<int>(i)});
            m_appSprites.push_back(sprite(m_host.appIcon(m_apps[i]), c));
            if (m_h2 && m_appSprites.back().v) {
                m_waveItems.push_back({m_appSprites.back(), (c.left + c.right) / 2, (c.top + c.bottom) / 2, c.bottom - c.top});
                if (m_waveLive) AttachWave(m_waveItems.back().ic, m_waveItems.back().cx, m_waveItems.back().cy, m_waveItems.back().h);
            }
        }
        for (size_t i = 0; i < m_files.size(); i++) {
            auto const& c = m_fileCells[i];
            m_hits.push_back({c, Kind::File, static_cast<int>(i)});
            const float ic = start::tk::kFileIcon * s;  // a row: the 16 px icon at the left end
            m_fileSprites.push_back(sprite(FileBrush(m_files[i].path, start::tk::kFileIcon),
                                           R(c.left + ui::kS2 * s, (c.top + c.bottom) / 2 - ic / 2, c.left + ui::kS2 * s + ic, (c.top + c.bottom) / 2 + ic / 2)));
        }
    }
    if (m_query.empty()) m_hits.push_back({m_allAppsRect, Kind::AllApps, 0});
    for (auto const& h : m_staticHits) m_hits.push_back(h);
    m_hitBase.clear();
    for (auto const& h : m_hits) m_hitBase.push_back(h.r);
    if (m_h2 && !results) ShiftHits();
    Redraw(m_bodySurface, &Launcher::DrawBody);

    // The plate rests on the selected result, or is away on the home view.
    if (results && !m_resultRows.empty()) Highlight(&m_resultRows[m_sel], ui::kRadiusHover, false);
    else Highlight(nullptr, 0, false);
}

// The dock bar: hairline, the user (account picture or an initial in a circle, name), Settings and Power.
void Launcher::DrawStatic(ID2D1DeviceContext* dc, float ox, float oy) {
    const float s = m_s;
    Text txt{m_dwrite};
    dc->Clear(D2D1::ColorF(0, 0.0f));
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    auto const& pal = theme::P();
    winrt::com_ptr<ID2D1SolidColorBrush> white, dim, line;  // "white" = primary ink
    dc->CreateSolidColorBrush(pal.text, white.put());
    dc->CreateSolidColorBrush(MutedOf(m_h2), dim.put());
    dc->CreateSolidColorBrush(pal.line, line.put());
    const float dy = oy - m_divider.top;
    auto at = [&](D2D1_RECT_F r) { return R(r.left + ox, r.top + dy, r.right + ox, r.bottom + dy); };

    auto fName = txt.Format(ui::kFontText, ui::kBody * s, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    auto fGlyph = txt.Format(ui::kFontIcons, ui::kIcon * s, DWRITE_FONT_WEIGHT_NORMAL);

    {
        auto d = at(m_divider);
        d.bottom = d.top + std::max(1.0f, std::round(ui::kHair * s));  // a hairline
        dc->FillRectangle(d, line.get());
    }

    auto circle = [&](D2D1_RECT_F r) {
        float rad = (r.right - r.left) / 2;
        return D2D1::Ellipse({(r.left + r.right) / 2, (r.top + r.bottom) / 2}, rad, rad);
    };
    auto av = at(m_avatarRect);
    bool drew = false;
    if (!m_avatar.empty()) {
        winrt::com_ptr<ID2D1Bitmap> bmp;
        D2D1_BITMAP_PROPERTIES props =
            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        winrt::com_ptr<ID2D1BitmapBrush> brush;
        if (SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(m_avatarPx, m_avatarPx), m_avatar.data(), m_avatarPx * 4, &props, bmp.put())) &&
            SUCCEEDED(dc->CreateBitmapBrush(bmp.get(), brush.put()))) {
            float k = (av.right - av.left) / m_avatarPx;
            brush->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            brush->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(av.left, av.top));
            dc->FillEllipse(circle(av), brush.get());
            drew = true;
        }
    }
    if (!drew) {
        winrt::com_ptr<ID2D1SolidColorBrush> fill;
        dc->CreateSolidColorBrush(pal.accent, fill.put());
        dc->FillEllipse(circle(av), fill.get());
        auto fInitial = txt.Format(ui::kFontText, ui::kBody * s, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        std::wstring initial = m_user.empty() ? L"?" : std::wstring(1, static_cast<wchar_t>(std::towupper(m_user[0])));
        winrt::com_ptr<ID2D1SolidColorBrush> onAccent;
        dc->CreateSolidColorBrush(pal.onAccent, onAccent.put());
        txt.Draw(dc, initial, fInitial.get(), av, onAccent.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    // A hairline ring so a dark picture still has an edge on dark glass.
    dc->DrawEllipse(circle(av), line.get(), std::max(1.0f, std::round(ui::kHair * s)));
    txt.Draw(dc, m_user, fName.get(), at(m_nameRect), white.get(), DWRITE_TEXT_ALIGNMENT_LEADING, true);

    // Settings and Power: 36 px icon buttons (their hover and pressed plates are the panel's one plate).
    for (auto const& [rect, g] : {std::pair{m_settingsRect, L"\uE713"}, std::pair{m_powerRect, L"\uE7E8"}})
        txt.Draw(dc, g, fGlyph.get(), at(rect), white.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
}

// The search bar: a round-ended field with a magnifier, the typed text (or the placeholder), scrolled to keep the end in view.
void Launcher::DrawSearch(ID2D1DeviceContext* dc, float ox, float oy) {
    const float s = m_s;
    Text txt{m_dwrite};
    dc->Clear(D2D1::ColorF(0, 0.0f));
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    auto const& pal = theme::P();
    const float w = m_searchRect.right - m_searchRect.left, h = m_searchRect.bottom - m_searchRect.top;
    winrt::com_ptr<ID2D1SolidColorBrush> fill, line, ink, dim;
    dc->CreateSolidColorBrush(pal.fill, fill.put());
    dc->CreateSolidColorBrush(pal.line, line.put());
    dc->CreateSolidColorBrush(pal.text, ink.put());
    dc->CreateSolidColorBrush(theme::Muted(), dim.put());
    const float hair = std::max(1.0f, std::round(ui::kHair * s));
    auto pill = D2D1::RoundedRect(R(ox + hair / 2, oy + hair / 2, ox + w - hair / 2, oy + h - hair / 2), h / 2, h / 2);
    dc->FillRoundedRectangle(pill, fill.get());
    dc->DrawRoundedRectangle(pill, line.get(), hair);

    auto fGlyph = txt.Format(ui::kFontIcons, ui::kIcon * s, DWRITE_FONT_WEIGHT_NORMAL);
    auto fText = txt.Format(ui::kFontText, ui::kBody * s, DWRITE_FONT_WEIGHT_NORMAL);
    txt.Draw(dc, L"\uE721", fGlyph.get(), R(ox + ui::kS4 * s, oy, ox + (ui::kS4 + ui::kIcon) * s, oy + h), dim.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    const float left = kSearchLeft * s, area = w - left - kSearchRight * s;
    if (m_query.empty()) {
        const float off = m_h2 ? ui::kS2 * s : 0;  // the caret sits before the placeholder, not on its first letter
        txt.Draw(dc, L"Search apps, settings and files", fText.get(), R(ox + left + off, oy, ox + left + area, oy + h), dim.get());
        return;
    }
    float tw = txt.Width(m_query, fText.get());
    float shift = std::max(0.0f, tw - area);  // longer than the bar: the end stays visible
    dc->PushAxisAlignedClip(R(ox + left, oy, ox + left + area + ui::kS1 * s, oy + h), D2D1_ANTIALIAS_MODE_ALIASED);
    txt.Draw(dc, m_query, fText.get(), R(ox + left - shift, oy, ox + left - shift + tw + ui::kS2 * s, oy + h), ink.get());
    dc->PopAxisAlignedClip();
}

// Redraws the search bar and puts the caret at the end of the text; the blink restarts so it stays solid while typing.
void Launcher::SearchDraw() {
    const float s = m_s;
    Redraw(m_searchSurface, &Launcher::DrawSearch);
    Text txt{m_dwrite};
    auto fText = txt.Format(ui::kFontText, ui::kBody * s, DWRITE_FONT_WEIGHT_NORMAL);
    const float w = m_searchRect.right - m_searchRect.left, left = kSearchLeft * s, area = w - left - kSearchRight * s;
    float x = m_searchRect.left + left + std::min(m_query.empty() ? 0.0f : txt.Width(m_query, fText.get()), area);
    float cw = std::max(1.0f, std::round(ui::kHair * s)), ch = ui::kHeading * s;
    m_caret.Size({cw, ch});
    m_caret.Offset({x, m_searchRect.top + ((m_searchRect.bottom - m_searchRect.top) - ch) / 2, 0});
    if (m_h2) {  // solid while typing; then a 530 ms timer toggles it (two compositor frames a second, not 60)
        m_caret.StopAnimation(L"Opacity");
        m_caret.Opacity(1.0f);
        m_caretOn = true;
        SetTimer(m_hwnd, kCaretTimer, kCaretBlinkMs, nullptr);
        return;
    }
    auto blink = m_comp.CreateScalarKeyFrameAnimation();
    blink.InsertKeyFrame(0.0f, 1.0f);
    blink.InsertKeyFrame(0.55f, 1.0f);
    blink.InsertKeyFrame(0.56f, 0.0f);
    blink.InsertKeyFrame(1.0f, 0.0f);
    blink.Duration(std::chrono::milliseconds(2 * kCaretBlinkMs));
    blink.IterationBehavior(wuc::AnimationIterationBehavior::Forever);
    m_caret.StartAnimation(L"Opacity", blink);
}

// The labelled sections and file names, or the result rows.
void Launcher::DrawBody(ID2D1DeviceContext* dc, float ox, float oy) {
    const float s = m_s;
    Text txt{m_dwrite};
    dc->Clear(D2D1::ColorF(0, 0.0f));
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    auto const& pal = theme::P();
    winrt::com_ptr<ID2D1SolidColorBrush> white, soft, dim;  // "white" = primary ink
    dc->CreateSolidColorBrush(pal.text, white.put());
    dc->CreateSolidColorBrush(pal.textSoft, soft.put());
    dc->CreateSolidColorBrush(MutedOf(m_h2), dim.put());
    const float dy = oy - m_bodyTop;
    auto at = [&](D2D1_RECT_F r) { return R(r.left + ox, r.top + dy, r.right + ox, r.bottom + dy); };

    auto fLabel = txt.Format(ui::kFontText, ui::kCaption * s, DWRITE_FONT_WEIGHT_NORMAL);
    auto fRow = txt.Format(ui::kFontText, ui::kBody * s, DWRITE_FONT_WEIGHT_NORMAL);
    auto fName = txt.Format(ui::kFontText, ui::kCaption * s, DWRITE_FONT_WEIGHT_NORMAL);

    // "All apps ›" on the home view, "‹ Back" on the list.
    if (m_query.empty()) {
        auto fChevron = txt.Format(ui::kFontIcons, ui::kCaption * s, DWRITE_FONT_WEIGHT_NORMAL);
        auto r = at(m_allAppsRect);
        const float right = r.right - 8 * s;
        if (m_allApps) {
            float w = txt.Width(L"Back", fLabel.get());
            txt.Draw(dc, L"Back", fLabel.get(), R(right - w - 2 * s, r.top, right, r.bottom), soft.get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
            txt.Draw(dc, L"\uE76B", fChevron.get(), R(right - w - 18 * s, r.top, right - w - 6 * s, r.bottom), soft.get(),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            txt.Draw(dc, L"\uE76C", fChevron.get(), R(right - 12 * s, r.top, right, r.bottom), soft.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            txt.Draw(dc, L"All apps", fLabel.get(), R(r.left, r.top, right - 16 * s, r.bottom), soft.get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
    }

    if (Listing()) {
        if (ListHeader()) txt.Draw(dc, L"All apps", fLabel.get(), at(m_appsLabel), dim.get());
        if (m_results.empty())
            txt.Draw(dc, m_query.empty() ? std::wstring(L"No apps found") : L"No results for \"" + m_query + L"\"", fRow.get(),
                     at(R(kPad * s, ListTop(), m_lw - kPad * s, ListTop() + kResultRowH * s)), dim.get(),
                     DWRITE_TEXT_ALIGNMENT_LEADING, true);
        // A thin thumb in the right margin when there is more than fits.
        const int total = static_cast<int>(m_matches.size()), rows = ListRows();
        if (total > rows) {
            const float track = rows * kResultRowH * s, h = std::max(ui::kThumbMin * s, track * rows / total);
            const float y = ListTop() + (track - h) * m_scroll / (total - rows), x = m_lw - (kPad - ui::kS2) * s + ui::kThumbW * s;
            winrt::com_ptr<ID2D1SolidColorBrush> thumb;
            dc->CreateSolidColorBrush(pal.dim, thumb.put());
            thumb->SetOpacity(0.5f);
            dc->FillRoundedRectangle(D2D1::RoundedRect(at(R(x, y, x + ui::kThumbW * s, y + h)), ui::kThumbW * s / 2, ui::kThumbW * s / 2), thumb.get());
        }
        return;
    }

    if (m_showApps) txt.Draw(dc, L"Recent apps", fLabel.get(), at(m_appsLabel), dim.get());
    if (m_showApps && m_apps.empty())
        txt.Draw(dc, L"Apps you open show up here", fRow.get(),
                 at(R(m_appsLabel.left, m_appsLabel.bottom + 6 * s, m_appsLabel.right, m_appsLabel.bottom + 46 * s)), dim.get());
    if (m_showFiles) txt.Draw(dc, L"Recent files", fLabel.get(), at(m_filesLabel), dim.get());
    if (m_showFiles && m_files.empty())
        txt.Draw(dc, L"No recent files", fRow.get(),
                 at(R(m_filesLabel.left, m_filesLabel.bottom + 6 * s, m_filesLabel.right, m_filesLabel.bottom + 34 * s)), dim.get());
    {
        namespace tk = start::tk;
        // App names under their icons, in the six-column grid (empty slots stay empty; the grid keeps its height).
        auto fApp = txt.Format(ui::kFontText, tk::kCap * s, DWRITE_FONT_WEIGHT_NORMAL);
        auto fFile = txt.Format(ui::kFontText, tk::kText * s, DWRITE_FONT_WEIGHT_NORMAL);
        const float colW = (m_lw - m_gap - 2 * kPad * s) / kH2AppCols;
        for (size_t i = 0; i < m_apps.size(); i++) {
            auto const& c = m_appCells[i];
            const float cx = (c.left + c.right) / 2;
            txt.Draw(dc, AppName(m_apps[i]), fApp.get(),
                     at(R(cx - colW / 2 + ui::kS1 * s, c.bottom + tk::kAppNameGap * s, cx + colW / 2 - ui::kS1 * s, c.bottom + (tk::kAppNameGap + kLabelH) * s)),
                     soft.get(), DWRITE_TEXT_ALIGNMENT_CENTER, true);
        }
        // Recent files: a 40 px row each: a 16 px icon (a sprite) at the left, the name, and a right-aligned caption (when).
        for (size_t i = 0; i < m_files.size(); i++) {
            auto r = at(m_fileCells[i]);
            const std::wstring when = FileCaption(m_files[i].time);
            const float capW = when.empty() ? 0.0f : txt.Width(when, fApp.get()) + ui::kS2 * s;
            const float nameL = r.left + (ui::kS2 + tk::kFileIcon + ui::kS3) * s, capR = r.right - ui::kS3 * s;
            txt.Draw(dc, m_files[i].name, fFile.get(), R(nameL, r.top, capR - capW, r.bottom), white.get(), DWRITE_TEXT_ALIGNMENT_LEADING, true);
            if (capW > 0) txt.Draw(dc, when, fApp.get(), R(capR - capW, r.top, capR, r.bottom), dim.get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
    }
}

void Launcher::DrawMenu(ID2D1DeviceContext* dc, float ox, float oy) {
    const float s = m_s;
    Text txt{m_dwrite};
    dc->Clear(D2D1::ColorF(0, 0.0f));
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    winrt::com_ptr<ID2D1SolidColorBrush> white;
    dc->CreateSolidColorBrush(theme::P().text, white.put());
    auto fRow = txt.Format(ui::kFontText, ui::kBody * s, DWRITE_FONT_WEIGHT_NORMAL);
    winrt::com_ptr<ID2D1SolidColorBrush> dim;
    dc->CreateSolidColorBrush(theme::Muted(), dim.put());
    auto fHint = txt.Format(ui::kFontText, ui::kCaption * s, DWRITE_FONT_WEIGHT_NORMAL);
    for (int i = 0; i < kMenuCount; i++) {
        auto r = m_menuRows[i];
        txt.Draw(dc, kMenuItems[i], fRow.get(), R(r.left + ui::kS3 * s + ox, r.top + oy, r.right + ox, r.bottom + oy), white.get());
        if (i == kMenuTaskbar)  // the shortcut, so the way back is learnt at the one place that offers it
            txt.Draw(dc, kMenuTaskbarHint, fHint.get(), R(r.left + ox, r.top + oy, r.right - ui::kS3 * s + ox, r.bottom + oy), dim.get(),
                     DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
}

// ---- Showing and hiding -----------------------------------------------------------------------

void Launcher::Animate(wuc::CompositionObject const& target, wchar_t const* prop, float to, int ms) {
    auto a = m_comp.CreateScalarKeyFrameAnimation();
    a.InsertKeyFrame(1.0f, to, m_easeOut);
    a.Duration(std::chrono::milliseconds(ms));
    target.StartAnimation(prop, a);
}

void Launcher::Show(LauncherAnchor const& anchor) {
    LARGE_INTEGER t1, fq;
    QueryPerformanceCounter(&m_showAt);
    m_anchor = anchor;
    m_s = anchor.scale;
    m_query.clear();
    m_matches.clear();
    m_results.clear();
    m_sel = 0;
    m_scroll = 0;
    m_allApps = false;
    m_activated = false;
    m_pressedIndex = -1;  // a press from the last visit must not complete a click on this one
    m_pressedRowKey.clear();
    m_wheel = 0;
    ++m_gen;
    m_detail = Detail::None;  // halo2: the right column starts on its home controls, with the keyboard on search
    m_col = 0;
    m_ccScroll = 0;
    m_ccFocus = m_ccHover = -1;
    m_drag = 0;
    m_demo = false;
    m_menuOpen = false;
    ApplyRecent(false);

    // Everything slow (Recent files, their icons, the account picture, the app index) is cached; Refresh() updates it after the show.
    try {
        Build();
    } catch (winrt::hresult_error const& e) {
        Log(L"build FAILED " + std::wstring(e.message()));
        return;
    }
    LARGE_INTEGER built;
    QueryPerformanceCounter(&built);

    // Rises out of the gap above the dock with the dock's reveal spring (clipped by the window's bottom edge).
    const float home = kTop * m_s;
    const float2 rise{0, ui::kEnterTravel * m_s};  // a short rise as it fades in, just above the dock
    auto const& st = settings::Get().start;
    const float2 corner{CornerX(), 1.0f};  // the swoop grows from just above the dock (the bottom centre; in 2.0 the dock is under the panel's middle-left)
    if (m_h2 && DockLikeMotion()) {
        // Parked below the window edge (invisible) while the freshly drawn surfaces reach the GPU: starting the spring
        // in the same frame put that one-off hitch into the first, fastest part of the motion.
        ParkBelow(m_panel, {0, home, 0});
        if (m_shadowHold) ParkBelow(m_shadowHold, PanelHome());
        SetTimer(m_hwnd, kOpenTimer, 34, nullptr);
    } else if (m_h2) {
        OpenMotion(m_panel, {0, home, 0}, true);
        if (m_shadowHold) OpenMotion(m_shadowHold, PanelHome(), true);
    } else {
        motion::In(m_comp, m_panel, {0, home, 0}, rise, st, corner);
        if (m_shadowHold) motion::In(m_comp, m_shadowHold, PanelHome(), rise, st, corner);
    }
    if (!(m_h2 && DockLikeMotion())) SetTimer(m_hwnd, kSettleTimer, SettleDelayMs(), nullptr);  // the other path arms it from kOpenTimer, once the spring starts
    ShowDim(st.dimDesktop);
    Place();  // shows the window: after the slide is set up, so no frame shows the panel at rest first
    SetTimer(m_hwnd, kRefreshTimer, 900, nullptr);  // once, after the spring has settled (a rebuild mid-motion is a visible hitch)
    ArmClock();

    m_open = true;
    // Build establishes m_h2 on the first opening. Yield before binding devices,
    // so synchronous audio setup cannot hold back Show and its compositor commit.
    // Nothing heavy while the panel moves: every service answer and every notification snapshot redraws a surface and uploads it
    // to the GPU, and those frames land in the middle of the spring. 2.0 waits until the motion has settled (the notification
    // column and the control centre show what they had last time until then).
    const auto bindDelay = m_h2 ? 600u : std::max<UINT>(USER_TIMER_MINIMUM, static_cast<UINT>(std::lround(motion::kFadeInMs / settings::Speed(st))));
    SetTimer(m_hwnd, kServicesTimer, bindDelay, nullptr);
    Focus();
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    auto ms = [&](LARGE_INTEGER a, LARGE_INTEGER b) { return std::to_wstring((b.QuadPart - a.QuadPart) * 10000 / fq.QuadPart / 10.0); };
    Log(L"shown, " + std::to_wstring(m_apps.size()) + L" apps, " + std::to_wstring(m_files.size()) + L" files, build " +
        ms(m_showAt, built) + L" ms, open latency Show()->window up and focused " + ms(m_showAt, t1) + L" ms");
}

// The Win key arrives from a hook thread, so Windows may refuse the foreground change; borrowing the
// foreground thread's input queue is the standard way around that.
void Launcher::Focus() {
    if (EnvIs(L"HALO_TEST_NOFOCUS")) return;  // test runs must never take the keyboard from whoever is typing
    DWORD me = GetCurrentThreadId(), fg = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    bool attached = fg && fg != me && AttachThreadInput(me, fg, TRUE);
    SetForegroundWindow(m_hwnd);
    SetFocus(m_hwnd);
    if (attached) AttachThreadInput(me, fg, FALSE);
    if (GetForegroundWindow() == m_hwnd) m_activated = true;  // else WM_ACTIVATE may still arrive and set it
    else Log(L"panel could NOT take focus");
}

void Launcher::Hide() {
    if (!m_open) return;
    m_open = false;
    KillTimer(m_hwnd, kOpenTimer);
    KillTimer(m_hwnd, kSettleTimer);
    KillTimer(m_hwnd, kRehoverTimer);
    if (m_wave) m_wave.InsertScalar(L"Intensity", 0.0f);
    m_waveOn = false;
    WaveStop();
    if (m_notifCol) m_notifCol->SetActive(false);
    unsigned gen = ++m_gen;
    auto batch = m_comp.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
    const float2 rise{0, ui::kEnterTravel * m_s};
    auto const& st = settings::Get().start;
    const float2 corner{CornerX(), 1.0f};
    if (m_h2) {
        OpenMotion(m_panel, {0, kTop * m_s, 0}, false);
        if (m_shadowHold) OpenMotion(m_shadowHold, PanelHome(), false);
    } else {
        motion::Out(m_comp, m_panel, {0, kTop * m_s, 0}, rise, st, corner);
        if (m_shadowHold) motion::Out(m_comp, m_shadowHold, PanelHome(), rise, st, corner);
    }
    batch.End();
    batch.Completed([this, gen](auto&&, auto&&) {
        if (gen == m_gen && !m_open) Close();
    });
    Log(L"hiding");
}

// The dock's reveal for Start (see OpenMotion): the Bounce and Rise effects slide the panel in, they do not fade it.
bool Launcher::DockLikeMotion() const {  // every motion but None: they all start a beat after the window is up
    return !settings::Get().reduceMotion && settings::Get().start.effect != settings::Effect::None;
}

void Launcher::ParkBelow(wuc::Visual const& v, float3 rest) {
    v.StopAnimation(L"Offset");
    v.StopAnimation(L"Offset.Y");
    v.StopAnimation(L"Opacity");
    v.Opacity(0.0f);
    const bool rises = settings::Get().start.effect == settings::Effect::Bounce || settings::Get().start.effect == settings::Effect::Rise;
    v.Offset({rest.x, rest.y + (rises ? OpenTravel() : 0.0f), rest.z});
    v.StopAnimation(L"Scale");
    v.Scale({1, 1, 1});  // never scaled: a scale animation on three blurred panes drops the open to ~30 fps
}

// How long after the motion starts it is certainly over: 1 s, longer when the speed setting slows it down.
UINT Launcher::SettleDelayMs() const {
    return static_cast<UINT>(std::clamp(1000.0f / settings::Speed(settings::Get().start), 1000.0f, 2500.0f));
}

// A finished animation still counts as an animated property to the compositor, which then re-composes the whole panel
// (three blurred panes) every other refresh for as long as it is open. Cancel them and pin the end values.
void Launcher::SettleMotion() {
    auto pin = [&](wuc::Visual const& v, float3 rest) {
        if (!v) return;
        v.StopAnimation(L"Offset");
        v.StopAnimation(L"Offset.Y");
        v.StopAnimation(L"Opacity");
        v.StopAnimation(L"Scale");
        v.Opacity(1.0f);
        v.Scale({1, 1, 1});
        v.Offset(rest);
    };
    pin(m_panel, {0, kTop * m_s, 0});
    pin(m_shadowHold, PanelHome());
}

// How far Bounce / Rise start below their resting place: the shared panel travel (16 px), not the panel's height.
float Launcher::OpenTravel() const {
    return ui::kEnterTravel * m_s * std::clamp(settings::Amount(settings::Get().start), 0.8f, 1.5f);
}

// HALO 2.0 open and close: a bounded ease-out rise and fade; closing, it sinks and fades
// from the current compositor pose, including when the entrance is interrupted. Only offset and opacity
// animate, on the compositor: the glass behind is never resized or scaled. speed scales the times, motion the travel;
// reduce_motion and the fade effect do not move at all.
void Launcher::OpenMotion(wuc::Visual const& v, float3 rest, bool in) {
    auto const& st = settings::Get().start;
    const bool reduce = settings::Get().reduceMotion;
    if (m_h2 && !reduce && (st.effect == settings::Effect::Bounce || st.effect == settings::Effect::Rise || st.effect == settings::Effect::Fade)) {
        // Two clean motions. Fade: opacity only, nothing moves. Bounce / Rise: a short rise on a spring that overshoots a
        // little, with a quick fade so it never pops in. Both start a beat after the window is up (see Show) so the
        // window's first-frame hitch happens while the panel is still invisible.
        const float speed = settings::Speed(st);
        const bool rises = st.effect != settings::Effect::Fade;
        const float travel = OpenTravel();
        auto ms = [&](float x) { return std::chrono::milliseconds(std::max(1, static_cast<int>(std::lround(x / speed)))); };
        auto outCurve = m_comp.CreateCubicBezierEasingFunction({0.0f, 0.0f}, {0.2f, 1.0f});
        const bool bounce = st.effect == settings::Effect::Bounce;
        if (in) {
            v.StopAnimation(L"Offset");
            v.StopAnimation(L"Offset.Y");
            v.StopAnimation(L"Opacity");
            v.StopAnimation(L"Scale");
            v.Scale({1, 1, 1});
            v.Opacity(0.0f);
            v.Offset({rest.x, rest.y + (rises ? travel : 0.0f), rest.z});
            auto fade = m_comp.CreateScalarKeyFrameAnimation();
            fade.InsertKeyFrame(0.0f, 0.0f);
            fade.InsertKeyFrame(1.0f, 1.0f, outCurve);
            fade.Duration(ms(static_cast<float>(ui::kPanelEnterMs)));
            v.StartAnimation(L"Opacity", fade);
            if (rises) {
                // The shared panel spring: it rises past its place by ~a sixth of the 16 px (Bounce) or a little (Rise) and settles.
                const float zeta = bounce ? ui::kPanelSpringDamping : ui::kPanelRiseDamping;
                auto spring = m_comp.CreateSpringScalarAnimation();
                spring.FinalValue(rest.y);
                spring.DampingRatio(zeta);
                spring.Period(ms(static_cast<float>(ui::kPanelSpringMs)));
                v.StartAnimation(L"Offset.Y", spring);
            }
        } else {
            auto down = m_comp.CreateCubicBezierEasingFunction({0.5f, 0.0f}, {0.9f, 0.5f});
            auto fade = m_comp.CreateScalarKeyFrameAnimation();
            fade.InsertKeyFrame(1.0f, 0.0f, down);
            fade.Duration(ms(static_cast<float>(ui::kPanelExitMs)));
            v.StartAnimation(L"Opacity", fade);
            if (rises) {
                auto slide = m_comp.CreateScalarKeyFrameAnimation();
                slide.InsertKeyFrame(1.0f, rest.y + travel, down);  // sinks the way it came, accelerating
                slide.Duration(ms(static_cast<float>(ui::kPanelExitMoveMs)));
                v.StartAnimation(L"Offset.Y", slide);
            }
        }
        return;
    }
    // The compact Halo 2 transition handles rise and fade without scaling the
    // glass. Other selected effects use the same compositor motion as panels.
    if (!reduce && (st.effect == settings::Effect::Bounce || st.effect == settings::Effect::Swipe || st.effect == settings::Effect::Swoop)) {
        const float2 travel{0, ui::kEnterTravel * m_s};
        const float2 corner{CornerX(), 1.0f};
        if (in) motion::In(m_comp, v, rest, travel, st, corner);
        else motion::Out(m_comp, v, rest, travel, st, corner);
        return;
    }
    const float speed = settings::Speed(st);
    auto ms = [&](float x) { return std::chrono::milliseconds(std::max(1, static_cast<int>(std::lround(x / speed)))); };
    const float fadeInMs = m_h2 ? static_cast<float>(ui::kPanelEnterMs) : static_cast<float>(motion::kFadeInMs),
                outMs = m_h2 ? static_cast<float>(ui::kPanelExitMs) : static_cast<float>(motion::kOutMs);
    v.StopAnimation(L"Scale");
    v.Scale({1, 1, 1});
    if (st.effect == settings::Effect::None) {
        v.StopAnimation(L"Offset");
        v.StopAnimation(L"Opacity");
        v.Offset(rest);
        v.Opacity(in ? 1.0f : 0.0f);
        return;
    }
    const float travel = reduce || st.effect == settings::Effect::Fade ? 0.0f : ui::kEnterTravel * m_s * settings::Amount(st);
    if (in) {
        v.StopAnimation(L"Offset");
        v.StopAnimation(L"Opacity");
        v.Offset({rest.x, rest.y + travel, rest.z});
        v.Opacity(0);
        if (travel != 0) {
            auto slide = m_comp.CreateVector3KeyFrameAnimation();
            slide.InsertKeyFrame(0.0f, {rest.x, rest.y + travel, rest.z});
            slide.InsertKeyFrame(1.0f, rest, m_easeOut);
            slide.Duration(ms(fadeInMs));
            v.StartAnimation(L"Offset", slide);
        } else {
            v.Offset(rest);
        }
        auto fade = m_comp.CreateScalarKeyFrameAnimation();
        fade.InsertKeyFrame(0.0f, 0.0f);
        fade.InsertKeyFrame(1.0f, 1.0f, m_easeOut);
        fade.Duration(ms(fadeInMs));
        v.StartAnimation(L"Opacity", fade);
    } else {
        auto down = m_comp.CreateCubicBezierEasingFunction({0.5f, 0.0f}, {0.9f, 0.5f});
        if (travel != 0) {
            auto slide = m_comp.CreateVector3KeyFrameAnimation();
            slide.InsertKeyFrame(1.0f, {rest.x, rest.y + travel, rest.z}, down);
            slide.Duration(ms(outMs));
            v.StartAnimation(L"Offset", slide);
        }
        auto fade = m_comp.CreateScalarKeyFrameAnimation();
        fade.InsertKeyFrame(1.0f, 0.0f, down);
        fade.Duration(ms(outMs));
        v.StartAnimation(L"Opacity", fade);
    }
}

// HALO 2.0: while hidden the panel holds nothing: the glass, the shadow and every surface go (the next Show builds them
// again in ~20 ms), so a closed Start costs the memory it had before it was ever opened.
void Launcher::ReleaseVisuals() {
    if (!m_h2 || !m_root) return;
    if (m_notifCol) m_notifCol->Release();
    m_notifRoot = nullptr;
    m_root.Children().RemoveAll();
    m_appSprites.clear();
    m_waveItems.clear();
    m_waveLive = false;
    m_fileSprites.clear();
    m_hoverIcon = nullptr;
    m_fileBrushes.clear();
    m_panel = m_content = m_shadowHold = m_menu = m_menuOuter = m_bodyIcons = m_listClip = m_listHold = m_listIcons = m_homeHold = m_ccClip = m_ccHold = m_usageVisual = nullptr;
    m_searchSurface = m_bodySurface = m_headerSurface = m_listSurface = m_ccSurface = m_usageSurface = m_footSurface = nullptr;
    m_hoverBrush = m_ccPlateBrush = nullptr;
    m_hoverSprite = nullptr;
    m_menuGlassHold = nullptr;
    m_menuGlassBuilt = false;
    m_caret = nullptr;
    m_wave = nullptr;
    m_hover = m_menuHover = m_ccPlate = m_sliderShapes = nullptr;
    m_hoverShape = m_menuHoverShape = m_ccPlateShape = m_brTrack = m_brFill = m_vlTrack = m_vlFill = nullptr;
    m_ccPlateSprite = nullptr;
    m_listInset = nullptr;
}

void Launcher::Close() {
    ShowDim(false);
    StopServices();
    ShowWindow(m_hwnd, SW_HIDE);
    if (m_caret) m_caret.StopAnimation(L"Opacity");  // nothing animates while hidden
    KillTimer(m_hwnd, kCaretTimer);
    KillTimer(m_hwnd, kWaveOffTimer);
    KillTimer(m_hwnd, kSettleTimer);
    ReleaseVisuals();
    m_tracking = false;
    if (m_host.closed) m_host.closed();
}

void Launcher::Raise() {
    if (m_open) SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Window position and size for the current layout; the test points are where a test can click (client px).
void Launcher::Place() {
    int x = static_cast<int>(PanelX());
    int margin = static_cast<int>(kTop * m_s);
    int mx = static_cast<int>(m_shadowM), mb = static_cast<int>(m_shadowB);
    int top = m_anchor.bottom - static_cast<int>(m_h) - margin;
    int winW = static_cast<int>(PanelW()) + 2 * mx, winH = static_cast<int>(m_h) + margin + mb;
    SetWindowPos(m_hwnd, HWND_TOPMOST, x - mx, top, winW, winH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (EnvIs(L"HALO_TEST")) {
        auto mid0 = [&](D2D1_RECT_F const& r) {
            return std::to_wstring(static_cast<int>((r.left + r.right) / 2 + m_shadowM + m_nw)) + L"," +
                   std::to_wstring(static_cast<int>((r.top + r.bottom) / 2 + kTop * m_s));
        };
        if (m_h2 && !m_ccBase.empty()) {  // the right column's controls, for the screenshot drivers
            auto cc = [&](Kind k, int i) {
                for (auto const& b : m_ccBase)
                    if (b.kind == k && b.index == i) return mid0(b.r);
                return std::wstring(L"0,0");
            };
            Log(L"cc points: wifi=" + cc(Kind::CcWifi, 0) + L" wifimore=" + cc(Kind::CcWifiMore, 0) + L" air=" + cc(Kind::CcQuick, 0) + L" hot=" +
                cc(Kind::CcQuick, 1) + L" dnd=" + cc(Kind::CcQuick, 2) + L" mic=" + cc(Kind::CcQuick, 5) + L" bright=" + cc(Kind::CcBright, 0) +
                L" vol=" + cc(Kind::CcVol, 0) + L" outmore=" + cc(Kind::CcOutMore, 0) + L" play=" + cc(Kind::CcPlay, 0) + L" seg0=" +
                mid0(m_modeSeg[0]) + L" seg2=" + mid0(m_modeSeg[2]));
        }
        auto mid = [&](D2D1_RECT_F const& r) {
            return std::to_wstring(static_cast<int>((r.left + r.right) / 2 + m_shadowM + m_nw)) + L"," +
                   std::to_wstring(static_cast<int>((r.top + r.bottom) / 2 + kTop * m_s));
        };
        auto none = std::wstring(L"0,0");
        Log(L"test points: power=" + mid(m_powerRect) + L" settings=" + mid(m_settingsRect) + L" app0=" +
            (m_appCells.empty() ? none : mid(m_appCells[0])) + L" app1=" + (m_appCells.size() < 2 ? none : mid(m_appCells[1])) +
            L" menu2=" + mid(m_menuHits[2].r) + L" blank=" + mid(m_divider) + L" file0=" +
            (m_fileCells.empty() ? none : mid(m_fileCells[0])) + L" file7=" + (m_fileCells.size() < 8 ? none : mid(m_fileCells[7])) +
            L" result2=" + mid(R(kPad * m_s, m_bodyTop + (4 + 2 * kResultRowH) * m_s, m_w - kPad * m_s,
                                 m_bodyTop + (4 + 3 * kResultRowH) * m_s)));
    }
}

// The file icon cache key: the extension, or the whole path for types whose icon is per file.
std::wstring Launcher::IconKey(std::wstring const& path) {
    auto dot = path.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? L"" : Lower(path.substr(dot));
    bool own = ext.empty() || ext == L".exe" || ext == L".lnk" || ext == L".ico" || ext == L".scr" || ext == L".cpl";
    return own ? Lower(path) : ext;
}

// The slow content: Recent files, their icons, the account picture and name, the app index (once). Run at startup and after each Show.
void Launcher::Prefetch() {
    Refresh();
    if (!m_indexed) BuildIndex();
    else RefreshIndex();
    wchar_t name[256] = {};
    ULONG n = ARRAYSIZE(name);
    if (!GetUserNameExW(kNameDisplay, name, &n) || !name[0]) {
        n = ARRAYSIZE(name);
        GetUserNameW(name, reinterpret_cast<LPDWORD>(&n));
    }
    m_user = name[0] ? name : L"User";
    if (wchar_t over[64]; GetEnvironmentVariableW(L"HALO_TEST_USER", over, 64)) {  // tools/start_preview: a stand-in name, and no account picture
        m_user = over;
        m_avatarTried = true;
        m_avatar.clear();
    }
    if (!m_avatarTried) {
        m_avatarTried = true;
        std::wstring pic = AccountPicture(96);
        if (pic.empty() || !LoadImageFile(pic, m_avatar, m_avatarPx)) m_avatar.clear();
        Log(pic.empty() ? L"no account picture, using an initial" : L"account picture " + pic);
    }
}

// Rebuilds in place at the resting pose (no animation), for content that changed under an open panel.
void Launcher::Relayout() {
    if (!m_open || m_menuOpen) return;
    try {
        Build();
    } catch (winrt::hresult_error const& e) {
        Log(L"rebuild FAILED " + std::wstring(e.message()));
        return;
    }
    m_panel.Offset({0, kTop * m_s, 0});
    Place();
}

// After the panel is up: refresh the cached content, and rebuild only if something shows differently.
void Launcher::Refresh() {
    ApplyRecent();
    auto state = m_recentRefresh;
    {
        std::lock_guard lock(state->mutex);
        if (state->busy) return;
        state->busy = true;
        state->notify = m_hwnd;
    }
    try {
        std::thread([state, icons = m_fileIcons]() mutable {
            auto co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            std::vector<FileEntry> files;
            bool ok = SUCCEEDED(co);
            if (ok) {
                try {
                    files = LoadFiles();
                    std::map<std::wstring, IconImage> fresh;
                    for (auto const& f : files) {
                        { std::lock_guard lock(state->mutex); if (!state->notify) { ok = false; break; } }
                        auto key = IconKey(f.path);
                        if (fresh.contains(key)) continue;
                        if (auto it = icons.find(key); it != icons.end()) fresh.emplace(key, std::move(it->second));
                        else {
                            IconImage image;
                            if (!LoadJumboIcon(f.path, image) || !image.width) image = {};
                            fresh.emplace(key, std::move(image));
                        }
                    }
                    icons = std::move(fresh);
                } catch (...) { ok = false; }
                CoUninitialize();
            }
            std::lock_guard lock(state->mutex);
            state->busy = false;
            if (!ok || !state->notify) return;
            state->files = std::move(files);
            state->icons = std::move(icons);
            state->ready = true;
            PostMessageW(state->notify, kMsgRecent, 0, 0);
        }).detach();
    } catch (...) {
        std::lock_guard lock(state->mutex);
        state->busy = false;
        Log(L"recent refresh worker unavailable");
    }
}

void Launcher::ApplyRecent(bool redraw) {
    if (m_menuOpen) return;  // keep the visible rows and hit arrays on the same snapshot until the menu closes
    std::wstring before;
    for (auto const& f : m_allFiles) before += f.path + L"|";
    bool iconsChanged = false;
    {
        std::lock_guard lock(m_recentRefresh->mutex);
        if (!m_recentRefresh->ready) return;
        m_recentRefresh->ready = false;
        auto const& icons = m_recentRefresh->icons;
        iconsChanged = icons.size() != m_fileIcons.size() ||
            !std::equal(icons.begin(), icons.end(), m_fileIcons.begin(), [](auto const& a, auto const& b) {
                return a.first == b.first && a.second.width == b.second.width &&
                    a.second.height == b.second.height && a.second.bgra == b.second.bgra;
            });
        m_allFiles = std::move(m_recentRefresh->files);
        m_fileIcons = std::move(m_recentRefresh->icons);
    }
    m_files.assign(m_allFiles.begin(), m_allFiles.begin() + std::min<size_t>(m_allFiles.size(), m_h2 ? kH2Files : kMaxFiles));
    if (iconsChanged) m_fileBrushes.clear();
    std::wstring after;
    for (auto const& f : m_allFiles) after += f.path + L"|";
    if (after != before) {
        Log(L"refreshed in the background, rebuilding");
        RunSearch();
    }
    if (redraw && m_open && (after != before || iconsChanged)) Relayout();
}

// ---- Search -------------------------------------------------------------------------------------

// Apps, Settings pages and recent files whose names match the query: names that start with it first, then names
// with a word that does, then the rest (a Settings page also matches on the other words it is known by, never
// first); shorter names win ties, then apps, Settings, files. With no query, All apps is every app, A to Z.
void Launcher::RunSearch() {
    m_matches.clear();
    std::wstring q = Lower(m_query);
    while (!q.empty() && q.back() == L' ') q.pop_back();
    if (q.empty()) {
        if (m_allApps)
            for (auto const& a : m_index) m_matches.push_back({Result::Type::App, a.name, a.path});
        ShowMatches();
        return;
    }
    struct Scored {
        int rank;
        size_t len;
        Result r;
    };
    std::vector<Scored> hits;
    for (auto const& a : m_index)
        if (int rk = MatchRank(a.lname, q); rk >= 0) hits.push_back({rk, a.name.size(), {Result::Type::App, a.name, a.path}});
    for (auto const& p : kSettingsPages) {
        int rk = MatchRank(Lower(p.name), q);
        if (rk < 0) rk = MatchRank(L" " + std::wstring(p.words), q);  // the leading space: a word start ranks 1, never 0
        if (rk >= 0) {
            if (q == L"settings" && wcscmp(p.uri, L"ms-settings:") == 0) rk = -1;
            hits.push_back({rk, wcslen(p.name), {Result::Type::Setting, p.name, p.uri}});
        }
    }
    for (auto const& f : m_allFiles)
        if (int rk = MatchRank(Lower(f.name), q); rk >= 0) hits.push_back({rk, f.name.size(), {Result::Type::File, f.name, f.path}});
    std::stable_sort(hits.begin(), hits.end(), [](Scored const& a, Scored const& b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        if (a.len != b.len) return a.len < b.len;
        return a.r.type < b.r.type;
    });
    for (auto& h : hits) m_matches.push_back(std::move(h.r));
    ShowMatches();  // Refresh() re-runs this: the scroll and selection hold where they can
}

float Launcher::ListTop() const { return ListHeader() ? m_appsLabel.bottom + 6 * m_s : m_bodyTop + 4 * m_s; }

int Launcher::ListRows() const {
    return std::max(1, static_cast<int>(std::floor((m_bodyBottom - ListTop()) / (kResultRowH * m_s))));
}

void Launcher::ShowMatches() {
    const int total = static_cast<int>(m_matches.size()), rows = ListRows();
    m_scroll = std::clamp(m_scroll, 0, std::max(0, total - rows));
    m_results.assign(m_matches.begin() + m_scroll, m_matches.begin() + std::min(total, m_scroll + rows));
    m_sel = std::clamp(m_sel, 0, std::max(0, static_cast<int>(m_results.size()) - 1));
}

// Moves the list by whole rows; the selected item stays selected while it is on screen.
void Launcher::Scroll(int rows) {
    const int before = m_scroll;
    m_scroll += rows;
    ShowMatches();
    if (m_scroll == before) return;
    m_sel = std::clamp(m_sel - (m_scroll - before), 0, std::max(0, static_cast<int>(m_results.size()) - 1));
    // The rows start where they were (plus what is left of a slide still running) and glide to their new place.
    const float rowPx = kResultRowH * m_s, limit = kListBuffer * rowPx;
    const float from = std::clamp(ScrollLeft() + (m_scroll - before) * rowPx, -limit, limit);
    BuildBody();
    const float y = ListTop() - limit;
    auto slide = m_comp.CreateScalarKeyFrameAnimation();
    auto linear = m_comp.CreateLinearEasingFunction();
    for (int k = 0; k <= 8; k++) {  // an ease-out cubic, in steps ScrollLeft() can follow
        const float t = k / 8.0f;
        slide.InsertKeyFrame(t, y + from * std::pow(1 - t, 3.0f), linear);
    }
    slide.Duration(std::chrono::milliseconds(kScrollMs));
    m_listHold.StartAnimation(L"Offset.Y", slide);
    m_scrollFrom = from;
    QueryPerformanceCounter(&m_scrollAt);
}

float Launcher::ScrollLeft() const {
    if (!m_scrollFrom) return 0;
    LARGE_INTEGER now, fq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&fq);
    const float t = (now.QuadPart - m_scrollAt.QuadPart) * 1000.0f / fq.QuadPart / kScrollMs;
    return t >= 1 ? 0 : m_scrollFrom * std::pow(1 - t, 3.0f);
}

// The result rows, kListBuffer past each end of what shows: name, and what it is (except in All apps).
void Launcher::DrawList(ID2D1DeviceContext* dc, float ox, float oy) {
    const float s = m_s, rowPx = kResultRowH * s;
    Text txt{m_dwrite};
    dc->Clear(D2D1::ColorF(0, 0.0f));
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    auto const& pal = theme::P();
    winrt::com_ptr<ID2D1SolidColorBrush> white, dim;
    dc->CreateSolidColorBrush(pal.text, white.put());
    dc->CreateSolidColorBrush(MutedOf(m_h2), dim.put());
    auto fRow = txt.Format(ui::kFontText, ui::kBody * s, DWRITE_FONT_WEIGHT_NORMAL);
    auto fName = txt.Format(ui::kFontText, ui::kCaption * s, DWRITE_FONT_WEIGHT_NORMAL);
    static constexpr const wchar_t* kTypeNames[] = {L"App", L"Setting", L"File"};
    const int total = static_cast<int>(m_matches.size()), rows = ListRows();
    const float left = ox + (kPad - 8) * s, right = ox + m_lw - (kPad - 8) * s;
    for (int j = -kListBuffer; j < rows + kListBuffer; j++) {
        const int k = m_scroll + j;
        if (k < 0 || k >= total) continue;
        const float top = oy + (j + kListBuffer) * rowPx;
        txt.Draw(dc, m_matches[k].name, fRow.get(), R(left + (12 + kResultIcon + 12) * s, top, right - 72 * s, top + rowPx), white.get(),
                 DWRITE_TEXT_ALIGNMENT_LEADING, true);
        if (!ListHeader())  // every row of All apps is an app
            txt.Draw(dc, kTypeNames[static_cast<int>(m_matches[k].type)], fName.get(), R(right - 68 * s, top, right - 14 * s, top + rowPx),
                     dim.get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
}

void Launcher::ToggleAllApps() {
    m_allApps = !m_allApps;
    m_scroll = 0;
    m_sel = 0;
    RunSearch();
    BuildBody();
}

void Launcher::QueryChanged() {
    m_scroll = 0;
    m_sel = 0;
    RunSearch();
    SearchDraw();
    BuildBody();
}

// Launches an app / file, with the icon's bounce on its way out. The launch does not wait for the animation.
void Launcher::Activate(Kind kind, int index) {
    std::wstring path;
    bool app = false;
    if (kind == Kind::App && index >= 0 && index < static_cast<int>(m_apps.size())) {
        path = m_apps[index];
        app = true;
        if (index < static_cast<int>(m_appSprites.size())) BounceIcon(m_appSprites[index]);
    } else if (kind == Kind::File && index >= 0 && index < static_cast<int>(m_files.size())) {
        path = m_files[index].path;
        if (index < static_cast<int>(m_fileSprites.size())) BounceIcon(m_fileSprites[index]);
    } else if (kind == Kind::Result && index >= 0 && index < static_cast<int>(m_results.size())) {
        path = m_results[index].path;
        app = m_results[index].type == Result::Type::App;
    } else {
        return;
    }
    Hide();
    if (app && _wcsicmp(path.c_str(), kSettingsApp) != 0) {
        if (m_host.launch) m_host.launch(path);
    } else {
        OpenQuietly(path);
    }
}

void Launcher::OnKey(WPARAM vk) {
    if (m_h2 && !m_menuOpen) {  // Tab moves between the columns; the right one has its own keys
        if (vk == VK_TAB) {
            CcTab(GetKeyState(VK_SHIFT) < 0 ? -1 : 1);
            return;
        }
        if (m_col == 1) {
            if (vk == VK_ESCAPE) {
                if (m_detail != Detail::None) ToggleDetail(Detail::None);
                else Hide();
            } else {
                CcKey(vk);
            }
            return;
        }
    }
    if (m_h2 && !Listing() && !m_menuOpen && (vk == VK_NEXT || vk == VK_PRIOR || vk == VK_HOME || vk == VK_END)) {
        const float page = m_bodyBottom - m_bodyTop;
        ScrollHome(vk == VK_HOME ? -m_plan.contentBottom * m_s : vk == VK_END ? m_plan.contentBottom * m_s : vk == VK_NEXT ? page : -page);
        return;
    }
    switch (vk) {
        case VK_ESCAPE:  // closes the Power menu first, then clears the query, then leaves All apps, then the panel
            if (m_menuOpen) ToggleMenu(false);
            else if (!m_query.empty()) {
                m_query.clear();
                QueryChanged();
            } else if (m_allApps) ToggleAllApps();
            else if (m_detail != Detail::None) ToggleDetail(Detail::None);
            else Hide();
            break;
        case VK_RETURN:
            if (!m_menuOpen && !m_results.empty()) Activate(Kind::Result, m_sel);
            break;
        case VK_UP:
        case VK_DOWN: {  // past the first or last row, the list scrolls
            if (m_menuOpen || m_results.empty()) break;
            const int d = vk == VK_DOWN ? 1 : -1;
            if (m_sel + d < 0 || m_sel + d >= static_cast<int>(m_results.size())) Scroll(d);
            m_sel = std::clamp(m_sel + d, 0, static_cast<int>(m_results.size()) - 1);
            Highlight(&m_resultRows[m_sel], ui::kRadiusHover, false);
            break;
        }
    }
}

// ---- Pointer ------------------------------------------------------------------------------------

Launcher::Hit const* Launcher::HitAt(float x, float y) const {
    if (m_menuOpen)
        for (auto const& h : m_menuHits)
            if (In(h.r, x, y)) return &h;
    if (m_h2 && !m_menuOpen)
        if (auto const* cc = CcHitAt(x, y)) return cc;
    for (auto const& h : m_hits)
        if ((!m_menuOpen || h.kind == Kind::Power) && In(h.r, x, y)) return &h;  // the menu is modal to the panel
    return nullptr;
}

Launcher::IconV const* Launcher::IconOf(Hit const& hit) const {
    if (m_h2) return nullptr;  // 2.0: the wave drives the app icons (a spring on Scale would replace its expression)
    if (hit.kind == Kind::App && hit.index < static_cast<int>(m_appSprites.size()) && m_appSprites[hit.index].v)
        return &m_appSprites[hit.index];
    if (!m_h2 && hit.kind == Kind::File && hit.index < static_cast<int>(m_fileSprites.size()) && m_fileSprites[hit.index].v)
        return &m_fileSprites[hit.index];
    return nullptr;
}

// The hovered icon swells and lifts on the dock's kind of spring (it overshoots a little); leaving springs it back.
void Launcher::SpringIcon(IconV const& ic, bool up) {
    if (!ic.v) return;
    const float k = up ? kHoverScale : 1.0f, y = ic.restY - (up ? kHoverLift * m_s : 0.0f);
    auto scale = m_comp.CreateSpringVector3Animation();
    scale.FinalValue(Ref3({k, k, 1.0f}));
    scale.DampingRatio(kHoverDamping);
    scale.Period(std::chrono::milliseconds(kHoverPeriodMs));
    ic.v.StartAnimation(L"Scale", scale);
    auto lift = m_comp.CreateSpringScalarAnimation();
    lift.FinalValue(y);
    lift.DampingRatio(kHoverDamping);
    lift.Period(std::chrono::milliseconds(kHoverPeriodMs));
    ic.v.StartAnimation(L"Offset.Y", lift);
}

// A click: the icon hops up and settles on the dock's bounce curves while the panel leaves; it springs
// back from the press squash to the hover size.
void Launcher::BounceIcon(IconV const& ic) {
    if (!ic.v) return;
    auto up = m_comp.CreateCubicBezierEasingFunction({0.3f, 0.5f}, {0.5f, 1.0f});    // dock.cpp BounceUp
    auto down = m_comp.CreateCubicBezierEasingFunction({0.5f, 0.0f}, {0.7f, 0.5f});  // dock.cpp BounceDown
    const float rest = ic.restY - kHoverLift * m_s;
    auto hop = m_comp.CreateScalarKeyFrameAnimation();
    hop.InsertKeyFrame(0.5f, rest - kBounceRise * m_s, up);
    hop.InsertKeyFrame(1.0f, rest, down);
    hop.Duration(std::chrono::milliseconds(kBounceMs));
    ic.v.StartAnimation(L"Offset.Y", hop);
    auto scale = m_comp.CreateSpringVector3Animation();
    scale.FinalValue(Ref3({kHoverScale, kHoverScale, 1.0f}));
    scale.DampingRatio(kHoverDamping);
    scale.Period(std::chrono::milliseconds(kHoverPeriodMs));
    ic.v.StartAnimation(L"Scale", scale);
}

// Moves the single hover plate to the item under the pointer (or fades it away). It glides between items on a
// spring, like the dock's magnification, rather than easing linearly.
void Launcher::Highlight(D2D1_RECT_F const* r, float radius, bool inMenu) {
    auto shape = inMenu ? m_menuHoverShape : m_hoverShape;
    auto visual = inMenu ? m_menuHover : m_hover;
    bool& on = inMenu ? m_menuHoverOn : m_hoverOn;
    if (!r) {
        if (on) Animate(visual, L"Opacity", 0.0f, ui::kFastMs);
        on = false;
        return;
    }
    float2 off{r->left, r->top}, size{r->right - r->left, r->bottom - r->top};
    if (!inMenu) m_plateTo = *r;
    shape.CornerRadius({radius * m_s, radius * m_s});
    shape.StopAnimation(L"Offset");  // no glide between items: the plate is simply there (a gliding plate dragged on every scroll step)
    shape.StopAnimation(L"Size");
    shape.Offset(off);
    shape.Size(size);
    on = true;
    Animate(visual, L"Opacity", 1.0f, ui::kFastMs);
}

void Launcher::OnHover(float x, float y) {
    if (m_h2) MoveWave(x, y - kTop * m_s);
    auto const* hit = HitAt(x, y - kTop * m_s);
    {   // the right column has its own plate (it sits in the scrolling holder, so a scroll carries it along)
        int cc = -1;
        if (m_h2 && hit && !m_ccHits.empty() && hit >= m_ccHits.data() && hit < m_ccHits.data() + m_ccHits.size())
            cc = static_cast<int>(hit - m_ccHits.data());
        if (cc != m_ccHover) {
            m_ccHover = cc;
            CcPlateAt(cc);
        }
        if (cc >= 0) hit = nullptr;
    }
    int key = hit ? static_cast<int>(hit->kind) * 1000 + hit->index : -1;
    if (key == m_hoverIndex) return;
    m_hoverIndex = key;
    auto const* ic = hit ? IconOf(*hit) : nullptr;
    if (ic != m_hoverIcon) {
        if (m_hoverIcon) SpringIcon(*m_hoverIcon, false);
        if (ic) SpringIcon(*ic, true);
        m_hoverIcon = ic;
    }
    const bool results = !m_results.empty() && Listing();
    if (!hit) {
        if (results) Highlight(&m_resultRows[m_sel], ui::kRadiusHover, false);  // the plate stays on the selection
        else Highlight(nullptr, 0, false);
        Highlight(nullptr, 0, true);
    } else if (hit->kind == Kind::MenuItem) {
        auto r = m_menuRows[hit->index];
        Highlight(&r, ui::kRadiusHover, true);
    } else {
        D2D1_RECT_F r = hit->r;
        if (hit->kind == Kind::Result) m_sel = hit->index;
        Highlight(&r, hit->kind == Kind::PowerMode ? ui::kRadiusLarge : ui::kRadiusHover, false);  // one plate: the widget card is a card, all else a row/button
        if (m_menuOpen) Highlight(nullptr, 0, true);
    }
}

void Launcher::ToggleMenu(bool open) {
    if (open == m_menuOpen) return;
    m_menuOpen = open;
    m_hoverIndex = -2;
    Highlight(nullptr, 0, true);
    auto mv = m_h2 ? m_menuOuter : m_menu;
    if (open) {
        if (!m_menuGlassBuilt && m_menuGlassHold && m_host.glass) {  // first opening: build the menu's glass now (a closed menu has none)
            m_menuGlassBuilt = true;
            settings::Surface const& glassCfg = m_h2 ? static_cast<settings::Surface const&>(settings::Get().taskbar) : settings::Get().start;
            auto glass = m_host.glass(static_cast<int>(std::ceil(m_menuH)), glassCfg, static_cast<int>(std::ceil(m_menuW)), 0.0f);
            glass.Size({m_menuW, m_menuH});
            m_menuGlassHold.Children().InsertAtTop(glass);
        }
        mv.IsVisible(true);
        mv.Offset({m_menuX + m_nw, m_menuY + ui::kS2 * m_s, 0});
        Animate(mv, L"Offset.Y", m_menuY, ui::kPanelEnterMs);
        Animate(mv, L"Opacity", 1.0f, ui::kPanelExitMs);
        Animate(m_content, L"Opacity", kMenuDim, ui::kPanelExitMs);
        if (m_notifRoot) Animate(m_notifRoot, L"Opacity", kMenuDim, ui::kPanelExitMs);
    } else {
        Animate(m_content, L"Opacity", 1.0f, ui::kPanelExitMs);
        if (m_notifRoot) Animate(m_notifRoot, L"Opacity", 1.0f, ui::kPanelExitMs);
        auto menu = mv;
        auto batch = m_comp.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
        Animate(mv, L"Opacity", 0.0f, ui::kFastMs);
        batch.End();
        batch.Completed([this, menu](auto&&, auto&&) {
            if (!m_menuOpen) menu.IsVisible(false);
        });
    }
}

void Launcher::Click(Hit const& hit) {
    switch (hit.kind) {
        case Kind::App:
        case Kind::File:
        case Kind::Result: Activate(hit.kind, hit.index); break;
        case Kind::Settings:
            Hide();
            OpenQuietly(L"ms-settings:");
            break;
        case Kind::Power: ToggleMenu(!m_menuOpen); break;
        case Kind::Lock:
            Hide();
            RunPower(3);  // Lock (a dry run under HALO_TEST)
            break;
        case Kind::AllApps: ToggleAllApps(); break;
        case Kind::PowerMode: SetMode(hit.index); break;
        case Kind::MenuItem: {
            int item = hit.index;
            Hide();
            RunPower(item);
            break;
        }
        default: CcClick(hit); break;  // halo2: the right column
    }
}

// Sleep, Restart, Shut down, Lock, Sign out, then "Use Windows taskbar". Under HALO_TEST (or HALO_TEST_POWER_DRYRUN=1) only logs.
void Launcher::RunPower(int item) {
    if (item < 0 || item >= kMenuCount) return;
    if (m_dryRun) {
        Log(std::wstring(L"power: DRY RUN, would run ") + kMenuItems[item]);
        return;
    }
    Log(std::wstring(L"power: ") + kMenuItems[item]);
    constexpr DWORD kReason = SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED;
    switch (item) {
        case 0: SetSuspendState(FALSE, FALSE, FALSE); break;
        case 1:
            EnableShutdownPrivilege();
            ExitWindowsEx(EWX_REBOOT, kReason);
            break;
        case 2:
            EnableShutdownPrivilege();
            ExitWindowsEx(EWX_POWEROFF, kReason);
            break;
        case 3: LockWorkStation(); break;
        case 4: ExitWindowsEx(EWX_LOGOFF, kReason); break;
        case kMenuTaskbar: {  // guardian.exe --off turns Halo off and brings the Windows taskbar back; it sits next to halo.exe
            wchar_t exe[MAX_PATH];
            DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
            if (!n || n >= MAX_PATH) break;
            std::wstring dir(exe, n);
            dir.resize(dir.find_last_of(L'\\') + 1);
            std::wstring guardian = dir + L"guardian.exe";
            SHELLEXECUTEINFOW sei{sizeof(sei)};
            sei.fMask = SEE_MASK_FLAG_NO_UI;  // missing file: fail quietly, no dialog
            sei.lpFile = guardian.c_str();
            sei.lpParameters = L"--off";
            sei.lpDirectory = dir.c_str();
            sei.nShow = SW_HIDE;
            if (!ShellExecuteExW(&sei)) Log(L"guardian --off failed (" + std::to_wstring(GetLastError()) + L")");
            break;
        }
    }
}

void Launcher::TestCommand(WPARAM w) {
    if (!m_open) return;
    if (w == 2) ToggleMenu(!m_menuOpen);
    else if (w >= 3 && w < 3 + kMenuCount) RunPower(static_cast<int>(w) - 3);
    else if (w == 9) {  // a click elsewhere
        Log(L"lost activation");
        Hide();
    } else if (w == 20) {  // a locked session does not run animations; screenshots take the end state instead
        m_hover.StopAnimation(L"Opacity");
        m_hover.Opacity(m_hoverOn ? 1.0f : 0.0f);
        m_hoverShape.StopAnimation(L"Offset");
        m_hoverShape.StopAnimation(L"Size");
        m_hoverShape.Offset({m_plateTo.left, m_plateTo.top});
        m_hoverShape.Size({m_plateTo.right - m_plateTo.left, m_plateTo.bottom - m_plateTo.top});
        if (m_hoverIcon) {
            auto v = m_hoverIcon->v;
            v.StopAnimation(L"Scale");
            v.StopAnimation(L"Offset.Y");
            v.Scale({kHoverScale, kHoverScale, 1.0f});
            v.Offset({v.Offset().x, m_hoverIcon->restY - kHoverLift * m_s, 0});
        }
    } else if (w == 21) {
        ToggleAllApps();
    } else if (m_h2 && w >= 23 && w <= 29) {  // screenshots of the right column's states, without touching the system
        if (w == 23) ToggleDetail(Detail::Output);
        else if (w == 24) {  // pretend: radios, a hotspot, a mic and a speaker exist, some things are on
            m_demo = true;
            m_wifiOn = true;
            m_ssid = L"Home Wi-Fi";
            m_btOn = false;
            m_dnd = true;
            m_hotspotAvail = true;
            m_vol = 0.62f;
            m_bright = 70;
            RedrawRight();
        } else if (w == 25) ScrollHome(200 * m_s);
        else if (w == 26) ScrollRight(160 * m_s);
        else if (w == 27) {
            ScrollHome(-1e6f);
            ScrollRight(-1e6f);
        } else if (w == 28) {  // the Wi-Fi list with made-up networks
            m_demo = true;
            m_wifiOn = true;
            m_detail = Detail::Wifi;
            m_wifiRows.clear();
            for (auto const& n : {std::pair<const wchar_t*, int>{L"Home Wi-Fi", 4}, {L"Neighbour 5G", 3}, {L"Cafe Guest", 2}, {L"Hidden", 1}}) {
                svc::WifiNet net;
                net.ssid = n.first;
                net.bars = n.second;
                net.connected = n.second == 4;
                m_wifiRows.push_back(net);
            }
            LayoutRight();
            RedrawRight();
        } else if (w == 29) {  // hover the control the pointer is on, at once
            if (m_ccHover >= 0 && m_ccHover < static_cast<int>(m_ccBase.size())) {
                m_ccPlate.StopAnimation(L"Opacity");
                m_ccPlate.Opacity(m_ccPlateOn ? 1.0f : 0.0f);
                m_ccPlateShape.StopAnimation(L"Offset");
                m_ccPlateShape.StopAnimation(L"Size");
                auto const& r = m_ccBase[m_ccHover].r;
                m_ccPlateShape.Offset({r.left - m_lw, r.top});
                m_ccPlateShape.Size({r.right - r.left, r.bottom - r.top});
            }
        }
    } else if (w == 22) {
        Scroll(5);
    }
}

LRESULT CALLBACK Launcher::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        static_cast<Launcher*>(cs->lpCreateParams)->m_hwnd = h;
    }
    auto* self = reinterpret_cast<Launcher*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return DefWindowProcW(h, m, w, l);
    // A throw out of a handler (a failed surface, a rejected expression, a lost device) would unwind through user32 and
    // take the whole shell with it (Halo::WndProc has the same guard). Log it; the next event retries.
    try {
        return self->OnMessage(m, w, l);
    } catch (winrt::hresult_error const& e) {
        Log(L"message " + std::to_wstring(m) + L" threw " + std::to_wstring(static_cast<unsigned>(e.code().value)) + L" " + e.message().c_str());
    } catch (std::exception const&) {
        Log(L"message " + std::to_wstring(m) + L" threw std::exception");
    } catch (...) {
        Log(L"message " + std::to_wstring(m) + L" threw");
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT Launcher::OnMessage(UINT m, WPARAM w, LPARAM l) {
    const float mx = static_cast<float>(static_cast<short>(LOWORD(l)));
    const float my = static_cast<float>(static_cast<short>(HIWORD(l)));
    if (m == kMsgRecent) { ApplyRecent(); return 0; }
    if (m == kMsgDeferred) { RunDeferred(); return 0; }
    if (m == kMsgLate) {
        std::unique_ptr<LateRead> r(reinterpret_cast<LateRead*>(l));
        if (m_servicesOn && r) {
            m_hotspotAvail = r->hotspotAvail;
            m_hotspotOn = r->hotspotOn;
            RedrawRight();
        }
        return 0;
    }
    if (m == kMsgOutputs) {  // the cached output list changed (or a switch of default output finished)
        OnServiceMessage(m);
        return 0;
    }
    if (m >= kMsgAudio && m <= kMsgMic) {  // halo2 services
        if (m == kMsgAudio) m_speaker.OnMessage(w);  // follows a default-device change
        if (m == kMsgMic) m_mic.OnMessage(w);
        OnServiceMessage(m);
        return 0;
    }
    switch (m) {
        case WM_ACTIVATE:
            if (LOWORD(w) != WA_INACTIVE) {
                m_activated = true;
            } else if (m_open && m_activated && !EnvIs(L"HALO_TEST_STAY")) {  // a click elsewhere, or another window took the foreground
                Log(L"lost activation");
                Hide();
            }
            break;  // DefWindowProc gives the keyboard focus to the window
        case WM_KEYDOWN:
            if (m_open) OnKey(w);
            return 0;
        case WM_CHAR: {
            if (!m_open || m_menuOpen) return 0;
            const wchar_t c = static_cast<wchar_t>(w);
            if (m_h2 && m_col == 1) {
                if (c < 33 || c == 127) return 0;  // Space, Enter, Tab, Esc act in the right column; they never type
                SetCol(0);                         // a letter goes back to search and starts the query
            }
            if (c == 8) {  // Backspace (a surrogate pair goes as one)
                if (m_query.empty()) return 0;
                if (m_query.size() > 1 && IS_LOW_SURROGATE(m_query.back()) && IS_HIGH_SURROGATE(m_query[m_query.size() - 2]))
                    m_query.pop_back();
                m_query.pop_back();
            } else if (c == 127) {  // Ctrl+Backspace: the last word and the spaces before it
                if (m_query.empty()) return 0;
                while (!m_query.empty() && m_query.back() == L' ') m_query.pop_back();
                while (!m_query.empty() && m_query.back() != L' ') m_query.pop_back();
            } else if (c >= 32 && m_query.size() < kMaxQuery && !(c == L' ' && m_query.empty())) {
                m_query.push_back(c);
            } else {
                return 0;
            }
            QueryChanged();
            return 0;
        }
        case WM_TIMER:
            if (w == kServicesTimer) {
                KillTimer(m_hwnd, kServicesTimer);
                if (m_open && m_h2 && m_notifCol) m_notifCol->SetActive(true);  // reads the notifications and keeps them current while Start is up
                if (m_open && !m_servicesOn && !EnvIs(L"HALO_TEST_NOSERVICES")) StartServices();  // (the preview shows stand-in state, never the machine's)
            }
            else if (w == kRefreshTimer) {
                KillTimer(m_hwnd, kRefreshTimer);
                if (m_open) Refresh();
            }
            else if (w == kSettleTimer) {
                KillTimer(m_hwnd, kSettleTimer);
                if (m_open) SettleMotion();
            }
            else if (w == kWaveOffTimer) {
                KillTimer(m_hwnd, kWaveOffTimer);
                if (!m_waveOn) WaveStop();
            }
            else if (w == kRehoverTimer) {
                KillTimer(m_hwnd, kRehoverTimer);
                if (m_open) RehoverAtCursor();
            }
            else if (w == kOpenTimer) {  // the first frame with the new surfaces has landed: now the spring
                KillTimer(m_hwnd, kOpenTimer);
                if (m_open) {
                    OpenMotion(m_panel, {0, kTop * m_s, 0}, true);
                    if (m_shadowHold) OpenMotion(m_shadowHold, PanelHome(), true);
                    SetTimer(m_hwnd, kSettleTimer, SettleDelayMs(), nullptr);
                }
            }
            else if (w == kCaretTimer) {
                if (m_open && m_caret) {  // the caret blinks from here, not from a forever-animation (that keeps the GPU compositing at 60 fps)
                    m_caretOn = !m_caretOn;
                    m_caret.Opacity(m_caretOn ? 1.0f : 0.0f);
                }
            }
            else if (w == kClockTimer) {
                KillTimer(m_hwnd, kClockTimer);
                if (m_open && m_h2) {
                    RefreshWidgets();
                    ArmClock();
                }
            }
            return 0;
        case WM_MOUSEMOVE:
            if (m_drag) {
                CcDrag(PX(mx));
                return 0;
            }
            static const bool test = EnvIs(L"HALO_TEST");  // posted test moves would be followed by an instant WM_MOUSELEAVE
            if (!m_tracking && !test) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, m_hwnd, 0};
                m_tracking = TrackMouseEvent(&tme);
            }
            if (m_open && m_h2 && m_notifCol && mx - m_shadowM < m_nw) {  // over the notification column
                OnHover(-1, -1);                                              // nothing of the middle or right is hovered
                m_notifCol->Move(mx - m_shadowM, my - kTop * m_s);
                return 0;
            }
            if (m_notifCol) m_notifCol->Leave();
            if (m_open) OnHover(PX(mx), my);
            return 0;
        case kMsgAppsFolder: {
            std::unique_ptr<Found> more(reinterpret_cast<Found*>(l));
            const size_t before = m_index.size();
            if (w == 1) {  // a full rescan (RefreshIndex): replaces the index, so removed apps go too
                m_rescanning = false;
                m_index.clear();
                AddToIndex(*more);
                if (m_index.size() != before)
                    Log(L"app index rescanned: " + std::to_wstring(before) + L" -> " + std::to_wstring(m_index.size()) + L" apps");
            } else {
                AddToIndex(*more);
                Log(L"app index: " + std::to_wstring(m_index.size() - before) + L" more from the Apps folder, " +
                    std::to_wstring(m_index.size()) + L" apps");
            }
            if (m_open && Listing()) {  // what is on screen may have changed
                // New apps slot in among the matches: keep the selected item selected, not the selected row, so
                // an Enter already on its way launches what the user was looking at.
                const std::wstring keep = m_sel < static_cast<int>(m_results.size()) ? m_results[m_sel].path : std::wstring();
                RunSearch();
                if (auto it = std::find_if(m_matches.begin(), m_matches.end(), [&](Result const& r) { return !keep.empty() && r.path == keep; });
                    it != m_matches.end()) {
                    const int k = static_cast<int>(it - m_matches.begin()), rows = ListRows();
                    if (k < m_scroll) m_scroll = k;
                    else if (k >= m_scroll + rows) m_scroll = k - rows + 1;
                    ShowMatches();
                    m_sel = k - m_scroll;
                }
                BuildBody();
            }
            return 0;
        }
        case WM_MOUSEWHEEL:  // three rows a notch; a touchpad's smaller steps add up
            if (m_open && m_h2 && m_notifCol && !m_menuOpen) {  // the notification list scrolls under the pointer
                POINT p{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
                ScreenToClient(m_hwnd, &p);
                if (p.x - m_shadowM < m_nw && m_notifCol->Wheel(static_cast<float>(p.x) - m_shadowM, p.y - kTop * m_s, GET_WHEEL_DELTA_WPARAM(w))) return 0;
            }
            if (m_open && m_h2 && !m_menuOpen && !Listing()) {
                POINT p{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
                ScreenToClient(m_hwnd, &p);
                if (PX(static_cast<float>(p.x)) >= 0 && PX(static_cast<float>(p.x)) < m_lw && p.y - kTop * m_s >= m_bodyTop && p.y - kTop * m_s < m_bodyBottom) {
                    ScrollHome(-GET_WHEEL_DELTA_WPARAM(w) * ui::kS6 * m_s / WHEEL_DELTA);
                    return 0;
                }
            }
            if (m_open && m_h2 && !m_menuOpen && m_detail == Detail::None) {
                POINT p{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
                ScreenToClient(m_hwnd, &p);
                if (PX(static_cast<float>(p.x)) >= m_lw) {
                    ScrollRight(-GET_WHEEL_DELTA_WPARAM(w) * ui::kS6 * m_s / WHEEL_DELTA);
                    return 0;
                }
            }
            if (m_open && m_h2 && !m_menuOpen && m_detail != Detail::None) {  // over the right column the radio list scrolls
                POINT p;
                if (GetCursorPos(&p) && ScreenToClient(m_hwnd, &p) && PX(static_cast<float>(p.x)) >= m_lw) {
                    m_wheel -= GET_WHEEL_DELTA_WPARAM(w) * 3;
                    const int rows = m_wheel / WHEEL_DELTA;
                    m_wheel -= rows * WHEEL_DELTA;
                    if (rows) {
                        m_ccScroll += rows;
                        LayoutRight();
                        RedrawRight();
                    }
                    return 0;
                }
            }
            if (m_open && !m_menuOpen && Listing()) {
                m_wheel -= GET_WHEEL_DELTA_WPARAM(w) * 3;
                const int rows = m_wheel / WHEEL_DELTA;
                m_wheel -= rows * WHEEL_DELTA;
                if (rows) {
                    Scroll(rows);
                    POINT p;  // the list moved under a still pointer: hover what is there now
                    if (GetCursorPos(&p) && ScreenToClient(m_hwnd, &p)) {
                        m_hoverIndex = -2;
                        OnHover(PX(static_cast<float>(p.x)), static_cast<float>(p.y));
                    }
                }
            }
            return 0;
        case WM_MOUSELEAVE:
            m_tracking = false;
            if (m_notifCol) m_notifCol->Leave();
            if (m_open) OnHover(-1, -1);
            return 0;
        case WM_LBUTTONDOWN:
            // The shadow margins (sides, bottom) are outside the panel: treat a click there as a click
            // outside and dismiss, like macOS.
            if (m_open && (mx < m_shadowM || mx >= m_shadowM + PanelW() || my >= kTop * m_s + m_h)) {
                Hide();
                return 0;
            }
            m_pressedNotif = m_open && m_h2 && m_notifCol && mx - m_shadowM < m_nw;
            if (m_pressedNotif) {  // the notification column acts on release; its press answers now
                m_notifCol->Press(mx - m_shadowM, my - kTop * m_s);
                return 0;
            }
            if (m_open) {
                auto const* hit = HitAt(PX(mx), my - kTop * m_s);
                m_pressedIndex = hit ? static_cast<int>(hit->kind) * 1000 + hit->index : -1;
                m_pressedRowKey = hit && hit->kind == Kind::CcRow ? RowKey(hit->index) : std::wstring();
                if (hit && (hit->kind == Kind::CcBright || hit->kind == Kind::CcVol)) {  // a slider takes the pointer until the button comes up
                    m_drag = static_cast<int>(hit->kind);
                    m_pressedIndex = -1;
                    SetCapture(m_hwnd);
                    CcDrag(PX(mx));
                    return 0;
                }
                const bool inCc = m_h2 && hit && !m_ccHits.empty() && hit >= m_ccHits.data() && hit < m_ccHits.data() + m_ccHits.size();
                if (inCc && hit->kind != Kind::CcBright && hit->kind != Kind::CcVol) CcPress(true, hit);  // the pressed look, before anything else happens
                else if (hit && !inCc) PressFeel(hit, true);
                if (auto const* ic = hit ? IconOf(*hit) : nullptr) {  // the press squash
                    auto sq = m_comp.CreateVector3KeyFrameAnimation();
                    sq.InsertKeyFrame(1.0f, {kPressScale, kPressScale, 1.0f}, m_easeOut);
                    sq.Duration(std::chrono::milliseconds(ui::kPressDownMs));
                    ic->v.StartAnimation(L"Scale", sq);
                }
            }
            return 0;
        case WM_LBUTTONUP:
            if (m_drag) {
                const bool bright = m_drag == static_cast<int>(Kind::CcBright);
                m_drag = 0;
                ReleaseCapture();
                if (bright && m_bright >= 0) svc::brightness::Set(m_bright);  // the final value always lands
                m_pressedIndex = -1;
                return 0;
            }
            if (std::exchange(m_pressedNotif, false)) {
                if (m_notifCol) m_notifCol->Unpress();
                if (m_open && m_notifCol && mx - m_shadowM < m_nw) m_notifCol->Click(mx - m_shadowM, my - kTop * m_s);
                return 0;
            }
            if (m_open) {
                PressFeel(nullptr, false);
                if (m_h2) CcPress(false);
                auto const* hit = HitAt(PX(mx), my - kTop * m_s);
                int key = hit ? static_cast<int>(hit->kind) * 1000 + hit->index : -1;
                bool same = hit && key == m_pressedIndex;
                if (hit && hit->kind == Kind::CcRow && m_pressedIndex >= 0 && m_pressedIndex / 1000 == static_cast<int>(Kind::CcRow))
                    same = !m_pressedRowKey.empty() && RowKey(hit->index) == m_pressedRowKey;  // by id: the same device, wherever its row is now
                if (same) {
                    Hit copy = *hit;
                    Click(copy);
                } else {
                    if (m_hoverIcon) SpringIcon(*m_hoverIcon, true);  // a press that went elsewhere: back from the squash
                    if (!hit && m_menuOpen) ToggleMenu(false);        // a click on the panel itself closes the menu
                }
            }
            m_pressedIndex = -1;
            return 0;
    }
    return DefWindowProcW(m_hwnd, m, w, l);
}

// ---- HALO 2.0: the widget strip (left) and the compact control centre (right) -------------------------------------
// Only runs with settings.halo2. The radios, audio, brightness, media and power-mode calls are the Control
// Center's services (services.h); this file adds the layout, the painting and the input for the panel.
// Look: macOS Control Center / Spotlight. Neutral surfaces; an icon sits in a round badge that fills with the accent when
// it is on (the accent shows nowhere else except today in the week); thick neutral sliders with the glyph in the track;
// one hero (the greeting). Hover is a plate that glides on the compositor, so a hover never redraws a surface, and a
// scroll slides a holder, so a scroll never redraws or rebuilds.

namespace {

namespace tk = start::tk;

struct Ink {  // the palette as brushes on one device context
    winrt::com_ptr<ID2D1SolidColorBrush> ink, soft, muted, fill, hover, press, track, line, accent, onAccent, sliderInk, sel;
    explicit Ink(ID2D1DeviceContext* dc) {
        auto const& p = theme::P();
        dc->CreateSolidColorBrush(p.text, ink.put());
        dc->CreateSolidColorBrush(p.textSoft, soft.put());
        dc->CreateSolidColorBrush(theme::Muted(), muted.put());
        dc->CreateSolidColorBrush(p.fill, fill.put());
        dc->CreateSolidColorBrush(p.fillHover, hover.put());
        dc->CreateSolidColorBrush(p.fillPress, press.put());
        dc->CreateSolidColorBrush(p.off, track.put());
        dc->CreateSolidColorBrush(p.line, line.put());
        dc->CreateSolidColorBrush(p.accent, accent.put());
        dc->CreateSolidColorBrush(p.onAccent, onAccent.put());
        dc->CreateSolidColorBrush(theme::SliderOnFill(), sliderInk.put());
        auto c = p.text;
        c.a = 0.18f;
        dc->CreateSolidColorBrush(c, sel.put());
    }
};

D2D1_RECT_F Inflate(D2D1_RECT_F r, float d) { return R(r.left - d, r.top - d, r.right + d, r.bottom + d); }
float Mid(float a, float b) { return (a + b) / 2; }

wchar_t const* ModeLabel(svc::PowerMode m) {
    switch (m) {
        case svc::PowerMode::Efficiency: return L"Best efficiency";
        case svc::PowerMode::Balanced: return L"Balanced";
        case svc::PowerMode::Performance: return L"Best performance";
        case svc::PowerMode::Max: return L"Max performance";
        default: return L"Power mode";
    }
}

// "(2) Some page | Site" -> "Some page | Site": browsers put the unread count in front of the title.
std::wstring CleanTitle(std::wstring t) {
    if (t.size() > 3 && t[0] == L'(') {
        size_t i = 1;
        while (i < t.size() && std::iswdigit(t[i])) i++;
        if (i > 1 && i + 1 < t.size() && t[i] == L')' && t[i + 1] == L' ') t.erase(0, i + 2);
    }
    return t;
}

int ModeIndex(svc::PowerMode m) {
    return m == svc::PowerMode::Efficiency ? 0 : m == svc::PowerMode::Balanced ? 1 : 2;
}

constexpr float kBatteryW = 24, kBatteryH = 12;  // the battery glyph in the control column's footer

// ISO 8601 week number of a date (week 1 holds the year's first Thursday).
int IsoWeek(int y, int m, int d) {
    auto weeksIn = [](int yy) {
        auto p = [](int a) { return (a + a / 4 - a / 100 + a / 400) % 7; };
        return p(yy) == 4 || p(yy - 1) == 3 ? 53 : 52;
    };
    const int wd = start::Weekday(y, m, d), iso = wd == 0 ? 7 : wd;
    const int doy = static_cast<int>(start::DaysFromCivil(y, m, d) - start::DaysFromCivil(y, 1, 1)) + 1;
    int week = (doy - iso + 10) / 7;
    if (week < 1) week = weeksIn(y - 1);
    else if (week > weeksIn(y)) week = 1;
    return week;
}

}  // namespace

float Launcher::PanelX() const {
    const RECT& mon = m_anchor.monitor;
    if (m_h2) return mon.left + (mon.right - mon.left - PanelW()) / 2;
    return static_cast<float>(std::clamp(m_anchor.centerX - static_cast<int>(m_w) / 2, static_cast<int>(mon.left), static_cast<int>(mon.right - m_w)));
}

float Launcher::CornerX() const {
    return m_h2 ? std::clamp((m_anchor.centerX - PanelX()) / std::max(PanelW(), 1.0f), 0.0f, 1.0f) : 0.5f;
}

std::wstring Launcher::AppName(std::wstring const& target) const {
    for (auto const& a : m_index)
        if (!_wcsicmp(a.path.c_str(), target.c_str())) return a.name;
    if (!_wcsicmp(target.c_str(), kSettingsApp)) return L"Settings";
    std::wstring t = target;
    if (t.size() > 4 && !_wcsicmp(t.c_str() + t.size() - 4, L".lnk")) t.resize(t.size() - 4);
    if (auto bang = t.find(L'!'); bang != std::wstring::npos && t.starts_with(L"shell:AppsFolder\\")) {  // a Store app not in the index yet: its package name
        std::wstring pkg = t.substr(16, bang - 16);
        if (auto us = pkg.find(L'_'); us != std::wstring::npos) pkg.resize(us);
        if (auto dot = pkg.find_last_of(L'.'); dot != std::wstring::npos) pkg = pkg.substr(dot + 1);
        std::wstring out;  // WindowsTerminal -> Windows Terminal
        for (size_t i = 0; i < pkg.size(); i++) {
            if (i && std::iswupper(pkg[i]) && !std::iswupper(pkg[i - 1])) out += L' ';
            out += pkg[i];
        }
        if (!out.empty()) return out;
    }
    auto at = t.find_last_of(L"\\!");
    return at == std::wstring::npos ? t : t.substr(at + 1);
}

// ---- Scrolling: a holder slides on the compositor, nothing is redrawn ---------------------------------------------

void Launcher::SetScroll(wuc::ContainerVisual const& hold, float y) {
    if (!hold) return;
    if (settings::Get().reduceMotion) {
        hold.StopAnimation(L"Offset.Y");
        hold.Offset({hold.Offset().x, y, hold.Offset().z});
    } else {
        // Critically damped spring: each frame closes a fixed fraction of what is left (an exponential ease-out), and a
        // new wheel notch only moves the target, so velocity carries over instead of restarting like a keyframe would.
        auto a = m_comp.CreateSpringScalarAnimation();
        a.FinalValue(y);
        a.DampingRatio(1.0f);
        a.Period(std::chrono::milliseconds(std::max(1, static_cast<int>(std::lround(kScrollSpringMs / settings::Speed(settings::Get().start))))));
        hold.StartAnimation(L"Offset.Y", a);
    }
    SetTimer(m_hwnd, kRehoverTimer, 140, nullptr);  // what is under a still pointer once the wheel has paused
}

void Launcher::RehoverAtCursor() {
    POINT p;
    if (!m_open || m_menuOpen || !GetCursorPos(&p) || !ScreenToClient(m_hwnd, &p)) return;
    m_hoverIndex = -2;
    m_ccHover = -2;
    OnHover(PX(static_cast<float>(p.x)), static_cast<float>(p.y));
}

void Launcher::ShiftHits() {
    if (m_hitBase.size() != m_hits.size()) return;
    for (size_t i = 0; i < m_hits.size(); i++) {
        auto& h = m_hits[i];
        auto const& b = m_hitBase[i];
        if (h.kind != Kind::App && h.kind != Kind::File && h.kind != Kind::AllApps) continue;
        const float top = std::max(b.top - m_homeScroll, m_bodyTop), bottom = std::min(b.bottom - m_homeScroll, m_bodyBottom);
        h.r = bottom > top ? R(b.left, top, b.right, bottom) : R(0, 0, 0, 0);
    }
}

void Launcher::ScrollHome(float delta) {
    const float to = std::clamp(m_homeScroll + delta, 0.0f, std::max(0.0f, m_plan.contentBottom * m_s - m_bodyBottom));
    if (to == m_homeScroll) return;
    m_homeScroll = to;
    ShiftHits();
    if (m_hoverIcon) {
        SpringIcon(*m_hoverIcon, false);
        m_hoverIcon = nullptr;
    }
    Highlight(nullptr, 0, false);
    m_hoverIndex = -2;
    SetScroll(m_homeHold, m_bodyTop - to);
}

void Launcher::ScrollRight(float delta) {
    if (m_detail != Detail::None) return;
    const float to = std::clamp(m_rightScroll + delta, 0.0f, std::max(0.0f, m_rplan.bottom * m_s - m_barTop));
    if (to == m_rightScroll) return;
    m_rightScroll = to;
    LayoutRight();
    CcPlate(nullptr, 0);
    m_ccHover = -1;
    SetScroll(m_ccHold, -to);
}

// ---- Left: greeting, quote, battery and power mode, the week ----------------------------------------------------

void Launcher::DrawWidgets(ID2D1DeviceContext* dc, float ox, float dy) {
    const float s = m_s;
    Text txt{m_dwrite};
    Ink c(dc);
    auto at = [&](D2D1_RECT_F r) { return R(r.left + ox, r.top + dy, r.right + ox, r.bottom + dy); };
    SYSTEMTIME st;
    GetLocalTime(&st);
    {   // the greeting (28 semibold) with the date under it (12, muted)
        auto g = at(m_greetRect);
        const wchar_t* hello = st.wHour < 12 ? L"Good morning" : st.wHour < 18 ? L"Good afternoon" : L"Good evening";
        auto fHello = txt.Format(ui::kFontDisplay, tk::kHero * s, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        txt.Draw(dc, std::wstring(hello) + L", " + start::FirstName(m_user, m_user), fHello.get(), R(g.left, g.top, g.right, g.top + start::kGreetH * s),
                 c.ink.get(), DWRITE_TEXT_ALIGNMENT_LEADING, true);
        wchar_t date[64] = {};
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"dddd d MMMM", date, 64, nullptr);
        auto fDate = txt.Format(ui::kFontText, tk::kCap * s, DWRITE_FONT_WEIGHT_NORMAL);
        txt.Draw(dc, date, fDate.get(), R(g.left, g.top + (start::kGreetH + ui::kS1) * s, g.right, g.bottom), c.muted.get(), DWRITE_TEXT_ALIGNMENT_LEADING, true);
    }
    {   // three 72 px cards: battery, power mode (a click steps it), this week
        const float innerW = m_greetRect.right - m_greetRect.left, gap = tk::kPad * s, cw = (innerW - 2 * gap) / 3, wy = m_plan.widgetsY * s;
        auto fCap = txt.Format(ui::kFontText, tk::kCap * s, DWRITE_FONT_WEIGHT_NORMAL);
        auto fVal = txt.Format(ui::kFontText, tk::kTitle * s, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        std::wstring battCap = L"Battery", battVal = m_battery.hasBattery ? std::to_wstring(m_battery.percent) + L"%" : L"AC";
        if (m_battery.hasBattery && m_battery.charging) battCap += L" · charging";
        else if (m_battery.hasBattery && m_battery.remainingSeconds > 0)
            battCap += L" · " + std::to_wstring(m_battery.remainingSeconds / 3600) + L" h " + std::to_wstring(m_battery.remainingSeconds % 3600 / 60) + L" min";
        static const wchar_t* modes[] = {L"Efficiency", L"Balanced", L"Performance"};
        const int mode = m_mode == svc::PowerMode::Unknown ? -1 : ModeIndex(m_mode);
        const int week = IsoWeek(st.wYear, st.wMonth, st.wDay);
        const std::wstring caps[3] = {battCap, L"Power mode", L"This week"};
        const std::wstring vals[3] = {battVal, mode < 0 ? std::wstring(L"Unknown") : std::wstring(modes[mode]), L"Week " + std::to_wstring(week)};
        for (int i = 0; i < 3; i++) {
            auto card = at(R(kPad * s + i * (cw + gap), wy, kPad * s + i * (cw + gap) + cw, wy + m_plan.widgetsH * s));
            dc->FillRoundedRectangle(D2D1::RoundedRect(card, tk::kRCard * s, tk::kRCard * s), c.fill.get());
            const float x0 = card.left + tk::kWidgetPad * s, x1 = card.right - tk::kWidgetPad * s, y0 = card.top + tk::kWidgetPad * s;
            txt.Draw(dc, caps[i], fCap.get(), R(x0, y0, x1, y0 + start::kLabelH * s), c.muted.get(), DWRITE_TEXT_ALIGNMENT_LEADING, true);
            txt.Draw(dc, vals[i], fVal.get(), R(x0, y0 + (start::kLabelH + ui::kS1 / 2) * s, x1, y0 + (start::kLabelH + ui::kS1 / 2 + 24) * s), c.ink.get(),
                     DWRITE_TEXT_ALIGNMENT_LEADING, true);
        }
    }
}

// The control column's footer: a rule, the battery on the left, Lock / Settings / Power (36 px icon buttons) on the right.
void Launcher::DrawFoot(ID2D1DeviceContext* dc, float ox, float oy) {
    const float s = m_s;
    Text txt{m_dwrite};
    Ink c(dc);
    dc->Clear(D2D1::ColorF(0, 0.0f));
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    const float pad = start::kRightPad * s, w = m_w - m_lw, top = m_h - start::kRightFootH * s;
    const float hair = std::max(1.0f, std::round(ui::kHair * s));
    dc->FillRectangle(R(ox + pad, oy, ox + w - pad, oy + hair), c.line.get());
    auto fIcon = txt.Format(ui::kFontIcons, ui::kIcon * s, DWRITE_FONT_WEIGHT_NORMAL);
    auto fCap = txt.Format(ui::kFontText, tk::kCap * s, DWRITE_FONT_WEIGHT_NORMAL);
    auto local = [&](D2D1_RECT_F r) { return R(r.left - m_lw + ox, r.top - top + oy, r.right - m_lw + ox, r.bottom - top + oy); };
    const std::pair<D2D1_RECT_F, const wchar_t*> btns[] = {{m_lockRect, L""}, {m_settingsRect, L""}, {m_powerRect, L""}};
    for (auto const& [rect, glyph] : btns) txt.Draw(dc, glyph, fIcon.get(), local(rect), c.ink.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    // the battery: a glyph (outline, a fill to the charge, the nub) and the percentage
    const auto lr = local(m_lockRect);
    const float cy = Mid(lr.top, lr.bottom), bx = ox + pad, bw = kBatteryW * s, bh = kBatteryH * s, by = cy - bh / 2;
    float tx = bx;
    if (m_battery.hasBattery) {
        const float hl = std::max(1.0f, std::round(ui::kHair * s * 1.5f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(R(bx, by, bx + bw, by + bh), ui::kS1 * s, ui::kS1 * s), c.soft.get(), hl);
        const float inner = hl * 1.5f, fillW = (bw - 2 * inner) * std::clamp(m_battery.percent, 0, 100) / 100.0f;
        if (fillW > 0.5f) dc->FillRoundedRectangle(D2D1::RoundedRect(R(bx + inner, by + inner, bx + inner + fillW, by + bh - inner), ui::kS1 * s / 2, ui::kS1 * s / 2), m_battery.charging ? c.accent.get() : c.ink.get());
        dc->FillRoundedRectangle(D2D1::RoundedRect(R(bx + bw + ui::kHair * s, by + ui::kS1 * s, bx + bw + 3 * ui::kHair * s, by + bh - ui::kS1 * s), ui::kHair * s, ui::kHair * s), c.soft.get());
        tx = bx + bw + ui::kS2 * s + 3 * ui::kHair * s;
    }
    const std::wstring label = m_battery.hasBattery ? std::to_wstring(m_battery.percent) + L"%" : L"AC power";
    txt.Draw(dc, label, fCap.get(), R(tx, lr.top, lr.left - ui::kS2 * s, lr.bottom), c.soft.get(), DWRITE_TEXT_ALIGNMENT_LEADING, true);
}

void Launcher::DrawHeader(ID2D1DeviceContext* dc, float ox, float oy) {
    dc->Clear(D2D1::ColorF(0, 0.0f));
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    DrawWidgets(dc, ox, oy);
}

void Launcher::RedrawBody() {
    if (!m_open || !m_h2 || !m_bodySurface || Listing()) return;
    try { Redraw(m_bodySurface, &Launcher::DrawBody); } catch (...) {}
}

// Time and battery: once a minute while the panel is open (the timer is armed for the next minute boundary).
void Launcher::ArmClock() {
    if (!m_h2) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    SetTimer(m_hwnd, kClockTimer, 60000 - (st.wSecond * 1000 + st.wMilliseconds) + 50, nullptr);
}

void Launcher::RefreshWidgets() {
    m_battery = svc::ReadBattery();
    m_mode = svc::ReadPowerMode();
    if (m_headerSurface) Redraw(m_headerSurface, &Launcher::DrawHeader);
    if (m_footSurface) Redraw(m_footSurface, &Launcher::DrawFoot);
    RedrawBody();
    LayoutRight();  // the power-mode targets exist only while the mode is known
    RedrawRight();
}

// A segment of the power-mode control: Efficiency, Balanced or Performance, through the Windows power overlay API.
void Launcher::SetMode(int index) {
    if (m_mode == svc::PowerMode::Unknown) return;
    const svc::PowerMode next = index <= 0 ? svc::PowerMode::Efficiency : index == 1 ? svc::PowerMode::Balanced : svc::PowerMode::Performance;
    if (m_dryRun || svc::DryRun()) {
        Log(std::wstring(L"power mode: DRY RUN, would set ") + ModeLabel(next));
        m_mode = next;
    } else {
        svc::SetPowerMode(next);
        const svc::PowerMode now = svc::ReadPowerMode();
        m_mode = now == svc::PowerMode::Unknown ? next : now;
        Log(std::wstring(L"power mode: ") + ModeLabel(m_mode));
    }
    RedrawRight();
}

// ---- Right: services -----------------------------------------------------------------------------------------

void Launcher::StartServices() {
    if (!m_h2) return;
    m_servicesOn = true;
    m_dnd = svc::ReadDnd();
    m_night = svc::ReadNightLight();
    m_dark = svc::ReadDarkMode();
    m_speaker.Bind(eRender, m_hwnd, kMsgAudio);
    m_vol = m_speaker.Available() ? m_speaker.Volume() : 0.0f;
    m_muted = m_speaker.Available() && m_speaker.Muted();
    m_mic.Bind(eCapture, m_hwnd, kMsgMic);
    m_micMuted = m_mic.Available() && m_mic.Muted();
    m_radios.Start(m_hwnd, kMsgRadios);  // finds the radios once per process; keeps the cached SSID fresh and posts kMsgRadios
    m_ssid = svc::CachedSsid();
    svc::brightness::Probe(m_hwnd, kMsgBright);
    m_bright = svc::brightness::Supported() ? svc::brightness::Level() : -1;
    m_media.Start(m_hwnd, kMsgMedia, [this] {
        if (m_servicesOn) RedrawRight();
    });
    m_use = {};
    m_use.ram = usage::RamPercent();
    m_sampler.Start(m_hwnd, kMsgUsage);  // ~1 Hz, off the UI thread, and only until StopServices()
    m_hotspotAvail = m_hotspotOn = false;
    std::thread([h = m_hwnd] {  // the hotspot query takes 20+ ms: off the UI thread, like the Control Center's late reads
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        auto* r = new LateRead;
        r->hotspotAvail = svc::HotspotAvailable();
        r->hotspotOn = r->hotspotAvail && svc::ReadHotspot();
        if (!PostMessageW(h, kMsgLate, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

// Nothing runs while Start is closed: no sampler thread, no PDH query, no radio or media subscription.
void Launcher::StopServices() {
    m_servicesOn = false;
    m_sampler.Stop();
    m_media.Stop();
    m_radios.Stop();
    m_speaker.Unbind();
    m_mic.Unbind();
    svc::WifiStop();
    svc::BtStop();
    svc::OutputsStop();  // (the cached lists stay for the next opening)
    if (m_hwnd) {
        KillTimer(m_hwnd, kServicesTimer);
        KillTimer(m_hwnd, kClockTimer);
        KillTimer(m_hwnd, kCaretTimer);
    }
    if (m_drag) {
        ReleaseCapture();
        m_drag = 0;
    }
    m_detail = Detail::None;
}

void Launcher::OnServiceMessage(UINT m) {
    if (!m_servicesOn) return;
    switch (m) {
        case kMsgAudio:
            m_vol = m_speaker.Available() ? m_speaker.Volume() : 0.0f;
            m_muted = m_speaker.Available() && m_speaker.Muted();
            ApplySliders(true);
            RedrawRight();
            break;
        case kMsgMic:
            m_micMuted = m_mic.Available() && m_mic.Muted();
            RedrawRight();
            break;
        case kMsgRadios:
            m_wifiOn = m_radios.WifiOn();
            m_btOn = m_radios.BluetoothOn();
            m_ssid = m_wifiOn ? svc::CachedSsid() : std::wstring();  // (ConnectedSsid blocks: not on the UI thread)
            RedrawRight();
            break;
        case kMsgBright:
            m_bright = svc::brightness::Supported() ? svc::brightness::Level() : -1;
            ApplySliders(true);
            RedrawRight();
            break;
        case kMsgMedia: m_media.Pull(); break;
        // The cached lists update in place: the same rows redraw where they are; only a changed row count re-lays out the hits
        // (a data update never rebuilds the hit list under the pointer, and never resets a press).
        case kMsgWifi: {
            auto rows = svc::WifiNets();
            const bool resized = rows.size() != m_wifiRows.size();
            m_wifiRows = std::move(rows);
            m_ssid = m_wifiOn ? svc::CachedSsid() : std::wstring();
            if (resized && m_detail == Detail::Wifi) LayoutRight();
            RedrawRight();
            break;
        }
        case kMsgBt: {
            auto rows = svc::BtDevices();
            const bool resized = rows.size() != m_btRows.size();
            m_btRows = std::move(rows);
            if (resized && m_detail == Detail::Bt) LayoutRight();
            RedrawRight();
            break;
        }
        case kMsgOutputs: {
            auto outs = svc::OutputDevicesCached();
            const bool resized = outs.size() != m_outs.size();
            m_outs = std::move(outs);
            if (m_detail == Detail::Bt) {  // a Bluetooth row's audio facts come from the output list
                const size_t before = m_btRows.size();
                m_btRows = svc::BtDevices();
                if (before != m_btRows.size()) LayoutRight();
            }
            if (resized && m_detail == Detail::Output) LayoutRight();
            RedrawRight();
            break;
        }
        case kMsgUsage:
            m_use = m_sampler.Latest();
            RedrawUsage();
            break;
    }
}

// ---- Right: layout ---------------------------------------------------------------------------------------------

void Launcher::BuildRight(float rightW) {
    const float s = m_s;
    m_rplan = start::PlanRight(m_h / s, kQuickTiles, true);  // two pills, the small toggles, sliders, now playing, usage; the footer is pinned below
    const float wpx = m_w - m_lw;
    m_rightRect = R(m_lw, 0, m_w, m_h);
    m_barTop = m_h - start::kRightFootH * s;  // the controls scroll above the pinned footer
    const float contentH = std::max(m_barTop, std::ceil(m_rplan.bottom * s));
    auto const& pal = theme::P();

    m_ccClip = m_comp.CreateContainerVisual();  // the viewport: above the footer
    m_ccClip.Size({wpx, m_barTop});
    m_ccClip.Offset({m_lw, 0, 0});
    m_ccClip.Clip(m_comp.CreateInsetClip());
    m_ccHold = m_comp.CreateContainerVisual();  // what scrolls: everything below slides with its Offset.Y
    m_ccHold.Size({wpx, contentH});
    m_ccClip.Children().InsertAtTop(m_ccHold);

    {   // the slider tracks and fills: shapes, so a drag moves a rectangle's width and nothing is redrawn
        auto sv = m_comp.CreateShapeVisual();
        sv.Size({wpx, contentH});
        auto add = [&](wuc::CompositionRoundedRectangleGeometry& g, D2D1_COLOR_F col, bool ring) {
            g = m_comp.CreateRoundedRectangleGeometry();
            auto sp = m_comp.CreateSpriteShape(g);
            sp.FillBrush(m_comp.CreateColorBrush(Col(col)));
            if (ring && !theme::Dark()) {  // a white fill on light glass needs its hairline (the mock's 0 0 0 1px rgba(0,0,0,.08))
                sp.StrokeBrush(m_comp.CreateColorBrush(Col({0, 0, 0, 0.08f})));
                sp.StrokeThickness(std::max(1.0f, std::round(ui::kHair * s)));
            }
            sv.Shapes().Append(sp);
        };
        add(m_brTrack, pal.off, false);  // a neutral track and a white fill, no thumb
        add(m_brFill, theme::SliderFill(), true);
        add(m_vlTrack, pal.off, false);
        add(m_vlFill, theme::SliderFill(), true);
        m_sliderShapes = sv;
        m_ccHold.Children().InsertAtTop(sv);
    }
    m_ccSurface = MakeSurface(wpx, contentH);
    m_ccHold.Children().InsertAtTop(MakeSurfaceVisual(m_ccSurface, wpx, contentH));
    // The usage numbers repaint every second on their own small surface, so a sample does not redraw the controls.
    const float uw = (rightW - 2 * start::kRightPad) * s, uh = m_rplan.usageH * s;
    m_usageRect = R(0, 0, uw, uh);
    m_usageSurface = MakeSurface(uw, uh);
    m_usageVisual = MakeSurfaceVisual(m_usageSurface, uw, uh);
    m_usageVisual.Offset({start::kRightPad * s, m_rplan.usageY * s, 0});
    m_ccHold.Children().InsertAtTop(m_usageVisual);
    {   // the hover plate: one shape that glides between controls
        m_ccPlateShape = m_comp.CreateRoundedRectangleGeometry();
        m_ccPlateSprite = m_comp.CreateSpriteShape(m_ccPlateShape);
        m_ccPlateBrush = m_comp.CreateColorBrush(Col(pal.fillHover));
        m_ccPlateSprite.FillBrush(m_ccPlateBrush);
        m_ccPlate = m_comp.CreateShapeVisual();
        m_ccPlate.Size({wpx, contentH});
        m_ccPlate.Shapes().Append(m_ccPlateSprite);
        m_ccPlate.Opacity(0.0f);
        m_ccPlateOn = false;
        m_ccHold.Children().InsertAtTop(m_ccPlate);
    }
    m_content.Children().InsertAtTop(m_ccClip);
    m_ccHover = -1;
    m_ccFocus = -1;
    LayoutRight();
    RedrawRight();
    RedrawUsage();
}

// The stable id of the list row at `index` on the open page (empty when there is none): a press is keyed by it, so a reorder
// between button-down and button-up cannot click a different device.
std::wstring Launcher::RowKey(int index) const {
    if (index < 0) return {};
    const size_t i = static_cast<size_t>(index);
    if (m_detail == Detail::Wifi) return i < m_wifiRows.size() ? m_wifiRows[i].id : std::wstring();
    if (m_detail == Detail::Bt) return i < m_btRows.size() ? m_btRows[i].id : std::wstring();
    if (m_detail == Detail::Output) return i < m_outs.size() ? m_outs[i].id : std::wstring();
    return {};
}

int Launcher::CcIndexOf(Kind kind, int index) const {
    for (size_t i = 0; i < m_ccHits.size(); i++)
        if (m_ccHits[i].kind == kind && m_ccHits[i].index == index) return static_cast<int>(i);
    return -1;
}

Launcher::Hit const* Launcher::CcHitAt(float x, float y) const {
    if (y < 0 || y >= m_barTop) return nullptr;
    for (size_t i = m_ccHits.size(); i-- > 0;)  // later entries sit on top of earlier ones (a chevron on its tile)
        if (In(m_ccHits[i].r, x, y)) return &m_ccHits[i];
    return nullptr;
}

// The right column's controls, in tab order, for the current state: the home controls, or a list under the first row.
// m_ccBase holds them where they are drawn (content coordinates); m_ccHits where they are now (scrolled).
void Launcher::LayoutRight() {
    const float s = m_s, pad = start::kRightPad * s, x0 = m_lw + pad, x1 = m_w - pad;
    auto const& p = m_rplan;
    m_ccBase.clear();
    m_ccHits.clear();
    const float shift = m_detail == Detail::None ? m_rightScroll : 0.0f;
    auto add = [&](Kind k, int i, D2D1_RECT_F r) {
        m_ccBase.push_back({r, k, i});
        r.top -= shift;
        r.bottom -= shift;
        m_ccHits.push_back({r, k, i});
    };

    const float gap = tk::kInCard * s, tw = (x1 - x0 - gap) / 2, ty0 = p.rowY[0] * s, ty1 = ty0 + p.rowH * s;
    const D2D1_RECT_F wifi = R(x0, ty0, x0 + tw, ty1), bt = R(x1 - tw, ty0, x1, ty1);
    // The round toggle of a pill switches it; anywhere else on the pill opens the list (Wi-Fi, Bluetooth).
    auto zone = [&](D2D1_RECT_F t) { return R(t.left, t.top, t.left + ui::kPillH * s, t.bottom); };
    add(Kind::CcWifi, 0, wifi);
    add(Kind::CcToggle, 0, zone(wifi));
    add(Kind::CcBt, 0, bt);
    add(Kind::CcToggle, 1, zone(bt));

    m_cardRect = m_modeRect = R(0, 0, 0, 0);
    if (m_detail == Detail::None) {
        // The ten quick toggles: 44 px round-ended buttons, kMiniCols across.
        const float mw = (x1 - x0 - (tk::kMiniCols - 1) * gap) / tk::kMiniCols, mh = tk::kMini * s;
        for (int i = 0; i < kQuickTiles; ++i) {
            const float x = x0 + (i % tk::kMiniCols) * (mw + gap), y = p.miniY * s + (i / tk::kMiniCols) * (mh + gap);
            add(Kind::CcQuick, i, R(x, y, x + mw, y + mh));
        }
        // The sliders are 22 px thick; their pointer target is 4 px taller on each side.
        const float bh = p.sliderH * s, grab = ui::kS1 * s, chev = tk::kChevBtn * s;
        add(Kind::CcBright, 0, R(x0, p.brightY * s - grab, x1, p.brightY * s + bh + grab));
        add(Kind::CcVol, 0, R(x0, p.volY * s - grab, x1 - chev - gap, p.volY * s + bh + grab));
        add(Kind::CcMute, 0, R(x0, p.volY * s - grab, x0 + bh, p.volY * s + bh + grab));
        add(Kind::CcOutMore, 0, R(x1 - chev, p.volY * s + bh / 2 - chev / 2, x1, p.volY * s + bh / 2 + chev / 2));
        const float my = p.mediaY * s, mhh = p.mediaH * s, b = tk::kMediaBtn * s;
        const float bx = x1 - tk::kPad * s - 3 * b, by = my + (mhh - b) / 2;
        add(Kind::CcPrev, 0, R(bx, by, bx + b, by + b));
        add(Kind::CcPlay, 0, R(bx + b, by, bx + 2 * b, by + b));
        add(Kind::CcNext, 0, R(bx + 2 * b, by, bx + 3 * b, by + b));
        m_usageVisual.IsVisible(true);
        m_sliderShapes.IsVisible(true);
        return;
    }
    m_cardRect = R(0, 0, 0, 0);
    m_usageVisual.IsVisible(false);
    m_sliderShapes.IsVisible(false);
    const float top = ty1 + tk::kInGroup * s, bottom = m_barTop - pad;
    const float header = 28 * s, rowH = 32 * s;
    const bool radio = m_detail != Detail::Output;
    const float rowsTop = top + header + tk::kInCard / 2 * s + (radio ? rowH : 0);
    add(Kind::CcBack, 0, R(x0, top, x0 + 200 * s, top + header));
    if (radio) {
        add(Kind::CcRefresh, 0, R(x1 - 88 * s, top, x1, top + header));
        add(Kind::CcRadioSettings, 0, R(x0, top + header, x1, top + header + rowH));
    }
    const int total = static_cast<int>(m_detail == Detail::Wifi ? m_wifiRows.size() : m_detail == Detail::Bt ? m_btRows.size() : m_outs.size());
    m_ccRowsFit = std::max(1, static_cast<int>(std::floor((bottom - rowsTop) / rowH)));
    m_ccScroll = std::clamp(m_ccScroll, 0, std::max(0, total - m_ccRowsFit));
    for (int i = 0; i < m_ccRowsFit && m_ccScroll + i < total; i++)
        add(Kind::CcRow, m_ccScroll + i, R(x0, rowsTop + i * rowH, x1, rowsTop + (i + 1) * rowH));
}

void Launcher::RedrawRight() {
    if (!m_h2 || !m_ccSurface) return;  // not gated on m_open: Build() paints before Show() marks the panel open
    try { Redraw(m_ccSurface, &Launcher::DrawRight); } catch (...) {}
    ApplySliders(false);
}

void Launcher::RedrawUsage() {
    if (!m_h2 || !m_usageSurface || m_detail != Detail::None) return;
    try { Redraw(m_usageSurface, &Launcher::DrawUsage); } catch (...) {}
}

// The two sliders' rectangles follow the state (a drag sets them directly, a change from outside glides).
void Launcher::ApplySliders(bool animate) {
    if (!m_brTrack || m_detail != Detail::None) return;
    const bool glide = animate && !settings::Get().reduceMotion;
    auto one = [&](Kind kind, wuc::CompositionRoundedRectangleGeometry const& track, wuc::CompositionRoundedRectangleGeometry const& fill, bool live, float v) {
        const int i = CcIndexOf(kind, 0);
        if (i < 0) return;
        auto const& r = m_ccBase[i].r;
        const float w = r.right - r.left, h = ui::kSliderH * m_s, top = (r.top + r.bottom) / 2 - h / 2, fw = live ? h + (w - h) * std::clamp(v, 0.0f, 1.0f) : 0.0f;
        track.Offset({r.left - m_lw, top});
        track.Size({w, h});
        track.CornerRadius({h / 2, h / 2});
        fill.Offset({r.left - m_lw, top});
        fill.CornerRadius({h / 2, h / 2});
        if (glide) {
            auto a = m_comp.CreateVector2KeyFrameAnimation();
            a.InsertKeyFrame(1.0f, {fw, h}, m_easeOut);
            a.Duration(std::chrono::milliseconds(tk::kFastMs));
            fill.StartAnimation(L"Size", a);
        } else {
            fill.StopAnimation(L"Size");
            fill.Size({fw, h});
        }
    };
    one(Kind::CcBright, m_brTrack, m_brFill, m_bright >= 0, m_bright / 100.0f);
    one(Kind::CcVol, m_vlTrack, m_vlFill, m_speaker.Available() || m_demo, m_vol);
}

// ---- Right: hover plate and press ----------------------------------------------------------------------------

void Launcher::CcPlate(D2D1_RECT_F const* r, float radius) {
    if (!m_ccPlate) return;
    if (!r) {
        if (m_ccPlateOn) Animate(m_ccPlate, L"Opacity", 0.0f, ui::kFastMs);
        m_ccPlateOn = false;
        return;
    }
    const float2 off{r->left - m_lw, r->top}, size{r->right - r->left, r->bottom - r->top};
    m_ccPlateShape.CornerRadius({radius, radius});
    m_ccPlateSprite.CenterPoint({off.x + size.x / 2, off.y + size.y / 2});
    m_ccPlateShape.StopAnimation(L"Offset");  // no glide: see Highlight
    m_ccPlateShape.StopAnimation(L"Size");
    m_ccPlateShape.Offset(off);
    m_ccPlateShape.Size(size);
    m_ccPlateOn = true;
    Animate(m_ccPlate, L"Opacity", 1.0f, ui::kFastMs);
}

// The hover plate for control `cc`, shaped to the control: a round-ended button gets a round-ended plate, a pill or
// the Now Playing buttons the one hover radius, the pill's round toggle a circle around the toggle.
void Launcher::CcPlateAt(int cc) {
    if (cc < 0 || m_ccBase.size() != m_ccHits.size() || cc >= static_cast<int>(m_ccBase.size())) {
        CcPlate(nullptr, 0);
        return;
    }
    auto const& b = m_ccBase[cc];
    const float hh = b.r.bottom - b.r.top;
    switch (b.kind) {
        case Kind::CcBright:
        case Kind::CcVol: CcPlate(nullptr, 0); break;  // a slider has no plate: it answers the drag
        case Kind::CcWifi:
        case Kind::CcBt: {
            D2D1_RECT_F r = b.r;
            CcPlate(&r, ui::kRadiusLarge * m_s);
            break;
        }
        case Kind::CcToggle: {  // a circle around the round toggle
            const float bd = ui::kRound * m_s, cx = b.r.left + ui::kPillH * m_s / 2, cy = (b.r.top + b.r.bottom) / 2, hr = bd / 2 + ui::kS1 * m_s;
            D2D1_RECT_F halo = R(cx - hr, cy - hr, cx + hr, cy + hr);
            CcPlate(&halo, hr);
            break;
        }
        case Kind::CcBack:
        case Kind::CcRow:
        case Kind::CcPrev:
        case Kind::CcPlay:
        case Kind::CcNext: CcPlate(&b.r, ui::kRadiusHover * m_s); break;
        default: CcPlate(&b.r, hh / 2); break;  // round-ended: the small toggles, the output chevron, Refresh
    }
}

// The press: the plate takes the pressed fill and squashes at once (70 ms), and comes back on release (110 ms). Called on
// mouse-down, before anything a click does, so the control answers even when the service behind it is slow.
void Launcher::CcPress(bool down, Hit const* hit) {
    if (!m_ccPlateSprite) return;
    if (down && hit && !m_ccHits.empty() && hit >= m_ccHits.data() && hit < m_ccHits.data() + m_ccHits.size()) {
        const int cc = static_cast<int>(hit - m_ccHits.data());
        if (cc != m_ccHover || !m_ccPlateOn) {  // a press before any hover (a fast click, touch): the plate is on the control now
            m_ccHover = cc;
            CcPlateAt(cc);
        }
    }
    if (down && !m_ccPlateOn) return;  // a control without a plate (a slider)
    if (down) {
        m_ccPlate.StopAnimation(L"Opacity");
        m_ccPlate.Opacity(1.0f);
    }
    m_ccPlateBrush.Color(down ? theme::C(theme::P().press) : Col(theme::P().fillHover));
    auto a = m_comp.CreateVector2KeyFrameAnimation();
    a.InsertKeyFrame(1.0f, down ? float2{ui::kPressScale, ui::kPressScale} : float2{1.0f, 1.0f}, m_easeOut);
    a.Duration(std::chrono::milliseconds(down ? ui::kPressDownMs : ui::kPressUpMs));
    m_ccPlateSprite.StartAnimation(L"Scale", a);
}

// Mouse-down on a control of the middle card, the footer or the Power menu: its plate goes to the pressed fill and squashes, at once.
void Launcher::PressFeel(Hit const* hit, bool down) {
    if (!down) {
        for (auto const& [sprite, brush] : {std::pair{m_hoverSprite, m_hoverBrush}, std::pair{m_menuHoverSprite, m_menuHoverBrush}}) {
            if (!sprite || !brush) continue;
            brush.Color(Col(theme::P().fillHover));
            auto a = m_comp.CreateVector2KeyFrameAnimation();
            a.InsertKeyFrame(1.0f, float2{1.0f, 1.0f}, m_easeOut);
            a.Duration(std::chrono::milliseconds(ui::kPressUpMs));
            sprite.StartAnimation(L"Scale", a);
        }
        return;
    }
    if (!hit) return;
    const bool menu = hit->kind == Kind::MenuItem;
    auto sprite = menu ? m_menuHoverSprite : m_hoverSprite;
    auto brush = menu ? m_menuHoverBrush : m_hoverBrush;
    auto visual = menu ? m_menuHover : m_hover;
    auto shape = menu ? m_menuHoverShape : m_hoverShape;
    if (!sprite || !brush || !visual || !shape) return;
    if (bool& on = menu ? m_menuHoverOn : m_hoverOn; !on) {  // pressed before any hover: the plate goes to the control now
        D2D1_RECT_F r = menu ? m_menuRows[hit->index] : hit->r;
        Highlight(&r, hit->kind == Kind::PowerMode ? ui::kRadiusLarge : ui::kRadiusHover, menu);
    }
    visual.StopAnimation(L"Opacity");
    visual.Opacity(1.0f);
    brush.Color(theme::C(theme::P().press));
    const float2 off = shape.Offset(), size = shape.Size();
    sprite.CenterPoint({off.x + size.x / 2, off.y + size.y / 2});
    auto a = m_comp.CreateVector2KeyFrameAnimation();
    a.InsertKeyFrame(1.0f, float2{ui::kPressScale, ui::kPressScale}, m_easeOut);
    a.Duration(std::chrono::milliseconds(ui::kPressDownMs));
    sprite.StartAnimation(L"Scale", a);
}

// ---- Right: painting ---------------------------------------------------------------------------------------------

void Launcher::DrawRight(ID2D1DeviceContext* dc, float ox, float oy) {
    const float s = m_s;
    Text txt{m_dwrite};
    Ink c(dc);
    dc->Clear(D2D1::ColorF(0, 0.0f));
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    auto at = [&](D2D1_RECT_F r) { return R(r.left - m_lw + ox, r.top + oy, r.right - m_lw + ox, r.bottom + oy); };

    auto fName = txt.Format(ui::kFontText, tk::kText * s, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    auto fBody = txt.Format(ui::kFontText, tk::kText * s, DWRITE_FONT_WEIGHT_NORMAL);
    auto fSub = txt.Format(ui::kFontText, tk::kCap * s, DWRITE_FONT_WEIGHT_NORMAL);
    auto fIcon = txt.Format(ui::kFontIcons, ui::kIcon * s, DWRITE_FONT_WEIGHT_NORMAL);
    auto fIconS = txt.Format(ui::kFontIcons, ui::kIconSmall * s, DWRITE_FONT_WEIGHT_NORMAL);
    auto glyph = [&](wchar_t g, D2D1_RECT_F r, ID2D1Brush* b, IDWriteTextFormat* f) {
        txt.Draw(dc, std::wstring(1, g), f, r, b, DWRITE_TEXT_ALIGNMENT_CENTER);
    };
    auto pill = [&](D2D1_RECT_F r, float rad, ID2D1Brush* b) { dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), b); };
    const bool demo = m_demo;

    // A pill (Wi-Fi, Bluetooth): a 64 px card with a 36 px round toggle (solid accent when on, the `off` fill when not), a
    // semibold name and the state under it. A control that is not there is dimmed.
    auto tile = [&](D2D1_RECT_F r, wchar_t g, bool on, bool avail, std::wstring const& name, std::wstring const& state) {
        pill(r, tk::kRCard * s, c.fill.get());
        const float a = avail ? 1.0f : 0.45f, bd = ui::kRound * s, cx = r.left + ui::kPillH * s / 2, cy = Mid(r.top, r.bottom);
        c.ink->SetOpacity(a);
        c.muted->SetOpacity(a);
        c.accent->SetOpacity(a);
        c.track->SetOpacity(a);
        c.onAccent->SetOpacity(a);
        dc->FillEllipse(D2D1::Ellipse({cx, cy}, bd / 2, bd / 2), on ? c.accent.get() : c.track.get());
        glyph(g, R(cx - bd / 2, cy - bd / 2, cx + bd / 2, cy + bd / 2), on ? c.onAccent.get() : c.ink.get(), fIcon.get());
        const float tx = cx + bd / 2 + ui::kS3 * s, tr = r.right - ui::kS3 * s, line = 20 * s;
        if (state.empty()) {
            txt.Draw(dc, name, fName.get(), R(tx, r.top, tr, r.bottom), c.ink.get(), DWRITE_TEXT_ALIGNMENT_LEADING, true);
        } else {
            txt.Draw(dc, name, fName.get(), R(tx, cy - line, tr, cy), c.ink.get(), DWRITE_TEXT_ALIGNMENT_LEADING, true);
            txt.Draw(dc, state, fSub.get(), R(tx, cy, tr, cy + start::kLabelH * s), c.muted.get(), DWRITE_TEXT_ALIGNMENT_LEADING, true);
        }
        c.ink->SetOpacity(1.0f);
        c.muted->SetOpacity(1.0f);
        c.accent->SetOpacity(1.0f);
        c.track->SetOpacity(1.0f);
        c.onAccent->SetOpacity(1.0f);
    };
    // A small toggle: a 44 px round-ended button, filled with the solid accent when on.
    auto mini = [&](D2D1_RECT_F r, wchar_t g, bool on, bool avail) {
        pill(r, (r.bottom - r.top) / 2, on ? c.accent.get() : c.fill.get());
        auto* ink = on ? c.onAccent.get() : c.ink.get();
        ink->SetOpacity(avail ? 1.0f : 0.45f);
        glyph(g, r, ink, fIcon.get());
        ink->SetOpacity(1.0f);
    };
    const bool hasWifi = demo || m_radios.HasWifi(), hasBt = demo || m_radios.HasBluetooth();

    for (size_t i = 0; i < m_ccBase.size(); i++) {
        auto const& h = m_ccBase[i];
        auto r = at(h.r);
        const float h2 = (r.bottom - r.top) / 2;
        switch (h.kind) {
            case Kind::CcQuick: {
                static const wchar_t icons[] = {0xE709, 0xE704, 0xE708, 0xF08C, 0xE790, 0xE720, 0xE83F, 0xF7ED, 0xEC15, 0xE7F4};  // Airplane, Hotspot, Focus, Night light, Dark mode, Microphone, Battery saver, Screenshot, Cast, Project
                SYSTEM_POWER_STATUS sps{};
                GetSystemPowerStatus(&sps);
                const bool airAvail = hasWifi || hasBt;
                const bool avail = h.index == 0 ? airAvail : h.index == 1 ? m_hotspotAvail : h.index == 5 ? (m_mic.Available() || demo) : true;
                const bool on = h.index == 0 ? (airAvail && !m_wifiOn && !m_btOn) : h.index == 1 ? m_hotspotOn : h.index == 2 ? m_dnd
                                : h.index == 3 ? m_night : h.index == 4 ? m_dark : h.index == 5 ? m_micMuted : h.index == 6 ? (sps.SystemStatusFlag & 1) != 0 : false;
                mini(r, h.index == 5 && m_micMuted ? 0xEC54 : icons[h.index], on, avail);
                break;
            }
            case Kind::CcWifi:
            case Kind::CcBt: {
                const bool wifi = h.kind == Kind::CcWifi;
                const bool has = wifi ? hasWifi : hasBt, on = has && (wifi ? m_wifiOn : m_btOn);
                std::wstring sub = !has ? (!m_servicesOn || m_radios.Loading() ? L"Checking…" : m_radios.Failed() ? L"Check Settings" : L"Unavailable") : on ? (wifi && !m_ssid.empty() ? m_ssid : L"On") : L"Off";
                if (has && !demo && (wifi ? m_radios.WifiPending() : m_radios.BluetoothPending())) sub = on ? L"Turning on…" : L"Turning off…";
                tile(r, wifi ? 0xE701 : 0xE702, on, has, wifi ? L"Wi-Fi" : L"Bluetooth", sub);
                break;
            }
            case Kind::CcWifiMore:
            case Kind::CcBtMore: {
                const bool wifi = h.kind == Kind::CcWifiMore, open = m_detail == (wifi ? Detail::Wifi : Detail::Bt);
                c.muted->SetOpacity((wifi ? hasWifi : hasBt) ? 1.0f : 0.45f);
                glyph(open ? 0xE70D : 0xE76C, r, c.muted.get(), fIconS.get());
                c.muted->SetOpacity(1.0f);
                break;
            }
            case Kind::CcBright:
            case Kind::CcVol: {
                const bool vol = h.kind == Kind::CcVol;
                const bool live = vol ? (m_speaker.Available() || demo) : m_bright >= 0;
                const float v = !live ? 0.0f : vol ? m_vol : m_bright / 100.0f;
                const wchar_t g = vol ? (m_muted || v <= 0.0f ? 0xE74F : v < 0.34f ? 0xE993 : v < 0.67f ? 0xE994 : 0xE995) : 0xE706;
                const float th = ui::kSliderH * s, cyc = Mid(r.top, r.bottom);  // the glyph sits in the track's left end, on the white fill
                glyph(g, R(r.left, cyc - th / 2, r.left + th, cyc + th / 2), live ? c.sliderInk.get() : c.muted.get(), fIconS.get());
                break;
            }
            case Kind::CcOutMore: {
                dc->FillEllipse(D2D1::Ellipse({Mid(r.left, r.right), Mid(r.top, r.bottom)}, h2, h2), c.fill.get());
                glyph(m_detail == Detail::Output ? 0xE70D : 0xE76C, r, c.ink.get(), fIconS.get());
                break;
            }
            case Kind::CcPrev:
            case Kind::CcPlay:
            case Kind::CcNext: break;  // drawn with the card below
            case Kind::CcBack: {
                glyph(0xE72B, R(r.left, r.top, r.left + 24 * s, r.bottom), c.soft.get(), fIconS.get());
                txt.Draw(dc, m_detail == Detail::Wifi ? L"Wi-Fi networks" : m_detail == Detail::Bt ? L"Bluetooth devices" : L"Sound output", fName.get(),
                         R(r.left + 28 * s, r.top, r.right, r.bottom), c.ink.get(), DWRITE_TEXT_ALIGNMENT_LEADING, true);
                break;
            }
            case Kind::CcRefresh: {
                const bool wifi = m_detail == Detail::Wifi;
                const auto status = wifi ? svc::WifiStatus() : svc::BtStatus();
                using P = svc::DiscoveryPhase;
                const bool loading = status.phase == P::Loading || status.phase == P::Connecting;
                const size_t rowCount = wifi ? m_wifiRows.size() : m_btRows.size();
                const wchar_t* label = status.phase == P::Connecting ? L"Connecting…" : loading ? (rowCount ? L"Updating…" : L"Scanning…")
                                       : status.phase == P::AccessDenied || status.phase == P::Unavailable ? L"Settings"
                                       : status.phase == P::Failed ? L"Retry" : L"Refresh";
                pill(r, h2, c.hover.get());
                c.ink->SetOpacity(loading ? 0.5f : 1.0f);
                txt.Draw(dc, label, fSub.get(), r, c.ink.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
                c.ink->SetOpacity(1.0f);
                break;
            }
            case Kind::CcRadioSettings:
                txt.Draw(dc, m_detail == Detail::Wifi ? L"Open Wi-Fi settings" : L"Open Bluetooth settings", fSub.get(),
                         r, c.soft.get(), DWRITE_TEXT_ALIGNMENT_LEADING, true);
                break;
            case Kind::CcRow: {
                if (m_detail == Detail::Wifi) {
                    auto const& n = m_wifiRows[h.index];
                    for (int b = 0; b < 4; b++) {  // signal bars
                        const float bw = 3 * s, bh = (4 + 3 * b) * s, bx = r.left + 12 * s + b * 5 * s;
                        auto* br = b < n.bars ? c.ink.get() : c.track.get();
                        pill(R(bx, Mid(r.top, r.bottom) + 7 * s - bh, bx + bw, Mid(r.top, r.bottom) + 7 * s), 1 * s, br);
                    }
                    txt.Draw(dc, n.ssid.empty() ? L"Hidden network" : n.ssid, n.connected ? fName.get() : fBody.get(),
                             R(r.left + 40 * s, r.top, r.right - (n.pending ? 104 : 48) * s, r.bottom), c.ink.get(), DWRITE_TEXT_ALIGNMENT_LEADING, true);
                    if (n.pending) txt.Draw(dc, L"Connecting…", fSub.get(), R(r.right - 96 * s, r.top, r.right - 8 * s, r.bottom), c.muted.get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
                    else if (n.connected) glyph(0xE73E, R(r.right - 32 * s, r.top, r.right - 8 * s, r.bottom), c.ink.get(), fIconS.get());
                    else if (n.secure) glyph(0xE72E, R(r.right - 32 * s, r.top, r.right - 8 * s, r.bottom), c.muted.get(), fIconS.get());
                } else if (m_detail == Detail::Bt) {
                    auto const& d = m_btRows[h.index];
                    glyph(0xE702, R(r.left + 8 * s, r.top, r.left + 32 * s, r.bottom), c.soft.get(), fIconS.get());
                    txt.Draw(dc, d.name, d.connected ? fName.get() : fBody.get(), R(r.left + 40 * s, r.top, r.right - 104 * s, r.bottom),
                             c.ink.get(), DWRITE_TEXT_ALIGNMENT_LEADING, true);
                    const wchar_t* state = d.pending ? L"Connecting…" : d.connected ? (d.audio && !d.audioDefault ? L"Switch to" : L"Connected") : d.audio ? L"Connect" : L"Paired";
                    txt.Draw(dc, state, fSub.get(), R(r.right - 96 * s, r.top, r.right - 8 * s, r.bottom), d.pending || d.connected ? c.ink.get() : c.muted.get(), DWRITE_TEXT_ALIGNMENT_TRAILING);
                } else {
                    auto const& d = m_outs[h.index];
                    glyph(0xE767, R(r.left + 8 * s, r.top, r.left + 32 * s, r.bottom), c.soft.get(), fIconS.get());
                    txt.Draw(dc, d.name.empty() ? d.id : d.name, d.isDefault ? fName.get() : fBody.get(), R(r.left + 40 * s, r.top, r.right - 48 * s, r.bottom),
                             c.ink.get(), DWRITE_TEXT_ALIGNMENT_LEADING, true);
                    if (d.isDefault) glyph(0xE73E, R(r.right - 32 * s, r.top, r.right - 8 * s, r.bottom), c.ink.get(), fIconS.get());
                }
                break;
            }
            default: break;
        }
    }

    if (m_detail == Detail::None) {  // Now Playing under the sliders
        const float x0 = m_lw + start::kRightPad * s, x1 = m_w - start::kRightPad * s;
        auto r = at(R(x0, m_rplan.mediaY * s, x1, (m_rplan.mediaY + m_rplan.mediaH) * s));
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, tk::kRCard * s, tk::kRCard * s), c.fill.get());
        auto const& ms = m_media.State();
        const float artSz = tk::kMediaArt * s;
        D2D1_RECT_F ar = R(r.left + ui::kS3 * s, Mid(r.top, r.bottom) - artSz / 2, r.left + ui::kS3 * s + artSz, Mid(r.top, r.bottom) + artSz / 2);
        bool drew = false;
        if (ms.artSize > 0 && ms.art.size() == static_cast<size_t>(ms.artSize) * ms.artSize) {  // the real thumbnail
            winrt::com_ptr<ID2D1Bitmap> bmp;
            auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            winrt::com_ptr<ID2D1BitmapBrush> bb;
            if (SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(ms.artSize, ms.artSize), ms.art.data(), ms.artSize * 4, &props, bmp.put())) &&
                SUCCEEDED(dc->CreateBitmapBrush(bmp.get(), bb.put()))) {
                const float k = artSz / ms.artSize;
                bb->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(ar.left, ar.top));
                bb->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                dc->FillRoundedRectangle(D2D1::RoundedRect(ar, tk::kRRow * s, tk::kRRow * s), bb.get());
                drew = true;
            }
        }
        if (!drew) {  // otherwise a neutral tile with a music note
            pill(ar, tk::kRRow * s, c.track.get());
            glyph(0xE8D6, ar, c.muted.get(), fIcon.get());
        }
        const int iPrev = CcIndexOf(Kind::CcPrev, 0);
        const float tl = ar.right + ui::kS3 * s, tr = (iPrev >= 0 ? at(m_ccBase[iPrev].r).left : r.right - tk::kPad * s) - tk::kInCard * s;
        const std::wstring title = ms.has ? (CleanTitle(ms.title).empty() ? L"Now Playing" : CleanTitle(ms.title)) : L"Not playing";
        const std::wstring artist = ms.has ? (ms.artist.empty() ? L"Media" : ms.artist) : L"Media you play shows up here";
        const float mid = Mid(r.top, r.bottom);
        txt.Draw(dc, title, fName.get(), R(tl, mid - 20 * s, tr, mid), ms.has ? c.ink.get() : c.soft.get(), DWRITE_TEXT_ALIGNMENT_LEADING, true);
        txt.Draw(dc, artist, fSub.get(), R(tl, mid, tr, mid + start::kLabelH * s), c.muted.get(), DWRITE_TEXT_ALIGNMENT_LEADING, true);
        for (auto const& hit : m_ccBase) {  // transport controls: 36 px buttons, dimmed when nothing plays
            if (hit.kind != Kind::CcPrev && hit.kind != Kind::CcPlay && hit.kind != Kind::CcNext) continue;
            const wchar_t icon = hit.kind == Kind::CcPrev ? 0xE892 : hit.kind == Kind::CcNext ? 0xE893 : ms.playing ? 0xE769 : 0xE768;
            c.ink->SetOpacity(ms.has ? 1.0f : 0.45f);
            glyph(icon, at(hit.r), c.ink.get(), fIcon.get());
            c.ink->SetOpacity(1.0f);
        }
    } else {  // a list's message when it has no rows
        const bool wifi = m_detail == Detail::Wifi, out = m_detail == Detail::Output;
        const auto status = wifi ? svc::WifiStatus() : svc::BtStatus();
        const size_t total = wifi ? m_wifiRows.size() : out ? m_outs.size() : m_btRows.size();
        const wchar_t* message = nullptr;
        if (out) {
            if (!total) message = L"No sound output found";
        } else {
            switch (status.phase) {
                case svc::DiscoveryPhase::Idle: message = L"Refresh to find devices"; break;
                case svc::DiscoveryPhase::Loading: if (!total) message = wifi ? L"Looking for nearby networks…" : L"Looking for paired devices…"; break;
                case svc::DiscoveryPhase::Connecting: message = L"Connecting to network…"; break;
                case svc::DiscoveryPhase::Ready: if (!total) message = wifi ? L"No networks found" : L"No paired devices"; break;
                case svc::DiscoveryPhase::AccessDenied: message = wifi ? L"Allow location access to scan for Wi-Fi" : L"Bluetooth access is blocked"; break;
                case svc::DiscoveryPhase::Unavailable: message = wifi ? L"Wi-Fi is unavailable. Check the radio in Settings." : L"Bluetooth is unavailable. Check Settings."; break;
                case svc::DiscoveryPhase::Failed: message = L"Couldn't refresh. Try again."; break;
            }
        }
        if (message) {
            const int back = CcIndexOf(Kind::CcBack, 0);
            if (back >= 0) {
                auto b = at(m_ccBase[back].r);
                const float messageTop = b.bottom + (out ? 12 : 44) * s;
                txt.Draw(dc, message, fSub.get(), R(b.left, messageTop, at(m_rightRect).right - start::kRightPad * s, messageTop + 44 * s), c.muted.get(),
                         DWRITE_TEXT_ALIGNMENT_LEADING);
            }
        }
        const int total2 = static_cast<int>(total);
        if (total2 > m_ccRowsFit) {  // a thin thumb in the right margin
            const int first = CcIndexOf(Kind::CcRow, m_ccScroll);
            if (first >= 0) {
                auto fr = at(m_ccBase[first].r);
                const float rowH = fr.bottom - fr.top, track = m_ccRowsFit * rowH, th = std::max(ui::kThumbMin * s, track * m_ccRowsFit / total2);
                const float y = fr.top + (track - th) * m_ccScroll / (total2 - m_ccRowsFit), x = fr.right + ui::kS1 * s;
                c.muted->SetOpacity(0.5f);
                pill(R(x, y, x + ui::kThumbW * s, y + th), ui::kThumbW * s / 2, c.muted.get());
                c.muted->SetOpacity(1.0f);
            }
        }
    }

    // The keyboard focus ring (only while the right column has the keyboard).
    if (m_col == 1 && m_ccFocus >= 0 && m_ccFocus < static_cast<int>(m_ccBase.size())) {
        auto const& h = m_ccBase[m_ccFocus];
        auto r = at(h.r);
        float rad = (r.bottom - r.top) / 2;
        if (h.kind == Kind::CcWifi || h.kind == Kind::CcBt) rad = tk::kRCard * s;
        else if (h.kind == Kind::CcBack || h.kind == Kind::CcRow || h.kind == Kind::CcPrev || h.kind == Kind::CcPlay || h.kind == Kind::CcNext) rad = tk::kRRow * s;
        if (h.kind == Kind::CcBright || h.kind == Kind::CcVol || h.kind == Kind::CcMute) {  // a slider's ring hugs the 22 px track, not its taller target
            const float cyc = Mid(r.top, r.bottom), th = ui::kSliderH * s;
            r = R(r.left, cyc - th / 2, r.right, cyc + th / 2);
            rad = th / 2;
        }
        const float gap = ui::kFocusGap * s;
        dc->DrawRoundedRectangle(D2D1::RoundedRect(Inflate(r, gap), rad + gap, rad + gap), c.accent.get(), ui::kFocusRing * s);
    }
}

// Concise RAM, CPU, GPU and disk percentages in a fixed 2 x 2 grid: tabular numerals, right-aligned.
void Launcher::DrawUsage(ID2D1DeviceContext* dc, float ox, float oy) {
    const float s = m_s;
    Text txt{m_dwrite};
    Ink c(dc);
    dc->Clear(D2D1::ColorF(0, 0.0f));
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    auto fLabel = txt.Format(ui::kFontText, tk::kCap * s, DWRITE_FONT_WEIGHT_NORMAL);  // the section label, as on the middle card
    auto fVal = txt.Format(ui::kFontText, tk::kCap * s, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    auto fRow = txt.Format(ui::kFontText, tk::kCap * s, DWRITE_FONT_WEIGHT_NORMAL);
    const float w = m_usageRect.right - m_usageRect.left;
    txt.Draw(dc, L"Usage", fLabel.get(), R(ox, oy, ox + w, oy + start::kLabelH * s), c.muted.get());

    struct Row {
        const wchar_t* name;
        float value;
        bool has;
    } rows[] = {{L"CPU", m_use.cpu, m_use.hasCpu}, {L"RAM", m_use.ram, true}, {L"GPU", m_use.gpu, m_use.hasGpu}, {L"Disk", m_use.disk, m_use.hasDisk}};
    const float top = oy + (start::kLabelH + tk::kInCard) * s;
    auto one = [&](Row const& row, float x, float y, float cw) {
        const D2D1_RECT_F r = R(x, y, x + cw, y + tk::kUsageRow * s);
        txt.Draw(dc, row.name, fRow.get(), r, c.muted.get());
        txt.Draw(dc, row.has ? std::to_wstring(static_cast<int>(std::lround(row.value))) + L"%" : std::wstring(L"—"), fVal.get(), r, c.ink.get(),
                 DWRITE_TEXT_ALIGNMENT_TRAILING, false, true);
    };
    const float gap = tk::kInGroup * s, cw = (w - gap) / 2;
    for (int i = 0; i < 4; i++) one(rows[i], ox + (i % 2) * (cw + gap), top + (i / 2) * tk::kUsageRow * s, cw);
}

// ---- Right: input ---------------------------------------------------------------------------------------------------

void Launcher::ToggleDetail(Detail d) {
    const Detail before = m_detail;
    if (d == before) d = Detail::None;
    m_detail = d;
    m_ccScroll = 0;
    m_rightScroll = 0;
    m_ccHold.StopAnimation(L"Offset.Y");
    m_ccHold.Offset({0, 0, 0});
    // A page opens at once with the services' last rows and updates in place when they post; nothing is cleared.
    if (d == Detail::Wifi) {
        svc::BtStop();
        svc::OutputsStop();
        m_wifiRows = svc::WifiNets();
        svc::WifiRefresh(m_hwnd, kMsgWifi);
    } else if (d == Detail::Bt) {
        svc::WifiStop();
        m_btRows = svc::BtDevices();
        svc::BtRefresh(m_hwnd, kMsgBt);
        svc::OutputsRefresh(m_hwnd, kMsgOutputs);  // the rows' audio state (connected, default) follows the output list
    } else if (d == Detail::Output) {
        svc::WifiStop();
        svc::BtStop();
        m_outs = svc::OutputDevicesCached();
        svc::OutputsRefresh(m_hwnd, kMsgOutputs);
    } else {
        svc::WifiStop();
        svc::BtStop();
        svc::OutputsStop();
    }
    LayoutRight();
    if (m_col == 1) {
        const Kind from = before == Detail::Wifi ? Kind::CcWifiMore : before == Detail::Bt ? Kind::CcBtMore : Kind::CcOutMore;
        m_ccFocus = d == Detail::None ? CcIndexOf(from, 0) : CcIndexOf(Kind::CcBack, 0);
    }
    m_ccHover = -1;
    CcPlate(nullptr, 0);
    RedrawRight();
    if (d == Detail::None) RedrawUsage();
    if (!settings::Get().reduceMotion) {  // the new content fades in rather than snapping
        m_ccHold.Opacity(0.3f);
        Animate(m_ccHold, L"Opacity", 1.0f, tk::kFastMs + 50);
    }
}

// A click in the control column. A toggle shows its new state at once (the state is flipped and repainted first) and the
// slow part -- the radio, the Windows setting, a read-back -- runs afterwards from a posted message (RunDeferred), which
// repaints again with what the system reports. A service that is slow or refuses just reconciles the toggle back.
void Launcher::CcClick(Hit const& hit) {
    const bool dry = m_dryRun || svc::DryRun();
    auto note = [&](wchar_t const* what) { if (dry) Log(std::wstring(L"control: DRY RUN ") + what); };
    auto later = [&](std::function<void()> f) { if (!dry) m_deferred.push_back(std::move(f)); };
    const bool hasWifi = m_demo || m_radios.HasWifi(), hasBt = m_demo || m_radios.HasBluetooth();
    // A pill has two targets. Its round toggle (CcToggle) switches it; the rest of it (the pill's own hit) opens the
    // Wi-Fi / Bluetooth list. The small toggles 0..5 switch a setting; 6..9 are actions: Battery saver opens its
    // Settings page, Screenshot starts the snip, Cast and Project press Win+K / Win+P for the shell's own flyouts.
    auto runExtra = [&](int i) {
        if (dry) { note(L"extra tile"); return; }
        Hide();
        auto combo = [](WORD vk) {
            INPUT in[4] = {};
            in[0].type = in[1].type = in[2].type = in[3].type = INPUT_KEYBOARD;
            in[0].ki.wVk = VK_LWIN;
            in[1].ki.wVk = vk;
            in[2].ki.wVk = vk;
            in[2].ki.dwFlags = KEYEVENTF_KEYUP;
            in[3].ki.wVk = VK_LWIN;
            in[3].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(4, in, sizeof(INPUT));
        };
        switch (i) {
            case 6: OpenQuietly(L"ms-settings:batterysaver"); break;
            case 7: OpenQuietly(L"ms-screenclip:"); break;
            case 8: combo('K'); break;
            case 9: combo('P'); break;
        }
    };
    Hit h = hit;
    if (h.kind == Kind::CcToggle) {
        h = {h.r, h.index == 0 ? Kind::CcWifi : Kind::CcBt, 0};
    } else if (h.kind == Kind::CcWifi) {
        if (hasWifi) ToggleDetail(Detail::Wifi);
        return;
    } else if (h.kind == Kind::CcBt) {
        if (hasBt) ToggleDetail(Detail::Bt);
        return;
    } else if (h.kind == Kind::CcQuick && h.index >= 6) {
        runExtra(h.index);
        return;
    }
    switch (h.kind) {
        case Kind::CcQuick:
            switch (h.index) {
                case 0:  // Airplane mode: both radios off (remembering what was on), or back to what was on
                    if (!hasWifi && !hasBt) return;
                    if (m_wifiOn || m_btOn) {
                        m_savedWifi = m_wifiOn;
                        m_savedBt = m_btOn;
                        m_wifiOn = m_btOn = false;
                        m_ssid.clear();
                    } else {
                        m_wifiOn = m_savedWifi || !hasBt;
                        m_btOn = m_savedBt;
                    }
                    if (!dry) {  // non-blocking; Radios::WifiOn()/BluetoothOn() hold the requested state until it lands
                        if (m_radios.HasWifi()) m_radios.SetWifi(m_wifiOn);
                        if (m_radios.HasBluetooth()) m_radios.SetBluetooth(m_btOn);
                    }
                    note(L"airplane mode");
                    break;
                case 1:
                    if (!m_hotspotAvail) return;
                    m_hotspotOn = !m_hotspotOn;
                    later([on = m_hotspotOn] { svc::SetHotspot(on); });
                    note(L"hotspot");
                    break;
                case 2:
                    m_dnd = !m_dnd;
                    later([this, on = m_dnd] { svc::SetDnd(on); m_dnd = svc::ReadDnd(); });
                    note(L"do not disturb");
                    break;
                case 3:
                    m_night = !m_night;
                    later([this, on = m_night] { svc::SetNightLight(on); m_night = svc::ReadNightLight(); });
                    note(L"night light");
                    break;
                case 4:
                    m_dark = !m_dark;
                    later([this, on = m_dark] { svc::SetDarkMode(on); m_dark = svc::ReadDarkMode(); });
                    note(L"dark mode");
                    break;
                case 5:  // Microphone: mute / unmute the default capture device
                    if (!m_mic.Available() && !m_demo) return;
                    m_micMuted = !m_micMuted;
                    later([this, on = m_micMuted] { m_mic.SetMute(on); });
                    note(L"microphone");
                    break;
            }
            break;
        case Kind::CcWifi:
            if (!hasWifi) return;
            m_wifiOn = !m_wifiOn;
            if (!dry) m_radios.SetWifi(m_wifiOn);
            if (!m_wifiOn) m_ssid.clear();
            note(L"wifi");
            break;
        case Kind::CcBt:
            if (!hasBt) return;
            m_btOn = !m_btOn;
            if (!dry) m_radios.SetBluetooth(m_btOn);
            note(L"bluetooth");
            break;
        case Kind::CcWifiMore:
            if (!hasWifi) return;  // no adapter (an Ethernet-only desktop): nothing to list
            ToggleDetail(Detail::Wifi);
            return;
        case Kind::CcBtMore:
            if (!hasBt) return;
            ToggleDetail(Detail::Bt);
            return;
        case Kind::CcOutMore: ToggleDetail(Detail::Output); return;
        case Kind::CcMute:
            if (!m_speaker.Available() && !m_demo) return;
            m_muted = !m_muted;
            later([this, on = m_muted] { m_speaker.SetMute(on); });
            note(L"mute");
            break;
        case Kind::CcPrev: m_media.Prev(); return;
        case Kind::CcPlay: m_media.PlayPause(); return;
        case Kind::CcNext: m_media.Next(); return;
        case Kind::CcBack: ToggleDetail(Detail::None); return;
        case Kind::CcRadioSettings: {
            const bool wifi = m_detail == Detail::Wifi;
            Hide();
            OpenQuietly(wifi ? L"ms-settings:network-wifi" : L"ms-settings:bluetooth");
            return;
        }
        case Kind::CcRefresh: {
            const bool wifi = m_detail == Detail::Wifi;
            const auto status = wifi ? svc::WifiStatus() : svc::BtStatus();
            using P = svc::DiscoveryPhase;
            if (status.phase == P::Loading || status.phase == P::Connecting) return;
            if (status.phase == P::AccessDenied || status.phase == P::Unavailable) {
                Hide();
                OpenQuietly(wifi ? (status.phase == P::AccessDenied ? L"ms-settings:privacy-location" : L"ms-settings:network-wifi")
                                 : L"ms-settings:bluetooth");
            } else if (wifi) {
                svc::WifiRefresh(m_hwnd, kMsgWifi);  // the rows stay and update in place
            } else {
                svc::BtRefresh(m_hwnd, kMsgBt);
            }
            break;
        }
        case Kind::CcRow: {
            // Rows are found by id (RowKey), copied, and acted on; every service call below returns at once.
            const std::wstring key = RowKey(hit.index);
            if (key.empty()) return;
            if (m_detail == Detail::Wifi) {
                auto it = std::find_if(m_wifiRows.begin(), m_wifiRows.end(), [&](svc::WifiNet const& n) { return n.id == key; });
                if (it == m_wifiRows.end() || it->pending) return;
                const svc::WifiNet row = *it;
                if (!dry) svc::WifiConnect(row);
                note(L"wifi connect");
                if (!dry) m_wifiRows = svc::WifiNets();  // the row now shows its pending state
            } else if (m_detail == Detail::Output) {
                const std::wstring id = key;
                if (!dry) svc::SetDefaultAudioOutput(id, m_hwnd, kMsgOutputs);  // off the UI thread; kMsgAudio rebinds the sliders when Windows has switched
                note(L"default output");
                for (auto& d : m_outs) d.isDefault = d.id == id;
                ToggleDetail(Detail::None);
                return;
            } else {
                auto it = std::find_if(m_btRows.begin(), m_btRows.end(), [&](svc::BtDevice const& d) { return d.id == key; });
                if (it == m_btRows.end() || it->pending) return;
                const svc::BtDevice d = *it;
                if (d.audio && !d.audioActive) {  // a paired audio device that is not connected: connect it and make it the output
                    if (!dry) {
                        svc::ConnectBluetoothAudio(d.id, m_hwnd, kMsgBt);
                        m_btRows = svc::BtDevices();  // the row shows "Connecting…"
                    }
                    note(L"bluetooth audio connect");
                } else if (d.audio && d.audioActive && !d.audioDefault && !d.endpointId.empty()) {  // connected: switch the output to it
                    if (!dry) svc::SetDefaultAudioOutput(d.endpointId, m_hwnd, kMsgOutputs);
                    for (auto& o : m_outs) o.isDefault = o.id == d.endpointId;
                    note(L"bluetooth audio switch");
                } else if (d.audio) {
                    return;  // already the output
                } else {
                    Hide();
                    OpenQuietly(L"ms-settings:bluetooth");  // no audio endpoint (a mouse, a keyboard): pairing and the rest are in Settings
                    return;
                }
            }
            break;
        }
        default: return;
    }
    RedrawRight();  // the new state, on screen now
    if (!m_deferred.empty()) PostMessageW(m_hwnd, kMsgDeferred, 0, 0);  // the system call follows, after the UI has had its turn
}

// The slow part of a click (see CcClick), run from the message loop after the optimistic repaint; then it repaints with
// what the system reports, so a toggle that did not take goes back.
void Launcher::RunDeferred() {
    auto work = std::move(m_deferred);
    m_deferred.clear();
    for (auto& f : work) {
        try { f(); } catch (...) { Log(L"a control's service call threw"); }
    }
    if (m_open && m_servicesOn) RedrawRight();  // radios reconcile through kMsgRadios when the system reports; DND, night light, dark mode were read back above
}

// A slider drag, x in panel coordinates. Volume applies at once; brightness (a WMI call) at most every 60 ms,
// and the last value always lands when the button comes up. The fill is a shape: nothing is redrawn while dragging,
// except the glyph when the speaker goes from silent to audible.
void Launcher::CcDrag(float x) {
    const int i = CcIndexOf(static_cast<Kind>(m_drag), 0);
    if (i < 0) return;
    auto const& r = m_ccHits[i].r;
    const float h = ui::kSliderH * m_s, f = std::clamp((x - r.left - h / 2) / std::max(1.0f, r.right - r.left - h), 0.0f, 1.0f);
    const bool dry = m_dryRun || svc::DryRun();
    bool glyphChanged = false;
    if (static_cast<Kind>(m_drag) == Kind::CcVol) {
        if (!m_speaker.Available() && !m_demo) return;
        glyphChanged = (m_vol <= 0.0f) != (f <= 0.0f) || (m_vol < 0.34f) != (f < 0.34f) || (m_vol < 0.67f) != (f < 0.67f);
        m_vol = f;
        if (!dry) m_speaker.SetVolume(f);
    } else {
        if (m_bright < 0) return;
        m_bright = static_cast<int>(std::lround(f * 100));
        const ULONGLONG now = GetTickCount64();
        if (!dry && now - m_lastBrightSet >= 60) {
            m_lastBrightSet = now;
            svc::brightness::Set(m_bright);
        }
    }
    if (glyphChanged) RedrawRight();
    else ApplySliders(false);
}

void Launcher::SetCol(int col) {
    if (!m_h2 || col == m_col) return;
    m_col = col;
    if (m_caret) m_caret.IsVisible(col == 0);
    if (col == 1 && (m_ccFocus < 0 || m_ccFocus >= static_cast<int>(m_ccHits.size()))) m_ccFocus = m_ccHits.empty() ? -1 : 0;
    RedrawRight();
}

// Keys while the right column has the keyboard: Tab / Shift+Tab step through the controls in reading order (and leave
// to search at either end), arrows move to the neighbour (and adjust a slider), Enter or Space acts, Esc backs out.
void Launcher::CcKey(WPARAM vk) {
    if (m_ccHits.empty()) return;
    if (m_ccFocus < 0 || m_ccFocus >= static_cast<int>(m_ccHits.size())) m_ccFocus = 0;
    auto const cur = m_ccHits[m_ccFocus];
    if (vk == VK_RETURN || vk == VK_SPACE) {
        Hit copy = cur;
        if (cur.kind == Kind::CcBright || cur.kind == Kind::CcVol) return;
        CcClick(copy);
        return;
    }
    if (vk != VK_LEFT && vk != VK_RIGHT && vk != VK_UP && vk != VK_DOWN) return;
    if ((vk == VK_LEFT || vk == VK_RIGHT) && (cur.kind == Kind::CcBright || cur.kind == Kind::CcVol)) {  // ±5 %
        const float d = vk == VK_RIGHT ? 0.05f : -0.05f;
        const bool dry = m_dryRun || svc::DryRun();
        if (cur.kind == Kind::CcVol) {
            if (!m_speaker.Available() && !m_demo) return;
            m_vol = std::clamp(m_vol + d, 0.0f, 1.0f);
            if (!dry) m_speaker.SetVolume(m_vol);
        } else {
            if (m_bright < 0) return;
            m_bright = std::clamp(m_bright + (vk == VK_RIGHT ? 5 : -5), 0, 100);
            if (!dry) svc::brightness::Set(m_bright);
        }
        ApplySliders(true);
        RedrawRight();
        return;
    }
    // Nearest control in that direction: along the row for left/right, the row above or below for up/down.
    const float cx = Mid(cur.r.left, cur.r.right), cy = Mid(cur.r.top, cur.r.bottom), ch = cur.r.bottom - cur.r.top;
    int best = -1;
    float bestScore = 1e9f;
    for (size_t i = 0; i < m_ccHits.size(); i++) {
        if (static_cast<int>(i) == m_ccFocus) continue;
        auto const& r = m_ccHits[i].r;
        const float x = Mid(r.left, r.right), y = Mid(r.top, r.bottom), dx = x - cx, dy = y - cy;
        float score;
        if (vk == VK_LEFT || vk == VK_RIGHT) {
            if (std::fabs(dy) > std::min(ch, r.bottom - r.top) / 2 || (vk == VK_RIGHT ? dx <= 0 : dx >= 0)) continue;
            score = std::fabs(dx);
        } else {
            if (vk == VK_DOWN ? dy <= ch / 4 : dy >= -ch / 4) continue;
            score = std::fabs(dy) * 4 + std::fabs(dx);
        }
        if (score < bestScore) {
            bestScore = score;
            best = static_cast<int>(i);
        }
    }
    if (best >= 0) {
        m_ccFocus = best;
        FocusVisible();
        RedrawRight();
    } else if (cur.kind == Kind::CcRow && (vk == VK_UP || vk == VK_DOWN)) {  // past the first or last row of a list: scroll
        const int total = static_cast<int>(m_detail == Detail::Wifi ? m_wifiRows.size() : m_detail == Detail::Bt ? m_btRows.size() : m_outs.size());
        const int next = std::clamp(m_ccScroll + (vk == VK_DOWN ? 1 : -1), 0, std::max(0, total - m_ccRowsFit));
        if (next != m_ccScroll) {
            const int row = cur.index;
            m_ccScroll = next;
            LayoutRight();
            m_ccFocus = CcIndexOf(Kind::CcRow, std::clamp(row + (vk == VK_DOWN ? 1 : -1), m_ccScroll, m_ccScroll + m_ccRowsFit - 1));
            RedrawRight();
        }
    }
}

// Scrolls the focused control into the viewport.
void Launcher::FocusVisible() {
    if (m_detail != Detail::None || m_ccFocus < 0 || m_ccFocus >= static_cast<int>(m_ccHits.size())) return;
    const auto r = m_ccHits[m_ccFocus].r;
    const float pad = start::kRightPad * m_s;
    if (r.top < pad) ScrollRight(r.top - pad);
    else if (r.bottom > m_barTop - pad) ScrollRight(r.bottom - m_barTop + pad);
}

// Tab (dir = 1) and Shift+Tab (dir = -1): the next control in the right column, or back to search past either end.
void Launcher::CcTab(int dir) {
    if (m_col == 0) {
        SetCol(1);
        m_ccFocus = dir > 0 ? 0 : static_cast<int>(m_ccHits.size()) - 1;
    } else {
        const int next = m_ccFocus + dir;
        if (next < 0 || next >= static_cast<int>(m_ccHits.size())) {
            SetCol(0);
            return;
        }
        m_ccFocus = next;
    }
    FocusVisible();
    RedrawRight();
}

}  // namespace halo
