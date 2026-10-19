#define SECURITY_WIN32
#include "launcher.h"
#include "motion.h"
#include "theme.h"
#include "tokens.h"

#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.UI.h>
#include <dwmapi.h>
#include <powrprof.h>
#include <security.h>
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

// Layout, logical px.
constexpr float kPad = 24;
constexpr float kLabelH = 16;
constexpr float kIconSize = 44;
constexpr float kIconPad = 8;     // the hover plate around an app icon
constexpr float kSearchTop = 20, kSearchH = 40;
constexpr float kAppRowH = 64;
constexpr float kFileIcon = 32, kFileCellH = 70, kFileRowPitch = 74;
constexpr float kResultRowH = 34, kResultIcon = 24;
constexpr float kBottomH = 56;
constexpr float kButtonH = 32;
constexpr float kAvatar = 32;
constexpr float kMenuW = 168;
constexpr float kMenuRowH = 32;
constexpr float kMenuPad = 8;
constexpr float kMinWidth = 560, kMaxWidth = 720, kWidth = 640, kScreenMargin = 24;
constexpr float kTop = 8;         // room above the panel for the spring's overshoot
constexpr int kMaxApps = 6, kMaxFiles = 12, kFileCols = 6, kKeepApps = 12, kKeepFiles = 60, kMaxResults = 8, kMaxQuery = 120;
constexpr UINT_PTR kRefreshTimer = 1;
constexpr int kListBuffer = 4, kScrollMs = 180;  // rows drawn past each end of the list; the scroll slide
constexpr UINT kMsgAppsFolder = WM_APP + 1;  // lParam: a Launcher::Found* from the Apps folder thread, owned by the panel
constexpr float kPanelRadius = 28;  // local: theme.h has no panel-radius token
// Icon motion, matched to the dock: hover swells and lifts on a spring, a press squashes, a click bounces.
constexpr float kHoverScale = 1.18f, kHoverLift = 3, kPressScale = 0.94f, kBounceRise = 8;
constexpr float kHoverDamping = 0.6f;
constexpr int kHoverPeriodMs = 45, kBounceMs = 420;

inline winrt::Windows::UI::Color Col(D2D1_COLOR_F c) {
    auto b = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return winrt::Windows::UI::ColorHelper::FromArgb(b(c.a), b(c.r), b(c.g), b(c.b));
}

constexpr wchar_t kMenuItems[5][10] = {L"Sleep", L"Restart", L"Shut down", L"Lock", L"Sign out"};

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

IReference<float3> Ref3(float3 v) { return v; }  // spring animations take IReference<float3>
IReference<float2> Ref2(float2 v) { return v; }


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
void Launcher::LoadFiles() {
    m_files.clear();
    m_allFiles.clear();
    wchar_t* dir = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Recent, 0, nullptr, &dir))) return;
    std::wstring folder = std::wstring(dir) + L"\\";
    CoTaskMemFree(dir);

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
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, link.put_void()))) return;
    auto file = link.as<IPersistFile>();
    size_t looked = 0;
    for (auto const& l : links) {
        if (m_allFiles.size() >= kKeepFiles || ++looked > 160) break;
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
        if (std::any_of(m_allFiles.begin(), m_allFiles.end(), [&](FileEntry const& e) { return !_wcsicmp(e.path.c_str(), path.c_str()); }))
            continue;
        m_allFiles.push_back({path.substr(path.find_last_of(L'\\') + 1), path});
    }
    m_files.assign(m_allFiles.begin(), m_allFiles.begin() + std::min<size_t>(m_allFiles.size(), kMaxFiles));
}

