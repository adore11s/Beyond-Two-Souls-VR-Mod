#include "xr.h"

#define XR_USE_GRAPHICS_API_D3D11
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <atomic>
#include <algorithm>
#include <cstdio>
#include <share.h>
#include <string>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "hotkeys.h"
#include "log.h"
#include "posemath.h"
#include "videowatch.h"
#include "xr_geometry.h"
#include "xr_recenter.h"

namespace b2s::xr {
namespace {

// ---- math: src/posemath.h (row-major, row-vector, RH, Y up, -Z forward, metres) -----------

using pose::Identity;
using pose::Mat4;
using pose::Mul;
using pose::RigidInverse;

pose::Quat Q(const XrQuaternionf& q) { return {q.x, q.y, q.z, q.w}; }
pose::Vec3 V(const XrVector3f& p) { return {p.x, p.y, p.z}; }
Mat4 FromPose(const XrQuaternionf& q, const XrVector3f& p) { return pose::FromPose(Q(q), V(p)); }

// ---- OpenXR state --------------------------------------------------------------------------

XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_local = XR_NULL_HANDLE, g_viewSpace = XR_NULL_HANDLE;
ReferenceChangeSchedule g_referenceChanges;
std::atomic<XrSessionState> g_state{XR_SESSION_STATE_UNKNOWN};
std::atomic<bool> g_running{false};  // xrBeginSession done
std::atomic<bool> g_started{false};
std::atomic<bool> g_lost{false};  // runtime is exiting / session or instance loss pending

enum class InputAction { Grip, Aim, Stick, Trigger, Squeeze, A, B, StickClick, Menu, Count };
constexpr int kActionCount = int(InputAction::Count);
XrActionSet g_inputActions = XR_NULL_HANDLE;
XrAction g_action[kActionCount] = {};
XrPath g_handPath[2] = {};
XrSpace g_gripSpace[2] = {}, g_aimSpace[2] = {};
bool g_inputAttached = false;
std::atomic<uint64_t> g_inputSerial{0};
bool g_focusedOnce = false;

struct EyeSwapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    int32_t width = 0, height = 0;
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::vector<ID3D11RenderTargetView*> rtvs;
};
EyeSwapchain g_eyes[2];
EyeSwapchain g_quad;  // flat UI / 2D screen panel
bool g_linearizeInShader = false;
constexpr float kQuadDistance = 1.8f, kQuadWidth = 2.2f;

// Seated reference (recenter): yaw-only orientation + head position, in LOCAL space.
std::mutex g_refMutex;
Mat4 g_refInv = Identity();
XrPosef g_refPose{{0, 0, 0, 1}, {0, 0, 0}};
std::atomic<bool> g_recenterRequest{true};  // pending until a settled pose is accepted
pose::SettleLatch g_settle;                     // game thread only
std::atomic<uint32_t> g_recenterEpoch{0};       // bumped once per accepted recenter

// Frame packets, one per recorded stereo pair; the slot travels in the eye marker name.
struct Packet {
    std::atomic<bool> valid{false};
    XrTime displayTime = 0;
    bool shouldRender = false;
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    XrPosef refPose{{0, 0, 0, 1}, {0, 0, 0}};  // seated reference these views were composed with
    uint32_t recenterEpoch = 0;
    FrameInput input;
};
Packet g_packets[kSlots];
// Last views with valid, normalised orientation and position (held through tracking loss).
XrView g_goodViews[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
// EXP-014: render and submit a symmetric per-eye frustum containing the runtime's asymmetric one.
// Some engine passes ignore the projection's off-centre terms (P20/P21) and assumed a centred
// frustum: with the PSVR2's asymmetric eyes the backdrop was screen-locked and clipped, sparks
// vanished from one eye. Symmetric is the default; `symfov off` restores the runtime's frustum.
std::atomic<bool> g_symFov{true};
std::atomic<float> g_headOffY{0.f}, g_headOffPitch{0.f};  // EXP-018 test offsets (m, deg)
bool g_haveGoodViews = false;
ULONGLONG g_nextGeometryReport = 0;  // game thread; 0 = report on the next tracked frame
std::atomic<int> g_nextSlot{0};
std::atomic<bool> g_beginPending{false};  // waited on the game thread, not yet begun
std::atomic<bool> g_frameOpen{false};     // render thread: xrBeginFrame without xrEndFrame
std::atomic<uint64_t> g_framesEnded{0};
// Last xrWaitFrame result, for a flat frame that begins a wait whose stereo pair never ran.
XrTime g_lastWaitTime = 0;
bool g_lastWaitShouldRender = false;
// Last completed stereo pair: poses/fov and the scene band of the eye images, for the frozen
// backdrop behind 2D-only screens.
XrView g_lastViews[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
bool g_haveLastViews = false;
uint32_t g_lastEyeTop = 0, g_lastEyeHeight = 0;

// ---- D3D blit (render thread) ---------------------------------------------------------------

ID3D11Device* g_device = nullptr;
ID3DDeviceContextState* g_blitState = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3D11Buffer* g_cb = nullptr;
ID3D11BlendState* g_premulBlend = nullptr;
ID3D11Texture2D* g_uiTex = nullptr;
ID3D11Texture2D* g_bbForRtv = nullptr;
ID3D11RenderTargetView* g_bbRtv = nullptr;

struct SrvEntry {
    ID3D11Texture2D* tex;
    ID3D11ShaderResourceView* srv;
};
SrvEntry g_srvCache[8] = {};

const char kBlitHlsl[] = R"(
cbuffer Rect : register(b0) { float4 srcRect; float4 flags; };
Texture2D src : register(t0);
SamplerState samp : register(s0);
struct VsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VsOut VSMain(uint id : SV_VertexID) {
    VsOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}
float4 PSMain(VsOut i) : SV_Target {
    float2 uv = srcRect.xy + i.uv * srcRect.zw;
    float4 c = src.Sample(samp, uv);
    if (flags.x > 0.5) c.rgb = pow(saturate(c.rgb), 2.2);  // sRGB bytes -> linear for an sRGB RTV
    // flags.y: 0 opaque, 1 keep alpha, 2 UI drawn over transparent black (alpha from coverage),
    // 3 brightness only: black UI overlays (cutscene letterbox bars, vignette) drop out.
    float lum = max(c.r, max(c.g, c.b));
    float a = flags.y > 2.5 ? lum : (flags.y > 1.5 ? max(c.a, lum) : (flags.y > 0.5 ? c.a : 1));
    if (flags.z > 0) c.rgb *= flags.z;  // dim
    // flags.w: border width in uv; a thin grey frame marks the panel edges.
    if (flags.w > 0 && (min(i.uv.x, 1 - i.uv.x) < flags.w || min(i.uv.y, 1 - i.uv.y) < flags.w * 1.78)) {
        c.rgb = 0.45; a = 1;
    }
    return float4(c.rgb, a);
}
)";

bool Check(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    char buf[XR_MAX_RESULT_STRING_SIZE] = {};
    if (g_instance) xrResultToString(g_instance, r, buf);
    Log("XR %s failed: %d %s", what, int(r), buf);
    return false;
}

void DestroyControllerActions() {
    g_inputAttached = false;
    for (auto& space : g_gripSpace) {
        if (space) xrDestroySpace(space);
        space = XR_NULL_HANDLE;
    }
    for (auto& space : g_aimSpace) {
        if (space) xrDestroySpace(space);
        space = XR_NULL_HANDLE;
    }
    if (g_inputActions) xrDestroyActionSet(g_inputActions);  // also destroys its actions
    g_inputActions = XR_NULL_HANDLE;
    for (auto& action : g_action) action = XR_NULL_HANDLE;
    g_handPath[0] = g_handPath[1] = XR_NULL_PATH;
}

bool CreateControllerActions() {
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(si.actionSetName, "gameplay");
    strcpy_s(si.localizedActionSetName, "Gameplay");
    if (!Check(xrCreateActionSet(g_instance, &si, &g_inputActions), "create controller action set")) return false;
    for (int h = 0; h < 2; ++h)
        if (!Check(xrStringToPath(g_instance, h ? "/user/hand/right" : "/user/hand/left", &g_handPath[h]),
                   "controller hand path")) return false;
    const struct { const char* name; XrActionType type; } actions[kActionCount] = {
        {"grip_pose", XR_ACTION_TYPE_POSE_INPUT}, {"aim_pose", XR_ACTION_TYPE_POSE_INPUT},
        {"stick", XR_ACTION_TYPE_VECTOR2F_INPUT}, {"trigger", XR_ACTION_TYPE_FLOAT_INPUT},
        {"squeeze", XR_ACTION_TYPE_FLOAT_INPUT}, {"button_a", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {"button_b", XR_ACTION_TYPE_BOOLEAN_INPUT}, {"stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {"menu", XR_ACTION_TYPE_BOOLEAN_INPUT}
    };
    for (int i = 0; i < kActionCount; ++i) {
        XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
        strcpy_s(ai.actionName, actions[i].name);
        strcpy_s(ai.localizedActionName, actions[i].name);
        ai.actionType = actions[i].type;
        ai.countSubactionPaths = 2;
        ai.subactionPaths = g_handPath;
        if (!Check(xrCreateAction(g_inputActions, &ai, &g_action[i]), "create controller action")) return false;
    }
    struct Binding { InputAction action; const char* suffix; int hands = 3; };
    auto suggest = [&](const char* profile, const Binding* bindings, size_t count) {
        XrPath profilePath = XR_NULL_PATH;
        if (!XR_SUCCEEDED(xrStringToPath(g_instance, profile, &profilePath))) return;
        std::vector<XrActionSuggestedBinding> paths;
        for (size_t i = 0; i < count; ++i)
            for (int h = 0; h < 2; ++h) {
                if (!(bindings[i].hands & (1 << h))) continue;
                char path[160];
                sprintf_s(path, "%s%s", h ? "/user/hand/right" : "/user/hand/left", bindings[i].suffix);
                XrPath p = XR_NULL_PATH;
                if (XR_SUCCEEDED(xrStringToPath(g_instance, path, &p)))
                    paths.push_back({g_action[int(bindings[i].action)], p});
            }
        XrInteractionProfileSuggestedBinding bi{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        bi.interactionProfile = profilePath;
        bi.countSuggestedBindings = uint32_t(paths.size());
        bi.suggestedBindings = paths.data();
        // Unsupported suggestions are per-profile failures; other bindings and head tracking remain usable.
        const XrResult r = xrSuggestInteractionProfileBindings(g_instance, &bi);
        Log("XR controller bindings %s: %d (%u)", profile, int(r), bi.countSuggestedBindings);
    };
    const Binding touch[] = {
        {InputAction::Grip, "/input/grip/pose"}, {InputAction::Aim, "/input/aim/pose"},
        {InputAction::Stick, "/input/thumbstick"}, {InputAction::Trigger, "/input/trigger/value"},
        {InputAction::Squeeze, "/input/squeeze/value"}, {InputAction::StickClick, "/input/thumbstick/click"},
        {InputAction::A, "/input/x/click", 1}, {InputAction::B, "/input/y/click", 1},
        {InputAction::A, "/input/a/click", 2}, {InputAction::B, "/input/b/click", 2},
        {InputAction::Menu, "/input/menu/click", 1}
    };
    const Binding index[] = {
        {InputAction::Grip, "/input/grip/pose"}, {InputAction::Aim, "/input/aim/pose"},
        {InputAction::Stick, "/input/thumbstick"}, {InputAction::Trigger, "/input/trigger/value"},
        {InputAction::Squeeze, "/input/squeeze/value"}, {InputAction::StickClick, "/input/thumbstick/click"},
        {InputAction::A, "/input/a/click"}, {InputAction::B, "/input/b/click"}
    };
    const Binding simple[] = {
        {InputAction::Grip, "/input/grip/pose"}, {InputAction::Aim, "/input/aim/pose"},
        {InputAction::Trigger, "/input/select/click"}, {InputAction::Menu, "/input/menu/click"}
    };
    // SteamVR may map Sense controllers onto a supported profile; do not invent a Sony path.
    suggest("/interaction_profiles/oculus/touch_controller", touch, std::size(touch));
    suggest("/interaction_profiles/valve/index_controller", index, std::size(index));
    suggest("/interaction_profiles/khr/simple_controller", simple, std::size(simple));
    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo ai{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        ai.subactionPath = g_handPath[h];
        ai.poseInActionSpace.orientation.w = 1.f;
        ai.action = g_action[int(InputAction::Grip)];
        if (!Check(xrCreateActionSpace(g_session, &ai, &g_gripSpace[h]), "controller grip space")) return false;
        ai.action = g_action[int(InputAction::Aim)];
        if (!Check(xrCreateActionSpace(g_session, &ai, &g_aimSpace[h]), "controller aim space")) return false;
    }
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &g_inputActions;
    if (!Check(xrAttachSessionActionSets(g_session, &attach), "attach controller actions")) return false;
    g_inputAttached = true;
    return true;
}

// Per-hand grip-pose diagnostics, logged when they change: 0 ok, 1 xrGetActionStatePose failed,
// 2 action inactive (no binding / controller off), 3 xrLocateSpace failed, 4 pose not valid, 5 bad numbers.
struct PoseDiag { int stage = -1; XrResult result = XR_SUCCESS; XrSpaceLocationFlags flags = 0; };
PoseDiag g_poseDiag[4];  // left grip, right grip, left aim, right aim
uint64_t g_poseDiagLogged[4] = {};
void ReportPoseDiag(int h, const PoseDiag& d, bool aim = false) {
    const int slot = h + (aim ? 2 : 0);
    PoseDiag& last = g_poseDiag[slot];
    const uint64_t now = GetTickCount64();
    if (d.stage == last.stage && d.flags == last.flags) return;
    last = d;
    g_poseDiagLogged[slot] = now;
    static const char* const kStage[] = {"ok", "get-state failed", "inactive (no binding or controller off)",
                                         "locate failed", "pose not valid", "bad numbers"};
    Log("XR %s %s pose: %s result=%d flags=0x%llx", h ? "right" : "left", aim ? "aim" : "grip", kStage[d.stage], int(d.result),
        static_cast<unsigned long long>(d.flags));
}

void SampleControllers(XrTime time, const Mat4& refInv, FrameInput& input) {
    if (g_state != XR_SESSION_STATE_FOCUSED) {
        input.focused = false;
        return;
    }
    if (!g_inputAttached) return;
    XrActiveActionSet active{g_inputActions, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    // XR_SESSION_NOT_FOCUSED is a positive result, but must never enable controls.
    if (xrSyncActions(g_session, &sync) != XR_SUCCESS) {
        input.focused = false;  // focus may have changed before its event is polled
        return;
    }
    auto scalar = [](float x, float lo, float hi) {
        return std::isfinite(x) ? std::max(lo, std::min(hi, x)) : 0.f;
    };
    for (int h = 0; h < 2; ++h) {
        HandInput& hand = input.hand[h];
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.subactionPath = g_handPath[h];
        auto locate = [&](InputAction action, XrSpace space, Mat4& relative, PoseDiag& d) {
            gi.action = g_action[int(action)];
            XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
            d.result = xrGetActionStatePose(g_session, &gi, &state);
            if (!XR_SUCCEEDED(d.result)) { d.stage = 1; return false; }
            if (!state.isActive) { d.stage = 2; return false; }
            XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
            // VALID is the spec's "usable" bit. TRACKED only says the runtime is not inferring the
            // pose, and some runtimes / controllers (EXP-034) never set it, so it is not required.
            constexpr XrSpaceLocationFlags required = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                XR_SPACE_LOCATION_POSITION_VALID_BIT;
            d.result = xrLocateSpace(space, g_local, time, &loc);
            d.flags = loc.locationFlags;
            if (!XR_SUCCEEDED(d.result)) { d.stage = 3; return false; }
            if ((loc.locationFlags & required) != required) { d.stage = 4; return false; }
            pose::Quat q = Q(loc.pose.orientation);
            const pose::Vec3 p = V(loc.pose.position);
            if (!pose::Normalize(q) || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
                d.stage = 5;
                return false;
            }
            relative = Mul(pose::FromPose(q, p), refInv);
            d.stage = 0;
            return true;
        };
        PoseDiag gripDiag, aimDiag;
        hand.gripValid = locate(InputAction::Grip, g_gripSpace[h], hand.gripRel, gripDiag);
        hand.aimValid = locate(InputAction::Aim, g_aimSpace[h], hand.aimRel, aimDiag);
        ReportPoseDiag(h, gripDiag);
        ReportPoseDiag(h, aimDiag, true);
        // Some SteamVR controller conversions bind the aim pose but leave the grip pose invalid:
        // the aim pose is then the best hand pose there is (same controller, a few cm and degrees off).
        if (!hand.gripValid && hand.aimValid) {
            hand.gripRel = hand.aimRel;
            hand.gripValid = true;
        }
        // Controls follow the controller's presence (its stick action is bound and active), not
        // its tracking: an untracked but connected controller still walks and presses buttons.
        // A controller that is off or unbound has inactive actions and stays neutral.
        gi.action = g_action[int(InputAction::Stick)];
        XrActionStateVector2f presence{XR_TYPE_ACTION_STATE_VECTOR2F};
        const bool present = XR_SUCCEEDED(xrGetActionStateVector2f(g_session, &gi, &presence)) && presence.isActive;
        hand.active = hand.gripValid || present;
        if (!hand.active) continue;
        auto value = [&](InputAction action) {
            gi.action = g_action[int(action)];
            XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
            return XR_SUCCEEDED(xrGetActionStateFloat(g_session, &gi, &state)) && state.isActive
                ? scalar(state.currentState, 0.f, 1.f) : 0.f;
        };
        auto button = [&](InputAction action) {
            gi.action = g_action[int(action)];
            XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
            return XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &gi, &state)) && state.isActive && state.currentState;
        };
        gi.action = g_action[int(InputAction::Stick)];
        XrActionStateVector2f stick{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(g_session, &gi, &stick)) && stick.isActive) {
            hand.stick[0] = scalar(stick.currentState.x, -1.f, 1.f);
            hand.stick[1] = scalar(stick.currentState.y, -1.f, 1.f);
        }
        hand.trigger = value(InputAction::Trigger);
        hand.squeeze = value(InputAction::Squeeze);
        hand.a = button(InputAction::A);
        hand.b = button(InputAction::B);
        hand.stickClick = button(InputAction::StickClick);
        hand.menu = button(InputAction::Menu);
    }
    if (g_state != XR_SESSION_STATE_FOCUSED) {
        input.focused = false;
        input.hand[0] = HandInput{};
        input.hand[1] = HandInput{};
    }
}

void CopyFrameReference(Mat4& refInv, XrPosef& refPose, uint32_t& epoch) {
    {
        std::lock_guard lock(g_refMutex);
        refInv = g_refInv;
        refPose = g_refPose;
        epoch = g_recenterEpoch;
    }
    // EXP-018 applies the same diagnostic reference offset to head, hands and eyes.
    const float dy = g_headOffY.load(), pitch = g_headOffPitch.load() * 0.0174533f;
    if (dy != 0.f || pitch != 0.f) {
        const XrQuaternionf q{std::sin(pitch * 0.5f), 0.f, 0.f, std::cos(pitch * 0.5f)};
        refInv = Mul(refInv, FromPose(q, XrVector3f{0.f, -dy, 0.f}));
    }
}

void SampleFrameInput(XrTime time, const Mat4& refInv, uint32_t epoch, FrameInput* input,
                      bool trackedViews = true) {
    if (!input) return;
    *input = FrameInput{};
    input->serial = ++g_inputSerial;
    input->tickMs = GetTickCount64();
    input->displayTime = time;
    input->recenterEpoch = epoch;
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    constexpr XrSpaceLocationFlags required = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
        XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    if (!trackedViews || g_state != XR_SESSION_STATE_FOCUSED ||
        xrLocateSpace(g_viewSpace, g_local, time, &head) != XR_SUCCESS ||
        (head.locationFlags & required) != required) return;
    pose::Quat q = Q(head.pose.orientation);
    const auto pos = V(head.pose.position);
    if (!pose::Normalize(q) || !std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) return;
    input->focused = true;
    input->headRel = Mul(pose::FromPose(q, pos), refInv);
    SampleControllers(time, refInv, *input);
}

bool CreateBlitResources() {
    if (g_vs && g_ps && g_sampler && g_cb && g_blitState) return true;  // kept across session restarts
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kBlitHlsl, sizeof(kBlitHlsl) - 1, "b2svr_blit", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0,
                          &vsb, &err)) ||
        FAILED(D3DCompile(kBlitHlsl, sizeof(kBlitHlsl) - 1, "b2svr_blit", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0,
                          &psb, &err))) {
        Log("XR blit shader compile failed: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    g_device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
    g_device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
    vsb->Release();
    psb->Release();
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_device->CreateSamplerState(&sd, &g_sampler);
    D3D11_BLEND_DESC bld{};
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_device->CreateBlendState(&bld, &g_premulBlend);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 32;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_device->CreateBuffer(&bd, nullptr, &g_cb);
    // A private device-context state: the blit never disturbs the game's bound pipeline.
    ID3D11Device1* dev1 = nullptr;
    if (FAILED(g_device->QueryInterface(IID_PPV_ARGS(&dev1)))) {
        Log("XR: device has no ID3D11Device1");
        return false;
    }
    const D3D_FEATURE_LEVEL fl[] = {g_device->GetFeatureLevel()};
    HRESULT hr = dev1->CreateDeviceContextState(0, fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr,
                                                &g_blitState);
    dev1->Release();
    if (FAILED(hr)) {
        Log("XR: CreateDeviceContextState failed 0x%08lx", hr);
        return false;
    }
    return g_vs && g_ps && g_sampler && g_cb;
}

// Only for textures B2SVR owns or that stay alive (eye copies, UI/flat copies): the view keeps
// its texture alive, so an entry is evicted round-robin when the cache is full rather than
// refusing new textures after a few resizes.
ID3D11ShaderResourceView* SrvFor(ID3D11Texture2D* tex) {
    for (auto& e : g_srvCache)
        if (e.tex == tex) return e.srv;
    static size_t next = 0;
    SrvEntry* slot = nullptr;
    for (auto& e : g_srvCache)
        if (!e.tex) {
            slot = &e;
            break;
        }
    if (!slot) {
        slot = &g_srvCache[next++ % (sizeof(g_srvCache) / sizeof(g_srvCache[0]))];
        if (slot->srv) slot->srv->Release();
        *slot = {};
    }
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = d.Format;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    const HRESULT hr = g_device->CreateShaderResourceView(tex, &sd, &slot->srv);
    if (FAILED(hr)) {
        Log("XR SRV creation failed 0x%08lx (fmt=%d bind=0x%x)", hr, int(d.Format), d.BindFlags);
        slot->srv = nullptr;
        return nullptr;
    }
    slot->tex = tex;
    return slot->srv;
}

// The swapchain backbuffer is not created for shader input and is replaced by ResizeBuffers,
// so a 2D screen is first copied into this owned, sampleable texture.
ID3D11Texture2D* g_flatTex = nullptr;

ID3D11Texture2D* FlatSource(ID3D11DeviceContext* ctx, ID3D11Texture2D* backbuffer) {
    D3D11_TEXTURE2D_DESC d{};
    backbuffer->GetDesc(&d);
    if (g_flatTex) {
        D3D11_TEXTURE2D_DESC fd{};
        g_flatTex->GetDesc(&fd);
        if (fd.Width != d.Width || fd.Height != d.Height || fd.Format != d.Format) {
            for (auto& e : g_srvCache)
                if (e.tex == g_flatTex) {
                    if (e.srv) e.srv->Release();
                    e = {};
                }
            g_flatTex->Release();
            g_flatTex = nullptr;
        }
    }
    if (!g_flatTex) {
        D3D11_TEXTURE2D_DESC fd = d;
        fd.Usage = D3D11_USAGE_DEFAULT;
        fd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        fd.CPUAccessFlags = 0;
        fd.MiscFlags = 0;
        if (FAILED(g_device->CreateTexture2D(&fd, nullptr, &g_flatTex))) return nullptr;
        Log("XR flat source texture %ux%u fmt=%d", d.Width, d.Height, int(d.Format));
    }
    ctx->CopyResource(g_flatTex, backbuffer);
    return g_flatTex;
}

// Drops an owned texture and its cached shader view (a size change needs a new one).
void DropTexture(ID3D11Texture2D*& tex) {
    if (!tex) return;
    for (auto& e : g_srvCache)
        if (e.tex == tex) {
            if (e.srv) e.srv->Release();
            e = {};
        }
    tex->Release();
    tex = nullptr;
}

ID3D11RenderTargetView* BackbufferRtv(ID3D11Texture2D* bb) {
    if (g_bbForRtv != bb) {
        if (g_bbRtv) g_bbRtv->Release();
        g_bbRtv = nullptr;
        g_bbForRtv = bb;
        g_device->CreateRenderTargetView(bb, nullptr, &g_bbRtv);
    }
    return g_bbRtv;
}

// Draws src rows [top, top+height) over the whole of rtv (w x h). alphaMode: 0 opaque,
// 1 keep alpha, 2 UI-over-black coverage alpha. premulBlend composites instead of replacing.
void Blit(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, uint32_t top, uint32_t height, ID3D11RenderTargetView* rtv,
          int w, int h, int alphaMode = 0, bool premulBlend = false, bool linearize = false, float dim = 0.f,
          float border = 0.f) {
    ID3D11DeviceContext1* ctx1 = nullptr;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))) return;
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    const float rect[8] = {0.f, float(top) / float(d.Height), 1.f, float(height) / float(d.Height),
                           linearize ? 1.f : 0.f, float(alphaMode), dim, border};
    ID3DDeviceContextState* previous = nullptr;
    ctx1->SwapDeviceContextState(g_blitState, &previous);
    ctx1->UpdateSubresource(g_cb, 0, nullptr, rect, 0, 0);
    D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    ctx1->RSSetViewports(1, &vp);
    ctx1->OMSetRenderTargets(1, &rtv, nullptr);
    ctx1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx1->VSSetShader(g_vs, nullptr, 0);
    ctx1->PSSetShader(g_ps, nullptr, 0);
    ID3D11ShaderResourceView* srv = SrvFor(src);
    ctx1->PSSetShaderResources(0, 1, &srv);
    const float blendFactor[4] = {0, 0, 0, 0};
    ctx1->OMSetBlendState(premulBlend ? g_premulBlend : nullptr, blendFactor, 0xffffffff);
    ctx1->PSSetSamplers(0, 1, &g_sampler);
    ctx1->PSSetConstantBuffers(0, 1, &g_cb);
    ctx1->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx1->PSSetShaderResources(0, 1, &nullSrv);
    // The blit state object keeps whatever is bound in it: a target left bound there held the
    // game's backbuffer, and ResizeBuffers then failed (DXGI_ERROR_INVALID_CALL, EXP-041).
    ctx1->OMSetRenderTargets(0, nullptr, nullptr);
    ctx1->SwapDeviceContextState(previous, nullptr);
    if (previous) previous->Release();
    ctx1->Release();
}

