#pragma once
// Playbook revision 37, 19.8 point 11 L2 (c) / L5 (EXP-069): the held rule for objects the engine plays WITH Jodie but
// links to no bone of hers - in Beyond's F1 the objects in her motion graph's follower list whose skeleton does not
// copy hers (SyncMotionKit; L1 table: the two skis, DRAGSKI) - and the drawn matrix of a carried object.
//
// MovieCarry, BlendCarry, QuatOf, RowsOf, MulAffine and InvAffine are the owner's Heavy Rain port's rule (HRVR 0.33,
// rigmap.h), copied unchanged: the playbook names it the rule to use (proven offline on 1,748 Heavy Rain clip pairs,
// 0 open). Its comments speak of Heavy Rain's clips and data; Beyond's own numbers are in EXP-069. Pure: the mod
// (fpbody.cpp) and the desk-only DLL (tests/carry_desk.cpp, tools/gate1/l5_replay.py) run this exact code.
//
// Beyond's link for the candidate: the object's skeleton is in her motion graph's follower list (player::State::
// syncedProps, EXP-064). Distances are measured to the object's nearest model bone (FeedPoints), never only its root.
// Row vectors, 16 floats, rows 0..2 = axes, row 3 = origin, metres.
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace b2s::carry {

inline void MulAffine(const float* a, const float* b, float* out) {  // row vectors: out = a x b
    float r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    for (int k = 0; k < 16; ++k) out[k] = r[k];
}

inline bool InvAffine(const float* m, float* out) {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!(std::fabs(det) > 1e-12f)) return false;
    const float s = 1.f / det;
    float r[16] = {(e * i - f * h) * s, (c * h - b * i) * s, (b * f - c * e) * s, 0.f,
                   (f * g - d * i) * s, (a * i - c * g) * s, (c * d - a * f) * s, 0.f,
                   (d * h - e * g) * s, (b * g - a * h) * s, (a * e - b * d) * s, 0.f,
                   0.f,                 0.f,                 0.f,                 1.f};
    r[12] = -(m[12] * r[0] + m[13] * r[4] + m[14] * r[8]);
    r[13] = -(m[12] * r[1] + m[13] * r[5] + m[14] * r[9]);
    r[14] = -(m[12] * r[2] + m[13] * r[6] + m[14] * r[10]);
    for (int k = 0; k < 16; ++k) out[k] = r[k];
    return true;
}

