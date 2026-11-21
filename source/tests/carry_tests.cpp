// carry.h (playbook revision 37 L2): the linked carry keeps the object in the drawn bone's frame, the held rule takes an
// object that moves with a wrist (measured to its nearest bone, not its root) and leaves one that stays at the feet.
#include "../src/carry.h"

#include <cmath>
#include <cstdio>

using namespace b2s;

namespace {
int failures = 0;
void Check(bool ok, const char* why) {
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", why);
    }
}
void Ident(float* m, float x, float y, float z) {
    for (int k = 0; k < 16; ++k) m[k] = (k % 5 == 0) ? 1.f : 0.f;
    m[12] = x, m[13] = y, m[14] = z;
}
void TurnY(float* m, float deg) {  // rows: rotated axes
    const float a = deg * 3.14159265f / 180.f, c = std::cos(a), s = std::sin(a);
    m[0] = c, m[1] = 0, m[2] = -s, m[4] = 0, m[5] = 1, m[6] = 0, m[8] = s, m[9] = 0, m[10] = c;
}
}  // namespace

int main() {
    // Rebase: an object 10 cm in front of the hand keeps that place when the hand moves 40 cm and turns 60 degrees.
    {
        float hand[16], obj[16], drawn[16], out[16], cm, deg;
        Ident(hand, 0.3f, 1.0f, 0.f);
        Ident(obj, 0.3f, 1.0f, -0.1f);
        Ident(drawn, 0.5f, 1.3f, 0.2f);
        TurnY(drawn, 60.f);
        Check(carry::Rebase(obj, hand, drawn, out), "rebase");
        carry::Drift(obj, out, hand, drawn, &cm, &deg);
        Check(cm < 1e-3f && deg < 0.05f, "rebase drift 0");
    }
    // Held rule: a 3-bone object whose root is 45 cm from the wrist but whose second bone is in the hand, moving with
    // the right wrist (the Detroit dish stack): held by the right hand after a few samples.
    {
        carry::MovieCarry c;
        float wr[16], out[2] = {0, 0}, pts[9];
        int hand = -1;
        for (int f = 0; f < 40; ++f) {
            const float x = 0.01f * f;
            Ident(wr, 0.3f + x, 1.0f, 0.f);
            const float p[9] = {0.75f + x, 1.0f, 0.f, 0.32f + x, 1.0f, -0.02f, 0.5f + x, 1.0f, 0.f};
            for (int k = 0; k < 9; ++k) pts[k] = p[k];
            const float* w[2] = {nullptr, wr};
            hand = c.FeedPoints(true, pts, 3, w, out);
        }
        Check(hand == 1 && out[1] > 0.99f, "held by the nearest model bone");
        carry::MovieCarry root;  // the same object fed by its root only: never within reach
        for (int f = 0; f < 40; ++f) {
            const float x = 0.01f * f;
            Ident(wr, 0.3f + x, 1.0f, 0.f);
            const float p[3] = {0.75f + x, 1.0f, 0.f};
            const float* w[2] = {nullptr, wr};
            hand = root.FeedPoints(true, p, 1, w, out);
        }
        Check(hand == -1, "root alone: not held");
    }
    // A ski on her feet (1 m below the wrists) while she walks: never held.
    {
        carry::MovieCarry c;
        float wl[16], wr[16], out[2] = {0, 0};
        int hand = -1, ever = -1;
        for (int f = 0; f < 120; ++f) {
            const float z = -0.02f * f, sway = 0.05f * std::sin(0.3f * f);
            Ident(wl, -0.25f, 0.95f + sway, z);
            Ident(wr, 0.25f, 0.95f - sway, z);
            const float p[6] = {0.1f, 0.03f, z, 0.1f, 0.03f, z - 0.8f};
            const float* w[2] = {wl, wr};
            hand = c.FeedPoints(true, p, 2, w, out);
            if (hand >= 0) ever = hand;
        }
        Check(ever == -1, "ski at the feet: never held");
    }
    // BlendCarry at full weight equals Rebase.
    {
        float cl[16], dr[16], obj[16], a[16], b[16];
        Ident(cl, 0.f, 1.f, 0.f);
        Ident(dr, 0.2f, 1.1f, 0.f);
        TurnY(dr, 45.f);
        Ident(obj, 0.f, 1.f, -0.1f);
        const float* clean[2] = {nullptr, cl};
        const float* drawn[2] = {nullptr, dr};
        const float wt[2] = {0.f, 1.f};
        carry::BlendCarry(obj, clean, drawn, wt, a);
        carry::Rebase(obj, cl, dr, b);
        float d = 0.f;
        for (int k = 0; k < 16; ++k) d = std::fmax(d, std::fabs(a[k] - b[k]));
        Check(d < 1e-5f, "blend at weight 1 = rebase");
    }
    std::printf("carry_tests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
