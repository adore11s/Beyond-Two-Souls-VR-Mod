// VR settings panel (panel.h): the GoW VR mod's GDI panel (its vr/panel.cpp, MIT, same owner) on profile
// B2S_ELEVEN_ROW_V6 (1.0.3, playbook revision 23, panel_math.h): the GOW_STYLE_NINE_ROW_V6 rows and geometry, the V6
// look (patterned grey background, the built-in Montserrat, brighter labels / hints), the owner's light blue of the
// Beyond: Two Souls cover (2026-10-27, sampled from the Steam library capsule: light blue median #6BA2C7, light end
// #8DBEDE) where GoW has red, and two B2S rows: "Aiden possessions (first person)" (EXP-046) and "Camera closer (third
// person)" (owner 2026-11-15). No "Hand smoothing" row (revision 13; b2svr.ini hand_smoothing). Rows bind to B2S
// capabilities; a choice whose capability is not there is drawn disabled and does nothing, rather than a
// working-looking button.
#include "panel.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "fpcam.h"
#include "log.h"
#include "rendersize.h"
#include "stereo.h"
#include "vrinput.h"
#include "welcome_draw.h"

namespace b2s::panel {
namespace {

constexpr int kW = kWidthPx, kH = kHeightPx;

enum Action {
    kNone, kF1On, kF1Off, kF3On, kF3Off, kMotionOn, kMotionOff, kTurnSmooth, kTurnSnap, kStepDown, kStepUp,
    kPadXbox, kPadPs, kSizeDown, kSizeUp, kNative, kAfr, kMono, kVrOn, kVrOff, kSmoothOn, kSmoothOff, kPossOn, kPossOff,
    kPushDown, kPushUp
};
struct Button {
    RECT r;
    Action a;
    const wchar_t* text;
    bool selected;
    bool enabled;
};

// GDI surface
HDC g_dc = nullptr;
HBITMAP g_bmp = nullptr;
uint32_t* g_bits = nullptr;
HFONT g_fTitle = nullptr, g_fLabel = nullptr, g_fButton = nullptr, g_fHint = nullptr;
uint32_t g_out[kW * kH];
uint32_t g_bgPix[kW * kH];  // V6 background pattern
uint32_t g_cursor[kCursorPx * kCursorPx];
bool g_cursorMade = false;

Button g_buttons[28];
int g_nButtons = 0;
Action g_hover = kNone;
bool g_needDraw = true;
char g_lastState[160] = "";

std::atomic<bool> g_capture[2] = {false, false};
TriggerClick g_click[2];

void Apply(Action a) {
    switch (a) {
        case kF1On: fpcam::SetGameplayFP(true); break;
        case kF1Off: fpcam::SetGameplayFP(false); break;
        case kF3On: fpcam::SetCutsceneFP(true); break;
        case kF3Off: fpcam::SetCutsceneFP(false); break;
        case kMotionOn: fpcam::SetMotionControls(true); break;
        case kMotionOff: fpcam::SetMotionControls(false); break;
        case kSmoothOn: fpcam::SetHandSmoothing(true); break;
        case kSmoothOff: fpcam::SetHandSmoothing(false); break;
        case kPossOn: fpcam::SetPossessionFP(true); break;
        case kPossOff: fpcam::SetPossessionFP(false); break;
        case kTurnSmooth: fpcam::SetSnapTurn(false); break;
        case kTurnSnap: fpcam::SetSnapTurn(true); break;
        case kStepDown:
        case kStepUp: {
            const int d = a == kStepUp ? 1 : -1;
            if (fpcam::SnapTurn()) fpcam::SetSnapDegrees(StepPreset(kSnaps, fpcam::SnapDegrees(), d));
            else fpcam::SetTurnSpeed(StepPreset(kSpeeds, fpcam::TurnSpeed(), d));
            break;
        }
        case kPushDown:
        case kPushUp: stereo::SetCameraPush(StepPreset(kPushes, stereo::CameraPush(), a == kPushUp ? 1 : -1)); break;
        case kPadXbox: vrinput::SetPadType(2); break;
        case kPadPs: vrinput::SetPadType(5); break;
        case kSizeDown:
        case kSizeUp:
            rendersize::SetSetting(StepSize(rendersize::Setting(), a == kSizeUp ? 1 : -1, int(rendersize::AutoSize())));
            break;
        case kNative: stereo::SetMethod(stereo::kNative); break;
        case kAfr: stereo::SetMethod(stereo::kAfr); break;
        case kMono: stereo::SetMethod(stereo::kMono); break;
        case kVrOn: break;                              // VR is on while the panel shows
        case kVrOff: stereo::RequestVr(false); break;  // as F9; F9 turns it back on
        default: return;
    }
    g_needDraw = true;
    Log("PANEL action %d -> F1 %d, F3 %d, motion controls %d, hand smoothing %d, turning %s %.0f %% / %.0f deg, pad %s, "
        "picture size %d, %s, camera closer %.2f m",
        int(a), int(fpcam::GameplayFP()), int(fpcam::CutsceneFP()), int(fpcam::MotionControls()),
        int(fpcam::HandSmoothing()), fpcam::SnapTurn() ? "snap" : "smooth", fpcam::TurnSpeed() * 100.f, fpcam::SnapDegrees(),
        vrinput::PadType() == 5 ? "PlayStation" : "Xbox", rendersize::Setting(), stereo::MethodName(stereo::GetMethod()),
        stereo::CameraPush());
}

bool MakeSurface() {
    if (g_dc) return g_bits != nullptr;
    g_dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = kW;
    bi.bmiHeader.biHeight = -kH;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    g_bmp = CreateDIBSection(g_dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!g_bmp || !bits) {
        Log("PANEL DIB section failed");
        return false;
    }
    g_bits = static_cast<uint32_t*>(bits);
    SelectObject(g_dc, g_bmp);
    auto font = [](int px, int weight, const wchar_t* face) {
        return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
    };
    // V6 (playbook revision 20): Montserrat (SIL OFL 1.1) built into the DLL (RCDATA 3101 Bold, 3102 SemiBold, 3103
    // Regular), added for this process only - the same on every PC, never a system font.
    const bool mont = welcome::LoadPanelFonts();
    if (mont) {  // GDI faces: "Montserrat" Bold / Regular, "Montserrat SemiBold" (its own family)
        g_fTitle = font(50, FW_BOLD, L"Montserrat");
        g_fLabel = font(32, FW_SEMIBOLD, L"Montserrat SemiBold");
        g_fButton = font(34, FW_BOLD, L"Montserrat");
        g_fHint = font(26, FW_NORMAL, L"Montserrat");
    } else {
        g_fTitle = font(54, FW_BOLD, L"Segoe UI");
        g_fLabel = font(36, FW_SEMIBOLD, L"Segoe UI");
        g_fButton = font(38, FW_SEMIBOLD, L"Segoe UI");
        g_fHint = font(28, FW_NORMAL, L"Segoe UI");
    }
    Log("PANEL font %s", mont ? "Montserrat (built in)" : "Segoe UI (the built-in Montserrat did not load)");
    // V6: the patterned grey, computed once and copied under every redraw.
    for (int py = 0; py < kH; ++py)
        for (int px = 0; px < kW; ++px) g_bgPix[py * kW + px] = welcome::PatternPixel(px, py);
    SetBkMode(g_dc, TRANSPARENT);
    return true;
}

void Fill(const RECT& r, COLORREF c, int round = 0) {
    HBRUSH b = CreateSolidBrush(c);
    if (round > 0) {
        HGDIOBJ ob = SelectObject(g_dc, b), op = SelectObject(g_dc, GetStockObject(NULL_PEN));
        RoundRect(g_dc, r.left, r.top, r.right + 1, r.bottom + 1, round, round);
        SelectObject(g_dc, ob);
        SelectObject(g_dc, op);
    } else {
        FillRect(g_dc, &r, b);
    }
    DeleteObject(b);
}

void Text(const wchar_t* s, RECT r, HFONT f, COLORREF c, UINT fmt) {
    SelectObject(g_dc, f);
    SetTextColor(g_dc, c);
    DrawTextW(g_dc, s, -1, &r, fmt | DT_SINGLELINE | DT_NOPREFIX);
}

void AddButton(int x, int y, int w, int h, Action a, const wchar_t* text, bool selected, bool enabled) {
    if (g_nButtons < int(sizeof(g_buttons) / sizeof(g_buttons[0])))
        g_buttons[g_nButtons++] = {{x, y, x + w, y + h}, a, text, selected, enabled};
}

// A row: a label, then two or three choices or "- value +". B2S_ELEVEN_ROW_V2 order (zero-based index -> y).
struct Row {
    const wchar_t* label;
    Action a, b, c;               // the choices (c: a third, Rendering method only), or the stepper's down / up
    const wchar_t *ta, *tb, *tc;  // their texts (ta nullptr: a stepper; tc nullptr: two choices)
    int sel;                      // the selected choice: 0 a, 1 b, 2 c (choices only)
    int value;                    // stepper: 0 = turning step, 1 = picture size, 2 = camera closer
    bool enabledA, enabledB, enabledC;
};
int Rows(Row* r) {
    const bool snap = fpcam::SnapTurn();
    int n = 0;
    auto two = [](const wchar_t* label, Action a, Action b, const wchar_t* ta, const wchar_t* tb, bool selA) {
        return Row{label, a, b, kNone, ta, tb, nullptr, selA ? 0 : 1, 0, true, true, false};
    };
    r[n++] = two(L"First person (walking)", kF1On, kF1Off, L"On", L"Off", fpcam::GameplayFP());
    r[n++] = two(L"First person (cutscenes)", kF3On, kF3Off, L"On", L"Off", fpcam::CutsceneFP());
    // EXP-046: a body Aiden possesses becomes the first-person body (with F1 on: head hidden, body turned to the
    // view, the arms on the controllers when motion controls are on).
    r[n++] = two(L"Aiden possessions (first person)", kPossOn, kPossOff, L"On", L"Off", fpcam::PossessionFP());
    r[n++] = two(L"Motion controls (first person)", kMotionOn, kMotionOff, L"On", L"Off", fpcam::MotionControls());
    // No "Hand smoothing" row (playbook revision 13, the owner's rule for every port): b2svr.ini hand_smoothing only.
    r[n++] = two(L"Turning (first person)", kTurnSmooth, kTurnSnap, L"Smooth", L"Snap", !snap);
    r[n++] = {snap ? L"Snap angle" : L"Turning speed", kStepDown, kStepUp, kNone, nullptr, nullptr, nullptr, 0, 0, true, true,
              false};
    // 1.0.3 (owner): the game's own camera moved toward the characters (third person only: first person has its own
    // eye), Off .. 2 m.
    constexpr float kPushTop = kPushes[sizeof(kPushes) / sizeof(kPushes[0]) - 1];
    r[n++] = {L"Camera closer (third person)", kPushDown, kPushUp, kNone, nullptr, nullptr, nullptr, 0, 2,
              stereo::CameraPush() > 0.001f, stereo::CameraPush() < kPushTop - 0.001f, false};
    r[n++] = two(L"Controller buttons", kPadXbox, kPadPs, L"Xbox", L"PlayStation", vrinput::PadType() != 5);
    // '+' at Auto and '-' at the game's own size do nothing (panel_math.h StepSize).
    const int size = rendersize::Setting();
    r[n++] = {L"VR picture size (per eye)", kSizeDown, kSizeUp, kNone, nullptr, nullptr, nullptr, 0, 1,
              rendersize::Available() && size != kGame, rendersize::Available() && size != kAuto, false};
    // Playbook revision 6 (18.7): Native | AFR | Mono. AFR and Mono are drawn disabled while this port has no
    // single-pass path (stereo::AfrSupported; Mono is built on it).
    const int method = stereo::GetMethod();
    r[n++] = {L"Rendering method", kNative, kAfr, kMono, L"Native", L"AFR", L"Mono",
              method == stereo::kNative ? 0 : method == stereo::kMono ? 2 : 1, 0, true, stereo::AfrSupported(),
              stereo::AfrSupported()};
    r[n++] = two(L"VR on / off  (Crashing during cutscene)", kVrOn, kVrOff, L"On", L"Off", true);
    return n;
}

void Layout(wchar_t* stepText, wchar_t* sizeText, wchar_t* pushText) {
    g_nButtons = 0;
    Row rows[kRows];
    const int n = Rows(rows);
    for (int i = 0; i < n; ++i) {
        const Row& r = rows[i];
        const int y = kTop + i * kRow + kButtonY;
        if (r.tc) {  // three choices (revision 6 tokens): x = 40, 360, 680, 280 wide
            AddButton(40, y, 280, kButtonH, r.a, r.ta, r.sel == 0, r.enabledA);
            AddButton(360, y, 280, kButtonH, r.b, r.tb, r.sel == 1, r.enabledB);
            AddButton(680, y, 280, kButtonH, r.c, r.tc, r.sel == 2, r.enabledC);
        } else if (r.ta) {
            AddButton(40, y, 440, kButtonH, r.a, r.ta, r.sel == 0, r.enabledA);
            AddButton(520, y, 440, kButtonH, r.b, r.tb, r.sel == 1, r.enabledB);
        } else {
            AddButton(40, y, 140, kButtonH, r.a, L"\x2212", false, r.enabledA);
            AddButton(820, y, 140, kButtonH, r.b, L"+", false, r.enabledB);
        }
    }
    const float push = stereo::CameraPush();
    if (push < 0.001f) swprintf_s(pushText, 32, L"Off");
    else swprintf_s(pushText, 32, L"%.2f m", push);
    if (fpcam::SnapTurn()) swprintf_s(stepText, 32, L"%.0f\x00b0", fpcam::SnapDegrees());
    else swprintf_s(stepText, 32, L"%.0f %%", fpcam::TurnSpeed() * 100.f);
    // GoW formatting: "2048 x 2048", "Auto (N x N)".
    const int size = rendersize::Setting();
    const uint32_t autoN = rendersize::AutoSize();
    if (size == kAuto) {
        if (autoN) swprintf_s(sizeText, 32, L"Auto (%u x %u)", autoN, autoN);
        else swprintf_s(sizeText, 32, L"Auto");
    } else if (size == kGame) {
        swprintf_s(sizeText, 32, L"Game resolution");
    } else {
        swprintf_s(sizeText, 32, L"%d x %d", size, size);
    }
}

void Draw() {
    if (!MakeSurface()) return;
    wchar_t stepText[32], sizeText[32], pushText[32];
    Layout(stepText, sizeText, pushText);
    // V6 tokens (playbook revision 20) with the owner's accent: the light blue of the Beyond: Two Souls cover
    // (owner 2026-10-27; selected #5E97BF, selected hover #7DB0D3) where GoW has red.
    const COLORREF title = RGB(94, 151, 191), button = RGB(66, 66, 70), hover = RGB(96, 96, 102),
                   selected = RGB(94, 151, 191), selectedHover = RGB(125, 176, 211), white = RGB(245, 245, 245),
                   label = RGB(225, 225, 230), hint = RGB(165, 165, 170), valueBox = RGB(30, 30, 32);
    GdiFlush();
    std::memcpy(g_bits, g_bgPix, sizeof(g_bgPix));  // the patterned grey
    Fill({0, 0, kW, 100}, title);
    Text(L"VR SETTINGS", {40, 0, kW - 40, 100}, g_fTitle, white, DT_LEFT | DT_VCENTER);
    Row rows[kRows];
    const int n = Rows(rows);
    for (int i = 0; i < n; ++i) {
        const int y = kTop + i * kRow;
        Text(rows[i].label, {40, y, kW - 40, y + kLabelH}, g_fLabel, label, DT_LEFT | DT_VCENTER);
        if (!rows[i].ta) {  // the stepper's value box
            const int by = y + kButtonY;
            Fill({200, by, 800, by + kButtonH}, valueBox, 14);
            Text(rows[i].value == 1 ? sizeText : rows[i].value == 2 ? pushText : stepText, {200, by, 800, by + kButtonH},
                 g_fButton, white,
                 DT_CENTER | DT_VCENTER);
        }
    }
    for (int i = 0; i < g_nButtons; ++i) {
        const Button& b = g_buttons[i];
        const bool hv = b.enabled && b.a == g_hover;
        Fill(b.r, b.selected ? (hv ? selectedHover : selected) : (hv ? hover : button), 14);
        Text(b.text, b.r, g_fButton, b.enabled ? white : hint, DT_CENTER | DT_VCENTER);
    }
    // Footer (TWELVE_ROW_V1: 1668..1708, 1712..1752, 1756..1796).
    Text(L"Point a controller at an option and pull its trigger.", {40, kH - 150, kW - 40, kH - 110}, g_fHint, hint,
         DT_LEFT | DT_VCENTER);
    Text(L"VR off: press F9 on the keyboard to turn it back on.", {40, kH - 106, kW - 40, kH - 66}, g_fHint, hint,
         DT_LEFT | DT_VCENTER);
    Text(L"First person on / off: right hand at your head + B / \x25CB. Menu: A B X Y.", {40, kH - 62, kW - 40, kH - 22},
         g_fHint, hint, DT_LEFT | DT_VCENTER);
    GdiFlush();
    for (int i = 0; i < kW * kH; ++i) g_out[i] = g_bits[i] | 0xff000000u;  // GDI leaves alpha 0: opaque
}

void MakeCursor() {
    if (g_cursorMade) return;
    g_cursorMade = true;
    const float c = (kCursorPx - 1) * 0.5f;
    // the ring in the cover's light blue (#8DBEDE)
    const float rr = 141.f / 255.f, rg = 190.f / 255.f, rb = 222.f / 255.f;
    for (int y = 0; y < kCursorPx; ++y)
        for (int x = 0; x < kCursorPx; ++x) {
            const float d = std::sqrt((x - c) * (x - c) + (y - c) * (y - c)) / c;  // 0 centre .. 1 edge
            float a = 0.f, r = 0.f, g = 0.f, b = 0.f;
            if (d < 0.55f) {
                a = 1.f;
                r = g = b = 1.f;  // white dot
            } else if (d < 0.85f) {
                a = 1.f;
                r = rr;
                g = rg;
                b = rb;
            } else if (d < 1.f) {
                a = (1.f - d) / 0.15f;  // soft edge (premultiplied)
                r = rr * a;
                g = rg * a;
                b = rb * a;
            }
            auto u8 = [](float v) { return uint32_t(std::lrint(std::fmin(std::fmax(v, 0.f), 1.f) * 255.f)); };
            g_cursor[y * kCursorPx + x] = u8(a) << 24 | u8(r) << 16 | u8(g) << 8 | u8(b);
        }
}

// The pointer ray through a One Euro filter (GoW D-263: at ~2 m a trembling hand is a jumping dot): still at
// rest, a fast sweep passes (min cutoff 3 Hz, beta 6). Raw controller samples still drive the game.
void SmoothAim(int hand, pose::Vec3& p, pose::Vec3& d) {
    static bool have[2] = {false, false};
    static pose::Vec3 sp[2], sd[2];
    static LARGE_INTEGER last[2] = {}, qf{};
    if (!qf.QuadPart) QueryPerformanceFrequency(&qf);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const float dt = last[hand].QuadPart ? float(now.QuadPart - last[hand].QuadPart) / float(qf.QuadPart) : 0.f;
    last[hand] = now;
    if (!have[hand] || dt <= 0.f || dt > 0.1f) {
        sp[hand] = p;
        sd[hand] = d;
        have[hand] = true;
        return;
    }
    const float dp = std::sqrt((p.x - sp[hand].x) * (p.x - sp[hand].x) + (p.y - sp[hand].y) * (p.y - sp[hand].y) +
                               (p.z - sp[hand].z) * (p.z - sp[hand].z));
    const float dd = std::sqrt((d.x - sd[hand].x) * (d.x - sd[hand].x) + (d.y - sd[hand].y) * (d.y - sd[hand].y) +
                               (d.z - sd[hand].z) * (d.z - sd[hand].z));
    const float cutoff = 3.f + 6.f * (dp + 2.f * dd) / dt;
    const float a = 1.f / (1.f + 1.f / (6.2831853f * cutoff * dt));
    sp[hand] = {sp[hand].x + (p.x - sp[hand].x) * a, sp[hand].y + (p.y - sp[hand].y) * a, sp[hand].z + (p.z - sp[hand].z) * a};
    sd[hand] = {sd[hand].x + (d.x - sd[hand].x) * a, sd[hand].y + (d.y - sd[hand].y) * a, sd[hand].z + (d.z - sd[hand].z) * a};
    const float n = std::sqrt(sd[hand].x * sd[hand].x + sd[hand].y * sd[hand].y + sd[hand].z * sd[hand].z);
    if (n > 1e-6f) sd[hand] = {sd[hand].x / n, sd[hand].y / n, sd[hand].z / n};
    p = sp[hand];
    d = sd[hand];
}

}  // namespace

void Update(const xr::FrameInput& in, const pose::Vec3& screen, float screenW, Frame* out) {
    const pose::Vec3 head{in.headRel.m[12], in.headRel.m[13], in.headRel.m[14]};
    const Placement pl = Place(screen, screenW, head);
    Action hover = kNone;
    bool cursor = false;
    pose::Vec3 hitP{};
    for (int hand = 1; hand >= 0; --hand) {  // the right hand first
        const xr::HandInput& hs = in.hand[hand];
        bool hit = false;
        if (hs.active && hs.aimValid) {
            pose::Vec3 o{hs.aimRel.m[12], hs.aimRel.m[13], hs.aimRel.m[14]};
            pose::Vec3 d{-hs.aimRel.m[8], -hs.aimRel.m[9], -hs.aimRel.m[10]};  // aim -Z
            SmoothAim(hand, o, d);
            float px = 0.f, py = 0.f;
            pose::Vec3 q{};
            if (Hit(pl, o, d, &px, &py, &q)) {
                hit = true;
                if (!cursor) {
                    cursor = true;
                    hitP = q;
                    for (int i = 0; i < g_nButtons; ++i) {
                        const RECT& r = g_buttons[i].r;
                        if (g_buttons[i].enabled && px >= r.left && px < r.right && py >= r.top && py < r.bottom)
                            hover = g_buttons[i].a;
                    }
                    if (g_click[hand].Feed(hs.trigger) && hover != kNone) Apply(hover);
                }
            }
        }
        g_capture[hand] = hit;
        // Off the panel the trigger only re-arms (below 0.35) or disarms (a pull): a pull begun elsewhere
        // never clicks when the ray reaches the panel.
        if (!hit) g_click[hand].Feed(hs.active ? hs.trigger : 0.f);
    }
    // Redraw when a value or the hovered button changed (values also change with F1 / F3 / F10 / F11, the head
    // switch and b2svr.ini).
    char state[160];
    sprintf_s(state, "%d %d %d %d %d %.2f %.0f %d %d %u %d %d %d %.2f", int(fpcam::GameplayFP()), int(fpcam::CutsceneFP()),
              int(fpcam::MotionControls()), int(fpcam::HandSmoothing()), int(fpcam::SnapTurn()), fpcam::TurnSpeed(), fpcam::SnapDegrees(),
              vrinput::PadType(), rendersize::Setting(), rendersize::AutoSize(), stereo::GetMethod(),
              int(stereo::AfrSupported()), int(rendersize::Available()), stereo::CameraPush());
    if (hover != g_hover || std::strcmp(state, g_lastState) != 0 || g_needDraw) {
        g_hover = hover;
        strcpy_s(g_lastState, state);
        g_needDraw = false;
        Draw();
        out->dirty = true;
        static int shown = 0;
        if (!shown++) Log("PANEL VR settings panel shown (%dx%d px, %.2f x %.4f m, right of the screen)", kW, kH, kWidthM, kHeightM);
    } else {
        out->dirty = false;
    }
    out->placement = pl;
    out->pixels = g_out;
    out->cursor = cursor;
    if (cursor) {
        const float nx = std::sin(pl.yaw), nz = std::cos(pl.yaw);
        out->cursorAt = {hitP.x + nx * 0.01f, hitP.y, hitP.z + nz * 0.01f};  // just in front of the panel
    }
}

void Hidden() {
    g_capture[0] = g_capture[1] = false;
    g_hover = kNone;
    g_needDraw = true;
}

const uint32_t* CursorPixels() {
    MakeCursor();
    return g_cursor;
}

bool CapturesTrigger(int hand) { return g_capture[hand & 1].load(std::memory_order_relaxed); }

}  // namespace b2s::panel
