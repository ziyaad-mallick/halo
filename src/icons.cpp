#include "icons.h"

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <shobjidl.h>
#include <propsys.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

namespace {

// IID of IImageList (commoncontrols.h). MinGW's uuid library does not export it.
const IID kIID_IImageList = {0x46EB5926, 0x582E, 0x4017, {0x9F, 0xDF, 0xE8, 0x99, 0x8D, 0xAA, 0x09, 0x50}};

// PKEY_AppUserModel_ID ({9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3}, 5), defined by hand like the IID above.
const PROPERTYKEY kPKEY_AppUserModel_ID = {
    {0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};

const wchar_t kAppsFolder[] = L"shell:AppsFolder\\";
const wchar_t kTaskbandKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Taskband";

// Minimal owning wrapper for COM interface pointers.
template <typename T>
class ComRef {
public:
    ComRef() = default;
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;
    ~ComRef() {
        if (ptr_) ptr_->Release();
    }
    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    T** put() { return &ptr_; }

private:
    T* ptr_ = nullptr;
};

// Minimal owning wrapper for Win32 handles released by a function.
template <typename T, auto Release>
class Owned {
public:
    explicit Owned(T h = nullptr) : h_(h) {}
    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;
    ~Owned() {
        if (h_) Release(h_);
    }
    T get() const { return h_; }

private:
    T h_;
};

std::wstring StripExtension(const std::wstring& file) {
    const auto dot = file.rfind(L'.');
    return dot == std::wstring::npos ? file : file.substr(0, dot);
}

// Resolves a .lnk to its target path. Reads the stored path first; only asks
// the shell to resolve (no UI, no drive search, no write-back) when that path
// is missing on disk.
std::wstring ResolveLinkTarget(const std::wstring& linkPath, std::wstring* aumid) {
    ComRef<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, reinterpret_cast<void**>(link.put())))) {
        return {};
    }
    ComRef<IPersistFile> file;
    if (FAILED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(file.put())))) {
        return {};
    }
    if (FAILED(file->Load(linkPath.c_str(), STGM_READ))) {
        return {};
    }

    if (aumid) {
        ComRef<IPropertyStore> props;
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(link->QueryInterface(IID_IPropertyStore, reinterpret_cast<void**>(props.put()))) &&
            SUCCEEDED(props->GetValue(kPKEY_AppUserModel_ID, &v)) && v.vt == VT_LPWSTR && v.pwszVal) {
            *aumid = v.pwszVal;
        }
        PropVariantClear(&v);
    }

    std::wstring target;
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(link->GetPath(buf, MAX_PATH, nullptr, SLGP_UNCPRIORITY))) {
        target = buf;
    }

    if (target.empty() || GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (SUCCEEDED(link->Resolve(nullptr, SLR_NO_UI | SLR_NOUPDATE | SLR_NOSEARCH))) {
            wchar_t resolved[MAX_PATH] = {};
            if (SUCCEEDED(link->GetPath(resolved, MAX_PATH, nullptr, SLGP_UNCPRIORITY)) &&
                resolved[0] != L'\0') {
                target = resolved;
            }
        }
    }
    return target;
}

// Premultiplies in place. Exact for alpha 0 and 255, rounded otherwise.
void Premultiply(std::vector<uint8_t>& px) {
    for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
        const uint32_t a = px[i + 3];
        for (int c = 0; c < 3; ++c) {
            const uint32_t t = static_cast<uint32_t>(px[i + c]) * a + 127;
            px[i + c] = static_cast<uint8_t>((t + (t >> 8)) >> 8);
        }
    }
}