// ---- session -------------------------------------------------------------------------------

void HandleEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& s = reinterpret_cast<const XrEventDataSessionStateChanged&>(ev);
            g_state = s.state;
            Log("XR session state -> %d", int(s.state));
            // The recenter at session start can run before the headset tracks; redo it the
            // first time the session gains focus.
            if (s.state == XR_SESSION_STATE_FOCUSED && !g_focusedOnce) {
                g_focusedOnce = true;
                g_recenterRequest = true;
            }
            if (s.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (Check(xrBeginSession(g_session, &bi), "xrBeginSession")) g_running = true;
            } else if (s.state == XR_SESSION_STATE_STOPPING) {
                g_running = false;
                g_referenceChanges.Reset();
                Check(xrEndSession(g_session), "xrEndSession");
            } else if (s.state == XR_SESSION_STATE_EXITING || s.state == XR_SESSION_STATE_LOSS_PENDING) {
                g_running = false;
                g_lost = true;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            const auto& change = reinterpret_cast<const XrEventDataReferenceSpaceChangePending&>(ev);
            if (change.session == g_session && change.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                g_referenceChanges.Add(reinterpret_cast<uint64_t>(change.session),
                                       int32_t(change.referenceSpaceType), change.changeTime);
                Log("XR LOCAL reference change scheduled at %lld", static_cast<long long>(change.changeTime));
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            Log("XR instance loss pending");
            g_running = false;
            g_lost = true;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// Feeds one head sample to the settle latch (playbook A2.5); on acceptance sets the yaw-only
// seated reference and bumps the recenter epoch. Returns true when the recenter took effect.
bool TryRecenter(XrTime t) {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (!XR_SUCCEEDED(xrLocateSpace(g_viewSpace, g_local, t, &loc))) return false;
    pose::Quat q = Q(loc.pose.orientation);
    const bool oriOk = (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) && pose::Normalize(q);
    const bool posOk = (loc.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0;
    if (!g_settle.Feed(q, V(loc.pose.position), posOk, oriOk)) return false;
    // Keep yaw only so the horizon stays level.
    const float yaw = pose::Yaw(q);
    const pose::Quat yq = pose::YawQuat(yaw);
    const XrQuaternionf xyq{yq.x, yq.y, yq.z, yq.w};
    const Mat4 ref = FromPose(xyq, loc.pose.position);
    uint32_t epoch;
    {
        std::lock_guard lock(g_refMutex);
        g_refInv = RigidInverse(ref);
        g_refPose.orientation = xyq;
        g_refPose.position = loc.pose.position;
        epoch = ++g_recenterEpoch;
    }
    Log("XR recentered (epoch %u): yaw=%.1f deg pos=(%.3f %.3f %.3f)", epoch, yaw * 57.2958f, loc.pose.position.x,
        loc.pose.position.y, loc.pose.position.z);
    return true;
}

// Game thread, once per xrWaitFrame: F2 or a pending request recenters once the pose settles.
void ServiceRecenter(XrTime t) {
    static hotkeys::Edge f2;
    if (f2.Feed(VK_F2)) {
        g_recenterRequest = true;
        g_settle.Reset();
    }
    if (g_referenceChanges.ConsumeReady(reinterpret_cast<uint64_t>(g_session),
                                         int32_t(XR_REFERENCE_SPACE_TYPE_LOCAL), t)) {
        g_recenterRequest = true;
        g_settle.Reset();
        Log("XR LOCAL reference change applied at %lld; scheduling recenter", static_cast<long long>(t));
    }
    if (g_recenterRequest && TryRecenter(t)) g_recenterRequest = false;
}

// Replaces untracked or degenerate views with the last good ones so the camera never
// receives a zero quaternion (tracking loss holds the last pose).
void ValidateViews(XrView views[2], XrViewStateFlags flags) {
    static bool holding = false;
    bool ok = (flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) && (flags & XR_VIEW_STATE_POSITION_VALID_BIT);
    for (int e = 0; e < 2 && ok; ++e) {
        pose::Quat q = Q(views[e].pose.orientation);
        const auto& p = views[e].pose.position;
        ok = pose::Normalize(q) && std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
        views[e].pose.orientation = {q.x, q.y, q.z, q.w};
    }
    if (ok) {
        g_goodViews[0] = views[0];
        g_goodViews[1] = views[1];
        g_haveGoodViews = true;
        if (holding) Log("XR tracking restored");
        holding = false;
        return;
    }
    if (!holding) Log("XR views not valid (flags=0x%llx); holding the last good pose", (unsigned long long)flags);
    holding = true;
    if (g_haveGoodViews) {
        views[0] = g_goodViews[0];
        views[1] = g_goodViews[1];
    } else {
        for (int e = 0; e < 2; ++e) views[e].pose = {{0, 0, 0, 1}, {0, 0, 0}};
    }
}

}  // namespace

char g_systemName[XR_MAX_SYSTEM_NAME_SIZE] = "";
char g_runtimeName[XR_MAX_RUNTIME_NAME_SIZE] = "";

bool Running() { return g_started && g_running; }
const char* SystemName() { return g_systemName; }
const char* RuntimeName() { return g_runtimeName; }
uint64_t FramesEnded() { return g_framesEnded; }

bool Start(ID3D11Device* device) {
    if (g_started) return true;
    g_device = device;
    const char* exts[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ci.applicationInfo.applicationName, "B2SVR");
    ci.applicationInfo.applicationVersion = 1;
    strcpy_s(ci.applicationInfo.engineName, "QDT");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = exts;
    if (!Check(xrCreateInstance(&ci, &g_instance), "xrCreateInstance")) return false;
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(g_instance, &ip);
    Log("XR runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
        XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
    strcpy_s(g_runtimeName, ip.runtimeName);

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!Check(xrGetSystem(g_instance, &sgi, &g_system), "xrGetSystem (is the headset connected?)")) return false;
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(g_instance, g_system, &sp);
    Log("XR system: %s", sp.systemName);
    strcpy_s(g_systemName, sp.systemName);

    PFN_xrGetD3D11GraphicsRequirementsKHR getReq = nullptr;
    xrGetInstanceProcAddr(g_instance, "xrGetD3D11GraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&getReq));
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!getReq || !Check(getReq(g_instance, g_system, &req), "xrGetD3D11GraphicsRequirementsKHR")) return false;
    IDXGIDevice* dxgiDev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC ad{};
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDev))) && SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
        adapter->GetDesc(&ad);
        adapter->Release();
    }
    if (dxgiDev) dxgiDev->Release();
    const bool sameAdapter = std::memcmp(&ad.AdapterLuid, &req.adapterLuid, sizeof(LUID)) == 0;
    Log("XR adapter check: game adapter %s the runtime's", sameAdapter ? "matches" : "DOES NOT match");

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = g_system;
    if (!Check(xrCreateSession(g_instance, &sci, &g_session), "xrCreateSession")) return false;

    // Attach before HandleEvents can begin the session. Controller setup is optional:
    // unsupported bindings or unavailable actions must not disable head tracking.
    if (!CreateControllerActions()) {
        DestroyControllerActions();
        Log("XR controller actions unavailable; continuing with head tracking");
    }

    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1.f;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!Check(xrCreateReferenceSpace(g_session, &rs, &g_local), "local space")) return false;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!Check(xrCreateReferenceSpace(g_session, &rs, &g_viewSpace), "view space")) return false;

    uint32_t n = 0;
    xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr);
    std::vector<XrViewConfigurationView> vcv(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n,
                                      vcv.data());
    if (n != 2) {
        Log("XR: expected 2 views, got %u", n);
        return false;
    }

    uint32_t nf = 0;
    xrEnumerateSwapchainFormats(g_session, 0, &nf, nullptr);
    std::vector<int64_t> formats(nf);
    xrEnumerateSwapchainFormats(g_session, nf, &nf, formats.data());
    int64_t format = 0;
    for (int64_t want : {int64_t(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB), int64_t(DXGI_FORMAT_R8G8B8A8_UNORM)})
        for (int64_t f : formats)
            if (!format && f == want) format = f;
    if (!format) format = formats.empty() ? DXGI_FORMAT_R8G8B8A8_UNORM : formats[0];
    Log("XR swapchain format %lld, eye size %ux%u", static_cast<long long>(format),
        vcv[0].recommendedImageRectWidth, vcv[0].recommendedImageRectHeight);

    for (int e = 0; e < 3; ++e) {
        EyeSwapchain& s = e < 2 ? g_eyes[e] : g_quad;
        XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sc.format = format;
        sc.sampleCount = 1;
        sc.width = s.width = e < 2 ? int32_t(vcv[e].recommendedImageRectWidth) : 1920;
        sc.height = s.height = e < 2 ? int32_t(vcv[e].recommendedImageRectHeight) : 1080;
        sc.faceCount = sc.arraySize = sc.mipCount = 1;
        if (!Check(xrCreateSwapchain(g_session, &sc, &s.handle), "xrCreateSwapchain")) return false;
        uint32_t ni = 0;
        xrEnumerateSwapchainImages(s.handle, 0, &ni, nullptr);
        s.images.assign(ni, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(s.handle, ni, &ni, reinterpret_cast<XrSwapchainImageBaseHeader*>(s.images.data()));
        // The game's pixels are already sRGB-encoded bytes. Write them raw through a UNORM view
        // of the (typeless) sRGB image; if the runtime's texture is not typeless, fall back to
        // the native format and linearise in the shader (double-gamma trap, playbook ch09).
        for (auto& img : s.images) {
            D3D11_RENDER_TARGET_VIEW_DESC rd{};
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            rd.Format = format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT(format);
            ID3D11RenderTargetView* rtv = nullptr;
            if (FAILED(device->CreateRenderTargetView(img.texture, &rd, &rtv))) {
                rd.Format = DXGI_FORMAT(format);
                g_linearizeInShader = format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                device->CreateRenderTargetView(img.texture, &rd, &rtv);
            }
            s.rtvs.push_back(rtv);
        }
    }
    Log("XR swapchains ready (%zu images, linearize=%d)", g_eyes[0].images.size(), int(g_linearizeInShader));
    if (!CreateBlitResources()) return false;
    g_started = true;
    return true;
}