// ---- the held rule (Heavy Rain rigmap::MovieCarry, unchanged) ----
struct MovieCarry {
    // DATA (SOCKETS.md): Heavy Rain's hand sockets put an item's origin up to 28.7 cm from the wrist (Beyond's 25 cm,
    // playbook 19.8 point 8, is too short: the 0201 bag hangs 25 .. 28 cm from the hand).
    // (35 / 40 cm was tried for long items held at one end, e.g. the 0201 ruler at 29 .. 30 cm: 32 open Gate 2 pairs
    // instead of 26 and more drops - kept at 30 / 35.)
    static constexpr float kNear = 0.30f;
    static constexpr float kKeep = 0.35f;  // once held: the edge of kNear must not flicker
    static constexpr float kSlip = 0.03f;
    static constexpr float kSteady = 0.015f;
    static constexpr float kMoving = 0.002f;
    static constexpr float kAnchor = 0.1f;
    static constexpr float kRatio = 2.f;
    static constexpr float kPick = 0.02f;
    static constexpr float kWithHand = 0.02f;  // a sample's step against the hand that still counts as "with it"
    // m per sample: below this the item does not really move (1 mm: the 5001 hangman sheet pinned to the table by the
    // left hand jitters 0.1 .. 0.7 mm per frame and was passed to that hand; the sub-mm handcuffs are worn, not held)
    static constexpr float kStill = 0.001f;
    static constexpr float kRest = 0.001f;   // smoothed world speed below this (m per sample): at rest
    static constexpr float kRestAlpha = 0.5f;
    static constexpr float kCut = 0.5f;  // m in one sample: the clip jumps (a scene cut), never carried across it
    // On the HEAD, not in the hand (owner 2026-11-20 "distinguish when he has it in hands and when on his body"; Jayden's
    // ARI glasses): within kHead of the head bone and kHeadRatio times steadier against the head than against the
    // hand (smoothed per-sample steps, the hand's at least kMoving). DATA (Gate 2, 0020 ARIGLASSES_PUTON): on the face
    // the glasses move 0.01 cm per frame against the head, 13 cm from it, while the hand leaves at 5 .. 9 cm per frame.
    // A phone held to the ear is steady against both, so it stays in the hand.
    static constexpr float kHead = 0.20f;
    static constexpr float kHeadRatio = 3.f;
    static constexpr int kLoose = 2;
    static constexpr int kRamp = 6;
    bool linkOnly = false;
    bool held = false;
    int hand = -1;
    int loose = 0;
    float weight[2] = {0.f, 0.f};
    float anchor[3] = {0.f, 0.f, 0.f};
    bool anchored = false;
    // Per hand: the item's place in that hand and in the world when it came within kNear (or last slipped).
    float last[3] = {0.f, 0.f, 0.f};
    float rel[2][3] = {};
    float worldEma = 0.f;  // smoothed world speed: a slowly swinging bag (1.6 .. 7 mm per frame) is not at rest
    float relHead[3] = {0.f, 0.f, 0.f};
    float emaHead = 0.f, emaHand[2] = {0.f, 0.f};
    bool haveHead = false;
    bool cand[2] = {false, false};
    float candRel[2][3] = {}, candTravel[2] = {0.f, 0.f};
    bool have = false;
    // Playbook r37 19.8 point 11 L2: distances to the object's NEAREST MODEL BONE, never only its root (a skinned prop's
    // root can be far from the part in the fingers - DATA docs/items/L2.md: 45 held pairs, the 3003 shop bag's strap
    // 40 cm from its root). Per hand: the bone nearest that wrist, kept while that hand holds it or is building a pick-up
    // (its place in the hand must stay one point); point 0 = the root. With one point this is exactly the 0.33.0 rule.
    int bone[2] = {0, 0};
    bool boneChosen[2] = {false, false};
    float lastPt[2][3] = {};
    void Seed(int h) {
        held = h >= 0;
        hand = h;
        loose = 0;
        anchored = false;  // taken from the first sample
        boneChosen[0] = boneChosen[1] = false;  // the seeded hand's bone too: the nearest in the first sample
        weight[0] = weight[1] = 0.f;
        if (h >= 0) weight[h] = 1.f;
    }
    // o: the object's root (world, animated); w[s]: the clean wrist worlds (16 floats, row vectors) or nullptr.
    // Writes the two hands' weights to out[2]. Returns the hand with the larger weight (-1 = none).
    // head: the clean Head bone world (16 floats) or nullptr (then nothing is taken for worn on the head).
    int Feed(bool link, const float* o, const float* const w[2], float* out, const float* head = nullptr) {
        return FeedPoints(link, o, 1, w, out, head);
    }
    // pts: n model-bone positions (world, 3 floats each), pts[0..2] = the root.
    int FeedPoints(bool link, const float* pts, int n, const float* const w[2], float* out, const float* head = nullptr) {
        const float* o = pts;  // the root: world speed, cuts, the head test
        if (n < 1) n = 1;
        float r[2][3] = {}, d[2] = {1e9f, 1e9f};
        bool ok[2] = {false, false}, fresh[2] = {false, false};
        const float* po[2] = {pts, pts};
        for (int s = 0; s < 2; ++s) {
            if (w[s]) {
                const bool locked = boneChosen[s] && bone[s] < n && ((held && hand == s) || cand[s]);
                int b = locked ? bone[s] : 0;
                if (!locked) {
                    float best = 1e30f;
                    for (int i = 0; i < n; ++i) {
                        const float* q = pts + 3 * i;
                        const float dd = (q[0] - w[s][12]) * (q[0] - w[s][12]) + (q[1] - w[s][13]) * (q[1] - w[s][13]) +
                                         (q[2] - w[s][14]) * (q[2] - w[s][14]);
                        if (dd < best) best = dd, b = i;
                    }
                }
                fresh[s] = b != bone[s];  // another point than lastPt[s] / rel[s] describe: no history for this hand
                bone[s] = b;
                boneChosen[s] = true;
            }
            po[s] = pts + 3 * (bone[s] < n ? bone[s] : 0);
            const float* q = po[s];
            float inv[16];
            if (!w[s] || !InvAffine(w[s], inv)) continue;
            for (int k = 0; k < 3; ++k) r[s][k] = q[0] * inv[k] + q[1] * inv[4 + k] + q[2] * inv[8 + k] + inv[12 + k];
            const float* p = w[s] + 12;
            d[s] = std::sqrt((q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]) + (q[2] - p[2]) * (q[2] - p[2]));
            ok[s] = true;
        }
        // Per hand: the world step of that hand's point (the root's step with one point).
        bool hv[2] = {have && !fresh[0], have && !fresh[1]};
        float ws[2] = {0.f, 0.f};
        for (int s = 0; s < 2; ++s)
            if (hv[s]) {
                const float* q = po[s];
                ws[s] = std::sqrt((q[0] - lastPt[s][0]) * (q[0] - lastPt[s][0]) + (q[1] - lastPt[s][1]) * (q[1] - lastPt[s][1]) +
                                  (q[2] - lastPt[s][2]) * (q[2] - lastPt[s][2]));
            }
        for (int s = 0; s < 2; ++s)
            if (fresh[s]) cand[s] = false;
        int nearest = -1;
        for (int s = 0; s < 2; ++s)
            if (ok[s] && d[s] <= kNear && (nearest < 0 || d[s] < d[nearest])) nearest = s;
        // Moves with hand s: it kept its place against s (within kSlip) while it moved kPick in the world since it
        // came within reach - accumulated, so a handcuff that moves 0.4 mm per frame with a resting hand counts
        // (Gate 2, 3202 HANDCUFF_LIFT) and a box at rest beside a resting hand never does.
        // Or, in one sample, it kept its place against s (< kSteady) while it moved kMoving (a quick pick-up: the
        // 0403 pizza box leaves the fridge at 2 .. 3 mm per frame while the hand still settles on it).
        float worldStep = 0.f;
        if (have)
            worldStep = std::sqrt((o[0] - last[0]) * (o[0] - last[0]) + (o[1] - last[1]) * (o[1] - last[1]) +
                                  (o[2] - last[2]) * (o[2] - last[2]));
        if (have && worldStep <= kCut) worldEma += kRestAlpha * (worldStep - worldEma);
        bool movesWith[2] = {false, false}, nowWith[2] = {false, false};
        float relStep[2] = {1e9f, 1e9f};
        for (int s = 0; s < 2; ++s) {
            if (hv[s] && ok[s]) {
                const float ax = r[s][0] - rel[s][0], ay = r[s][1] - rel[s][1], az = r[s][2] - rel[s][2];
                relStep[s] = std::sqrt(ax * ax + ay * ay + az * az);
            }
            if (hv[s] && ok[s] && d[s] <= kKeep && ws[s] >= kMoving) {
                const float ax = r[s][0] - rel[s][0], ay = r[s][1] - rel[s][1], az = r[s][2] - rel[s][2];
                // and it moved much less against the hand than in the world: a still hand beside an item another hand
                // nudges sees it move as much as the world does (Gate 2, 5001 origami nudged 0.6 cm next to the
                // resting left hand was picked and kept for 250 frames).
                const float rs = std::sqrt(ax * ax + ay * ay + az * az);
                nowWith[s] = rs <= kSteady && rs * kRatio <= ws[s];
                movesWith[s] = nowWith[s] && d[s] <= kNear;
            }
            if (!ok[s] || d[s] > (cand[s] ? kKeep : kNear)) {  // starts within kNear, may go on to kKeep
                cand[s] = false;
                continue;
            }
            const float sx = r[s][0] - candRel[s][0], sy = r[s][1] - candRel[s][1], sz = r[s][2] - candRel[s][2];
            if (!cand[s] || sx * sx + sy * sy + sz * sz > kSlip * kSlip) {
                cand[s] = true;
                candTravel[s] = 0.f;
                for (int k = 0; k < 3; ++k) candRel[s][k] = r[s][k];
                continue;
            }
            for (int k = 0; k < 3; ++k) candRel[s][k] += kAnchor * (r[s][k] - candRel[s][k]);  // slow re-grips
            // path length WITH the hand: only samples where it kept its place against it (a resting item's clip does
            // not move at all; a notepad flung 11 cm in one frame while 5 cm against the hand does not count - Gate 2,
            // 1101 NOTEPAD_THROW was picked up again in flight)
            if (relStep[s] <= kWithHand) candTravel[s] += ws[s];
            // (A ratio on the whole stretch, with 1 cm, was tried: the hand settling on the item counts as drift and
            // the pizza box came 7 frames later; 164 open pairs instead of 82. Gate 2, 2026-11-20.)
            // ... and only in a sample where it moves at all (> kStill): a memory card at rest beside a still hand that
            // had carried it earlier was passed to it (Gate 2, 2901 PHONEMEMCARD_TAKE)
            movesWith[s] = movesWith[s] || (candTravel[s] >= kPick && ws[s] > kStill);
        }
        // Worn on the head?
        bool onHead[2] = {false, false};
        {
            float hinv[16], rh[3] = {0.f, 0.f, 0.f}, dh = 1e9f;
            const bool hok = head && InvAffine(head, hinv);
            if (hok) {
                for (int k = 0; k < 3; ++k) rh[k] = o[0] * hinv[k] + o[1] * hinv[4 + k] + o[2] * hinv[8 + k] + hinv[12 + k];
                dh = std::sqrt((o[0] - head[12]) * (o[0] - head[12]) + (o[1] - head[13]) * (o[1] - head[13]) +
                               (o[2] - head[14]) * (o[2] - head[14]));
                if (haveHead && worldStep <= kCut) {
                    const float hx = rh[0] - relHead[0], hy = rh[1] - relHead[1], hz = rh[2] - relHead[2];
                    emaHead += kRestAlpha * (std::sqrt(hx * hx + hy * hy + hz * hz) - emaHead);
                }
                for (int k = 0; k < 3; ++k) relHead[k] = rh[k];
            }
            haveHead = hok;
            for (int s = 0; s < 2; ++s) {
                if (have && ok[s] && relStep[s] < 1e8f && worldStep <= kCut) emaHand[s] += kRestAlpha * (relStep[s] - emaHand[s]);
                onHead[s] = hok && dh <= kHead && emaHand[s] >= kMoving && emaHead * kHeadRatio <= emaHand[s];
                // Near the head it is picked up only when it clearly moves with the hand rather than the head (glasses
                // taken off leave the face); glasses on the face of a lying, almost still Jayden (4801) beside his
                // resting hand were taken by the 2 cm accumulated test.
                if (hok && dh <= kHead && !(emaHand[s] * kHeadRatio <= emaHead)) movesWith[s] = false;
            }
        }
        for (int k = 0; k < 3; ++k) last[k] = o[k];
        for (int s = 0; s < 2; ++s)
            for (int k = 0; k < 3; ++k) rel[s][k] = r[s][k], lastPt[s][k] = po[s][k];
        // A cut: the hold ends in this sample and both weights go to 0 at once (Gate 2: 1903 memory card, 10.9 m in
        // one frame while held - the ramp drew it metres off the hand for 8 frames). The engine cuts the view too.
        const bool cut = worldStep > kCut;
        if (cut) {
            weight[0] = weight[1] = 0.f;
            for (int s = 0; s < 2; ++s) cand[s] = false;
        }
        if (!link || cut || (linkOnly && nearest < 0)) {
            held = false;
            hand = -1;
            loose = 0;
            anchored = false;
        } else if (linkOnly) {
            held = true;
            hand = nearest;
        } else if (!held) {
            const int pick = movesWith[0] && movesWith[1] ? (d[0] <= d[1] ? 0 : 1) : movesWith[0] ? 0 : movesWith[1] ? 1 : -1;
            if (pick >= 0) {
                held = true;
                hand = pick;
                loose = 0;
                for (int k = 0; k < 3; ++k) anchor[k] = r[pick][k];
                anchored = true;
            }
        } else {
            if (!anchored && ok[hand]) {
                for (int k = 0; k < 3; ++k) anchor[k] = r[hand][k];
                anchored = true;
            }
            // The other hand takes it when it moves with that one and no longer with this one (a pass).
            const int other = 1 - hand;
            float slip = 1e9f;
            if (ok[hand]) {
                const float ax = r[hand][0] - anchor[0], ay = r[hand][1] - anchor[1], az = r[hand][2] - anchor[2];
                slip = std::sqrt(ax * ax + ay * ay + az * az);
            }
            // Let go only when it comes to REST in the world away from its place in the hand (a put-down) or leaves
            // kKeep. While it still moves it slides in the hand (a re-grip, or the item's keys lag a fast swing by
            // 3 .. 4 cm per frame - Gate 2, 3601 gun) and is re-anchored, never let go and picked again (that made
            // the weight flicker between 5/6 and 1).
            // "At rest" is the smoothed world speed (2801 bag swings 1.6 .. 7 mm per frame; 1903 key in the lock: 0).
            const bool moving = worldEma >= kRest;
            // (Tried and dropped, for the thrown 1101 notepad: ending a slide faster than 5 cm per frame against the
            // hand - also only while it gets farther, also only above 8 cm per frame in the world; the fights' swung
            // weapons then dropped out: 18 .. 38 open pairs instead of 16.)
            const bool slides = slip > kSlip && (nowWith[hand] || moving);
            // Put down: it rests in the world this sample while the hand moves against it (> kSteady). Two such
            // samples (kLoose) let go even before it is kSlip from its place (Gate 2: the 0201 plate, 1101 notepad,
            // 2901 memory card were put down with the hand resting beside them and held for up to 50 more frames).
            const bool putDown = hv[hand] && ws[hand] < kMoving && relStep[hand] > kSteady && relStep[hand] < 1e8f;
            const bool keeps = ok[hand] && d[hand] <= kKeep && (slip <= kSlip || slides) && !onHead[hand] && !putDown;
            // A held item that keeps its place against the OTHER hand while it still moves is passed, never let go
            // and picked again (2801 golf club in both hands shifts to the right hand at 1.6 mm per frame).
            const bool passes = !keeps && !onHead[other] && (movesWith[other] ||
                                           (moving && ws[other] >= kRest && ok[other] && d[other] <= kNear &&
                                            relStep[other] <= kSteady));  // not an item at rest this sample (2901
                                                                          // memory card put down beside the still
                                                                          // left hand was passed to it)
            if (keeps) {
                loose = 0;
                for (int k = 0; k < 3; ++k) anchor[k] = slides ? r[hand][k] : anchor[k] + kAnchor * (r[hand][k] - anchor[k]);
            } else if (passes) {
                hand = other, loose = 0;
                for (int k = 0; k < 3; ++k) anchor[k] = r[other][k];
            } else if (++loose >= kLoose) {
                held = false, hand = -1, loose = 0, anchored = false;
            }
        }
        have = true;
        const float step = 1.f / float(kRamp);
        for (int s = 0; s < 2; ++s) {
            const float target = (held && hand == s && ok[s]) ? 1.f : 0.f;
            weight[s] += std::fmin(step, std::fmax(-step, target - weight[s]));
            if (weight[s] < 1e-4f) weight[s] = 0.f;
        }
        out[0] = weight[0];
        out[1] = weight[1];
        return weight[0] == 0.f && weight[1] == 0.f ? -1 : weight[1] > weight[0] ? 1 : 0;
    }
};

