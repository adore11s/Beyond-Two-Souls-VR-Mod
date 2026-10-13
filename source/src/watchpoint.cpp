#include "watchpoint.h"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdint>

#include "game.h"
#include "log.h"

namespace b2s::watchpoint {
namespace {

constexpr int kSlots = 64;
struct Hit {
    std::atomic<uint64_t> rva{0};
    std::atomic<uint32_t> thread{0};
    std::atomic<uint32_t> which{0};
    std::atomic<uint64_t> count{0};
};
Hit g_hits[kSlots];
// Reads with no game code on the stack (driver / runtime threads): module + offset of the reader.
struct Foreign {
    std::atomic<uintptr_t> mod{0};
    std::atomic<uint64_t> off{0};
    std::atomic<uint32_t> thread{0};
    std::atomic<uint64_t> count{0};
};
Foreign g_foreign[16];
std::atomic<uint64_t> g_other{0};
std::atomic<bool> g_armed{false};
std::atomic<bool> g_busy{false};
PVOID g_veh = nullptr;
uintptr_t g_addr[2] = {};
int g_seconds = 5;

constexpr DWORD64 kOurDr7 = 0x5ull | 0xFF0000ull;  // L0, L1 and their RW / LEN fields

LONG CALLBACK OnSingleStep(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = info->ContextRecord;
    const DWORD64 status = c->Dr6 & 3;
    // LIVE 0.16.21 crash (0x80000004 at exe+0x997073): a trap of ours arriving while the watch was
    // being disarmed had DR6 already clear and went unhandled. Any trap while one of our slots is
    // enabled in this thread (and no trace flag) is ours; once disarmed, the thread's slots are
    // cleared through the context it resumes with (the kernel restores debug registers from it).
    const bool ourSlots = (c->Dr7 & 0x5ull) != 0 && (c->EFlags & 0x100) == 0;
    if (!status && !ourSlots) return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 = 0;
    if (!g_armed.load() || !status) {
        if (!g_armed.load()) {
            c->Dr0 = c->Dr1 = 0;
            c->Dr7 &= ~kOurDr7;
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    const uintptr_t base = game::ModuleBase();
    const uintptr_t rip = static_cast<uintptr_t>(c->Rip);
    uint64_t rva = 0;
    if (rip < base || rip >= base + 0x2200000) {
        ++g_other;
        // Not game code (CRT memcpy, D3D11, our DLL): attribute it to the first game-code return
        // address on the stack, tagged with bit 63 so the report can say "via".
        HMODULE self = nullptr, mod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&OnSingleStep), &self);
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(rip), &mod);
        if (mod == self) return EXCEPTION_CONTINUE_EXECUTION;  // our own reads
        const auto* sp = reinterpret_cast<const uintptr_t*>(c->Rsp);
        for (int i = 0; i < 64 && !rva; ++i) {
            const uintptr_t v = sp[i];
            if (v > base + 0x1000 && v < base + 0x13bc000) rva = (v - base) | (1ull << 63);
        }
        if (!rva) {
            const uintptr_t m = reinterpret_cast<uintptr_t>(mod);
            for (auto& f : g_foreign) {
                uintptr_t expected = 0;
                if (f.mod.load() == m && f.off.load() == rip - m) { ++f.count; break; }
                if (f.mod.compare_exchange_strong(expected, m)) {
                    f.off = rip - m;
                    f.thread = GetCurrentThreadId();
                    f.count = 1;
                    break;
                }
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    } else {
        rva = rip - base;
    }
    for (auto& h : g_hits) {
        uint64_t expected = 0;
        if (h.rva.load() == rva || h.rva.compare_exchange_strong(expected, rva)) {
            if (h.count.fetch_add(1) == 0) {
                h.thread = GetCurrentThreadId();
                h.which = static_cast<uint32_t>(status);
            }
            break;
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

void SetAll(bool arm) {
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof(te)};
    int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != DWORD(-1)) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c)) {
                if (arm) {
                    c.Dr0 = g_addr[0];
                    c.Dr1 = g_addr[1];
                    // L0/L1 on; RW = 11 (read or write); LEN = 10 (8 bytes)
                    c.Dr7 = (c.Dr7 & ~0xFF0000Full) | (g_addr[0] ? 1 : 0) | (g_addr[1] ? 4 : 0) | (3ull << 16) |
                            (2ull << 18) | (3ull << 20) | (2ull << 22);
                } else {
                    c.Dr0 = c.Dr1 = 0;
                    c.Dr7 &= ~0xFF0000Full;
                }
                if (SetThreadContext(t, &c)) ++n;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    Log("WATCH %s on %d threads (a=%p b=%p)", arm ? "armed" : "disarmed", n, reinterpret_cast<void*>(g_addr[0]),
        reinterpret_cast<void*>(g_addr[1]));
}

DWORD WINAPI Run(LPVOID) {
    for (auto& h : g_hits) { h.rva = 0; h.count = 0; h.thread = 0; h.which = 0; }
    for (auto& f : g_foreign) { f.mod = 0; f.off = 0; f.thread = 0; f.count = 0; }
    g_other = 0;
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, OnSingleStep);  // stays installed for late traps
    g_armed = true;
    SetAll(true);
    Sleep(DWORD(g_seconds) * 1000);
    g_armed = false;
    SetAll(false);
    for (auto& h : g_hits) {
        if (!h.rva.load()) continue;
        const uint64_t r = h.rva.load();
        Log("WATCH %sexe+0x%llx thread=%u slot=%s hits=%llu", (r >> 63) ? "outside-exe via " : "",
            static_cast<unsigned long long>(r & ~(1ull << 63)), h.thread.load(), h.which.load() & 1 ? "A" : "B",
            static_cast<unsigned long long>(h.count.load()));
    }
    for (auto& f : g_foreign) {
        if (!f.mod.load()) continue;
        wchar_t name[MAX_PATH] = L"?";
        GetModuleFileNameW(reinterpret_cast<HMODULE>(f.mod.load()), name, MAX_PATH);
        const wchar_t* base = wcsrchr(name, L'\\');
        char narrow[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, base ? base + 1 : name, -1, narrow, sizeof(narrow), nullptr, nullptr);
        Log("WATCH foreign %s+0x%llx thread=%u hits=%llu (no game code on the stack)", narrow,
            static_cast<unsigned long long>(f.off.load()), f.thread.load(), static_cast<unsigned long long>(f.count.load()));
    }
    Log("WATCH done (%llu hits outside the exe)", static_cast<unsigned long long>(g_other.load()));
    g_busy = false;
    return 0;
}

}  // namespace

bool Start(void* a, void* b, int seconds) {
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) return false;
    g_addr[0] = reinterpret_cast<uintptr_t>(a);
    g_addr[1] = reinterpret_cast<uintptr_t>(b);
    g_seconds = seconds > 0 && seconds <= 30 ? seconds : 5;
    if (HANDLE t = CreateThread(nullptr, 0, Run, nullptr, 0, nullptr)) {
        CloseHandle(t);
        return true;
    }
    g_busy = false;
    return false;
}

}  // namespace b2s::watchpoint
