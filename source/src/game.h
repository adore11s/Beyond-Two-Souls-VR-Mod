#pragma once
#include <cstddef>
#include <cstdint>

// Engine anchors for Beyond: Two Souls. Every anchor is the VA of the analysed build; at runtime it
// is mapped to this build's address through sigdb (signature-verified, or found by signature in
// another build). An anchor that cannot be found exactly once resolves to null, and callers must
// leave that feature off.
// Canonical list with evidence: docs/ADDRESS_REGISTRY.md.
namespace b2s::game {

enum class Anchor {
    ViewportListDisplay,   // ADDR-001 "VIEWPORT_LIST::Display"(list, cmdbuf, mode, flag)
    ViewportDisplay,       // ADDR-002 "VIEWPORT::Display"(vp, cmdbuf, ...)
    ViewportCameraSetup,   // ADDR-003 per-viewport current-camera publish + layer mask
    SceneDisplay,          // ADDR-004 scene draw for one viewport (scene, vp, mask, pass)
    DisplayWindowDisplay,  // ADDR-005 caller of VIEWPORT_LIST::Display
    KcomResolve,           // ADDR-006 KCOM handle -> object
    KcomResolveSlow,       // ADDR-007 KCOM cached-handle fallback
    CameraBuild,           // ADDR-008 CAMERA view/projection build (cam, aspect, p3, p4)
    RunLuaFile,            // ADDR-009 run a Lua script by name (boot.lua / Immediate.lua runner)
    EngineTrace,           // ADDR-010 printf-style engine trace, compiled to a no-op in retail
    CmdAlloc,              // ADDR-012 append an engine command record (cmdbuf, type) -> record
    CmdCommit,             // ADDR-013 commit a record (dispatches immediately when not deferred)
    ArrayGrow,             // ADDR-014 grow an engine {ptr,count,cap} array (record args)
    ListDisplay2D,         // ADDR-015 VIEWPORT_LIST::Display2D(list, cmdbuf): UI after the 3D scene
    PreUpdateParticles,    // ADDR-016 per-list "PreUpdateParticles" stage (list)
    PostUpdateParticles,   // ADDR-017 per-list "PostUpdateParticles" stage (list)
    CloseupLightsAttach,   // ADDR-018 CLOSEUP_LIGHTING_MANAGER: add rig lights to scene (mgr, scene)
    ViewportEndDisplay,    // ADDR-019 per-viewport end-of-display cleanup, first loop of Display2D (vp, cmdbuf, i)
    LightSetReset,         // ADDR-020 reset a scene light set (*(scene+0x190)+0x60), second loop of Display2D
    NodeUpdateWorld,       // ADDR-025 recompute a scene node's world matrix (+0x80) when +0x120 bit1 is clear
    SkeletonBuild,        // EXP-032 skeleton local -> world matrices
    FinalAnimationCallsite, // EXP-032 final world build; return address = this + 5
    SteamApiUpdate,        // INPUT_API_STEAM::vfunc_03 controller enumeration
    SteamPadCreate,        // INPUT_DEVICE_STEAM_PAD constructor
    SteamContextAccess,    // guarded RIP-relative context data + imported init call
    MeshPaletteCopy,       // skeleton worlds (+0x10) -> a mesh's remapped skinning palette (palette, skeleton)
    SkeletonPublish,       // EXP-035 per-frame swap of a skeleton's world arrays (+0x10 <-> +0x18), before animation
    HavokSkinIndexed,      // EXP-036 Havok "TtPrepare Matrices": (xf, srcWorlds, u16 idx, invBind, count, out)
    HavokSkinContiguous,   // EXP-036 same, bones 0..count-1: (xf, srcWorlds, invBind, count, out)
    SkinPaletteBuild,      // EXP-036 skinned element (type 3): +0x70 bone worlds x +0x60 -> 3x4 palette at +0x80
    SkinPaletteBounds,     // EXP-036 same element: bounds from the +0x70 bone worlds
    ElementTransform,      // EXP-036 rigid element (type 0xe): +0x28 = +0x48 local x *(+0x18) parent world
    ElementWorldUpdate,    // EXP-036 element world refresh from *(+0x18) (types 3 and 0xe)
    ElementTransform24,    // EXP-036 element type 0x24 from *(+0x18)
    ElementFinalize,       // EXP-036 per-element, every frame, all types: distance sort from +0x28 / *(+0x18)
    TransformSetsGet,      // EXP-036 TRANSFORM_SETS::vfunc_01: (self, out worlds, out remap) -> count (cloth input)
    ClothInstanceUpdate,   // EXP-036 HAVOK_CLOTH_INSTANCE::vfunc_13 (self, dt, data): bone worlds at data+0x80
    Count
};

// Resolves every game address (sigdb: verified on the analysed build, found by signature in any
// other build) and fills in the anchors. `textSha256` identifies the build. Returns true when the
// core (render / camera) addresses are all present; features with their own missing addresses turn
// themselves off. Safe to retry while a SteamStub build is still decrypting .text.
bool ResolveAll(const char* textSha256, bool log);
// Number of anchors resolved by the last ResolveAll.
int ResolvedAnchors();
// True when the .text hash is the analysed build's (recorded addresses used as they are).
bool KnownBuild();
// Runtime address of a recorded VA of the analysed build (0x14xxxxxxx); 0 when it is missing or
// ambiguous in this build.
uintptr_t Va(uint64_t recordedVa);
// Guard bytes at a runtime address (false for 0 or an unreadable page).
bool GuardAt(uintptr_t address, const uint8_t* guard, size_t n);
void* Addr(Anchor a);
const char* AnchorName(Anchor a);

uintptr_t ModuleBase();

// Mirrors the engine's handle idiom: bit29 of h+0x20 marks a cached object at h+0x30.
void* ResolveHandle(void* handle);

// A KCOM object's name: its handle (+0x20) -> +0x18 -> char* (STATIC: FUN_140e7d320 prints names
// this way). SEH-guarded; false with an empty string when unreadable.
bool ObjectName(const void* obj, char* out, size_t cap);

// Engine object layouts (docs/CAMERA_FINDINGS.md). Offsets are STATIC evidence.
namespace off {
constexpr uintptr_t VpListArray = 0x40;   // VIEWPORT_LIST: KCOM handle array
constexpr uintptr_t VpListCount = 0x48;   // VIEWPORT_LIST: u32 count
constexpr uintptr_t VpFlags = 0x890;      // VIEWPORT: bit0 enabled, bit17 skip
constexpr uintptr_t VpCamera = 0x8B8;     // VIEWPORT: camera handle
constexpr uintptr_t CamLayerMask = 0x58;
constexpr uintptr_t CamSceneNode = 0x78;
constexpr uintptr_t CamNode = 0xA0;
constexpr uintptr_t CamFov = 0x104;
constexpr uintptr_t CamAspect = 0x108;
constexpr uintptr_t CamScale = 0x10C;
constexpr uintptr_t CamNear = 0x114;
constexpr uintptr_t CamFar = 0x118;
constexpr uintptr_t CamLensShiftX = 0x154;  // view-space X offset applied after inverting the view
constexpr uintptr_t CamLensShiftY = 0x158;
constexpr uintptr_t CamProj = 0x240;        // projection (row-major, RH, row-vector)
constexpr uintptr_t CamView = 0x280;        // view = inverse(node world) * lens shift
constexpr uintptr_t CamProjFar = 0x2E0;
}  // namespace off

template <typename T>
T Read(const void* base, uintptr_t offset) {
    return *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(base) + offset);
}

}  // namespace b2s::game
