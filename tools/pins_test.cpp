// Prints the pin list as the dock would load it (seeding HALO_PINS_FILE from the taskbar if it is missing).
#include "../src/pins.h"
#include <cstdio>
#include <objbase.h>

int wmain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wprintf(L"file: %ls\n", halo::pins::FilePath().c_str());
    for (auto const& a : halo::pins::Load()) std::wprintf(L"%ls\n", a.linkPath.c_str());
    return 0;
}
