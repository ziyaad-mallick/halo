"""Execute the actual media mouse-up handler with synchronous capture loss."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
src = (root / 'src/halo.cpp').read_text(encoding='utf-8')
begin = src.index('        case WM_LBUTTONUP:')
begin = src.index('            ', src.index('OpenLauncher();', begin) + len('OpenLauncher();'))
end = src.index('            if (m_pressed >= 0 && m_dock)', begin)
handler = src[begin:end]
out = root / 'build/media-capture-test'
out.mkdir(parents=True, exist_ok=True)
code = r'''
#include <utility>
#include <cstdio>
constexpr int WM_LBUTTONUP=1, WM_CAPTURECHANGED=2;
struct DockWidgets { enum class Part {None, Play, Next}; void SetPressed(Part) {} };
struct Dock {
 enum class Kind {None, Media, MediaPlay, MediaNext};
 struct Target {Kind kind;};
 Kind hit=Kind::MediaPlay;
 Target HitTarget(float,float) { return {hit}; }
};
DockWidgets::Part MediaPart(Dock::Kind k) { return k==Dock::Kind::None ? DockWidgets::Part::None : DockWidgets::Part::Play; }
struct Media {
 int play=0, next=0;
 struct StateType { bool has=true, canNext=true; } state;
 StateType State() { return state; }
 void PlayPause() { ++play; }
 void Next() { ++next; }
};
struct Fixture {
 DockWidgets widgets; Dock dock;
 DockWidgets* m_widgets=&widgets; Dock* m_dock=&dock;
 Dock::Kind m_pressedWidget=Dock::Kind::MediaPlay;
 int m_pressed=-1, opened=0; float mx=0,my=0;
 void OpenMediaSource() { ++opened; }
 Media m_dockMedia;
 void ReleaseCapture() { Up(WM_CAPTURECHANGED); }
 void Up(int m) {
''' + handler + r'''
 }
};
int main() {
 int failures=0;
 for (auto kind : {Dock::Kind::Media, Dock::Kind::MediaPlay, Dock::Kind::MediaNext}) {
  Fixture f; f.dock.hit=kind; f.m_pressedWidget=kind;
  f.Up(WM_LBUTTONUP);
  if(f.m_dockMedia.play+f.m_dockMedia.next+f.opened != 1) ++failures;
  Fixture outside; outside.m_pressedWidget=kind; outside.dock.hit=Dock::Kind::None;
  outside.Up(WM_LBUTTONUP);
  if(outside.m_dockMedia.play+outside.m_dockMedia.next+outside.opened) ++failures;
  Fixture cancelled; cancelled.Up(WM_CAPTURECHANGED);
  if(cancelled.m_dockMedia.play) ++failures;
 }
 std::printf("media capture: %d failures\n",failures);
 return failures ? 1 : 0;
}
'''
(out / 'fixture.cpp').write_text(code, encoding='utf-8')
compiler = root / 'toolchain/audit-download/llvm-mingw-20250709-ucrt-x86_64/bin/clang++.exe'
exe = out / 'fixture.exe'
subprocess.run([str(compiler), '-std=c++23', '-O2', '-static', str(out / 'fixture.cpp'), '-o', str(exe)], check=True)
raise SystemExit(subprocess.run([str(exe)]).returncode)
