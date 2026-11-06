// Offline render of the VR settings panel (src/panel.cpp) with the game's side stubbed: writes
// panel_preview.bmp (and panel_preview_hover.bmp with a controller pointing at "Snap") next to the exe, for a
// visual check of the layout and palette without the game or a headset. Not a CTest test.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "panel.h"
#include "stereo.h"

namespace b2s {
void Log(const char* fmt, ...) { (void)fmt; }
namespace fpcam {
bool g_f1 = true, g_f3 = false, g_arms = true, g_smooth = true, g_snap = false;
float g_deg = 45.f, g_speed = 1.f;
bool GameplayFP() { return g_f1; }
bool CutsceneFP() { return g_f3; }
void SetGameplayFP(bool on) { g_f1 = on; }
void SetCutsceneFP(bool on) { g_f3 = on; }
bool MotionControls() { return g_arms; }
void SetMotionControls(bool on) { g_arms = on; }
bool HandSmoothing() { return g_smooth; }
void SetHandSmoothing(bool on) { g_smooth = on; }
bool g_poss = false;  // the default (0.19.1)
bool PossessionFP() { return g_poss; }
void SetPossessionFP(bool on) { g_poss = on; }
bool SnapTurn() { return g_snap; }
void SetSnapTurn(bool on) { g_snap = on; }
float SnapDegrees() { return g_deg; }
void SetSnapDegrees(float d) { g_deg = d; }
float TurnSpeed() { return g_speed; }
void SetTurnSpeed(float f) { g_speed = f; }
}  // namespace fpcam
namespace vrinput {
int g_pad = 2;
int PadType() { return g_pad; }
void SetPadType(int t) { g_pad = t; }
}  // namespace vrinput
namespace rendersize {
int g_set = 2048;
int Setting() { return g_set; }
void SetSetting(int s) { g_set = s; }
uint32_t AutoSize() { return 2160; }
bool Available() { return true; }
}  // namespace rendersize
namespace stereo {
void RequestVr(bool) {}
bool AfrSupported() { return true; }
int g_method = kAfr;  // the default (0.18.1)
int GetMethod() { return g_method; }
void SetMethod(int m) { g_method = m; }
const char* MethodName(int) { return "preview"; }
}  // namespace stereo
}  // namespace b2s

using namespace b2s;

static void SaveBmp(const char* name, const uint32_t* px, int w, int h) {
    FILE* f = nullptr;
    if (fopen_s(&f, name, "wb") || !f) return;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = -h;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + DWORD(w) * DWORD(h) * 4;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(px, 4, size_t(w) * size_t(h), f);
    fclose(f);
    std::printf("wrote %s\n", name);
}

int main() {
    xr::FrameInput in{};
    in.headRel.m[13] = 0.f;
    panel::Frame f;
    panel::Update(in, {0.f, 0.f, -1.8f}, 2.2f, &f);
    SaveBmp("panel_preview.bmp", f.pixels, panel::kWidthPx, panel::kHeightPx);
    // Point the right controller at row 4's right button ("Snap"): x 520..960, label y 122 + 4 * 128.
    const panel::Placement pl = f.placement;
    const float u = 740.f / panel::kWidthPx, v = (122.f + 4 * 128.f + 48.f + 34.f) / panel::kHeightPx;
    const float lx = (u - 0.5f) * panel::kWidthM, ly = (0.5f - v) * panel::kHeightM;
    const pose::Vec3 target{pl.center.x + lx * std::cos(pl.yaw), pl.center.y + ly, pl.center.z - lx * std::sin(pl.yaw)};
    pose::Vec3 o{0.2f, -0.3f, -0.2f};
    pose::Vec3 d{target.x - o.x, target.y - o.y, target.z - o.z};
    const float n = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    // aim -Z = d: row 2 of the matrix is +Z = -d (only the direction is read)
    in.hand[1].active = in.hand[1].aimValid = true;
    in.hand[1].aimRel = pose::Identity();
    in.hand[1].aimRel.m[8] = -d.x / n;
    in.hand[1].aimRel.m[9] = -d.y / n;
    in.hand[1].aimRel.m[10] = -d.z / n;
    in.hand[1].aimRel.m[12] = o.x;
    in.hand[1].aimRel.m[13] = o.y;
    in.hand[1].aimRel.m[14] = o.z;
    panel::Update(in, {0.f, 0.f, -1.8f}, 2.2f, &f);  // first pass lays out the buttons
    panel::Update(in, {0.f, 0.f, -1.8f}, 2.2f, &f);
    SaveBmp("panel_preview_hover.bmp", f.pixels, panel::kWidthPx, panel::kHeightPx);
    std::printf("cursor=%d\n", int(f.cursor));
    return 0;
}
