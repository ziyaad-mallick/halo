#include "usage.h"

#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <map>

namespace halo::usage {

std::wstring GpuAdapterOf(std::wstring const& instance) {
    auto luid = instance.find(L"luid_");
    auto phys = instance.find(L"_phys_");
    if (luid == std::wstring::npos || phys == std::wstring::npos || phys < luid) return {};
    auto end = instance.find(L'_', phys + 6);  // the digits after _phys_
    return instance.substr(luid, (end == std::wstring::npos ? instance.size() : end) - luid);
}

std::wstring GpuEngineOf(std::wstring const& instance) {
    auto eng = instance.find(L"_eng_");
    return eng == std::wstring::npos ? std::wstring() : instance.substr(eng + 1);
}

float GpuFigure(std::vector<std::pair<std::wstring, double>> const& items) {
    std::map<std::wstring, std::map<std::wstring, double>> engines;  // adapter -> engine -> sum over processes
    for (auto const& [name, value] : items) {
        auto adapter = GpuAdapterOf(name), engine = GpuEngineOf(name);
        if (adapter.empty() || engine.empty()) continue;
        engines[adapter][engine] += value < 0 ? 0 : value;
    }
    float best = 0;
    for (auto const& [adapter, perEngine] : engines)
        for (auto const& [engine, sum] : perEngine) best = std::max(best, ClampPct(sum));
    return best;
}

float RamPercent() {
    MEMORYSTATUSEX m{sizeof(m)};
    return GlobalMemoryStatusEx(&m) ? ClampPct(m.dwMemoryLoad) : 0.0f;
}

namespace {

bool Valid(DWORD status) { return status == PDH_CSTATUS_VALID_DATA || status == PDH_CSTATUS_NEW_DATA; }

// One PDH query with one counter, or nothing when the counter does not exist on this machine.
struct Query {
    PDH_HQUERY q = nullptr;
    PDH_HCOUNTER c = nullptr;
    bool Open(wchar_t const* path) {
        Close();
        if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) {
            q = nullptr;
            return false;
        }
        if (PdhAddEnglishCounterW(q, path, 0, &c) != ERROR_SUCCESS) {
            Close();
            return false;
        }
        PdhCollectQueryData(q);  // the first collect only sets the baseline of the rate
        return true;
    }
    void Close() {
        if (q) PdhCloseQuery(q);
        q = nullptr;
        c = nullptr;
    }
    bool Ok() const { return q != nullptr; }
    bool Collect() const { return q && PdhCollectQueryData(q) == ERROR_SUCCESS; }
    bool Value(double& out) const {
        PDH_FMT_COUNTERVALUE v{};
        if (!c || PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE, nullptr, &v) != ERROR_SUCCESS || !Valid(v.CStatus)) return false;
        out = v.doubleValue;
        return true;
    }
    bool Array(std::vector<std::pair<std::wstring, double>>& out) const {
        DWORD bytes = 0, count = 0;
        if (!c || PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &bytes, &count, nullptr) != PDH_MORE_DATA) return false;
        std::vector<uint8_t> buf(bytes);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) return false;
        for (DWORD i = 0; i < count; i++)
            if (Valid(items[i].FmtValue.CStatus)) out.emplace_back(items[i].szName, items[i].FmtValue.doubleValue);
        return true;
    }
};

constexpr wchar_t kCpu[] = L"\\Processor(_Total)\\% Processor Time";
constexpr wchar_t kDisk[] = L"\\PhysicalDisk(_Total)\\% Disk Time";
constexpr wchar_t kGpu[] = L"\\GPU Engine(*)\\Utilization Percentage";
constexpr int kGpuRebuildEvery = 5;  // GPU engine instances come and go with processes: the wildcard is re-expanded every 5 s

}  // namespace

void Sampler::Start(HWND notify, UINT msg) {
    if (m_thread.joinable()) return;
    m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_stop) return;
    m_thread = std::thread([this, notify, msg] { Run(notify, msg); });
}

void Sampler::Stop() {
    if (!m_thread.joinable()) return;
    SetEvent(m_stop);
    m_thread.join();
    CloseHandle(m_stop);
    m_stop = nullptr;
}

Sample Sampler::Latest() const {
    std::lock_guard lock(m_mutex);
    return m_latest;
}

void Sampler::Run(HWND notify, UINT msg) {
    Query cpu, disk, gpu, gpuNext;
    cpu.Open(kCpu);
    disk.Open(kDisk);
    gpu.Open(kGpu);
    for (int tick = 1;; tick++) {
        if (WaitForSingleObject(m_stop, 1000) != WAIT_TIMEOUT) break;
        if (gpuNext.Ok()) {  // baselined a second ago: it takes over, and this collect gives it its first rate
            gpu.Close();
            gpu = gpuNext;
            gpuNext = Query{};
        }
        Sample s;
        s.ram = RamPercent();
        double v = 0;
        if (cpu.Collect() && cpu.Value(v)) {
            s.cpu = ClampPct(v);
            s.hasCpu = true;
        }
        if (disk.Collect() && disk.Value(v)) {
            s.disk = ClampPct(v);
            s.hasDisk = true;
        }
        if (gpu.Collect()) {
            std::vector<std::pair<std::wstring, double>> items;
            if (gpu.Array(items)) {
                s.gpu = GpuFigure(items);
                s.hasGpu = true;
            }
        }
        if (tick % kGpuRebuildEvery == 0 && gpu.Ok()) gpuNext.Open(kGpu);
        {
            std::lock_guard lock(m_mutex);
            m_latest = s;
        }
        PostMessageW(notify, msg, 0, 0);
    }
    cpu.Close();
    disk.Close();
    gpu.Close();
    gpuNext.Close();
}

}  // namespace halo::usage
