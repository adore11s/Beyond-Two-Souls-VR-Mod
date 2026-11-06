#include "ctrlprobe.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "game.h"
#include "log.h"

namespace b2s::ctrlprobe {
namespace {

constexpr uint64_t kControllersVa = 0x141ca6d50;  // g_activeControllers: array +0, count +8 (uint32)

template <typename T>
bool Rd(const void* base, size_t off, T* out) {
    __try {
        std::memcpy(out, static_cast<const uint8_t*>(base) + off, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// KCOM handle -> object, cached handles only (no resolver side effects).
void* Cached(void* handle) {
    uint32_t flags = 0;
    void* obj = nullptr;
    if (!handle || reinterpret_cast<uintptr_t>(handle) < 0x10000 || !Rd(handle, 0x20, &flags) || !((flags >> 29) & 1) ||
        !Rd(handle, 0x30, &obj))
        return nullptr;
    return obj;
}

// RTTI class name of a polymorphic object, without the ".?AV" / "@SINT@QDT@@" decoration; cached per vtable.
const char* ClassName(void* obj) {
    struct Entry {
        uintptr_t vt;
        char name[64];
    };
    static Entry cache[128];
    static int used = 0;
    uintptr_t vt = 0;
    if (!obj || !Rd(obj, 0, &vt) || !vt) return "?";
    for (int i = 0; i < used; ++i)
        if (cache[i].vt == vt) return cache[i].name;
    char raw[96] = "";
    __try {
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* col = *reinterpret_cast<const uint32_t* const*>(vt - 8);
        const char* name = reinterpret_cast<const char*>(base + col[3] + 0x10);
        size_t i = 0;
        for (; i + 1 < sizeof(raw) && name[i]; ++i) raw[i] = name[i];
        raw[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        strcpy_s(raw, "?");
    }
    const char* s = std::strncmp(raw, ".?AV", 4) == 0 ? raw + 4 : raw;
    Entry* e = used < 128 ? &cache[used++] : &cache[127];
    e->vt = vt;
    size_t n = 0;
    for (; n + 1 < sizeof(e->name) && s[n] && s[n] != '@'; ++n) e->name[n] = s[n];
    e->name[n] = 0;
    return e->name;
}

}  // namespace

void Frame(void* character) {
    static void* lastCharacter = nullptr;
    static char last[1024] = "";
    static ULONGLONG lastLog = 0, windowStart = 0;
    static int windowLines = 0;
    if (!character) {
        lastCharacter = nullptr;
        return;
    }
    char sig[1024];
    int n = 0;
    // Her own fields.
    void* physics = nullptr;
    Rd(character, 0x508, &physics);
    void* kit = nullptr;
    uint8_t stopped = 0xff, running = 0xff;
    if (Rd(character, 0x340, &kit) && kit) {
        Rd(kit, 0x40, &stopped);
        void* rt = nullptr;
        if (Rd(kit, 0x58, &rt) && rt) Rd(rt, 0xd8, &running);
    }
    n += snprintf(sig + n, sizeof(sig) - n, "physics=%d motionkit=%s stopped=%d running=%d", physics ? 1 : 0,
                  kit ? "yes" : "none", stopped == 0xff ? -1 : int(stopped), running == 0xff ? -1 : int(running));
    void* player = nullptr;
    void* table = nullptr;
    uint32_t count = 0;
    if (Rd(character, 0x348, &player) && player && Rd(player, 0x18, &table) && table && Rd(table, 0x180, &count) &&
        count <= 16) {
        n += snprintf(sig + n, sizeof(sig) - n, " anims=%u [", count);
        for (uint32_t i = 0; i < count && n < 900; ++i) {
            uint32_t layer = 0;
            void* anim = nullptr;
            Rd(table, 0x10 + i * 0x18, &layer);
            Rd(table, 0x8 + i * 0x18, &anim);
            n += snprintf(sig + n, sizeof(sig) - n, "%sL%u:%s", i ? " " : "", layer, anim ? ClassName(anim) : "-");
        }
        n += snprintf(sig + n, sizeof(sig) - n, "]");
    } else {
        n += snprintf(sig + n, sizeof(sig) - n, " anims=?");
    }
    // Controllers the engine updates this frame that name her, and every sequence being played.
    void** arr = nullptr;
    uint32_t total = 0;
    const uintptr_t g = game::Va(kControllersVa);
    int onHer = 0, seqPlaying = 0, seqOnHer = 0;
    if (g && Rd(reinterpret_cast<void*>(g), 0, &arr) && Rd(reinterpret_cast<void*>(g), 8, &total) && arr && total < 4096) {
        n += snprintf(sig + n, sizeof(sig) - n, " | on her:");
        for (uint32_t i = 0; i < total; ++i) {
            void* c = nullptr;
            if (!Rd(arr, i * 8u, &c) || !c) continue;
            const char* cls = ClassName(c);
            void* h100 = nullptr;
            void* h108 = nullptr;
            Rd(c, 0x100, &h100);
            Rd(c, 0x108, &h108);
            const bool mine = Cached(h100) == character || Cached(h108) == character;
            if (std::strcmp(cls, "SEQUENCE_CONTROLLER_PLAY") == 0) {
                uint8_t playing = 0;
                Rd(c, 0x148, &playing);
                void* ih = nullptr;
                void* inst = Rd(c, 0x118, &ih) ? Cached(ih) : nullptr;
                void* target = nullptr;
                if (inst) Rd(inst, 0x188, &target);
                if (playing) ++seqPlaying;
                if (playing && (mine || target == character)) ++seqOnHer;
                continue;
            }
            if (mine && n < 900) {
                n += snprintf(sig + n, sizeof(sig) - n, " %s", cls);
                ++onHer;
            }
        }
        if (!onHer) n += snprintf(sig + n, sizeof(sig) - n, " none");
        n += snprintf(sig + n, sizeof(sig) - n, " | sequences playing %d (on her %d)", seqPlaying, seqOnHer);
    }
    const ULONGLONG now = GetTickCount64();
    const bool changed = character != lastCharacter || std::strcmp(sig, last) != 0;
    if (!changed && now - lastLog < 10000) return;
    if (now - windowStart >= 1000) {
        windowStart = now;
        windowLines = 0;
    }
    if (changed && ++windowLines > 8) return;  // a flickering field: at most 8 lines a second
    Log("CTRLPROBE %s%s", sig, changed ? "" : " (unchanged 10 s)");
    strcpy_s(last, sig);
    lastCharacter = character;
    lastLog = now;
}

}  // namespace b2s::ctrlprobe
