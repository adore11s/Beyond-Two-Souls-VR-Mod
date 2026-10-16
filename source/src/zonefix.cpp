#include "zonefix.h"

#include <windows.h>

#include <algorithm>
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

// STATIC (0.17.1): FUN_14092e260(room) returns the room's box: room+0x118 -> +0x28 -> +8 -> min.xyzw,
// max.xyzw (FLT_MAX / -FLT_MAX when there is none). Read SEH-guarded; false without a usable box.
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

// Rooms of the scene's entities (refreshed every 0.5 s): the rooms around her that the eye can step into.
void* g_knownZones[64] = {};
int g_knownCount = 0;
void* g_knownScene = nullptr;
uint64_t g_knownAt = 0;

void* SceneOf(void* cam) {
    void* link = nullptr;
    void* scene = nullptr;
    if (!cam || !ReadPtr(cam, kEntitySceneLink, &link) || !link || !ReadPtr(link, 0x30, &scene)) return nullptr;
    return scene;
}

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

// How far p is outside the box (0 inside).
float Outside(const float p[3], const float mn[3], const float mx[3]) {
    float d = 0.f;
    for (int i = 0; i < 3; ++i) d = std::max(d, std::max(mn[i] - p[i], p[i] - mx[i]));
    return d;
}

// The room the eye is in: hers while the eye is inside her room's box; once the eye has just left it (head
// through a door before the game moves her, LIVE 0.17.0: ~50 ms of black in the demo, 160.61 s), the
// smallest known room box holding the eye. Only when her body is inside her room's box (the box describes
// her room) and the eye is at most 0.6 m outside it (a head through a doorway, not a box that misses part
// of the room).
void* EyeZone(void* jodieZone, const float eye[3], const float body[3], bool* left) {
    *left = false;
    float mn[3], mx[3];
    if (!jodieZone || !ZoneBox(jodieZone, mn, mx) || Inside(eye, mn, mx) || !Inside(body, mn, mx) ||
        Outside(eye, mn, mx) > 0.6f)
        return jodieZone;
    *left = true;
    void* best = nullptr;
    float bestVolume = 3.4e38f;
    for (int k = 0; k < g_knownCount; ++k) {
        void* z = g_knownZones[k];
        if (z == jodieZone || !IsZone(z) || !ZoneBox(z, mn, mx) || !Inside(eye, mn, mx)) continue;
        const float volume = (mx[0] - mn[0]) * (mx[1] - mn[1]) * (mx[2] - mn[2]);
        if (volume < bestVolume) {
            bestVolume = volume;
            best = z;
        }
    }
    return best ? best : jodieZone;
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
void* g_eyeZone = nullptr;      // the room the eye passes start from (hers, or the one the eye stepped into)
void* g_lastEyeZone = nullptr;
bool g_eyeLeft = false;
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
        g_eyeZone = nullptr;
        g_rescue = false;
        return;
    }
    const uint64_t now = GetTickCount64();
    const player::State st = player::Poll();
    g_jodieZone = nullptr;
    float body[3] = {}, headM[16];
    const bool haveBody = st.jodie && JodieBody(st.jodie, body);
    if (haveBody && player::HeadWorld(st.jodie, headM)) {
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
        if (g_zoneLogs++ < 60) {
            float mn[3] = {}, mx[3] = {}, eye[3] = {};
            const bool box = ZoneBox(g_jodieZone, mn, mx), hasEye = fpcam::EyeWorld(eye);
            Log("ZONE Jodie's room %p -> %p (the game camera's room %p); box %s (%.2f %.2f %.2f)-(%.2f %.2f %.2f), eye "
                "(%.2f %.2f %.2f) %s",
                g_prevJodieZone, g_jodieZone, g_lastCamZone, box ? "" : "none", mn[0], mn[1], mn[2], mx[0], mx[1], mx[2],
                eye[0], eye[1], eye[2], !hasEye ? "unknown" : box && Inside(eye, mn, mx) ? "inside" : "outside");
        }
        g_prevJodieZone = g_jodieZone;
    }
    // The room the eye is in (0.17.1): her room, or the room the eye has stepped into ahead of her.
    if (now - g_knownAt >= 500) {
        g_knownAt = now;
        RefreshKnownZones(g_lastCam);
    }
    g_eyeZone = g_jodieZone;
    float eye[3];
    if (g_jodieZone && haveBody && fpcam::EyeWorld(eye)) {
        bool left = false;
        g_eyeZone = EyeZone(g_jodieZone, eye, body, &left);
        if (left != g_eyeLeft || (left && g_eyeZone != g_lastEyeZone)) {
            if (g_zoneLogs++ < 60) {
                if (left)
                    Log("ZONE the eye left Jodie's room %p at (%.2f %.2f %.2f): %s %p (%d rooms known)", g_jodieZone, eye[0],
                        eye[1], eye[2], g_eyeZone != g_jodieZone ? "eye passes start from room" : "no known room holds it, still",
                        g_eyeZone, g_knownCount);
                else
                    Log("ZONE the eye is back in Jodie's room %p", g_jodieZone);
            }
            g_eyeLeft = left;
            g_lastEyeZone = g_eyeZone;
        }
    }
    if (!g_tracks && g_jodieZoneChangedAt && g_camZoneChangedAt >= g_jodieZoneChangedAt &&
        g_camZoneChangedAt - g_jodieZoneChangedAt < 5000 && g_lastCamZone == g_jodieZone) {
        g_tracks = true;
        Log("ZONE the game camera followed Jodie into her new room %llu ms after her: her room is used for the eye "
            "from now on",
            static_cast<unsigned long long>(g_camZoneChangedAt - g_jodieZoneChangedAt));
    }
    if (!g_rescue && xr::ViewDark() && g_eyeZone && g_eyeZone != g_lastCamZone) {
        g_rescue = true;
        g_rescueCamZone = g_lastCamZone;
        g_rescueSince = now;
        Log("ZONE view black in first person: eye passes start from room %p (the camera's is %p)", g_eyeZone,
            g_lastCamZone);
    } else if (g_rescue && (g_lastCamZone != g_rescueCamZone || now - g_rescueSince > 3000)) {
        g_rescue = false;
        Log("ZONE rescue ended after %llu ms (%s)", static_cast<unsigned long long>(now - g_rescueSince),
            g_lastCamZone != g_rescueCamZone ? "the camera changed room" : "3 s");
    }
}

