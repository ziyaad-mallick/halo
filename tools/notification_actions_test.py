"""Execute actual notification actions against failing Windows API stubs."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
source = (root / 'src/notifications.cpp').read_text(encoding='utf-8')
handlers = source[source.index('void NotificationCenter::Impl::Dismiss(Row const& r) {'):source.index('void NotificationCenter::Impl::BuildFonts()')]
code = r'''
#include <cstdio>
#include <cstdint>
#include <set>
#include <string>
#include <vector>
using UINT=unsigned; using INT_PTR=intptr_t;
constexpr int SW_SHOWNORMAL=1;
namespace winrt { struct hresult_error { std::wstring message() const {return L"synthetic failure";} }; }
namespace svc { void Log(std::wstring const&) {} }
std::set<UINT> rejected;
bool clearFails=false;
int launchResult=33, removes=0;
void* ShellExecuteW(void*,const wchar_t*,const wchar_t*,void*,void*,int) { return reinterpret_cast<void*>(launchResult); }
namespace wun::Management {
struct UserNotificationListener {
 static UserNotificationListener Current() {return {};}
 void RemoveNotification(UINT id) {++removes; if(rejected.count(id)) throw winrt::hresult_error{};}
 void ClearNotifications() {if(clearFails) throw winrt::hresult_error{};}
};
}
struct Notif { UINT id=1; std::wstring aumid=L"synthetic"; };
struct Row { std::vector<UINT> ids{1}; Notif n; };
struct Fixture {
 bool dry=false; std::set<UINT> hidden; int hoverCard=0;
 std::vector<Notif> all{{1,L"a"},{2,L"b"}};
 void Update(bool) {}
 void Dismiss(Row const&); void ClearAll(); void Activate(Row const&);
};
''' + handlers.replace('NotificationCenter::Impl::', 'Fixture::') + r'''
int main() {
 int failures=0;
 auto check=[&](bool ok,const char* name){ std::printf("%s %s\n",ok?"PASS":"FAIL",name); failures+=!ok; };
 rejected={1}; Fixture failed; failed.Dismiss(Row{}); check(failed.hidden.empty(),"failed removal preserved");
 rejected.clear(); Fixture success; success.Dismiss(Row{}); check(success.hidden==std::set<UINT>{1},"successful removal hidden");
 rejected={1}; Fixture partial; Row group; group.ids={1,2}; partial.Dismiss(group); check(partial.hidden==std::set<UINT>{2},"partial group preserves failed item");
 rejected.clear(); clearFails=true; Fixture clear; clear.hidden={9}; clear.ClearAll(); check(clear.hidden==std::set<UINT>{9},"failed clear preserved");
 clearFails=false; Fixture cleared; cleared.ClearAll(); check(cleared.hidden==std::set<UINT>{1,2},"successful clear hidden");
 launchResult=31; Fixture launch; int before=removes; launch.Activate(Row{}); check(launch.hidden.empty() && removes==before,"failed launch preserved");
 launchResult=33; Fixture missing; Row noApp; noApp.n.aumid.clear(); before=removes; missing.Activate(noApp); check(missing.hidden.empty() && removes==before,"missing app preserved");
 Fixture opened; opened.Activate(Row{}); check(opened.hidden==std::set<UINT>{1},"successful launch dismissed");
 Fixture dry; dry.dry=true; dry.Dismiss(Row{}); check(dry.hidden==std::set<UINT>{1},"synthetic dry removal retained");
 std::printf("notification actions: 9 cases, %d failures\n",failures); return failures?1:0;
}
'''
out = root / 'build/notification-actions-test'
out.mkdir(parents=True, exist_ok=True)
(out / 'fixture.cpp').write_text(code, encoding='utf-8')
exe = out / 'fixture.exe'
subprocess.run([str(root / 'toolchain/llvm-mingw/bin/clang++.exe'), '-std=c++23', '-O2', '-static', str(out / 'fixture.cpp'), '-o', str(exe)], check=True)
raise SystemExit(subprocess.run([str(exe)]).returncode)
