"""Execute SetItems' unchanged-geometry path with synthetic visual objects."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
src = (root / 'src/dock.cpp').read_text(encoding='utf-8')
begin = src.index('void Dock::SetItems(std::vector<Item> const& items) {')
begin = src.index('\n', begin)
end = src.index('    const bool first =', begin)
prefix = src[begin:end]
code = r'''
#include <algorithm>
#include <vector>
#include <string>
#include <cstdio>
constexpr int kFadeMs=100;
struct Visual {};
struct Item { std::wstring key; int brush; bool running,extra; };
struct Entry { std::wstring key; int brush; bool running,extra,leaving; Visual dot; };
struct Fixture {
 std::vector<Entry> m_entries{{L"a",1,false,false,false,{}}};
 int rebuilds=0, fades=0, settles=0;
 float DotOpacity() { return .8f; }
 void AnimateTo(Visual&,const wchar_t*,float,int) { ++fades; }
 void Settle(Entry&) { ++settles; }
 void Update(std::vector<Item> const& items) {
''' + prefix + r'''
 ++rebuilds;
 }
};
int main() {
 int fail=0;
 Fixture same; same.Update({{L"a",1,false,false}});
 if(same.rebuilds || same.fades) ++fail;
 Fixture running; running.Update({{L"a",1,true,false}});
 if(running.rebuilds || !running.m_entries[0].running || running.fades!=1 || running.settles!=1) ++fail;
 running.Update({{L"a",1,false,false}});
 if(running.rebuilds || running.m_entries[0].running || running.fades!=2 || running.settles!=1) ++fail;
 Fixture changed; changed.Update({{L"b",1,false,false}}); if(changed.rebuilds!=1) ++fail;
 Fixture brush; brush.Update({{L"a",2,false,false}}); if(brush.rebuilds!=1) ++fail;
 Fixture extra; extra.Update({{L"a",1,false,true}}); if(extra.rebuilds!=1) ++fail;
 Fixture leaving; leaving.m_entries[0].leaving=true;
 leaving.Update({{L"a",1,false,false}}); if(leaving.rebuilds!=1) ++fail;
 Fixture count; count.Update({}); if(count.rebuilds!=1) ++fail;
 std::printf("dock updates: 8 cases, %d failures\n",fail);
 return fail ? 1 : 0;
}
'''
out = root / 'build/dock-update-test'
out.mkdir(parents=True, exist_ok=True)
(out / 'fixture.cpp').write_text(code, encoding='utf-8')
compiler = root / 'toolchain/llvm-mingw/bin/clang++.exe'
exe = out / 'fixture.exe'
subprocess.run([str(compiler), '-std=c++23', '-O2', '-static', str(out / 'fixture.cpp'), '-o', str(exe)], check=True)
raise SystemExit(subprocess.run([str(exe)]).returncode)