// Diagnostic (0.17.3, "The Party": the right eye lost objects in some shots): the scene camera's room at
// the start of each eye pass; a frame whose right-eye pass starts from another room than its left-eye
// pass is logged (first 30, then counted).
void* g_passRoom[2] = {};
uint64_t g_roomSplits = 0, g_roomSplitReported = 0;

// 0.17.8 (owner, "The Party", F1 at the enterable house: "looking down renders nothing, plain blue; looking up
// most things but not the door, sometimes no shadows"). In F1 the eye is at Jodie's head, but every eye pass
// starts from the game camera's room (outside: no box), and her own room was not found there ("0 at Jodie's
// body"). The house is the one room with a box (LIVE `zones`: (-12.4 -5.1 -3.5)-(12.5 5.3 8.5)). `eyeroom on`:
// each eye pass starts from the smallest known room box holding THAT eye (the camera node after the eye pose,
// read at the start of the pass), else the game camera's room as before.
std::atomic<int> g_eyeRoom{0};  // 0 off, 1 known room boxes, 2 the game's own lookup (FUN_14092e2b0)
// STATIC (0.17.9): FUN_14092e2b0(scene, point, hint room) is the game's "which room holds this point": the hint
// room if its volume (+0x118 -> +0x28, test FUN_1409dee30) holds it, else every room of the scene (+400 list)
// whose volume holds it, else the scene's default room. FUN_14086c540 (VIEWPORT setup, once per frame key) uses
// it with the camera node's position and keeps the result at VIEWPORT+0x8b0, while visibility starts from the
// camera entity's own room (+0x98 -> VIEWPORT+0x41e0). With the eye moved off the game camera the two differ.
constexpr uint64_t kFindZoneVa = 0x14092e2b0;
using FindZoneFn = void* (*)(void* scene, const float* point, void* hint);
FindZoneFn g_findZone = nullptr;
int g_gameRoomLogs = 0;
void* g_gameRoomLast[2] = {};
uint64_t g_darkRoomAt[2] = {};
int g_darkRoomLogs = 0;
void* g_eyeRoomLast[2] = {};
int g_eyeRoomLogs = 0;

void* RoomHolding(const float p[3]) {
    void* best = nullptr;
    float bestVolume = 3.4e38f;
    for (int k = 0; k < g_knownCount; ++k) {
        float mn[3], mx[3];
        void* z = g_knownZones[k];
        if (!IsZone(z) || !ZoneBox(z, mn, mx) || !Inside(p, mn, mx)) continue;
        const float volume = (mx[0] - mn[0]) * (mx[1] - mn[1]) * (mx[2] - mn[2]);
        if (volume < bestVolume) {
            bestVolume = volume;
            best = z;
        }
    }
    return best;
}

