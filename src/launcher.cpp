#define SECURITY_WIN32
#include "launcher.h"
#include "motion.h"
#include "theme.h"
#include "winkey.h"

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

// Layout, logical px.
constexpr float kPad = 24;
constexpr float kLabelH = 16;
constexpr float kIconSize = 44;
constexpr float kIconPad = 8;     // the hover plate around an app icon
constexpr float kGridPitchX = 76, kGridPitchY = 80;
constexpr float kRowH = 28;
constexpr float kFileIcon = 20;
constexpr float kBottomH = 56;
constexpr float kButtonH = 32;
constexpr float kAvatar = 32;
constexpr float kMenuW = 168;
constexpr float kMenuRowH = 32;
constexpr float kMenuPad = 8;
constexpr float kMinWidth = 640, kMaxWidth = 760, kWidth = 720, kScreenMargin = 24;
constexpr float kRise = 24;       // how far below its place the panel starts
constexpr float kTop = 8;         // room above the panel for the spring's overshoot
constexpr int kMaxApps = 8, kMaxFiles = 6, kKeepApps = 12;
constexpr int kGraceMs = 800;     // how long Command Palette has to take the keyboard
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT_PTR kStackTimer = 2;
constexpr float kPanelRadius = 24;  // local: theme.h has no panel-radius token

inline winrt::Windows::UI::Color Col(D2D1_COLOR_F c) {
    auto b = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return winrt::Windows::UI::ColorHelper::FromArgb(b(c.a), b(c.r), b(c.g), b(c.b));
}

constexpr wchar_t kMenuItems[5][10] = {L"Sleep", L"Restart", L"Shut down", L"Lock", L"Sign out"};

Launcher* g_launcher = nullptr;  // WinEvent callbacks carry no context

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

// Command Palette (and PowerToys Run): the windows that own the keyboard while the panel is up.
std::wstring ExeNameOf(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    std::wstring name;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = ARRAYSIZE(buf);
        if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
            name.assign(buf, n);
            name = name.substr(name.find_last_of(L'\\') + 1);
        }
        CloseHandle(h);
    }
    return Lower(name);
}

bool IsPaletteTarget(std::wstring const& lowered) {
    return lowered.find(L"cmdpal") != std::wstring::npos || lowered.find(L"commandpalette") != std::wstring::npos ||
           lowered.find(L"powerlauncher") != std::wstring::npos;
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

void CALLBACK ForegroundProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    if (g_launcher) g_launcher->OnForeground(hwnd);
}

}  // namespace

Launcher::Launcher(HINSTANCE inst, wuc::Compositor const& comp, wuc::CompositionGraphicsDevice const& gdev,
                   LauncherHost host)
    : m_comp(comp), m_gdev(gdev), m_host(std::move(host)) {
    g_launcher = this;
    m_dryRun = EnvIs(L"HALO_TEST_POWER_DRYRUN") || EnvIs(L"HALO_TEST");
    winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                             reinterpret_cast<IUnknown**>(m_dwrite.put())));
    m_easeOut = m_comp.CreateCubicBezierEasingFunction({0.2f, 0.0f}, {0.0f, 1.0f});
    LoadMru();
    CreateHwnd(inst);
    Prefetch();
    Log(L"ready, " + std::to_wstring(m_mru.size()) + L" recent apps" + (m_dryRun ? L", power actions are DRY RUN" : L""));
}

Launcher::~Launcher() {
    if (m_hook) UnhookWinEvent(m_hook);
    if (m_hwnd) DestroyWindow(m_hwnd);
    g_launcher = nullptr;
}

void Launcher::CreateHwnd(HINSTANCE inst) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"HaloLauncherWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    m_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                             wc.lpszClassName, L"Halo Launcher", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, inst, this);
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
    if (target.empty() || IsPaletteTarget(Lower(target))) return;
    if (!m_mru.empty() && !_wcsicmp(m_mru.front().c_str(), target.c_str())) return;
    std::erase_if(m_mru, [&](std::wstring const& t) { return !_wcsicmp(t.c_str(), target.c_str()); });
    m_mru.insert(m_mru.begin(), target);
    if (m_mru.size() > kKeepApps) m_mru.resize(kKeepApps);
    SaveMru();
}

