from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[1]
s=(root/'src/control_center.cpp').read_text(encoding='utf-8')
checks={'classic radio settings action': 'case Id::RadioSettings:' in s,'classic radio settings in detail layout':'add(Id::RadioSettings' in s}
for name,ok in checks.items(): print(('PASS ' if ok else 'FAIL ')+name)
if not all(checks.values()): raise SystemExit(1)
subprocess.run(['python',str(root/'tools/control_center_layout_test.py')],check=True,stdout=subprocess.DEVNULL)
out=root/'build/control-center-layout-test'
code=(out/'layout-repro.cpp').read_text(encoding='utf-8').split('int main() {',1)[0]
code+=r'''
int main() {
 int failures=0;
 for (int pixels : {768,1080}) for (float scale : {1.0f,1.25f,1.5f}) for (auto which : {Expand::Wifi,Expand::Bluetooth}) {
  Fixture f; f.mon.bottom=pixels-40; f.s=scale; f.expand=which; f.Layout();
  auto b=std::find_if(f.items.begin(),f.items.end(),[](auto const& i){return i.id==Id::Back;});
  auto a=std::find_if(f.items.begin(),f.items.end(),[](auto const& i){return i.id==Id::RadioSettings;});
  bool ok=b!=f.items.end() && a!=f.items.end() && b->r.right<=a->r.left && a->r.right<=kW && a->r.bottom<=f.height;
  printf("%s: radio settings geometry %d %.2f %d\n",ok?"PASS":"FAIL",pixels,scale,int(which)); failures+=!ok;
 }
 return failures?1:0;
}
'''
cpp=out/'radio-settings-repro.cpp'; cpp.write_text(code,encoding='utf-8')
compiler=root/'toolchain/audit-download/llvm-mingw-20250709-ucrt-x86_64/bin/clang++.exe'
exe=out/'radio-settings-repro.exe'
subprocess.run([str(compiler),'-std=c++23','-static',str(cpp),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