// Destroys every session object (also after a partial Start). Game thread at Present, with
// the render thread idle; afterwards no XR call may run until the next Start.
void Shutdown(const char* why) {
    const bool had = g_instance != XR_NULL_HANDLE;
    g_started = false;
    g_running = false;
    g_referenceChanges.Reset();
    if (g_session && g_frameOpen) {
        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
        ei.displayTime = g_lastWaitTime;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        xrEndFrame(g_session, &ei);
    }
    g_frameOpen = false;
    g_beginPending = false;
    for (auto& p : g_packets) p.valid = false;
    for (EyeSwapchain* s : {&g_eyes[0], &g_eyes[1], &g_quad}) {
        for (auto* rtv : s->rtvs)
            if (rtv) rtv->Release();
        s->rtvs.clear();
        s->images.clear();
        if (s->handle) xrDestroySwapchain(s->handle);
        s->handle = XR_NULL_HANDLE;
    }
    // Views on swapchain images must not outlive them; B2SVR-owned textures stay cached.
    DestroyControllerActions();
    if (g_local) xrDestroySpace(g_local);
    if (g_viewSpace) xrDestroySpace(g_viewSpace);
    g_local = g_viewSpace = XR_NULL_HANDLE;
    if (g_session) xrDestroySession(g_session);
    g_session = XR_NULL_HANDLE;
    if (g_instance) xrDestroyInstance(g_instance);
    g_instance = XR_NULL_HANDLE;
    g_system = XR_NULL_SYSTEM_ID;
    g_state = XR_SESSION_STATE_UNKNOWN;
    g_focusedOnce = false;
    g_lost = false;
    g_haveLastViews = false;
    g_haveGoodViews = false;
    g_nextGeometryReport = 0;
    g_recenterRequest = true;
    g_settle.Reset();
    if (had) Log("XR shut down (%s); the game renders flat until the next start", why);
}

