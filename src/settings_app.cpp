// Halo Settings (halo-settings.exe): the dashboard for docs/SETTINGS.md.
//
// A tiny HTTP server on 127.0.0.1 (random port, random 128-bit token in the URL path) that serves one
// embedded page (ui/settings.html) plus a JSON API, shown in an Edge "app mode" window. The settings file is
// read and written only through settings.h; live stats (CPU/RAM/GPU of halo.exe + guardian.exe, GPU of
// dwm.exe) are sampled once a second.
//
// Every request must carry the token and a Host of exactly 127.0.0.1:<port> (DNS-rebinding guard).
// Flags (for testing): --serve            run the server only, no window, never exit on idle
//                      --url-file <path>  where to write the page URL (default %LOCALAPPDATA%\Halo\settings-ui\url.txt)
//                      --register         (re)write the Start-menu shortcut and exit (tools/install.ps1 runs this)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <propvarutil.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "settings.h"

namespace st = halo::settings;

extern "C" BOOLEAN WINAPI SystemFunction036(PVOID, ULONG);  // RtlGenRandom (advapi32)

// The page, embedded at build time.
static const unsigned char kPage[] = {
#embed "../ui/settings.html"
};

namespace {

#ifndef HALO_EXE
#define HALO_EXE L"halo.exe"  // overridable at compile time, only to test the sampler against a busy process
#endif
const char kBuild[] = __DATE__ " " __TIME__;

// ---------------------------------------------------------------- small helpers
std::string Utf8(std::wstring const& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Wide(std::string const& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string JStr(std::string const& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o + "\"";
}
std::string Num(double v) {
    char b[48];
    snprintf(b, sizeof b, "%.2f", v);
    return b;
}
std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
std::string Trim(std::string s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
    return s.substr(i);
}

std::atomic<ULONGLONG> g_lastRequest{0};
std::string g_token;
int g_port = 0;
std::mutex g_settingsMu;

// ---------------------------------------------------------------- minimal JSON (flat object of scalars)
// Accepts {"a":"b","c":12,"d":true,"e":null}; values come back as strings. Returns false on anything else.
bool ParseFlatJson(std::string const& s, std::map<std::string, std::string>& out) {
    size_t i = 0;
    auto ws = [&] { while (i < s.size() && isspace((unsigned char)s[i])) i++; };
    auto str = [&](std::string& r) -> bool {
        if (i >= s.size() || s[i] != '"') return false;
        i++;
        r.clear();
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                char e = s[i + 1];
                i += 2;
                switch (e) {
                    case 'n': r += '\n'; break;
                    case 't': r += '\t'; break;
                    case 'r': r += '\r'; break;
                    case 'u': {
                        if (i + 4 > s.size()) return false;
                        unsigned cp = (unsigned)strtoul(s.substr(i, 4).c_str(), nullptr, 16);
                        i += 4;
                        if (cp < 0x80) r += (char)cp;
                        else if (cp < 0x800) { r += (char)(0xC0 | (cp >> 6)); r += (char)(0x80 | (cp & 0x3F)); }
                        else { r += (char)(0xE0 | (cp >> 12)); r += (char)(0x80 | ((cp >> 6) & 0x3F)); r += (char)(0x80 | (cp & 0x3F)); }
                        break;
                    }
                    default: r += e;
                }
            } else r += s[i++];
        }
        if (i >= s.size()) return false;
        i++;
        return true;
    };
    ws();
    if (i >= s.size() || s[i] != '{') return false;
    i++;
    ws();
    if (i < s.size() && s[i] == '}') return true;
    for (;;) {
        ws();
        std::string k, v;
        if (!str(k)) return false;
        ws();
        if (i >= s.size() || s[i] != ':') return false;
        i++;
        ws();
        if (i < s.size() && s[i] == '"') {
            if (!str(v)) return false;
        } else {
            size_t b = i;
            while (i < s.size() && s[i] != ',' && s[i] != '}' && !isspace((unsigned char)s[i])) i++;
            v = s.substr(b, i - b);
            if (v.empty() || v == "null") v.clear();
            else if (v != "true" && v != "false" && v.find_first_not_of("-0123456789.") != std::string::npos) return false;
        }
        out[k] = v;
        ws();
        if (i < s.size() && s[i] == ',') { i++; continue; }
        if (i < s.size() && s[i] == '}') return true;
        return false;
    }
}

// ---------------------------------------------------------------- live stats
struct Group {
    bool running = false;
    std::vector<DWORD> pids;
    bool cpuOk = false;
    double cpu = 0, priv = 0, ws = 0;
    bool gpuOk = false;
    double gpu = 0;
};
std::mutex g_statsMu;
std::string g_statsJson = "{\"running\":false}";

std::vector<DWORD> PidsByName(const wchar_t* exe) {
    std::vector<DWORD> v;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return v;
    PROCESSENTRY32W pe{sizeof pe};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, exe) == 0) v.push_back(pe.th32ProcessID);
    CloseHandle(snap);
    return v;
}

