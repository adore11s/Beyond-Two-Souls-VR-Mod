#include "bones.h"

#include <windows.h>

#include <cstring>

#include "game.h"
#include "log.h"

namespace b2s::bones {
namespace {

constexpr uintptr_t kBoneByHashVa = 0x140977950;  // ADDR-031: (skel, crc32) -> bone entry
constexpr uint8_t kBoneByHashGuard[] = {0x4c, 0x8b, 0x91, 0x70, 0x02, 0x00, 0x00, 0x4d,
                                        0x85, 0xd2, 0x74, 0x19, 0x44, 0x8b, 0x09, 0x45};
using BoneByHashFn = void* (*)(void* skel, uint32_t hash);
BoneByHashFn g_boneByHash = nullptr;

// KCOM handle whose bit 21 of +0x20 marks a cached object at +0x30 (SCRIPT_ENTITY_ABC::vfunc_159).
void* ResolveCached21(void* handle) {
    if (!handle) return nullptr;
    if (((game::Read<uint32_t>(handle, 0x20) >> 21) & 1) == 0) return nullptr;
    return game::Read<void*>(handle, 0x30);
}

}  // namespace

bool Install() {
    auto* p = reinterpret_cast<uint8_t*>(game::Va(kBoneByHashVa));
    if (!game::GuardAt(reinterpret_cast<uintptr_t>(p), kBoneByHashGuard, sizeof(kBoneByHashGuard))) {
        Log("BONES guard MISMATCH; first person disabled");
        return false;
    }
    g_boneByHash = reinterpret_cast<BoneByHashFn>(p);
    return true;
}

void* EntityNode(void* scriptEntity) {
    // STATIC SCRIPT_ENTITY_ABC::vfunc_159 (0x140de0960): +0x300 handle (bit21) -> model object;
    // model+0xa0 -> +0x30 = scene node. LIVE: the model object is an M3D::SKELETON on characters.
    void* model = scriptEntity ? ResolveCached21(game::Read<void*>(scriptEntity, 0x300)) : nullptr;
    void* link = model ? game::Read<void*>(model, 0xa0) : nullptr;
    return link ? game::Read<void*>(link, 0x30) : nullptr;
}

const float* BoneWorld(void* node, uint32_t nameHash) {
    if (!node || !g_boneByHash) return nullptr;
    void* skel = game::Read<void*>(node, 0x170);
    if (!skel) return nullptr;
    void* entry = g_boneByHash(skel, nameHash);
    return entry ? game::Read<const float*>(entry, 0x30) : nullptr;
}

}  // namespace b2s::bones
