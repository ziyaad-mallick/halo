# Halo memory: where the 36.5 MB goes (2026-10-17)

Closes the housekeeping item in `docs/HANDOFF.md`: *"Memory 36.5 MB private (README says
17.5) — OPEN, uninvestigated."*

Nothing here touched the running dock: every figure comes from reading a live process, or
from a standalone probe that opens no Halo window and draws nothing on screen.

## What the two numbers actually are

- **17.5 MB does not reproduce on this build at all.** The owner toggled Halo off and on at
  14:21 (his Win+Alt+T; guardian.log), and the **freshly launched** instance (`pid 18312`,
  started 14:21:15) already read **31.7 MB private**, working set 55.5 MB, 477 handles,
  41 GDI objects. So the README figure is not an early-startup reading — it is **stale**,
  left over from an earlier build. The handoff's own note agrees that ~30.5 MB predates the
  polish work ("M1's ~27 icons plus Store icon loading"), i.e. ~30 MB is this build's floor.
- **Readings still fluctuate.** On the previous instance: 32.7 MB at 14:00, 27.9 MB at
  14:20, nothing else changed in between (a ~4.8 MB swing). So a single number cannot
  describe this process in either direction — the README should state a range, and the
  floor is ~30 MB, not 17.5 MB.
- Otherwise the live process is well behaved: 475 handles, 41 GDI objects, 5–7 threads,
  all **flat over 30 s**, and **0 ms CPU over 30 s idle**. There is no idle leak and no
  handle/GDI leak.

Address-space walk of the live process (`VirtualQueryEx`, 964 regions):

| | bytes |
|---|---|
| `MEM_PRIVATE` committed | 24.0 MB |
| `MEM_MAPPED` (sections: composition shared memory, DLL data) | 20.0 MB |
| `MEM_IMAGE` (DLLs — working set, not private bytes) | 111.0 MB |
| reserved (address space only: composition/D3D reservations) | 4.2 GB |

The private 24.0 MB is dominated by **5 regions** (6.8, 4.1, 3.3, 1.9, 1.6 MB ≈ 17 MB):
D3D11/composition arenas. There are **zero** 262144-byte regions, so no retained 256×256
icon buffers — the obvious suspect is not the cause.

## Method: `tools/memprobe.cpp`

```sh
tools/memprobe.exe warp  <mode>     # warp = what Halo::InitGraphics uses
tools/memprobe.exe hw    <mode>     # D3D11_DRIVER_TYPE_HARDWARE, for comparison
```

It rebuilds Halo's exact stack (dispatcher queue → `Compositor` → D3D11 device → D2D1
device → `CompositionGraphicsDevice`) and then makes icon surfaces the way
`Halo::MakeIconBrush` does, printing private commit after every phase. Modes: `none`
(surfaces, no draw), `cubic` (Halo today), `linear`, `downs` (source pre-shrunk to the
magnified size), `clear`, `cap`, `reuse`, `reuse44`, `reclaim`. It compiles with the same
flags and libraries as `tools/build.sh`; it is not part of that script.

## Measurement (WARP, 27 icons at the magnified 44 px)

| draw path | 27 icons | per icon | growth per refresh of 27 |
|---|---|---|---|
| surfaces only, nothing drawn | +0.4 MB | 13 KB | ~0 |
| 256 px source, `HIGH_QUALITY_CUBIC` — **Halo today** | **+7.3 MB** | **277 KB** | **+9.5 MB** |
| 256 px source, `LINEAR` | +7.3 MB | 278 KB | +7.0 MB |
| 256 px source, repaint the **same** surfaces | +7.3 MB | 277 KB | +9.4 MB |
| `ClearResources()` per build | +7.3 MB | 278 KB | +9.8 MB |
| `SetMaximumTextureMemory(2 MB)` | +7.3 MB | 279 KB | +9.5 MB |
| **44 px source, drawn 1:1** | **+0.8 MB** | **30 KB** | **+0.5 MB** |

For reference, the whole graphics stack is cheap: dispatcher queue +0.1, `Compositor` +0.2,
WARP device **+0.8**, D2D1 device +0.4, graphics device +0.0. On the **hardware** driver the
D3D11 device alone is **+26.9 MB** — which confirms the comment in `Halo::InitGraphics`
("17 MB private vs 46 MB on hardware") and means **WARP should stay**.

## Root cause

`Halo::MakeIconBrush` sizes the *surface* correctly — 44 px, about 13 KB — but paints into
it from a `ID2D1Bitmap` built from the **full 256 px** source (262 KB). The cost is in the
**paint**, not the surface: repainting the same surface costs the same as recreating it,
and the interpolation mode makes no difference.

So, per icon drawn from the 256 px source, Halo carries **~277 KB**. That gives:

- the 27 pinned icons ≈ **7.3 MB** of permanent cost;
- and `m_brushes` (`src/halo.cpp:143`) caches one brush per icon source path and **is never
  cleared**, so every *distinct* app path seen in a session adds another ~277 KB forever —
  running apps included, via `IconBrushFor(running[i].aumid / .exe)`.

**What does not work** (measured, so nobody repeats it): changing the interpolation mode;
`ID2D1Device::ClearResources()`; `ID2D1Device::SetMaximumTextureMemory()`. Releasing the
surfaces, graphics device, devices and compositor together did **not** reclaim — it
reported 10.4 MB → 15.9 MB. So this memory is not reliably recoverable from the client
side, and **an LRU/eviction pass on `m_brushes` would not fix it**.

## The lever that works

Draw from a source already shrunk to the magnified size: **277 KB → 30 KB per icon (9×)**,
and the per-refresh growth drops from 9.5 MB to 0.5 MB. For 27 icons: 7.3 MB → 0.8 MB.
The saving is robust to the exact pre-shrink size — 44 px and 88 px both land near 30 KB —
so there is room to keep quality headroom by pre-shrinking to 88 px (2× the displayed size)
and letting D2D do the last cubic step.

The probe's growth figures are an **upper bound**: the live process's private bytes were
observed to fall by ~4.8 MB while idle, so a real process trims some of this.

## Needs the owner's decision (do not guess)

The 256→44 downscale would move from D2D's `HIGH_QUALITY_CUBIC` to a CPU-side filter. The
displayed size is unchanged, but the pixels are not bit-identical, and "animations must look
polished" is the owner's call, not ours. Options to put to him:

1. pre-shrink to 88 px on the CPU, keep D2D cubic for the last 2× (best quality per byte);
2. pre-shrink straight to 44 px (simplest, fewest moving parts);
3. leave it alone and instead correct the README: 17.5 MB is stale, the floor is ~30 MB.

Whichever is chosen, re-measure with `tools/memprobe.exe` and a fresh `halo.exe` before and
after, and note that the README's single number should become a range either way.

## Not established here

Which specific regions shrink during the 4.8 MB fluctuation, and whether the 9.5 MB
per-refresh figure ever appears in real use (it requires a refresh that rebuilds surfaces
for paths not already in `m_brushes`). Both are worth a look once the fix lands.