// The newest non-folder files in the shell's Recent folder, from their shortcuts.
void Launcher::LoadFiles() {
    m_files.clear();
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
        if (m_files.size() >= kMaxFiles || ++looked > 80) break;
        wchar_t target[MAX_PATH] = {};
        if (FAILED(file->Load(l.path.c_str(), STGM_READ)) ||
            FAILED(link->GetPath(target, MAX_PATH, nullptr, SLGP_RAWPATH)) || !target[0])
            continue;
        if (target[0] == L'\\' && target[1] == L'\\') continue;  // network: stat could hang
        DWORD attrs = GetFileAttributesW(target);
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring path = target;
        if (std::any_of(m_files.begin(), m_files.end(), [&](FileEntry const& e) { return !_wcsicmp(e.path.c_str(), path.c_str()); }))
            continue;
        m_files.push_back({path.substr(path.find_last_of(L'\\') + 1), path});
    }
}

// ---- Drawing --------------------------------------------------------------------------------

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
    m_iconMiss = false;
    Text txt{m_dwrite};
    auto fButton = txt.Format(L"Segoe UI Variable Text", 13 * s, DWRITE_FONT_WEIGHT_NORMAL);

    float wl = std::min(kWidth, (m_anchor.monitor.right - m_anchor.monitor.left) / s - 2 * kScreenMargin);
    wl = std::clamp(wl, std::min(kMinWidth, wl), kMaxWidth);
    if (m_stackW > 0) wl = m_stackW / s;  // the palette's exact width, when stacked or glued to it
    m_w = std::round(wl * s);

    // Recent apps: those that still have an icon.
    std::vector<wuc::CompositionBrush> brushes;
    m_apps.clear();
    for (auto const& t : m_mru) {
        if (m_apps.size() >= kMaxApps) break;
        if (auto b = m_host.appIcon ? m_host.appIcon(t) : nullptr) {
            m_apps.push_back(t);
            brushes.push_back(b);
        }
    }

    // Two columns: the apps grid (4 x 2) and the files, a hairline between, the user row under both.
    float gridY = 24 + kLabelH + 8, gapDiv = 16;
    float rowH = kRowH, pitchY = kGridPitchY, bottomH = kBottomH;
    size_t shown = kMaxFiles;
    auto heightFor = [&] { return gridY + std::max(2 * pitchY, shown * rowH) + gapDiv + 1 + bottomH; };
    // Under Command Palette: when its bottom edge is known, tighten the layout step by step to stay below it
    // (rows, grid, user row, header, then fewer files) instead of overlapping it.
    if (m_paletteBottom > 0) {
        float avail = (m_anchor.bottom - kTop * s) - (m_paletteBottom + 4 * s);
        while (heightFor() * s > avail) {
            if (rowH > 22 || pitchY > 64) { rowH = std::max(22.0f, rowH - 1); pitchY = std::max(64.0f, pitchY - 2); }
            else if (gapDiv > 6) gapDiv -= 2;
            else if (bottomH > 40) bottomH -= 2;
            else if (gridY > 32) gridY -= 2;
            else if (pitchY > 42) pitchY -= 2;
            else if (shown > 3) shown--;
            else break;
        }
        Log(L"fit under the palette: avail " + std::to_wstring(static_cast<int>(avail)) + L" px, need " +
            std::to_wstring(static_cast<int>(heightFor() * s)) + L", rows " + std::to_wstring(static_cast<int>(rowH)) +
            L" x " + std::to_wstring(shown) + L", grid pitch " + std::to_wstring(static_cast<int>(pitchY)));
    }
    const float labelY = gridY - kLabelH - 6;
    const float iconSize = std::clamp(pitchY - 12, 30.0f, kIconSize);
    m_iconPx = iconSize;
    const float pad = std::min(kIconPad, (pitchY - iconSize) / 2 - 1);
    const float dividerY = gridY + std::max(2 * pitchY, shown * rowH) + gapDiv;
    const float bottomY = dividerY + 1;
    m_h = std::round((bottomY + bottomH) * s);

    m_hits.clear();
    m_appCells.clear();
    m_fileRows.clear();
    const float pitchX = std::clamp(wl * 0.44f / 4, kGridPitchX, 96.0f);
    const float gridW = 4 * pitchX;
    const float sepX = kPad + gridW + 14;
    const float rightX = sepX + 18;
    for (size_t i = 0; i < m_apps.size(); i++) {
        float x = kPad + (i % 4) * pitchX + (pitchX - iconSize) / 2;
        float y = gridY + (i / 4) * pitchY + (pitchY - iconSize) / 2;
        m_appCells.push_back(R(x * s, y * s, (x + iconSize) * s, (y + iconSize) * s));
        m_hits.push_back({R((x - pad) * s, (y - pad) * s, (x + iconSize + pad) * s,
                            (y + iconSize + pad) * s), Kind::App, static_cast<int>(i)});
    }
    m_appsLabel = R(kPad * s, labelY * s, (kPad + gridW) * s, (labelY + kLabelH) * s);
    m_filesLabel = R(rightX * s, labelY * s, (wl - kPad) * s, (labelY + kLabelH) * s);
    for (size_t i = 0; i < std::min(m_files.size(), shown); i++) {
        auto row = R((rightX - 8) * s, (gridY + i * rowH) * s, (wl - kPad + 8) * s, (gridY + (i + 1) * rowH) * s);
        m_fileRows.push_back(row);
        m_hits.push_back({row, Kind::File, static_cast<int>(i)});
    }
    m_vsep = R(sepX * s, labelY * s, (sepX + 1) * s, (dividerY - gapDiv) * s);
    m_divider = R(kPad * s, dividerY * s, (wl - kPad) * s, (dividerY + 1) * s);

    // Bottom row: picture, name, and the two buttons from the right.
    const float rowMid = (bottomY + bottomH / 2);
    m_avatarRect = R(kPad * s, (rowMid - kAvatar / 2) * s, (kPad + kAvatar) * s, (rowMid + kAvatar / 2) * s);
    float powerW = 12 + 16 + 6 + txt.Width(L"Power", fButton.get()) / s + 6 + 12 + 12;
    float settingsW = 12 + 16 + 6 + txt.Width(L"Settings", fButton.get()) / s + 12;
    float powerX = wl - kPad + 6 - powerW;  // the button's text, not its plate, lines up with the margin
    float settingsX = powerX - 4 - settingsW;
    m_powerRect = R(powerX * s, (rowMid - kButtonH / 2) * s, (powerX + powerW) * s, (rowMid + kButtonH / 2) * s);
    m_settingsRect = R(settingsX * s, (rowMid - kButtonH / 2) * s, (settingsX + settingsW) * s, (rowMid + kButtonH / 2) * s);
    m_nameRect = R((kPad + kAvatar + 10) * s, (rowMid - 12) * s, (settingsX - 8) * s, (rowMid + 12) * s);
    m_hits.push_back({m_settingsRect, Kind::Settings, 0});
    m_hits.push_back({m_powerRect, Kind::Power, 0});

    // The Power menu opens above the Power button, right-aligned with it.
    m_menuW = std::round(kMenuW * s);
    float menuRow = kMenuRowH;
    m_menuH = std::round((5 * menuRow + 2 * kMenuPad) * s);
    m_menuX = std::round(m_powerRect.right - m_menuW);
    m_menuY = std::round(m_powerRect.top - 6 * s - m_menuH);
    if (m_menuY < 6 * s) {
        // A short panel has no room above the button: the menu stands beside it, tighter, on the panel's floor.
        menuRow = std::clamp((m_h / s - 16 - 2 * kMenuPad) / 5, 22.0f, kMenuRowH);
        m_menuH = std::round((5 * menuRow + 2 * kMenuPad) * s);
        m_menuX = std::round(m_powerRect.left - 8 * s - m_menuW);
        m_menuY = std::round(m_h - 8 * s - m_menuH);
    }
    m_menuRows.clear();
    m_menuHits.clear();
    for (int i = 0; i < 5; i++) {
        float top = (kMenuPad + i * menuRow) * s;
        m_menuRows.push_back(R(kMenuPad * s, top, m_menuW - kMenuPad * s, top + menuRow * s));
        m_menuHits.push_back({R(m_menuX + kMenuPad * s, m_menuY + top, m_menuX + m_menuW - kMenuPad * s,
                                m_menuY + top + menuRow * s), Kind::MenuItem, i});
    }

    // Visual tree: [glass] [content: hover plate, text surface, app icons] [menu].
    m_root.Children().RemoveAll();
    m_panel = m_comp.CreateContainerVisual();
    m_panel.Size({m_w, m_h});
    m_panel.Offset({0, kTop * s, 0});
    auto clip = m_comp.CreateRoundedRectangleGeometry();
    clip.CornerRadius({kPanelRadius * s, kPanelRadius * s});
    clip.Size({m_w, m_h});
    if (m_stackW > 0) {  // under the palette: square at the seam, rounded at the bottom
        clip.Offset({0, -40 * s});
        clip.Size({m_w, m_h + 40 * s});
    }
    m_panel.Clip(m_comp.CreateGeometricClip(clip));
    m_root.Children().InsertAtTop(m_panel);

    if (m_host.glass) {
        auto glass = m_host.glass(static_cast<int>(std::ceil(m_h)));
        glass.Size({m_w, m_h});
        m_panel.Children().InsertAtTop(glass);
    }
    m_content = m_comp.CreateContainerVisual();
    m_content.Size({m_w, m_h});
    m_panel.Children().InsertAtTop(m_content);

    {   // 1 px hairline around the panel (separates light glass from a light wallpaper); inset so the clip keeps all of it
        const float t = std::max(1.0f, std::round(s));
        auto edgeShape = m_comp.CreateRoundedRectangleGeometry();
        float top = m_stackW > 0 ? -40 * s : 0;
        edgeShape.Offset({t / 2, top + t / 2});
        edgeShape.Size({m_w - t, m_h - top - t});
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

    auto surface = MakeSurface(m_w, m_h);
    {
        auto interop = surface.as<ICompositionDrawingSurfaceInterop>();
        winrt::com_ptr<ID2D1DeviceContext> dc;
        POINT offset{};
        winrt::check_hresult(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &offset));
        try {
            DrawPanel(dc.get(), static_cast<float>(offset.x), static_cast<float>(offset.y));
        } catch (...) {
            interop->EndDraw();
            throw;
        }
        interop->EndDraw();
    }
    m_content.Children().InsertAtTop(MakeSurfaceVisual(surface, m_w, m_h));

    for (size_t i = 0; i < m_apps.size(); i++) {
        auto icon = m_comp.CreateSpriteVisual();
        icon.Size({m_iconPx * s, m_iconPx * s});
        icon.Offset({m_appCells[i].left, m_appCells[i].top, 0});
        icon.Brush(brushes[i]);
        m_content.Children().InsertAtTop(icon);
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
        auto glass = m_host.glass(static_cast<int>(std::ceil(m_menuH)));
        glass.Size({m_menuW, m_menuH});
        m_menu.Children().InsertAtTop(glass);
        // Over the panel's own text the dock's 50% tint is not enough; the backdrop is the desktop, not the panel.
        auto veil = m_comp.CreateSpriteVisual();
        veil.Size({m_menuW, m_menuH});
        // theme::P().veil is the full-screen dim; this one frosts the menu, so it follows the glass tint.
        auto const& tp = theme::P();
        veil.Brush(m_comp.CreateColorBrush(theme::Dark() ? theme::C({0xB0, 0x16, 0x16, 0x1A})
                                                          : theme::C({0xD8, tp.tint.r, tp.tint.g, tp.tint.b})));
        m_menu.Children().InsertAtTop(veil);
    }
    m_menuHover = plate(m_menuHoverShape);
    m_menuHover.Size({m_menuW, m_menuH});
    m_menu.Children().InsertAtTop(m_menuHover);
    auto menuSurface = MakeSurface(m_menuW, m_menuH);
    {
        auto interop = menuSurface.as<ICompositionDrawingSurfaceInterop>();
        winrt::com_ptr<ID2D1DeviceContext> dc;
        POINT offset{};
        winrt::check_hresult(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &offset));
        try {
            DrawMenu(dc.get(), static_cast<float>(offset.x), static_cast<float>(offset.y));
        } catch (...) {
            interop->EndDraw();
            throw;
        }
        interop->EndDraw();
    }
    m_menu.Children().InsertAtTop(MakeSurfaceVisual(menuSurface, m_menuW, m_menuH));
    m_panel.Children().InsertAtTop(m_menu);
    m_menuOpen = false;
}