// Every app, once: the Start-menu shortcuts (per user, then all users) now, then what the Apps folder has that
// they don't (Store apps such as Settings). What search and All apps look through.
void Launcher::BuildIndex() {
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    Found found;
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
    if (img == m_fileIcons.end()) {  // a search hit outside the head of the list: its icon is loaded now
        IconImage loaded;
        if (!LoadJumboIcon(path, loaded) || !loaded.width) loaded = {};
        img = m_fileIcons.emplace(key, std::move(loaded)).first;
    }
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
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING, bool ellipsis = false) const {
        auto l = Layout(t, fmt, r.right - r.left, r.bottom - r.top);
        if (!l) return;
        l->SetTextAlignment(align);
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
    Text txt{m_dwrite};
    auto fButton = txt.Format(L"Segoe UI Variable Text", 13 * s, DWRITE_FONT_WEIGHT_NORMAL);

    float wl = std::min(kWidth, (m_anchor.monitor.right - m_anchor.monitor.left) / s - 2 * kScreenMargin);
    wl = std::clamp(wl, std::min(kMinWidth, wl), kMaxWidth);
    m_w = std::round(wl * s);

    // [start] recent_apps / recent_files: 0 hides the section; up to six files is one row.
    auto const& cfg = settings::Get().start;
    const size_t maxApps = std::min(cfg.recentApps, kMaxApps), maxFiles = std::min(cfg.recentFiles, kMaxFiles);
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

    // Top to bottom: search bar, recent apps (one row), recent files (2 x 6), the dock bar under a hairline.
    // The apps and the files share six columns.
    const float innerW = wl - 2 * kPad, colW = innerW / kFileCols;
    const float searchBot = kSearchTop + kSearchH;
    const float appsLabelY = searchBot + 16, appsTop = appsLabelY + kLabelH + 6;
    const float filesLabelY = m_showApps ? appsTop + kAppRowH + 14 : searchBot + 16, filesTop = filesLabelY + kLabelH + 6;
    const float fileRows = maxFiles > kFileCols ? 2.0f : 1.0f;
    const float dividerY = std::max(m_showFiles ? filesTop + (fileRows - 1) * kFileRowPitch + kFileCellH + 12
                                                : m_showApps ? appsTop + kAppRowH + 2 : searchBot + 12,
                                    searchBot + 8 + 4 + kMaxResults * kResultRowH + 4);  // a page of results always fits
    const float bottomY = dividerY + 1;
    m_iconPx = kIconSize;
    m_h = std::round((bottomY + kBottomH) * s);
    m_bodyTop = (searchBot + 8) * s;
    m_bodyBottom = dividerY * s;

    m_searchRect = R(kPad * s, kSearchTop * s, (kPad + innerW) * s, searchBot * s);
    m_appsLabel = R(kPad * s, appsLabelY * s, (kPad + innerW) * s, (appsLabelY + kLabelH) * s);
    m_filesLabel = R(kPad * s, filesLabelY * s, (kPad + innerW) * s, (filesLabelY + kLabelH) * s);
    // "All apps" at the right end of the first label's line (its text, not its plate, on the margin).
    m_allAppsRect = R((kPad + innerW + 8 - 104) * s, (appsLabelY - 6) * s, (kPad + innerW + 8) * s, (appsLabelY + kLabelH + 6) * s);
    m_divider = R(kPad * s, dividerY * s, (wl - kPad) * s, (dividerY + 1) * s);
    m_appCells.clear();
    m_fileCells.clear();
    for (size_t i = 0; i < m_apps.size(); i++) {
        float x = kPad + (i + 0.5f) * colW - kIconSize / 2, y = appsTop + (kAppRowH - kIconSize) / 2;
        m_appCells.push_back(R(x * s, y * s, (x + kIconSize) * s, (y + kIconSize) * s));
    }
    for (size_t i = 0; i < m_files.size(); i++) {
        float x = kPad + (i % kFileCols) * colW + 2, y = filesTop + (i / kFileCols) * kFileRowPitch;
        m_fileCells.push_back(R(x * s, y * s, (x + colW - 4) * s, (y + kFileCellH) * s));
    }

    // Bottom row: picture, name, and the two buttons from the right.
    const float rowMid = (bottomY + kBottomH / 2);
    m_avatarRect = R(kPad * s, (rowMid - kAvatar / 2) * s, (kPad + kAvatar) * s, (rowMid + kAvatar / 2) * s);
    float powerW = 12 + 16 + 6 + txt.Width(L"Power", fButton.get()) / s + 6 + 12 + 12;
    float settingsW = 12 + 16 + 6 + txt.Width(L"Settings", fButton.get()) / s + 12;
    float powerX = wl - kPad + 6 - powerW;  // the button's text, not its plate, lines up with the margin
    float settingsX = powerX - 4 - settingsW;
    m_powerRect = R(powerX * s, (rowMid - kButtonH / 2) * s, (powerX + powerW) * s, (rowMid + kButtonH / 2) * s);
    m_settingsRect = R(settingsX * s, (rowMid - kButtonH / 2) * s, (settingsX + settingsW) * s, (rowMid + kButtonH / 2) * s);
    m_nameRect = R((kPad + kAvatar + 10) * s, (rowMid - 12) * s, (settingsX - 8) * s, (rowMid + 12) * s);
    m_staticHits.clear();
    m_staticHits.push_back({m_settingsRect, Kind::Settings, 0});
    m_staticHits.push_back({m_powerRect, Kind::Power, 0});

    // The Power menu opens above the Power button, right-aligned with it.
    m_menuW = std::round(kMenuW * s);
    m_menuH = std::round((5 * kMenuRowH + 2 * kMenuPad) * s);
    m_menuX = std::round(m_powerRect.right - m_menuW);
    m_menuY = std::round(m_powerRect.top - 6 * s - m_menuH);
    m_menuRows.clear();
    m_menuHits.clear();
    for (int i = 0; i < 5; i++) {
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
    if (m_shadowM > 0) {
        if (auto sh = MakeShadow(m_comp, m_gdev, std::ceil(m_w), std::ceil(m_h), kPanelRadius * s,
                                 theme::P().shadowBlur * s, theme::P().shadowY * s, theme::P().shadow)) {
            sh.Size({m_w, m_h});
            m_shadowHold = m_comp.CreateContainerVisual();
            m_shadowHold.Size({m_w, m_h});
            m_shadowHold.Offset(PanelHome());
            m_shadowHold.Children().InsertAtTop(sh);
            m_root.Children().InsertAtTop(m_shadowHold);
        }
    }
    // The stage clips the panel to the old window rect, so the rise/exit still happens behind the window bottom edge.
    auto stage = m_comp.CreateContainerVisual();
    stage.Offset({m_shadowM, 0, 0});
    stage.Size({m_w, kTop * s + m_h});
    {
        auto sc = m_comp.CreateRectangleClip();
        sc.Left(0); sc.Top(0); sc.Right(m_w); sc.Bottom(kTop * s + m_h);
        stage.Clip(sc);
    }
    m_root.Children().InsertAtTop(stage);
    m_panel = m_comp.CreateContainerVisual();
    m_panel.Size({m_w, m_h});
    m_panel.Offset({0, kTop * s, 0});
    auto clip = m_comp.CreateRoundedRectangleGeometry();
    clip.CornerRadius({kPanelRadius * s, kPanelRadius * s});
    clip.Size({m_w, m_h});
    m_panel.Clip(m_comp.CreateGeometricClip(clip));
    stage.Children().InsertAtTop(m_panel);

    if (m_host.glass) {
        auto glass = m_host.glass(static_cast<int>(std::ceil(m_h)), settings::Get().start);
        glass.Size({m_w, m_h});
        m_panel.Children().InsertAtTop(glass);
    }
    m_content = m_comp.CreateContainerVisual();
    m_content.Size({m_w, m_h});
    m_panel.Children().InsertAtTop(m_content);

    {   // 1 px hairline around the panel (separates light glass from a light wallpaper); inset so the clip keeps all of it
        const float t = std::max(1.0f, std::round(s));
        auto edgeShape = m_comp.CreateRoundedRectangleGeometry();
        edgeShape.Offset({t / 2, t / 2});
        edgeShape.Size({m_w - t, m_h - t});
        edgeShape.CornerRadius({kPanelRadius * s - t / 2, kPanelRadius * s - t / 2});
        auto edgeSprite = m_comp.CreateSpriteShape(edgeShape);
        edgeSprite.StrokeBrush(m_comp.CreateColorBrush(theme::C(theme::P().edge)));
        edgeSprite.StrokeThickness(t);
        auto edge = m_comp.CreateShapeVisual();
        edge.Size({m_w, m_h});
        edge.Shapes().Append(edgeSprite);
        m_panel.Children().InsertAtTop(edge);
    }

    auto plate = [&](wuc::CompositionRoundedRectangleGeometry& shape) {
        shape = m_comp.CreateRoundedRectangleGeometry();
        auto sprite = m_comp.CreateSpriteShape(shape);
        sprite.FillBrush(m_comp.CreateColorBrush(Col(theme::P().fillHover)));
        auto v = m_comp.CreateShapeVisual();
        v.Size({m_w, m_h});
        v.Shapes().Append(sprite);
        v.Opacity(0.0f);
        return v;
    };
    m_hover = plate(m_hoverShape);
    m_content.Children().InsertAtTop(m_hover);
    m_hoverIndex = -1;
    m_hoverOn = m_menuHoverOn = false;

    {   // the dock bar: divider, user, Settings, Power
        float top = m_divider.top, h = m_h - top;
        auto surface = MakeSurface(m_w, h);
        Redraw(surface, &Launcher::DrawStatic);
        auto v = MakeSurfaceVisual(surface, m_w, h);
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
    {   // the body: one text surface, and the icon sprites on top of it
        float h = m_bodyBottom - m_bodyTop;
        m_bodySurface = MakeSurface(m_w, h);
        auto v = MakeSurfaceVisual(m_bodySurface, m_w, h);
        v.Offset({0, m_bodyTop, 0});
        m_content.Children().InsertAtTop(v);
        m_bodyIcons = m_comp.CreateContainerVisual();
        m_bodyIcons.Size({m_w, m_h});
        m_content.Children().InsertAtTop(m_bodyIcons);
    }
    {   // the result rows
        const float h = (std::ceil((m_bodyBottom - m_bodyTop) / (kResultRowH * s)) + 2 * kListBuffer) * kResultRowH * s;
        m_listSurface = MakeSurface(m_w, h);
        m_listClip = m_comp.CreateContainerVisual();
        m_listClip.Size({m_w, m_h});
        m_listInset = m_comp.CreateInsetClip();
        m_listClip.Clip(m_listInset);
        m_listHold = m_comp.CreateContainerVisual();
        m_listHold.Size({m_w, h});
        m_listHold.Children().InsertAtTop(MakeSurfaceVisual(m_listSurface, m_w, h));
        m_listIcons = m_comp.CreateContainerVisual();
        m_listIcons.Size({m_w, h});
        m_listHold.Children().InsertAtTop(m_listIcons);
        m_listClip.Children().InsertAtTop(m_listHold);
        m_content.Children().InsertAtTop(m_listClip);
    }

    // The menu: its own glass, dimming the panel behind it while it is open.
    m_menu = m_comp.CreateContainerVisual();
    m_menu.Size({m_menuW, m_menuH});
    m_menu.Offset({m_menuX, m_menuY, 0});
    m_menu.Opacity(0.0f);
    m_menu.IsVisible(false);
    auto menuClip = m_comp.CreateRoundedRectangleGeometry();
    menuClip.CornerRadius({18 * s, 18 * s});
    menuClip.Size({m_menuW, m_menuH});
    m_menu.Clip(m_comp.CreateGeometricClip(menuClip));
    if (m_host.glass) {
        auto glass = m_host.glass(static_cast<int>(std::ceil(m_menuH)), settings::Get().start);
        glass.Size({m_menuW, m_menuH});
        m_menu.Children().InsertAtTop(glass);
        // Over the panel's own text the dock's 50% tint is not enough; the backdrop is the desktop, not the panel.
        auto veil = m_comp.CreateSpriteVisual();
        veil.Size({m_menuW, m_menuH});
        // theme::P().veil is the full-screen dim; this one frosts the menu, so it follows the glass tint.
        const auto tint = theme::Glass(settings::Get().start).tint;
        veil.Brush(m_comp.CreateColorBrush(theme::Dark() ? theme::C({0xB0, 0x16, 0x16, 0x1A})
                                                          : theme::C({0xD8, tint.r, tint.g, tint.b})));
        m_menu.Children().InsertAtTop(veil);
    }
    m_menuHover = plate(m_menuHoverShape);
    m_menuHover.Size({m_menuW, m_menuH});
    m_menu.Children().InsertAtTop(m_menuHover);
    auto menuSurface = MakeSurface(m_menuW, m_menuH);
    Redraw(menuSurface, &Launcher::DrawMenu);
    m_menu.Children().InsertAtTop(MakeSurfaceVisual(menuSurface, m_menuW, m_menuH));
    m_panel.Children().InsertAtTop(m_menu);
    m_menuOpen = false;

    SearchDraw();
    BuildBody();
}

// The part between the search bar and the dock bar: the home sections, or the results while there is a query.
// Rebuilt on every keystroke; the glass, shadow and bars are not.
void Launcher::BuildBody() {
    const float s = m_s;
    const bool results = Listing();
    m_bodyIcons.Children().RemoveAll();
    m_appSprites.clear();
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
        ic.v.Offset({r.left, r.top, 0});
        ic.v.CenterPoint({w / 2, h / 2, 0});
        ic.v.Brush(brush);
        ic.restY = r.top;
        m_bodyIcons.Children().InsertAtTop(ic.v);
        return ic;
    };

    m_listIcons.Children().RemoveAll();
    m_listHold.StopAnimation(L"Offset.Y");
    m_scrollFrom = 0;
    m_listClip.IsVisible(results);
    if (results) {
        const float rowPx = kResultRowH * s;
        for (size_t i = 0; i < m_results.size(); i++) {
            float top = ListTop() + i * rowPx;
            auto row = R((kPad - 8) * s, top, m_w - (kPad - 8) * s, top + rowPx);
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
            m_hits.push_back({R(c.left - kIconPad * s, c.top - kIconPad * s, c.right + kIconPad * s, c.bottom + kIconPad * s),
                              Kind::App, static_cast<int>(i)});
            m_appSprites.push_back(sprite(m_host.appIcon(m_apps[i]), c));
        }
        for (size_t i = 0; i < m_files.size(); i++) {
            auto const& c = m_fileCells[i];
            m_hits.push_back({c, Kind::File, static_cast<int>(i)});
            float ix = (c.left + c.right) / 2 - kFileIcon * s / 2, iy = c.top + 8 * s;
            m_fileSprites.push_back(sprite(FileBrush(m_files[i].path, kFileIcon), R(ix, iy, ix + kFileIcon * s, iy + kFileIcon * s)));
        }
    }
    if (m_query.empty()) m_hits.push_back({m_allAppsRect, Kind::AllApps, 0});
    for (auto const& h : m_staticHits) m_hits.push_back(h);
    Redraw(m_bodySurface, &Launcher::DrawBody);

    // The plate rests on the selected result, or is away on the home view.
    if (results && !m_resultRows.empty()) Highlight(&m_resultRows[m_sel], 12, false);
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
    dc->CreateSolidColorBrush(pal.dim, dim.put());
    dc->CreateSolidColorBrush(pal.line, line.put());
    const float dy = oy - m_divider.top;
    auto at = [&](D2D1_RECT_F r) { return R(r.left + ox, r.top + dy, r.right + ox, r.bottom + dy); };

    auto fRow = txt.Format(L"Segoe UI Variable Text", 13 * s, DWRITE_FONT_WEIGHT_NORMAL);
    auto fName = txt.Format(L"Segoe UI Variable Text", 14 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    auto fGlyph = txt.Format(L"Segoe Fluent Icons", 16 * s, DWRITE_FONT_WEIGHT_NORMAL);
    auto fChevron = txt.Format(L"Segoe Fluent Icons", 10 * s, DWRITE_FONT_WEIGHT_NORMAL);

    dc->FillRectangle(at(m_divider), line.get());

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
        auto fInitial = txt.Format(L"Segoe UI Variable Text", 15 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        std::wstring initial = m_user.empty() ? L"?" : std::wstring(1, static_cast<wchar_t>(std::towupper(m_user[0])));
        winrt::com_ptr<ID2D1SolidColorBrush> onAccent;
        dc->CreateSolidColorBrush(pal.onAccent, onAccent.put());
        txt.Draw(dc, initial, fInitial.get(), av, onAccent.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    // A hairline ring so a dark picture still has an edge on dark glass.
    dc->DrawEllipse(circle(av), line.get(), 1.0f);
    txt.Draw(dc, m_user, fName.get(), at(m_nameRect), white.get(), DWRITE_TEXT_ALIGNMENT_LEADING, true);

    // Buttons: glyph, label (Power also a chevron).
    auto button = [&](D2D1_RECT_F plate, wchar_t glyph, wchar_t const* label, bool chevron) {
        auto r = at(plate);
        txt.Draw(dc, std::wstring(1, glyph), fGlyph.get(), R(r.left + 12 * s, r.top, r.left + 28 * s, r.bottom), white.get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        txt.Draw(dc, label, fRow.get(), R(r.left + 34 * s, r.top, r.right, r.bottom), white.get());
        if (chevron)
            txt.Draw(dc, L"", fChevron.get(), R(r.right - 24 * s, r.top, r.right - 12 * s, r.bottom), dim.get(),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
    };
    button(m_settingsRect, L'', L"Settings", false);
    button(m_powerRect, L'', L"Power", true);
}

// The search bar: a pill with a magnifier, the typed text (or the placeholder), scrolled to keep the end in view.
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
    dc->CreateSolidColorBrush(pal.dim, dim.put());
    auto pill = D2D1::RoundedRect(R(ox + 0.5f, oy + 0.5f, ox + w - 0.5f, oy + h - 0.5f), h / 2, h / 2);
    dc->FillRoundedRectangle(pill, fill.get());
    dc->DrawRoundedRectangle(pill, line.get(), 1.0f);

    auto fGlyph = txt.Format(L"Segoe Fluent Icons", 16 * s, DWRITE_FONT_WEIGHT_NORMAL);
    auto fText = txt.Format(L"Segoe UI Variable Text", 15 * s, DWRITE_FONT_WEIGHT_NORMAL);
    txt.Draw(dc, L"", fGlyph.get(), R(ox + 14 * s, oy, ox + 34 * s, oy + h), dim.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    const float left = 42 * s, area = w - left - 18 * s;
    if (m_query.empty()) {
        txt.Draw(dc, L"Search apps, settings and files", fText.get(), R(ox + left, oy, ox + left + area, oy + h), dim.get());
        return;
    }
    float tw = txt.Width(m_query, fText.get());
    float shift = std::max(0.0f, tw - area);  // longer than the bar: the end stays visible
    dc->PushAxisAlignedClip(R(ox + left, oy, ox + left + area + 4 * s, oy + h), D2D1_ANTIALIAS_MODE_ALIASED);
    txt.Draw(dc, m_query, fText.get(), R(ox + left - shift, oy, ox + left - shift + tw + 8 * s, oy + h), ink.get());
    dc->PopAxisAlignedClip();
}

// Redraws the search bar and puts the caret at the end of the text; the blink restarts so it stays solid while typing.
void Launcher::SearchDraw() {
    const float s = m_s;
    Redraw(m_searchSurface, &Launcher::DrawSearch);
    Text txt{m_dwrite};
    auto fText = txt.Format(L"Segoe UI Variable Text", 15 * s, DWRITE_FONT_WEIGHT_NORMAL);
    const float w = m_searchRect.right - m_searchRect.left, left = 42 * s, area = w - left - 18 * s;
    float x = m_searchRect.left + left + std::min(m_query.empty() ? 0.0f : txt.Width(m_query, fText.get()), area);
    float cw = std::max(1.0f, std::round(1.5f * s)), ch = 20 * s;
    m_caret.Size({cw, ch});
    m_caret.Offset({x, m_searchRect.top + ((m_searchRect.bottom - m_searchRect.top) - ch) / 2, 0});
    auto blink = m_comp.CreateScalarKeyFrameAnimation();
    blink.InsertKeyFrame(0.0f, 1.0f);
    blink.InsertKeyFrame(0.55f, 1.0f);
    blink.InsertKeyFrame(0.56f, 0.0f);
    blink.InsertKeyFrame(1.0f, 0.0f);
    blink.Duration(std::chrono::milliseconds(1060));
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
    dc->CreateSolidColorBrush(pal.dim, dim.put());
    const float dy = oy - m_bodyTop;
    auto at = [&](D2D1_RECT_F r) { return R(r.left + ox, r.top + dy, r.right + ox, r.bottom + dy); };

    auto fLabel = txt.Format(L"Segoe UI Variable Text", 12 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    auto fRow = txt.Format(L"Segoe UI Variable Text", 13 * s, DWRITE_FONT_WEIGHT_NORMAL);
    auto fName = txt.Format(L"Segoe UI Variable Text", 12 * s, DWRITE_FONT_WEIGHT_NORMAL);

    // "All apps ›" on the home view, "‹ Back" on the list.
    if (m_query.empty()) {
        auto fChevron = txt.Format(L"Segoe Fluent Icons", 9 * s, DWRITE_FONT_WEIGHT_NORMAL);
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
        if (ListHeader()) txt.Draw(dc, L"ALL APPS", fLabel.get(), at(m_appsLabel), dim.get());
        if (m_results.empty())
            txt.Draw(dc, m_query.empty() ? std::wstring(L"No apps found") : L"No results for \"" + m_query + L"\"", fRow.get(),
                     at(R(kPad * s, ListTop(), m_w - kPad * s, ListTop() + kResultRowH * s)), dim.get(),
                     DWRITE_TEXT_ALIGNMENT_LEADING, true);
        // A thin thumb in the right margin when there is more than fits.
        const int total = static_cast<int>(m_matches.size()), rows = ListRows();
        if (total > rows) {
            const float track = rows * kResultRowH * s, h = std::max(24 * s, track * rows / total);
            const float y = ListTop() + (track - h) * m_scroll / (total - rows), x = m_w - (kPad - 8) * s + 3 * s;
            winrt::com_ptr<ID2D1SolidColorBrush> thumb;
            dc->CreateSolidColorBrush(pal.dim, thumb.put());
            thumb->SetOpacity(0.5f);
            dc->FillRoundedRectangle(D2D1::RoundedRect(at(R(x, y, x + 3 * s, y + h)), 1.5f * s, 1.5f * s), thumb.get());
        }
        return;
    }

    if (m_showApps) txt.Draw(dc, L"RECENT APPS", fLabel.get(), at(m_appsLabel), dim.get());
    if (m_showApps && m_apps.empty())
        txt.Draw(dc, L"Apps you open show up here", fRow.get(),
                 at(R(m_appsLabel.left, m_appsLabel.bottom + 6 * s, m_appsLabel.right, m_appsLabel.bottom + 46 * s)), dim.get());
    if (m_showFiles) txt.Draw(dc, L"RECENT FILES", fLabel.get(), at(m_filesLabel), dim.get());
    if (m_showFiles && m_files.empty())
        txt.Draw(dc, L"No recent files", fRow.get(),
                 at(R(m_filesLabel.left, m_filesLabel.bottom + 6 * s, m_filesLabel.right, m_filesLabel.bottom + 34 * s)), dim.get());
    // File names under their icons: one line, ellipsized.
    for (size_t i = 0; i < m_files.size(); i++) {
        auto const& c = m_fileCells[i];
        txt.Draw(dc, m_files[i].name, fName.get(), at(R(c.left + 4 * s, c.top + 44 * s, c.right - 4 * s, c.top + 64 * s)), soft.get(),
                 DWRITE_TEXT_ALIGNMENT_CENTER, true);
    }
}

void Launcher::DrawMenu(ID2D1DeviceContext* dc, float ox, float oy) {
    const float s = m_s;
    Text txt{m_dwrite};
    dc->Clear(D2D1::ColorF(0, 0.0f));
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    winrt::com_ptr<ID2D1SolidColorBrush> white;
    dc->CreateSolidColorBrush(theme::P().text, white.put());
    auto fRow = txt.Format(L"Segoe UI Variable Text", 13 * s, DWRITE_FONT_WEIGHT_NORMAL);
    for (int i = 0; i < 5; i++) {
        auto r = m_menuRows[i];
        txt.Draw(dc, kMenuItems[i], fRow.get(), R(r.left + 12 * s + ox, r.top + oy, r.right + ox, r.bottom + oy), white.get());
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
    ++m_gen;

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
    const float2 corner{0.5f, 1.0f};  // the swoop grows from the bottom centre, just above the dock
    motion::In(m_comp, m_panel, {0, home, 0}, rise, st, corner);
    if (m_shadowHold) motion::In(m_comp, m_shadowHold, PanelHome(), rise, st, corner);
    ShowDim(st.dimDesktop);
    Place();  // shows the window: after the slide is set up, so no frame shows the panel at rest first
    SetTimer(m_hwnd, kRefreshTimer, 420, nullptr);  // once, after the rise has settled

    m_open = true;
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
    unsigned gen = ++m_gen;
    auto batch = m_comp.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
    const float2 rise{0, ui::kEnterTravel * m_s};
    auto const& st = settings::Get().start;
    const float2 corner{0.5f, 1.0f};
    motion::Out(m_comp, m_panel, {0, kTop * m_s, 0}, rise, st, corner);
    if (m_shadowHold) motion::Out(m_comp, m_shadowHold, PanelHome(), rise, st, corner);
    batch.End();
    batch.Completed([this, gen](auto&&, auto&&) {
        if (gen == m_gen && !m_open) Close();
    });
    Log(L"hiding");
}

void Launcher::Close() {
    ShowDim(false);
    ShowWindow(m_hwnd, SW_HIDE);
    m_caret.StopAnimation(L"Opacity");  // nothing animates while hidden
    m_tracking = false;
    if (m_host.closed) m_host.closed();
}

void Launcher::Raise() {
    if (m_open) SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Window position and size for the current layout; the test points are where a test can click (client px).
void Launcher::Place() {
    int x = std::clamp(m_anchor.centerX - static_cast<int>(m_w) / 2, static_cast<int>(m_anchor.monitor.left),
                       static_cast<int>(m_anchor.monitor.right - m_w));
    int margin = static_cast<int>(kTop * m_s);
    int mx = static_cast<int>(m_shadowM), mb = static_cast<int>(m_shadowB);
    int top = m_anchor.bottom - static_cast<int>(m_h) - margin;
    int winW = static_cast<int>(m_w) + 2 * mx, winH = static_cast<int>(m_h) + margin + mb;
    SetWindowPos(m_hwnd, HWND_TOPMOST, x - mx, top, winW, winH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (EnvIs(L"HALO_TEST")) {
        auto mid = [&](D2D1_RECT_F const& r) {
            return std::to_wstring(static_cast<int>((r.left + r.right) / 2 + m_shadowM)) + L"," +
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
    LoadFiles();
    for (auto const& f : m_files) {
        auto key = IconKey(f.path);
        if (m_fileIcons.contains(key)) continue;
        IconImage img;
        if (!LoadJumboIcon(f.path, img) || !img.width) img = {};
        m_fileIcons.emplace(key, std::move(img));
    }
    if (!m_indexed) BuildIndex();
    wchar_t name[256] = {};
    ULONG n = ARRAYSIZE(name);
    if (!GetUserNameExW(NameDisplay, name, &n) || !name[0]) {
        n = ARRAYSIZE(name);
        GetUserNameW(name, reinterpret_cast<LPDWORD>(&n));
    }
    m_user = name[0] ? name : L"User";
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
    std::wstring before;
    for (auto const& f : m_allFiles) before += f.path + L"|";
    bool hadAvatar = !m_avatar.empty();
    Prefetch();
    std::wstring after;
    for (auto const& f : m_allFiles) after += f.path + L"|";
    if (after != before || hadAvatar != !m_avatar.empty()) {
        Log(L"refreshed in the background, rebuilding");
        RunSearch();
        Relayout();
    }
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
        if (rk >= 0) hits.push_back({rk, wcslen(p.name), {Result::Type::Setting, p.name, p.uri}});
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
    dc->CreateSolidColorBrush(pal.dim, dim.put());
    auto fRow = txt.Format(L"Segoe UI Variable Text", 13 * s, DWRITE_FONT_WEIGHT_NORMAL);
    auto fName = txt.Format(L"Segoe UI Variable Text", 12 * s, DWRITE_FONT_WEIGHT_NORMAL);
    static constexpr const wchar_t* kTypeNames[] = {L"App", L"Setting", L"File"};
    const int total = static_cast<int>(m_matches.size()), rows = ListRows();
    const float left = ox + (kPad - 8) * s, right = ox + m_w - (kPad - 8) * s;
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
    if (app) {
        if (m_host.launch) m_host.launch(path);
    } else {
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void Launcher::OnKey(WPARAM vk) {
    switch (vk) {
        case VK_ESCAPE:  // closes the Power menu first, then clears the query, then leaves All apps, then the panel
            if (m_menuOpen) ToggleMenu(false);
            else if (!m_query.empty()) {
                m_query.clear();
                QueryChanged();
            } else if (m_allApps) ToggleAllApps();
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
            Highlight(&m_resultRows[m_sel], 12, false);
            break;
        }
    }
}

// ---- Pointer ------------------------------------------------------------------------------------

Launcher::Hit const* Launcher::HitAt(float x, float y) const {
    if (m_menuOpen)
        for (auto const& h : m_menuHits)
            if (In(h.r, x, y)) return &h;
    for (auto const& h : m_hits)
        if ((!m_menuOpen || h.kind == Kind::Power) && In(h.r, x, y)) return &h;  // the menu is modal to the panel
    return nullptr;
}

Launcher::IconV const* Launcher::IconOf(Hit const& hit) const {
    if (hit.kind == Kind::App && hit.index < static_cast<int>(m_appSprites.size()) && m_appSprites[hit.index].v)
        return &m_appSprites[hit.index];
    if (hit.kind == Kind::File && hit.index < static_cast<int>(m_fileSprites.size()) && m_fileSprites[hit.index].v)
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
        if (on) Animate(visual, L"Opacity", 0.0f, 100);
        on = false;
        return;
    }
    float2 off{r->left, r->top}, size{r->right - r->left, r->bottom - r->top};
    if (!inMenu) m_plateTo = *r;
    shape.CornerRadius({radius * m_s, radius * m_s});
    if (!on) {
        shape.StopAnimation(L"Offset");
        shape.StopAnimation(L"Size");
        shape.Offset(off);
        shape.Size(size);
    } else {
        auto glide = [&](wchar_t const* prop, float2 to) {
            auto sp = m_comp.CreateSpringVector2Animation();
            sp.FinalValue(Ref2(to));
            sp.DampingRatio(0.72f);
            sp.Period(std::chrono::milliseconds(50));
            shape.StartAnimation(prop, sp);
        };
        glide(L"Offset", off);
        glide(L"Size", size);
    }
    on = true;
    Animate(visual, L"Opacity", 1.0f, 100);
}

void Launcher::OnHover(float x, float y) {
    auto const* hit = HitAt(x, y - kTop * m_s);
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
        if (results) Highlight(&m_resultRows[m_sel], 12, false);  // the plate stays on the selection
        else Highlight(nullptr, 0, false);
        Highlight(nullptr, 0, true);
    } else if (hit->kind == Kind::MenuItem) {
        auto r = m_menuRows[hit->index];
        Highlight(&r, 10, true);
    } else {
        D2D1_RECT_F r = hit->r;
        if (hit->kind == Kind::Result) m_sel = hit->index;
        Highlight(&r, hit->kind == Kind::App ? 14 : hit->kind == Kind::AllApps ? 8 : 12, false);
        if (m_menuOpen) Highlight(nullptr, 0, true);
    }
}

void Launcher::ToggleMenu(bool open) {
    if (open == m_menuOpen) return;
    m_menuOpen = open;
    m_hoverIndex = -2;
    Highlight(nullptr, 0, true);
    if (open) {
        m_menu.IsVisible(true);
        m_menu.Offset({m_menuX, m_menuY + 6 * m_s, 0});
        Animate(m_menu, L"Offset.Y", m_menuY, 140);
        Animate(m_menu, L"Opacity", 1.0f, 120);
        Animate(m_content, L"Opacity", 0.3f, 120);
    } else {
        Animate(m_content, L"Opacity", 1.0f, 120);
        auto menu = m_menu;
        auto batch = m_comp.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
        Animate(m_menu, L"Opacity", 0.0f, 100);
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
            ShellExecuteW(nullptr, L"open", L"ms-settings:", nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case Kind::Power: ToggleMenu(!m_menuOpen); break;
        case Kind::AllApps: ToggleAllApps(); break;
        case Kind::MenuItem: {
            int item = hit.index;
            Hide();
            RunPower(item);
            break;
        }
    }
}

// Sleep, Restart, Shut down, Lock, Sign out. Under HALO_TEST (or HALO_TEST_POWER_DRYRUN=1) only logs.
void Launcher::RunPower(int item) {
    if (item < 0 || item > 4) return;
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
    }
}

void Launcher::TestCommand(WPARAM w) {
    if (!m_open) return;
    if (w == 2) ToggleMenu(!m_menuOpen);
    else if (w >= 3 && w < 8) RunPower(static_cast<int>(w) - 3);
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
    return self ? self->OnMessage(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT Launcher::OnMessage(UINT m, WPARAM w, LPARAM l) {
    const float mx = static_cast<float>(static_cast<short>(LOWORD(l)));
    const float my = static_cast<float>(static_cast<short>(HIWORD(l)));
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
            if (w == kRefreshTimer) {
                KillTimer(m_hwnd, kRefreshTimer);
                if (m_open) Refresh();
            }
            return 0;
        case WM_MOUSEMOVE:
            static const bool test = EnvIs(L"HALO_TEST");  // posted test moves would be followed by an instant WM_MOUSELEAVE
            if (!m_tracking && !test) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, m_hwnd, 0};
                m_tracking = TrackMouseEvent(&tme);
            }
            if (m_open) OnHover(mx - m_shadowM, my);
            return 0;
        case kMsgAppsFolder: {
            std::unique_ptr<Found> more(reinterpret_cast<Found*>(l));
            const size_t before = m_index.size();
            AddToIndex(*more);
            Log(L"app index: " + std::to_wstring(m_index.size() - before) + L" more from the Apps folder, " +
                std::to_wstring(m_index.size()) + L" apps");
            if (m_open && Listing()) {  // what is on screen may have changed
                RunSearch();
                BuildBody();
            }
            return 0;
        }
        case WM_MOUSEWHEEL:  // three rows a notch; a touchpad's smaller steps add up
            if (m_open && !m_menuOpen && Listing()) {
                m_wheel -= GET_WHEEL_DELTA_WPARAM(w) * 3;
                const int rows = m_wheel / WHEEL_DELTA;
                m_wheel -= rows * WHEEL_DELTA;
                if (rows) {
                    Scroll(rows);
                    POINT p;  // the list moved under a still pointer: hover what is there now
                    if (GetCursorPos(&p) && ScreenToClient(m_hwnd, &p)) {
                        m_hoverIndex = -2;
                        OnHover(p.x - m_shadowM, static_cast<float>(p.y));
                    }
                }
            }
            return 0;
        case WM_MOUSELEAVE:
            m_tracking = false;
            if (m_open) OnHover(-1, -1);
            return 0;
        case WM_LBUTTONDOWN:
            // The shadow margins (sides, bottom) are outside the panel: treat a click there as a click
            // outside and dismiss, like macOS.
            if (m_open && (mx < m_shadowM || mx >= m_shadowM + m_w || my >= kTop * m_s + m_h)) {
                Hide();
                return 0;
            }
            if (m_open) {
                auto const* hit = HitAt(mx - m_shadowM, my - kTop * m_s);
                m_pressedIndex = hit ? static_cast<int>(hit->kind) * 1000 + hit->index : -1;
                if (auto const* ic = hit ? IconOf(*hit) : nullptr) {  // the press squash
                    auto sq = m_comp.CreateVector3KeyFrameAnimation();
                    sq.InsertKeyFrame(1.0f, {kPressScale, kPressScale, 1.0f}, m_easeOut);
                    sq.Duration(std::chrono::milliseconds(70));
                    ic->v.StartAnimation(L"Scale", sq);
                }
            }
            return 0;
        case WM_LBUTTONUP:
            if (m_open) {
                auto const* hit = HitAt(mx - m_shadowM, my - kTop * m_s);
                int key = hit ? static_cast<int>(hit->kind) * 1000 + hit->index : -1;
                if (hit && key == m_pressedIndex) {
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

}  // namespace halo
