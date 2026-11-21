#pragma once
#include "fp_math.h"
#include <algorithm>
#include <cstdint>

namespace b2s::fpbody {
// First-person body edits (head hidden, body turned to the view, arms to the controllers).
// The engine's own skeleton is never written: the edits are laid over the copy of Jodie's bone
// matrices into each mesh's skinning palette (MESH::PaletteCopy), the one place every drawn
// pose passes through. Game logic, IK, physics and the camera anchors keep the clean pose.
//
// Published once per game frame. The runtime copies this value under its lock;
// hand matrices and the eye pivot must share the camera's reference epoch.
struct Plan {
    void* node = nullptr;
    void* skeleton = nullptr;
    bool firstPerson = false;
    bool armsActive = false;
    bool turnBody = false;               // F1 gameplay: the drawn body faces the view (about eyePivot)
    bool moveStick = false;              // the player pushes the VR movement stick (idle hold: walking)
    uint32_t recenterEpoch = 0;
    float bodyYaw = 0.f;                 // absolute world yaw, radians
    pose::Vec3 eyePivot{};               // calibrated eye, before HMD translation
    pose::Vec3 headPos{};                // HMD head position in the world
    pose::Vec3 headFwd{};                // HMD view direction in the world (unit)
    float eyeForward = 0.f;              // eye offset in front of the head bone (fp_eye_fwd)
    float eyeUp = 0.f;                   // eye offset above the head bone (fp_eye_up)
    float pivotRight = 0.f, pivotBack = 0.f;
    pose::Mat4 handWorld[2]{};
    bool handValid[2]{};
    float trigger[2]{}, grip[2]{};       // analog trigger / squeeze 0..1 (EXP-067: drawn fingers, playbook 14.12)
    bool triggerTouch[2]{}, thumbTouch[2]{};  // finger touch sensors: index on the trigger, thumb on stick / buttons / rest
    bool touchSeen[2]{};                 // the controller has reported a finger touch since VR started (else resting bases)
    float wristPitch = 0.f;              // radians, + = fingers further down (b2svr.ini fp_wrist_pitch)
};

// Runtime evidence for the final-animation hook.  This is intentionally a
// snapshot API so the command/UI layer can inspect it without touching the
// engine-owned skeleton pointers.
struct Status {
    uint64_t selectedFinalCalls = 0;
    uint64_t captureValid = 0;
    uint64_t applyWrites = 0;            // palette copies that received the first-person edits
    uint32_t lastFailure = 0;
    uint32_t lastHeadIndex = 1024;
    uint32_t bonesCount = 0;
    uintptr_t node = 0, skeleton = 0, worlds = 0;
    float authoredRootYaw = 0.f, wantedBodyYaw = 0.f;
    float headDeltaY = 0.f, bodyShiftY = 0.f;  // headset vs her head height; vertical body shift used
    bool idleHeld = false;                     // standing still: the idle animation is held
    bool firstSuccess = false;
    bool handValid[2]{};
    uint32_t upper[2]{1024, 1024};
    uint32_t fore[2]{1024, 1024};
    uint32_t hand[2]{1024, 1024};
    uint32_t hiddenBones = 0;            // head/face/hair bones collapsed on Jodie's skeleton
    uint32_t armState[2]{};              // 0 arms off, 1 solved, 2 controller not tracked, 3 solve failed, 4 no arm chain
    uint64_t attachedHidden = 0;         // (unused since 0.9.5: separate rigs are no longer touched)
    uint64_t restores = 0;               // clean pose put back before a skeleton publish
    float armReach[2]{}, armLength[2]{}; // shoulder -> wrist target distance, authored arm length (m)
    bool armClamped[2]{};                // target beyond reach (arm fully stretched toward it)
    float armScale = 1.f;                // arm stretch factor used to reach the controller (1..2)
    float handTurn[2]{};                 // degrees between the drawn and the animated hand
    bool handRetargeted[2]{};            // wrist rotation taken from the controller
    uint32_t attachedBones = 0;          // learned arm helper bones carried with the arm
    uint64_t clothWrites = 0;            // cloth matrix maps given the edited pose
    uint64_t propDraws = 0;              // prop pieces drawn from her edited bones
};

bool Install();
// Who a skeleton belongs to (EXP-046): a bodykind::Family value and the evidence, before it is watched.
// Young-Jodie rules apply to the JHO9 family only (or, family unknown, to arms up to 0.42 m).
void Identify(void* skeleton, int family, const char* why);
// Game thread, whenever Jodie's node/skeleton are known: her clean head/root are captured
// for the camera at each final animation build from then on.
void Watch(void* node, void* skeleton);
void Publish(const Plan& plan);
Status ReadStatus();
bool CleanHead(void* node, pose::Mat4* out);
bool CleanRoot(void* node, pose::Mat4* out);
// She stands in her bind pose (arms straight out: the game is not animating her), from the same
// clean final build; held for ~0.1 s before it reports true, ~0.3 s before false again.
bool TPosed(void* node);
// How far her head (clean pose, in her root's frame) is from where it was when she last stood still
// in F1 (the idle hold); false until she has stood still once on this skeleton.
bool HeadOffStanding(void* node, float* meters);
// Her head bone and eye positions as last DRAWN (the array Havok skinned, one frame old), which
// includes head turns added after the animation build (look-at); false until first person has drawn.
bool RenderedHead(void* node, pose::Mat4* head, pose::Vec3* eyeL, pose::Vec3* eyeR);
// Clean eye positions (FA_L_Eye / FA_R_Eye of the face rig) from the same final build; false when
// the rig has no eye bones.
bool CleanEyes(void* node, pose::Vec3* left, pose::Vec3* right);
// Game thread, F1 first person only (the drawn body faces the view): Jodie's clean head position
// and where her left hand is (the left controller while the arms follow it, else the drawn hand),
// for pinning in-world prompts. False outside F1.
bool PromptAnchors(pose::Vec3* head, pose::Vec3* leftHand);
// Game thread, first person (F1 or F3): Jodie's clean head position, the HMD eye position and view
// direction in the world. False outside first person.
bool PromptView(pose::Vec3* head, pose::Vec3* eye, pose::Vec3* forward);
void Suspend();
// Dev: put the clean pose back before each skeleton publish (default on).
void SetRestoreAtPublish(bool on);
// Dev: turn all skeleton edits on/off (camera first person keeps working).
void SetEdits(bool on);
// Dev (EXP-056): fpclothpose one|old = every cloth input gets the one physics pose on named rigs / the earlier mix;
// fpclothjump on|off = the engine's cloth teleport when the first-person pose jumps.
void SetClothOnePose(bool on);
void SetClothTeleports(bool on);
// Dev (0.26.0): fpheadcloth on|off = her cloth hanging from a hidden head bone (hair) is hidden with her head.
void SetHeadCloth(bool on);
// Her head / face / teeth / hair in first person (b2svr.ini fp_head): 0 = shown (0.23.0, for mirror reflections),
// 1 = hidden in all first person, 2 = hidden while first person is a cutscene (F3), shown in F1 gameplay (0.24.0
// default, owner 2026-11-05: "hide the head in first person cutscenes, keep it as it is in first person gameplay").
// Hidden, the head set collapses onto the neck (with everything attached to those bones: hair pieces on M_Head).
enum HeadMode : int { kHeadShown = 0, kHeadHidden = 1, kHeadHiddenInCutscenes = 2 };
void SetHeadMode(int mode);
int GetHeadMode();
const char* HeadModeName(int mode);
// 0.24.0 dev A/B (fpitemcull on|off): her items dropped by the engine's visibility where the animation holds them are
// tested again where they are drawn.
void SetItemCull(bool on);
// EXP-067 (playbook 14.12): fingers on the controllers while F1 motion controls own the arms (b2svr.ini fp_fingers, on)
// and whether they start from the rig's rest shape (fp_fingers_rest_shape, on). Every rig, young Jodie included (owner
// 2026-11-17).
void SetFingers(bool on, bool restShape);
// Dev (fpelems): log every drawable element within 2 m of Jodie for one frame.
void DumpElements();
// Dev (fpwatch): addresses of the head bone's translation in both world arrays (+0x10, +0x18).
bool HeadWorldAddresses(void* out[2]);
// Dev (fpwatchbone): watch this bone index instead of M_Head (1024 = M_Head again).
void SetWatchBone(uint32_t bone);
// Kept for the stereo frame lease; the overlay never leaves engine state to restore.
void Restore();
void BeginRender();
void EndRender();

namespace detail {
constexpr uint32_t kMaxBones = 1024;
inline bool ValidHierarchy(const uint32_t* parents, uint32_t count) {
    if (!parents || !count || count>kMaxBones) return false;
    for (uint32_t i=0; i<count; ++i) {
        uint32_t at=i, steps=0;
        while (at!=count) {
            if (at>=count || ++steps>count) return false;
            at=parents[at];
        }
    }
    return true;
}
inline bool Descendant(const uint32_t* parents, uint32_t count, uint32_t bone, uint32_t ancestor) {
    if (!parents || ancestor>=count) return false;
    for (uint32_t steps=0; bone<count && steps<count; ++steps) {
        if (bone==ancestor) return true;
        bone=parents[bone];
    }
    return false;
}
inline pose::Mat4 BodyTransform(float authoredYaw, float wantedYaw, pose::Vec3 pivot,
                                float right, float back) {
    auto delta=pose::FromPose(pose::YawQuat(wantedYaw-authoredYaw),{0,0,0});
    const auto moved=pose::TransformPoint(pivot,delta);
    const auto offsets=pose::TransformPoint({right,0,back},
        pose::FromPose(pose::YawQuat(wantedYaw),{0,0,0}));
    delta.m[12]=pivot.x-moved.x+offsets.x;
    delta.m[13]=pivot.y-moved.y+offsets.y;
    delta.m[14]=pivot.z-moved.z+offsets.z;
    return delta;
}
// Affine inverse preserves authored bone scale; a rigid inverse would lose it.
inline bool AffineInverse(const pose::Mat4& m, pose::Mat4& out) {
    if (!fp::ValidMatrix(m) || std::fabs(m.m[3])+std::fabs(m.m[7])+std::fabs(m.m[11])>1e-4f) return false;
    const float a=m.m[0],b=m.m[1],c=m.m[2],d=m.m[4],e=m.m[5],f=m.m[6],g=m.m[8],h=m.m[9],i=m.m[10];
    const float det=a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g);
    if (!std::isfinite(det) || std::fabs(det)<1e-8f) return false;
    const float s=1.f/det;
    out=pose::Identity();
    out.m[0]=(e*i-f*h)*s; out.m[1]=(c*h-b*i)*s; out.m[2]=(b*f-c*e)*s;
    out.m[4]=(f*g-d*i)*s; out.m[5]=(a*i-c*g)*s; out.m[6]=(c*d-a*f)*s;
    out.m[8]=(d*h-e*g)*s; out.m[9]=(b*g-a*h)*s; out.m[10]=(a*e-b*d)*s;
    const auto t=pose::TransformPoint({-m.m[12],-m.m[13],-m.m[14]},out);
    out.m[12]=t.x; out.m[13]=t.y; out.m[14]=t.z;
    return fp::ValidMatrix(out);
}
inline bool RebuildDescendants(const pose::Mat4* clean, const uint32_t* parents,
                              uint32_t count, uint32_t joint, const pose::Mat4& replacement,
                              pose::Mat4* output) {
    if (!clean || !output || joint>=count || !ValidHierarchy(parents,count) || !fp::ValidMatrix(replacement)) return false;
    bool pending[kMaxBones]{};
    uint32_t left=0;
    for (uint32_t i=0; i<count; ++i)
        if (i!=joint && Descendant(parents,count,i,joint)) { pending[i]=true; ++left; }
    output[joint]=replacement;
    while (left) {
        bool progress=false;
        for (uint32_t i=0; i<count; ++i) {
            if (!pending[i] || pending[parents[i]]) continue;
            pose::Mat4 inverse;
            if (!AffineInverse(clean[parents[i]],inverse)) return false;
            output[i]=pose::Mul(pose::Mul(clean[i],inverse),output[parents[i]]);
            if (!fp::ValidMatrix(output[i])) return false;
            pending[i]=false; --left; progress=true;
        }
        if (!progress) return false;
    }
    return true;
}
inline uint32_t FirstChild(const uint32_t* parents, uint32_t count, uint32_t bone) {
    for (uint32_t i=0; i<count; ++i) if (parents[i]==bone) return i;
    return kMaxBones;
}
inline uint32_t DescendantCount(const uint32_t* parents, uint32_t count, uint32_t bone) {
    uint32_t n=0;
    for (uint32_t i=0; i<count; ++i) if (i!=bone && Descendant(parents,count,i,bone)) ++n;
    return n;
}
// The fingers of a hand bone, found from the pose (no finger names are known): each child with
// at least two bones below it is a finger (helper / prop bones are leaves), its knuckle the first
// bone of the chain at least 3.5 cm from the wrist. A single child carrying every finger is a
// palm bone and is stepped through. The thumb is the chain pointing furthest from the others;
// of the rest, the knuckle nearest the thumb is the index finger, the farthest the little finger.
struct Fingers {
    uint32_t knuckles[8]{};  // non-thumb fingers
    uint32_t count=0;
    uint32_t index=kMaxBones, pinky=kMaxBones;
    float palmLength=0.f;    // wrist to the knuckle centroid
    bool anatomical=false;
};
// `fingerRoot`: the bone the finger chains hang from. LIVE (young Jodie): the fingers are not under
// L_Hand/R_Hand but under LC_L_Hand/LC_R_Hand, constraint copies parented to M_Hips that follow the hand.
inline Fingers FindFingers(const uint32_t* parents, uint32_t count, const pose::Mat4* w, uint32_t hand,
                           uint32_t fingerRoot=kMaxBones) {
    Fingers f;
    if (!parents || !w || hand>=count) return f;
    auto pos=[&](uint32_t b){ return pose::Vec3{w[b].m[12],w[b].m[13],w[b].m[14]}; };
    const pose::Vec3 wrist=pos(hand);
    uint32_t knuckle[16]{};
    pose::Vec3 at[16]{};
    int n=0;
    uint32_t base=fingerRoot<count?fingerRoot:hand;
    for (int level=0; level<2; ++level) {
        n=0;
        uint32_t only=kMaxBones;
        for (uint32_t c=0; c<count && n<16; ++c) {
            if (parents[c]!=base || DescendantCount(parents,count,c)<2) continue;
            only=c;
            uint32_t k=c;
            for (int step=0; step<4 && fp::Len(fp::Sub(pos(k),wrist))<0.035f; ++step) {
                const uint32_t next=FirstChild(parents,count,k);
                if (next>=count) break;
                k=next;
            }
            knuckle[n]=k; at[n]=pos(k); ++n;
        }
        if (n!=1 || DescendantCount(parents,count,only)<6) break;
        base=only;
    }
    if (n<3) return f;
    int thumb=-1;
    float best=2.f;
    for (int i=0; i<n; ++i) {
        pose::Vec3 others{0,0,0};
        for (int j=0; j<n; ++j) if (j!=i) others=fp::Add(others,fp::Norm(fp::Sub(at[j],wrist)));
        const float d=fp::Dot(fp::Norm(fp::Sub(at[i],wrist)),fp::Norm(others));
        if (d<best) { best=d; thumb=i; }
    }
    if (thumb<0) return f;
    pose::Vec3 centroid{0,0,0};
    float nearest=1e9f, farthest=-1.f;
    for (int i=0; i<n && f.count<8; ++i) {
        if (i==thumb) continue;
        f.knuckles[f.count++]=knuckle[i];
        centroid=fp::Add(centroid,at[i]);
        const float d=fp::Len(fp::Sub(at[i],at[thumb]));
        if (d<nearest) { nearest=d; f.index=knuckle[i]; }
        if (d>farthest) { farthest=d; f.pinky=knuckle[i]; }
    }
    centroid=fp::Mul(centroid,1.f/float(f.count));
    f.palmLength=fp::Len(fp::Sub(centroid,wrist));
    f.anatomical=f.count>=2 && f.index!=f.pinky && f.palmLength>0.02f && f.palmLength<0.3f;
    return f;
}
// Bones to collapse so no head, face or hair is drawn at the eye. LIVE (young Jodie, 262 bones):
// M_Head is a leaf; the face is the LC_M_Head subtree (99 bones, parented to M_Hips) and the hair
// secondary-motion branches near the head. So: the subtrees of M_Head, LC_M_Head and the neck, plus
// every non-core branch rooted near the head (radius below), never the spine / shoulders / arms or
// the protected subtrees (arms and finger groups: a hand may be near the face when the rig is built).
// The neck (head's parent and the bone below it) is hidden too (owner, 2026-10-06); `collapseAt`
// receives the neck base, where the hidden bones are collapsed, so nothing stretches up to the eye.
inline uint32_t HeadHideSet(const uint32_t* parents, uint32_t count, const pose::Mat4* w, uint32_t head,
                            uint32_t lcHead, const uint32_t* protect, uint32_t protectCount, bool* out,
                            uint32_t* collapseAt=nullptr) {
    if (!parents || !w || !out || head>=count || parents[head]>=count) return 0;
    auto pos=[&](uint32_t b){ return pose::Vec3{w[b].m[12],w[b].m[13],w[b].m[14]}; };
    // Half the head-to-neck-base length (two bones down: M_Head -> M_Neck1 -> M_Neck), 2..6 cm.
    // LIVE young Jodie: 3.5 cm; hair root 1.8 cm, nearest clothing bone 4.4 cm.
    const uint32_t neck=parents[parents[head]]<count?parents[parents[head]]:parents[head];
    // Branches rooted within twice the head-to-neck-base length (8..18 cm; LIVE young Jodie 14 cm):
    // the hair crown (1.8 cm) and the hair hanging behind / beside the neck (11-12 cm), which
    // 0.9.1's 3.5 cm missed (owner: "a bit of hair" still visible).
    const float radius=std::clamp(2.f*fp::Len(fp::Sub(pos(head),pos(neck))),0.08f,0.18f);
    // Core: the head's ancestors (neck, spine, hips) and the protected chains' ancestors (shoulders).
    auto core=[&](uint32_t b){
        if (Descendant(parents,count,head,b)) return true;
        for (uint32_t i=0;i<protectCount;++i) if (protect[i]<count && Descendant(parents,count,protect[i],b)) return true;
        return false;
    };
    auto protectedBone=[&](uint32_t b){
        for (uint32_t i=0;i<protectCount;++i) if (protect[i]<count && Descendant(parents,count,b,protect[i])) return true;
        return false;
    };
    bool root[kMaxBones]{};
    root[head]=true;
    root[parents[head]]=true;   // M_Neck1
    root[neck]=true;            // M_Neck: its other children (if any) are protected below
    if (collapseAt) *collapseAt=neck;
    if (lcHead<count) root[lcHead]=true;
    for (uint32_t b=0;b<count;++b)
        if (fp::Len(fp::Sub(pos(b),pos(head)))<radius && !core(b) && !protectedBone(b)) root[b]=true;
    uint32_t n=0;
    for (uint32_t b=0;b<count;++b) {
        out[b]=false;
        for (uint32_t a=b,steps=0; a<count && steps<=count; a=parents[a],++steps)
            if (root[a]) { out[b]=!protectedBone(b); break; }
        n+=out[b];
    }
    return n;
}

// EXP-064: a copied skeleton (an object whose skeleton copies hers bone for bone, played in step with her, e.g. the
// carried Navajo bucket IW_ACC_UNDEFINED_CROSSSET_EXT_NAVBUCKETB): `pose` already holds her drawn bone for every bone
// with onHers[j]; every other bone hangs off its parent with this frame's local (current[j] x current[parent]^-1),
// parents first in any index order, so a bone below a copied bone (the bucket under the handle socket) moves with her
// drawn bone. Roots and bones whose chain cannot be solved keep their current world. Returns the bones carried.
// Two-handed objects played in step with her (EXP-064: the extinguisher, tank on her right hand socket, nozzle on her
// left, hose bones hanging from the hips): when handA / handB are two of the object's bones on her hand sockets, a bone
// that hangs from neither hand but lies within 25 cm of the line between them follows both: the edit of each hand
// applied to it, blended by its place t along that line (rotation of the nearer hand). Pass count for no hands.
// The copy test (EXP-016 / 056): two bone worlds are "the same bone" within 3 mm and 0.02 on the rotation rows.
inline bool SameBoneWorld(const pose::Mat4& a, const pose::Mat4& b) {
    for (int k : {12, 13, 14})
        if (std::fabs(a.m[k] - b.m[k]) > 0.003f) return false;
    for (int k : {0, 1, 2, 4, 5, 6})
        if (std::fabs(a.m[k] - b.m[k]) > 0.02f) return false;
    return true;
}
// EXP-056 hierarchy-mapped objects (MapHierarchy: the backpack, coats, shields), moved here unchanged from fpbody.cpp's
// HierarchyMappedPose so the L5 desk replay (EXP-069) runs the same code. pair[j] = her bone paired with object bone j by
// name (-1 = none). Paired bones take her bone's own edit (clean -> drawn; where the bone sits exactly on hers, her drawn
// bone); the rest hang from their parent with this frame's local, parents first; a root of its own takes the body turn
// (bodyDelta, or none). Returns the bones it could not place (0 = all placed).
inline uint32_t MappedCarry(const pose::Mat4* current, const uint32_t* parents, const int16_t* pair,
                            const pose::Mat4* herClean, const pose::Mat4* herDrawn, const pose::Mat4* bodyDelta,
                            pose::Mat4* out, uint32_t count) {
    bool done[kMaxBones]{};
    uint32_t left = count;
    for (uint32_t j = 0; j < count; ++j)
        if (pair[j] >= 0) {
            const uint32_t k = uint32_t(pair[j]);
            pose::Mat4 inv;
            out[j] = SameBoneWorld(current[j], herClean[k]) || !AffineInverse(herClean[k], inv)
                         ? herDrawn[k]
                         : pose::Mul(pose::Mul(current[j], inv), herDrawn[k]);
            if (!fp::ValidMatrix(out[j])) out[j] = herDrawn[k];
            done[j] = true;
            --left;
        }
    for (uint32_t pass = 0; left && pass < count; ++pass) {
        bool progress = false;
        for (uint32_t j = 0; j < count; ++j) {
            if (done[j]) continue;
            const uint32_t p = parents[j];
            if (p < count && !done[p]) continue;
            pose::Mat4 inv;
            if (p < count && AffineInverse(current[p], inv))
                out[j] = pose::Mul(pose::Mul(current[j], inv), out[p]);
            else  // a root of its own: its node is on her node (FUN_1411dd920), so it turns with her drawn body
                out[j] = bodyDelta ? pose::Mul(current[j], *bodyDelta) : current[j];
            if (!fp::ValidMatrix(out[j])) out[j] = current[j];
            done[j] = true;
            --left;
            progress = true;
        }
        if (!progress) break;
    }
    return left;
}

inline uint32_t CarryUnmapped(const pose::Mat4* current, const uint32_t* parents, const bool* onHers,
                              pose::Mat4* pose, uint32_t count, uint32_t handA=~0u, uint32_t handB=~0u) {
    if (!current || !parents || !onHers || !pose || count==0 || count>kMaxBones) return 0;
    const bool hands=handA<count && handB<count && handA!=handB && onHers[handA] && onHers[handB];
    pose::Mat4 invA, invB;
    const bool handInv=hands && AffineInverse(current[handA],invA) && AffineInverse(current[handB],invB);
    auto underHand=[&](uint32_t j){
        for (uint32_t a=j,steps=0; a<count && steps<=count; a=parents[a],++steps)
            if (a==handA || a==handB) return true;
        return false;
    };
    bool done[kMaxBones]{};
    uint32_t left=0, carried=0;
    for (uint32_t j=0;j<count;++j) { done[j]=onHers[j]; if (!done[j]) ++left; }
    for (uint32_t pass=0; left && pass<count; ++pass) {
        bool progress=false;
        for (uint32_t j=0;j<count;++j) {
            if (done[j]) continue;
            const uint32_t p=parents[j];
            if (p<count && !done[p]) continue;
            pose::Mat4 inv;
            pose[j]=current[j];
            if (p<count && AffineInverse(current[p],inv)) {
                const pose::Mat4 m=pose::Mul(pose::Mul(current[j],inv),pose[p]);
                if (fp::ValidMatrix(m)) { pose[j]=m; ++carried; }
            }
            if (handInv && !underHand(j)) {
                const pose::Vec3 a{current[handA].m[12],current[handA].m[13],current[handA].m[14]};
                const pose::Vec3 b{current[handB].m[12],current[handB].m[13],current[handB].m[14]};
                const pose::Vec3 x{current[j].m[12],current[j].m[13],current[j].m[14]};
                const pose::Vec3 ab=fp::Sub(b,a);
                const float l2=ab.x*ab.x+ab.y*ab.y+ab.z*ab.z;
                if (l2>1e-4f) {
                    const pose::Vec3 ax=fp::Sub(x,a);
                    float t=(ax.x*ab.x+ax.y*ab.y+ax.z*ab.z)/l2;
                    t=t<0.f?0.f:(t>1.f?1.f:t);
                    const pose::Vec3 c{a.x+ab.x*t,a.y+ab.y*t,a.z+ab.z*t};
                    if (fp::Len(fp::Sub(x,c))<0.25f) {
                        const pose::Mat4 ma=pose::Mul(pose::Mul(current[j],invA),pose[handA]);
                        const pose::Mat4 mb=pose::Mul(pose::Mul(current[j],invB),pose[handB]);
                        pose::Mat4 m=t<0.5f?ma:mb;
                        for (int k=12;k<15;++k) m.m[k]=ma.m[k]*(1.f-t)+mb.m[k]*t;
                        if (fp::ValidMatrix(m)) pose[j]=m;
                    }
                }
            }
            done[j]=true; --left; progress=true;
        }
        if (!progress) break;
    }
    for (uint32_t j=0;j<count;++j) if (!done[j]) pose[j]=current[j];
    return carried;
}
} // namespace detail
} // namespace b2s::fpbody