void Launcher::DrawPanel(ID2D1DeviceContext* dc, float ox, float oy) {
    const float s = m_s;
    Text txt{m_dwrite};
    dc->Clear(D2D1::ColorF(0, 0.0f));
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    auto const& pal = theme::P();
    winrt::com_ptr<ID2D1SolidColorBrush> white, soft, dim, line;  // "white" = primary ink
    dc->CreateSolidColorBrush(pal.text, white.put());
    dc->CreateSolidColorBrush(pal.textSoft, soft.put());
    dc->CreateSolidColorBrush(pal.dim, dim.put());
    dc->CreateSolidColorBrush(pal.line, line.put());
    auto at = [&](D2D1_RECT_F r) { return R(r.left + ox, r.top + oy, r.right + ox, r.bottom + oy); };

    auto fLabel = txt.Format(L"Segoe UI Variable Text", 11 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    auto fRow = txt.Format(L"Segoe UI Variable Text", 13 * s, DWRITE_FONT_WEIGHT_NORMAL);
    auto fName = txt.Format(L"Segoe UI Variable Text", 14 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    auto fGlyph = txt.Format(L"Segoe Fluent Icons", 16 * s, DWRITE_FONT_WEIGHT_NORMAL);
    auto fChevron = txt.Format(L"Segoe Fluent Icons", 10 * s, DWRITE_FONT_WEIGHT_NORMAL);

    txt.Draw(dc, L"RECENT APPS", fLabel.get(), at(m_appsLabel), dim.get());
    if (m_apps.empty())
        txt.Draw(dc, L"Apps you open show up here", fRow.get(),
                 at(R(m_appsLabel.left, m_appsLabel.bottom + 4 * s, m_appsLabel.right, m_appsLabel.bottom + 44 * s)), dim.get());
    txt.Draw(dc, L"RECENT FILES", fLabel.get(), at(m_filesLabel), dim.get());
    if (m_files.empty())
        txt.Draw(dc, L"No recent files", fRow.get(),
                 at(R(m_filesLabel.left, m_filesLabel.bottom + 6 * s, m_filesLabel.right, m_filesLabel.bottom + (6 + kRowH) * s)),
                 dim.get());

    // Files: the shell's own icon for the type, then the name.
    for (size_t i = 0; i < std::min(m_files.size(), m_fileRows.size()); i++) {
        auto const& row = m_fileRows[i];
        float iconPx = kFileIcon * s;
        float iy = row.top + (row.bottom - row.top - iconPx) / 2;
        auto const& path = m_files[i].path;
        auto it = m_fileIcons.find(IconKey(path));
        if (it == m_fileIcons.end()) m_iconMiss = true;  // Refresh() loads it and rebuilds
        if (it != m_fileIcons.end() && it->second.width) {
            winrt::com_ptr<ID2D1Bitmap> bmp;
            D2D1_BITMAP_PROPERTIES props =
                D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(it->second.width, it->second.height), it->second.bgra.data(),
                                           it->second.width * 4, &props, bmp.put()))) {
                float ix = row.left + 8 * s;
                D2D1_RECT_F dest = R(ix + ox, iy + oy, ix + iconPx + ox, iy + iconPx + oy);
                dc->DrawBitmap(bmp.get(), &dest, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            }
        }
        txt.Draw(dc, m_files[i].name, fRow.get(), at(R(row.left + (8 + kFileIcon + 10) * s, row.top, row.right - 8 * s, row.bottom)),
                 soft.get(), DWRITE_TEXT_ALIGNMENT_LEADING, true);
    }

    dc->FillRectangle(at(m_divider), line.get());
    dc->FillRectangle(at(m_vsep), line.get());

    // The user: account picture (or an initial in a circle) and name.
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
            txt.Draw(dc, L"\uE70D", fChevron.get(), R(r.right - 24 * s, r.top, r.right - 12 * s, r.bottom), dim.get(),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
    };
    button(m_settingsRect, L'\uE713', L"Settings", false);
    button(m_powerRect, L'\uE7E8', L"Power", true);
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

void Launcher::Show(LauncherAnchor const& anchor, bool standalone) {
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    m_anchor = anchor;
    m_s = anchor.scale;
    m_standalone = standalone;
    m_sawPalette = false;
    m_shownAt = std::chrono::steady_clock::now();
    ++m_gen;

    // Everything slow (Recent files, their icons, the account picture) is cached; Refresh() updates it after the show.
    m_stackW = 0; m_glueTop = 0; m_paletteBottom = 0; m_palette = nullptr;  // learned again when the palette appears
    try {
        Build();
    } catch (winrt::hresult_error const& e) {
        Log(L"build FAILED " + std::wstring(e.message()));
        return;
    }

    // Rises out of the gap above the dock with the dock's spring (clipped by the window's bottom edge),
    // growing from its bottom centre.
    const float home = kTop * m_s;
    m_panel.Offset({0, home, 0});
    Place();
    motion::In(m_comp, m_panel, {0, home, 0}, {m_w / 2, m_h}, {0, kRise * m_s});
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    SetTimer(m_hwnd, kRefreshTimer, 420, nullptr);  // once, after the rise has settled

    m_open = true;
    if (!m_hook)
        m_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, ForegroundProc, 0, 0,
                                 WINEVENT_OUTOFCONTEXT);
    winkey::WatchEscape(true);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    Log(L"shown, " + std::to_wstring(m_apps.size()) + L" apps, " + std::to_wstring(m_files.size()) + L" files, built in " +
        std::to_wstring(static_cast<int>((t1.QuadPart - t0.QuadPart) * 1000 / fq.QuadPart)) + L" ms");
}

