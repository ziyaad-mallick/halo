#include "pins.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>

namespace halo::pins {
namespace {

void Log(std::wstring const& msg) {
    auto line = L"[halo] pins: " + msg + L"\n";
    OutputDebugStringW(line.c_str());
    fputws(line.c_str(), stderr);
    fflush(stderr);
}

std::wstring Utf16(std::string const& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Utf8(std::wstring const& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// The paths in the file, in order; false if there is no file.
bool ReadPaths(std::vector<std::wstring>& out) {
    out.clear();
    HANDLE f = CreateFileW(FilePath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(f, nullptr);
    if (size == INVALID_FILE_SIZE || size > (1u << 20)) {  // not a pin list (a few KB at most): don't allocate for it
        CloseHandle(f);
        Log(L"ignoring " + FilePath() + L": unreadable or implausibly large");
        return false;
    }
    std::string data(size, '\0');
    DWORD got = 0;
    ReadFile(f, data.data(), static_cast<DWORD>(data.size()), &got, nullptr);
    CloseHandle(f);
    data.resize(got);
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF && static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF)
        data.erase(0, 3);
    size_t at = 0;
    while (at < data.size()) {
        size_t end = data.find('\n', at);
        if (end == std::string::npos) end = data.size();
        std::string line = data.substr(at, end - at);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) out.push_back(Utf16(line));
        at = end + 1;
    }
    return true;
}

// Writes next to the file, then swaps it in, so a crash never leaves half a list.
bool WritePaths(std::vector<std::wstring> const& paths) {
    std::wstring file = FilePath();
    auto slash = file.find_last_of(L"\\/");
    if (slash != std::wstring::npos) CreateDirectoryW(file.substr(0, slash).c_str(), nullptr);
    std::string data;
    for (auto const& p : paths) data += Utf8(p) + "\n";
    std::wstring tmp = file + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &put, nullptr) && put == data.size();
    CloseHandle(f);
    if (ok) ok = MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

}  // namespace

std::wstring FilePath() {
    wchar_t over[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"HALO_PINS_FILE", over, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return over;
    wchar_t dir[MAX_PATH];
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::wstring(dir) + L"\\Halo\\pins.txt";
}

std::vector<PinnedApp> Load() {
    std::vector<std::wstring> paths;
    if (FilePath().empty()) return LoadPinnedApps();
    if (!ReadPaths(paths)) {
        // First run: copy the taskbar's pins once. An empty read (taskbar not ready) seeds nothing, so the next start retries.
        std::vector<PinnedApp> seed = LoadPinnedApps();
        for (auto const& a : seed) paths.push_back(a.linkPath);
        if (!paths.empty()) {
            Log(WritePaths(paths) ? L"seeded " + std::to_wstring(paths.size()) + L" pins from the taskbar into " + FilePath()
                                  : L"could not write " + FilePath());
        }
        return seed;
    }
    std::vector<PinnedApp> apps;
    for (auto const& p : paths) {
        // A path listed twice (hand-edited file) would be two icons with one key.
        if (std::any_of(apps.begin(), apps.end(), [&](PinnedApp const& a) { return !_wcsicmp(a.linkPath.c_str(), p.c_str()); })) continue;
        PinnedApp a;
        if (MakePinnedApp(p, a)) apps.push_back(std::move(a));
    }
    return apps;
}

bool IsPinned(std::wstring const& path) {
    std::vector<std::wstring> paths;
    ReadPaths(paths);
    for (auto const& p : paths)
        if (!_wcsicmp(p.c_str(), path.c_str())) return true;
    return false;
}

bool Pin(std::wstring const& path) {
    if (path.empty()) return false;
    std::vector<std::wstring> paths;
    ReadPaths(paths);
    for (auto const& p : paths)
        if (!_wcsicmp(p.c_str(), path.c_str())) return false;
    paths.push_back(path);
    bool ok = WritePaths(paths);
    Log(std::wstring(ok ? L"pinned " : L"FAILED to pin ") + path);
    return ok;
}

bool Unpin(std::wstring const& path) {
    std::vector<std::wstring> paths;
    ReadPaths(paths);
    bool found = false;
    for (auto it = paths.begin(); it != paths.end();) {
        if (!_wcsicmp(it->c_str(), path.c_str())) {
            it = paths.erase(it);
            found = true;
        } else {
            ++it;
        }
    }
    if (!found) return false;
    bool ok = WritePaths(paths);
    Log(std::wstring(ok ? L"unpinned " : L"FAILED to unpin ") + path);
    return ok;
}

}  // namespace halo::pins
