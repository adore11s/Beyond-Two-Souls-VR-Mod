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
LARGE_INTEGER g_freq{}, g_start{};
wchar_t g_dir[MAX_PATH] = L"";

// The last lines, for crash reports (static: a crashed process must not need the heap).
constexpr int kRecent = 80;
char g_recent[kRecent][320];
unsigned g_recentNext = 0;
}  // namespace

void LogOpen(const wchar_t* gameDir) {
    std::lock_guard lock(g_mutex);
    swprintf_s(g_dir, L"%s\\b2svr_logs", gameDir);
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