void BeginPass(void* const* cams, int count, int eye) {
    g_savedCount = 0;
    if (!cams || count <= 0) return;
    g_lastCam = cams[0];
    if (eye == 0 || eye == 1) {
        void* room = nullptr;
        ReadPtr(cams[0], kEntityZone, &room);
        g_passRoom[eye] = room;
        if (eye == 1 && room != g_passRoom[0]) {
            const uint64_t n = ++g_roomSplits;
            if (n <= 30)
                Log("ZONE eye passes start from different rooms: left %p, right %p (camera %p)", g_passRoom[0], room,
                    cams[0]);
            else if (n - g_roomSplitReported >= 900) {
                g_roomSplitReported = n;
                Log("ZONE eye passes started from different rooms in %llu frames", static_cast<unsigned long long>(n));
            }
        }
    }
    // The game's own room for each eye (diagnostic always; `eyeroom game` starts the pass from it).
    if ((eye == 0 || eye == 1) && g_zoneVt) {
        if (!g_findZone) g_findZone = reinterpret_cast<FindZoneFn>(game::Va(kFindZoneVa));
        void* zone = nullptr;
        float p[4] = {0, 0, 0, 1};
        void* scene = SceneOf(cams[0]);
        if (g_findZone && scene && ReadPtr(cams[0], kEntityZone, &zone) && IsZone(zone) && EntityPosition(cams[0], p)) {
            void* room = nullptr;
            __try {
                room = g_findZone(scene, p, zone);
            } __except (EXCEPTION_EXECUTE_HANDLER) { room = nullptr; }
            if (xr::ViewDark()) {
                const uint64_t now = GetTickCount64();
                if (now - g_darkRoomAt[eye] >= 500 && g_darkRoomLogs < 160) {
                    g_darkRoomAt[eye] = now;
                    ++g_darkRoomLogs;
                    Log("DARK eye %d at (%.2f %.2f %.2f): camera's room %p, the game's room for that point %p", eye, p[0],
                        p[1], p[2], zone, room);
                }
            }
            if (room != g_gameRoomLast[eye]) {
                g_gameRoomLast[eye] = room;
                if (g_gameRoomLogs++ < 80)
                    Log("ZONE eye %d at (%.2f %.2f %.2f): the game puts it in room %p, the camera's room is %p%s", eye, p[0],
                        p[1], p[2], room, zone, room != zone ? " (DIFFERENT)" : "");
            }
            if (g_eyeRoom == 2 && room && IsZone(room) && room != zone && WritePtr(cams[0], kEntityZone, room)) {
                g_saved[g_savedCount++] = {cams[0], zone, room};
                return;
            }
        }
    }
    if (g_eyeRoom == 1 && (eye == 0 || eye == 1) && g_zoneVt) {
        const uint64_t now = GetTickCount64();
        if (eye == 0 && now - g_knownAt >= 500) {
            g_knownAt = now;
            RefreshKnownZones(cams[0]);
        }
        for (int i = 0; i < count && g_savedCount < static_cast<int>(std::size(g_saved)); ++i) {
            void* cam = cams[i];
            void* zone = nullptr;
            float p[3];
            if (!cam || !ReadPtr(cam, kEntityZone, &zone) || !IsZone(zone) || !EntityPosition(cam, p)) continue;
            void* room = RoomHolding(p);
            if (i == 0 && room != g_eyeRoomLast[eye]) {
                g_eyeRoomLast[eye] = room;
                if (g_eyeRoomLogs++ < 60)
                    Log("ZONE eye %d at (%.2f %.2f %.2f): %s %p (the game camera's room %p)", eye, p[0], p[1], p[2],
                        room ? "inside room" : "in no room box, keeps the camera's room", room, zone);
            }
            if (!room || room == zone || !WritePtr(cam, kEntityZone, room)) continue;
            g_saved[g_savedCount++] = {cam, zone, room};
        }
        if (g_savedCount) return;
    }
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
        if (!g_eyeZone || g_eyeZone == zone || !IsZone(g_eyeZone)) continue;
        if (!WritePtr(cam, kEntityZone, g_eyeZone)) continue;
        g_saved[g_savedCount++] = {cam, zone, g_eyeZone};
        if (i == 0 && (g_lastLoggedPair[0] != zone || g_lastLoggedPair[1] != g_eyeZone)) {
            g_lastLoggedPair[0] = zone;
            g_lastLoggedPair[1] = g_eyeZone;
            if (g_overrides < 40)
                Log("ZONE eye passes start from %s room %p (the game camera's room is %p)",
                    g_eyeZone == g_jodieZone ? "Jodie's" : "the eye's", g_eyeZone, zone);
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
    if (std::strcmp(verb, "zones") == 0) {
        // The game camera's room and every room of the scene's entities, with their boxes (EXP-042).
        float cam[3] = {};
        const bool camPos = g_lastCam && EntityPosition(g_lastCam, cam);
        void* camZone = nullptr;
        if (g_lastCam) ReadPtr(g_lastCam, kEntityZone, &camZone);
        RefreshKnownZones(g_lastCam);
        Log("ZONES camera %p at (%.2f %.2f %.2f)%s, its room %p, %d rooms known", g_lastCam, cam[0], cam[1], cam[2],
            camPos ? "" : " (no position)", camZone, g_knownCount);
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
    if (std::strcmp(verb, "eyeroom") == 0) {
        g_eyeRoom = !arg ? 0 : std::strcmp(arg, "on") == 0 || std::strcmp(arg, "box") == 0 ? 1 : std::strcmp(arg, "game") == 0 ? 2 : 0;
        g_eyeRoomLast[0] = g_eyeRoomLast[1] = nullptr;
        Log("ZONE eye passes start from: %s", g_eyeRoom == 2   ? "the room the game finds for each eye"
                                              : g_eyeRoom == 1 ? "the room box holding each eye"
                                                               : "the game camera's room");
        return true;
    }
    if (std::strcmp(verb, "zonefix") != 0) return false;
    g_enabled = !(arg && (std::strcmp(arg, "off") == 0 || std::strcmp(arg, "0") == 0));
    Log("ZONE first-person room visibility: %s", g_enabled.load() ? "on" : "off");
    return true;
}

}  // namespace b2s::zonefix
