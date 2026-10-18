// Counts the frames the desktop actually presents while something animates, to check for 60 fps.
// Desktop Duplication hands over every frame DWM composes on the primary output; while an animation
// runs there should be one every refresh (16.7 ms at 60 Hz), and a longer gap is a dropped frame.
//   framegaps.exe <ms>   prints each present interval, then the count of gaps over 1.5 refreshes.
//   FG_ROI=l,t,r,b       only count frames that changed that screen rectangle.
// Build: tools/build.sh framegaps
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    const int ms = argc > 1 ? _wtoi(argv[1]) : 1500;
    ID3D11Device* dev = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, nullptr))) return 1;
    IDXGIDevice* xd = nullptr;
    dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&xd));
    IDXGIAdapter* ad = nullptr;
    xd->GetAdapter(&ad);
    IDXGIOutput* out = nullptr;
    if (FAILED(ad->EnumOutputs(0, &out))) return 2;
    IDXGIOutput1* out1 = nullptr;
    out->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&out1));
    IDXGIOutputDuplication* dup = nullptr;
    if (FAILED(out1->DuplicateOutput(dev, &dup))) return 3;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    std::vector<LONGLONG> t;
    // FG_ROI=l,t,r,b: count only frames that touched that rectangle (to ignore, say, a video elsewhere).
    RECT region{};
    const bool roi = getenv("FG_ROI") && sscanf(getenv("FG_ROI"), "%ld,%ld,%ld,%ld", &region.left, &region.top, &region.right, &region.bottom) == 4;
    unsigned accumulated = 0;
    const ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end) {
        DXGI_OUTDUPL_FRAME_INFO fi{};
        IDXGIResource* r = nullptr;
        HRESULT hr = dup->AcquireNextFrame(50, &fi, &r);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) return 4;
        bool keep = fi.LastPresentTime.QuadPart != 0;
        if (keep && roi && fi.TotalMetadataBufferSize) {  // only frames that changed something inside the region
            std::vector<BYTE> buf(fi.TotalMetadataBufferSize);
            UINT got = 0;
            keep = false;
            if (SUCCEEDED(dup->GetFrameDirtyRects(static_cast<UINT>(buf.size()), reinterpret_cast<RECT*>(buf.data()), &got)))
                for (UINT i = 0; i < got / sizeof(RECT); i++) {
                    RECT x;
                    if (IntersectRect(&x, &reinterpret_cast<RECT*>(buf.data())[i], &region)) keep = true;
                }
        }
        if (keep) { t.push_back(fi.LastPresentTime.QuadPart); accumulated += fi.AccumulatedFrames; }
        if (getenv("FG_RECTS") && fi.TotalMetadataBufferSize && t.size() <= static_cast<size_t>(atoi(getenv("FG_RECTS")))) {  // where the screen changed
            std::vector<BYTE> buf(fi.TotalMetadataBufferSize);
            UINT got = 0;
            if (SUCCEEDED(dup->GetFrameDirtyRects(static_cast<UINT>(buf.size()), reinterpret_cast<RECT*>(buf.data()), &got)))
                for (UINT i = 0; i < got / sizeof(RECT); i++) {
                    RECT const& d = reinterpret_cast<RECT*>(buf.data())[i];
                    std::printf("dirty %ld,%ld %ldx%ld\n", d.left, d.top, d.right - d.left, d.bottom - d.top);
                }
        }
        r->Release();
        dup->ReleaseFrame();
    }
    DXGI_OUTDUPL_DESC d;
    dup->GetDesc(&d);
    const double period = 1000.0 * d.ModeDesc.RefreshRate.Denominator / (d.ModeDesc.RefreshRate.Numerator ? d.ModeDesc.RefreshRate.Numerator : 60);
    int long_ = 0;
    std::printf("frames %zu (DWM presents %u), refresh %.2f ms\nintervals ms:", t.size(), accumulated, period);
    for (size_t i = 1; i < t.size(); i++) {
        double g = (t[i] - t[i - 1]) * 1000.0 / f.QuadPart;
        std::printf(" %.1f", g);
        if (g > period * 1.5 && g < 200) { long_++; std::printf(" [drop@%.0f]", (t[i] - t[0]) * 1000.0 / f.QuadPart); }  // >200 ms: nothing was moving, not a drop
    }
    std::printf("\ngaps over 1.5 refreshes (dropped, excluding idle >200 ms): %d\n", long_);
    return 0;
}