void StatsThread() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const double cores = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
    LARGE_INTEGER freq, prev{};
    QueryPerformanceFrequency(&freq);
    std::map<DWORD, ULONGLONG> lastCpu;

    PDH_HQUERY q = nullptr;
    PDH_HCOUNTER c = nullptr;
    bool pdh = PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS &&
               PdhAddEnglishCounterW(q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &c) == ERROR_SUCCESS;
    if (pdh) PdhCollectQueryData(q);
    int collects = pdh ? 1 : 0;

    for (;; Sleep(1000)) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double wall = prev.QuadPart ? double(now.QuadPart - prev.QuadPart) / freq.QuadPart : 0;
        prev = now;

        // GPU engines, summed per pid.
        std::map<DWORD, double> gpuByPid;
        bool gpuOk = false;
        if (pdh && PdhCollectQueryData(q) == ERROR_SUCCESS) {
            collects++;
            DWORD size = 0, count = 0;
            if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &size, &count, nullptr) == (PDH_STATUS)PDH_MORE_DATA && size) {
                std::vector<char> buf(size);
                auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
                if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &size, &count, items) == ERROR_SUCCESS) {
                    gpuOk = collects >= 2;
                    for (DWORD i = 0; i < count; i++) {
                        if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
                        const wchar_t* n = items[i].szName;
                        if (wcsncmp(n, L"pid_", 4) != 0) continue;
                        gpuByPid[(DWORD)wcstoul(n + 4, nullptr, 10)] += items[i].FmtValue.doubleValue;
                    }
                }
            }
        }

        std::map<DWORD, ULONGLONG> nowCpu;
        auto measure = [&](const wchar_t* exe) {
            Group g;
            g.pids = PidsByName(exe);
            g.running = !g.pids.empty();
            double cpuSec = 0;
            for (DWORD pid : g.pids) {
                HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (!h) continue;
                FILETIME cr, ex, kr, us;
                if (GetProcessTimes(h, &cr, &ex, &kr, &us)) {
                    ULONGLONG t = ((ULONGLONG)kr.dwHighDateTime << 32 | kr.dwLowDateTime) + ((ULONGLONG)us.dwHighDateTime << 32 | us.dwLowDateTime);
                    nowCpu[pid] = t;
                    auto it = lastCpu.find(pid);
                    if (it != lastCpu.end() && wall > 0) { cpuSec += (t - it->second) * 1e-7; g.cpuOk = true; }
                }
                PROCESS_MEMORY_COUNTERS_EX pmc{sizeof pmc};
                if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof pmc)) {
                    g.priv += pmc.PrivateUsage / 1048576.0;
                    g.ws += pmc.WorkingSetSize / 1048576.0;
                }
                CloseHandle(h);
            }
            if (g.cpuOk) g.cpu = cpuSec / wall / cores * 100.0;
            if (g.running && gpuOk) {
                g.gpuOk = true;
                for (DWORD pid : g.pids) if (auto it = gpuByPid.find(pid); it != gpuByPid.end()) g.gpu += it->second;
            }
            return g;
        };
        Group halo = measure(HALO_EXE), guard = measure(L"guardian.exe"), dwm = measure(L"dwm.exe");
        lastCpu = std::move(nowCpu);

        auto grp = [&](Group const& g) {
            std::string s = "{\"running\":" + std::string(g.running ? "true" : "false") + ",\"pids\":[";
            for (size_t i = 0; i < g.pids.size(); i++) s += (i ? "," : "") + std::to_string(g.pids[i]);
            s += "],\"cpu\":" + (g.cpuOk ? Num(g.cpu) : "null");
            s += ",\"ram_private_mb\":" + (g.running ? Num(g.priv) : "null");
            s += ",\"ram_ws_mb\":" + (g.running ? Num(g.ws) : "null");
            s += ",\"gpu\":" + (g.gpuOk ? Num(g.gpu) : "null") + "}";
            return s;
        };
        // "Halo" = halo.exe + guardian.exe together.
        bool any = halo.running || guard.running;
        std::string total = "{\"running\":" + std::string(any ? "true" : "false");
        total += ",\"cpu\":" + ((halo.cpuOk || guard.cpuOk) ? Num((halo.cpuOk ? halo.cpu : 0) + (guard.cpuOk ? guard.cpu : 0)) : "null");
        total += ",\"ram_private_mb\":" + (any ? Num(halo.priv + guard.priv) : "null");
        total += ",\"ram_ws_mb\":" + (any ? Num(halo.ws + guard.ws) : "null");
        total += ",\"gpu\":" + ((halo.gpuOk || guard.gpuOk) ? Num((halo.gpuOk ? halo.gpu : 0) + (guard.gpuOk ? guard.gpu : 0)) : "null") + "}";

        std::string j = "{\"build\":\"" + std::string(kBuild) + "\",\"running\":" + std::string(halo.running ? "true" : "false") + ",\"cores\":" + std::to_string((int)cores);
        j += ",\"time\":" + std::to_string(GetTickCount64()) + ",\"total\":" + total;
        j += ",\"halo\":" + grp(halo) + ",\"guardian\":" + grp(guard);
        j += ",\"dwm\":{\"gpu\":" + (dwm.gpuOk ? Num(dwm.gpu) : "null") + "}";
        j += ",\"gpu_processes_seen\":" + (gpuOk ? std::to_string(gpuByPid.size()) : "null") + "}";
        std::lock_guard lk(g_statsMu);
        g_statsJson = std::move(j);
    }
}