inline void QuatOf(const float* m, float* q) {  // x, y, z, w of the rotation rows (orthonormal assumed)
    const float t = m[0] + m[5] + m[10];
    if (t > 0.f) {
        const float s = std::sqrt(t + 1.f) * 2.f;
        q[3] = 0.25f * s, q[0] = (m[6] - m[9]) / s, q[1] = (m[8] - m[2]) / s, q[2] = (m[1] - m[4]) / s;
    } else if (m[0] > m[5] && m[0] > m[10]) {
        const float s = std::sqrt(1.f + m[0] - m[5] - m[10]) * 2.f;
        q[3] = (m[6] - m[9]) / s, q[0] = 0.25f * s, q[1] = (m[1] + m[4]) / s, q[2] = (m[8] + m[2]) / s;
    } else if (m[5] > m[10]) {
        const float s = std::sqrt(1.f + m[5] - m[0] - m[10]) * 2.f;
        q[3] = (m[8] - m[2]) / s, q[0] = (m[1] + m[4]) / s, q[1] = 0.25f * s, q[2] = (m[6] + m[9]) / s;
    } else {
        const float s = std::sqrt(1.f + m[10] - m[0] - m[5]) * 2.f;
        q[3] = (m[1] - m[4]) / s, q[0] = (m[8] + m[2]) / s, q[1] = (m[6] + m[9]) / s, q[2] = 0.25f * s;
    }
}

