// Manual test for icons.h: lists pinned taskbar apps, extracts each jumbo icon,
// prints its size and opaque bounding box, and writes build/icons/<NN>-<name>.png.
// Build and run from the repo root (see the build line in the commit message).

#include "icons.h"

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

namespace {

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

// Replaces characters that are invalid in Windows file names.
std::wstring SafeFileName(const std::wstring& name) {
    std::wstring out = name;
    for (wchar_t& c : out) {
        if (c < 32 || wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    }
    return out;
}

bool SavePng(IWICImagingFactory* factory, const IconImage& img, const std::wstring& path) {
    ComRef<IWICBitmap> bitmap;
    if (FAILED(factory->CreateBitmapFromMemory(
            static_cast<UINT>(img.width), static_cast<UINT>(img.height),
            GUID_WICPixelFormat32bppPBGRA, static_cast<UINT>(img.width * 4),
            static_cast<UINT>(img.bgra.size()),
            const_cast<BYTE*>(img.bgra.data()), bitmap.put()))) {
        return false;
    }

    ComRef<IWICStream> stream;
    if (FAILED(factory->CreateStream(stream.put())) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
        return false;
    }

    ComRef<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put())) ||
        FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) {
        return false;
    }

    ComRef<IWICBitmapFrameEncode> frame;
    ComRef<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(frame.put(), props.put())) ||
        FAILED(frame->Initialize(props.get())) ||
        FAILED(frame->SetSize(static_cast<UINT>(img.width), static_cast<UINT>(img.height)))) {
        return false;
    }

    // WIC un-premultiplies PBGRA to straight BGRA when writing the PNG.
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format))) return false;
    if (FAILED(frame->WriteSource(bitmap.get(), nullptr))) return false;
    if (FAILED(frame->Commit()) || FAILED(encoder->Commit())) return false;
    return true;
}

}  // namespace

int wmain() {
    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hrCom)) {
        std::wprintf(L"CoInitializeEx failed: 0x%08lX\n", static_cast<unsigned long>(hrCom));
        return 1;
    }

    int exitCode = 0;
    {
        ComRef<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IWICImagingFactory,
                                    reinterpret_cast<void**>(factory.put())))) {
            std::wprintf(L"WIC factory unavailable\n");
            CoUninitialize();
            return 1;
        }

        CreateDirectoryW(L"build", nullptr);
        CreateDirectoryW(L"build\\icons", nullptr);

        const std::vector<PinnedApp> apps = LoadPinnedApps();
        std::wprintf(L"Pinned apps: %zu\n\n", apps.size());

        int failures = 0;
        for (std::size_t i = 0; i < apps.size(); ++i) {
            const PinnedApp& app = apps[i];
            std::wprintf(L"[%02zu] %ls\n", i, app.name.c_str());
            std::wprintf(L"     link:   %ls\n", app.linkPath.c_str());
            std::wprintf(L"     target: %ls\n", app.targetPath.empty() ? L"(none)" : app.targetPath.c_str());

            IconImage img;
            if (!LoadJumboIcon(app.linkPath, img)) {
                std::wprintf(L"     icon:   FAILED\n");
                ++failures;
                continue;
            }

            // Opaque bounding box: pixels with alpha >= 8 (premultiplied, so alpha is the coverage).
            int minX = img.width, minY = img.height, maxX = -1, maxY = -1;
            for (int y = 0; y < img.height; ++y) {
                for (int x = 0; x < img.width; ++x) {
                    const uint8_t a = img.bgra[(static_cast<std::size_t>(y) * img.width + x) * 4 + 3];
                    if (a >= 8) {
                        if (x < minX) minX = x;
                        if (x > maxX) maxX = x;
                        if (y < minY) minY = y;
                        if (y > maxY) maxY = y;
                    }
                }
            }
            const int bw = maxX < 0 ? 0 : maxX - minX + 1;
            const int bh = maxY < 0 ? 0 : maxY - minY + 1;

            auto alphaAt = [&](int x, int y) {
                return static_cast<int>(img.bgra[(static_cast<std::size_t>(y) * img.width + x) * 4 + 3]);
            };
            const int cornerA[4] = {alphaAt(0, 0), alphaAt(img.width - 1, 0),
                                    alphaAt(0, img.height - 1), alphaAt(img.width - 1, img.height - 1)};

            const bool small = bw * 4 < img.width * 3 && bh * 4 < img.height * 3;
            std::wprintf(L"     icon:   %dx%d, opaque bbox %dx%d at (%d,%d)%ls\n",
                         img.width, img.height, bw, bh, minX < img.width ? minX : 0,
                         minY < img.height ? minY : 0, small ? L"  ** SMALL: under 75% of canvas" : L"");
            std::wprintf(L"     corner alpha: %d %d %d %d\n", cornerA[0], cornerA[1], cornerA[2], cornerA[3]);

            const std::wstring pngPath =
                L"build\\icons\\" + std::wstring(i < 10 ? L"0" : L"") + std::to_wstring(i) + L"-" +
                SafeFileName(app.name) + L".png";
            if (SavePng(factory.get(), img, pngPath)) {
                std::wprintf(L"     png:    %ls\n", pngPath.c_str());
            } else {
                std::wprintf(L"     png:    WRITE FAILED %ls\n", pngPath.c_str());
                ++failures;
            }
            std::wprintf(L"\n");
        }

        std::wprintf(L"Done. failures=%d\n", failures);
        exitCode = failures == 0 ? 0 : 2;
    }

    CoUninitialize();
    return exitCode;
}
