// The usage monitor behind Start 2.0: RAM, CPU, GPU and disk as percentages.
//
// A Sampler owns one worker thread that exists only between Start() and Stop() (Start is open), wakes once a
// second, reads PDH, publishes a Sample and posts `msg` to the window. Closed means no thread, no PDH query,
// no timer: zero cost. All PDH work is off the UI thread; Latest() only copies a small struct under a lock.
//
//   CPU   \Processor(_Total)\% Processor Time
//   Disk  \PhysicalDisk(_Total)\% Disk Time (it can read over 100 on a busy queue, so it is clamped)
//   GPU   \GPU Engine(*)\Utilization Percentage: summed over the processes of one engine, then the busiest
//         engine of an adapter, then the busiest adapter (the figure Task Manager calls GPU)
//   RAM   GlobalMemoryStatusEx().dwMemoryLoad
#pragma once
#include <windows.h>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace halo::usage {

struct Sample {
    float ram = 0, cpu = 0, gpu = 0, disk = 0;  // 0..100
    bool hasCpu = false, hasGpu = false, hasDisk = false;  // false until the counter produced a value (or is absent)
};

inline float ClampPct(double v) { return v != v ? 0.0f : v < 0 ? 0.0f : v > 100 ? 100.0f : static_cast<float>(v); }

// "pid_1234_luid_0x00000000_0x0000F1B2_phys_0_eng_3_engtype_3D" -> adapter "luid_0x00000000_0x0000F1B2_phys_0".
std::wstring GpuAdapterOf(std::wstring const& instance);
// ... -> engine within the adapter: "eng_3_engtype_3D" (empty when the name is not a GPU Engine instance).
std::wstring GpuEngineOf(std::wstring const& instance);
// (instance name, value) pairs of \GPU Engine(*)\Utilization Percentage -> the figure to show, 0..100.
float GpuFigure(std::vector<std::pair<std::wstring, double>> const& items);

class Sampler {
public:
    ~Sampler() { Stop(); }
    // Starts the worker (a no-op if running). `msg` is posted to `notify` after each sample.
    void Start(HWND notify, UINT msg);
    void Stop();  // joins the worker and closes the queries
    bool Running() const { return m_thread.joinable(); }
    Sample Latest() const;

private:
    void Run(HWND notify, UINT msg);
    mutable std::mutex m_mutex;
    Sample m_latest;
    std::thread m_thread;
    HANDLE m_stop = nullptr;
};

// RAM only; also what the first paint uses before the worker has a first sample.
float RamPercent();

}  // namespace halo::usage
