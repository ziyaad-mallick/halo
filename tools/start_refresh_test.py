"""Guard the Start input path against synchronous Recent/icon shell work."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
source = (root / 'src/launcher.cpp').read_text(encoding='utf-8')

def body(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[opening:end]

checks = {
    'refresh never calls synchronous Prefetch': 'Prefetch();' not in body('void Launcher::Refresh()'),
    'file brush never loads shell pixels on input thread': 'LoadJumboIcon(' not in body('wuc::CompositionBrush Launcher::FileBrush('),
    'Recent scan has no Launcher member dependency': 'm_allFiles' not in body('std::vector<Launcher::FileEntry> Launcher::LoadFiles()') if 'std::vector<Launcher::FileEntry> Launcher::LoadFiles()' in source else False,
    'worker captures state and cache, never Launcher': 'std::thread([state, icons = m_fileIcons]' in source,
    'worker bounds concurrency': 'if (state->busy) return;' in source,
    'destruction cancels delivery without waiting for shell': 'm_recentRefresh->notify = nullptr;' in body('Launcher::~Launcher()') and '.join(' not in body('Launcher::~Launcher()'),
    'worker owns results instead of message payload': 'state->files = std::move(files);' in source and 'PostMessageW(state->notify, kMsgRecent, 0, 0)' in source,
}
for name, ok in checks.items():
    print(('PASS ' if ok else 'FAIL ') + name)
if not all(checks.values()):
    raise SystemExit(1)

# Execute the production worker-launch method with a blocked synthetic shell scan.
# This verifies bounded concurrency and cancellation without reading Recent or opening UI.
code = r'''
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
using HWND = void*;
constexpr int COINIT_APARTMENTTHREADED=0, kMsgRecent=1;
int CoInitializeEx(void*,int) { return 0; }
bool SUCCEEDED(int n) { return n>=0; }
void CoUninitialize() {}
std::atomic<int> scans=0, posts=0;
std::mutex scanMutex;
std::condition_variable scanCv;
bool released=false;
int PostMessageW(HWND,int,int,int) { ++posts; return 1; }
void Log(std::wstring const&) {}
struct IconImage { int width=0; };
bool LoadJumboIcon(std::wstring const&, IconImage& image) { image.width=32; return true; }
struct Fixture {
    struct FileEntry { std::wstring name,path; };
    struct RecentRefresh {
        std::mutex mutex; HWND notify=nullptr; bool busy=false,ready=false;
        std::vector<FileEntry> files;
        std::map<std::wstring,IconImage> icons;
    };
    std::shared_ptr<RecentRefresh> m_recentRefresh=std::make_shared<RecentRefresh>();
    std::map<std::wstring,IconImage> m_fileIcons;
    HWND m_hwnd=reinterpret_cast<HWND>(1);
    void ApplyRecent() {}
    static std::wstring IconKey(std::wstring const& path) { return path; }
    static std::vector<FileEntry> LoadFiles() {
        ++scans;
        std::unique_lock lock(scanMutex);
        scanCv.wait(lock,[]{return released;});
        return {{L"synthetic", L"synthetic.txt"}};
    }
    void Refresh();
};
''' + 'void Fixture::Refresh() ' + body('void Launcher::Refresh()') + r'''
template<class P> bool Wait(P predicate) {
    auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!predicate()) {
        if(std::chrono::steady_clock::now()>until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}
int main() {
    int failures=0;
    Fixture f;
    f.Refresh();
    if(!Wait([]{return scans==1;})) return 1;
    for(int i=0;i<100;++i) f.Refresh();
    if(scans!=1) ++failures;
    auto abandoned=f.m_recentRefresh;
    { std::lock_guard lock(abandoned->mutex); abandoned->notify=nullptr; }
    { std::lock_guard lock(scanMutex); released=true; }
    scanCv.notify_all();
    if(!Wait([&]{std::lock_guard lock(abandoned->mutex); return !abandoned->busy;})) return 1;
    if(posts!=0 || abandoned->ready) ++failures;
    Fixture normal;
    normal.Refresh();
    if(!Wait([&]{std::lock_guard lock(normal.m_recentRefresh->mutex); return !normal.m_recentRefresh->busy;})) return 1;
    { std::lock_guard lock(normal.m_recentRefresh->mutex);
      if(!normal.m_recentRefresh->ready || normal.m_recentRefresh->files.size()!=1 ||
         normal.m_recentRefresh->icons.size()!=1 || posts!=1) ++failures;
    }
    std::printf("Start refresh worker: %d failures (bounded scan, cancelled delivery, owned completion)\n",failures);
    return failures ? 1:0;
}
'''
out = root / 'build/start-refresh-test'
out.mkdir(parents=True, exist_ok=True)
(out / 'fixture.cpp').write_text(code, encoding='utf-8')
compiler = root / 'toolchain/audit-download/llvm-mingw-20250709-ucrt-x86_64/bin/clang++.exe'
exe = out / 'fixture.exe'
subprocess.run([str(compiler), '-std=c++23', '-O2', '-static', str(out / 'fixture.cpp'), '-o', str(exe)], check=True)
raise SystemExit(subprocess.run([str(exe)]).returncode)