// Converts an HICON to premultiplied BGRA at the icon's native bitmap size.
bool HIconToBgra(HICON icon, IconImage& out) {
    ICONINFO info = {};
    if (!GetIconInfo(icon, &info)) return false;
    Owned<HBITMAP, &DeleteObject> colorBmp(info.hbmColor);
    Owned<HBITMAP, &DeleteObject> maskBmp(info.hbmMask);
    if (!info.hbmColor) return false;  // monochrome icon: not supported

    BITMAP bm = {};
    if (GetObjectW(info.hbmColor, sizeof bm, &bm) == 0) return false;
    const int w = bm.bmWidth;
    const int h = bm.bmHeight;
    if (w <= 0 || h <= 0) return false;

    Owned<HDC, &DeleteDC> dc(CreateCompatibleDC(nullptr));
    if (!dc.get()) return false;

    // Color bitmap as top-down 32bpp.
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    std::vector<uint8_t> px(static_cast<std::size_t>(w) * h * 4);
    if (GetDIBits(dc.get(), info.hbmColor, 0, static_cast<UINT>(h), px.data(), &bmi,
                  DIB_RGB_COLORS) == 0) {
        return false;
    }

    bool hasAlpha = false;
    for (std::size_t i = 3; i < px.size(); i += 4) {
        if (px[i] != 0) {
            hasAlpha = true;
            break;
        }
    }

    // Legacy icons carry no alpha in the color bitmap; the 1bpp AND mask
    // decides coverage (bit 1 = transparent, bit 0 = opaque).
    if (!hasAlpha) {
        if (info.hbmMask) {
            struct MonoInfo {
                BITMAPINFOHEADER hdr;
                RGBQUAD colors[2];
            } mono = {};
            mono.hdr.biSize = sizeof(BITMAPINFOHEADER);
            mono.hdr.biWidth = w;
            mono.hdr.biHeight = -h;
            mono.hdr.biPlanes = 1;
            mono.hdr.biBitCount = 1;
            mono.hdr.biCompression = BI_RGB;
            mono.colors[1] = {255, 255, 255, 0};

            const std::size_t stride = ((static_cast<std::size_t>(w) + 31) / 32) * 4;
            std::vector<uint8_t> bits(stride * h);
            if (GetDIBits(dc.get(), info.hbmMask, 0, static_cast<UINT>(h), bits.data(),
                          reinterpret_cast<BITMAPINFO*>(&mono), DIB_RGB_COLORS) == 0) {
                return false;
            }
            for (int y = 0; y < h; ++y) {
                const uint8_t* row = bits.data() + stride * y;
                for (int x = 0; x < w; ++x) {
                    const bool transparent = (row[x >> 3] >> (7 - (x & 7))) & 1;
                    px[(static_cast<std::size_t>(y) * w + x) * 4 + 3] = transparent ? 0 : 255;
                }
            }
        } else {
            for (std::size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        }
    }

    Premultiply(px);
    out.width = w;
    out.height = h;
    out.bgra = std::move(px);
    return true;
}

// Premultiplied BGRA from a 32bpp HBITMAP (IShellItemImageFactory). The shell
// hands back straight alpha, so premultiply when any channel exceeds alpha.
bool HBitmapToBgra(HBITMAP bmp, IconImage& out) {
    BITMAP bm = {};
    if (GetObjectW(bmp, sizeof bm, &bm) == 0 || bm.bmWidth <= 0 || bm.bmHeight <= 0) return false;
    const int w = bm.bmWidth;
    const int h = bm.bmHeight;
    Owned<HDC, &DeleteDC> dc(CreateCompatibleDC(nullptr));
    if (!dc.get()) return false;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> px(static_cast<std::size_t>(w) * h * 4);
    if (GetDIBits(dc.get(), bmp, 0, static_cast<UINT>(h), px.data(), &bmi, DIB_RGB_COLORS) == 0) {
        return false;
    }

    bool hasAlpha = false;
    bool straight = false;
    for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
        if (px[i + 3]) hasAlpha = true;
        if (px[i] > px[i + 3] || px[i + 1] > px[i + 3] || px[i + 2] > px[i + 3]) straight = true;
    }
    if (!hasAlpha) return false;
    if (straight) Premultiply(px);
    out.width = w;
    out.height = h;
    out.bgra = std::move(px);
    return true;
}

// 256px icon of a shell:AppsFolder\<AUMID> item (Store apps have no file for the image list).
bool LoadAppsFolderIcon(const std::wstring& path, IconImage& out) {
    ComRef<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_IShellItemImageFactory,
                                           reinterpret_cast<void**>(factory.put())))) {
        return false;
    }
    HBITMAP hbmp = nullptr;
    if (FAILED(factory->GetImage(SIZE{256, 256}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbmp)) || !hbmp) {
        return false;
    }
    Owned<HBITMAP, &DeleteObject> bmp(hbmp);
    return HBitmapToBgra(hbmp, out);
}

std::wstring ItemName(IShellItem* item, SIGDN kind) {
    wchar_t* raw = nullptr;
    std::wstring s;
    if (SUCCEEDED(item->GetDisplayName(kind, &raw)) && raw) s = raw;
    CoTaskMemFree(raw);
    return s;
}

// True if the bytes are a well-formed ITEMIDLIST: an SHITEMID chain whose
// 2-byte terminator sits exactly at the end.
bool ValidPidl(const uint8_t* p, std::size_t size) {
    std::size_t pos = 0;
    while (pos + 2 <= size) {
        const std::size_t cb = p[pos] | (p[pos + 1] << 8);
        if (cb == 0) return pos + 2 == size;
        if (cb < 4 || cb > size - pos) return false;
        pos += cb;
    }
    return false;
}