void Service(ID3D11Device* device, bool wanted) {
    static ULONGLONG nextTry = 0;
    if (!wanted) {
        if (g_instance) Shutdown("disabled");
        return;
    }
    if (g_started) {
        HandleEvents();
        if (!g_lost) return;
        Shutdown("runtime exiting or session lost");
        nextTry = GetTickCount64() + 5000;
        return;
    }
    if (!device || GetTickCount64() < nextTry) return;
    if (Start(device)) {
        Log("XR started at Present; stereo follows the session");
        return;
    }
    Shutdown("start failed");
    nextTry = GetTickCount64() + 5000;  // headset or runtime not ready: retry quietly
    static int failLogs = 0;
    if (failLogs++ < 3) Log("XR start failed; retrying every 5 s while VR is enabled");
}

void Recenter() { g_recenterRequest = true; }
uint32_t RecenterEpoch() { return g_recenterEpoch; }

bool GetInteractionProfile(int hand, char* output, uint32_t capacity) {
    if (!output || capacity == 0 || hand < 0 || hand > 1 || !g_instance ||
        !g_session || g_handPath[hand] == XR_NULL_PATH) return false;
    output[0] = '\0';
    XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
    if (xrGetCurrentInteractionProfile(g_session, g_handPath[hand], &state) != XR_SUCCESS ||
        state.interactionProfile == XR_NULL_PATH) return false;
    uint32_t length = 0;
    if (xrPathToString(g_instance, state.interactionProfile, capacity, &length, output) != XR_SUCCESS)
        return false;
    output[capacity - 1] = '\0';
    return true;
}

