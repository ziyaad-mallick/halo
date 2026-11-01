#pragma once
#include <windows.h>
#include <memory>

namespace halo {
// The device is warmed on a separate thread. A missing device simply selects
// the existing DWM thumbnail renderer for that animation.
class SwoopDevice;
std::shared_ptr<SwoopDevice> WarmSwoopDevice();

class SwoopCompositor {
public:
    static std::unique_ptr<SwoopCompositor> Create(const std::shared_ptr<SwoopDevice>& device,
                                                   HWND overlay, HWND source, RECT from, RECT icon,
                                                   RECT bounds, bool leaving);
    ~SwoopCompositor();
    bool Start();
    // DWM can take time to release dozens of visuals; keep that off the dock.
    static void DisposeAsync(std::unique_ptr<SwoopCompositor> compositor);
private:
    struct Impl;
    explicit SwoopCompositor(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};
}  // namespace halo
