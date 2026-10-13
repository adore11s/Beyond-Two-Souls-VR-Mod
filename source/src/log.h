#pragma once
#include <cstdarg>

namespace b2s {

// Opens <gameDir>\b2svr_logs\b2svr.log, rotating the previous run to b2svr.prev.log.
void LogOpen(const wchar_t* gameDir);
void Log(const char* fmt, ...);
void LogFlush();
// Seconds since LogOpen: the clock of every log line's timestamp.
double LogSeconds();
// <gameDir>\b2svr_logs (empty before LogOpen).
const wchar_t* LogDir();
// Crash reports: calls fn(line, ctx) for the most recent log lines, oldest first. Never blocks
// (a crash inside Log may hold the lock) and never allocates.
void LogRecent(void (*fn)(const char* line, void* ctx), void* ctx);

}  // namespace b2s