std::atomic<bool> g_uiShotRequest{false};
std::atomic<int> g_uiTestPattern{0};  // 0 off, 1 opaque test pattern on the quad (FAIL-HUD-010)
std::atomic<int> g_uiAlphaMode{3};    // quad alpha: 2 coverage (keeps black overlays), 3 brightness
void RequestUiShot() { g_uiShotRequest = true; }
void ReleaseBackbufferViews() {
    // ResizeBuffers fails while any view on the old backbuffer is alive (EXP-041).
    if (g_bbRtv) g_bbRtv->Release();
    g_bbRtv = nullptr;
    g_bbForRtv = nullptr;
}
std::atomic<bool> g_flatShotRequest{false};
void RequestFlatShot() { g_flatShotRequest = true; }
void SetUiTestPattern(int mode) { g_uiTestPattern = mode; }
void SetUiAlphaMode(int mode) { g_uiAlphaMode = mode == 2 ? 2 : 3; }
void SetSymmetricFov(bool on) { g_symFov = on; }
void SetHeadTestOffset(float dy, float pitchDeg) {
    g_headOffY = dy;
    g_headOffPitch = pitchDeg;
}

// Session log (xr_geometry.h): each eye relative to the head as the runtime reports it, what is
// submitted, and any oddity flagged as a CHECK WARNING. On the first tracked frame of a session,
// then every 30 s.
void ReportViewGeometry(XrTime t) {
    XrView hv[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = t;
    li.space = g_viewSpace;
    uint32_t n = 0;
    if (!XR_SUCCEEDED(xrLocateViews(g_session, &li, &vs, 2, &n, hv)) || n != 2) {
        Log("XR GEOMETRY: views relative to the head unavailable");
        return;
    }
    xrgeom::Eye e[2];
    for (int i = 0; i < 2; ++i)
        e[i] = {Q(hv[i].pose.orientation), V(hv[i].pose.position), hv[i].fov.angleLeft, hv[i].fov.angleRight,
                hv[i].fov.angleUp, hv[i].fov.angleDown};
    const xrgeom::Report r = xrgeom::Evaluate(e[0], e[1]);
    constexpr float kToDeg = 57.2957795f;
    Log("XR GEOMETRY %s, %s (view state 0x%llx)", g_runtimeName, g_systemName, (unsigned long long)vs.viewStateFlags);
    for (int i = 0; i < 2; ++i)
        Log("XR GEOMETRY %s eye: pos=(%+.1f %+.1f %+.1f) mm yaw=%+.2f pitch=%+.2f roll=%+.2f deg "
            "fov left=%.2f right=%.2f up=%.2f down=%.2f deg",
            i ? "right" : "left", e[i].p.x * 1000.f, e[i].p.y * 1000.f, e[i].p.z * 1000.f, r.eye[i].yaw,
            r.eye[i].pitch, r.eye[i].roll, e[i].angleLeft * kToDeg, e[i].angleRight * kToDeg,
            e[i].angleUp * kToDeg, e[i].angleDown * kToDeg);
    Log("XR GEOMETRY eyes: ipd=%.1f mm, right-left height %+.2f mm, depth %+.2f mm, angle between eyes %.2f deg",
        r.ipdMm, r.dyMm, r.dzMm, r.cantDeg);
    if (g_symFov) {
        float h[2], v[2];
        for (int i = 0; i < 2; ++i) {
            h[i] = std::tan(std::max(-e[i].angleLeft, e[i].angleRight));
            v[i] = std::tan(std::max(e[i].angleUp, -e[i].angleDown));
        }
        Log("XR GEOMETRY submitted: symmetric fov, tan left eye +-%.3f x +-%.3f, right eye +-%.3f x +-%.3f; "
            "eye images %dx%d", h[0], v[0], h[1], v[1], g_eyes[0].width, g_eyes[0].height);
    } else {
        Log("XR GEOMETRY submitted: the runtime's own fov; eye images %dx%d", g_eyes[0].width, g_eyes[0].height);
    }
    if (!r.flags) Log("XR CHECK geometry OK");
    for (unsigned b = 0; b < xrgeom::kFlagCount; ++b)
        if (r.flags & (1u << b)) Log("XR CHECK WARNING: %s", xrgeom::FlagText(1u << b));
}

int BeginGameFrame(EyeView eyes[2], FrameInput* input) {
    if (input) *input = FrameInput{};  // also neutral on every early exit
    if (!g_started) return -1;
    HandleEvents();
    if (!g_running || g_beginPending) return -1;  // xrWaitFrame must not run ahead of xrBeginFrame

    XrFrameState fs{XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    if (!Check(xrWaitFrame(g_session, &wi, &fs), "xrWaitFrame")) return -1;

    ServiceRecenter(fs.predictedDisplayTime);

    g_lastWaitTime = fs.predictedDisplayTime;
    g_lastWaitShouldRender = fs.shouldRender == XR_TRUE;
    const int slot = g_nextSlot.fetch_add(1) % kSlots;
    Packet& p = g_packets[slot];
    p.displayTime = fs.predictedDisplayTime;
    p.shouldRender = fs.shouldRender == XR_TRUE;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = fs.predictedDisplayTime;
    li.space = g_local;
    uint32_t nv = 0;
    p.views[0] = {XR_TYPE_VIEW};
    p.views[1] = {XR_TYPE_VIEW};
    if (!XR_SUCCEEDED(xrLocateViews(g_session, &li, &vs, 2, &nv, p.views)) || nv != 2) vs.viewStateFlags = 0;
    constexpr XrViewStateFlags tracked = XR_VIEW_STATE_ORIENTATION_VALID_BIT |
        XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_TRACKED_BIT |
        XR_VIEW_STATE_POSITION_TRACKED_BIT;
    bool trackedViews = (vs.viewStateFlags & tracked) == tracked;
    for (const auto& v : p.views) {
        pose::Quat q = Q(v.pose.orientation);
        const auto& pos = v.pose.position;
        trackedViews = trackedViews && pose::Normalize(q) && std::isfinite(pos.x) &&
            std::isfinite(pos.y) && std::isfinite(pos.z);
    }
    ValidateViews(p.views, vs.viewStateFlags);
    if (trackedViews && GetTickCount64() >= g_nextGeometryReport) {
        g_nextGeometryReport = GetTickCount64() + 30000;
        ReportViewGeometry(fs.predictedDisplayTime);
    }

    // One reference per packet: the eye transforms and the UI panel of this frame use the same
    // recenter generation.
    Mat4 refInv;
    CopyFrameReference(refInv, p.refPose, p.recenterEpoch);
    SampleFrameInput(fs.predictedDisplayTime, refInv, p.recenterEpoch, input, trackedViews);
    // Separate an incorrect seated reference from game-camera/input effects.
    // Log only once per accepted generation, after the same frame's head/eyes
    // have been located using the reference captured by this packet.
    static uint32_t reportedEpoch = 0;
    if (input && input->focused && p.recenterEpoch != reportedEpoch) {
        reportedEpoch = p.recenterEpoch;
        const auto l = pose::Mul(pose::FromPose(Q(p.views[0].pose.orientation), V(p.views[0].pose.position)), refInv);
        const auto r = pose::Mul(pose::FromPose(Q(p.views[1].pose.orientation), V(p.views[1].pose.position)), refInv);
        Log("XR recenter-check epoch=%u headYaw=%.2f headPos=(%.3f %.3f %.3f) eyeYaw=(%.2f %.2f) eyeCentre=(%.3f %.3f %.3f)",
            p.recenterEpoch, std::atan2(input->headRel.m[8], input->headRel.m[10]) * 57.29578f,
            input->headRel.m[12], input->headRel.m[13], input->headRel.m[14],
            std::atan2(l.m[8], l.m[10]) * 57.29578f, std::atan2(r.m[8], r.m[10]) * 57.29578f,
            (l.m[12]+r.m[12])*.5f, (l.m[13]+r.m[13])*.5f, (l.m[14]+r.m[14])*.5f);
    }
    if (g_symFov) {
        // EXP-014 discriminator: render and submit a symmetric frustum that contains the eye's own.
        for (auto& v : p.views) {
            const float h = std::max(-v.fov.angleLeft, v.fov.angleRight);
            const float vv = std::max(v.fov.angleUp, -v.fov.angleDown);
            v.fov = {-h, h, vv, -vv};
        }
    }
    for (int e = 0; e < 2; ++e) {
        const XrView& v = p.views[e];
        // Eye relative to the seated reference, then its inverse post-multiplies the view.
        const Mat4 off = pose::EyeViewOffset(Q(v.pose.orientation), V(v.pose.position), refInv);
        std::memcpy(eyes[e].viewOffset, off.m, sizeof(off.m));
        eyes[e].tanLeft = std::tan(v.fov.angleLeft);
        eyes[e].tanRight = std::tan(v.fov.angleRight);
        eyes[e].tanUp = std::tan(v.fov.angleUp);
        eyes[e].tanDown = std::tan(v.fov.angleDown);
    }
    p.valid = true;
    g_beginPending = true;
    return slot;
}

void EndFrameWithLayers(Packet& p, const XrCompositionLayerQuad* quad, bool sceneHidden = false,
                        const XrCompositionLayerQuad* extra = nullptr);

namespace {
// Diagnostic (owner 0.16.27: "in F3, going through doors the whole screen goes black for a second";
// LIVE: no frame-time spike at those moments, so something is drawn black). The left eye image is
// sampled 32x18 and read back a frame later without waiting; a view darker than 3% average for more
// than 0.1 s is logged with its duration, next to the FP state lines.
struct EyeDarkness {
    ID3D11Texture2D* tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11Texture2D* stage[2] = {};
    bool pending[2] = {};
    uint64_t seq[2] = {}, counter = 0;
    uint64_t darkSince = 0;
    bool reported = false;
    float darkest = 1.f;
} g_eyeDark;
std::atomic<uint64_t> g_darkTraceUntil{0};
std::atomic<bool> g_viewDark{false};
int g_darkSpells = 0;

void SampleEyeDarkness(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, uint32_t top, uint32_t height) {
    EyeDarkness& e = g_eyeDark;
    if (!e.tex) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = 32;
        d.Height = 18;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(g_device->CreateTexture2D(&d, nullptr, &e.tex))) return;
        g_device->CreateRenderTargetView(e.tex, nullptr, &e.rtv);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& s : e.stage) g_device->CreateTexture2D(&d, nullptr, &s);
    }
    if (!e.rtv || !e.stage[0] || !e.stage[1]) return;
    float avg = -1.f;
    uint64_t newest = 0;
    for (int i = 0; i < 2; ++i) {
        if (!e.pending[i]) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(e.stage[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) continue;
        uint32_t sum = 0;
        for (UINT y = 0; y < 18; ++y) {
            const uint8_t* row = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
            for (UINT x = 0; x < 32; ++x) sum += std::max(row[x * 4], std::max(row[x * 4 + 1], row[x * 4 + 2]));
        }
        ctx->Unmap(e.stage[i], 0);
        e.pending[i] = false;
        if (e.seq[i] > newest) {
            newest = e.seq[i];
            avg = float(sum) / (32.f * 18.f * 255.f);
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (e.pending[i]) continue;
        Blit(ctx, src, top, height, e.rtv, 32, 18);
        ctx->CopyResource(e.stage[i], e.tex);
        e.pending[i] = true;
        e.seq[i] = ++e.counter;
        break;
    }
    if (avg < 0.f) return;
    const uint64_t now = GetTickCount64();
    g_viewDark.store(avg < 0.03f, std::memory_order_relaxed);
    if (avg < 0.03f) {
        if (!e.darkSince) {
            e.darkSince = now;
            e.darkest = avg;
            if (++g_darkSpells <= 6) Log("XR view dark (%.1f%%): per-frame DARKTRACE on", avg * 100.f);
        }
        if (g_darkSpells <= 6) g_darkTraceUntil.store(now + 300, std::memory_order_relaxed);
        e.darkest = std::min(e.darkest, avg);
        if (!e.reported && now - e.darkSince >= 100) {
            e.reported = true;
            Log("XR view went dark (left eye %.1f%% brightness)", avg * 100.f);
        }
    } else if (e.darkSince) {
        if (e.reported)
            Log("XR view dark for %llu ms (darkest %.1f%%), bright again", static_cast<unsigned long long>(now - e.darkSince),
                e.darkest * 100.f);
        e.darkSince = 0;
        e.reported = false;
    }
}
}  // namespace

bool ViewDark() { return g_viewDark.load(std::memory_order_relaxed); }
bool DarkTraceActive() { return GetTickCount64() < g_darkTraceUntil.load(std::memory_order_relaxed); }

void OnEye(int eye, int slot, ID3D11DeviceContext* ctx, ID3D11Texture2D* src, uint32_t srcTop, uint32_t srcHeight,
           bool uiFollows) {
    if (!g_started || slot < 0 || slot >= kSlots) return;
    Packet& p = g_packets[slot];
    if (!p.valid) return;
    if (eye == 0) {
        if (g_frameOpen) {  // a pair never finished: close it empty so begin/end stay paired
            XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
            ei.displayTime = p.displayTime;
            ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            xrEndFrame(g_session, &ei);
            g_frameOpen = false;
        }
        XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
        const XrResult r = xrBeginFrame(g_session, &bi);
        g_beginPending = false;
        if (!XR_SUCCEEDED(r)) {
            Check(r, "xrBeginFrame");
            p.valid = false;
            return;
        }
        g_frameOpen = true;
    }
    if (!g_frameOpen) return;

    g_lastEyeTop = srcTop;
    g_lastEyeHeight = srcHeight;
    if (p.shouldRender) {
        EyeSwapchain& s = g_eyes[eye];
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_SUCCEEDED(xrAcquireSwapchainImage(s.handle, &ai, &idx))) {
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = XR_INFINITE_DURATION;
            xrWaitSwapchainImage(s.handle, &wi);
            Blit(ctx, src, srcTop, srcHeight, s.rtvs[idx], s.width, s.height, 0, false, g_linearizeInShader);
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(s.handle, &ri);
        }
        if (eye == 0) SampleEyeDarkness(ctx, src, srcTop, srcHeight);
    }

    if (eye == 1 && !uiFollows) EndFrameWithLayers(p, nullptr);
}

