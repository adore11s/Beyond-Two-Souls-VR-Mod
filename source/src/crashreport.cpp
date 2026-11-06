#include "crashreport.h"

#include <windows.h>
#include <bcrypt.h>
#include <d3d11.h>
#include <dbghelp.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <csignal>
#include <cstring>

#include "fpcam.h"
#include "log.h"
#include "storyvars.h"
#include "texguard.h"
#include "xr.h"

namespace b2s::crash {
namespace {

// ---- facts collected ahead of time (a crashed process should do as little as possible) ----
char g_version[64] = "?";
// SHA-256 of the mod's own DLL file: the name of its folder in the private symbols archive
// (tools/make_release.ps1: backups\symbols\<SHA-256>\dinput8.pdb), so a report maps to its exact build.
char g_dllHash[72] = "?";
char g_textHash[80] = "not checked yet";
bool g_checked = false, g_supported = false;
char g_os[96] = "?", g_cpu[96] = "?", g_ram[32] = "?", g_gpu[192] = "(game device not created yet)";
uintptr_t g_exeBase = 0, g_exeSize = 0, g_modBase = 0, g_modSize = 0;
// EXP-057: the GPU device was lost (a Windows GPU timeout / driver reset) before the crash: what DXGI said, for the report.
char g_deviceLost[320] = "";
// 1.0.0: the level (NoteLevel), e.g. "2101 MEET MOM PARK (adult Jodie (JHO20), character '_2101_MEET_MOM_PARK_Jodie')".
char g_level[256] = "";

// SHA-256 of the DLL file on disk (upper-case hex, as Get-FileHash prints it).
void HashOwnFile(HMODULE self) {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(self, path, MAX_PATH)) return;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        static UCHAR buf[1 << 16];
        DWORD got = 0;
        while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) BCryptHashData(hash, buf, got, 0);
        UCHAR digest[32];
        if (BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0)
            for (int i = 0; i < 32; ++i) sprintf_s(g_dllHash + i * 2, 3, "%02X", digest[i]);
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
}

void ModuleRange(HMODULE m, uintptr_t* base, uintptr_t* size) {
    const auto* b = reinterpret_cast<const uint8_t*>(m);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(b + reinterpret_cast<const IMAGE_DOS_HEADER*>(b)->e_lfanew);
    *base = reinterpret_cast<uintptr_t>(m);
    *size = nt->OptionalHeader.SizeOfImage;
}

void CollectSystemInfo() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    if (auto rgv = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) {
        OSVERSIONINFOW v{sizeof(v)};
        if (rgv(&v) == 0) sprintf_s(g_os, "Windows %lu.%lu build %lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    }
    wchar_t cpu[96] = L"";
    DWORD sz = sizeof(cpu);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, cpu, &sz) == ERROR_SUCCESS)
        sprintf_s(g_cpu, "%ls", cpu);
    for (size_t n = strlen(g_cpu); n && g_cpu[n - 1] == ' '; --n) g_cpu[n - 1] = 0;  // the registry pads it
    MEMORYSTATUSEX ms{sizeof(ms)};
    if (GlobalMemoryStatusEx(&ms)) sprintf_s(g_ram, "%.0f GB", double(ms.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0));
}

// ---- the report text: one static buffer, no heap ----
char g_rep[96 * 1024];
size_t g_repLen = 0;

void Add(const char* fmt, ...) {
    if (g_repLen >= sizeof(g_rep) - 1) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = _vsnprintf_s(g_rep + g_repLen, sizeof(g_rep) - g_repLen, _TRUNCATE, fmt, ap);
    va_end(ap);
    g_repLen = n < 0 ? sizeof(g_rep) - 1 : g_repLen + size_t(n);
}

// A log line, scrubbed again (lines are scrubbed when logged; this also covers a line logged before the
// log was opened).
void AddScrubbed(const char* line, void*) {
    char out[400];
    strncpy_s(out, line, _TRUNCATE);
    LogScrub(out, sizeof(out));
    Add("  %s\r\n", out);
}

