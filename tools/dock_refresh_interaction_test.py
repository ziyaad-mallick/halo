"""Execute RefreshDock's actual prefix against reentrant capture stubs."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
source = (root / 'src/halo.cpp').read_text(encoding='utf-8')
prefix = source.split('void Halo::RefreshDock() {', 1)[1].split('auto running = m_tracker->Apps();', 1)[0]
code = r'''
#include <cstdio>
using HWND=int;
constexpr int HWND_TOPMOST=1, SWP_NOMOVE=1, SWP_NOSIZE=2, SWP_NOACTIVATE=4;
constexpr int kPreviewTimer=21, kPreviewGrace=22;
struct Dock { enum class Kind { None,MediaPlay }; int pressed=2; void SetPressed(int n){pressed=n;} };
struct DockWidgets { enum class Part { None,Play }; Part pressed=Part::Play; void SetPressed(Part p){pressed=p;} };
struct Panel { bool hidden=false; void Raise(){} void Hide(){hidden=true;} };
struct Halo; Halo* host;
HWND capture=7;
bool previewTimer=true, graceTimer=true, reentrantSafe=true;
void SetWindowPos(HWND,int,int,int,int,int,int){}
void KillTimer(HWND,int timer){if(timer==kPreviewTimer)previewTimer=false; if(timer==kPreviewGrace)graceTimer=false;}
HWND GetCapture(){return capture;}
void ReleaseCapture();
struct Halo {
 HWND m_hwnd=7; Dock dock; Dock* m_dock=&dock;
 DockWidgets widgets; DockWidgets* m_widgets=&widgets;
 Panel launcher,menu,label; Panel* m_launcher=&launcher; Panel* m_menu=&menu; Panel* m_label=&label;
 int m_hoverItem=2,m_pressed=2; bool m_pressedStart=true,previewsHidden=false;
 Dock::Kind m_pressedWidget=Dock::Kind::MediaPlay;
 void HidePreviews(bool){previewsHidden=true;} void RefreshDock();
};
void ReleaseCapture(){
 reentrantSafe=host->m_pressed==-1 && !host->m_pressedStart && host->m_pressedWidget==Dock::Kind::None;
 capture=0;
}
void Halo::RefreshDock(){
''' + prefix + r'''
}
int main(){
 Halo h; host=&h; h.RefreshDock();
 bool safe=h.m_hoverItem==-1 && h.m_pressed==-1 && !h.m_pressedStart && h.m_pressedWidget==Dock::Kind::None &&
 h.dock.pressed==-1 && h.widgets.pressed==DockWidgets::Part::None && !capture && !previewTimer && !graceTimer &&
 h.previewsHidden && h.label.hidden && reentrantSafe;
 std::printf("refresh cancels interactions before capture reentry: %s\n",safe?"PASS":"FAIL");
 if(!safe)return 1;
 capture=99; h.RefreshDock(); bool other=capture==99;
 std::printf("foreign capture preserved: %s\n",other?"PASS":"FAIL"); return other?0:1;
}
'''
out=root / 'build/dock-refresh-test'
out.mkdir(parents=True,exist_ok=True)
(out / 'fixture.cpp').write_text(code,encoding='utf-8')
exe=out / 'fixture.exe'
subprocess.run([str(root / 'toolchain/llvm-mingw/bin/clang++.exe'), '-std=c++23','-O2','-static',str(out / 'fixture.cpp'),'-o',str(exe)],check=True)
raise SystemExit(subprocess.run([str(exe)]).returncode)
