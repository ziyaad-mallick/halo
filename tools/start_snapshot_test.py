"""Execute Start snapshot application without rendering or real shell data."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[1]
source=(root/'src/launcher.cpp').read_text(encoding='utf-8')
begin=source.index('void Launcher::ApplyRecent(bool redraw) {')
end=source.index('\n// ---- Search',begin)
method=source[begin:end].replace('Launcher::ApplyRecent','Fixture::ApplyRecent')
code=r'''
#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
constexpr int kMaxFiles=12;
void Log(std::wstring const&){}
struct IconImage { int width=1,height=1; std::vector<unsigned char> bgra{1}; };
struct FileEntry { std::wstring name,path; };
struct State { std::mutex mutex; bool ready=true; std::vector<FileEntry> files{{L"a",L"a.txt"}}; std::map<std::wstring,IconImage> icons{{L".txt",{}}}; };
struct Fixture {
 bool m_menuOpen=false,m_open=true; int searches=0,layouts=0;
 std::shared_ptr<State> m_recentRefresh=std::make_shared<State>();
 std::vector<FileEntry> m_allFiles{{L"a",L"a.txt"}},m_files=m_allFiles;
 std::map<std::wstring,IconImage> m_fileIcons{{L".txt",{}}};
 std::map<int,int> m_fileBrushes{{1,1}};
 void RunSearch(){++searches;} void Relayout(){++layouts;}
 void ApplyRecent(bool redraw=true);
};
'''+method+r'''
int main(){
 int failures=0;
 Fixture same; same.ApplyRecent(); if(same.layouts || same.searches || same.m_fileBrushes.empty())++failures;
 Fixture icon; icon.m_recentRefresh->icons[L".txt"].bgra={2}; icon.ApplyRecent(); if(icon.layouts!=1 || !icon.m_fileBrushes.empty())++failures;
 Fixture files; files.m_recentRefresh->files={{L"b",L"b.txt"}}; files.ApplyRecent(); if(files.layouts!=1 || files.searches!=1 || files.m_files[0].path!=L"b.txt")++failures;
 Fixture menu; menu.m_menuOpen=true; menu.ApplyRecent(); if(!menu.m_recentRefresh->ready || menu.layouts)++failures;
 Fixture closed; closed.m_open=false; closed.m_recentRefresh->files.clear(); closed.ApplyRecent(); if(closed.layouts)++failures;
 Fixture deferred; deferred.m_recentRefresh->files.clear(); deferred.ApplyRecent(false); if(deferred.layouts || deferred.m_recentRefresh->ready)++failures;
 std::printf("Start snapshots: 6 cases, %d failures\n",failures); return failures?1:0;
}
'''
out=root/'build/start-snapshot-test'
out.mkdir(parents=True,exist_ok=True)
(out/'fixture.cpp').write_text(code,encoding='utf-8')
exe=out/'fixture.exe'
subprocess.run([str(root/'toolchain/llvm-mingw/bin/clang++.exe'),'-std=c++23','-O2','-static',str(out/'fixture.cpp'),'-o',str(exe)],check=True)
raise SystemExit(subprocess.run([str(exe)]).returncode)
