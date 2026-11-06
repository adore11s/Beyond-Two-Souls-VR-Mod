// Head-tracked sound listener (src/audio_math.h): which listener feed follows the game camera, the head's
// vectors in the engine convention, freshness.
#include "audio_math.h"

#include <cmath>
#include <cstdio>

using namespace b2s::audio;
using namespace b2s::pose;

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL: %s\n", #expr); ++fails; } else ++checks; } while (0)

int main() {
    int fails = 0, checks = 0;
    const Mat4 cam = FromPose(YawQuat(0.f), {10.f, 1.7f, -4.f});
    const float same[3] = {10.f, 1.7f, -4.f}, eased[3] = {10.3f, 1.7f, -4.2f}, other[3] = {14.f, 1.7f, -4.f};
    CHECK(FollowsCamera(same, cam) && FollowsCamera(eased, cam) && !FollowsCamera(other, cam));
    const float bad[3] = {NAN, 0.f, 0.f};
    CHECK(!FollowsCamera(bad, cam));

    // A head turned 90 degrees left (yaw +90): front = world -X, up = +Y, position copied, w = 1 / 0 / 0.
    const Mat4 head = FromPose(YawQuat(1.5707963f), {1.f, 1.6f, 2.f});
    float p[4], f[4], u[4];
    HeadVectors(head, p, f, u);
    CHECK(Near(p[0], 1.f) && Near(p[1], 1.6f) && Near(p[2], 2.f) && Near(p[3], 1.f));
    CHECK(Near(f[0], -1.f) && Near(f[1], 0.f) && Near(f[2], 0.f) && Near(f[3], 0.f));
    CHECK(Near(u[0], 0.f) && Near(u[1], 1.f) && Near(u[2], 0.f) && Near(u[3], 0.f));

    HeadPublish pub;
    pub.valid = true;
    pub.tickMs = 1000;
    CHECK(Fresh(pub, 1100) && Fresh(pub, 1250) && !Fresh(pub, 1251) && !Fresh(pub, 999));
    pub.valid = false;
    CHECK(!Fresh(pub, 1000));

    std::printf("AUDIO CHECKS: %d passed, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
