// Records when the desktop image really changes, with the DXGI desktop duplication API:
// the QPC time DWM presented each updated frame (LastPresentTime) and how many it merged
// (AccumulatedFrames > 1 = a missed frame). Optionally keeps a few frames as PNGs.
//   framecap.exe <ms> <csv> [<png-dir> <x> <y> <w> <h> <every>]
// csv columns: seen_ms (when we received it), present_ms (when DWM presented it), accumulated;
// times are QPC milliseconds, the first row also gives the QPC at start (qpc_start_ms),
// the same clock as Stopwatch.GetTimestamp() so a driver script can mark its own events.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static double g_freq;
static double Ms(long long q) { return q * 1000.0 / g_freq; }

static uint32_t Crc(uint32_t crc, const uint8_t* p, size_t n) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// PNG with stored (uncompressed) deflate blocks: big but needs no zlib.
static void WritePng(const std::wstring& path, const uint8_t* bgra, int w, int h, int stride) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(w * 3 + 1) * h);
    for (int y = 0; y < h; y++) {
        raw.push_back(0);
        for (int x = 0; x < w; x++) {
            const uint8_t* p = bgra + (size_t)y * stride + x * 4;
            raw.push_back(p[2]); raw.push_back(p[1]); raw.push_back(p[0]);
        }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw.size(); off += 65535) {
        size_t n = std::min<size_t>(65535, raw.size() - off);
        z.push_back(off + n >= raw.size());
        z.push_back(n & 0xFF); z.push_back(n >> 8); z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
    }
    uint32_t ad = b << 16 | a;
    for (int s = 24; s >= 0; s -= 8) z.push_back(ad >> s);
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    auto be = [](uint32_t v) { return std::vector<uint8_t>{(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v}; };
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        auto len = be((uint32_t)data.size());
        fwrite(len.data(), 1, 4, f);
        std::vector<uint8_t> body(type, type + 4);
        body.insert(body.end(), data.begin(), data.end());
        fwrite(body.data(), 1, body.size(), f);
        auto crc = be(Crc(0, body.data(), body.size()));
        fwrite(crc.data(), 1, 4, f);
    };
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    std::vector<uint8_t> ihdr;
    for (uint32_t v : {(uint32_t)w, (uint32_t)h}) { auto x = be(v); ihdr.insert(ihdr.end(), x.begin(), x.end()); }
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    fclose(f);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
    SetProcessDPIAware();
    LARGE_INTEGER fq, t0;
    QueryPerformanceFrequency(&fq);
    g_freq = (double)fq.QuadPart;
    double durMs = _wtof(argv[1]);
    std::wstring pngDir = argc > 3 ? argv[3] : L"";
    int cx = argc > 4 ? _wtoi(argv[4]) : 0, cy = argc > 5 ? _wtoi(argv[5]) : 0, cw = argc > 6 ? _wtoi(argv[6]) : 0,
        ch = argc > 7 ? _wtoi(argv[7]) : 0, every = argc > 8 ? _wtoi(argv[8]) : 1;

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) return 3;
    IDXGIDevice* dxgi = nullptr;
    dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi);
    IDXGIAdapter* ad = nullptr;
    dxgi->GetAdapter(&ad);
    IDXGIOutput* out = nullptr;
    IDXGIOutput1* out1 = nullptr;
    IDXGIOutputDuplication* dup = nullptr;
    // Desktop duplication is per output; with more than one monitor the first output can
    // be a screen that doesn't change. Probe them all; the first that shows a frame wins.
    for (UINT i = 0; !out1; i++) {
        IDXGIOutput* cand = nullptr;
        if (FAILED(ad->EnumOutputs(i, &cand))) break;
        IDXGIOutput1* c1 = nullptr;
        if (SUCCEEDED(cand->QueryInterface(__uuidof(IDXGIOutput1), (void**)&c1))) {
            IDXGIOutputDuplication* cd = nullptr;
            if (SUCCEEDED(c1->DuplicateOutput(dev, &cd))) {
                DXGI_OUTDUPL_FRAME_INFO probe;
                IDXGIResource* probeRes = nullptr;
                HRESULT hr = cd->AcquireNextFrame(900, &probe, &probeRes);
                if (probeRes) probeRes->Release();
                if (hr == S_OK) {
                    cd->ReleaseFrame();
                    out = cand;
                    out1 = c1;
                    dup = cd;
                    fprintf(stderr, "using output %u\n", i);
                    continue;
                }
                cd->Release();
            }
            c1->Release();
        }
        cand->Release();
    }
    if (!out1) { fwprintf(stderr, L"no output produced a frame in 0.9 s (locked screen? idle system?)\n"); return 4; }
    DXGI_OUTDUPL_DESC dd;
    dup->GetDesc(&dd);
    if (!cw) { cw = dd.ModeDesc.Width; ch = dd.ModeDesc.Height; }

    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = cw; sd.Height = ch; sd.MipLevels = sd.ArraySize = 1; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* stage = nullptr;
    dev->CreateTexture2D(&sd, nullptr, &stage);

    FILE* csv = _wfopen(argv[2], L"w");
    QueryPerformanceCounter(&t0);
    fprintf(csv, "qpc_start_ms,%.3f\nseen_ms,present_ms,accumulated\n", Ms(t0.QuadPart));
    int n = 0, shots = 0;
    while (true) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (Ms(now.QuadPart - t0.QuadPart) > durMs) break;
        DXGI_OUTDUPL_FRAME_INFO info;
        IDXGIResource* res = nullptr;
        HRESULT hr = dup->AcquireNextFrame(4, &info, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) break;
        QueryPerformanceCounter(&now);
        if (info.LastPresentTime.QuadPart) {
            fprintf(csv, "%.3f,%.3f,%u\n", Ms(now.QuadPart), Ms(info.LastPresentTime.QuadPart), info.AccumulatedFrames);
            if (!pngDir.empty() && (n++ % every) == 0 && shots < 200) {
                ID3D11Texture2D* tex = nullptr;
                res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
                D3D11_BOX box = {(UINT)cx, (UINT)cy, 0, (UINT)(cx + cw), (UINT)(cy + ch), 1};
                ctx->CopySubresourceRegion(stage, 0, 0, 0, 0, tex, 0, &box);
                D3D11_MAPPED_SUBRESOURCE m;
                if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) {
                    wchar_t name[32];
                    swprintf(name, 32, L"\\f%03d.png", shots++);
                    WritePng(pngDir + name, (const uint8_t*)m.pData, cw, ch, m.RowPitch);
                    ctx->Unmap(stage, 0);
                }
                tex->Release();
            }
        }
        res->Release();
        dup->ReleaseFrame();
    }
    fclose(csv);
    return 0;
}
