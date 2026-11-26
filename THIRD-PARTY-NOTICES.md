# Third-party notices

Halo is released under the MIT licence (see [LICENSE](LICENSE)). The component
below contains code derived from third-party work and is reproduced under that
work's licence.

## MacOS Minimize Animation (Windhawk mod)

- **Component:** MacOS Minimize Animation, a Windhawk mod (genie minimize and
  restore animation).
- **Version forked:** 3.1.6 (the `@version` in the upstream file at the commit
  below; Halo's own header also names v3.1.6).
- **Upstream file:** <https://github.com/ramensoftware/windhawk-mods/blob/1cefd9b6c5400916d74378ed90d711adc699f69b/mods/macos-minimize-animation.wh.cpp>
  (`mods/macos-minimize-animation.wh.cpp` in `ramensoftware/windhawk-mods`,
  commit `1cefd9b6c5400916d74378ed90d711adc699f69b`, "MacOS Minimize Animation
  v3.1.6: revert hardware D2D render target (#5909)", 2026-10-05).
- **Declared licence:** MIT (`@license MIT` in the upstream mod header).
- **Authors, as credited upstream:**
  - Abdullah Masood (`@author Abdullah Masood`,
    <https://github.com/Abdullah-Masood-05>).
  - Potassiumuncher (<https://github.com/Potassiumuncher>), credited in the
    upstream readme and source comments as the contributor of the Direct2D
    "modern" genie engine and the v3.1.1 capture and renderer fixes.
- **Halo files that contain derived code or cite this work:**
  - `mods/halo-genie.wh.cpp` (a fork of the mod).
  - `src/lamp_geometry.h` (lamp curve adapted from the mod).
  - `src/swoop.cpp` and `src/swoop_compositor.cpp` include `lamp_geometry.h`
    and use its curve, but contain no comment citing the upstream mod.

### Licence note

The upstream mod declares MIT through the `@license MIT` field of its Windhawk
header, naming Abdullah Masood as author. It contains no separate copyright
line or licence text (source: the upstream file linked above). Potassiumuncher
is credited as a contributor, with no copyright or licence statement of their
own in that file, so the copyright line below names both as credited. Their
public repository linked above carries no licence file.

### Licence text

MIT License

Copyright (c) Abdullah Masood and Potassiumuncher

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
