#include "icons.h"

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commoncontrols.h>

#include <cstddef>
#include <utility>

namespace {

// IID of IImageList (commoncontrols.h). MinGW's uuid library does not export it.
const IID kIID_IImageList = {0x46EB5926, 0x582E, 0x4017, {0x9F, 0xDF, 0xE8, 0x99, 0x8D, 0xAA, 0x09, 0x50}};

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
std::wstring ResolveLinkTarget(const std::wstring& linkPath) {
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

}  // namespace

std::vector<PinnedApp> LoadPinnedApps() {
    std::vector<PinnedApp> apps;

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
        app.targetPath = ResolveLinkTarget(app.linkPath);
        apps.push_back(std::move(app));
    } while (FindNextFileW(find, &fd));
    FindClose(find);

    return apps;
}

bool LoadJumboIcon(const std::wstring& path, IconImage& out) {
    out = {};

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