// ---------------------------------------------------------------- API bodies
const char* TypeName(st::Type t) {
    switch (t) {
        case st::Type::Bool: return "bool";
        case st::Type::Int: return "int";
        case st::Type::Color: return "color";
        case st::Type::Effect: return "effect";
        case st::Type::Theme: return "theme";
    }
    return "";
}
st::Key const* FindKey(std::string const& sk) {
    for (auto const& k : st::Schema())
        if (sk == std::string(k.section) + "." + k.name) return &k;
    return nullptr;
}
st::Values Current() {  // the file over the defaults
    st::Values v = st::Defaults();
    for (auto& [k, val] : st::Read()) v[k] = val;
    return v;
}
std::string ValuesJson(st::Values const& v) {
    std::string s = "{";
    bool first = true;
    for (auto const& k : st::Schema()) {
        std::string sk = std::string(k.section) + "." + k.name;
        auto it = v.find(sk);
        s += std::string(first ? "" : ",") + JStr(sk) + ":" + JStr(it != v.end() ? it->second : k.def);
        first = false;
    }
    return s + "}";
}
std::string SettingsJson() {
    std::string s = "{\"path\":" + JStr(Utf8(st::Path())) + ",\"schema\":[";
    bool first = true;
    for (auto const& k : st::Schema()) {
        s += std::string(first ? "" : ",") + "{\"key\":" + JStr(std::string(k.section) + "." + k.name) + ",\"section\":" + JStr(k.section) +
             ",\"name\":" + JStr(k.name) + ",\"type\":\"" + TypeName(k.type) + "\",\"default\":" + JStr(k.def);
        if (k.type == st::Type::Int) s += ",\"min\":" + std::to_string(k.lo) + ",\"max\":" + std::to_string(k.hi);
        s += "}";
        first = false;
    }
    return s + "],\"defaults\":" + ValuesJson(st::Defaults()) + ",\"values\":" + ValuesJson(Current()) + "}";
}

// ---------------------------------------------------------------- HTTP
struct Response {
    int status = 200;
    std::string type = "application/json; charset=utf-8";
    std::string body;
    std::string extra;
    bool page = false;
};
Response Err(int code, const char* msg) {
    Response r;
    r.status = code;
    r.body = std::string("{\"ok\":false,\"error\":") + JStr(msg) + "}";
    return r;
}