// "Module.dll+0x1234" (file name only: no folders, so no user name).
void AddrName(uintptr_t a, char* out, size_t cap) {
    HMODULE m = nullptr;
    if (a && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(a), &m) && m) {
        wchar_t p[MAX_PATH] = L"?";
        GetModuleFileNameW(m, p, MAX_PATH);
        const wchar_t* n = wcsrchr(p, L'\\');
        sprintf_s(out, cap, "%ls+0x%llx", n ? n + 1 : p, static_cast<unsigned long long>(a - reinterpret_cast<uintptr_t>(m)));
    } else {
        sprintf_s(out, cap, "0x%llx", static_cast<unsigned long long>(a));
    }
}

bool InExe(uintptr_t a) { return g_exeBase && a >= g_exeBase + 0x1000 && a < g_exeBase + g_exeSize; }
bool InMod(uintptr_t a) { return g_modBase && a >= g_modBase + 0x1000 && a < g_modBase + g_modSize; }

constexpr DWORD kTestCode = 0xE0B25001;
// EXP-054: no exception reaches the filter for these (the CRT ends the process with __fastfail): reported from the
// CRT's own handlers, the context taken there.
constexpr DWORD kPurecallCode = 0xE0B25002, kAbortCode = 0xE0B25003;

const char* ExceptionName(DWORD c) {
    switch (c) {
        case EXCEPTION_ACCESS_VIOLATION: return "access violation";
        case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
        case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
        case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
        case EXCEPTION_BREAKPOINT: return "breakpoint";
        case 0xE06D7363: return "C++ exception";
        case 0xC0000409: return "stack buffer overrun / fail fast";
        case kTestCode: return "TEST REPORT (command crashreport - no crash happened)";
        case kPurecallCode: return "pure virtual function call (an object used after it was destroyed)";
        case kAbortCode: return "abort (the game's runtime ended the process)";
        default: return "exception";
    }
}

// 0.26.0 (owner 2026-11-06: "make sure its only one file they have to send over when it crashes"): the report file
// carries everything. Before, the whole log went to a second file (crash_<date>_log.txt) and no memory dump was kept.
// Static buffers only: the process is crashing.
uint8_t g_io[48 * 1024];  // a multiple of 3 (base64 groups)
char g_b64[64 * 1024 + 2048];

void Put(HANDLE f, const void* data, size_t n) {
    DWORD w = 0;
    if (n) WriteFile(f, data, DWORD(n), &w, nullptr);
}
void PutText(HANDLE f, const char* text) { Put(f, text, strlen(text)); }

// The whole log of the crashed run (the next two game starts rotate b2svr.log away; its lines are already scrubbed by
// LogScrub when they were logged, so this is a plain copy).
void AppendLog(HANDLE out) {
    wchar_t src[MAX_PATH];
    swprintf_s(src, L"%s\\b2svr.log", LogDir());
    PutText(out, "\r\n==================== The mod's whole log of this run (b2svr.log) ====================\r\n");
    HANDLE f = CreateFileW(src, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        PutText(out, "(the log could not be read)\r\n");
        return;
    }
    DWORD got = 0;
    while (ReadFile(f, g_io, sizeof(g_io), &got, nullptr) && got) Put(out, g_io, got);
    CloseHandle(f);
    PutText(out, "\r\n==================== End of the log ====================\r\n");
}

