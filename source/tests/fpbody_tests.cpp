#include "../src/fpbody.h"
#include "../src/bodykind.h"
#include <cmath>
#include <cstdio>

using namespace b2s;
namespace {
int failures = 0;
void Check(bool ok, const char* why) { if (!ok) { ++failures; std::printf("FAIL %s\n", why); } }
bool Near(float a, float b) { return std::fabs(a-b) < 1e-4f; }
pose::Mat4 At(float x, float y, float z) {
    auto m=pose::Identity(); m.m[12]=x; m.m[13]=y; m.m[14]=z; return m;
}
}
int main() {
    const uint32_t parents[]{3,0,1};
    Check(fpbody::detail::ValidHierarchy(parents,3), "count is the valid root sentinel");
    const uint32_t bad[]{4,0,1}, cycle[]{1,0,3};
    Check(!fpbody::detail::ValidHierarchy(bad,3), "out-of-range parent rejected");
    Check(!fpbody::detail::ValidHierarchy(cycle,3), "cycle rejected");
    Check(fpbody::detail::Descendant(parents,3,2,0), "grandchild found");
    Check(!fpbody::detail::Descendant(parents,3,0,2), "sentinel terminates ancestry");
    const pose::Vec3 pivot{2,3,4};
    auto yaw=fpbody::detail::BodyTransform(0.f,1.5707963268f,pivot,0.f,0.f);
    auto fixed=pose::TransformPoint(pivot,yaw);
    Check(Near(fixed.x,pivot.x)&&Near(fixed.y,pivot.y)&&Near(fixed.z,pivot.z), "yaw preserves eye pivot");
    auto right=pose::TransformPoint({3,3,4},yaw);
    Check(Near(right.x,2)&&Near(right.z,3), "row-vector yaw convention");
    pose::Mat4 clean[]{At(0,0,0),At(1,0,0),At(2,0,0),At(0,0,0)};
    pose::Mat4 output[]{clean[0],clean[1],clean[2],clean[3]};
    Check(fpbody::detail::RebuildDescendants(clean,parents,3,1,At(5,0,0),output), "rebuild succeeds");
    Check(Near(output[2].m[12],6), "child uses authored relative offset");
    Check(Near(clean[1].m[12],1)&&Near(clean[2].m[12],2), "clean snapshot stays immutable");
    Check(fpbody::detail::RebuildDescendants(clean,parents,3,1,At(8,0,0),output), "second rebuild succeeds");
    Check(Near(output[2].m[12],9), "second rebuild does not consume earlier edit");
    const uint32_t reverseParents[]{1,3,0};
    pose::Mat4 reverseClean[]{At(1,0,0),At(0,0,0),At(2,0,0),At(0,0,0)};
    pose::Mat4 reverseOutput[]{reverseClean[0],reverseClean[1],reverseClean[2],reverseClean[3]};
    Check(fpbody::detail::RebuildDescendants(reverseClean,reverseParents,3,1,At(7,0,0),reverseOutput), "unsorted parents supported");
    Check(Near(reverseOutput[2].m[12],9), "unsorted child follows clean hierarchy");
    auto scaled=At(3,4,5); scaled.m[0]=2; scaled.m[5]=3; scaled.m[10]=4;
    pose::Mat4 inverse;
    Check(fpbody::detail::AffineInverse(scaled,inverse), "authored scale invertible");
    auto identity=pose::Mul(scaled,inverse);
    Check(Near(identity.m[0],1)&&Near(identity.m[5],1)&&Near(identity.m[10],1)&&Near(identity.m[12],0), "affine inverse preserves scale and translation");
    clean[0].m[0]=0.f;
    Check(!fpbody::detail::RebuildDescendants(clean,parents,3,0,At(4,0,0),output), "singular parent rejected");
    // Finger detection on a synthetic right hand: wrist at the origin, fingers towards -Z,
    // thumb out to +X, plus a prop bone (a leaf) that must be ignored.
    {
        constexpr uint32_t N=18;
        uint32_t hp[N]; pose::Mat4 hw[N+1];
        auto bone=[&](uint32_t i,uint32_t parent,float x,float y,float z){ hp[i]=parent; hw[i]=At(x,y,z); };
        bone(0,N,0,0,.25f); bone(1,0,0,0,0);
        bone(2,1,.03f,0,-.02f); bone(3,2,.06f,0,-.04f); bone(4,3,.08f,0,-.06f);        // thumb
        bone(5,1,.02f,0,-.09f); bone(6,5,.02f,0,-.12f); bone(7,6,.02f,0,-.14f);        // index
        bone(8,1,0,0,-.095f); bone(9,8,0,0,-.13f); bone(10,9,0,0,-.15f);               // middle
        bone(11,1,-.02f,0,-.09f); bone(12,11,-.02f,0,-.12f); bone(13,12,-.02f,0,-.14f); // ring
        bone(14,1,-.04f,0,-.08f); bone(15,14,-.04f,0,-.1f); bone(16,15,-.04f,0,-.12f);  // little
        bone(17,1,0,-.02f,-.05f);                                                       // prop
        hw[N]=At(0,0,0);
        const auto f=fpbody::detail::FindFingers(hp,N,hw,1);
        bool thumbOut=true; for (uint32_t i=0;i<f.count;++i) thumbOut&=f.knuckles[i]!=2&&f.knuckles[i]!=17;
        Check(f.anatomical&&f.count==4, "four fingers found beside the thumb");
        Check(thumbOut, "thumb and prop bone are not fingers");
        Check(f.index==5&&f.pinky==14, "index beside the thumb, little finger farthest");
        Check(std::fabs(f.palmLength-.0893f)<.002f, "palm length is wrist to knuckle centroid");
        Check(!fpbody::detail::FindFingers(hp,N,hw,5).anatomical, "a finger bone is not a hand");
        // The same hand with every finger under one palm bone (1 -> 18 -> fingers).
        constexpr uint32_t M=19;
        uint32_t pp[M]; pose::Mat4 pw[M+1];
        for (uint32_t i=0;i<N;++i) { pp[i]=hp[i]==N?M:hp[i]; pw[i]=hw[i]; }
        pp[M-1]=1; pw[M-1]=At(0,0,-.01f); pw[M]=At(0,0,0);
        for (uint32_t i : {2u,5u,8u,11u,14u}) pp[i]=M-1;
        const auto g=fpbody::detail::FindFingers(pp,M,pw,1);
        Check(g.anatomical&&g.count==4&&g.index==5&&g.pinky==14, "a palm bone is stepped through");
        // A finger chain starting next to the wrist: its knuckle is the next bone out.
        uint32_t mp[N]; pose::Mat4 mw[N+1];
        for (uint32_t i=0;i<=N;++i) { if (i<N) mp[i]=hp[i]; mw[i]=hw[i]; }
        mw[5]=At(.01f,0,-.02f);
        const auto m=fpbody::detail::FindFingers(mp,N,mw,1);
        bool walked=false; for (uint32_t i=0;i<m.count;++i) walked|=m.knuckles[i]==6;
        Check(m.anatomical&&walked, "a chain starting at the wrist uses its first bone 3.5 cm out");
    }
    {   // EXP-034 rig shape (young Jodie): fingers and face are constraint groups under the hips.
        // 0 hips, 1 spine, 2 neck, 3 neck1, 4 head (leaf), 5 arm, 6 forearm, 7 hand (leaf),
        // 8 LC_Hand -> 3 finger chains 9..17 + thumb 18..19, 20 LC_Head -> 21 face,
        // 22 secondary group -> 23 hair root at the head -> 24 hair tip, 25 collar 4.4 cm away.
        constexpr uint32_t N=26;
        uint32_t p[N]={N,0,1,2,3,1,5,6,0, 8,9,10, 8,12,13, 8,15,16, 8,18, 0,20, 0,22,23,22};
        pose::Mat4 w[N+1];
        auto set=[&](uint32_t b,float x,float y,float z){ w[b]=At(x,y,z); };
        set(0,0,.65f,0); set(1,0,.84f,0); set(2,0,1.0f,0); set(3,0,1.035f,0); set(4,0,1.07f,0);
        set(5,.15f,.92f,0); set(6,.15f,.76f,0); set(7,.15f,.67f,0); set(8,.15f,.67f,0);
        for (int c=0;c<3;++c) for (int k=0;k<3;++k) set(9+c*3+k,.15f+(c-1)*.02f,.67f-.04f-.025f*k,-.01f);
        set(18,.12f,.66f,-.03f); set(19,.11f,.64f,-.05f);
        set(20,0,1.07f,.02f); set(21,0,1.1f,-.05f); set(22,0,.65f,0); set(23,0,1.072f,.01f); set(24,0,1.2f,.02f);
        set(25,.03f,1.04f,.03f); w[N]=At(0,0,0);
        const auto f=fpbody::detail::FindFingers(p,N,w,7,8);
        Check(f.anatomical&&f.count>=2, "fingers found under the LC hand group");
        Check(!fpbody::detail::FindFingers(p,N,w,7).anatomical, "a leaf hand bone has no fingers of its own");
        bool hide[N]{};
        const uint32_t protect[2]={5,8};
        uint32_t at=99;
        const uint32_t n=fpbody::detail::HeadHideSet(p,N,w,4,20,protect,2,hide,&at);
        Check(hide[4]&&hide[20]&&hide[21], "head, LC head and face hidden");
        Check(hide[23]&&hide[24], "hair branch rooted at the head hidden");
        Check(hide[2]&&hide[3]&&at==2, "neck hidden, collapsed at the neck base");
        Check(hide[25], "a collar / hair bone 4.4 cm from the head is hidden (within 2 neck lengths)");
        Check(!hide[0]&&!hide[1]&&!hide[22], "hips, spine and secondary group root kept");
        Check(!hide[7]&&!hide[8]&&!hide[9], "arm and fingers never hidden");
        Check(n==8, "exactly the head, neck, face, hair and collar bones");
    }
    {   // EXP-046: Jodie's model families (bodykind.h) and when the young-Jodie rules apply.
        using bodykind::Family;
        Check(bodykind::FromModelName("IW_JHO9_INT2")==Family::Child, "JHO9 is young Jodie");
        Check(bodykind::FromModelName("IW_JHO9_PAJ1RIP_IW_JHO9_PAJ1RIP_SKLRAGD")==Family::Child, "JHO9 sub-object");
        Check(bodykind::FromModelName("IW_JHO14_BS")==Family::Teen && bodykind::FromModelName("IW_JHO16_KRAV_B")==Family::Teen,
              "JHO14 / JHO16 are the teenager");
        Check(bodykind::FromModelName("IW_JHO20_PRO")==Family::Adult && bodykind::FromModelName("IW_JHOWITHBIKE")==Family::Adult &&
              bodykind::FromModelName("IW_JHOB")==Family::Adult, "JHO20, JHOWITHBIKE, JHOB are adult");
        Check(bodykind::FromModelName("IW_JHO2020_HUN")==Family::Adult, "JHO2020 materials read as 20");
        Check(bodykind::FromModelName("IW_PBINTM03")==Family::Unknown && bodykind::FromModelName(nullptr)==Family::Unknown,
              "other models are unknown");
        Check(bodykind::SceneOf("_0102_PROLOGUE_ARRESTED_Jodie")==102 && bodykind::SceneOf("_0401_JODIE_AND_ANAEL_Jodie")==401,
              "scene from the character name");
        Check(bodykind::SceneOf("MENU_GAME_Jodie")==-1 && bodykind::SceneOf("_04x1_A")==-1, "no scene");
        Check(bodykind::FromScene(102)==Family::Adult && bodykind::FromScene(401)==Family::Child &&
              bodykind::FromScene(503)==Family::Child && bodykind::FromScene(705)==Family::Child, "adult prologue, child chapters");
        Check(bodykind::FromScene(801)==Family::Teen && bodykind::FromScene(901)==Family::Teen &&
              bodykind::FromScene(1209)==Family::Teen && bodykind::FromScene(4001)==Family::Teen, "teen chapters and the DLC");
        Check(bodykind::FromScene(1401)==Family::Adult && bodykind::FromScene(2408)==Family::Adult, "adult chapters");
        Check(bodykind::FromScene(2502)==Family::Unknown && bodykind::FromScene(-1)==Family::Unknown,
              "Black Sun holds every family: unknown");
        Check(bodykind::ChildRules(Family::Child,0.50f) && !bodykind::ChildRules(Family::Teen,0.38f) &&
              !bodykind::ChildRules(Family::Adult,0.30f) && !bodykind::ChildRules(Family::Other,0.30f),
              "the family decides when known");
        Check(bodykind::ChildRules(Family::Unknown,0.34f) && !bodykind::ChildRules(Family::Unknown,0.48f),
              "unknown family: the 0.42 m arm rule (young Jodie 0.34, adult 0.48)");
    }
    {   // EXP-064: the carried Navajo bucket IW_ACC_UNDEFINED_CROSSSET_EXT_NAVBUCKETB (game file, rest pose): a 26-bone
        // copy of her skeleton, R_Hand(15) > EA_R_ATTACH01(16, the handle) > bone 17 (the bucket, 15 cm below).
        const int par[26]{-1,0,1,2,3,0,5,6,7,0,9,10,11,12,13,14,15,16,11,18,19,20,11,22,23,21};
        const float at[26][3]{{0,.83f,-.017f},{-.075f,.797f,-.017f},{-.075f,.429f,-.017f},{-.075f,.071f,-.036f},
            {-.075f,.02f,.078f},{.075f,.797f,-.017f},{.075f,.429f,-.017f},{.075f,.071f,-.036f},{.075f,.02f,.078f},
            {0,.909f,-.017f},{0,.98f,.006f},{0,1.052f,.004f},{-.029f,1.251f,.008f},{-.138f,1.236f,-.017f},
            {-.315f,1.059f,-.017f},{-.455f,.92f,.097f},{-.455f,.92f,.097f},{-.455f,.77f,.097f},{.029f,1.251f,.008f},
            {.138f,1.236f,-.017f},{.315f,1.059f,-.017f},{.455f,.92f,.097f},{0,1.306f,-.012f},{0,1.353f,.005f},
            {0,1.4f,.023f},{.455f,.92f,.097f}};
        uint32_t parents[26];
        pose::Mat4 current[26], drawn[26];
        bool onHers[26];
        for (int j=0;j<26;++j) {
            parents[j]=par[j]<0?26u:uint32_t(par[j]);
            current[j]=At(at[j][0],at[j][1],at[j][2]);
            onHers[j]=j!=17;          // every bone but the bucket sits on one of hers
            drawn[j]=current[j];
        }
        // the controller hand: her drawn right hand (and the copied handle socket) 20 cm forward, 10 cm up, turned 90 deg
        // about her up axis at the wrist
        auto moved=[&](const pose::Mat4& m){
            const pose::Vec3 w{at[15][0],at[15][1],at[15][2]};
            pose::Mat4 r=pose::Identity();
            r.m[0]=0; r.m[2]=-1; r.m[8]=1; r.m[10]=0;            // 90 deg about Y (row vectors)
            pose::Mat4 out=pose::Mul(pose::Mul(m,At(-w.x,-w.y,-w.z)),r);
            out=pose::Mul(out,At(w.x,w.y+0.1f,w.z+0.2f));
            return out;
        };
        drawn[15]=moved(current[15]);
        drawn[16]=moved(current[16]);
        const uint32_t carried=fpbody::detail::CarryUnmapped(current,parents,onHers,drawn,26);
        Check(carried==1, "the bucket is the one bone carried");
        const float dx=drawn[17].m[12]-drawn[16].m[12], dy=drawn[17].m[13]-drawn[16].m[13],
                    dz=drawn[17].m[14]-drawn[16].m[14];
        Check(Near(dx,0)&&Near(dy,-0.15f)&&Near(dz,0), "the bucket hangs 15 cm below the drawn handle");
        Check(Near(drawn[17].m[12],-0.455f)&&Near(drawn[17].m[13],0.87f)&&Near(drawn[17].m[14],0.297f),
              "the bucket went with the controller hand (not left at its animated place)");
        Check(Near(drawn[0].m[13],0.83f)&&Near(drawn[24].m[13],1.4f), "bones on hers keep her drawn pose");
        bool none[26]{};
        pose::Mat4 still[26];
        for (int j=0;j<26;++j) still[j]=current[j];
        Check(fpbody::detail::CarryUnmapped(current,parents,none,still,26)==25 && Near(still[17].m[13],0.77f),
              "nothing on hers: every bone keeps its own place (roots stay, the rest follow unchanged parents)");
        // the handle socket is one of the hands: the bucket hangs from it and is not blended with the left hand
        pose::Mat4 two[26];
        for (int j=0;j<26;++j) two[j]=current[j];
        two[15]=drawn[15]; two[16]=drawn[16];
        fpbody::detail::CarryUnmapped(current,parents,onHers,two,26,16,25);
        Check(Near(two[17].m[13]-two[16].m[13],-0.15f) && Near(two[17].m[14],0.297f),
              "two hand sockets: a bone hanging from one hand still follows that hand only");
    }
    {   // EXP-064: the extinguisher (two-handed, played in step): hips(0) > R socket(1), L socket(2), hose bones 3, 4 on
        // the hips between the sockets. Her drawn right hand 30 cm forward, the left where it was.
        const uint32_t parents[5]{5,0,0,0,0};
        pose::Mat4 current[5]{At(.5f,.5f,0),At(0,1,0),At(1,1,0),At(.25f,1,0),At(.75f,1,0)};
        const bool onHers[5]{true,true,true,false,false};
        pose::Mat4 drawn[5]{current[0],At(0,1,.3f),current[2],current[3],current[4]};
        pose::Mat4 alone[5]{current[0],drawn[1],current[2],current[3],current[4]};
        fpbody::detail::CarryUnmapped(current,parents,onHers,alone,5);
        Check(Near(alone[3].m[14],0) && Near(alone[4].m[14],0), "without the hand pair the hose follows the hips");
        fpbody::detail::CarryUnmapped(current,parents,onHers,drawn,5,1,2);
        Check(Near(drawn[3].m[14],0.225f) && Near(drawn[4].m[14],0.075f) && Near(drawn[3].m[12],0.25f),
              "with the hand pair the hose follows both hands by its place between them");
    }
    std::printf("fpbody tests: %d failure(s)\n",failures);
    return failures?1:0;
}