Response Route(std::string const& method, std::string const& path, std::string const& body) {
    Response r;
    if (path.empty() || path == "/") return Err(404, "not found");
    // path must be /<token>/...
    std::string prefix = "/" + g_token;
    if (path.compare(0, prefix.size(), prefix) != 0) return Err(404, "not found");
    std::string rest = path.substr(prefix.size());
    if (rest.empty()) { r.status = 301; r.extra = "Location: " + prefix + "/\r\n"; return r; }
    if (rest[0] != '/') return Err(404, "not found");
    rest = rest.substr(1);

    if (rest.empty()) {
        if (method != "GET") return Err(405, "method not allowed");
        r.type = "text/html; charset=utf-8";
        r.body.assign(reinterpret_cast<const char*>(kPage), sizeof kPage);
        r.page = true;
        return r;
    }
    if (rest == "api/stats") {
        if (method != "GET") return Err(405, "method not allowed");
        std::lock_guard lk(g_statsMu);
        r.body = g_statsJson;
        return r;
    }
    if (rest == "api/settings") {
        std::lock_guard lk(g_settingsMu);
        if (method == "GET") { r.body = SettingsJson(); return r; }
        if (method != "POST") return Err(405, "method not allowed");
        std::map<std::string, std::string> in;
        if (!ParseFlatJson(body, in)) return Err(400, "body must be a JSON object like {\"taskbar.speed\":\"120\"}");
        st::Values cur = st::Read();
        std::string rejected = "[";
        bool anyRejected = false;
        for (auto const& [k, val] : in) {
            st::Key const* key = FindKey(k);
            std::string n = key ? st::Normalize(k, val) : std::string();
            bool valid = key && (!n.empty() || (key->type == st::Type::Color && !*key->def && Trim(val).empty()));
            if (!valid) { rejected += std::string(anyRejected ? "," : "") + JStr(k); anyRejected = true; continue; }
            cur[k] = n;
        }
        rejected += "]";
        if (!in.empty() && !st::Write(cur)) return Err(500, "could not write the settings file");
        r.body = "{\"ok\":true,\"rejected\":" + rejected + ",\"values\":" + ValuesJson(Current()) + "}";
        return r;
    }
    if (rest == "api/reset") {
        if (method != "POST") return Err(405, "method not allowed");
        std::lock_guard lk(g_settingsMu);
        std::map<std::string, std::string> in;
        if (!ParseFlatJson(body, in)) return Err(400, "body must be a JSON object like {\"section\":\"taskbar\"}");
        std::string sec = Lower(in["section"]);
        bool all = sec.empty() || sec == "all";
        bool known = all;
        st::Values cur = st::Read();
        for (auto const& k : st::Schema()) {
            if (all || sec == k.section) {
                known = true;
                cur[std::string(k.section) + "." + k.name] = k.def;
            }
        }
        if (!known) return Err(400, "unknown section");
        if (!st::Write(cur)) return Err(500, "could not write the settings file");
        r.body = "{\"ok\":true,\"values\":" + ValuesJson(Current()) + "}";
        return r;
    }
    return Err(404, "not found");
}

bool SendAll(SOCKET s, const char* p, size_t n) {
    while (n) {
        int w = send(s, p, (int)std::min<size_t>(n, 1 << 20), 0);
        if (w <= 0) return false;
        p += w;
        n -= w;
    }
    return true;
}

void Reply(SOCKET s, Response const& r) {
    const char* text = r.status == 200 ? "OK" : r.status == 301 ? "Moved Permanently" : r.status == 400 ? "Bad Request" : r.status == 403 ? "Forbidden"
                     : r.status == 404 ? "Not Found" : r.status == 405 ? "Method Not Allowed" : "Error";
    std::string h = "HTTP/1.1 " + std::to_string(r.status) + " " + text + "\r\nContent-Type: " + r.type + "\r\nContent-Length: " + std::to_string(r.body.size()) +
                    "\r\nCache-Control: no-store\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n";
    if (r.page)
        h += "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'; img-src data:; base-uri 'none'; form-action 'none'; frame-ancestors 'none'\r\n";
    h += r.extra + "\r\n";
    if (SendAll(s, h.data(), h.size())) SendAll(s, r.body.data(), r.body.size());
}