// A small Windows minidump of the crash, base64 inside the report (tools/crash_extract.py turns it back into a .dmp):
// every thread's registers and stack, for the symbols kept privately per build (backups\symbols\<sha256>). Privacy:
// MiniDumpFilterMemory keeps only what a stack walk needs from the stacks (other stack bytes are zeroed),
// MiniDumpFilterModulePaths drops the folders from module paths (no user name); no heap memory is included.
wchar_t g_dumpPath[MAX_PATH];  // static, wiped before the dump: LIVE 0.26.0 a stack copy of it (with the folder) was in the dump
void AppendDump(HANDLE out, EXCEPTION_POINTERS* ep) {
    using DumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                 PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
    auto dump = dbg ? reinterpret_cast<DumpFn>(GetProcAddress(dbg, "MiniDumpWriteDump")) : nullptr;
    PutText(out, "\r\n==================== Memory dump for the developer (base64, no personal data) ====================\r\n");
    if (!dump) {
        PutText(out, "(dbghelp.dll not available: no dump)\r\n");
        return;
    }
    swprintf_s(g_dumpPath, L"%s\\crash_reports\\dump.tmp", LogDir());
    HANDLE d = CreateFileW(g_dumpPath, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    SecureZeroMemory(g_dumpPath, sizeof(g_dumpPath));
    if (d == INVALID_HANDLE_VALUE) {
        PutText(out, "(no temporary file for the dump)\r\n");
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
    const MINIDUMP_TYPE type = MINIDUMP_TYPE(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                             MiniDumpFilterMemory | MiniDumpFilterModulePaths);
    if (!dump(GetCurrentProcess(), GetCurrentProcessId(), d, type, &mei, nullptr, nullptr)) {
        char line[96];
        sprintf_s(line, "(MiniDumpWriteDump failed, error %lu)\r\n", GetLastError());
        PutText(out, line);
        CloseHandle(d);
        return;
    }
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    SetFilePointer(d, 0, nullptr, FILE_BEGIN);
    DWORD got = 0;
    size_t col = 0, tailLen = 0;
    uint8_t tail[3]{};
    while (ReadFile(d, g_io, sizeof(g_io), &got, nullptr) && got) {
        size_t n = 0;
        const size_t whole = got - got % 3;  // only the last read can be short
        for (size_t i = 0; i < whole; i += 3) {
            const uint32_t v = (uint32_t(g_io[i]) << 16) | (uint32_t(g_io[i + 1]) << 8) | g_io[i + 2];
            g_b64[n++] = kAlphabet[(v >> 18) & 63];
            g_b64[n++] = kAlphabet[(v >> 12) & 63];
            g_b64[n++] = kAlphabet[(v >> 6) & 63];
            g_b64[n++] = kAlphabet[v & 63];
            if ((col += 4) >= 100) {
                g_b64[n++] = '\r';
                g_b64[n++] = '\n';
                col = 0;
            }
        }
        Put(out, g_b64, n);
        tailLen = got - whole;
        for (size_t k = 0; k < tailLen; ++k) tail[k] = g_io[whole + k];
    }
    if (tailLen) {
        const uint32_t v = (uint32_t(tail[0]) << 16) | (tailLen > 1 ? uint32_t(tail[1]) << 8 : 0u);
        const char last[5] = {kAlphabet[(v >> 18) & 63], kAlphabet[(v >> 12) & 63], tailLen > 1 ? kAlphabet[(v >> 6) & 63] : '=',
                              '=', 0};
        PutText(out, last);
    }
    CloseHandle(d);  // FILE_FLAG_DELETE_ON_CLOSE: the temporary dump is gone
    PutText(out, "\r\n==================== End of the dump ====================\r\n");
}

void WriteReport(EXCEPTION_POINTERS* ep) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const CONTEXT* c = ep->ContextRecord;
    const uintptr_t rip = reinterpret_cast<uintptr_t>(er->ExceptionAddress);
    char where[200];
    AddrName(rip, where, sizeof(where));
    char fp[160] = "?";
    fpcam::DescribeForCrash(fp, sizeof(fp));
    g_repLen = 0;
    Add("Beyond: Two Souls VR mod - crash report\r\n");
    Add("Please send this ONE file with a few words on what you were doing when the game crashed.\r\n");
    Add("It holds this report, the mod's whole log of this run and a small memory dump (at the end).\r\n");
    Add("No personal data: no user name, no files or folders outside the game (the dump is filtered by Windows).\r\n\r\n");
    Add("When:        %04u-%02u-%02u %02u:%02u:%02u\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    Add("Mod:         B2SVR %s (dinput8.dll sha256 %s)\r\n", g_version, g_dllHash);
    Add("Game:        .text sha256 %s (%s)\r\n", g_textHash,
        !g_checked ? "the mod was still starting up" : g_supported ? "the supported build" : "NOT a supported build - mod was off");
    Add("Windows:     %s\r\n", g_os);
    Add("CPU:         %s, %s RAM\r\n", g_cpu, g_ram);
    Add("GPU:         %s\r\n", g_gpu);
    Add("Headset:     %s (runtime %s)\r\n", xr::SystemName()[0] ? xr::SystemName() : "(no OpenXR session yet)",
        xr::RuntimeName()[0] ? xr::RuntimeName() : "-");
    Add("VR:          %s\r\n", xr::Running() ? "running" : "off");
    Add("First person:%s\r\n", fp);
    Add("Level:       %s\r\n", g_level[0] ? g_level : "(not in a level yet)");
    if (g_deviceLost[0])
        Add("\r\nGraphics:    %s\r\n"
            "             The game cannot draw after that: its own code rebuilds the swap chain and then reads the missing\r\n"
            "             back buffer (FUN_140cbc460 -> FUN_140cbc2f0 -> FUN_140cbe830), which is the crash below.\r\n",
            g_deviceLost);
    Add("\r\nCrash:       0x%08lx %s at %s (%s)\r\n", er->ExceptionCode, ExceptionName(er->ExceptionCode), where,
        InMod(rip) ? "inside the VR mod" : InExe(rip) ? "inside the game" : "inside another module");
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        Add("             %s address 0x%llx\r\n",
            er->ExceptionInformation[0] == 0 ? "reading" : er->ExceptionInformation[0] == 1 ? "writing" : "executing",
            static_cast<unsigned long long>(er->ExceptionInformation[1]));
    Add("Thread:      %lu\r\n", GetCurrentThreadId());
    Add("Registers:   rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx rbp %llx rsp %llx\r\n", c->Rax, c->Rbx,
        c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->Rsp);
    Add("             r8 %llx r9 %llx r10 %llx r11 %llx r12 %llx r13 %llx r14 %llx r15 %llx\r\n", c->R8, c->R9, c->R10,
        c->R11, c->R12, c->R13, c->R14, c->R15);
    Add("Stack (return address candidates in the game and the mod):\r\n");
    const auto* sp = reinterpret_cast<const uintptr_t*>(c->Rsp);
    int found = 0;
    for (int i = 0; i < 2048 && found < 32; ++i) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(sp + i, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) break;
        const uintptr_t v = sp[i];
        if (!InExe(v) && !InMod(v)) continue;
        char n[200];
        AddrName(v, n, sizeof(n));
        Add("  [rsp+0x%04x] %s\r\n", i * 8, n);
        ++found;
    }
    if (!found) Add("  (none found)\r\n");
    Add("\r\nStory (read-only, storyvars.h):\r\n");
    storyvars::DescribeForCrash(&AddScrubbed, nullptr);
    Add("\r\nTextures (EXP-054):\r\n");
    texguard::DescribeForCrash(&AddScrubbed, nullptr);
    Add("\r\nLast lines of the mod's log:\r\n");
    LogRecent(&AddScrubbed, nullptr);

    wchar_t path[MAX_PATH];
    if (!LogDir()[0]) return;
    swprintf_s(path, L"%s\\crash_reports", LogDir());
    CreateDirectoryW(path, nullptr);
    wchar_t file[64];
    swprintf_s(file, L"\\crash_%04u-%02u-%02u_%02u-%02u-%02u.txt", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
               t.wSecond);
    wcscat_s(path, file);
    // Logged (and flushed) first, so the copy of the log inside the report ends with this line.
    Log("CRASH report: b2svr_logs\\crash_reports%ls (%s at %s) - report, whole log and memory dump in this one file", file,
        ExceptionName(er->ExceptionCode), where);
    LogFlush();
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    Put(f, g_rep, g_repLen);
    FlushFileBuffers(f);  // the report itself is on disk even if the rest fails
    __try {
        AppendLog(f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        PutText(f, "\r\n(copying the log failed)\r\n");
    }
    SecureZeroMemory(path, sizeof(path));  // the report's own path (with the folder) is not left on this stack either
    __try {
        AppendDump(f, ep);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        PutText(f, "\r\n(writing the memory dump failed)\r\n");
    }
    FlushFileBuffers(f);
    CloseHandle(f);
}

// ---- the CRT's pure-call and abort paths (EXP-054) ----
// The game links the shared CRT (VCRUNTIME140 _purecall -> ucrtbase abort -> __fastfail 7): neither the vectored
// handler nor the top-level filter runs, so the owner's crash on 2026-11-04 left no report. ucrtbase's own handler
// setters are process-wide for every module on the shared CRT (the mod links the static one, hence GetProcAddress).
std::atomic<int> g_reported{0};
using PurecallHandler = void(__cdecl*)();
using SignalHandler = void(__cdecl*)(int);
PurecallHandler g_prevPurecall = nullptr;
SignalHandler g_prevAbort = nullptr;
using SetPurecallFn = PurecallHandler(__cdecl*)(PurecallHandler);
using GetPurecallFn = PurecallHandler(__cdecl*)();
SetPurecallFn g_setPurecall = nullptr;
GetPurecallFn g_getPurecall = nullptr;

void ReportFromHere(DWORD code) {
    if (g_reported.fetch_add(1) != 0) return;
    CONTEXT c{};
    RtlCaptureContext(&c);
    EXCEPTION_RECORD er{};
    er.ExceptionCode = code;
    er.ExceptionAddress = reinterpret_cast<void*>(c.Rip);
    // The crash site is the first game address up the stack (the caller of _purecall / abort), not this function.
    const auto* sp = reinterpret_cast<const uintptr_t*>(c.Rsp);
    for (int i = 0; i < 1024; ++i) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(sp + i, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) break;
        if (InExe(sp[i])) {
            er.ExceptionAddress = reinterpret_cast<void*>(sp[i]);
            break;
        }
    }
    EXCEPTION_POINTERS ep{&er, &c};
    __try {
        WriteReport(&ep);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void __cdecl OnPurecall() {
    Log("CRASH pure virtual function call on thread %lu (an object used after it was destroyed)", GetCurrentThreadId());
    ReportFromHere(kPurecallCode);
    if (g_prevPurecall) g_prevPurecall();
}

void __cdecl OnAbort(int sig) {
    Log("CRASH abort on thread %lu", GetCurrentThreadId());
    ReportFromHere(kAbortCode);
    if (g_prevAbort && g_prevAbort != SIG_DFL && g_prevAbort != SIG_IGN && g_prevAbort != SIG_ERR) g_prevAbort(sig);
}

void InstallCrtHandlers() {
    HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
    using SignalFn = SignalHandler(__cdecl*)(int, SignalHandler);
    auto setPurecall = ucrt ? reinterpret_cast<SetPurecallFn>(GetProcAddress(ucrt, "_set_purecall_handler")) : nullptr;
    g_setPurecall = setPurecall;
    g_getPurecall = ucrt ? reinterpret_cast<GetPurecallFn>(GetProcAddress(ucrt, "_get_purecall_handler")) : nullptr;
    auto setSignal = ucrt ? reinterpret_cast<SignalFn>(GetProcAddress(ucrt, "signal")) : nullptr;
    if (setPurecall) g_prevPurecall = setPurecall(&OnPurecall);
    if (setSignal) {
        const SignalHandler prev = setSignal(SIGABRT, &OnAbort);
        if (prev != SIG_ERR) g_prevAbort = prev;
    }
    Log("crash: pure virtual call and abort reports %s (shared CRT ucrtbase.dll%s)",
        setPurecall && setSignal ? "on" : "OFF", ucrt ? "" : " not loaded");
}

// ---- first-chance logger: where a crash started (never handles anything) ----
std::atomic<int> g_vehExe{0}, g_vehMod{0};

LONG CALLBACK FirstChance(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t rip = info->ContextRecord->Rip;
    const bool mod = InMod(rip);
    if (!mod && !InExe(rip)) return EXCEPTION_CONTINUE_SEARCH;  // drivers / Windows
    // The mod's own SEH-guarded reads raise first-chance faults too: separate, smaller budget.
    if ((mod ? g_vehMod : g_vehExe).fetch_add(1) >= (mod ? 4 : 6)) return EXCEPTION_CONTINUE_SEARCH;
    const CONTEXT* c = info->ContextRecord;
    char where[200];
    AddrName(rip, where, sizeof(where));
    Log("CRASH (first-chance) code=0x%08lx at %s addr=%p thread=%lu%s", code, where,
        reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]), GetCurrentThreadId(),
        mod ? " - may be a guarded read the mod handles itself" : "");
    Log("CRASH rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx r8=%llx r9=%llx", c->Rax, c->Rbx, c->Rcx, c->Rdx,
        c->Rsi, c->Rdi, c->R8, c->R9);
    const auto* sp = reinterpret_cast<const uintptr_t*>(c->Rsp);
    int shown = 0;
    for (int i = 0; i < 512 && shown < 16; ++i) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(sp + i, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) break;
        if (!InExe(sp[i]) && !InMod(sp[i])) continue;
        char n[200];
        AddrName(sp[i], n, sizeof(n));
        Log("CRASH   [rsp+0x%03x] %s", i * 8, n);
        ++shown;
    }
    LogFlush();
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---- the top-level filter: ours first, then the game's own, then any other that replaced ours ----
std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> g_gameFilter{nullptr}, g_otherFilter{nullptr};
thread_local bool t_inFilter = false;

LONG WINAPI Filter(EXCEPTION_POINTERS* ep) {
    if (t_inFilter) return EXCEPTION_CONTINUE_SEARCH;  // a chained filter calling back into ours
    t_inFilter = true;
    if (g_reported.fetch_add(1) == 0) {
        __try {
            WriteReport(ep);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    LONG r = EXCEPTION_CONTINUE_SEARCH;
    if (LPTOP_LEVEL_EXCEPTION_FILTER g = g_gameFilter.load(); g && g != Filter) r = g(ep);
    if (r == EXCEPTION_CONTINUE_SEARCH)
        if (LPTOP_LEVEL_EXCEPTION_FILTER o = g_otherFilter.load(); o && o != Filter && o != g_gameFilter.load()) r = o(ep);
    t_inFilter = false;
    return r;
}

// The game's own SetUnhandledExceptionFilter (its full-core-dump writer): kept behind ours.
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI HookSetFilter(LPTOP_LEVEL_EXCEPTION_FILTER f) {
    LPTOP_LEVEL_EXCEPTION_FILTER prev = g_gameFilter.load();
    if (f != Filter) g_gameFilter = f;
    Log("crash: the game installed its crash handler %p - chained behind the mod's crash report", reinterpret_cast<void*>(f));
    return prev;
}

// Points the exe's import of SetUnhandledExceptionFilter at HookSetFilter.
bool PatchExeImport() {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    void* real = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetUnhandledExceptionFilter"));
    void* realBase = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "SetUnhandledExceptionFilter"));
    bool patched = false;
    __try {
        for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
            auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
            for (; iat->u1.Function; ++iat) {
                void* fn = reinterpret_cast<void*>(iat->u1.Function);
                if (fn != real && fn != realBase) continue;
                DWORD old = 0;
                if (VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                    iat->u1.Function = reinterpret_cast<ULONGLONG>(&HookSetFilter);
                    VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
                    patched = true;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return patched;
}

// Keeps ours in front: anything that replaced it is chained behind (1 Hz, own thread).
void KeepFront() {
    if (g_getPurecall && g_setPurecall) {
        const PurecallHandler cur = g_getPurecall();
        if (cur != &OnPurecall) {
            if (cur) g_prevPurecall = cur;
            g_setPurecall(&OnPurecall);
            Log("crash: another pure-call handler (%p) was set - chained behind the mod's crash report",
                reinterpret_cast<void*>(cur));
        }
    }
    LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(Filter);
    if (prev && prev != Filter && prev != g_gameFilter.load() && prev != g_otherFilter.load()) {
        if (!g_gameFilter.load()) g_gameFilter = prev;
        else g_otherFilter = prev;
        Log("crash: another crash handler (%p) was installed - chained behind the mod's crash report",
            reinterpret_cast<void*>(prev));
    }
}

DWORD WINAPI KeepThread(LPVOID) {
    for (;;) {
        KeepFront();
        Sleep(1000);
    }
}

}  // namespace

void Install(const char* modVersion) {
    static bool done = false;
    if (done) return;
    done = true;
    strcpy_s(g_version, modVersion);
    ModuleRange(GetModuleHandleW(nullptr), &g_exeBase, &g_exeSize);
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&Install), &self) && self) {
        ModuleRange(self, &g_modBase, &g_modSize);
        HashOwnFile(self);
    }
    CollectSystemInfo();
    AddVectoredExceptionHandler(1, FirstChance);
    LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(Filter);
    if (prev && prev != Filter) g_gameFilter = prev;
    Log("crash: reports on (b2svr_logs\\crash_reports); %s %s, %s; dinput8.dll sha256 %s", g_os, g_cpu, g_ram, g_dllHash);
    InstallCrtHandlers();
}