inline void RowsOf(const float* q, float* m) {  // the inverse of QuatOf (same convention as rig_anim.quat_rows)
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z), m[1] = 2 * (x * y + w * z), m[2] = 2 * (x * z - w * y);
    m[4] = 2 * (x * y - w * z), m[5] = 1 - 2 * (x * x + z * z), m[6] = 2 * (y * z + w * x);
    m[8] = 2 * (x * z + w * y), m[9] = 2 * (y * z - w * x), m[10] = 1 - 2 * (x * x + y * y);
    m[3] = m[7] = m[11] = 0.f;
}

// anim: the object's animated world; clean[s] / drawn[s]: that hand's animated and drawn wrist (nullptr = none).
inline void BlendCarry(const float* anim, const float* const clean[2], const float* const drawn[2], const float* wt,
                       float* out) {
    float qa[4], acc[4], pos[3] = {anim[12], anim[13], anim[14]};
    QuatOf(anim, qa);
    float rest = 1.f;
    for (int k = 0; k < 4; ++k) acc[k] = 0.f;
    for (int s = 0; s < 2; ++s) {
        float inv[16], c[16], full[16], q[4];
        if (!(wt[s] > 0.f) || !clean[s] || !drawn[s] || !InvAffine(clean[s], inv)) continue;
        MulAffine(inv, drawn[s], c);
        MulAffine(anim, c, full);
        for (int k = 0; k < 3; ++k) pos[k] += wt[s] * (full[12 + k] - anim[12 + k]);
        QuatOf(full, q);
        const float sign = q[0] * qa[0] + q[1] * qa[1] + q[2] * qa[2] + q[3] * qa[3] < 0.f ? -1.f : 1.f;
        for (int k = 0; k < 4; ++k) acc[k] += wt[s] * sign * q[k];
        rest -= wt[s];
    }
    if (rest < 0.f) rest = 0.f;
    for (int k = 0; k < 4; ++k) acc[k] += rest * qa[k];
    const float n = std::sqrt(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2] + acc[3] * acc[3]);
    if (n > 1e-8f)
        for (int k = 0; k < 4; ++k) acc[k] /= n;
    else
        for (int k = 0; k < 4; ++k) acc[k] = qa[k];
    RowsOf(acc, out);
    out[12] = pos[0], out[13] = pos[1], out[14] = pos[2], out[15] = 1.f;
}