void Handle(SOCKET s) {
    DWORD tmo = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof tmo);
    std::string buf;
    char tmp[4096];
    size_t headEnd = std::string::npos;
    while (headEnd == std::string::npos && buf.size() < 65536) {
        int n = recv(s, tmp, sizeof tmp, 0);
        if (n <= 0) { closesocket(s); return; }
        buf.append(tmp, n);
        headEnd = buf.find("\r\n\r\n");
    }
    if (headEnd == std::string::npos) { Reply(s, Err(400, "bad request")); closesocket(s); return; }

    std::string head = buf.substr(0, headEnd);
    std::string body = buf.substr(headEnd + 4);
    size_t eol = head.find("\r\n");
    std::string line = head.substr(0, eol);
    std::map<std::string, std::string> hdr;
    for (size_t p = eol == std::string::npos ? head.size() : eol + 2; p < head.size();) {
        size_t e = head.find("\r\n", p);
        if (e == std::string::npos) e = head.size();
        std::string l = head.substr(p, e - p);
        p = e + 2;
        if (size_t c = l.find(':'); c != std::string::npos) hdr[Lower(Trim(l.substr(0, c)))] = Trim(l.substr(c + 1));
    }
    size_t sp1 = line.find(' '), sp2 = line.find(' ', sp1 + 1);
    Response r;
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        r = Err(400, "bad request");
    } else {
        std::string method = line.substr(0, sp1), path = line.substr(sp1 + 1, sp2 - sp1 - 1);
        if (size_t q = path.find('?'); q != std::string::npos) path.resize(q);
        std::string wantHost = "127.0.0.1:" + std::to_string(g_port);
        if (hdr["host"] != wantHost) {
            r = Err(403, "bad host");
        } else if (hdr.count("origin") && hdr["origin"] != "http://" + wantHost) {
            r = Err(403, "bad origin");
        } else {
            size_t want = hdr.count("content-length") ? (size_t)strtoul(hdr["content-length"].c_str(), nullptr, 10) : 0;
            if (want > 65536) {
                r = Err(400, "body too large");
            } else {
                while (body.size() < want) {
                    int n = recv(s, tmp, sizeof tmp, 0);
                    if (n <= 0) break;
                    body.append(tmp, n);
                }
                if (method == "POST" && Lower(hdr["content-type"]).rfind("application/json", 0) != 0) r = Err(400, "content-type must be application/json");
                else {
                    r = Route(method, path, body.substr(0, want));
                    // Only a request that carried the token counts as "the page is alive".
                    if (path.compare(0, g_token.size() + 1, "/" + g_token) == 0) g_lastRequest = GetTickCount64();
                }
            }
        }
    }
    Reply(s, r);
    shutdown(s, SD_SEND);
    closesocket(s);
}

SOCKET StartServer() {
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd)) return INVALID_SOCKET;
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return ls;
    BOOL ex = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&ex, sizeof ex);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(ls, (sockaddr*)&a, sizeof a) || listen(ls, 16)) { closesocket(ls); return INVALID_SOCKET; }
    int len = sizeof a;
    getsockname(ls, (sockaddr*)&a, &len);
    g_port = ntohs(a.sin_port);
    return ls;
}

