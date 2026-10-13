#include "zonefix.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "bones.h"
#include "fpcam.h"
#include "game.h"
#include "log.h"
#include "player.h"
#include "xr.h"

namespace b2s::zonefix {
namespace {

// STATIC: QDT::M3D::SCENE_ZONE vtable; ENTITY_ABC+0x98 = the entity's room (FUN_1408b2710 sets it,
// ENTITY_ABC::vfunc_133 FUN_1408b73a0 keeps it current for moving entities: cameras, meshes, lights).
// ENTITY_ABC+0x78 -> +0x30 = its SCENE (SCENE_ZONE::vfunc_110 FUN_14092bb10); SCENE+0x60 / +0x68 = the
// scene's entities (FUN_14088d700 walks them to clear a detached room); ENTITY_ABC+0xa0 = node handle,
// node world matrix at +0x80 (translation +0xb0).
// LIVE 0.16.36: a null room crashed the visibility pass (FUN_14092e260 reads room+0x118), so the
// cameras only ever get another valid room, never none.
constexpr uint64_t kSceneZoneVtableVa = 0x141710560;
constexpr uintptr_t kEntityZone = 0x98, kEntitySceneLink = 0x78, kEntityNode = 0xa0, kSceneEntities = 0x60,
                    kSceneEntityCount = 0x68, kNodeTranslation = 0xb0;
constexpr float kNearJodie = 0.8f;  // an entity of hers: its node within 0.8 m of her body node

std::atomic<bool> g_enabled{true};
uintptr_t g_zoneVt = 0;

bool ReadPtr(const void* p, uintptr_t off, void** out) {
    __try {
        *out = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(p) + off);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool WritePtr(void* p, uintptr_t off, void* v) {
    __try {
        *reinterpret_cast<void**>(static_cast<uint8_t*>(p) + off) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool IsZone(void* p) {
    void* vt = nullptr;
    return p && g_zoneVt && ReadPtr(p, 0, &vt) && reinterpret_cast<uintptr_t>(vt) == g_zoneVt;
}
// Node world position of an entity (its +0xa0 handle resolved), SEH-guarded.
bool EntityPosition(void* entity, float out[3]) {
    void* handle = nullptr;
    if (!ReadPtr(entity, kEntityNode, &handle) || !handle) return false;
    __try {
        void* node = game::ResolveHandle(handle);
        if (!node) return false;
        std::memcpy(out, static_cast<uint8_t*>(node) + kNodeTranslation, 12);
        return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
float Dist(const float a[3], const float b[3]) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

// Her room: an entity of the scene whose node is at her body (her meshes), read every frame from its +0x98.
void* g_jodieEntity = nullptr;
uint64_t g_lastSearch = 0;
void* g_lastCam = nullptr;

bool JodieBody(void* jodie, float out[3]) {
    float m[16];
    if (!player::BodyWorld(jodie, m)) return false;
    out[0] = m[12];
    out[1] = m[13];
    out[2] = m[14];
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

// Of her entities (within 0.8 m of her body node), the one nearest her head: the eyes go through a
// door before her body does (0.16.40).
void SearchEntity(const float body[3], const float head[3]) {
    g_jodieEntity = nullptr;
    void* link = nullptr;
    void* scene = nullptr;
    void* list = nullptr;
    void* countRaw = nullptr;
    if (!g_lastCam || !ReadPtr(g_lastCam, kEntitySceneLink, &link) || !link || !ReadPtr(link, 0x30, &scene) || !scene ||
        !ReadPtr(scene, kSceneEntities, &list) || !list || !ReadPtr(scene, kSceneEntityCount, &countRaw))
        return;
    const uint32_t count = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(countRaw));
    if (count == 0 || count > 50000) return;
    float best = 1e9f, bestBody = -1.f;
    uint32_t candidates = 0, rooms = 0;
    for (uint32_t i = 0; i < count; ++i) {
        void* e = nullptr;
        if (!ReadPtr(list, i * 8u, &e) || !e) continue;
        void* zone = nullptr;
        if (!ReadPtr(e, kEntityZone, &zone) || !IsZone(zone)) continue;
        ++rooms;
        float pos[3];
        if (!EntityPosition(e, pos)) continue;
        const float d = Dist(pos, body);
        if (d >= kNearJodie) continue;
        ++candidates;
        const float h = Dist(pos, head);
        if (h < best) {
            best = h;
            bestBody = d;
            g_jodieEntity = e;
        }
    }
    static int logs = 0;
    if (logs++ < 6) {
        void* zone = nullptr;
        if (g_jodieEntity) ReadPtr(g_jodieEntity, kEntityZone, &zone);
        Log("ZONE scene %p: %u entities, %u with a room, %u at Jodie's body; using %p (%.2f m from her head, %.2f m from "
            "her body, room %p)",
            scene, count, rooms, candidates, g_jodieEntity, g_jodieEntity ? best : -1.f, bestBody, zone);
    }
}

void* g_jodieZone = nullptr;
void* g_lastCamZone = nullptr;
uint64_t g_camZoneChangedAt = 0, g_jodieZoneChangedAt = 0;
void* g_prevJodieZone = nullptr;
// LIVE 0.16.39 (211.9 s): her room changed as she went through a door, the view went black at once and
// came back when the eye passes started from her room; the game camera followed into it 1.0 s later.
// So her room is used from the start (0.16.40: no wait for that proof, which left ~0.5 s of black per
// session); `g_tracks` only logs the first time the camera confirms it.
bool g_tracks = false;
bool g_firstPerson = false;
// When the view goes black in first person and her room differs from the camera's, her room is used
// until the game's camera changes room or 3 s pass.
bool g_rescue = false;
void* g_rescueCamZone = nullptr;
uint64_t g_rescueSince = 0;
int g_zoneLogs = 0;

struct Saved {
    void* cam;
    void* zone;
    void* written;
};
Saved g_saved[8];
int g_savedCount = 0;
uint64_t g_overrides = 0;
void* g_lastLoggedPair[2] = {};

}  // namespace

void Frame() {
    if (!g_zoneVt) {
        static bool tried = false;
        if (!tried) {
            tried = true;
            g_zoneVt = game::Va(kSceneZoneVtableVa);
            if (!g_zoneVt) Log("ZONE room vtable not found in this build: first-person room visibility off");
        }
        if (!g_zoneVt) return;
    }
    g_firstPerson = g_enabled.load(std::memory_order_relaxed) && fpcam::FirstPersonDrawn();
    if (!g_firstPerson) {
        g_jodieZone = nullptr;
        g_rescue = false;
        return;
    }
    const uint64_t now = GetTickCount64();
    const player::State st = player::Poll();
    g_jodieZone = nullptr;
    float body[3], headM[16];
    if (st.jodie && JodieBody(st.jodie, body) && player::HeadWorld(st.jodie, headM)) {
        const float head[3] = {headM[12], headM[13], headM[14]};
        float pos[3];
        void* zone = nullptr;
        const bool valid = g_jodieEntity && EntityPosition(g_jodieEntity, pos) && Dist(pos, body) < 1.5f &&
                           ReadPtr(g_jodieEntity, kEntityZone, &zone) && IsZone(zone);
        if (valid) {
            g_jodieZone = zone;
        } else if (now - g_lastSearch >= 2000) {
            g_lastSearch = now;
            SearchEntity(body, head);
            if (g_jodieEntity && ReadPtr(g_jodieEntity, kEntityZone, &zone) && IsZone(zone)) g_jodieZone = zone;
        }
    }
    if (g_jodieZone && g_jodieZone != g_prevJodieZone) {
        if (g_prevJodieZone) g_jodieZoneChangedAt = now;
        if (g_zoneLogs++ < 60)
            Log("ZONE Jodie's room %p -> %p (the game camera's room %p)", g_prevJodieZone, g_jodieZone, g_lastCamZone);
        g_prevJodieZone = g_jodieZone;
    }
    if (!g_tracks && g_jodieZoneChangedAt && g_camZoneChangedAt >= g_jodieZoneChangedAt &&
        g_camZoneChangedAt - g_jodieZoneChangedAt < 5000 && g_lastCamZone == g_jodieZone) {
        g_tracks = true;
        Log("ZONE the game camera followed Jodie into her new room %llu ms after her: her room is used for the eye "
            "from now on",
            static_cast<unsigned long long>(g_camZoneChangedAt - g_jodieZoneChangedAt));
    }
    if (!g_rescue && xr::ViewDark() && g_jodieZone && g_jodieZone != g_lastCamZone) {
        g_rescue = true;
        g_rescueCamZone = g_lastCamZone;
        g_rescueSince = now;
        Log("ZONE view black in first person: eye passes start from Jodie's room %p (the camera's is %p)", g_jodieZone,
            g_lastCamZone);
    } else if (g_rescue && (g_lastCamZone != g_rescueCamZone || now - g_rescueSince > 3000)) {
        g_rescue = false;
        Log("ZONE rescue ended after %llu ms (%s)", static_cast<unsigned long long>(now - g_rescueSince),
            g_lastCamZone != g_rescueCamZone ? "the camera changed room" : "3 s");
    }
}

void BeginPass(void* const* cams, int count) {
    g_savedCount = 0;
    if (!cams || count <= 0) return;
    g_lastCam = cams[0];
    if (!g_firstPerson) return;
    for (int i = 0; i < count && g_savedCount < static_cast<int>(std::size(g_saved)); ++i) {
        void* cam = cams[i];
        void* zone = nullptr;
        if (!cam || !ReadPtr(cam, kEntityZone, &zone) || !IsZone(zone)) continue;
        if (i == 0 && zone != g_lastCamZone) {
            if (g_zoneLogs++ < 60) Log("ZONE the game camera's room %p -> %p", g_lastCamZone, zone);
            g_lastCamZone = zone;
            g_camZoneChangedAt = GetTickCount64();
        }
        if (!g_jodieZone || g_jodieZone == zone || !IsZone(g_jodieZone)) continue;
        if (!WritePtr(cam, kEntityZone, g_jodieZone)) continue;
        g_saved[g_savedCount++] = {cam, zone, g_jodieZone};
        if (i == 0 && (g_lastLoggedPair[0] != zone || g_lastLoggedPair[1] != g_jodieZone)) {
            g_lastLoggedPair[0] = zone;
            g_lastLoggedPair[1] = g_jodieZone;
            if (g_overrides < 40)
                Log("ZONE eye passes start from Jodie's room %p (the game camera's room is %p)", g_jodieZone, zone);
        }
        ++g_overrides;
    }
}

void EndPass() {
    for (int i = 0; i < g_savedCount; ++i) {
        void* now = nullptr;
        // Put the game's room back unless the game changed it during the pass.
        if (ReadPtr(g_saved[i].cam, kEntityZone, &now) && now == g_saved[i].written)
            WritePtr(g_saved[i].cam, kEntityZone, g_saved[i].zone);
    }
    g_savedCount = 0;
}

bool Command(const char* verb, const char* arg) {
    if (std::strcmp(verb, "zonefix") != 0) return false;
    g_enabled = !(arg && (std::strcmp(arg, "off") == 0 || std::strcmp(arg, "0") == 0));
    Log("ZONE first-person room visibility: %s", g_enabled.load() ? "on" : "off");
    return true;
}

}  // namespace b2s::zonefix
