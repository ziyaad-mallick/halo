// A frame-accurate "screen recording" of one screen region: every frame DWM presents that changes the
// region is saved as a BMP named by its present time, so an animation can be inspected frame by frame
// (and its gaps seen, as with framegaps.cpp).
//   framedump.exe <ms> <l> <t> <r> <b> <outdir>
// Build: clang++ ... tools/framedump.cpp -ld3d11 -ldxgi
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static void SaveBmp(std::wstring const& path, BYTE const* bgra, int w, int h, int pitch) {
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = -h;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + w * h * 4;
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    for (int y = 0; y < h; y++) fwrite(bgra + y * pitch, 4, w, f);
    fclose(f);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 7) return 1;
    const int ms = _wtoi(argv[1]);
    RECT roi{_wtoi(argv[2]), _wtoi(argv[3]), _wtoi(argv[4]), _wtoi(argv[5])};
    std::wstring out = argv[6];
    CreateDirectoryW(out.c_str(), nullptr);
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) return 2;
    IDXGIDevice* xd = nullptr;
    dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&xd));
    IDXGIAdapter* ad = nullptr;
    xd->GetAdapter(&ad);
    IDXGIOutput* o = nullptr;
    if (FAILED(ad->EnumOutputs(0, &o))) return 3;
    IDXGIOutput1* o1 = nullptr;
    o->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&o1));
    IDXGIOutputDuplication* dup = nullptr;
    if (FAILED(o1->DuplicateOutput(dev, &dup))) return 4;
    const int w = roi.right - roi.left, h = roi.bottom - roi.top;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    dev->CreateTexture2D(&td, nullptr, &staging);
    LARGE_INTEGER f, t0{};
    QueryPerformanceFrequency(&f);
    int n = 0;
    struct Frame {
        long long at;
        std::vector<BYTE> px;
    };
    std::vector<Frame> frames;
    const ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end) {
        DXGI_OUTDUPL_FRAME_INFO fi{};
        IDXGIResource* r = nullptr;
        HRESULT hr = dup->AcquireNextFrame(50, &fi, &r);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) return 5;
        bool touched = n == 0;
        if (fi.LastPresentTime.QuadPart && fi.TotalMetadataBufferSize) {
            std::vector<BYTE> buf(fi.TotalMetadataBufferSize);
            UINT got = 0;
            if (SUCCEEDED(dup->GetFrameDirtyRects(static_cast<UINT>(buf.size()), reinterpret_cast<RECT*>(buf.data()), &got)))
                for (UINT i = 0; i < got / sizeof(RECT); i++) {
                    RECT x;
                    if (IntersectRect(&x, &reinterpret_cast<RECT*>(buf.data())[i], &roi)) touched = true;
                }
        }
        if (touched && fi.LastPresentTime.QuadPart) {
            ID3D11Texture2D* tex = nullptr;
            r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
            D3D11_BOX box{static_cast<UINT>(roi.left), static_cast<UINT>(roi.top), 0, static_cast<UINT>(roi.right), static_cast<UINT>(roi.bottom), 1};
            ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, &box);
            tex->Release();
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
                if (!t0.QuadPart) t0 = fi.LastPresentTime;
                const long long msAt = (fi.LastPresentTime.QuadPart - t0.QuadPart) * 1000 / f.QuadPart;
                // Kept in memory and written at the end: disk writes in the loop would make it miss frames.
                std::vector<BYTE> px(static_cast<size_t>(w) * h * 4);
                for (int y = 0; y < h; y++)
                    memcpy(px.data() + static_cast<size_t>(y) * w * 4, static_cast<BYTE const*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, w * 4);
                frames.push_back({msAt, std::move(px)});
                n++;
                ctx->Unmap(staging, 0);
            }
        }
        r->Release();
        dup->ReleaseFrame();
    }
    for (size_t i = 0; i < frames.size(); i++) {
        wchar_t name[64];
        swprintf(name, 64, L"\\f%03zu_%05lld.bmp", i, frames[i].at);
        SaveBmp(out + name, frames[i].px.data(), w, h, w * 4);
    }
    std::printf("%d frames\n", n);
    return 0;
}
