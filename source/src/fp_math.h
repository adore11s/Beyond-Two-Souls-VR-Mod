#pragma once
#include "posemath.h"
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace b2s::fp {
using pose::Mat4; using pose::Vec3;

inline bool Finite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
inline bool ValidMatrix(const Mat4& m) {
    for (float x : m.m) if (!std::isfinite(x)) return false;
    if (std::fabs(m.m[15] - 1.f) > 1e-3f) return false;
    return std::sqrt(m.m[0]*m.m[0] + m.m[1]*m.m[1] + m.m[2]*m.m[2]) > 1e-5f &&
           std::sqrt(m.m[4]*m.m[4] + m.m[5]*m.m[5] + m.m[6]*m.m[6]) > 1e-5f &&
           std::sqrt(m.m[8]*m.m[8] + m.m[9]*m.m[9] + m.m[10]*m.m[10]) > 1e-5f;
}
inline float Dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline Vec3 Add(Vec3 a, Vec3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
inline Vec3 Sub(Vec3 a, Vec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
inline Vec3 Mul(Vec3 a, float s) { return {a.x*s, a.y*s, a.z*s}; }
inline float Len(Vec3 a) { return std::sqrt(Dot(a, a)); }
inline Vec3 Norm(Vec3 a) { float n=Len(a); return n>1e-6f ? Mul(a,1.f/n) : Vec3{0,0,0}; }
inline Mat4 YawFrame(const Mat4& root) {
    // Project the root's forward basis onto the horizontal plane. m[0] is
    // affected by animation pitch/roll; m[10] is the horizontal forward Z.
    return pose::FromPose(pose::YawQuat(std::atan2(root.m[8], root.m[10])),
                          {root.m[12], root.m[13], root.m[14]});
}

// Drawn-hand filter (playbook 08.1, GoW SmoothHand): an adaptive one-pole filter on the controller grip
// matrix in the recentered tracking space, before it is placed on the body. Only the DRAWN hands use it;
// gestures and the game's input keep the raw samples. Per hand: one filtered matrix and the XR display
// time of its sample, so a repeated poll of the same sample never invents a velocity.
//   rot = sqrt(0.5 * sum((R - Rf)^2)), speed = (|p - pf| + 0.15 * rot) / dt,
//   cutoff = cutoffHz + beta * speed, alpha = 1 / (1 + tau / dt) with tau = 1 / (2 pi cutoff),
//   the 12 affine components move by alpha, then the rotation rows are made orthonormal (scale kept).
// The first sample, dt <= 0 with a new time, dt > 0.1 s or a new recenter epoch start again from the raw grip.
struct HandFilter {
    bool have = false;
    int64_t time = 0;
    uint32_t epoch = 0;
    Mat4 m = pose::Identity();
    void Reset() { have = false; }
    Mat4 Feed(const Mat4& raw, int64_t timeNs, uint32_t recenterEpoch, float cutoffHz = 3.f, float beta = 6.f) {
        if (have && timeNs == time && recenterEpoch == epoch) return m;  // the same sample again
        const float dt = float(double(timeNs - time) * 1e-9);
        if (!have || recenterEpoch != epoch || !(dt > 0.f) || dt > 0.1f || !ValidMatrix(m)) {
            m = raw;
            time = timeNs;
            epoch = recenterEpoch;
            have = true;
            return m;
        }
        float rot2 = 0.f;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                const float d = raw.m[r * 4 + c] - m.m[r * 4 + c];
                rot2 += d * d;
            }
        const float rot = std::sqrt(0.5f * rot2);
        const Vec3 dp{raw.m[12] - m.m[12], raw.m[13] - m.m[13], raw.m[14] - m.m[14]};
        const float speed = (Len(dp) + 0.15f * rot) / dt;
        const float cutoff = cutoffHz + beta * speed;
        const float tau = 1.f / (6.2831853f * cutoff);
        const float alpha = 1.f / (1.f + tau / dt);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c) m.m[r * 4 + c] += alpha * (raw.m[r * 4 + c] - m.m[r * 4 + c]);
        // Gram-Schmidt on the rows (X, Y; Z = X x Y), each row taking the raw sample's length (blending two
        // rotations shortens the rows; the scale to keep is the controller's own).
        Vec3 x{m.m[0], m.m[1], m.m[2]}, y{m.m[4], m.m[5], m.m[6]};
        const Vec3 z0{m.m[8], m.m[9], m.m[10]};
        const float sx = Len(Vec3{raw.m[0], raw.m[1], raw.m[2]}), sy = Len(Vec3{raw.m[4], raw.m[5], raw.m[6]}),
                    sz = Len(Vec3{raw.m[8], raw.m[9], raw.m[10]});
        x = Norm(x);
        y = Norm(Sub(y, Mul(x, Dot(x, y))));
        Vec3 z{x.y * y.z - x.z * y.y, x.z * y.x - x.x * y.z, x.x * y.y - x.y * y.x};
        if (Dot(z, z0) < 0.f) z = Mul(z, -1.f);  // keep the handedness the samples have
        for (int c = 0; c < 3; ++c) {
            m.m[c] = (&x.x)[c] * sx;
            m.m[4 + c] = (&y.x)[c] * sy;
            m.m[8 + c] = (&z.x)[c] * sz;
        }
        time = timeNs;
        return m;
    }
};

// Headset family for the wrist angle (playbook 08.3): the OpenXR system name, lower-cased.
// playstation / psvr / sony / vr2 -> 0 (PS VR2); quest / oculus / meta -> 1 (Meta Quest); anything else -> 2.
inline int HeadsetFamily(const char* systemName) {
    char s[128] = {};
    for (int i = 0; i < 127 && systemName && systemName[i]; ++i)
        s[i] = char(systemName[i] >= 'A' && systemName[i] <= 'Z' ? systemName[i] + 32 : systemName[i]);
    auto has = [&](const char* w) {
        for (const char* p = s; *p; ++p) {
            int k = 0;
            while (w[k] && p[k] == w[k]) ++k;
            if (!w[k]) return true;
        }
        return false;
    };
    if (has("playstation") || has("psvr") || has("sony") || has("vr2")) return 0;
    if (has("quest") || has("oculus") || has("meta")) return 1;
    return 2;
}

class StableEye {
public:
    static constexpr int kSamples=30;
    void Reset(uintptr_t actor=0){ actor_=actor; count_=0; ready_=false; offset_={0,0,0}; }
    bool Sample(uintptr_t actor,const Mat4& head,const Mat4& root,bool valid=true){
        if (actor==0) return false;
        if(actor!=actor_){Reset(actor);}
        if(!valid||!ValidMatrix(head)||!ValidMatrix(root)) return false;
        Mat4 rel=pose::Mul(head,pose::RigidInverse(YawFrame(root)));
        if(!Finite({rel.m[12],rel.m[13],rel.m[14]})) return false;
        if(!ready_){
            ++count_;
            offset_=Mul(Add(Mul(offset_,static_cast<float>(count_-1)),{rel.m[12],rel.m[13],rel.m[14]}),1.f/static_cast<float>(count_));
            if(count_==kSamples) ready_=true;
        }
        return ready_;
    }
    bool Feed(const void* actor,const Mat4& head,const Mat4& root,bool valid=true){ return Sample(reinterpret_cast<uintptr_t>(actor),head,root,valid); }
    bool Ready() const{return ready_;}
    bool HasSample() const{return count_>0||ready_;}
    int Samples() const{return count_;}
    Vec3 Offset() const{return offset_;}
    Mat4 Eye(const Mat4& root) const { Mat4 b=YawFrame(root); Vec3 p=pose::TransformPoint(offset_,b); b.m[12]=p.x;b.m[13]=p.y;b.m[14]=p.z; return b; }
    Mat4 Base(const Mat4& root,float /*bodyYaw*/,float viewYaw,float forwardOffset=0.f,float upOffset=0.f) const {
        Mat4 b=pose::FromPose(pose::YawQuat(viewYaw),{0,0,0}); Mat4 eye=Eye(root); Vec3 p{eye.m[12],eye.m[13],eye.m[14]}; Vec3 o=pose::TransformPoint({0,upOffset,-forwardOffset},b); b.m[12]=p.x+o.x;b.m[13]=p.y+o.y;b.m[14]=p.z+o.z; return b;
    }
private: uintptr_t actor_=0; int count_=0; bool ready_=false; Vec3 offset_{};
};

// EXP-059: the walking eye on foot and on a horse. A rider's head sits ~0.75 m higher above her root than a standing
// head, so one eye learned on the horse kept the view that much too high after the dismount (owner 0.24.0). The engine
// says which applies: her motion kit synced to another character's motion graph (SyncMotionKit) = riding; each mount
// (synced graph) learns its own eye, and the on-foot eye learned before the ride is used again once she is unsynced.
class MountedEyes {
public:
    void Reset(uintptr_t actor){ foot_.Reset(actor); ride_.Reset(actor); rideGraph_=0; riding_=false; }
    // kitRead: her motion kit could be read this frame (cutscenes have none: the last state is kept).
    // synced: the motion graph it is synced to, 0 = not synced. Returns true when on foot / riding changed.
    bool Update(uintptr_t actor,bool kitRead,uintptr_t synced){
        if(!kitRead) return false;
        const bool riding=synced!=0, changed=riding!=riding_;
        riding_=riding;
        if(riding && synced!=rideGraph_){ ride_.Reset(actor); rideGraph_=synced; }
        return changed;
    }
    bool Riding() const{return riding_;}
    StableEye& Active(){return riding_?ride_:foot_;}
    const StableEye& Foot() const{return foot_;}
    const StableEye& Ride() const{return ride_;}
private: StableEye foot_, ride_; uintptr_t rideGraph_=0; bool riding_=false;
};

inline Vec3 Cross(Vec3 a, Vec3 b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }

// Gameplay eye calibration only uses frames where Jodie stands or walks normally: the head
// joint is at a standing height above the root and roughly above it. A crouch, a fall, a
// climb or an interaction would otherwise fix a wrong eye height until F1 is toggled again.
inline bool PlausibleStandingHead(const Mat4& head, const Mat4& root) {
    const float up=head.m[13]-root.m[13];
    const float dx=head.m[12]-root.m[12], dz=head.m[14]-root.m[14];
    return std::isfinite(up) && up>0.7f && up<2.0f && dx*dx+dz*dz<0.45f*0.45f;
}

// Anatomical hand frame as a rotation (rows: forward = wrist to knuckles, thumb side = little
// finger knuckle to index knuckle, then forward x thumb side). Built the same way for the rig
// and for the controller, so left and right hands need no mirrored constants.
inline bool HandBasis(Vec3 forward, Vec3 thumbSide, Mat4* out) {
    const Vec3 f=Norm(forward);
    if (!Finite(f) || Len(f)<.5f) return false;
    const Vec3 s=Norm(Sub(thumbSide,Mul(f,Dot(thumbSide,f))));
    if (!Finite(s) || Len(s)<.5f) return false;
    const Vec3 u=Cross(f,s);
    Mat4 r=pose::Identity();
    r.m[0]=f.x; r.m[1]=f.y; r.m[2]=f.z;
    r.m[4]=s.x; r.m[5]=s.y; r.m[6]=s.z;
    r.m[8]=u.x; r.m[9]=u.y; r.m[10]=u.z;
    *out=r;
    return true;
}

// The hand frame of a controller from its OpenXR grip pose. The spec defines grip -Z as the
// line through the closed fingers from the little finger to the thumb and +X as the palm
// normal (the same side for both hands), so wrist-to-knuckles is -Y. pitchRad turns the
// fingers away from the thumb side (positive = fingers further down while holding the pad).
inline bool GripHandBasis(const Mat4& grip, float pitchRad, Mat4* out) {
    const Vec3 f=Norm({-grip.m[4],-grip.m[5],-grip.m[6]}), s=Norm({-grip.m[8],-grip.m[9],-grip.m[10]});
    const float c=std::cos(pitchRad), n=std::sin(pitchRad);
    return HandBasis(Sub(Mul(f,c),Mul(s,n)), Add(Mul(s,c),Mul(f,n)), out);
}

// Wrist rotation that carries the rig's anatomical frame onto targetBasis while keeping the
// bone's own axis convention: bone = rel * rig, so the result is rel * target.
inline Mat4 RetargetRotation(const Mat4& boneRotation, const Mat4& rigBasis, const Mat4& targetBasis) {
    return pose::Mul(pose::Mul(boneRotation,pose::RigidInverse(rigBasis)),targetBasis);
}

// Where the elbow bends: down, out to the arm's own side and a little back, from the body
// heading. Independent of the rig's bone axes and stable when a hand crosses the body.
inline Vec3 ArmPole(float bodyYaw, bool rightArm) {
    const Mat4 r=pose::FromPose(pose::YawQuat(bodyYaw),{0,0,0});
    const Vec3 fwd=pose::TransformPoint({0,0,-1},r), right=pose::TransformPoint({1,0,0},r);
    return Norm(Add(Add(Vec3{0,-1,0},Mul(rightArm?right:Mul(right,-1.f),.5f)),Mul(fwd,-.35f)));
}

struct TwoBoneResult { bool valid=false; bool targetClamped=false; Vec3 shoulder{},elbow{},wrist{}; };
inline TwoBoneResult TwoBone(Vec3 shoulder,Vec3 target,float upper,float lower,Vec3 pole={0,1,0}) {
    TwoBoneResult r; if(!Finite(shoulder)||!Finite(target)||!Finite(pole)||!std::isfinite(upper)||!std::isfinite(lower)||!(upper>1e-5f)||!(lower>1e-5f)) return r;
    Vec3 d=Sub(target,shoulder); float dist=Len(d); if(!(dist>1e-6f)) return r;
    // Keep the law-of-cosines solve away from both singular endpoints.  A
    // controller outside the authored arm is a valid tracked state; report
    // that fact while solving the nearest feasible pose.
    constexpr float margin=0.01f;
    float reach=upper+lower-margin; float lo=std::fabs(upper-lower)+margin;
    if (lo>reach) return r;
    float cl=dist<lo?lo:(dist>reach?reach:dist);
    r.targetClamped=std::fabs(cl-dist)>1e-5f;
    Vec3 axis=Mul(d,1.f/dist);
    Vec3 side=Sub(pole,Mul(axis,Dot(pole,axis))); if(Len(side)<1e-5f){ side=Sub(Vec3{0,1,0},Mul(axis,axis.y)); if(Len(side)<1e-5f) side=Sub(Vec3{1,0,0},Mul(axis,axis.x)); } side=Norm(side);
    float x=(upper*upper+cl*cl-lower*lower)/(2.f*cl); float h=std::sqrt(std::max(0.f,upper*upper-x*x));
    r.shoulder=shoulder; r.wrist=Add(shoulder,Mul(axis,cl)); r.elbow=Add(Add(shoulder,Mul(axis,x)),Mul(side,h));
    r.valid=std::isfinite(cl) && std::isfinite(x) && std::isfinite(h) && Finite(r.elbow) && Finite(r.wrist);
    if (!r.valid) r = TwoBoneResult{};
    return r;
}
}