void Launcher::Hide() {
    if (!m_open) return;
    m_open = false;
    winkey::WatchEscape(false);
    if (m_hook) {
        UnhookWinEvent(m_hook);
        m_hook = nullptr;
    }
    unsigned gen = ++m_gen;
    auto batch = m_comp.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
    motion::Out(m_comp, m_panel, {0, kTop * m_s, 0}, {m_w / 2, m_h}, {0, kRise * m_s});
    batch.End();
    batch.Completed([this, gen](auto&&, auto&&) {
        if (gen == m_gen && !m_open) Close();
    });
    Log(L"hiding");
}

void Launcher::Close() {
    ShowWindow(m_hwnd, SW_HIDE);
    m_tracking = false;
    if (m_host.closed) m_host.closed();
}

void Launcher::Raise() {
    if (m_open) SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

bool Launcher::IsPalette(HWND hwnd) const { return IsPaletteTarget(ExeNameOf(hwnd)); }

// Command Palette has the keyboard while the panel is up; the panel goes when something else gets it.
void Launcher::OnForeground(HWND fg) {
    if (!m_open || !fg) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return;
    if (IsPalette(fg)) {
        m_sawPalette = true;
        if (!m_palette) Stack(GetAncestor(fg, GA_ROOT));
        return;
    }
    // Before the palette has taken focus, the previous window may still announce itself.
    if (!m_standalone && !m_sawPalette &&
        std::chrono::steady_clock::now() - m_shownAt < std::chrono::milliseconds(kGraceMs))
        return;
    Log(L"foreground moved to " + ExeNameOf(fg));
    Hide();
}

// Command Palette's top-level window (hidden or not), to learn where its bottom edge is.
HWND Launcher::PaletteWindow() const {
    struct Find {
        HWND found = nullptr;
    } f;
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        RECT r{};
        if (!GetWindowRect(h, &r) || r.right - r.left < 300 || r.bottom - r.top < 100) return TRUE;
        if (!IsPaletteTarget(ExeNameOf(h)) || !IsWindowVisible(h)) return TRUE;
        reinterpret_cast<Find*>(l)->found = h;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&f));
    return f.found;
}

