// Desk-only DLL (playbook revision 31 / 37, 19.8 point 11 L5, EXP-069): the mod's own carry code behind a C interface,
// so tools/gate1/l5_replay.py replays exactly what fpbody.cpp runs (never a Python copy of it).
// Not part of the mod: CMake target carry_desk (EXCLUDE_FROM_ALL), built by `cmake --build build --target carry_desk`.
// Matrices: 16 floats, row vectors (rows 0..2 axes, row 3 origin), frames x 16 for per-frame arrays.
#include <cstdint>

#include "carry.h"
#include "fpbody.h"

using namespace b2s;

namespace {
const float* At(const float* a, int f) { return a ? a + size_t(f) * 16 : nullptr; }
}  // namespace

// L2 (a) linked carry (fpbody ItemSource / node link, the copy path): out[f] = obj[f] x clean[f]^-1 x drawn[f].
extern "C" __declspec(dllexport) int b2svr_carry_rebase(int frames, const float* obj, const float* clean,
                                                        const float* drawn, float* out) {
    int bad = 0;
    for (int f = 0; f < frames; ++f)
        if (!carry::Rebase(At(obj, f), At(clean, f), At(drawn, f), out + size_t(f) * 16)) ++bad;
    return bad;
}

// L2 (c) held rule (carry::MovieCarry::FeedPoints, as fpbody feeds it): pts frames x n x 3 (the object's bones, point 0
// = its root), wl / wr frames x 16 (her CLEAN wrists, either may be null), link frames bytes (null = always), seed -1 /
// 0 / 1, out frames x 2 weights, head frames x 16 or null.
extern "C" __declspec(dllexport) int b2svr_carry_points(int frames, int n, const float* pts, const float* wl,
                                                        const float* wr, const uint8_t* link, int seed, float* out,
                                                        const float* head) {
    carry::MovieCarry c;
    if (seed >= 0) c.Seed(seed);
    for (int f = 0; f < frames; ++f) {
        const float* w[2] = {At(wl, f), At(wr, f)};
        c.FeedPoints(link ? link[f] != 0 : true, pts + size_t(f) * size_t(n) * 3, n, w, out + f * 2, At(head, f));
    }
    return 0;
}

// The drawn object for every frame (carry::BlendCarry with both hands' clean and drawn wrists and the weights).
extern "C" __declspec(dllexport) int b2svr_carry_blend(int frames, const float* obj, const float* cl, const float* cr,
                                                       const float* dl, const float* dr, const float* weights,
                                                       float* out) {
    for (int f = 0; f < frames; ++f) {
        const float* clean[2] = {At(cl, f), At(cr, f)};
        const float* drawn[2] = {At(dl, f), At(dr, f)};
        carry::BlendCarry(At(obj, f), clean, drawn, weights + f * 2, out + size_t(f) * 16);
    }
    return 0;
}

// L2 (b) skeleton copies (fpbody FollowerSource -> detail::CarryUnmapped): current = the object's animated bones,
// pose = in: its bones on hers already drawn from her drawn bones, out: every other bone carried; returns the count.
extern "C" __declspec(dllexport) int b2svr_carry_unmapped(int count, const float* current, const uint32_t* parents,
                                                          const uint8_t* onHers, float* pose, int handA, int handB) {
    if (count <= 0 || count > int(fpbody::detail::kMaxBones)) return -1;
    bool on[fpbody::detail::kMaxBones]{};
    for (int j = 0; j < count; ++j) on[j] = onHers[j] != 0;
    return int(fpbody::detail::CarryUnmapped(reinterpret_cast<const pose::Mat4*>(current), parents, on,
                                             reinterpret_cast<pose::Mat4*>(pose), uint32_t(count),
                                             handA < 0 ? ~0u : uint32_t(handA), handB < 0 ? ~0u : uint32_t(handB)));
}

// L2 (b) mapped objects (fpbody HierarchyMappedPose -> detail::MappedCarry): pair[j] = her bone paired with object bone
// j (-1 none), herClean / herDrawn her bone worlds (herCount), bodyDelta 16 floats or null. Returns the bones not placed.
extern "C" __declspec(dllexport) int b2svr_carry_mapped(int count, const float* current, const uint32_t* parents,
                                                        const int16_t* pair, const float* herClean,
                                                        const float* herDrawn, const float* bodyDelta, float* out) {
    if (count <= 0 || count > int(fpbody::detail::kMaxBones)) return -1;
    return int(fpbody::detail::MappedCarry(
        reinterpret_cast<const pose::Mat4*>(current), parents, pair, reinterpret_cast<const pose::Mat4*>(herClean),
        reinterpret_cast<const pose::Mat4*>(herDrawn), reinterpret_cast<const pose::Mat4*>(bodyDelta),
        reinterpret_cast<pose::Mat4*>(out), uint32_t(count)));
}

// Drift of a drawn object from its place in the drawn hand (cm, deg), per frame.
extern "C" __declspec(dllexport) int b2svr_carry_drift(int frames, const float* anim, const float* item,
                                                       const float* clean, const float* drawn, float* cm, float* deg) {
    for (int f = 0; f < frames; ++f) carry::Drift(At(anim, f), At(item, f), At(clean, f), At(drawn, f), cm + f, deg + f);
    return 0;
}
