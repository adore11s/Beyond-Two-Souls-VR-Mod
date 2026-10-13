#include "camdirector.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include "game.h"
#include "log.h"

namespace b2s::camdirector {
namespace {

constexpr uintptr_t kDirectorUpdateVa = 0x140f4c110;  // ADDR-032 CAMERA_DIRECTOR::vfunc_110
constexpr uint8_t kDirectorUpdateGuard[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74,
                                            0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48};
constexpr uintptr_t kDefaultBehaviourVa = 0x141c9b308;  // default behaviour handle (empty stack)

struct ClassDef {
    uintptr_t vtable;
    const char* name;
    fp::BehaviourClass group;
};
using G = fp::BehaviourClass;
constexpr ClassDef kClasses[] = {
    {0x14174eff0, "FIXED", G::Gameplay},          {0x14174f3c8, "CHASE", G::Gameplay},
    {0x14174f7a8, "STANDALONE_CHASE", G::Gameplay}, {0x141752e30, "SHOT", G::Cutscene},
    {0x1417531f8, "RAIL", G::Gameplay},           {0x141754778, "FREE", G::Gameplay},
    {0x141758a70, "FREE_GAMEPLAY", G::Gameplay},  {0x14176f5b0, "FIRST_PERSON", G::NativeFirstPerson},
    {0x14176f978, "CIRCLE", G::Gameplay},         {0x141770c10, "FREE_ROAMING", G::Gameplay},
    {0x141773760, "CONTAINER", G::Gameplay},      {0x1417752c8, "SELECTOR", G::Cutscene},
    {0x141775a78, "INSERTCAM_SELECTOR", G::Cutscene}, {0x14177a598, "CROWD_CHASE", G::Gameplay},
    {0x141782b10, "SWIM", G::Gameplay},           {0x141788bc8, "ANAEL", G::NativeFirstPerson},
    {0x14178d9f0, "BIKE", G::Gameplay},           {0x14178fe48, "FOLLOW", G::Gameplay},
    {0x141791c38, "SEQUENCE", G::Cutscene},       {0x141795ab0, "FOLLOW_RAIL", G::Gameplay},
    {0x14179e750, "FOLLOW_VEHICLE", G::Gameplay},
};

using UpdateFn = void (*)(void* director, void* arg);
UpdateFn g_origUpdate = nullptr;
std::atomic<void*> g_director{nullptr};

// Camera filters. STATIC: CAMERA_MODIFIER_CAMERA_SHADER::vfunc_110 (0x141034190) registers the
// modifier with its behaviour's shader list (FUN_14121b9f0: the viewport camera's shader +0x3d0 is
// saved and replaced) and vfunc_111 (0x1410340d0) removes it again (FUN_14121bb30). Owner 0.16.12:
// in the DPA rooms the game cuts to a security-camera view with a filter while Jodie stands still.
constexpr uintptr_t kShaderModifierVt = 0x141770850;  // QDT::SINT::CAMERA_MODIFIER_CAMERA_SHADER
constexpr uintptr_t kShaderActivateVa = 0x141034190;
constexpr uintptr_t kShaderDeactivateVa = 0x1410340d0;
constexpr size_t kModifierOwner = 0x98;  // the behaviour the modifier belongs to (FUN_140f492e0 arg)
using ModifierFn = void (*)(void* self, void* a, void* b, void* c);
ModifierFn g_origActivate = nullptr, g_origDeactivate = nullptr;
std::mutex g_filterMutex;
void* g_filters[8]{};

void LogFilter(const char* what, void* modifier) {
    char name[96];
    game::ObjectName(modifier, name, sizeof(name));
    void* owner = nullptr;
    __try { owner = *reinterpret_cast<void**>(static_cast<uint8_t*>(modifier) + kModifierOwner); }
    __except (EXCEPTION_EXECUTE_HANDLER) { owner = nullptr; }
    char ownerName[96];
    game::ObjectName(owner, ownerName, sizeof(ownerName));
    void* active = Active();
    Log("CAMDIR camera filter %s: %s (%p) on %s %s; active camera %s / %s", what, name[0] ? name : "?", modifier,
        Name(owner), ownerName[0] ? ownerName : "?", Name(active), Name(Child(active)));
}
void HookActivate(void* self, void* a, void* b, void* c) {
    g_origActivate(self, a, b, c);
    {
        std::lock_guard lock(g_filterMutex);
        void** free = nullptr;
        bool known = false;
        for (void*& f : g_filters) {
            known |= f == self;
            if (!f && !free) free = &f;
        }
        if (!known && free) *free = self;
    }
    LogFilter("on", self);
}
void HookDeactivate(void* self, void* a, void* b, void* c) {
    {
        std::lock_guard lock(g_filterMutex);
        for (void*& f : g_filters)
            if (f == self) f = nullptr;
    }
    LogFilter("off", self);
    g_origDeactivate(self, a, b, c);
}
// Diagnostic (owner 0.16.28: "in F3 / F1, going through a door, the whole view goes black for a second";
// LIVE 0.16.28: pure black (0.0%) for 1031 ms at 156.8 s in F1, no frame stall). STATIC:
// VIEWPORT_CONTROLLER_FADE::vfunc_124 (0x140ff2330) is the per-frame fade update: elapsed +0x11c,
// duration +0x110, colour blended from the bytes at +0x108..+0x10b to +0x10c..+0x10f, applied to the
// viewport at +0x120. Each fade is logged once when it starts (elapsed restarts).
constexpr uintptr_t kFadeUpdateVa = 0x140ff2330;
using FadeUpdateFn = void (*)(void* self, float dt);
FadeUpdateFn g_origFadeUpdate = nullptr;
void* g_lastFade = nullptr;
float g_lastFadeElapsed = 0.f;
void ReadFade(const uint8_t* f, float* duration, float* elapsed, uint8_t col[8]) {
    __try {
        *duration = *reinterpret_cast<const float*>(f + 0x110);
        *elapsed = *reinterpret_cast<const float*>(f + 0x11c);
        std::memcpy(col, f + 0x108, 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *duration = *elapsed = -1.f;
    }
}
void HookFadeUpdate(void* self, float dt) {
    float duration = 0.f, elapsed = 0.f;
    uint8_t col[8]{};
    ReadFade(static_cast<const uint8_t*>(self), &duration, &elapsed, col);
    if (self != g_lastFade || elapsed < g_lastFadeElapsed) {
        char name[96];
        game::ObjectName(self, name, sizeof(name));
        Log("VIEWPORT fade %s (%p): %.2f s, colour %02x%02x%02x%02x -> %02x%02x%02x%02x", name[0] ? name : "?", self,
            duration, col[0], col[1], col[2], col[3], col[4], col[5], col[6], col[7]);
    }
    g_lastFade = self;
    g_lastFadeElapsed = elapsed;
    g_origFadeUpdate(self, dt);
}

// VIEWPORT_CONTROLLER_FADE_OPACITY::vfunc_124 (0x140ff36c0): the viewport's opacity fade, elapsed +0x118,
// duration +0x110. Logged once per start as well.
constexpr uintptr_t kOpacityFadeUpdateVa = 0x140ff36c0;
FadeUpdateFn g_origOpacityFadeUpdate = nullptr;
void* g_lastOpacityFade = nullptr;
float g_lastOpacityElapsed = 0.f;
void HookOpacityFadeUpdate(void* self, float dt) {
    float duration = -1.f, elapsed = -1.f;
    __try {
        duration = *reinterpret_cast<const float*>(static_cast<const uint8_t*>(self) + 0x110);
        elapsed = *reinterpret_cast<const float*>(static_cast<const uint8_t*>(self) + 0x118);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (self != g_lastOpacityFade || elapsed < g_lastOpacityElapsed) {
        char name[96];
        game::ObjectName(self, name, sizeof(name));
        Log("VIEWPORT opacity fade %s (%p): %.2f s", name[0] ? name : "?", self, duration);
    }
    g_lastOpacityFade = self;
    g_lastOpacityElapsed = elapsed;
    g_origOpacityFadeUpdate(self, dt);
}

bool IsLiveModifier(void* m) {
    __try { return m && *static_cast<uintptr_t*>(m) == game::Va(kShaderModifierVt); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void HookUpdate(void* director, void* arg) {
    g_director = director;
    g_origUpdate(director, arg);
}

void* Resolve(void* handle) { return game::ResolveHandle(handle); }

const ClassDef* Find(void* behaviour) {
    if (!behaviour) return nullptr;
    const auto vt = reinterpret_cast<uintptr_t>(*static_cast<void**>(behaviour));
    for (const ClassDef& c : kClasses)
        if (vt == game::Va(c.vtable)) return &c;
    return nullptr;
}

}  // namespace

bool Install() {
    auto* p = reinterpret_cast<uint8_t*>(game::Va(kDirectorUpdateVa));
    if (!game::GuardAt(reinterpret_cast<uintptr_t>(p), kDirectorUpdateGuard, sizeof(kDirectorUpdateGuard))) {
        Log("CAMDIR guard MISMATCH; first person disabled");
        return false;
    }
    if (MH_CreateHook(p, reinterpret_cast<void*>(&HookUpdate), reinterpret_cast<void**>(&g_origUpdate)) != MH_OK ||
        MH_EnableHook(p) != MH_OK) {
        Log("CAMDIR hook failed");
        return false;
    }
    Log("CAMDIR hooked CAMERA_DIRECTOR update at %p", p);
    auto* on = reinterpret_cast<void*>(game::Va(kShaderActivateVa));
    auto* off = reinterpret_cast<void*>(game::Va(kShaderDeactivateVa));
    const bool filters = on && off && game::Va(kShaderModifierVt) &&
                         MH_CreateHook(on, reinterpret_cast<void*>(&HookActivate), reinterpret_cast<void**>(&g_origActivate)) == MH_OK &&
                         MH_CreateHook(off, reinterpret_cast<void*>(&HookDeactivate), reinterpret_cast<void**>(&g_origDeactivate)) == MH_OK &&
                         MH_EnableHook(on) == MH_OK && MH_EnableHook(off) == MH_OK;
    Log("CAMDIR camera filter hooks: %s", filters ? "installed" : "unavailable (F1 is not paused for camera filters)");
    if (void* fade = reinterpret_cast<void*>(game::Va(kFadeUpdateVa))) {
        const bool fadeOk = MH_CreateHook(fade, reinterpret_cast<void*>(&HookFadeUpdate), reinterpret_cast<void**>(&g_origFadeUpdate)) == MH_OK &&
                            MH_EnableHook(fade) == MH_OK;
        Log("CAMDIR viewport fade logging: %s", fadeOk ? "on" : "FAILED");
    }
    if (void* fade = reinterpret_cast<void*>(game::Va(kOpacityFadeUpdateVa))) {
        const bool fadeOk = MH_CreateHook(fade, reinterpret_cast<void*>(&HookOpacityFadeUpdate),
                                          reinterpret_cast<void**>(&g_origOpacityFadeUpdate)) == MH_OK &&
                            MH_EnableHook(fade) == MH_OK;
        Log("CAMDIR viewport opacity fade logging: %s", fadeOk ? "on" : "FAILED");
    }
    return true;
}

bool FilterActive() {
    std::lock_guard lock(g_filterMutex);
    bool any = false;
    for (void*& f : g_filters) {
        if (f && !IsLiveModifier(f)) f = nullptr;  // freed without being switched off
        any |= f != nullptr;
    }
    return any;
}

void* Active() {
    auto* dir = static_cast<uint8_t*>(g_director.load());
    if (!dir) return nullptr;
    void* handle = nullptr;
    const uint32_t count = game::Read<uint32_t>(dir, 0xF0);
    if (count) {
        auto** stack = game::Read<void**>(dir, 0xE0);
        if (stack) handle = stack[0];
    }
    if (!handle) handle = *reinterpret_cast<void**>(game::Va(kDefaultBehaviourVa));
    return Resolve(handle);
}

fp::BehaviourClass Classify(void* behaviour) {
    const ClassDef* c = Find(behaviour);
    return c ? c->group : fp::BehaviourClass::Unknown;
}

const char* Name(void* behaviour) {
    const ClassDef* c = Find(behaviour);
    return c ? c->name : "?";
}

void* Child(void* behaviour) { return behaviour ? Resolve(game::Read<void*>(behaviour, 0x588)) : nullptr; }

}  // namespace b2s::camdirector