// Palette and panel as one Start menu: the panel takes the palette's width, and the palette is moved up so it
// sits directly on the panel, the stack centred above the dock. Checked once after the move (the one-shot
// kStackTimer): if PowerToys put the palette back, the panel is glued under the palette where it is instead.
void Launcher::Stack(HWND palette) {
    RECT wr{}, r{};  // the window rect includes the invisible shadow border; the frame bounds are what is seen
    if (!palette || !GetWindowRect(palette, &wr) || wr.right <= wr.left) return;
    if (FAILED(DwmGetWindowAttribute(palette, 9 /* DWMWA_EXTENDED_FRAME_BOUNDS */, &r, sizeof(r))) || r.right <= r.left) r = wr;
    m_palette = palette;
    int pw = r.right - r.left, ph = r.bottom - r.top;
    m_stackW = static_cast<float>(pw);
    Relayout();
    auto const& mon = m_anchor.monitor;
    int fLeft = std::clamp(m_anchor.centerX - pw / 2, static_cast<int>(mon.left), static_cast<int>(mon.right) - pw);
    int fTop = m_anchor.bottom - static_cast<int>(m_h) - ph;
    if (fTop < mon.top + static_cast<int>(8 * m_s)) {  // no room above: glue under it where it is
        Glue(r);
        return;
    }
    int left = fLeft - (r.left - wr.left), top = fTop - (r.top - wr.top);
    SetWindowPos(palette, nullptr, left, top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    m_stackAt = {left, top};
    Log(L"stacking: palette " + std::to_wstring(pw) + L"x" + std::to_wstring(ph) + L" frame from " + std::to_wstring(r.left) +
        L"," + std::to_wstring(r.top) + L" to " + std::to_wstring(fLeft) + L"," + std::to_wstring(fTop));
    SetTimer(m_hwnd, kStackTimer, 250, nullptr);
}

void Launcher::Glue(RECT const& r) {
    m_stackW = static_cast<float>(r.right - r.left);
    m_anchor.centerX = (r.left + r.right) / 2;
    m_glueTop = r.bottom;
    m_paletteBottom = static_cast<float>(r.bottom);
    Log(L"glueing the panel under the palette at " + std::to_wstring(r.left) + L"," + std::to_wstring(r.bottom));
    Relayout();
}

// Window position and size for the current layout; the test points are where a test can click (client px).
void Launcher::Place() {
    int x = std::clamp(m_anchor.centerX - static_cast<int>(m_w) / 2, static_cast<int>(m_anchor.monitor.left),
                       static_cast<int>(m_anchor.monitor.right - m_w));
    int margin = static_cast<int>(kTop * m_s);
    int top = (m_glueTop > 0 ? m_glueTop : m_anchor.bottom - static_cast<int>(m_h)) - margin;
    SetWindowPos(m_hwnd, HWND_TOPMOST, x, top, static_cast<int>(m_w), static_cast<int>(m_h) + margin,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    // Under the palette the transparent margin would sit on top of its bottom edge: leave it out of the window.
    SetWindowRgn(m_hwnd, m_stackW > 0 ? CreateRectRgn(0, margin, static_cast<int>(m_w), static_cast<int>(m_h) + margin) : nullptr, TRUE);
    if (EnvIs(L"HALO_TEST")) {
        auto mid = [&](D2D1_RECT_F const& r) {
            return std::to_wstring(static_cast<int>((r.left + r.right) / 2)) + L"," +
                   std::to_wstring(static_cast<int>((r.top + r.bottom) / 2 + kTop * m_s));
        };
        Log(L"test points: power=" + mid(m_powerRect) + L" settings=" + mid(m_settingsRect) + L" app0=" + mid(m_hits[0].r) +
            L" menu2=" + mid(m_menuHits[2].r) + L" blank=" + mid(m_divider) + L" file0=" +
            (m_fileRows.empty() ? std::wstring(L"0,0") : mid(m_fileRows[0])));
    }
}

// The file icon cache key: the extension, or the whole path for types whose icon is per file.
std::wstring Launcher::IconKey(std::wstring const& path) {
    auto dot = path.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? L"" : Lower(path.substr(dot));
    bool own = ext.empty() || ext == L".exe" || ext == L".lnk" || ext == L".ico" || ext == L".scr" || ext == L".cpl";
    return own ? Lower(path) : ext;
}

// The slow content: Recent files, their icons, the account picture and name. Run at startup and after each Show.
void Launcher::Prefetch() {
    LoadFiles();
    for (auto const& f : m_files) {
        auto key = IconKey(f.path);
        if (m_fileIcons.contains(key)) continue;
        IconImage img;
        if (!LoadJumboIcon(f.path, img) || !img.width) img = {};
        m_fileIcons.emplace(key, std::move(img));
    }
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
    for (auto const& f : m_files) before += f.path + L"|";
    bool hadAvatar = !m_avatar.empty();
    Prefetch();
    std::wstring after;
    for (auto const& f : m_files) after += f.path + L"|";
    if (after != before || m_iconMiss || hadAvatar != !m_avatar.empty()) {
        Log(L"refreshed in the background, rebuilding");
        Relayout();
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

// Moves the single hover plate to the item under the pointer (or fades it away).
void Launcher::Highlight(D2D1_RECT_F const* r, float radius, bool inMenu) {
    auto shape = inMenu ? m_menuHoverShape : m_hoverShape;
    auto visual = inMenu ? m_menuHover : m_hover;
    bool& on = inMenu ? m_menuHoverOn : m_hoverOn;
    if (!r) {
        if (on) Animate(visual, L"Opacity", 0.0f, 100);
        on = false;
        return;
    }
    winrt::Windows::Foundation::Numerics::float2 off{r->left, r->top}, size{r->right - r->left, r->bottom - r->top};
    shape.CornerRadius({radius * m_s, radius * m_s});
    if (!on) {
        shape.StopAnimation(L"Offset");
        shape.StopAnimation(L"Size");
        shape.Offset(off);
        shape.Size(size);
    } else {
        auto move = m_comp.CreateVector2KeyFrameAnimation();
        move.InsertKeyFrame(1.0f, off, m_easeOut);
        move.Duration(std::chrono::milliseconds(90));
        shape.StartAnimation(L"Offset", move);
        auto grow = m_comp.CreateVector2KeyFrameAnimation();
        grow.InsertKeyFrame(1.0f, size, m_easeOut);
        grow.Duration(std::chrono::milliseconds(90));
        shape.StartAnimation(L"Size", grow);
    }
    on = true;
    Animate(visual, L"Opacity", 1.0f, 100);
}

void Launcher::OnHover(float x, float y) {
    auto const* hit = HitAt(x, y - kTop * m_s);
    int key = hit ? static_cast<int>(hit->kind) * 1000 + hit->index : -1;
    if (key == m_hoverIndex) return;
    m_hoverIndex = key;
    if (!hit) {
        Highlight(nullptr, 0, false);
        Highlight(nullptr, 0, true);
    } else if (hit->kind == Kind::MenuItem) {
        auto r = m_menuRows[hit->index];
        Highlight(&r, 10, true);
    } else {
        D2D1_RECT_F r = hit->r;
        Highlight(&r, hit->kind == Kind::File ? 10 : 14, false);
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
        case Kind::App: {
            std::wstring target = m_apps[hit.index];
            Hide();
            if (m_host.launch) m_host.launch(target);
            break;
        }
        case Kind::File: {
            std::wstring path = m_files[hit.index].path;
            Hide();
            ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case Kind::Settings:
            Hide();
            ShellExecuteW(nullptr, L"open", L"ms-settings:", nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case Kind::Power: ToggleMenu(!m_menuOpen); break;
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
    HWND palette = nullptr;
    if (w == 2) ToggleMenu(!m_menuOpen);
    else if (w >= 3 && w < 8) RunPower(static_cast<int>(w) - 3);
    else if (w == 9) OnForeground(GetShellWindow());  // the desktop took the foreground
    else if (w == 10) {                                // Command Palette's process took it
        EnumWindows([](HWND h, LPARAM l) -> BOOL {
            if (!IsWindowVisible(h) || !IsPaletteTarget(ExeNameOf(h))) return TRUE;
            *reinterpret_cast<HWND*>(l) = h;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&palette));
        if (palette) OnForeground(palette);
        else Log(L"test: no Command Palette window to borrow");
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
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_TIMER:
            if (w == kStackTimer) {
                KillTimer(m_hwnd, kStackTimer);
                RECT r{};
                if (m_open && m_palette && GetWindowRect(m_palette, &r) &&
                    (std::abs(r.left - m_stackAt.x) > 3 || std::abs(r.top - m_stackAt.y) > 3)) {
                    Log(L"the palette is not where it was put, gluing under it");
                    Glue(r);
                } else if (m_open) {
                    Log(L"stack holds");
                }
            }
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
            if (m_open) OnHover(mx, my);
            return 0;
        case WM_MOUSELEAVE:
            m_tracking = false;
            if (m_open) OnHover(-1, -1);
            return 0;
        case WM_LBUTTONDOWN:
            if (m_open) {
                auto const* hit = HitAt(mx, my - kTop * m_s);
                m_pressedIndex = hit ? static_cast<int>(hit->kind) * 1000 + hit->index : -1;
            }
            return 0;
        case WM_LBUTTONUP:
            if (m_open) {
                auto const* hit = HitAt(mx, my - kTop * m_s);
                int key = hit ? static_cast<int>(hit->kind) * 1000 + hit->index : -1;
                if (hit && key == m_pressedIndex) {
                    Hit copy = *hit;
                    Click(copy);
                } else if (!hit && m_menuOpen) {
                    ToggleMenu(false);  // a click on the panel itself closes the menu
                }
            }
            m_pressedIndex = -1;
            return 0;
    }
    return DefWindowProcW(m_hwnd, m, w, l);
}

}  // namespace halo