// Taskband\Favorites: repeated [0x00][DWORD size][ITEMIDLIST of that size],
// ended by 0xFF. An unknown record type stops the parse; entries that fail to
// parse are skipped.
void LoadFromTaskband(std::vector<PinnedApp>& apps) {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kTaskbandKey, L"Favorites", RRF_RT_REG_BINARY, nullptr, nullptr,
                     &size) != ERROR_SUCCESS || size == 0) {
        return;
    }
    std::vector<uint8_t> data(size);
    if (RegGetValueW(HKEY_CURRENT_USER, kTaskbandKey, L"Favorites", RRF_RT_REG_BINARY, nullptr, data.data(),
                     &size) != ERROR_SUCCESS) {
        return;
    }
    data.resize(size);

    std::size_t pos = 0;
    while (pos + 5 <= data.size() && data[pos] == 0x00) {
        const std::size_t len = data[pos + 1] | (data[pos + 2] << 8) | (data[pos + 3] << 16) |
                                (static_cast<std::size_t>(data[pos + 4]) << 24);
        pos += 5;
        if (len > data.size() - pos) break;
        const uint8_t* src = data.data() + pos;
        pos += len;
        if (!ValidPidl(src, len)) continue;

        // The shell wants an aligned, CoTaskMem-allocated copy.
        void* pidl = CoTaskMemAlloc(len);
        if (!pidl) break;
        std::memcpy(pidl, src, len);
        ComRef<IShellItem> item;
        const HRESULT hr = SHCreateItemFromIDList(static_cast<PCIDLIST_ABSOLUTE>(pidl), IID_IShellItem,
                                                  reinterpret_cast<void**>(item.put()));
        CoTaskMemFree(pidl);
        if (FAILED(hr)) continue;

        PinnedApp app;
        app.name = ItemName(item.get(), SIGDN_NORMALDISPLAY);
        const std::wstring fs = ItemName(item.get(), SIGDN_FILESYSPATH);
        if (fs.size() > 4 && _wcsicmp(fs.c_str() + fs.size() - 4, L".lnk") == 0) {
            app.linkPath = fs;
            app.name = StripExtension(app.name);
            app.targetPath = ResolveLinkTarget(fs, &app.aumid);
        } else {
            // Packaged app: the parsing name is the bare AUMID (optionally "...\<AUMID>").
            const std::wstring parse = ItemName(item.get(), SIGDN_DESKTOPABSOLUTEPARSING);
            const auto bs = parse.rfind(L'\\');
            app.aumid = bs == std::wstring::npos ? parse : parse.substr(bs + 1);
            if (app.aumid.empty()) continue;
            app.linkPath = kAppsFolder + app.aumid;
        }
        if (app.name.empty() || app.linkPath.empty()) continue;
        apps.push_back(std::move(app));
    }
}

}  // namespace

std::vector<PinnedApp> LoadPinnedApps() {
    std::vector<PinnedApp> apps;
    LoadFromTaskband(apps);
    if (!apps.empty()) return apps;

    // Fallback: the pinned-shortcut folder in directory order (no Store apps).
    wchar_t appData[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return apps;

    const std::wstring dir =
        std::wstring(appData) + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar\\";

    WIN32_FIND_DATAW fd = {};
    HANDLE find = FindFirstFileW((dir + L"*.lnk").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return apps;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring file = fd.cFileName;
        PinnedApp app;
        app.name = StripExtension(file);
        app.linkPath = dir + file;
        app.targetPath = ResolveLinkTarget(app.linkPath, &app.aumid);
        apps.push_back(std::move(app));
    } while (FindNextFileW(find, &fd));
    FindClose(find);

    return apps;
}

bool LoadJumboIcon(const std::wstring& path, IconImage& out) {
    out = {};
    if (path.starts_with(kAppsFolder)) return LoadAppsFolderIcon(path, out);

    SHFILEINFOW sfi = {};
    if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof sfi, SHGFI_SYSICONINDEX) == 0) {
        return false;
    }

    static const int kLists[] = {SHIL_JUMBO, SHIL_EXTRALARGE};
    for (const int listSize : kLists) {
        ComRef<IImageList> list;
        if (FAILED(SHGetImageList(listSize, kIID_IImageList,
                                  reinterpret_cast<void**>(list.put())))) {
            continue;
        }
        HICON hicon = nullptr;
        if (FAILED(list->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &hicon)) || !hicon) {
            continue;
        }
        Owned<HICON, &DestroyIcon> icon(hicon);
        if (HIconToBgra(hicon, out)) return true;
    }
    return false;
}
