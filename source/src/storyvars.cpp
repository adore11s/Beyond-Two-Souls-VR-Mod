#include "storyvars.h"

#include <windows.h>
#include <intrin.h>

#include <MinHook.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "game.h"
#include "log.h"

namespace b2s::storyvars {
namespace {

constexpr uint64_t kLookupVa = 0x140b05c30;  // VARIABLE lookup by name (sigdb)
constexpr uint8_t kAfterCall[] = {0xBA, 0x01, 0x00, 0x00, 0x00, 0x49, 0x8B, 0xCE, 0x48, 0x85, 0xC0, 0x74};
constexpr uint8_t kLoadValue[] = {0x48, 0x8B, 0x40, 0x30};
constexpr uint8_t kReadInt[] = {0x48, 0x63, 0x38}, kReadBool[] = {0x0F, 0xB6, 0x38};

using LookupFn = uint8_t* (*)(void* entity, void* name, void* a3, void* a4);
LookupFn g_orig = nullptr;
std::atomic<AfterFn> g_after{nullptr};
std::atomic<bool> g_available{false};

// The object holding the story decisions (the entity of a Local.DT* read) and the misses the game read.
std::atomic<void*> g_storyEntity{nullptr};
constexpr int kMisses = 64, kName = 80;
struct Miss {
    void* entity;
    int reader;
    uint32_t reads;
    char name[kName];
};
Miss g_miss[kMisses];
int g_missCount = 0;
SRWLOCK g_lock = SRWLOCK_INIT;

bool CopyName(void* name, char* out) {
    __try {
        const char* t = name ? *static_cast<char**>(name) : nullptr;
        if (!t) return false;
        int n = 0;
        for (; n < kName - 1 && t[n]; ++n) {
            const char c = t[n];
            if (c < 32 || c > 126) return false;
            out[n] = c;
        }
        out[n] = 0;
        return n > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Record(void* entity, void* name, const uint8_t* found, int reader) {
    char text[kName];
    if (!CopyName(name, text)) return;
    if (found && std::strncmp(text, "Local.DT", 8) == 0) g_storyEntity.store(entity, std::memory_order_relaxed);
    if (found) return;
    AcquireSRWLockExclusive(&g_lock);
    int i = 0;
    for (; i < g_missCount; ++i)
        if (g_miss[i].entity == entity && std::strcmp(g_miss[i].name, text) == 0) break;
    if (i == g_missCount) {
        if (g_missCount < kMisses) ++g_missCount;
        else i = kMisses - 1;  // full: the newest replaces the last
        g_miss[i] = Miss{entity, reader, 0, {}};
        std::memcpy(g_miss[i].name, text, sizeof(text));
    }
    ++g_miss[i].reads;
    ReleaseSRWLockExclusive(&g_lock);
}

uint8_t* HookLookup(void* entity, void* name, void* a3, void* a4) {
    uint8_t* v = g_orig(entity, name, a3, a4);
    const int reader = ReaderAt(_ReturnAddress());
    if (reader && entity) Record(entity, name, v, reader);
    if (AfterFn after = g_after.load(std::memory_order_acquire)) v = after(entity, name, v, reader);
    return v;
}

// Walks one VARIABLE_CONTAINER (no lock: the report may run while the game is crashing). Decisions first.
int ListVariables(void* entity, bool decisionsOnly, void (*add)(const char*, void*), void* ctx, int budget) {
    int shown = 0;
    __try {
        const uint8_t* container = *reinterpret_cast<uint8_t* const*>(static_cast<uint8_t*>(entity) + 0x30);
        if (!container) return 0;
        const uint8_t* node = *reinterpret_cast<uint8_t* const*>(container + 0x60);
        for (int guard = 0; node && guard < 20000 && shown < budget; ++guard) {
            const char* nameText = *reinterpret_cast<const char* const*>(node + 0x40);
            const uint8_t type = node[0x49];
            const uint8_t* holder = *reinterpret_cast<const uint8_t* const*>(node + 0x30);
            char name[kName] = "";
            if (nameText) {
                int n = 0;
                for (; n < kName - 1 && nameText[n] >= 32 && nameText[n] <= 126; ++n) name[n] = nameText[n];
                name[n] = 0;
            }
            const bool decision = std::strncmp(name, "Local.DT", 8) == 0;
            if (name[0] && holder && (type == 0 || type == 1) && decision == decisionsOnly) {
                char line[160];
                if (type == 1) sprintf_s(line, "%s = %d", name, *reinterpret_cast<const int*>(holder));
                else sprintf_s(line, "%s = %s", name, holder[0] ? "true" : "false");
                add(line, ctx);
                ++shown;
            }
            node = *reinterpret_cast<const uint8_t* const*>(node + 0x10);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        add("(the list could not be read any further)", ctx);
    }
    return shown;
}

}  // namespace

int ReaderAt(const void* returnAddress) {
    const auto* ra = static_cast<const uint8_t*>(returnAddress);
    __try {
        if (std::memcmp(ra, kAfterCall, sizeof(kAfterCall)) != 0) return 0;
        const uint8_t* p = ra + sizeof(kAfterCall) + 1;  // past the je displacement
        if (std::memcmp(p, kLoadValue, sizeof(kLoadValue)) != 0) return 0;
        p += sizeof(kLoadValue);
        if (std::memcmp(p, kReadInt, sizeof(kReadInt)) == 0) return 1;
        if (std::memcmp(p, kReadBool, sizeof(kReadBool)) == 0) return 2;
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void Install() {
    void* target = reinterpret_cast<void*>(game::Va(kLookupVa));
    if (!target) {
        Log("STORY variable lookup not found in this build: no story variables in crash reports");
        return;
    }
    const bool ok = MH_CreateHook(target, reinterpret_cast<void*>(&HookLookup), reinterpret_cast<void**>(&g_orig)) == MH_OK &&
                    MH_EnableHook(target) == MH_OK;
    g_available = ok;
    Log("STORY variable reads observed for crash reports (read-only): %s", ok ? "on" : "FAILED");
}

bool Available() { return g_available.load(); }

void SetAfter(AfterFn fn) { g_after.store(fn, std::memory_order_release); }

void DescribeForCrash(void (*add)(const char* line, void* ctx), void* ctx) {
    if (!g_available.load()) {
        add("(story variables not available in this build)", ctx);
        return;
    }
    void* story = g_storyEntity.load();
    if (!story) {
        add("Story decisions: none read yet in this run (main menu, or the chapter has not asked for one)", ctx);
    } else {
        add("Story decisions in the loaded story (Local.DT<chapter><decision>, as the game holds them now):", ctx);
        if (!ListVariables(story, true, add, ctx, 400)) add("  (none)", ctx);
        add("Other variables of that story object:", ctx);
        if (!ListVariables(story, false, add, ctx, 150)) add("  (none)", ctx);
    }
    add("Story variables the game asked for and did not find (it reads them as 7777 / false):", ctx);
    if (!TryAcquireSRWLockShared(&g_lock)) {
        add("  (busy)", ctx);
        return;
    }
    for (int i = 0; i < g_missCount; ++i) {
        char line[160];
        sprintf_s(line, "  %s (%s, read %u time(s))", g_miss[i].name, g_miss[i].reader == 1 ? "int" : "bool", g_miss[i].reads);
        add(line, ctx);
    }
    if (!g_missCount) add("  (none)", ctx);
    ReleaseSRWLockShared(&g_lock);
}

}  // namespace b2s::storyvars