// sceneHidden: only the panel is shown (a full-screen video in the 2D layer), in black space.
void EndFrameWithLayers(Packet& p, const XrCompositionLayerQuad* quad, bool sceneHidden,
                        const XrCompositionLayerQuad* extra) {
    {
        XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                  {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        for (int e = 0; e < 2; ++e) {
            pv[e].pose = p.views[e].pose;
            pv[e].fov = p.views[e].fov;
            pv[e].subImage.swapchain = g_eyes[e].handle;
            pv[e].subImage.imageRect = {{0, 0}, {g_eyes[e].width, g_eyes[e].height}};
        }
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        layer.space = g_local;
        layer.viewCount = 2;
        layer.views = pv;
        const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer),
                                                        reinterpret_cast<const XrCompositionLayerBaseHeader*>(quad),
                                                        reinterpret_cast<const XrCompositionLayerBaseHeader*>(extra)};
        const bool panelOnly = sceneHidden && quad;
        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
        ei.displayTime = p.displayTime;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        ei.layerCount = p.shouldRender ? (panelOnly ? 1 : quad ? (extra ? 3 : 2) : 1) : 0;
        ei.layers = panelOnly ? layers + 1 : layers;
        const XrResult r = xrEndFrame(g_session, &ei);
        if (p.shouldRender) {
            g_lastViews[0] = p.views[0];
            g_lastViews[1] = p.views[1];
            g_haveLastViews = true;
        }
        g_frameOpen = false;
        p.valid = false;
        const uint64_t n = g_framesEnded++;
        if (!XR_SUCCEEDED(r) || n < 3 || n % 900 == 0) Log("XR frame %llu ended: %d", n, int(r));
    }
}