void InstallLate(const char* textSha256, bool supportedBuild) {
    strcpy_s(g_textHash, textSha256);
    g_supported = supportedBuild;
    g_checked = true;
    const bool patched = PatchExeImport();
    KeepFront();
    if (HANDLE t = CreateThread(nullptr, 0, KeepThread, nullptr, 0, nullptr)) {
        SetThreadPriority(t, THREAD_PRIORITY_LOWEST);
        CloseHandle(t);
    }
    Log("crash: game crash handler chaining: import hook %s, filter kept in front at 1 Hz", patched ? "on" : "not found");
}

void NoteDevice(ID3D11Device* dev) {
    if (!dev) return;
    IDXGIDevice* dx = nullptr;
    IDXGIAdapter* ad = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dx))) && SUCCEEDED(dx->GetAdapter(&ad))) {
        DXGI_ADAPTER_DESC d{};
        ad->GetDesc(&d);
        LARGE_INTEGER umd{};
        char drv[48] = "?";
        if (SUCCEEDED(ad->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
            sprintf_s(drv, "%u.%u.%u.%u", unsigned(HIWORD(umd.HighPart)), unsigned(LOWORD(umd.HighPart)),
                      unsigned(HIWORD(umd.LowPart)), unsigned(LOWORD(umd.LowPart)));
        sprintf_s(g_gpu, "%ls, %.1f GB, driver %s", d.Description, double(d.DedicatedVideoMemory) / (1024.0 * 1024.0 * 1024.0), drv);
        Log("crash: GPU %s (device creation flags 0x%x)", g_gpu, dev->GetCreationFlags());
    }
    if (ad) ad->Release();
    if (dx) dx->Release();
}

namespace {
const char* RemovedReasonName(HRESULT r) {
    switch (static_cast<uint32_t>(r)) {
        case 0x887A0006u: return "DXGI_ERROR_DEVICE_HUNG (Windows blames this game's own GPU work for the timeout)";
        case 0x887A0007u: return "DXGI_ERROR_DEVICE_RESET (the GPU was reset for a reason outside this game's own commands: "
                                 "another program's GPU work or the driver)";
        case 0x887A0005u: return "DXGI_ERROR_DEVICE_REMOVED (driver crash, update or GPU removed)";
        case 0x887A0020u: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR (graphics driver failure)";
        case 0x887A0001u: return "DXGI_ERROR_INVALID_CALL";
        case 0u: return "S_OK (the device is not removed)";
        default: return "unknown reason";
    }
}

}  // namespace

void NoteDeviceLost(const char* where, long hr, IDXGISwapChain* sc) {
    static std::atomic<int> noted{0};
    const uint32_t h = static_cast<uint32_t>(hr);
    if (h != 0x887A0005u && h != 0x887A0006u && h != 0x887A0007u && h != 0x887A0020u) return;
    if (noted.fetch_add(1) != 0) return;
    HRESULT reason = static_cast<HRESULT>(h);
    ID3D11Device* dev = nullptr;
    if (sc && SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&dev))) && dev) {
        reason = dev->GetDeviceRemovedReason();
        dev->Release();
    }
    sprintf_s(g_deviceLost, "the GPU device was lost at %s (0x%08lx): reason 0x%08lx %s - a Windows GPU timeout / driver "
              "reset (System event log: nvlddmkm / Display, LiveKernelEvent 141)",
              where, static_cast<unsigned long>(h), static_cast<unsigned long>(reason), RemovedReasonName(reason));
    Log("GPU DEVICE LOST: %s", g_deviceLost);
    LogFlush();
}


