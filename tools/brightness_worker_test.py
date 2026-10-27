"""Run the actual brightness queue against a blocked/failing synthetic WMI writer."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
src = (subprocess.check_output(['git', 'show', 'HEAD:src/services.cpp'], cwd=root).decode('utf-8')
       if '--baseline' in sys.argv else (root / 'src/services.cpp').read_text(encoding='utf-8'))
begin = src.index('namespace brightness {')
bridge = src.index('winrt::com_ptr<IWbemServices> Connect()', begin)
end_bridge = src.index('}  // namespace\n\nvoid Probe', bridge)
end = src.index('}  // namespace brightness', end_bridge) + len('}  // namespace brightness')
code = r'''
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>
using HWND=int; using UINT=unsigned;
constexpr int COINIT_MULTITHREADED=0;
std::atomic<int> active=0, peak=0, attempts=0, successful=-1;
std::atomic<bool> blocked=true, fail=false;
int CoInitializeEx(void*,int) {
 int n=++active, prev=peak; while(n>prev && !peak.compare_exchange_weak(prev,n)) {} return 0;
}
void CoUninitialize(){--active;}
bool SUCCEEDED(int n){return n>=0;} bool FAILED(int n){return n<0;}
bool DryRun(){return false;} void Log(std::wstring const&){}
void PostMessageW(int,unsigned,int,int){}
bool Wait(auto pred) {
 auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
 while(std::chrono::steady_clock::now()<end){if(pred())return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}return false;
}
'''
code += src[begin:bridge]
code += r'''
''' + ('void Read(State&) {}\n' if 'struct State {' in src[begin:bridge] else 'void Read() {}\n') + r'''
bool Write(int pct) {
 ++attempts;
 while(blocked) std::this_thread::sleep_for(std::chrono::milliseconds(1));
 if(fail)return false;
 successful=pct; return true;
}
'''
code += src[end_bridge:end]
code += r'''
int main() {
 using namespace brightness; int failures=0;
 Set(1); if(!Wait([]{return attempts>0;})) return 2;
 for(int i=2;i<=100;i++)Set(i);
 std::this_thread::sleep_for(std::chrono::milliseconds(100));
 if(peak!=1){std::printf("FAIL worker bound: peak %d\n",int(peak));++failures;}
 blocked=false;
 if(!Wait([]{return active==0 && successful==100;})){std::puts("FAIL latest target");++failures;}
 if(attempts>2){std::puts("FAIL intermediate targets not coalesced");++failures;}
 int before=attempts; fail=true; Set(73);
 if(!Wait([&]{return attempts>before && active==0;}))return 3;
 before=attempts; fail=false; Set(73);
 if(!Wait([&]{return active==0 && successful==73;})){std::puts("FAIL retry failed same value");++failures;}
 if(attempts==before){std::puts("FAIL failed target cached as applied");++failures;}
 Set(999); if(!Wait([]{return active==0 && successful==100;})){std::puts("FAIL clamp");++failures;}
 std::printf("brightness queue: %d failures, peak %d worker(s)\n",failures,int(peak));
 return failures?1:0;
}
'''
out = root / 'build/brightness-worker-test'
out.mkdir(parents=True, exist_ok=True)
(out / 'fixture.cpp').write_text(code, encoding='utf-8')
compiler = root / 'toolchain/audit-download/llvm-mingw-20250709-ucrt-x86_64/bin/clang++.exe'
exe = out / ('baseline.exe' if '--baseline' in sys.argv else 'fixture.exe')
subprocess.run([str(compiler), '-std=c++23', '-O2', '-static', str(out / 'fixture.cpp'), '-o', str(exe)], check=True)
raise SystemExit(subprocess.run([str(exe)]).returncode)
