#include "../src/fp_math.h"
#include <cmath>
#include <cstdio>
using namespace b2s::fp; using namespace b2s::pose;
static int fails=0; static void ck(bool v,const char* s){std::printf("%s %s\n",v?"PASS":"FAIL",s);if(!v)++fails;}
static bool near(float a,float b,float e=1e-3f){return std::fabs(a-b)<e;}
int main(){
 Mat4 root=FromPose(YawQuat(0),{3,0,2}); Mat4 head=FromPose(YawQuat(0),{3,.1f,1}); StableEye e;
 for(int i=0;i<29;++i){ e.Sample(1,head,root); ck(e.HasSample(),"early calibration has usable sample"); }
 ck(!e.Ready(),"calibration waits for 30 samples"); ck(e.Sample(1,head,root)&&e.Ready(),"calibration completes");
 StableEye running; Mat4 head2=FromPose(YawQuat(0),{3,.3f,1}); running.Sample(1,head,root); running.Sample(1,head2,root); ck(near(running.Offset().y,.2f)&&near(running.Eye(root).m[13],.2f),"calibration uses running average");
 Mat4 bob=FromPose(YawQuat(0),{3,.8f,1}); e.Sample(1,bob,root); ck(near(e.Eye(root).m[13],.1f),"post calibration ignores head bob");
 Mat4 turned=FromPose(YawQuat(1.5707963f),{3,0,2}); ck(near(e.Eye(turned).m[12],2)&&near(e.Eye(turned).m[14],2),"offset rotates with root yaw");
 Mat4 bad=Identity(); bad.m[0]=NAN; e.Sample(2,head,bad); ck(!e.Ready()&&!e.HasSample(),"actor switch clears calibration");
 ck(!e.Sample(4,bad,root),"invalid transform rejected"); Mat4 zero=Identity(); zero.m[0]=zero.m[5]=zero.m[10]=0; ck(!e.Sample(5,zero,root),"degenerate basis rejected");
 Mat4 b=e.Base(root,.7f,1.2f,2.f,.3f); ck(near(b.m[0],std::cos(1.2f))&&near(b.m[8],std::sin(1.2f)),"base uses view yaw");
 // Root animation can contain pitch and roll.  Heading must come from the
 // horizontal projection of the forward vector, independent of those tilts.
 Mat4 tilted = Mul(Mul(FromPose(YawQuat(.7f), {3,0,2}),
                       FromPose({std::sin(.8f),0,0,std::cos(.8f)}, {0,0,0})),
                   FromPose({0,0,std::sin(-.7f),std::cos(-.7f)}, {0,0,0}));
 Vec3 f0=TransformPoint({0,0,0},tilted), f1=TransformPoint({0,0,-1},tilted);
 float independent=std::atan2(-(f1.x-f0.x),-(f1.z-f0.z));
 ck(std::fabs(tilted.m[0]-tilted.m[10])>.01f,"tilted root separates horizontal basis from pitch/roll");
 Mat4 yf=YawFrame(tilted);
 ck(near(std::atan2(yf.m[8],yf.m[10]),independent),"YawFrame uses horizontal forward under pitch and roll");
 StableEye tiltedEye; Mat4 tiltedHead=Mul(FromPose({0,0,0,1},{.4f,.2f,-.1f}),tilted);
 for(int i=0;i<30;++i) tiltedEye.Sample(9,tiltedHead,tilted);
 Vec3 expected=TransformPoint({.4f,.2f,-.1f},tilted);
 Vec3 actual{tiltedEye.Eye(tilted).m[12],tiltedEye.Eye(tilted).m[13],tiltedEye.Eye(tilted).m[14]};
 ck(near(actual.x,expected.x)&&near(actual.z,expected.z),"calibrated eye offset preserves tilted root horizontal position");
 auto ik=TwoBone({0,0,0},{1,0,0},.6f,.6f,{0,1,0}); ck(ik.valid&&near(Len(Sub(ik.elbow,ik.shoulder)),.6f)&&near(Len(Sub(ik.wrist,ik.elbow)),.6f),"IK preserves lengths");
 auto unequal=TwoBone({0,0,0},{.2f,0,0},.8f,.3f,{1,0,0}); ck(unequal.valid&&near(Len(Sub(unequal.elbow,unequal.shoulder)),.8f)&&near(Len(Sub(unequal.wrist,unequal.elbow)),.3f),"near target unequal bones preserve lengths");
 ck(TwoBone({0,0,0},{1,0,0},.6f,.6f,{1,0,0}).valid,"parallel pole gets deterministic fallback");
 auto over=TwoBone({0,0,0},{4,0,0},1.f,1.f,{0,1,0});
 ck(over.valid&&over.targetClamped,"overreach reports a safety clamp");
 ck(Len(Sub(over.wrist,over.shoulder))<2.f,"overreach remains inside authored reach");
 ck(!TwoBone({0,0,0},{0,0,0},1.f,1.f,{0,1,0}).valid,"zero distance rejects IK singularity");
 ck(!TwoBone({0,0,0},{NAN,0,0},.6f,.6f).valid&&!TwoBone({0,0,0},{1,0,0},INFINITY,.6f).valid,"IK rejects nonfinite inputs");
 // Hand retargeting (rig-independent wrist orientation).
 Mat4 hb; ck(HandBasis({0,0,-1},{0,1,0},&hb)&&near(hb.m[0],0)&&near(hb.m[2],-1)&&near(hb.m[5],1),"hand basis rows are forward and thumb side");
 Vec3 hu{hb.m[8],hb.m[9],hb.m[10]}; ck(near(Dot(hu,Cross({0,0,-1},{0,1,0})),1),"hand basis third row is forward x thumb side (right-handed)");
 ck(HandBasis({0,0,-1},{0,1,-.5f},&hb)&&near(hb.m[6],0),"hand basis orthogonalises the thumb side");
 ck(!HandBasis({0,0,-1},{0,0,-2},&hb)&&!HandBasis({0,0,0},{0,1,0},&hb),"hand basis rejects degenerate axes");
 Mat4 gb; ck(GripHandBasis(Identity(),0,&gb)&&near(gb.m[1],-1)&&near(gb.m[6],-1),"grip: knuckles along -Y, thumb side along -Z");
 Mat4 gp; ck(GripHandBasis(Identity(),.3f,&gp)&&near(gp.m[1],-std::cos(.3f))&&near(gp.m[2],std::sin(.3f)),"grip pitch turns the fingers away from the thumb side");
 Mat4 bone=FromPose({.2f,.5f,-.1f,.83f},{0,0,0}); { Quat q{.2f,.5f,-.1f,.83f}; Normalize(q); bone=FromPose(q,{0,0,0}); }
 Mat4 rig; HandBasis({.3f,-.2f,-1},{.1f,1,0},&rig);
 Mat4 same=RetargetRotation(bone,rig,rig); bool eq=true; for(int i=0;i<16;++i) eq&=near(same.m[i],bone.m[i]); ck(eq,"retarget onto the rig's own frame keeps the bone");
 Mat4 turn=FromPose(YawQuat(.9f),{0,0,0}); Mat4 moved=RetargetRotation(bone,rig,Mul(rig,turn)); Mat4 want=Mul(bone,turn);
 eq=true; for(int i=0;i<16;++i) eq&=near(moved.m[i],want.m[i]); ck(eq,"retarget follows a rotated target frame");
 Vec3 pr=ArmPole(0,true), pl=ArmPole(0,false); ck(pr.x>0&&pl.x<0&&pr.y<0&&pr.z>0&&near(Len(pr),1),"elbow pole: down, outward, back");
 Vec3 pt=ArmPole(1.5707963f,true); ck(near(pt.z,pr.x,1e-3f)||near(pt.z,-pr.x,1e-3f),"elbow pole turns with the body");
 Mat4 r0=FromPose(YawQuat(0),{0,0,0}); ck(PlausibleStandingHead(FromPose(YawQuat(0),{.05f,1.5f,-.05f}),r0),"standing head accepted");
 ck(!PlausibleStandingHead(FromPose(YawQuat(0),{0,.5f,0}),r0)&&!PlausibleStandingHead(FromPose(YawQuat(0),{.8f,1.5f,0}),r0),"crouched or displaced head rejected");
 std::printf("%s (%d failed)\n",fails?"FAILED":"ALL PASS",fails); return fails?1:0; }
