"""Execute the real notification retirement handler against a compositor-tree fixture."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
src = (subprocess.check_output(['git', 'show', 'HEAD:src/notifications.cpp'], cwd=root).decode('utf-8')
       if '--baseline' in sys.argv else (root / 'src/notifications.cpp').read_text(encoding='utf-8'))
begin = src.index('                KillTimer(h, kTimerGone);')
end = src.index('            } else if (w == kTimerClose)', begin)
handler = src[begin:end]
out = root / 'build/notification-retirement-test'
out.mkdir(parents=True, exist_ok=True)
code = r'''
#include <algorithm>
#include <cstdio>
#include <vector>
struct Card { int outer; };
struct Tree {
 std::vector<int> nodes;
 Tree& Children() { return *this; }
 void Remove(int id) { nodes.erase(std::find(nodes.begin(), nodes.end(), id)); }
};
struct Fixture {
 int h=0, kTimerGone=1; Tree panelVis; std::vector<Card> dying;
 void KillTimer(int,int) {} void Place() {}
 void Retire() {
''' + handler + r'''
 }
};
int main() {
 Fixture f;
 for(int batch=0; batch<100; ++batch) {
  f.panelVis.nodes.push_back(batch); f.dying.push_back({batch}); f.Retire();
 }
 std::printf("notification retirement: %zu retained visuals after 100 dismissals\n", f.panelVis.nodes.size());
 return f.panelVis.nodes.empty() ? 0 : 1;
}
'''
(out / 'fixture.cpp').write_text(code, encoding='utf-8')
compiler = root / 'toolchain/audit-download/llvm-mingw-20250709-ucrt-x86_64/bin/clang++.exe'
exe = out / 'fixture.exe'
subprocess.run([str(compiler), '-std=c++23', '-O2', '-static', str(out / 'fixture.cpp'), '-o', str(exe)], check=True)
raise SystemExit(subprocess.run([str(exe)]).returncode)
