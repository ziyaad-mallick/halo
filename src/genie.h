#pragma once
// GPU genie: plays the minimize/restore/maximize animations of the Halo Genie
// Windhawk mod (mods/halo-genie.wh.cpp) with Windows.UI.Composition, so DWM
// runs them at vsync with no per-frame work here.
//
// Protocol (the mod's side is documented in its header; this is the same
// contract). The mod posts the registered message "Halo.Genie" to the dock
// window: wParam = op, lParam = id (the mod's pid << 32 | a sequence number).
//   op 1 start    Halo opens, by name, the file mapping  Local\HaloGenie.<id hex>
//                 and the events  Local\HaloGenie.<id hex>.ready / .done
//                 (all created by the mod). The mapping starts with GenieHeader
//                 (64 bytes) followed by the window snapshot, top-down
//                 premultiplied BGRA. Halo uploads it, builds the animation,
//                 and signals .ready once its first frame has been committed
//                 and DWM has composed it; the mod changes the real window only
//                 after that. Halo signals .done when the animation ends.
//   op 2 release  The mod has revealed the real window (restore/maximize kinds,
//                 sent after .done); Halo drops the overlay. Without it Halo
//                 drops it 1 s after .done.
//   op 3 cancel   Nothing changed after all; Halo signals .done and drops it.
// If the mapping can't be opened or parsed, or the message never arrives, .ready
// stays unset and the mod falls back to its own CPU animation.

#include <windows.h>
#include <map>
#include <memory>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>

enum class GenieKind : uint32_t { Minimize, Restore, Maximize, Unmaximize };

struct GenieHeader {
    uint32_t magic;    // 'HGEN'
    uint32_t version;  // 1
    uint32_t kind;     // GenieKind
    uint32_t width, height, stride;  // snapshot, px; stride in bytes
    RECT window;       // the window's extended frame when the animation starts, screen px
    RECT target;       // minimize/restore: the dock icon; maximize/unmaximize: the window's end rect
    uint32_t reserved[2];
};
static_assert(sizeof(GenieHeader) == 64);
constexpr uint32_t kGenieMagic = 0x4847454E;
constexpr size_t kGeniePixelOffset = sizeof(GenieHeader);

class Genie {
public:
    Genie(winrt::Windows::UI::Composition::Compositor const& compositor,
          winrt::Windows::UI::Composition::CompositionGraphicsDevice const& device, HWND dock);
    ~Genie();
    void OnMessage(WPARAM op, LPARAM id);

private:
    struct Anim;
    void Start(unsigned long long id);
    void Finish(unsigned long long id);
    static LRESULT CALLBACK OverlayProc(HWND, UINT, WPARAM, LPARAM);

    winrt::Windows::UI::Composition::Compositor m_comp;
    winrt::Windows::UI::Composition::CompositionGraphicsDevice m_device;
    winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget m_target{nullptr};
    winrt::Windows::UI::Composition::ContainerVisual m_root{nullptr};
    HWND m_dock{}, m_overlay{};
    RECT m_virtual{};  // the overlay's screen rect (the whole virtual desktop)
    std::map<unsigned long long, std::unique_ptr<Anim>> m_anims;
};
