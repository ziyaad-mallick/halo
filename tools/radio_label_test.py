"""Execute the launcher's actual Wi-Fi/Bluetooth label expression for startup states."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
src = (subprocess.check_output(['git', 'show', 'HEAD:src/launcher.cpp'], cwd=root).decode('utf-8')
       if '--baseline' in sys.argv else (root / 'src/launcher.cpp').read_text(encoding='utf-8'))
start = src.index('std::wstring sub =', src.index('void Launcher::DrawRight'))
end = src.index(';', start) + 1
label = src[start:end]
code = r'''
#include <string>
#include <cstdio>
struct Radio { bool loading=false,failed=false; bool Loading(){return loading;} bool Failed(){return failed;} };
std::wstring Label(bool m_servicesOn, bool pending, bool failed, bool has, bool on) {
 Radio m_radios{pending,failed}; bool wifi=true; std::wstring m_ssid=L"Connected network";
''' + label + r'''
 return sub;
}
int main() {
 int failed=0;
 auto check=[&](bool started,bool pending,bool error,bool has,bool on,const wchar_t* expected) {
  if(Label(started,pending,error,has,on)!=expected) ++failed;
 };
 check(false,false,false,false,false,L"Checking…");
 check(true,true,false,false,false,L"Checking…");
 check(true,false,true,false,false,L"Check Settings");
 check(true,false,false,false,false,L"Unavailable");
 check(true,false,false,true,true,L"Connected network");
 check(true,false,false,true,false,L"Off");
 std::printf("radio startup labels: 6 checks, %d failures\n",failed);
 return failed ? 1 : 0;
}
'''
out = root / 'build/radio-label-test'
out.mkdir(parents=True, exist_ok=True)
(out / 'fixture.cpp').write_text(code, encoding='utf-8')
compiler = root / 'toolchain/audit-download/llvm-mingw-20250709-ucrt-x86_64/bin/clang++.exe'
exe = out / 'fixture.exe'
subprocess.run([str(compiler), '-std=c++23', '-O2', '-static', str(out / 'fixture.cpp'), '-o', str(exe)], check=True)
raise SystemExit(subprocess.run([str(exe)]).returncode)
