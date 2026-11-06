#include "texguard.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "game.h"
#include "log.h"
#include "zonefix.h"

namespace b2s::texguard {
namespace {

// Analysed-build addresses (sigdb, tools/sig_targets.json).
constexpr uint64_t kBindTexturesVa = 0x140816f00;   // FUN_140816f00(rendererShader, a2, a3): binds +0x480[0..+0x47a)
constexpr uint64_t kSkinApplyVa = 0x141325fc0;      // RENDERER_SHADER_SKIN::vfunc_01: binds (*(this+0x10))+0x480[+0x48c]
constexpr uint64_t kReleaseVa = 0x140c7b080;        // FUN_140c7b080(object, deferred): deferred list or delete now
constexpr uint64_t kTextureDtorVa = 0x140cbe420;    // FUN_140cbe420: TEXTURE_DX11 destructor body (cube / volume too)
constexpr uint64_t kITextureVt = 0x141734f28;       // KRENDERER::I_TEXTURE vftable: the destructor's last vtable
// 0.24.0 (owner crash 2026-11-05, exe+0xcbebb1 = FUN_140cbeb00 calling vfunc +0x90 of a slot texture whose memory had been
// freed AND reused: its first qword was a heap pointer, so the I_TEXTURE test let it through). STATIC: the only live
// KRENDERER texture classes (RTTI: every class with I_TEXTURE as a base; constructors FUN_140cbe500 / 5c0 / 6f0 / 830 /
// 9d0, cube FUN_140cbecf0 / da0 and inlined in FUN_140914f00, volume FUN_140cbf440 / 4f0) are these three; a slot is a
// live texture only when its object carries one of their vtables. Anything else (destroyed: I_TEXTURE; reused memory: a
// heap pointer or another class; unreadable) is not a texture any more.
constexpr uint64_t kTextureDx11Vt = 0x141734d68;    // KRENDERER::TEXTURE_DX11
constexpr uint64_t kCubeTextureVt = 0x1417350d0;    // KRENDERER::CUBE_TEXTURE_DX11
constexpr uint64_t kVolumeTextureVt = 0x141735290;  // KRENDERER::VOLUME_TEXTURE_DX11
constexpr uint64_t kRenderStateVa = 0x141c78958;    // RenderState*; +0xBF4 frame counter (eye-keyed in eye passes)
constexpr uint32_t kEyeKeyBits = 0x60000000u;       // stereo.cpp kEyeKey
// 0.21.7 (STATIC): the engine's once-per-frame refill of a material shader, SHADER_CUSTOM::vfunc_03 (stamp +0x630 vs
// RenderState+0xBF4 -> FUN_14090fe80). For a streamed file texture it asks ENGINE_TEXTURE_FILE::vfunc_110 (+0x370),
// which LOADS the texture when it is not loaded (+0x140 == 0 -> vfunc +0x90): in the game, refilling a drawn shader is
// what brings its unloaded textures back. The streaming thread's step (FUN_140863290) and the frame-start flush
// (FUN_140828650) hold the spin lock g_streamLockOwner (owner thread id, recursion count at +4).
constexpr uint64_t kShaderCustomVt = 0x14170d2f0;
constexpr uint64_t kShaderRefillVa = 0x14090fd80;
constexpr uint64_t kStreamLockVa = 0x141c8f3e0;
constexpr size_t kShaderStamp = 0x630;

// Renderer shader fields (STATIC, FUN_140816f00 / FUN_14090fe80 / SHADER_CUSTOM::vfunc_05). The engine SHADER_CUSTOM
// embeds the renderer shader at +0x80 (FUN_140910f70), so engine fields are renderer - 0x80 + offset.
constexpr size_t kSlotCount = 0x47a;    // uint16
constexpr size_t kSlots = 0x480;        // void*[]
constexpr size_t kSkinSlot = 0x48c;     // uint8, 0xff = none
constexpr size_t kEngineFromRenderer = 0x80;
constexpr size_t kEngineHandles = 0x5b0;      // KCOM handles of the engine textures, same index
constexpr size_t kEngineHandleCount = 0x636;  // uint8
constexpr uint32_t kMaxSlots = 64;

uintptr_t g_deadVt = 0;
uintptr_t g_liveVt[3] = {};  // TEXTURE_DX11, CUBE_TEXTURE_DX11, VOLUME_TEXTURE_DX11 (all three found, else 0)
std::atomic<uint64_t> g_reusedCaught{0};

// 0 = a live texture, 1 = destroyed (I_TEXTURE vtable), 2 = not a texture any more (memory reused or unreadable).
// Without the three live vtables (another build where a signature is missing) only the destroyed case is recognised.
int TextureState(void* t) {
    uintptr_t vt = 0;
    __try {
        vt = *static_cast<uintptr_t*>(t);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return g_liveVt[0] ? 2 : 0;
    }
    if (vt == g_deadVt) return 1;
    if (!g_liveVt[0]) return 0;
    return vt == g_liveVt[0] || vt == g_liveVt[1] || vt == g_liveVt[2] ? 0 : 2;
}
bool IsLiveTexture(void* t) { return t && TextureState(t) == 0; }

// ---- ring of releases and destructions, with the game call stack ----
enum Kind : uint8_t { kQueued = 1, kDeletedNow = 2, kDestroyed = 3 };
constexpr int kFrames = 8;
struct Event {
    std::atomic<void*> obj{nullptr};
    uint32_t frame = 0;
    uint32_t tid = 0;
    uint8_t kind = 0;
    void* stack[kFrames] = {};
};
constexpr uint32_t kRing = 4096;
Event g_ring[kRing];
std::atomic<uint32_t> g_next{0};
std::atomic<uint64_t> g_queued{0}, g_deletedNow{0}, g_destroyed{0}, g_caught{0}, g_checks{0};

uint32_t FrameNumber() {
    __try {
        auto* rs = *reinterpret_cast<uint8_t**>(game::Va(kRenderStateVa));
        return rs ? (*reinterpret_cast<uint32_t*>(rs + 0xBF4) & ~kEyeKeyBits) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void Record(void* obj, Kind kind) {
    if (!obj) return;
    Event& e = g_ring[g_next.fetch_add(1, std::memory_order_relaxed) % kRing];
    e.obj.store(nullptr, std::memory_order_relaxed);  // invalid while it is rewritten
    e.frame = FrameNumber();
    e.tid = GetCurrentThreadId();
    e.kind = kind;
    std::memset(e.stack, 0, sizeof(e.stack));
    RtlCaptureStackBackTrace(2, kFrames, e.stack, nullptr);
    e.obj.store(obj, std::memory_order_release);
}

// "140914c50" for the game (its analysed-build address), "dinput8.dll+0x1234" / "0x..." otherwise.
void FrameName(void* a, char* out, size_t cap) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(a);
    const uintptr_t base = game::ModuleBase();
    HMODULE m = nullptr;
    if (v && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(v), &m) && m) {
        if (reinterpret_cast<uintptr_t>(m) == base) {
            snprintf(out, cap, "%llx", static_cast<unsigned long long>(v - base + 0x140000000ull));
            return;
        }
        wchar_t p[MAX_PATH] = L"?";
        GetModuleFileNameW(m, p, MAX_PATH);
        const wchar_t* n = wcsrchr(p, L'\\');
        snprintf(out, cap, "%ls+0x%llx", n ? n + 1 : p, static_cast<unsigned long long>(v - reinterpret_cast<uintptr_t>(m)));
        return;
    }
    snprintf(out, cap, "0x%llx", static_cast<unsigned long long>(v));
}

const char* KindName(uint8_t k) {
    return k == kQueued ? "released (deferred)" : k == kDeletedNow ? "released (deleted at once)" : "destroyed";
}

void DescribeEvent(const Event& e, void* obj, char* out, size_t cap) {
    int n = snprintf(out, cap, "%s %p at frame %u on thread %lu by", KindName(e.kind), obj, e.frame, e.tid);
    for (int i = 0; i < kFrames && e.stack[i] && n > 0 && size_t(n) < cap; ++i) {
        char f[96];
        FrameName(e.stack[i], f, sizeof(f));
        n += snprintf(out + n, cap - size_t(n), "%s%s", i ? " <- " : " ", f);
    }
}

// The newest event of each kind for obj (newest first in the ring).
void LogHistory(void* obj) {
    bool seen[4] = {};
    const uint32_t end = g_next.load(std::memory_order_acquire);
    for (uint32_t k = 0; k < kRing && k < end; ++k) {
        const Event& e = g_ring[(end - 1 - k) % kRing];
        if (e.obj.load(std::memory_order_acquire) != obj || e.kind > 3 || seen[e.kind]) continue;
        seen[e.kind] = true;
        char line[900];
        DescribeEvent(e, obj, line, sizeof(line));
        Log("TEXGUARD   %s", line);
    }
    if (!seen[kQueued] && !seen[kDeletedNow] && !seen[kDestroyed])
        Log("TEXGUARD   (its release is older than the last %u releases / destructions)", kRing);
}

// ---- names (SEH-guarded reads, no C++ objects with destructors) ----
bool HandleName(void* handle, char* out, size_t cap) {
    out[0] = 0;
    __try {
        if (!handle || reinterpret_cast<uintptr_t>(handle) < 0x10000) return false;
        const char* const* holder = *reinterpret_cast<const char* const* const*>(static_cast<uint8_t*>(handle) + 0x18);
        if (!holder || !*holder) return false;
        size_t i = 0;
        for (const char* s = *holder; i + 1 < cap && s[i]; ++i) out[i] = s[i];
        out[i] = 0;
        return i > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return false;
    }
}

void* EngineTextureHandle(uint8_t* rendererShader, uint32_t slot) {
    __try {
        uint8_t* engine = rendererShader - kEngineFromRenderer;
        if (slot >= *(engine + kEngineHandleCount)) return nullptr;
        void** handles = *reinterpret_cast<void***>(engine + kEngineHandles);
        return handles ? handles[slot] : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// ---- 0.21.7: drawn shaders get the engine's own per-frame refill ----
// LIVE 0.21.6 (owner: "again the black furniture and black bed"): the one repaired shader was drawn 400 - 2700 times
// per 30 s to the end with the not-ready texture, and its texture never counted as waiting or restored: it is not a
// KCOM 0x884 texture, so 0.21.6's refill never touched it, and the engine itself never refills that shader - which is
// why its slot kept a destroyed texture in the first place, and why its unloaded texture is never asked to load again.
// Now the render thread notes every SHADER_CUSTOM it binds; once per frame, before the eye passes are recorded, the game
// thread calls the engine's own refill (vfunc_03) for each one the engine did not refill in the last frame (its refills
// in the eye passes carry the eye key in the stamp), with the streaming lock taken only when it is free (else next
// frame) and only on a live object (vtable, handle back-pointer).
uintptr_t g_shaderVt = 0;
using RefillFn = char (*)(void* shader);
RefillFn g_refill = nullptr;
volatile LONG* g_lockOwner = nullptr;
constexpr uint32_t kDrawnSlots = 16384;
std::atomic<void*> g_drawn[kDrawnSlots];
std::atomic<uint64_t> g_refilled{0}, g_refillSkipped{0}, g_lockBusy{0}, g_drawnDropped{0}, g_notLive{0};

void NoteDrawn(void* rs) {
    uintptr_t h = reinterpret_cast<uintptr_t>(rs);
    h = (h ^ (h >> 17)) * 0x9E3779B97F4A7C15ull;
    const uint32_t start = static_cast<uint32_t>(h >> 40);
    for (uint32_t k = 0; k < 8; ++k) {
        std::atomic<void*>& slot = g_drawn[(start + k) & (kDrawnSlots - 1)];
        void* cur = slot.load(std::memory_order_relaxed);
        if (cur == rs) return;
        if (!cur) {
            void* expected = nullptr;
            if (slot.compare_exchange_strong(expected, rs, std::memory_order_relaxed) || expected == rs) return;
        }
    }
    g_drawnDropped.fetch_add(1, std::memory_order_relaxed);
}

// A live engine SHADER_CUSTOM: its vtable, and its KCOM handle (+0x20, cached) pointing back at it.
bool LiveShader(uint8_t* engine, uint32_t* stamp) {
    __try {
        if (*reinterpret_cast<uintptr_t*>(engine) != g_shaderVt) return false;
        uint8_t* handle = *reinterpret_cast<uint8_t**>(engine + 0x20);
        if (!handle || !((*reinterpret_cast<uint32_t*>(handle + 0x20) >> 29) & 1)) return false;
        if (*reinterpret_cast<uint8_t**>(handle + 0x30) != engine) return false;
        *stamp = *reinterpret_cast<uint32_t*>(engine + kShaderStamp);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallRefill(uint8_t* engine) {
    __try {
        g_refill(engine);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// FUN_140828650's lock, taken the same way but without waiting.
bool TryStreamLock(bool* recursive) {
    const LONG tid = static_cast<LONG>(GetCurrentThreadId());
    *recursive = *g_lockOwner == tid;
    if (!*recursive && InterlockedCompareExchange(g_lockOwner, tid, 0) != 0) return false;
    InterlockedIncrement(g_lockOwner + 1);
    return true;
}
void StreamUnlock() {
    const LONG tid = static_cast<LONG>(GetCurrentThreadId());
    if (*g_lockOwner == tid && InterlockedDecrement(g_lockOwner + 1) == 0) InterlockedExchange(g_lockOwner, 0);
}

// ---- the guard ----
struct Catch {
    void* shader;
    void* texture;
    uint32_t slot;
    uint32_t frame;
    const char* path;
};
constexpr int kCatchRing = 8;
Catch g_catches[kCatchRing];
std::atomic<uint32_t> g_catchNext{0};

// What the engine's own refill (FUN_14090fe80) would put in slot i now: the engine texture's current KRENDERER texture
// (handle loaded, bit 21 of +0x20; KCOM type 0x884; object +0x140). Otherwise the engine's "not ready" texture
// (RenderState+0xb60, FUN_14090fdc0) - 0.21.4: 0.21.3 left the slot empty, and an empty slot draws black (owner: "the
// fence or the ground is basically all black"). Never a destroyed one. `how` says which.
// The engine texture's current KRENDERER texture for slot i when it is loaded (the 0x884 branch of FUN_14090fe80),
// else null. `state`: 1 loaded, 0 not loaded, -1 no engine texture of that kind.
void* CurrentTexture(uint8_t* rs, uint32_t slot, int* state) {
    *state = -1;
    __try {
        uint8_t* engine = rs - kEngineFromRenderer;
        void** handles = *reinterpret_cast<void***>(engine + kEngineHandles);
        uint8_t* h = slot < *(engine + kEngineHandleCount) && handles ? static_cast<uint8_t*>(handles[slot]) : nullptr;
        if (!h || *reinterpret_cast<int32_t*>(h) != 0x884) return nullptr;
        *state = 0;
        if (!((*reinterpret_cast<uint32_t*>(h + 0x20) >> 21) & 1)) return nullptr;
        uint8_t* obj = *reinterpret_cast<uint8_t**>(h + 0x30);
        void* t = obj ? *reinterpret_cast<void**>(obj + 0x140) : nullptr;
        if (!IsLiveTexture(t)) return nullptr;
        *state = 1;
        return t;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* Replacement(uint8_t* rs, uint32_t slot, const char** how) {
    *how = "empty";
    int loaded = -1;
    if (void* t = CurrentTexture(rs, slot, &loaded)) {
        *how = "its engine texture's current one";
        return t;
    }
    __try {
        auto* state = *reinterpret_cast<uint8_t**>(game::Va(kRenderStateVa));
        void* f = state ? *reinterpret_cast<void**>(state + 0xb60) : nullptr;
        if (IsLiveTexture(f)) {
            *how = "the game's not-ready texture";
            return f;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

// Repairs the slots (all, or only `only`) that do not hold a live texture (destroyed: I_TEXTURE vtable; 0.24.0: memory
// already reused by something else). Null slots are the engine's own "not loaded" value and stay. Returns the count
// repaired and the first one (its old pointer and what it got).
int RepairDestroyed(uint8_t* rs, int only, uint32_t* firstSlot, void** firstTex, const char** firstHow) {
    __try {
        const uint32_t n = *reinterpret_cast<uint16_t*>(rs + kSlotCount);
        void** slots = *reinterpret_cast<void***>(rs + kSlots);
        if (!slots || n == 0 || n > kMaxSlots) return 0;
        int repaired = 0;
        for (uint32_t i = 0; i < n; ++i) {
            if (only >= 0 && i != uint32_t(only)) continue;
            void* t = slots[i];
            if (!t) continue;
            const int state = TextureState(t);
            if (state == 0) continue;
            if (state == 2) g_reusedCaught.fetch_add(1, std::memory_order_relaxed);
            const char* how = "empty";
            slots[i] = Replacement(rs, i, &how);
            if (!repaired) {
                *firstSlot = i;
                *firstTex = t;
                *firstHow = how;
            }
            ++repaired;
        }
        return repaired;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// 0.21.4 diagnostic: slots the engine left empty because their engine texture is not loaded (handle present, bit 21
// clear) - drawn black. Counts such bindings; the first ones per 30 s are logged (does black come from streaming?).
std::atomic<uint64_t> g_unloadedDraws{0};
int UnloadedSlots(uint8_t* rs, uint32_t* firstSlot) {
    __try {
        const uint32_t n = *reinterpret_cast<uint16_t*>(rs + kSlotCount);
        void** slots = *reinterpret_cast<void***>(rs + kSlots);
        uint8_t* engine = rs - kEngineFromRenderer;
        const uint32_t handlesN = *(engine + kEngineHandleCount);
        void** handles = *reinterpret_cast<void***>(engine + kEngineHandles);
        if (!slots || !handles || n == 0 || n > kMaxSlots) return 0;
        int count = 0;
        for (uint32_t i = 0; i < n && i < handlesN; ++i) {
            if (slots[i]) continue;
            auto* h = static_cast<uint8_t*>(handles[i]);
            if (!h || ((*reinterpret_cast<uint32_t*>(h + 0x20) >> 21) & 1)) continue;
            if (!count) *firstSlot = i;
            ++count;
        }
        return count;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// 0.21.5 diagnostic (owner, 0.21.4: "furniture, bed, certain ground were all black"): shaders the guard repaired with
// something other than their engine texture's current one (that texture is not loaded); their later bindings are
// counted, so the log shows whether black objects are these.
constexpr int kRepairedMax = 16;
std::atomic<void*> g_repaired[kRepairedMax];
std::atomic<int> g_repairedN{0};
std::atomic<uint64_t> g_repairedDraws{0};
void NoteRepaired(void* rs) {
    const int n = g_repairedN.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (g_repaired[i].load(std::memory_order_relaxed) == rs) return;
    if (n < kRepairedMax) {
        g_repaired[n].store(rs, std::memory_order_relaxed);
        g_repairedN.store(n + 1, std::memory_order_release);
    }
}
// 0.21.6 (owner, 0.21.5: the black furniture, bed and ground are black "also in third person"): a repaired shader is
// one the engine does not refill (its slot was left pointing at a destroyed texture), so the not-ready texture the guard
// gave it stayed for good, even after the game loaded the texture again. At every binding of a repaired shader, each
// slot gets its engine texture's current texture when that one is loaded - the value the engine's own refill
// (FUN_14090fe80) would write. A texture read here stays alive for this frame: the game deletes old textures only after
// the render thread has finished the frame (FUN_140828650).
std::atomic<uint64_t> g_restored{0};
int RefillSlots(uint8_t* rs, uint32_t* firstSlot) {
    __try {
        const uint32_t n = *reinterpret_cast<uint16_t*>(rs + kSlotCount);
        void** slots = *reinterpret_cast<void***>(rs + kSlots);
        if (!slots || n == 0 || n > kMaxSlots) return 0;
        int restored = 0;
        for (uint32_t i = 0; i < n; ++i) {
            int state = -1;
            void* cur = CurrentTexture(rs, i, &state);
            if (!cur || slots[i] == cur) continue;
            slots[i] = cur;
            if (!restored) *firstSlot = i;
            ++restored;
        }
        return restored;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
void CountRepairedDraw(void* rs) {
    const int n = g_repairedN.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (g_repaired[i].load(std::memory_order_relaxed) == rs) {
            g_repairedDraws.fetch_add(1, std::memory_order_relaxed);
            if (!g_refill) {  // no engine refill in this build: 0.21.6's own one for KCOM 0x884 textures
                uint32_t slot = 0;
                if (const int r = RefillSlots(static_cast<uint8_t*>(rs), &slot)) {
                    const uint64_t k = g_restored.fetch_add(uint64_t(r), std::memory_order_relaxed);
                    if (k < 40)
                        Log("TEXGUARD repaired shader %p: %d slot(s) got their engine texture again (slot %u first), "
                            "frame %u", rs, r, slot, FrameNumber());
                }
            }
            return;
        }
}
// Slots of repaired shaders whose engine texture is still not loaded (for the 30 s line).
int RepairedNotLoaded() {
    int count = 0;
    const int n = g_repairedN.load(std::memory_order_acquire);
    for (int k = 0; k < n; ++k) {
        auto* rs = static_cast<uint8_t*>(g_repaired[k].load(std::memory_order_relaxed));
        uint32_t slots = 0;
        __try {
            slots = *reinterpret_cast<uint16_t*>(rs + kSlotCount);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            slots = 0;
        }
        for (uint32_t i = 0; i < slots && i < kMaxSlots; ++i) {
            int state = -1;
            CurrentTexture(rs, i, &state);
            if (state == 0) ++count;
        }
    }
    return count;
}

void NoteUnloaded(uint8_t* rs) {
    uint32_t slot = 0;
    if (!UnloadedSlots(rs, &slot)) return;
    g_unloadedDraws.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<uint64_t> windowStart{0};
    static std::atomic<int> shown{0};
    const uint64_t now = GetTickCount64();
    uint64_t w = windowStart.load(std::memory_order_relaxed);
    if (now - w >= 30000 && windowStart.compare_exchange_strong(w, now)) shown = 0;
    if (shown.fetch_add(1) < 3)
        Log("TEXGUARD shader %p draws with slot %u empty: its engine texture is not loaded (drawn black), frame %u, eye "
            "passes from %s", rs, slot, FrameNumber(),
            zonefix::EyeRoomDiffers() ? "another room than the game camera's" : "the game camera's room");
}

void Caught(uint8_t* rs, uint32_t slot, void* tex, int count, const char* path, const char* how) {
    const uint64_t n = g_caught.fetch_add(uint64_t(count), std::memory_order_relaxed);
    const uint32_t frame = FrameNumber();
    g_catches[g_catchNext.fetch_add(1, std::memory_order_relaxed) % kCatchRing] = {rs, tex, slot, frame, path};
    if (n >= 30 && n % 500 != 0) return;
    char shaderName[128] = "", texName[160] = "";
    game::ObjectName(rs - kEngineFromRenderer, shaderName, sizeof(shaderName));
    HandleName(EngineTextureHandle(rs, slot), texName, sizeof(texName));
    if (std::strcmp(how, "its engine texture's current one") != 0) NoteRepaired(rs);
    Log("TEXGUARD %s texture %p in shader %p '%s' slot %u (engine texture '%s'), %s path, frame %u, thread %lu: "
        "%d slot(s) repaired, slot %u got %s (#%llu)",
        TextureState(tex) == 2 ? "freed (memory reused)" : "destroyed", tex, rs, shaderName, slot, texName, path, frame,
        GetCurrentThreadId(), count, slot, how, static_cast<unsigned long long>(n + 1));
    LogHistory(tex);
}

using BindFn = void (*)(void* shader, void* a2, void* a3);
BindFn g_origBind = nullptr;
void HookBind(void* shader, void* a2, void* a3) {
    if (shader) {
        g_checks.fetch_add(1, std::memory_order_relaxed);
        uint32_t slot = 0;
        void* tex = nullptr;
        const char* how = "";
        if (const int c = RepairDestroyed(static_cast<uint8_t*>(shader), -1, &slot, &tex, &how))
            Caught(static_cast<uint8_t*>(shader), slot, tex, c, "shader textures", how);
        NoteUnloaded(static_cast<uint8_t*>(shader));
        CountRepairedDraw(shader);
        if (g_refill) NoteDrawn(shader);
    }
    g_origBind(shader, a2, a3);
}

int SkinSlot(uint8_t* skin, uint8_t** rsOut) {
    __try {
        uint8_t* rs = *reinterpret_cast<uint8_t**>(skin + 0x10);
        if (!rs) return -1;
        *rsOut = rs;
        const uint8_t i = *(rs + kSkinSlot);
        return i == 0xff ? -1 : int(i);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

using SkinFn = void (*)(void* self, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6);
SkinFn g_origSkin = nullptr;
void HookSkin(void* self, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6) {
    if (self) {
        uint8_t* rs = nullptr;
        const int only = SkinSlot(static_cast<uint8_t*>(self), &rs);
        if (only >= 0) {
            uint32_t slot = 0;
            void* tex = nullptr;
            const char* how = "";
            if (const int c = RepairDestroyed(rs, only, &slot, &tex, &how)) Caught(rs, slot, tex, c, "skin", how);
            CountRepairedDraw(rs);
        }
    }
    g_origSkin(self, a2, a3, a4, a5, a6);
}

using ReleaseFn = DWORD (*)(void* obj, char deferred);
ReleaseFn g_origRelease = nullptr;
DWORD HookRelease(void* obj, char deferred) {
    if (obj) {
        Record(obj, deferred ? kQueued : kDeletedNow);
        (deferred ? g_queued : g_deletedNow).fetch_add(1, std::memory_order_relaxed);
    }
    return g_origRelease(obj, deferred);
}

using DtorFn = void (*)(void* tex, uintptr_t a2, uintptr_t a3, uintptr_t a4);
DtorFn g_origDtor = nullptr;
void HookDtor(void* tex, uintptr_t a2, uintptr_t a3, uintptr_t a4) {
    Record(tex, kDestroyed);
    g_destroyed.fetch_add(1, std::memory_order_relaxed);
    g_origDtor(tex, a2, a3, a4);
}

bool HookAt(uint64_t va, void* detour, void** orig, const char* name) {
    void* target = reinterpret_cast<void*>(game::Va(va));
    if (!target) {
        Log("TEXGUARD %s not found in this build: that part is off", name);
        return false;
    }
    MH_STATUS s = MH_CreateHook(target, detour, orig);
    if (s == MH_OK) s = MH_EnableHook(target);
    Log("hook %-24s %p %s", name, target, MH_StatusToString(s));
    return s == MH_OK;
}

}  // namespace

void Install() {
    g_deadVt = game::Va(kITextureVt);
    if (!g_deadVt) {
        Log("TEXGUARD I_TEXTURE vtable not found in this build: destroyed-texture guard off");
        return;
    }
    {
        const uintptr_t a = game::Va(kTextureDx11Vt), b = game::Va(kCubeTextureVt), c = game::Va(kVolumeTextureVt);
        if (a && b && c) {
            g_liveVt[0] = a;
            g_liveVt[1] = b;
            g_liveVt[2] = c;
        }
        Log("TEXGUARD live texture classes (TEXTURE_DX11 / CUBE / VOLUME vtables): %s", g_liveVt[0]
                ? "found: a slot must hold one of them (freed memory reused by something else is caught too)"
                : "NOT all found in this build: only destroyed textures not yet reused are caught");
    }
    const bool history = HookAt(kReleaseVa, reinterpret_cast<void*>(&HookRelease), reinterpret_cast<void**>(&g_origRelease),
                                "KRENDERER::ReleaseObject") &
                         HookAt(kTextureDtorVa, reinterpret_cast<void*>(&HookDtor), reinterpret_cast<void**>(&g_origDtor),
                                "TEXTURE_DX11::Destroy");
    const bool bind = HookAt(kBindTexturesVa, reinterpret_cast<void*>(&HookBind), reinterpret_cast<void**>(&g_origBind),
                             "RENDERER_SHADER::BindTextures");
    const bool skin = HookAt(kSkinApplyVa, reinterpret_cast<void*>(&HookSkin), reinterpret_cast<void**>(&g_origSkin),
                             "RENDERER_SHADER_SKIN::Apply");
    g_shaderVt = game::Va(kShaderCustomVt);
    g_refill = reinterpret_cast<RefillFn>(game::Va(kShaderRefillVa));
    g_lockOwner = reinterpret_cast<volatile LONG*>(game::Va(kStreamLockVa));
    if (!g_shaderVt || !g_lockOwner || !bind) g_refill = nullptr;
    // 0.23.0 (owner: "its probably crashing because of the furniture black fix"): b2svr.ini texture_refill=off turns the
    // 0.21.7 refill off for an A/B run (the black furniture / bed can come back with it off).
    {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring ini(exe);
        ini = ini.substr(0, ini.find_last_of(L"\\/")) + L"\\b2svr.ini";
        wchar_t v[16] = L"";
        GetPrivateProfileStringW(L"b2svr", L"texture_refill", L"on", v, 16, ini.c_str());
        if (_wcsicmp(v, L"off") == 0 && g_refill) {
            g_refill = nullptr;
            Log("TEXGUARD engine refill of drawn shaders switched off (b2svr.ini texture_refill=off)");
        }
    }
    Log("TEXGUARD destroyed-texture guard (EXP-054): shader textures %s, skin %s, release history %s, engine refill of "
        "drawn shaders %s", bind ? "on" : "OFF", skin ? "on" : "OFF", history ? "on" : "OFF", g_refill ? "on" : "OFF");
}

void RefillDrawn() {
    if (!g_refill) return;
    bool recursive = false;
    if (!TryStreamLock(&recursive)) {
        g_lockBusy.fetch_add(1, std::memory_order_relaxed);
        return;  // the streaming thread is working: next frame
    }
    const uint32_t frame = FrameNumber();
    for (uint32_t i = 0; i < kDrawnSlots; ++i) {
        void* p = g_drawn[i].exchange(nullptr, std::memory_order_relaxed);
        if (!p) continue;
        uint8_t* engine = static_cast<uint8_t*>(p) - kEngineFromRenderer;
        uint32_t stamp = 0;
        if (!LiveShader(engine, &stamp)) {
            g_notLive.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const uint32_t s = stamp & ~kEyeKeyBits;
        const bool engineDidIt = (stamp & kEyeKeyBits) ? (s <= frame && s + 1 >= frame) : s == frame;
        if (engineDidIt) {
            g_refillSkipped.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (CallRefill(engine)) g_refilled.fetch_add(1, std::memory_order_relaxed);
    }
    StreamUnlock();
}

void LogStats() {
    static uint64_t last[5] = {};
    const uint64_t now[5] = {g_queued.load(), g_deletedNow.load(), g_destroyed.load(), g_caught.load(), g_checks.load()};
    static uint64_t lastUnloaded = 0, lastRepaired = 0;
    const uint64_t unloaded = g_unloadedDraws.load(), repaired = g_repairedDraws.load();
    if (repaired != lastRepaired)
        Log("TEXGUARD 30 s: %llu bindings of the %d shader(s) repaired without their own texture; %d of their slots still "
            "wait for the game to load their texture, %llu slot(s) got it back since start",
            static_cast<unsigned long long>(repaired - lastRepaired), g_repairedN.load(), RepairedNotLoaded(),
            static_cast<unsigned long long>(g_restored.load()));
    lastRepaired = repaired;
    {
        static uint64_t l[5] = {};
        const uint64_t n[5] = {g_refilled.load(), g_refillSkipped.load(), g_lockBusy.load(), g_notLive.load(),
                               g_drawnDropped.load()};
        if (g_refill && (n[0] != l[0] || n[2] != l[2]))
            Log("TEXGUARD 30 s: engine refill of drawn shaders: %llu refilled (the game had not), %llu already refilled by "
                "the game, %llu frames skipped (streaming lock busy), %llu not live, %llu not noted (table full)",
                static_cast<unsigned long long>(n[0] - l[0]), static_cast<unsigned long long>(n[1] - l[1]),
                static_cast<unsigned long long>(n[2] - l[2]), static_cast<unsigned long long>(n[3] - l[3]),
                static_cast<unsigned long long>(n[4] - l[4]));
        std::memcpy(l, n, sizeof(l));
    }
    if (now[0] == last[0] && now[1] == last[1] && now[2] == last[2] && now[3] == last[3]) return;
    Log("TEXGUARD 30 s: %llu releases deferred, %llu deleted at once, %llu textures destroyed, %llu shader bindings "
        "checked, %llu destroyed textures caught (%llu since start, %llu of them freed memory already reused), %llu bindings "
        "with a not-loaded texture",
        static_cast<unsigned long long>(now[0] - last[0]), static_cast<unsigned long long>(now[1] - last[1]),
        static_cast<unsigned long long>(now[2] - last[2]), static_cast<unsigned long long>(now[4] - last[4]),
        static_cast<unsigned long long>(now[3] - last[3]), static_cast<unsigned long long>(now[3]),
        static_cast<unsigned long long>(g_reusedCaught.load()), static_cast<unsigned long long>(unloaded - lastUnloaded));
    lastUnloaded = unloaded;
    std::memcpy(last, now, sizeof(last));
}

void SelfTest() {
    if (!g_deadVt) {
        Log("TEXGUARD test: guard off (no I_TEXTURE vtable)");
        return;
    }
    // A fake engine SHADER_CUSTOM (renderer shader at +0x80) in the mod's own memory: slot 0 a destroyed texture whose
    // engine texture is loaded (KCOM type 0x884, flag bit 21, object +0x140 = a live texture), slot 1 a live texture,
    // slot 2 empty, slot 3 destroyed with no engine texture (gets the game's not-ready texture, or stays empty), slot 4
    // (0.24.0) freed memory already reused by another object (its first qword a heap pointer, as in the owner's crash).
    alignas(16) static uint8_t engine[0x700];
    std::memset(engine, 0, sizeof(engine));
    static uintptr_t dead[4], dead2[4], live[4], current[4], reused[4];
    const uintptr_t liveVt = g_liveVt[0] ? g_liveVt[0] : g_deadVt + 8;  // the real TEXTURE_DX11 vtable when known
    dead[0] = dead2[0] = g_deadVt;
    live[0] = current[0] = liveVt;
    reused[0] = reinterpret_cast<uintptr_t>(engine);  // a heap-like pointer, not a vtable
    alignas(16) static uint8_t handle[0x40], object[0x150];
    std::memset(handle, 0, sizeof(handle));
    std::memset(object, 0, sizeof(object));
    *reinterpret_cast<int32_t*>(handle) = 0x884;
    *reinterpret_cast<uint32_t*>(handle + 0x20) = 1u << 21;
    *reinterpret_cast<void**>(handle + 0x30) = object;
    *reinterpret_cast<void**>(object + 0x140) = current;
    static void* handles[5];
    handles[0] = handle;
    handles[1] = handles[2] = handles[3] = handles[4] = nullptr;
    *reinterpret_cast<void***>(engine + kEngineHandles) = handles;
    *(engine + kEngineHandleCount) = 5;
    static void* slots[5];
    slots[0] = dead;
    slots[1] = live;
    slots[2] = nullptr;
    slots[3] = dead2;
    slots[4] = reused;
    uint8_t* rs = engine + kEngineFromRenderer;
    *reinterpret_cast<uint16_t*>(rs + kSlotCount) = 5;
    *reinterpret_cast<void**>(rs + kSlots) = slots;
    uint32_t slot = 99;
    void* tex = nullptr;
    const char* how = "";
    const int c = RepairDestroyed(rs, -1, &slot, &tex, &how);
    const bool notDead3 = slots[3] != dead2 && (!slots[3] || IsLiveTexture(slots[3]));
    const bool gotNotReady3 = notDead3 && slots[3] != nullptr;
    // With the live classes known the reused slot must be repaired (empty or the not-ready texture, never the old one).
    const bool reusedOk = !g_liveVt[0] || (slots[4] != reused && (!slots[4] || IsLiveTexture(slots[4])));
    const int expected = g_liveVt[0] ? 3 : 2;
    const bool pass = c == expected && slot == 0 && tex == dead && slots[0] == current && slots[1] == live && !slots[2] &&
                      notDead3 && reusedOk;
    slots[4] = nullptr;
    // The skin path checks one slot only.
    slots[0] = dead;
    slots[3] = dead2;
    uint32_t s2 = 99;
    void* t2 = nullptr;
    const int c1 = RepairDestroyed(rs, 1, &s2, &t2, &how);
    const int c0 = RepairDestroyed(rs, 0, &s2, &t2, &how);
    const bool skinPass = c1 == 0 && c0 == 1 && s2 == 0 && slots[0] == current && slots[3] == dead2;
    // 0.21.6 refill: slot 3's engine texture is first not loaded (the repair gives the not-ready texture), then the game
    // loads it (a new texture at +0x140): the refill of a repaired shader puts it in; live and empty slots stay.
    alignas(16) static uint8_t handle3[0x40], object3[0x150];
    std::memset(handle3, 0, sizeof(handle3));
    std::memset(object3, 0, sizeof(object3));
    static uintptr_t reloaded[4];
    reloaded[0] = liveVt;
    *reinterpret_cast<int32_t*>(handle3) = 0x884;
    *reinterpret_cast<void**>(handle3 + 0x30) = object3;
    handles[3] = handle3;
    slots[3] = dead2;
    uint32_t s3 = 99;
    void* t3 = nullptr;
    RepairDestroyed(rs, 3, &s3, &t3, &how);
    const bool notLoadedRepair = slots[3] != dead2 && slots[3] != reloaded;
    uint32_t r0 = 99;
    const int before = RefillSlots(rs, &r0);  // still not loaded: nothing to restore except slot 0 (already current)
    *reinterpret_cast<uint32_t*>(handle3 + 0x20) = 1u << 21;
    *reinterpret_cast<void**>(object3 + 0x140) = reloaded;
    const int after = RefillSlots(rs, &r0);
    const bool refillPass = notLoadedRepair && before == 0 && after == 1 && r0 == 3 && slots[3] == reloaded &&
                            slots[0] == current && slots[1] == live && !slots[2];
    handles[3] = nullptr;
    Log("TEXGUARD test %s: all-slot check repaired %d (slot 0 %s, slot 3 %s, %s, reused memory %s), one-slot check %s, "
        "refill after the game loads the texture again %s (I_TEXTURE vtable %llx)",
        pass && skinPass && refillPass ? "PASS" : "FAIL", c,
        pass ? "got its engine texture's current one" : "WRONG",
        notDead3 ? (gotNotReady3 ? "got the not-ready texture" : "empty (no not-ready texture yet)") : "STILL DESTROYED",
        slots[1] == live ? "live texture kept" : "LIVE TEXTURE LOST",
        !g_liveVt[0] ? "not checked (live classes unknown)" : reusedOk ? "repaired" : "NOT CAUGHT", skinPass ? "ok" : "WRONG",
        refillPass ? "ok" : "WRONG", static_cast<unsigned long long>(g_deadVt - game::ModuleBase() + 0x140000000ull));
}

void DescribeForCrash(void (*add)(const char* line, void* ctx), void* ctx) {
    char line[900];
    snprintf(line, sizeof(line), "Destroyed textures caught by the guard: %llu (%llu freed and reused); releases %llu "
             "deferred, %llu at once, %llu textures destroyed", static_cast<unsigned long long>(g_caught.load()),
             static_cast<unsigned long long>(g_reusedCaught.load()),
             static_cast<unsigned long long>(g_queued.load()), static_cast<unsigned long long>(g_deletedNow.load()),
             static_cast<unsigned long long>(g_destroyed.load()));
    add(line, ctx);
    const uint32_t end = g_next.load(std::memory_order_acquire);
    for (uint32_t k = 0, shown = 0; k < kRing && k < end && shown < 6; ++k) {
        const Event& e = g_ring[(end - 1 - k) % kRing];
        void* obj = e.obj.load(std::memory_order_acquire);
        if (!obj || e.kind != kDestroyed) continue;
        DescribeEvent(e, obj, line, sizeof(line));
        add(line, ctx);
        ++shown;
    }
}

}  // namespace b2s::texguard