// ---------------------------------------------------------------- window
std::wstring EnvPath(const wchar_t* name) {
    wchar_t b[MAX_PATH] = {};
    GetEnvironmentVariableW(name, b, MAX_PATH);
    return b;
}
std::wstring FindEdge() {
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        wchar_t v[MAX_PATH] = {};
        DWORD sz = sizeof v;
        if (RegGetValueW(root, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\msedge.exe", nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                         nullptr, v, &sz) == ERROR_SUCCESS && GetFileAttributesW(v) != INVALID_FILE_ATTRIBUTES)
            return v;
    }
    for (const wchar_t* e : {L"ProgramFiles(x86)", L"ProgramFiles"}) {
        std::wstring p = EnvPath(e) + L"\\Microsoft\\Edge\\Application\\msedge.exe";
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    return {};
}

struct FindCtx { HWND found = nullptr; };
BOOL CALLBACK FindWin(HWND h, LPARAM lp) {
    wchar_t cls[64] = {}, title[128] = {};
    GetClassNameW(h, cls, 64);
    if (wcscmp(cls, L"Chrome_WidgetWin_1") != 0 || !IsWindowVisible(h)) return TRUE;
    GetWindowTextW(h, title, 128);
    if (wcsncmp(title, L"Halo Settings", 13) == 0) { reinterpret_cast<FindCtx*>(lp)->found = h; return FALSE; }
    return TRUE;
}
HWND FindSettingsWindow() {
    FindCtx c;
    EnumWindows(FindWin, (LPARAM)&c);
    return c.found;
}
void Focus(HWND h) {
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    SetForegroundWindow(h);
}

// The app ID Halo Settings goes by. Its Start shortcut carries it and so does its Edge window, so Halo's dock shows
// the window as Halo Settings, with this exe's icon, rather than as an Edge page.
constexpr wchar_t kAumid[] = L"Halo.Settings";
constexpr GUID kAppUserModel = {0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}};
const PROPERTYKEY kPkeyRelaunchCommand = {kAppUserModel, 2}, kPkeyRelaunchIcon = {kAppUserModel, 3},
                  kPkeyRelaunchName = {kAppUserModel, 4}, kPkeyAumid = {kAppUserModel, 5};

std::wstring SelfPath() {
    wchar_t b[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, b, ARRAYSIZE(b));
    return {b, n};
}

bool SetProps(IPropertyStore* ps, std::vector<std::pair<PROPERTYKEY, std::wstring>> const& props) {
    bool ok = true;
    for (auto const& [key, value] : props) {  // the callers put the ID last, as the shell asks
        PROPVARIANT v;
        if (FAILED(InitPropVariantFromString(value.c_str(), &v))) return false;
        ok = SUCCEEDED(ps->SetValue(key, v)) && ok;
        PropVariantClear(&v);
    }
    return SUCCEEDED(ps->Commit()) && ok;
}

// "Halo Settings" in Start (and in Halo's search): a shortcut to this exe that carries kAumid.
void EnsureShortcut() {
    PWSTR programs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs))) return;
    std::wstring lnk = std::wstring(programs) + L"\\Halo Settings.lnk";
    CoTaskMemFree(programs);
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return;
    std::wstring exe = SelfPath();
    link->SetPath(exe.c_str());
    link->SetWorkingDirectory(exe.substr(0, exe.find_last_of(L'\\')).c_str());
    link->SetDescription(L"Customise Halo");
    IPropertyStore* ps = nullptr;
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&ps)))) {
        SetProps(ps, {{kPkeyAumid, kAumid}});
        ps->Release();
    }
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
        file->Save(lnk.c_str(), TRUE);
        file->Release();
    }
    link->Release();
}

// Gives the Edge window our ID and relaunch details, then sets its title again so window watchers (Halo's dock)
// look at it again: setting a property raises no event of its own.
void TagWindow(HWND h) {
    IPropertyStore* ps = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(&ps)))) return;
    std::wstring exe = SelfPath();
    SetProps(ps, {{kPkeyRelaunchCommand, L"\"" + exe + L"\""}, {kPkeyRelaunchIcon, exe + L",0"},
                  {kPkeyRelaunchName, L"Halo Settings"}, {kPkeyAumid, kAumid}});
    ps->Release();
    wchar_t title[128] = {};
    GetWindowTextW(h, title, 128);
    SendMessageTimeoutW(h, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(title), SMTO_ABORTIFHUNG, 500, nullptr);
}

// Edge's window belongs to another process and turns up when it likes: look for it for up to `tries` x 50 ms
// (15 s for the first window; the relaunch path joins this thread, so it passes less).
std::thread TagWhenShown(int tries = 300) {
    return std::thread([tries] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        for (int i = 0; i < tries; i++, Sleep(50))
            if (HWND h = FindSettingsWindow()) {
                TagWindow(h);
                break;
            }
        if (SUCCEEDED(co)) CoUninitialize();
    });
}

