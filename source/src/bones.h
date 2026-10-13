#pragma once
// Named bone access on engine characters (EXP-031). STATIC: SCRIPT_ENTITY+0x300 -> model, +0xa0 -> +0x30 = scene node;
// node+0x170 -> skeleton instance; FUN_140977950(skel, crc32(name)) -> bone entry whose +0x30 points
// at the bone's 4x4 world matrix (row-major, translation at +0x30 of the matrix).
#include <cstdint>

#include "bones_crc.h"

namespace b2s::bones {

// Verifies the bone-lookup guard; false = first person stays disabled.
bool Install();

// Scene node of a SINT script entity, or nullptr.
void* EntityNode(void* scriptEntity);

// World matrix of the named bone (by CRC32 of its name) on a node's skeleton, or nullptr.
const float* BoneWorld(void* node, uint32_t nameHash);

}  // namespace b2s::bones
