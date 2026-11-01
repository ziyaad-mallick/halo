"""Exercise the production Win-key tracker against missed-release and normal sequences."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
baseline = '--baseline' in sys.argv
header = (subprocess.check_output(['git', 'show', 'HEAD:src/winkey.h'], cwd=root).decode('utf-8')
          if baseline else (root / 'src/winkey.h').read_text(encoding='utf-8'))
tracker = header[header.index('struct Tracker {'):header.index('\n};', header.index('struct Tracker {')) + 3]
code = r'''
#include <cassert>
#include <cstdio>
using DWORD = unsigned long;
constexpr DWORD VK_LWIN = 0x5b, VK_RWIN = 0x5c;
''' + tracker + (r'''
int main() {
    Tracker t;
    t.Feed(VK_LWIN, true); // release missed while switching session
    bool physicalWin = false;
    bool falseShortcut = t.winDown || physicalWin;
    t.Feed('A', true);
    t.Feed(VK_LWIN, true);
    bool loneTap = t.Feed(VK_LWIN, false);
    std::printf("baseline: A treated as Win+A=%d, next Win tap recognized=%d\n", falseShortcut, loneTap);
    return falseShortcut && !loneTap ? 0 : 1;
}
''' if baseline else r'''
int main() {
    Tracker t;
    assert(!t.Feed(VK_LWIN, true));
    t.Reconcile(false); // release was missed; physical Win is no longer held
    assert(!t.winDown);
    assert(!t.Feed('A', true));
    assert(!t.Feed(VK_LWIN, true));
    assert(t.Feed(VK_LWIN, false));
    assert(!t.Feed(VK_LWIN, true));
    t.Reconcile(true);
    assert(!t.Feed('A', true));
    assert(!t.Feed(VK_LWIN, false));
    assert(!t.Feed(VK_RWIN, true));
    assert(t.Feed(VK_RWIN, false));
    std::puts("Win-key tracker: missed release and normal tap/combo passed");
}
''')
with tempfile.TemporaryDirectory(dir=root / 'build') as d:
    src = Path(d) / 'test.cpp'
    exe = Path(d) / 'test.exe'
    src.write_text(code, encoding='utf-8')
    compiler = root / 'toolchain/llvm-mingw/bin/clang++.exe'
    subprocess.run([str(compiler), '-std=c++23', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
