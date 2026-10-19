// halo.exe's half of "Halo can never strand you without a taskbar": whatever way halo.exe ends,
// the native taskbar comes back unless a live guardian is going to do it, and if the guardian
// dies while halo runs, halo starts a new one that adopts it.
#pragma once
#include <windows.h>

namespace halo::safety {

// First thing in wWinMain: crash handlers, and the watch on the guardian that launched us
// (`halo.exe --guardian <pid>`; absent when halo is started by hand).
void Install();

// Last thing before the process ends. `crashed`: restore unconditionally; otherwise only if no
// responsive guardian will (it restores itself when halo exits with code 0).
void Leave(bool crashed);

}  // namespace halo::safety