HANDLE LaunchEdge(std::wstring const& edge, std::wstring const& url, std::wstring const& profile) {
    std::wstring cmd = L"\"" + edge + L"\" --app=" + url + L" --user-data-dir=\"" + profile + L"\" --window-size=1080,760 --no-first-run --no-default-browser-check";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

std::string ReadFileText(std::wstring const& p) {
    std::string s;
    if (FILE* f = _wfopen(p.c_str(), L"rb")) {
        char b[512];
        for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
        fclose(f);
    }
    return Trim(s);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    bool serveOnly = false;
    std::wstring urlFile;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--serve")) serveOnly = true;
        else if (!wcscmp(argv[i], L"--url-file") && i + 1 < argc) urlFile = argv[++i];
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (int i = 1; i < argc; i++)
        if (!wcscmp(argv[i], L"--register")) {
            EnsureShortcut();
            return 0;
        }

    std::wstring uiDir = st::Dir() + L"\\settings-ui";
    if (urlFile.empty()) urlFile = uiDir + L"\\url.txt";

    HANDLE mutex = nullptr;
    if (!serveOnly) {
        mutex = CreateMutexW(nullptr, FALSE, L"Local\\HaloSettingsSingleton");
        if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            // Already running: bring its window to the front, or reopen it if the window was closed.
            if (HWND h = FindSettingsWindow()) { Focus(h); return 0; }
            std::string url = ReadFileText(urlFile);
            std::wstring edge = FindEdge();
            if (!url.empty() && !edge.empty()) {
                CloseHandle(LaunchEdge(edge, Wide(url), uiDir));
                TagWhenShown(120).join();  // 6 s: this process only lingers for it, windowless
            }
            else if (!url.empty()) ShellExecuteW(nullptr, L"open", Wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
    }

    unsigned char rnd[16];
    if (!SystemFunction036(rnd, sizeof rnd)) return 1;
    for (unsigned char b : rnd) { char h[3]; snprintf(h, sizeof h, "%02x", b); g_token += h; }

    SOCKET ls = StartServer();
    if (ls == INVALID_SOCKET) {  // a -mwindows exe has no console: without this, clicking Halo Settings would do nothing at all
        MessageBoxW(nullptr, L"Halo Settings could not start its local server (127.0.0.1). Check that a firewall or security tool is not blocking it.",
                    L"Halo Settings", MB_OK | MB_ICONWARNING);
        return 2;
    }
    g_lastRequest = GetTickCount64();
    std::thread(StatsThread).detach();
    std::thread([ls] {
        for (;;) {
            SOCKET c = accept(ls, nullptr, nullptr);
            if (c == INVALID_SOCKET) {  // a reset or aborted connection must not end the server for good
                int e = WSAGetLastError();
                if (e == WSAENOTSOCK || e == WSAEINVAL || e == WSANOTINITIALISED) break;
                if (e != WSAEINTR) Sleep(10);
                continue;
            }
            try { std::thread(Handle, c).detach(); } catch (...) { closesocket(c); }  // out of threads: drop this one
        }
    }).detach();

    std::string url = "http://127.0.0.1:" + std::to_string(g_port) + "/" + g_token + "/";
    CreateDirectoryW(st::Dir().c_str(), nullptr);
    CreateDirectoryW(uiDir.c_str(), nullptr);
    if (FILE* f = _wfopen(urlFile.c_str(), L"wb")) { fwrite(url.data(), 1, url.size(), f); fclose(f); }

    if (serveOnly) { for (;;) Sleep(60000); }

    std::wstring edge = FindEdge();
    HANDLE proc = edge.empty() ? nullptr : LaunchEdge(edge, Wide(url), uiDir);
    if (proc) TagWhenShown().detach();
    if (proc) {
        ULONGLONG t0 = GetTickCount64();
        WaitForSingleObject(proc, INFINITE);
        CloseHandle(proc);
        if (GetTickCount64() - t0 < 5000) {
            // Edge handed the window to an already-running process of the same profile: wait for the window itself.
            // A slow machine can take a while to show the window: only count it gone once it has been seen, or after 20 s.
            bool seen = false;
            for (int gone = 0; gone < 3; Sleep(1000)) {
                if (FindSettingsWindow()) { seen = true; gone = 0; }
                else if (seen || GetTickCount64() - t0 > 20000) gone++;
            }
        }
    } else {
        // No Edge: default browser, and leave once the page stops polling.
        ShellExecuteW(nullptr, L"open", Wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        while (GetTickCount64() - g_lastRequest < 60000) Sleep(1000);
    }
    DeleteFileW(urlFile.c_str());
    if (mutex) CloseHandle(mutex);
    ExitProcess(0);
}
