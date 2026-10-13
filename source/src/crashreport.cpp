#include "crashreport.h"

#include <windows.h>
#include <bcrypt.h>
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "fpcam.h"
#include "log.h"
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
        default: return "exception";
    }
}

// The whole log of the crashed run, saved beside the report (crash_<date>_log.txt): the next two game
// starts rotate b2svr.log away, and the report only holds its last lines. Its lines are already scrubbed
// (LogScrub, when they were logged), so this is a plain copy.
void CopyLog(const SYSTEMTIME& t) {
    if (!LogDir()[0]) return;
    wchar_t src[MAX_PATH], dst[MAX_PATH];
    swprintf_s(src, L"%s\\b2svr.log", LogDir());
    swprintf_s(dst, L"%s\\crash_reports\\crash_%04u-%02u-%02u_%02u-%02u-%02u_log.txt", LogDir(), t.wYear, t.wMonth, t.wDay,
               t.wHour, t.wMinute, t.wSecond);
    CopyFileW(src, dst, FALSE);
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
    Add("Please send this file with a few words on what you were doing when the game crashed.\r\n");
    Add("It holds no personal data: no user name, no files or folders outside the game.\r\n\r\n");
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
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(f, g_rep, DWORD(g_repLen), &w, nullptr);
        FlushFileBuffers(f);
        CloseHandle(f);
    }
    Log("CRASH report written: b2svr_logs\\crash_reports%ls (%s at %s)", file, ExceptionName(er->ExceptionCode), where);
    LogFlush();
    CopyLog(t);
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
std::atomic<int> g_reported{0};
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
        Log("crash: GPU %s", g_gpu);
    }
    if (ad) ad->Release();
    if (dx) dx->Release();
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