// L2 (a): an object linked to one of her bones (or to a node on that bone) is drawn with that bone's own correction:
// drawn object = object x clean^-1 x drawn (the object keeps its place in the drawn bone's frame exactly). The node-link
// carry (fpbody ItemSource) and the copy path use this; drift 0 by construction (L5 checks it on every F1 pair).
inline bool Rebase(const float* object, const float* clean, const float* drawn, float* out) {
    float ic[16], c[16];
    if (!InvAffine(clean, ic)) return false;
    MulAffine(ic, drawn, c);
    MulAffine(object, c, out);
    return true;
}

// How far the drawn object is from its place in the drawn hand: its place in the animated hand (anim x clean^-1) against
// its drawn place in the drawn hand (item x drawn^-1), in cm and degrees. 0 at full weight by construction.
inline void Drift(const float* anim, const float* item, const float* clean, const float* drawn, float* cm, float* deg) {
    float ic[16], id[16], ra[16], rd[16];
    *cm = 0.f;
    *deg = 0.f;
    if (!InvAffine(clean, ic) || !InvAffine(drawn, id)) return;
    MulAffine(anim, ic, ra);
    MulAffine(item, id, rd);
    const float dx = ra[12] - rd[12], dy = ra[13] - rd[13], dz = ra[14] - rd[14];
    *cm = 100.f * std::sqrt(dx * dx + dy * dy + dz * dz);
    float rt[16] = {}, q[4];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            rt[i * 4 + j] = ra[i * 4] * rd[j * 4] + ra[i * 4 + 1] * rd[j * 4 + 1] + ra[i * 4 + 2] * rd[j * 4 + 2];
    rt[15] = 1.f;
    QuatOf(rt, q);
    const float v = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    *deg = 2.f * std::atan2(v, std::fabs(q[3])) * 57.29578f;
}

}  // namespace b2s::carry
