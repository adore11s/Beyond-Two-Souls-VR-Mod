#include "zonefix.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "fpcam.h"
#include "game.h"
#include "log.h"

namespace b2s::zonefix {
namespace {

// STATIC: QDT::M3D::SCENE_ZONE vtable; ENTITY_ABC+0x98 = the entity's room (FUN_1408b2710 sets it,
// ENTITY_ABC::vfunc_133 FUN_1408b73a0 keeps it current for moving entities: cameras, meshes, lights).
// ENTITY_ABC+0x78 -> +0x30 = its SCENE (FUN_1408b69c0 registers it there); SCENE+0x60 / +0x68 = the scene's
// entities; ENTITY_ABC+0xa0 = node handle, node world matrix at +0x80 (translation +0xb0).
// LIVE 0.16.36: a null room crashed the visibility pass (FUN_14092e260 reads room+0x118), so the cameras only ever get
// another valid room, never none.
constexpr uint64_t kSceneZoneVtableVa = 0x141710560;
constexpr uintptr_t kEntityZone = 0x98, kEntitySceneLink = 0x78, kEntityNode = 0xa0, kSceneEntities = 0x60,
                    kSceneEntityCount = 0x68, kNodeTranslation = 0xb0, kSceneRooms = 0x190;

// 0.24.0 (owner, 2026-11-05: in a first-person cutscene and in first-person walking "near the door only one room was
// rendering, everything else purple"; LIVE log: the game camera's room flipped between two rooms while her room was
// unknown, and the 0.21.x box rules picked none). STATIC, the engine's own answers instead of box distances:
// - FUN_14092e2b0(scene, point, hint) is the engine's point -> room lookup (FUN_140880120 uses it for a viewport whose
//   entity has no room): the hint room when its volume holds the point, else the first room of SCENE+0x190 (list +0x10,
//   count +0x18, layer mask room+0x58 & DAT_141b98820) whose volume (room+0x118 -> +0x28, FUN_1409dee30) holds it, else
//   the scene's default room (*(SCENE+0x190) -> +0x30).
// - The room query FUN_1409d9ca0 (viewport+0x8b8 = its camera) marks every room of the scene visible with the full
//   screen, no portal walk, while DAT_141c7901b is set (the engine sets it itself around some passes of
//   VIEWPORT::Display FUN_14086bc80 and VIEWPORT_LIST::Display FUN_14088a640). The query waits for its own jobs before
//   returning, so the flag is only needed for that call.
// The eye passes start from the room the engine puts the eye in (written into the stereo cameras' +0x98, as before).
// When the engine puts the eye in no room volume (the default room) while the game camera is in a room, the eye
// is in a gap between room volumes (a doorway): those eye passes use the all-rooms query instead of a guess.
constexpr uint64_t kRoomAtPointVa = 0x14092e2b0;
constexpr uint64_t kRoomQueryVa = 0x1409d9ca0;
constexpr uint64_t kAllRoomsFlagVa = 0x141c7901b;
constexpr uintptr_t kViewportCamera = 0x8b8;

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

// STATIC (0.17.1): FUN_14092e260(room) returns the room's box: room+0x118 -> +0x28 -> +8 -> min.xyzw, max.xyzw. For the
// `zones` dev listing only (the eye's room comes from the engine's own volume test).
bool ZoneBox(void* zone, float mn[3], float mx[3]) {
    void* a = nullptr;
    void* b = nullptr;
    void* c = nullptr;
    if (!ReadPtr(zone, 0x118, &a) || !a || !ReadPtr(a, 0x28, &b) || !b || !ReadPtr(b, 8, &c) || !c) return false;
    float v[8];
    __try {
        std::memcpy(v, c, sizeof(v));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    for (int i = 0; i < 3; ++i) {
        mn[i] = v[i];
        mx[i] = v[4 + i];
        if (!std::isfinite(mn[i]) || !std::isfinite(mx[i]) || mn[i] > mx[i] || mx[i] - mn[i] > 1e4f) return false;
    }
    return true;
}
bool Inside(const float p[3], const float mn[3], const float mx[3]) {
    return p[0] >= mn[0] && p[0] <= mx[0] && p[1] >= mn[1] && p[1] <= mx[1] && p[2] >= mn[2] && p[2] <= mx[2];
}

void* SceneOf(void* cam) {
    void* link = nullptr;
    void* scene = nullptr;
    if (!cam || !ReadPtr(cam, kEntitySceneLink, &link) || !link || !ReadPtr(link, 0x30, &scene)) return nullptr;
    return scene;
}
// *(SCENE+0x190) -> +0x30: what FUN_14092e2b0 returns when no room volume holds the point.
void* DefaultRoom(void* scene) {
    void* rooms = nullptr;
    void* first = nullptr;
    void* room = nullptr;
    if (!scene || !ReadPtr(scene, kSceneRooms, &rooms) || !rooms || !ReadPtr(rooms, 0, &first) || !first ||
        !ReadPtr(first, 0x30, &room))
        return nullptr;
    return room;
}

using RoomAtFn = void* (*)(void* scene, const float* point, void* hint);
RoomAtFn g_roomAt = nullptr;
void* EngineRoomAt(void* scene, const float eye[3], void* hint) {
    alignas(16) float p[4] = {eye[0], eye[1], eye[2], 1.f};
    __try {
        return g_roomAt(scene, p, hint);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// The all-rooms query for the eye passes.
using RoomQueryFn = DWORD (*)(void* a1, void* scene, void* query, void* viewport);
RoomQueryFn g_roomQueryOriginal = nullptr;
volatile uint8_t* g_allRoomsFlag = nullptr;
std::atomic<bool> g_passAllRooms{false};
std::atomic<void*> g_passCams[8];
std::atomic<int> g_passCamCount{0};
std::atomic<uint64_t> g_allRoomQueries{0}, g_eyeQueries{0};

bool ViewportOfPassCam(void* viewport) {
    void* handle = nullptr;
    if (!viewport || !ReadPtr(viewport, kViewportCamera, &handle) || !handle) return false;
    void* cam = nullptr;
    __try {
        cam = game::ResolveHandle(handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    const int n = g_passCamCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (cam && g_passCams[i].load(std::memory_order_relaxed) == cam) return true;
    return false;
}

DWORD HookRoomQuery(void* a1, void* scene, void* query, void* viewport) {
    if (g_passAllRooms.load(std::memory_order_acquire) && g_allRoomsFlag && ViewportOfPassCam(viewport)) {
        g_eyeQueries.fetch_add(1, std::memory_order_relaxed);
        const uint8_t before = *g_allRoomsFlag;
        *g_allRoomsFlag = 1;
        const DWORD r = g_roomQueryOriginal(a1, scene, query, viewport);
        *g_allRoomsFlag = before;
        g_allRoomQueries.fetch_add(1, std::memory_order_relaxed);
        return r;
    }
    if (g_passCamCount.load(std::memory_order_relaxed) && ViewportOfPassCam(viewport))
        g_eyeQueries.fetch_add(1, std::memory_order_relaxed);
    return g_roomQueryOriginal(a1, scene, query, viewport);
}

// Rooms of the scene's entities (for the `zones` dev listing).
void* g_knownZones[64] = {};
int g_knownCount = 0;
void* g_knownScene = nullptr;
void RefreshKnownZones(void* cam) {
    void* scene = SceneOf(cam);
    if (scene != g_knownScene) {
        g_knownScene = scene;
        g_knownCount = 0;
    }
    void* list = nullptr;
    void* countRaw = nullptr;
    if (!scene || !ReadPtr(scene, kSceneEntities, &list) || !list || !ReadPtr(scene, kSceneEntityCount, &countRaw)) return;
    const uint32_t count = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(countRaw));
    if (count == 0 || count > 50000) return;
    for (uint32_t i = 0; i < count; ++i) {
        void* e = nullptr;
        void* zone = nullptr;
        if (!ReadPtr(list, i * 8u, &e) || !e || !ReadPtr(e, kEntityZone, &zone) || !IsZone(zone)) continue;
        bool seen = false;
        for (int k = 0; k < g_knownCount && !seen; ++k) seen = g_knownZones[k] == zone;
        if (!seen && g_knownCount < static_cast<int>(std::size(g_knownZones))) g_knownZones[g_knownCount++] = zone;
    }
}

void* g_eyeZone = nullptr;   // the room the eye passes start from (the engine's room for the eye), null = the camera's
bool g_eyeAllRooms = false;  // the engine puts the eye in no room volume while the camera is in a room
void* g_engineEyeRoom = nullptr;
void* g_lastCamZone = nullptr;
void* g_lastCam = nullptr;
bool g_firstPerson = false;
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

void Install() {
    g_zoneVt = game::Va(kSceneZoneVtableVa);
    g_roomAt = reinterpret_cast<RoomAtFn>(game::Va(kRoomAtPointVa));
    g_allRoomsFlag = reinterpret_cast<volatile uint8_t*>(game::Va(kAllRoomsFlagVa));
    void* query = reinterpret_cast<void*>(game::Va(kRoomQueryVa));
    bool hooked = false;
    if (query && g_allRoomsFlag) {
        MH_STATUS s = MH_CreateHook(query, reinterpret_cast<void*>(&HookRoomQuery), reinterpret_cast<void**>(&g_roomQueryOriginal));
        if (s == MH_OK) s = MH_EnableHook(query);
        hooked = s == MH_OK;
        Log("hook %-24s %p %s", "VISIBILITY::RoomQuery", query, MH_StatusToString(s));
    }
    Log("ZONE first-person rooms (0.24.0): room of the eye from the engine's point lookup %s; all-rooms query for an eye "
        "between room volumes %s",
        g_zoneVt && g_roomAt ? "on" : "OFF (not found in this build)", hooked ? "on" : "OFF");
}

void Frame() {
    if (!g_zoneVt) return;
    g_firstPerson = g_enabled.load(std::memory_order_relaxed) && fpcam::FirstPersonDrawn();
    if (!g_firstPerson) {
        g_eyeZone = nullptr;
        g_eyeAllRooms = false;
        g_engineEyeRoom = nullptr;
        return;
    }
    float eye[3];
    void* scene = SceneOf(g_lastCam);
    if (!g_roomAt || !scene || !fpcam::EyeWorld(eye)) {
        // No engine lookup possible: if the query hook is there, the eye passes see every room (nothing missing).
        g_eyeZone = nullptr;
        g_eyeAllRooms = g_roomQueryOriginal != nullptr;
        return;
    }
    void* hint = IsZone(g_engineEyeRoom) ? g_engineEyeRoom : g_lastCamZone;
    void* room = EngineRoomAt(scene, eye, hint);
    void* fallback = DefaultRoom(scene);
    const bool valid = IsZone(room);
    const bool inGap = !valid || (room == fallback && g_lastCamZone && g_lastCamZone != fallback);
    void* before = g_eyeZone;
    const bool allBefore = g_eyeAllRooms;
    g_engineEyeRoom = valid && room != fallback ? room : nullptr;
    g_eyeZone = valid && !inGap ? room : nullptr;
    g_eyeAllRooms = inGap && g_roomQueryOriginal != nullptr;
    if ((g_eyeZone != before || g_eyeAllRooms != allBefore) && g_zoneLogs++ < 200) {
        if (g_eyeAllRooms)
            Log("ZONE the engine puts the eye (%.2f %.2f %.2f) in no room volume (default room %p; the game camera's room "
                "%p): eye passes see every room", eye[0], eye[1], eye[2], fallback, g_lastCamZone);
        else
            Log("ZONE the engine puts the eye (%.2f %.2f %.2f) in room %p%s (the game camera's room %p)", eye[0], eye[1],
                eye[2], g_eyeZone, g_eyeZone == fallback ? " = the scene's default room" : "", g_lastCamZone);
    }
}

void BeginPass(void* const* cams, int count) {
    g_savedCount = 0;
    g_passAllRooms.store(false, std::memory_order_release);
    g_passCamCount.store(0, std::memory_order_release);
    if (!cams || count <= 0) return;
    g_lastCam = cams[0];
    if (!g_firstPerson) return;
    int n = 0;
    for (int i = 0; i < count && n < static_cast<int>(std::size(g_passCams)); ++i)
        if (cams[i]) g_passCams[n++].store(cams[i], std::memory_order_relaxed);
    g_passCamCount.store(n, std::memory_order_release);
    for (int i = 0; i < count && g_savedCount < static_cast<int>(std::size(g_saved)); ++i) {
        void* cam = cams[i];
        void* zone = nullptr;
        if (!cam || !ReadPtr(cam, kEntityZone, &zone) || !IsZone(zone)) continue;
        if (i == 0 && zone != g_lastCamZone) {
            if (g_zoneLogs++ < 200) Log("ZONE the game camera's room %p -> %p", g_lastCamZone, zone);
            g_lastCamZone = zone;
        }
        if (!g_eyeZone || g_eyeZone == zone || !IsZone(g_eyeZone)) continue;
        if (!WritePtr(cam, kEntityZone, g_eyeZone)) continue;
        g_saved[g_savedCount++] = {cam, zone, g_eyeZone};
        if (i == 0 && (g_lastLoggedPair[0] != zone || g_lastLoggedPair[1] != g_eyeZone)) {
            g_lastLoggedPair[0] = zone;
            g_lastLoggedPair[1] = g_eyeZone;
            if (g_overrides < 200)
                Log("ZONE eye passes start from the eye's room %p (the game camera's room is %p)", g_eyeZone, zone);
        }
        ++g_overrides;
    }
    g_passAllRooms.store(g_eyeAllRooms, std::memory_order_release);
}

void EndPass() {
    g_passAllRooms.store(false, std::memory_order_release);
    g_passCamCount.store(0, std::memory_order_release);
    for (int i = 0; i < g_savedCount; ++i) {
        void* now = nullptr;
        // Put the game's room back unless the game changed it during the pass.
        if (ReadPtr(g_saved[i].cam, kEntityZone, &now) && now == g_saved[i].written)
            WritePtr(g_saved[i].cam, kEntityZone, g_saved[i].zone);
    }
    g_savedCount = 0;
}

bool EyeRoomDiffers() { return g_firstPerson && g_eyeZone && g_eyeZone != g_lastCamZone; }

void LogStats() {
    static uint64_t last[2] = {};
    const uint64_t now[2] = {g_allRoomQueries.load(), g_eyeQueries.load()};
    if (now[0] != last[0])
        Log("ZONE 30 s: %llu of %llu eye-pass room queries saw every room (eye between room volumes)",
            static_cast<unsigned long long>(now[0] - last[0]), static_cast<unsigned long long>(now[1] - last[1]));
    last[0] = now[0];
    last[1] = now[1];
}

bool Command(const char* verb, const char* arg) {
    if (std::strcmp(verb, "zones") == 0) {
        // The game camera's room and every room of the scene's entities, with their boxes (EXP-042), and where the
        // engine puts the eye.
        float cam[3] = {};
        const bool camPos = g_lastCam && EntityPosition(g_lastCam, cam);
        void* camZone = nullptr;
        if (g_lastCam) ReadPtr(g_lastCam, kEntityZone, &camZone);
        RefreshKnownZones(g_lastCam);
        float eye[3] = {};
        void* scene = SceneOf(g_lastCam);
        void* eyeRoom = g_roomAt && scene && fpcam::EyeWorld(eye) ? EngineRoomAt(scene, eye, camZone) : nullptr;
        Log("ZONES camera %p at (%.2f %.2f %.2f)%s, its room %p, %d rooms known; engine room of the eye (%.2f %.2f %.2f): "
            "%p (default room %p)",
            g_lastCam, cam[0], cam[1], cam[2], camPos ? "" : " (no position)", camZone, g_knownCount, eye[0], eye[1],
            eye[2], eyeRoom, DefaultRoom(scene));
        for (int k = 0; k < g_knownCount; ++k) {
            float mn[3], mx[3];
            const bool box = IsZone(g_knownZones[k]) && ZoneBox(g_knownZones[k], mn, mx);
            if (!box)
                Log("ZONES  room %p: no box", g_knownZones[k]);
            else
                Log("ZONES  room %p: (%.2f %.2f %.2f)-(%.2f %.2f %.2f)%s%s", g_knownZones[k], mn[0], mn[1], mn[2], mx[0],
                    mx[1], mx[2], camPos && Inside(cam, mn, mx) ? " holds the camera" : "",
                    g_knownZones[k] == camZone ? " [camera's room]" : "");
        }
        return true;
    }
    if (std::strcmp(verb, "zonefix") != 0) return false;
    g_enabled = !(arg && (std::strcmp(arg, "off") == 0 || std::strcmp(arg, "0") == 0));
    Log("ZONE first-person room visibility: %s", g_enabled.load() ? "on" : "off");
    return true;
}

}  // namespace b2s::zonefix