namespace {
// Saves an RGBA8 texture as two half-size BMPs next to the game: <name>_rgb.bmp and
// <name>_alpha.bmp, plus coverage statistics in the log (playbook: inspect isolated RGBA).
void SaveRgba(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const char* name) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &st))) return;
    ctx->CopyResource(st, tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        st->Release();
        return;
    }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    const UINT w = d.Width / 2, h = d.Height / 2, stride = (w * 3 + 3) & ~3u;
    uint64_t alphaPx = 0, colorPx = 0;
    for (int pass = 0; pass < 2; ++pass) {
        std::string n(name);
        n += pass == 0 ? "_rgb.bmp" : "_alpha.bmp";
        const std::wstring path = dir + L"\\" + std::wstring(n.begin(), n.end());
        FILE* f = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
        if (!f) continue;
        BITMAPFILEHEADER fh{};
        BITMAPINFOHEADER ih{};
        ih.biSize = sizeof(ih);
        ih.biWidth = LONG(w);
        ih.biHeight = -LONG(h);
        ih.biPlanes = 1;
        ih.biBitCount = 24;
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = fh.bfOffBits + stride * h;
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&ih, sizeof(ih), 1, f);
        std::string row(stride, '\0');
        for (UINT y = 0; y < h; ++y) {
            const uint8_t* src = static_cast<uint8_t*>(m.pData) + size_t(y) * 2 * m.RowPitch;
            for (UINT xx = 0; xx < w; ++xx) {
                const uint8_t* p = src + size_t(xx) * 2 * 4;
                if (pass == 0) {
                    row[xx * 3 + 0] = char(p[2]);
                    row[xx * 3 + 1] = char(p[1]);
                    row[xx * 3 + 2] = char(p[0]);
                    colorPx += (p[0] | p[1] | p[2]) != 0;
                } else {
                    row[xx * 3 + 0] = row[xx * 3 + 1] = row[xx * 3 + 2] = char(p[3]);
                    alphaPx += p[3] != 0;
                }
            }
            fwrite(row.data(), 1, stride, f);
        }
        fclose(f);
    }
    ctx->Unmap(st, 0);
    st->Release();
    Log("XR uishot %s: %ux%u fmt=%d nonblack=%llu nonzeroAlpha=%llu (of %u)", name, d.Width, d.Height, int(d.Format),
        colorPx, alphaPx, w * h);
}

XrCompositionLayerQuad MakeQuad(const XrPosef& ref) {
    XrCompositionLayerQuad q{XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.space = g_local;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = g_quad.handle;
    q.subImage.imageRect = {{0, 0}, {g_quad.width, g_quad.height}};
    // In front of the seated reference, facing it (the reference is yaw-only).
    const float s = 2.f * ref.orientation.y * ref.orientation.w, c = 1.f - 2.f * ref.orientation.y * ref.orientation.y;
    q.pose.orientation = ref.orientation;
    q.pose.position = {ref.position.x - s * kQuadDistance, ref.position.y, ref.position.z - c * kQuadDistance};
    q.size = {kQuadWidth, kQuadWidth * float(g_quad.height) / float(g_quad.width)};
    return q;
}

bool FillQuad(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, int alphaMode) {
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!XR_SUCCEEDED(xrAcquireSwapchainImage(g_quad.handle, &ai, &idx))) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(g_quad.handle, &wi);
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    Blit(ctx, src, 0, d.Height, g_quad.rtvs[idx], g_quad.width, g_quad.height, alphaMode, false, g_linearizeInShader);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g_quad.handle, &ri);
    return true;
}

// ---- a full-screen video in the 2D layer ("flat cutscene") ----
// Owner 0.16.9: a video played inside a cutscene showed as a see-through panel over the 3D scene
// (mode-3 panel alpha is the pixel brightness). While Bink decodes and most of the 2D layer is
// lit, the layer is that video: the panel goes opaque and the 3D scene is left out, as for videos
// with nothing 3D behind them (FlatFrame). Black fades and menus never qualify (no video decodes).
constexpr UINT kCovW = 32, kCovH = 18;
ID3D11Texture2D* g_covTex = nullptr;
ID3D11RenderTargetView* g_covRtv = nullptr;
ID3D11Texture2D* g_covStage[2] = {};
bool g_covPending[2] = {};
uint64_t g_covSeq[2] = {}, g_covCounter = 0;
uint8_t g_covGrid[kCovH][kCovW] = {};  // lit cells of the newest reading
bool g_videoMode = false, g_wasDecoding = false, g_videoShotTaken = false;
uint64_t g_videoDarkSince = 0, g_videoDiagAt = 0, g_decodeSince = 0;

bool EnsureCoverage() {
    if (!g_covTex) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = kCovW;
        d.Height = kCovH;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(g_device->CreateTexture2D(&d, nullptr, &g_covTex))) return false;
        g_device->CreateRenderTargetView(g_covTex, nullptr, &g_covRtv);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& s : g_covStage) g_device->CreateTexture2D(&d, nullptr, &s);
    }
    return g_covRtv && g_covStage[0] && g_covStage[1];
}

