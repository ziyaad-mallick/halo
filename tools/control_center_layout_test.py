"""Extract and execute Halo's actual Layout() in a headless C++ fixture.

Run: python tools/control_center_layout_test.py [--revision HEAD]
No shell windows, radio changes, or personal files are involved.
Synthetic work area is display height minus 40 physical pixels for the taskbar.
Exit 1 means at least one required action is outside the clipped panel.
"""
from pathlib import Path
import re
import subprocess
import argparse

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--revision')
args = parser.parse_args()
src = (subprocess.check_output(['git', 'show', f'{args.revision}:src/control_center.cpp'], cwd=root).decode('utf-8')
       if args.revision else (root / 'src/control_center.cpp').read_text(encoding='utf-8'))
start = src.index('void ControlCenter::Impl::Layout() {')
end = src.index('\n// The panel follows', start)
layout = src[start:end].replace('ControlCenter::Impl::Layout()', 'Fixture::Layout()')
ids = sorted(set(re.findall(r'Id::(\w+)', layout)) | {'DeviceListTile'})
constants = '\n'.join(line for line in src.splitlines()
                      if line.startswith('constexpr float kW =')
                      or line.startswith('constexpr float kHeaderH =')
                      or line.startswith('constexpr float kMaxH ='))
maxh = re.search(r'float MaxH\(\) const \{[^\n]+', src).group(0)
dense = re.search(r'bool Dense\(\) const \{[^\n]+', src).group(0)
out = root / 'build/control-center-layout-test'
out.mkdir(parents=True, exist_ok=True)
code = r'''
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>
#include "../../src/tokens.h"
namespace ui = halo::ui;
struct D2D1_RECT_F { float left, top, right, bottom; };
enum class Expand { None, Wifi, Bluetooth };
namespace svc {
std::vector<int> wifi(8), bt(6);
auto const& WifiNets() { return wifi; }
auto const& BtDevices() { return bt; }
}
'''
code += 'enum class Id {' + ','.join(ids) + '};\n' + constants
code += r'''
struct Fixture {
    struct Item { Id id; D2D1_RECT_F r; int arg; float rad; };
    std::vector<Item> items;
    std::vector<int> outs;
    std::vector<int> wifiRows=svc::wifi, btRows=svc::bt;
    size_t outOffset=0, outVisible=0;
    size_t listOffset=0, listVisible=0;
    struct { float top=0, bottom=768; } mon;
    float s=1, bright=50, height=0;
    bool outsOpen=false, powerOpen=false;
    size_t listCap=SIZE_MAX;
    Expand expand=Expand::None;
    struct Config { bool showSliders=true, showMedia=true; } config;
    Config const& cfg() const { return config; }
    void Layout();
'''
code += maxh + '\n' + dense + '\n};\n' + layout
code += r'''
int main() {
    int failures=0;
    for (int pixels : {768,1080}) for (float scale : {1.0f,1.25f,1.5f}) {
        Fixture f; f.mon.bottom=pixels-40; f.s=scale; f.Layout();
        auto power=std::find_if(f.items.begin(),f.items.end(),[](auto i){return i.id==Id::Power;});
        bool ok=power->r.bottom<=f.height;
        printf("%s: height=%d scale=%.2f panel=%.1f action-bottom=%.1f\n",ok?"PASS":"FAIL",pixels,scale,f.height,power->r.bottom);
        failures+=!ok;
    }
    Fixture expanded; expanded.mon.bottom=1040; expanded.s=1.5f;
    expanded.outsOpen=true; expanded.outs.resize(6); expanded.Layout();
    auto power=std::find_if(expanded.items.begin(),expanded.items.end(),[](auto i){return i.id==Id::Power;});
    bool ok=power->r.bottom<=expanded.height; failures+=!ok;
    printf("%s: six outputs at 1080/150%% panel=%.1f action-bottom=%.1f\n",ok?"PASS":"FAIL",expanded.height,power->r.bottom);
    Fixture shortExpanded; shortExpanded.mon.bottom=728; shortExpanded.s=1.5f;
    shortExpanded.outsOpen=true; shortExpanded.outs.resize(6); shortExpanded.Layout();
    auto shortPower=std::find_if(shortExpanded.items.begin(),shortExpanded.items.end(),[](auto i){return i.id==Id::Power;});
    auto rows=std::count_if(shortExpanded.items.begin(),shortExpanded.items.end(),[](auto i){return i.id==Id::OutDev;});
    bool shortOk=shortPower->r.bottom<=shortExpanded.height && rows>0 && rows<=shortExpanded.outs.size();
    failures+=!shortOk;
    printf("%s: six outputs at 768/150%% panel=%.1f action-bottom=%.1f visible-rows=%lld\n",shortOk?"PASS":"FAIL",shortExpanded.height,shortPower->r.bottom,static_cast<long long>(rows));
    shortExpanded.outs.resize(20);
    bool seen[20]{};
    for (size_t offset=0; offset<20; ++offset) {
        shortExpanded.outOffset=offset; shortExpanded.Layout();
        auto tile=std::find_if(shortExpanded.items.begin(),shortExpanded.items.end(),[](auto i){return i.id==Id::DeviceListTile;});
        if (tile==shortExpanded.items.end())
            tile=std::find_if(shortExpanded.items.begin(),shortExpanded.items.end(),[](auto i){return i.id==Id::DisplayTile;});
        bool fits=true;
        for (auto const& item : shortExpanded.items) if (item.id==Id::OutDev) {
            fits &= item.r.top>=tile->r.top && item.r.bottom<=tile->r.bottom;
            if (fits) seen[item.arg]=true;
        }
        printf("%s: output scroll offset=%zu stays inside its tile\n",fits?"PASS":"FAIL",offset);
        failures+=!fits;
    }
    bool allSeen=std::all_of(std::begin(seen),std::end(seen),[](bool value){return value;});
    printf("%s: all 20 outputs reachable through scroll positions\n",allSeen?"PASS":"FAIL");
    failures+=!allSeen;
    for (Expand expand : {Expand::Wifi,Expand::Bluetooth}) {
        Fixture f; f.mon.bottom=728; f.s=1.5f; f.expand=expand;
        auto& devices=expand==Expand::Wifi?f.wifiRows:f.btRows;
        devices.resize(24);
        bool seenDevices[24]{};
        for(size_t offset=0;offset<devices.size();++offset) {
            f.listOffset=offset;f.Layout();
            auto tile=std::find_if(f.items.begin(),f.items.end(),[](auto i){return i.id==Id::DeviceListTile;});
            if(tile==f.items.end()) continue;
            for(auto const& item:f.items) if(item.id==(expand==Expand::Wifi?Id::WifiNet:Id::BtDev)) {
                if(item.r.top>=tile->r.top && item.r.bottom<=tile->r.bottom) seenDevices[item.arg]=true;
                else ++failures;
            }
        }
        bool reached=std::all_of(std::begin(seenDevices),std::end(seenDevices),[](bool value){return value;});
        printf("%s: all 24 %s rows reachable\n",reached?"PASS":"FAIL",expand==Expand::Wifi?"Wi-Fi":"Bluetooth");
        failures+=!reached;
    }
    for (int pixels : {768,1080}) for (float scale : {1.0f,1.25f,1.5f}) {
        for (Expand expand : {Expand::None,Expand::Wifi,Expand::Bluetooth}) for (bool powerOpen : {false,true}) {
            Fixture f; f.mon.bottom=pixels-40; f.s=scale; f.expand=expand; f.powerOpen=powerOpen; f.Layout();
            bool fits=true;
            for(auto const& item:f.items) fits &= item.r.top>=0 && item.r.bottom<=f.height && item.r.left>=0 && item.r.right<=kW;
            failures+=!fits;
            if(!fits) printf("FAIL: all controls in bounds height=%d scale=%.2f expand=%d power=%d\n",pixels,scale,int(expand),powerOpen);
        }
    }
    printf("%d reachability failure(s); actual Layout(), synthetic display inputs\n",failures);
    return failures?1:0;
}
'''
cpp = out / 'layout-repro.cpp'
cpp.write_text(code, encoding='utf-8')
compiler = root / 'toolchain/audit-download/llvm-mingw-20250709-ucrt-x86_64/bin/clang++.exe'
exe = out / 'layout-repro.exe'
subprocess.run([str(compiler), '-std=c++23', '-static', str(cpp), '-o', str(exe)], check=True)
try:
    result = subprocess.run([str(exe)], capture_output=True, text=True)
except OSError as error:
    if getattr(error, 'winerror', None) == 4551:
        message = 'BLOCKED: Windows Application Control blocked the unsigned layout fixture (WinError 4551).'
        (out / 'layout-repro.log').write_text(message + '\n', encoding='utf-8')
        print(message)
        raise SystemExit(2)
    raise
(out / 'layout-repro.log').write_text(result.stdout + result.stderr, encoding='utf-8')
print(result.stdout, end='')
raise SystemExit(result.returncode)