void NoteLevel(const char* character, const char* who) {
    // "_2101_MEET_MOM_PARK_Jodie" -> "2101 MEET MOM PARK"; menu characters ("MENU_GAME_Jodie") -> the main menu.
    char level[128] = "";
    if (character && std::strncmp(character, "MENU_", 5) == 0) {
        strcpy_s(level, "main menu");
    } else if (character && character[0] == '_') {
        size_t n = 0;
        for (const char* c = character + 1; *c && n < sizeof(level) - 1; ++c) level[n++] = *c == '_' ? ' ' : *c;
        level[n] = 0;
        const size_t len = strlen(level);  // drop the trailing character name (" Jodie")
        if (const char* last = strrchr(level, ' '); last && len > 6) level[last - level] = 0;
    } else {
        strcpy_s(level, "unknown");
    }
    char text[256];
    sprintf_s(text, "%s (%s, character '%s')", level, who ? who : "?", character ? character : "?");
    LogScrub(text, sizeof(text));
    strcpy_s(g_level, text);
}

void WriteTestReport() {
    CONTEXT c{};
    RtlCaptureContext(&c);
    EXCEPTION_RECORD er{};
    er.ExceptionCode = kTestCode;
    er.ExceptionAddress = reinterpret_cast<void*>(c.Rip);
    EXCEPTION_POINTERS ep{&er, &c};
    WriteReport(&ep);
}

}  // namespace b2s::crash