// Fraction of the 2D layer that is not black: a 32x18 sample, read back a frame or two later
// without waiting on the GPU. -1 while no reading is ready. (0.16.10 used a fixed read slot: one
// "still drawing" answer left a copy pending forever and the check stopped reading, LIVE.)
float UiCoverage(ID3D11DeviceContext* ctx, ID3D11Texture2D* ui) {
    if (!EnsureCoverage()) return -1.f;
    float result = -1.f;
    uint64_t newest = 0;
    for (int i = 0; i < 2; ++i) {
        if (!g_covPending[i]) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(g_covStage[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) continue;
        UINT lit = 0;
        uint8_t grid[kCovH][kCovW];
        for (UINT y = 0; y < kCovH; ++y) {
            const uint8_t* row = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
            for (UINT x = 0; x < kCovW; ++x) {
                grid[y][x] = std::max(row[x * 4], std::max(row[x * 4 + 1], row[x * 4 + 2])) > 16;
                lit += grid[y][x];
            }
        }
        ctx->Unmap(g_covStage[i], 0);
        g_covPending[i] = false;
        if (g_covSeq[i] > newest) {
            newest = g_covSeq[i];
            result = float(lit) / float(kCovW * kCovH);
            std::memcpy(g_covGrid, grid, sizeof(grid));
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (g_covPending[i]) continue;
        D3D11_TEXTURE2D_DESC d{};
        ui->GetDesc(&d);
        Blit(ctx, ui, 0, d.Height, g_covRtv, int(kCovW), int(kCovH), 1);
        ctx->CopyResource(g_covStage[i], g_covTex);
        g_covPending[i] = true;
        g_covSeq[i] = ++g_covCounter;
        break;
    }
    return result;
}

// Enters on a decoding video with >= 30% of the layer lit; leaves when decoding stops, or after
// 3 s of an almost black layer (the video ended in-world or behind the scene).
bool UpdateVideoMode(ID3D11DeviceContext* ctx, ID3D11Texture2D* ui) {
    const uint64_t now = GetTickCount64();
    const bool decoding = videowatch::Playing(now);
    if (decoding != g_wasDecoding) {
        g_wasDecoding = decoding;
        g_decodeSince = now;
        Log("XR video decoding %s (panel %s)", decoding ? "started" : "stopped", g_videoMode ? "video" : "UI");
    }
    if (!decoding && !g_videoMode) return false;
    const float cover = UiCoverage(ctx, ui);
    if (!g_videoMode) {
        if (cover >= 0.3f) {
            g_videoMode = true;
            g_videoDarkSince = 0;
            Log("XR video in the 2D layer (%.0f%% lit): panel opaque, 3D scene hidden", cover * 100.f);
        } else if (cover >= 0.f && now - g_videoDiagAt >= (now - g_decodeSince < 5000 ? 1000u : 30000u)) {
            // every second for the first 5 s of a decode, then every 30 s (LIVE 0.16.12: an in-world
            // video in the DPA rooms decoded for minutes at 0% lit)
            g_videoDiagAt = now;
            Log("XR video decoding, 2D layer %.0f%% lit (not full-screen: panel unchanged)", cover * 100.f);
            // Once per run: the 2D layer itself (b2svr_uicap_rgb / _alpha.bmp) to see how it is drawn.
            if (!g_videoShotTaken && now - g_decodeSince >= 1000) {
                g_videoShotTaken = true;
                g_uiShotRequest = true;
            }
        }
        return g_videoMode;
    }
    if (cover >= 0.f) g_videoDarkSince = cover < 0.1f ? (g_videoDarkSince ? g_videoDarkSince : now) : 0;
    if (!decoding || (g_videoDarkSince && now - g_videoDarkSince >= 3000)) {
        g_videoMode = false;
        Log("XR video in the 2D layer ended (%s): back to the 3D scene", decoding ? "dark for 3 s" : "decoding stopped");
    }
    return g_videoMode;
}

}  // namespace

void OnUiBegin(int slot, ID3D11DeviceContext* ctx, ID3D11Texture2D* backbuffer) {
    if (!g_started || slot < 0 || slot >= kSlots || !g_packets[slot].valid || !g_frameOpen) return;
    // Display2D will draw the UI over transparent black; OnUiEnd lifts it onto the quad.
    const float clear[4] = {0, 0, 0, 0};
    if (ID3D11RenderTargetView* rtv = BackbufferRtv(backbuffer)) ctx->ClearRenderTargetView(rtv, clear);
}

void OnUiEnd(int slot, ID3D11DeviceContext* ctx, ID3D11Texture2D* backbuffer, ID3D11Texture2D* eyeImage) {
    if (!g_started || slot < 0 || slot >= kSlots) return;
    Packet& p = g_packets[slot];
    if (!p.valid || !g_frameOpen) return;
    D3D11_TEXTURE2D_DESC d{};
    backbuffer->GetDesc(&d);
    if (g_uiTex) {
        // The backbuffer changes size with the render size (b2svr.ini render_size): a copy between
        // different sizes does nothing, and the panel would keep the old UI (FAIL-STR-054).
        D3D11_TEXTURE2D_DESC ud{};
        g_uiTex->GetDesc(&ud);
        if (ud.Width != d.Width || ud.Height != d.Height || ud.Format != d.Format) DropTexture(g_uiTex);
    }
    if (!g_uiTex) {
        D3D11_TEXTURE2D_DESC ud = d;
        ud.Usage = D3D11_USAGE_DEFAULT;
        ud.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ud.CPUAccessFlags = 0;
        ud.MiscFlags = 0;
        g_device->CreateTexture2D(&ud, nullptr, &g_uiTex);
    }
    bool haveQuad = false, video = false;
    if (g_uiTex) {
        ctx->CopyResource(g_uiTex, backbuffer);
        if (g_uiShotRequest.exchange(false)) SaveRgba(ctx, g_uiTex, "b2svr_uicap");
        video = UpdateVideoMode(ctx, g_uiTex);
        // FAIL-HUD-010 discriminator: an opaque, known image on the quad proves the layer path
        // independently of the capture.
        // The headset panel drops black overlays (mode 3); the flat mirror below keeps them.
        if (p.shouldRender) haveQuad = FillQuad(ctx, g_uiTex, g_uiTestPattern || video ? 0 : g_uiAlphaMode.load());
        // Restore the flat mirror: the last eye's scene, then the UI composited on top.
        if (ID3D11RenderTargetView* rtv = BackbufferRtv(backbuffer)) {
            if (eyeImage) Blit(ctx, eyeImage, 0, d.Height, rtv, int(d.Width), int(d.Height), 0, false, false);
            Blit(ctx, g_uiTex, 0, d.Height, rtv, int(d.Width), int(d.Height), 2, true, false);
        }
    }
    XrCompositionLayerQuad quad = MakeQuad(p.refPose);
    if (video) quad.layerFlags = 0;  // the video opaque, like the flat panel
    static uint64_t uiFrames = 0;
    if (uiFrames++ % 900 == 0)
        Log("XR ui frame %llu: quad=%d shouldRender=%d video=%d pose=(%.2f %.2f %.2f) q=(%.2f %.2f %.2f %.2f) size=%.2fx%.2f",
            uiFrames, int(haveQuad), int(p.shouldRender), int(video), quad.pose.position.x, quad.pose.position.y,
            quad.pose.position.z, quad.pose.orientation.x, quad.pose.orientation.y, quad.pose.orientation.z,
            quad.pose.orientation.w, quad.size.width, quad.size.height);
    EndFrameWithLayers(p, haveQuad ? &quad : nullptr, video && haveQuad);
}

bool FlatFrame(ID3D11DeviceContext* ctx, ID3D11Texture2D* backbuffer, ID3D11Texture2D* const lastEyes[2],
               bool showPanel, FrameInput* input) {
    if (input) *input = FrameInput{};
    if (!g_started) return false;
    HandleEvents();
    if (!g_running) return false;
    if (g_frameOpen) {  // a stereo pair that never finished: close it empty
        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
        ei.displayTime = g_lastWaitTime;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        xrEndFrame(g_session, &ei);
        g_frameOpen = false;
    }
    // A wait whose stereo pair was never recorded is begun here; otherwise wait now, which
    // also paces 2D-only screens to the headset.
    if (!g_beginPending) {
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
        if (!Check(xrWaitFrame(g_session, &wi, &fs), "xrWaitFrame (flat)")) return false;
        g_lastWaitTime = fs.predictedDisplayTime;
        g_lastWaitShouldRender = fs.shouldRender == XR_TRUE;
        ServiceRecenter(fs.predictedDisplayTime);
    }
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    const XrResult r = xrBeginFrame(g_session, &bi);
    g_beginPending = false;
    if (!Check(r, "xrBeginFrame (flat)")) return false;
    Mat4 refInv;
    XrPosef refPose;
    uint32_t epoch;
    CopyFrameReference(refInv, refPose, epoch);
    // Input always uses a live head/hand locate, even when the backdrop reuses old eyes.
    SampleFrameInput(g_lastWaitTime, refInv, epoch, input);
    if (showPanel && g_flatShotRequest.exchange(false)) {
        SaveRgba(ctx, backbuffer, "b2svr_flat_bb");
        if (lastEyes[0]) SaveRgba(ctx, lastEyes[0], "b2svr_flat_eye0");
        Log("XR flatshot: band top=%u h=%u haveViews=%d shouldRender=%d", g_lastEyeTop, g_lastEyeHeight,
            int(g_haveLastViews), int(g_lastWaitShouldRender));
    }
    const XrCompositionLayerBaseHeader* layers[2] = {};
    uint32_t layerCount = 0;
    // A stalled stereo pair re-shows the last pair with the poses it was rendered for (world-
    // locked). 2D-only screens (intro, videos, pause menu) get no backdrop: the flat screen
    // floats in black space.
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                              {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    if (!showPanel && g_lastWaitShouldRender && g_haveLastViews && lastEyes[0] && lastEyes[1] && g_lastEyeHeight) {
        bool ok = true;
        for (int e = 0; e < 2 && ok; ++e) {
            EyeSwapchain& s = g_eyes[e];
            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            ok = XR_SUCCEEDED(xrAcquireSwapchainImage(s.handle, &ai, &idx));
            if (!ok) break;
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = XR_INFINITE_DURATION;
            xrWaitSwapchainImage(s.handle, &wi);
            Blit(ctx, lastEyes[e], g_lastEyeTop, g_lastEyeHeight, s.rtvs[idx], s.width, s.height, 0, false,
                 g_linearizeInShader);
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(s.handle, &ri);
            pv[e].pose = g_lastViews[e].pose;
            pv[e].fov = g_lastViews[e].fov;
            pv[e].subImage.swapchain = s.handle;
            pv[e].subImage.imageRect = {{0, 0}, {s.width, s.height}};
        }
        if (ok) {
            proj.space = g_local;
            proj.viewCount = 2;
            proj.views = pv;
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
        }
    }
    XrCompositionLayerQuad quad = MakeQuad(refPose);
    bool haveQuad = false;
    ID3D11Texture2D* flatSrc = showPanel && g_lastWaitShouldRender ? FlatSource(ctx, backbuffer) : nullptr;
    if (flatSrc) {
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_SUCCEEDED(xrAcquireSwapchainImage(g_quad.handle, &ai, &idx))) {
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = XR_INFINITE_DURATION;
            xrWaitSwapchainImage(g_quad.handle, &wi);
            D3D11_TEXTURE2D_DESC d{};
            flatSrc->GetDesc(&d);
            Blit(ctx, flatSrc, 0, d.Height, g_quad.rtvs[idx], g_quad.width, g_quad.height, 0, false,
                 g_linearizeInShader, 0.f, 0.004f);
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(g_quad.handle, &ri);
            haveQuad = true;
        }
        // The whole flat frame, opaque; the grey border keeps its edges visible on dark screens.
        quad.layerFlags = 0;
        if (haveQuad) layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
    }
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = g_lastWaitTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layerCount;
    ei.layers = layers;
    const XrResult er = xrEndFrame(g_session, &ei);
    g_framesEnded++;
    static uint64_t flatFrames = 0;
    if (!XR_SUCCEEDED(er) || flatFrames++ % 900 == 0)
        Log("XR flat frame %llu ended: %d (layers=%u panel=%d)", flatFrames, int(er), layerCount, int(haveQuad));
    if (!XR_SUCCEEDED(er) && input) *input = FrameInput{};
    return XR_SUCCEEDED(er);
}

}  // namespace b2s::xr

