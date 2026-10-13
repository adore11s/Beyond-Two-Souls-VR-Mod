#include "log.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <share.h>
#include <string>

namespace b2s {
namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
// Privacy (owner 2026-10-13: the files players send must not identify them; PC specs may stay). Every
// line is scrubbed before it is stored: the game folder path -> <game folder>, the user profile path ->
// <user profile>, the Windows user name as a path part -> <user>, the computer name -> <pc>. All UTF-8,
// as the mod prints paths (Narrow in dllmain.cpp). Case-insensitive, longest path first.
struct Secret {
    char text[520];
    size_t len;
    const char* with;
    bool pathPart;  // only between path separators (a user name may also be an ordinary word)
};
Secret g_secrets[4]{};
int g_secretCount = 0;
void AddSecret(const wchar_t* w, const char* with, bool pathPart) {
    if (!w || !w[0] || g_secretCount >= 4) return;
    Secret& x = g_secrets[g_secretCount];
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, x.text, int(sizeof(x.text)), nullptr, nullptr);
    if (n <= 2) return;
    x.len = strlen(x.text);
    while (x.len > 3 && (x.text[x.len - 1] == '\\' || x.text[x.len - 1] == '/')) x.text[--x.len] = 0;
    x.with = with;
    x.pathPart = pathPart;
    ++g_secretCount;
}
bool Sep(char c) { return c == '\\' || c == '/'; }
LARGE_INTEGER g_freq{}, g_start{};
wchar_t g_dir[MAX_PATH] = L"";

// The last lines, for crash reports (static: a crashed process must not need the heap).
constexpr int kRecent = 80;
char g_recent[kRecent][320];
unsigned g_recentNext = 0;
}  // namespace

void LogScrub(char* line, size_t cap) {
    if (!line || !g_secretCount) return;
    char out[2048];
    size_t o = 0;
    const char* p = line;
    while (*p && o < sizeof(out) - 32) {
        bool hit = false;
        for (int i = 0; i < g_secretCount && !hit; ++i) {
            const Secret& x = g_secrets[i];
            if (_strnicmp(p, x.text, x.len) != 0) continue;
            if (x.pathPart) {
                const char next = p[x.len];
                if (p == line || !Sep(p[-1]) || (next && !Sep(next) && next != '"' && next != ' ')) continue;
            }
            const size_t wl = strlen(x.with);
            memcpy(out + o, x.with, wl);
            o += wl;
            p += x.len;
            hit = true;
        }
        if (!hit) out[o++] = *p++;
    }
    out[o] = 0;
    strncpy_s(line, cap, out, _TRUNCATE);
}

void LogOpen(const wchar_t* gameDir) {
    std::lock_guard lock(g_mutex);
    swprintf_s(g_dir, L"%s\\b2svr_logs", gameDir);
    if (!g_secretCount) {
        wchar_t profile[MAX_PATH]{}, user[128]{}, pc[128]{};
        GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
        GetEnvironmentVariableW(L"USERNAME", user, 128);
        GetEnvironmentVariableW(L"COMPUTERNAME", pc, 128);
        // longest first: a game folder inside the profile becomes <game folder>, not <user profile>\...
        if (wcslen(gameDir) >= wcslen(profile)) {
            AddSecret(gameDir, "<game folder>", false);
            AddSecret(profile, "<user profile>", false);
        } else {
            AddSecret(profile, "<user profile>", false);
            AddSecret(gameDir, "<game folder>", false);
        }
        AddSecret(user, "<user>", true);
        AddSecret(pc, "<pc>", false);
    }
    CreateDirectoryW(g_dir, nullptr);
    std::wstring path = std::wstring(g_dir) + L"\\b2svr.log";
    std::wstring prev = std::wstring(g_dir) + L"\\b2svr.prev.log";
    MoveFileExW(path.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);
}

const wchar_t* LogDir() { return g_dir; }

double LogSeconds() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return g_freq.QuadPart ? double(now.QuadPart - g_start.QuadPart) / double(g_freq.QuadPart) : 0.0;
}

void Log(const char* fmt, ...) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double t = g_freq.QuadPart ? double(now.QuadPart - g_start.QuadPart) / double(g_freq.QuadPart) : 0.0;

    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    LogScrub(buf, sizeof(buf));

    std::lock_guard lock(g_mutex);
    snprintf(g_recent[g_recentNext % kRecent], sizeof(g_recent[0]), "[%9.3f][%5lu] %s", t, GetCurrentThreadId(), buf);
    ++g_recentNext;
    if (!g_file) return;
    fprintf(g_file, "[%9.3f][%5lu] %s\n", t, GetCurrentThreadId(), buf);
    fflush(g_file);
}

void LogFlush() {
    std::lock_guard lock(g_mutex);
    if (g_file) fflush(g_file);
}

void LogRecent(void (*fn)(const char* line, void* ctx), void* ctx) {
    // try_lock: the crashing thread may be inside Log. Reading unlocked can only tear a line.
    const bool locked = g_mutex.try_lock();
    const unsigned end = g_recentNext;
    const unsigned begin = end > unsigned(kRecent) ? end - kRecent : 0;
    for (unsigned i = begin; i < end; ++i) {
        char line[sizeof(g_recent[0])];
        memcpy(line, g_recent[i % kRecent], sizeof(line));
        line[sizeof(line) - 1] = 0;
        fn(line, ctx);
    }
    if (locked) g_mutex.unlock();
}

}  // namespace b2s
