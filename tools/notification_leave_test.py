"""Run the production pointer-dismiss and placement methods with deterministic inputs."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
src = (subprocess.check_output(['git', 'show', 'HEAD:src/notifications.cpp'], cwd=root).decode('utf-8')
       if '--baseline' in sys.argv else (root / 'src/notifications.cpp').read_text(encoding='utf-8'))
if 'void NotificationCenter::Impl::CheckPointer()' not in src:
    print('FAIL: notification panel has no pointer-leave dismissal handler')
    raise SystemExit(1)

def method(name):
    begin = src.index('void NotificationCenter::Impl::' + name + '()')
    end = src.index('\n}\n', begin) + 2
    return src[begin:end].replace('NotificationCenter::Impl::', '')

code = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
using ULONGLONG = unsigned long long;
struct POINT { long x, y; };
struct RECT { long left, top, right, bottom; };
POINT cursor{100,100}; RECT window{600,400,960,700}; ULONGLONG tick=100; bool held=false;
constexpr int VK_LBUTTON=1, HWND_TOPMOST=1, SWP_NOACTIVATE=1;
int GetAsyncKeyState(int) { return held ? 0x8000 : 0; }
bool GetCursorPos(POINT* p) { *p=cursor; return true; }
bool GetWindowRect(int,RECT* r) { *r=window; return true; }
bool PtInRect(RECT* r, POINT p) { return p.x>=r->left && p.x<r->right && p.y>=r->top && p.y<r->bottom; }
void InflateRect(RECT* r,int x,int y) {r->left-=x;r->right+=x;r->top-=y;r->bottom+=y;}
ULONGLONG GetTickCount64() { return tick; }
void SetWindowPos(int,int,int x,int y,int w,int h,int) { window={x,y,x+w,y+h}; }
namespace ui { constexpr float kS4=16, kS6=24; }
namespace motion { constexpr float kPad=16; }
namespace settings {
struct Config { struct {bool closeOnLeave=true;} cc; struct {int hideDelay=500, iconSize=48, magnification=150;} taskbar; bool halo2=true, reduceMotion=false; } cfg;
Config& Get() {return cfg;}
}
constexpr float kEdge=16, kAttachGap=12;
struct Fixture {
 enum class St { Open, Closed }; St st=St::Open;
 bool attached=false, byPointer=true, entered=false;
 ULONGLONG outSince=0; float s=1; RECT work{0,0,1000,768}, anchorRc{}; int monitorBottom=768;
 int hwnd=1, closed=0, m_width=1000; float totalH=300, winH=0, availH=640,kW=360;
 std::vector<int> dying;
 void Dismissed() {++closed; st=St::Closed;}
''' + src[src.index('    float DockClearance() const {'):src.index('\n    void BuildFonts();')].strip() + '\n' + method('CheckPointer') + '\n' + method('Place') + r'''
};
int main() {
 Fixture edge; edge.CheckPointer(); tick=599; edge.CheckPointer(); assert(!edge.closed);
 tick=600; edge.CheckPointer(); assert(edge.closed==1);
 Fixture keyboard; keyboard.byPointer=false; keyboard.CheckPointer(); tick=5000; keyboard.CheckPointer(); assert(!keyboard.closed);
 cursor={700,500}; keyboard.CheckPointer(); assert(keyboard.entered);
 cursor={100,100}; keyboard.CheckPointer(); tick+=500; keyboard.CheckPointer(); assert(keyboard.closed==1);
 Fixture crossing; cursor={995,500}; crossing.CheckPointer(); tick+=2000; crossing.CheckPointer(); assert(!crossing.closed);
 cursor={700,500}; crossing.CheckPointer(); cursor={100,100}; crossing.CheckPointer();
 tick+=300; cursor={700,500}; crossing.CheckPointer(); assert(crossing.outSince==0);
 Fixture drag; held=true; cursor={100,100}; drag.CheckPointer(); tick+=2000; drag.CheckPointer(); assert(!drag.closed); held=false;
 for(float scale : {1.f,1.25f,1.5f,2.f}) for(long height : {768,1080,1440}) {
  Fixture f; f.s=scale; f.work.bottom=height; f.monitorBottom=height; f.totalH=std::min(400.f,height/scale-128); f.availH=f.totalH; f.Place();
  const float bottom=window.top+motion::kPad*scale+f.totalH*scale;
  assert(std::abs(bottom-(height-(kEdge+f.DockClearance())*scale))<2);
  assert(f.DockClearance() > 60 && f.DockClearance() < 62);
  assert(window.top+motion::kPad*scale>=0);
 }
 Fixture reserved; reserved.monitorBottom=768; reserved.work.bottom=700;
 assert(reserved.DockClearance()==0);
 reserved.Place(); assert(std::abs((window.top+motion::kPad+reserved.totalH)-(700-kEdge))<2);
 settings::cfg.taskbar.iconSize=64; settings::cfg.taskbar.magnification=200;
 Fixture large; assert(large.DockClearance()>90);
 settings::cfg.reduceMotion=true; assert(large.DockClearance()<70);
 settings::cfg.halo2=false; Fixture classic; classic.Place(); assert(window.top+motion::kPad==kEdge);
 Fixture attached; attached.attached=true; attached.anchorRc={400,600,760,750}; attached.Place();
 assert(std::abs((window.top+motion::kPad+attached.totalH)-(attached.anchorRc.top-kAttachGap))<2);
 puts("notification leave/placement: all checks passed");
}
'''
out = root / 'build/notification-leave-test'
out.mkdir(parents=True, exist_ok=True)
(out / 'fixture.cpp').write_text(code, encoding='utf-8')
compiler = root / 'toolchain/audit-download/llvm-mingw-20250709-ucrt-x86_64/bin/clang++.exe'
exe = out / 'fixture.exe'
subprocess.run([str(compiler), '-std=c++23', '-O2', '-static', str(out / 'fixture.cpp'), '-o', str(exe)], check=True)
raise SystemExit(subprocess.run([str(exe)]).returncode)
