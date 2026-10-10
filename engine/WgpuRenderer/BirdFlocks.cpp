// SPDX-License-Identifier: GPL-3.0-or-later
//
// Seabirds around the camera: stage 2 of porting Tidewater's wildlife (dgreenheck/tidewater, MIT,
// (c) DRG Software Solutions LLC; see CREDITS.md). From src/world/wildlife/:
//   BirdShapes.js  the species (laughing gull, royal tern, brown pelican, magnificent frigatebird)
//                  and the shared template: lofted body with neck and head, lofted bill, fan tail,
//                  two three-bone wings with an airfoil section (buildBird);
//   BirdPose.js    the wing articulation (setWings, flapWings: the downstroke a little longer than
//                  the upstroke, the wing half folded and swept back on the upstroke);
//   Flight.js      the flight model (coordinated turns banked from the turn rate, climbing and
//                  sinking toward a target height, flapping when power is needed, the species'
//                  flap / glide rhythm) and the per-species FLIGHT table;
//   BirdBatch.js   the plumage of each species (its fragment shader's colour rules);
//   Birds.js       the behaviours: gulls roaming and circling, terns patrolling beyond the surf,
//                  pelicans skimming the swell in a line, frigatebirds soaring high.
//
// What OP changes: the wing poses are baked into meshes (16 phases of the flap cycle and the glide
// pose per species) instead of skinned in a vertex shader; the plumage is painted into a small
// per-species atlas on the CPU from the same rules; the birds are placed around the camera wherever
// OFP's sea is near (no authored bay, pier or perches -- they fly, they do not land yet).

#include "BirdFlocks.hpp"
#include "EngineWgpu.hpp"
#include "WildlifeWorld.hpp"
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace Poseidon
{
namespace
{
constexpr float Tau = 6.2831853f, Pi = 3.14159265f, Grav = 9.81f;
constexpr int Phases = 16; // flap cycle; mesh index Phases is the glide pose
constexpr float DrawRange = 1000.0f, SimRange = 450.0f; // drawn as far as they are a pixel or so

using Pts = std::vector<std::pair<float, float>>;
struct Bill
{
    float z, y, len, pitch, w, up, low, hook, gonys, pouch;
};
struct Tail
{
    float z, y, w;
    float len[4];
    float spread;
};
struct Wing
{
    float root[3], span, elbow, wrist, thick[3];
    Pts le, chord;
};
struct Species
{
    const char* name;
    float length, span;
    std::vector<std::array<float, 5>> body; // z, y, half width, half height above, half height below
    float neck[2];
    Bill bill;
    Tail tail;
    Wing wing;
    float eye[3];
};
enum
{
    Gull,
    Tern,
    Pelican,
    Frigate,
    SpeciesCount
};
// BirdShapes.js SPECIES (the four that fly here)
const Species SpeciesTable[SpeciesCount] = {
    {"laughing gull", 0.42f, 1.03f,
     {{-0.125f, 0.012f, 0.017f, 0.01f, 0.01f}, {-0.095f, 0.007f, 0.033f, 0.024f, 0.027f}, {-0.045f, 0, 0.049f, 0.038f, 0.045f},
      {0.01f, 0, 0.054f, 0.042f, 0.049f}, {0.055f, 0.005f, 0.049f, 0.039f, 0.045f}, {0.088f, 0.014f, 0.036f, 0.033f, 0.033f},
      {0.108f, 0.022f, 0.029f, 0.03f, 0.027f}, {0.132f, 0.027f, 0.028f, 0.03f, 0.024f}, {0.154f, 0.025f, 0.021f, 0.025f, 0.018f},
      {0.17f, 0.02f, 0.012f, 0.015f, 0.012f}},
     {0.066f, 0.104f},
     {0.163f, 0.018f, 0.046f, -0.07f, 0.0056f, 0.0068f, 0.0062f, 0.0035f, 0.0022f, 0},
     {-0.118f, 0.012f, 0.017f, {0.112f, 0.113f, 0.115f, 0.116f}, 0.32f},
     {{0.03f, 0.022f, 0.03f}, 0.485f, 0.23f, 0.47f, {0.15f, 0.08f, 0.035f},
      {{0, 0.045f}, {0.23f, 0.052f}, {0.47f, 0.046f}, {0.7f, 0.008f}, {0.87f, -0.042f}, {1, -0.1f}},
      {{0, 0.165f}, {0.23f, 0.165f}, {0.47f, 0.15f}, {0.7f, 0.112f}, {0.87f, 0.072f}, {1, 0.014f}}},
     {0.142f, 0.034f, 0.0036f}},
    {"royal tern", 0.47f, 1.3f,
     {{-0.12f, 0.012f, 0.016f, 0.009f, 0.009f}, {-0.09f, 0.006f, 0.03f, 0.021f, 0.023f}, {-0.045f, 0, 0.043f, 0.033f, 0.037f},
      {0.01f, 0, 0.046f, 0.036f, 0.04f}, {0.055f, 0.004f, 0.04f, 0.033f, 0.036f}, {0.088f, 0.013f, 0.027f, 0.027f, 0.026f},
      {0.11f, 0.021f, 0.022f, 0.03f, 0.021f}, {0.133f, 0.025f, 0.022f, 0.028f, 0.019f}, {0.155f, 0.023f, 0.017f, 0.02f, 0.015f},
      {0.17f, 0.019f, 0.01f, 0.012f, 0.01f}},
     {0.066f, 0.105f},
     {0.162f, 0.017f, 0.064f, -0.1f, 0.0058f, 0.0062f, 0.0058f, 0.0012f, 0.0015f, 0},
     {-0.114f, 0.012f, 0.016f, {0.085f, 0.098f, 0.128f, 0.168f}, 0.24f},
     {{0.028f, 0.02f, 0.028f}, 0.62f, 0.2f, 0.43f, {0.14f, 0.07f, 0.03f},
      {{0, 0.04f}, {0.2f, 0.05f}, {0.43f, 0.045f}, {0.7f, 0.0f}, {0.88f, -0.06f}, {1, -0.13f}},
      {{0, 0.15f}, {0.2f, 0.15f}, {0.43f, 0.135f}, {0.7f, 0.1f}, {0.88f, 0.06f}, {1, 0.012f}}},
     {0.142f, 0.031f, 0.0034f}},
    {"brown pelican", 1.25f, 2.1f,
     {{-0.31f, 0.03f, 0.045f, 0.028f, 0.028f}, {-0.245f, 0.02f, 0.09f, 0.068f, 0.07f}, {-0.12f, 0, 0.13f, 0.1f, 0.11f},
      {0.02f, 0, 0.14f, 0.11f, 0.12f}, {0.12f, 0.012f, 0.125f, 0.105f, 0.11f}, {0.19f, 0.05f, 0.088f, 0.095f, 0.09f},
      {0.225f, 0.105f, 0.058f, 0.066f, 0.068f}, {0.255f, 0.135f, 0.046f, 0.05f, 0.042f}, {0.28f, 0.139f, 0.039f, 0.044f, 0.034f},
      {0.302f, 0.131f, 0.026f, 0.029f, 0.024f}},
     {0.13f, 0.215f},
     {0.288f, 0.126f, 0.33f, -0.3f, 0.027f, 0.013f, 0.012f, 0.013f, 0, 0.05f},
     {-0.3f, 0.03f, 0.045f, {0.145f, 0.143f, 0.138f, 0.128f}, 0.36f},
     {{0.1f, 0.07f, 0.08f}, 0.95f, 0.28f, 0.55f, {0.14f, 0.08f, 0.035f},
      {{0, 0.11f}, {0.28f, 0.11f}, {0.55f, 0.09f}, {0.8f, 0.035f}, {0.93f, -0.02f}, {1, -0.085f}},
      {{0, 0.43f}, {0.28f, 0.41f}, {0.55f, 0.38f}, {0.8f, 0.31f}, {0.93f, 0.23f}, {1, 0.11f}}},
     {0.268f, 0.146f, 0.007f}},
    {"magnificent frigatebird", 1.0f, 2.3f,
     {{-0.172f, 0.012f, 0.022f, 0.015f, 0.015f}, {-0.12f, 0.004f, 0.048f, 0.036f, 0.038f}, {-0.04f, 0, 0.068f, 0.053f, 0.058f},
      {0.045f, 0, 0.068f, 0.053f, 0.06f}, {0.11f, 0.01f, 0.047f, 0.043f, 0.045f}, {0.15f, 0.022f, 0.031f, 0.033f, 0.032f},
      {0.178f, 0.029f, 0.03f, 0.033f, 0.028f}, {0.203f, 0.031f, 0.029f, 0.032f, 0.026f}, {0.224f, 0.028f, 0.022f, 0.025f, 0.019f},
      {0.24f, 0.024f, 0.013f, 0.015f, 0.011f}},
     {0.105f, 0.16f},
     {0.233f, 0.024f, 0.115f, -0.04f, 0.0075f, 0.0075f, 0.006f, 0.013f, 0, 0},
     {-0.165f, 0.012f, 0.02f, {0.14f, 0.2f, 0.31f, 0.42f}, 0.1f},
     {{0.05f, 0.03f, 0.05f}, 1.1f, 0.19f, 0.41f, {0.13f, 0.07f, 0.03f},
      {{0, 0.07f}, {0.19f, 0.08f}, {0.41f, 0.09f}, {0.68f, 0.01f}, {0.88f, -0.09f}, {1, -0.19f}},
      {{0, 0.3f}, {0.19f, 0.29f}, {0.41f, 0.26f}, {0.68f, 0.17f}, {0.88f, 0.09f}, {1, 0.018f}}},
     {0.212f, 0.04f, 0.0055f}},
};

// Flight.js FLIGHT: cruise airspeed, min speed, max bank, roll rate, climb, sink, flap Hz,
// amplitude, glide articulation (elev, sweep, twist, elbow, wrist, hand), flapping duty cycle
struct Cfg
{
    float speed, minSpeed, maxBank, roll, climb, sink, freq, amp;
    float elev, sweep, twist, elbow, wrist, hand;
    float duty;
};
const Cfg FlightTable[SpeciesCount] = {
    {10.5f, 6, 0.85f, 2.6f, 2.4f, 0.7f, 3.1f, 0.85f, 0.1f, -0.12f, 0.02f, 0.22f, 0.5f, -0.22f, 0.35f},
    {9, 5, 0.95f, 3, 2.2f, 0.8f, 2.7f, 0.95f, 0.12f, -0.1f, 0.02f, 0.25f, 0.55f, -0.12f, 0.85f},
    {11, 7, 0.6f, 1.4f, 1.6f, 0.55f, 1.6f, 0.62f, 0.02f, -0.05f, 0.02f, 0.12f, 0.12f, 0.06f, 0.2f},
    {9, 6, 0.55f, 1.2f, 1.2f, 0.45f, 2.0f, 0.55f, 0.1f, -0.18f, 0.03f, 0.42f, 0.85f, -0.28f, 0.02f},
};

// ---------------------------------------------------------------- small maths

struct BV3
{
    float x = 0, y = 0, z = 0;
};
BV3 operator+(BV3 a, BV3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
BV3 operator-(BV3 a, BV3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
BV3 operator*(BV3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
BV3 Mix(BV3 a, BV3 b, float t) { return a + (b - a) * t; }
struct BQ
{
    float x = 0, y = 0, z = 0, w = 1;
};
BQ QAxis(float ax, float ay, float az, float angle)
{
    const float s = std::sin(angle * 0.5f);
    return {ax * s, ay * s, az * s, std::cos(angle * 0.5f)};
}
BQ QMul(BQ a, BQ b)
{
    return {a.x * b.w + a.w * b.x + a.y * b.z - a.z * b.y, a.y * b.w + a.w * b.y + a.z * b.x - a.x * b.z,
            a.z * b.w + a.w * b.z + a.x * b.y - a.y * b.x, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
BV3 QRot(BQ q, BV3 v)
{
    const float tx = 2 * (q.y * v.z - q.z * v.y), ty = 2 * (q.z * v.x - q.x * v.z), tz = 2 * (q.x * v.y - q.y * v.x);
    return {v.x + q.w * tx + q.y * tz - q.z * ty, v.y + q.w * ty + q.z * tx - q.x * tz, v.z + q.w * tz + q.x * ty - q.y * tx};
}
float Clamp(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
float Approach(float rate, float dt) { return 1.0f - std::exp(-rate * dt); }
float AngleDiff(float a, float b)
{
    float d = std::fmod(a - b, Tau);
    if (d > Pi)
        d -= Tau;
    else if (d < -Pi)
        d += Tau;
    return d;
}
float Smooth01(float a, float b, float x)
{
    const float t = Clamp((x - a) / (b - a), 0, 1);
    return t * t * (3 - 2 * t);
}
float Spow(float x, float e) { return (x < 0 ? -1.0f : 1.0f) * std::pow(std::fabs(x), e); }
// BirdShapes.js prof: smooth piecewise profile
float Prof(const Pts& pts, float x)
{
    if (x <= pts.front().first)
        return pts.front().second;
    for (size_t i = 1; i < pts.size(); ++i)
        if (x <= pts[i].first)
        {
            const auto a = pts[i - 1], b = pts[i];
            const float t = (x - a.first) / (b.first - a.first);
            const float s = t * t * (3 - 2 * t);
            return a.second + (b.second - a.second) * (0.4f * t + 0.6f * s);
        }
    return pts.back().second;
}
Pts Column(const Species& sp, int k)
{
    Pts p;
    for (const auto& s : sp.body)
        p.push_back({s[0], s[k]});
    return p;
}

// ---------------------------------------------------------------- the template (buildBird)

constexpr int NB = 12, NR = 10, NBL = 5, NRB = 8, NT = 7, NS = 9;
const float RingS[NB] = {0, 0.06f, 0.16f, 0.29f, 0.43f, 0.56f, 0.66f, 0.74f, 0.81f, 0.875f, 0.94f, 1};
const float BillT[NBL] = {0, 0.22f, 0.48f, 0.72f, 0.9f};
const float WV[4] = {0, 0.18f, 0.55f, 1};
const float AirTop[4] = {0.05f, 1.0f, 0.62f, 0};
const float AirBot[4] = {0.05f, 0.42f, 0.22f, 0};
const float AirCamber[4] = {0, 0.05f, 0.045f, 0};

// one rest vertex: position, part (0 body .. 4 tail, 5 arm, 6 forearm, 7 hand), the parent
// bone's blend weight at the wing joints, and its place in the plumage atlas
struct RV
{
    BV3 p;
    int part;
    float w;
    float u, v;
};
struct Rest
{
    std::vector<RV> v;
    std::vector<uint32_t> idx;
    BV3 S, E, W; // left wing joints
    BV3 tailBase;
};

// Atlas: body (along, around) in u 0..0.5 / v 0..0.5; bill in u 0.5..0.75; tail in u 0.75..1;
// wings (span, chord) in v 0.5..1, the upper surface from the leading edge at v 0.75 up to the
// trailing edge at 0.5, the lower surface down to 1.
Rest BuildRest(const Species& sp)
{
    Rest r;
    auto vtx = [&](BV3 p, int part, float w, float u, float v)
    {
        r.v.push_back({p, part, w, u, v});
        return uint32_t(r.v.size() - 1);
    };
    const Pts Y = Column(sp, 1), HW = Column(sp, 2), HT = Column(sp, 3), HB = Column(sp, 4);
    const float z0 = sp.body.front()[0], z1 = sp.body.back()[0];
    auto partOf = [&](float z) { return z < sp.neck[0] ? 0 : z < sp.neck[1] ? 1 : 2; };
    // body loft (the seam column doubled so the atlas does not wrap)
    const uint32_t firstBody = uint32_t(r.v.size());
    for (int i = 0; i < NB; ++i)
    {
        const float s = RingS[i], z = z0 + (z1 - z0) * s;
        const float y = Prof(Y, z), hw = Prof(HW, z), ht = Prof(HT, z), hb = Prof(HB, z);
        for (int j = 0; j <= NR; ++j)
        {
            const float th = float(j) / NR * Tau, c = std::cos(th), sn = std::sin(th);
            vtx({hw * Spow(sn, 0.85f), y + (c > 0 ? ht : hb) * Spow(c, 0.85f), z}, partOf(z), 0, s * 0.5f, float(j) / NR * 0.5f);
        }
    }
    const uint32_t rear = vtx({0, Prof(Y, z0), z0 - Prof(HW, z0) * 0.5f}, 0, 0, 0.0f, 0.25f);
    const uint32_t front = vtx({0, Prof(Y, z1), z1 + Prof(HW, z1) * 0.6f}, 2, 0, 0.5f, 0.25f);
    auto P = [&](int i, int j) { return firstBody + uint32_t(i * (NR + 1) + j); };
    for (int i = 0; i < NB - 1; ++i)
        for (int j = 0; j < NR; ++j)
            r.idx.insert(r.idx.end(), {P(i, j), P(i + 1, j), P(i, j + 1), P(i, j + 1), P(i + 1, j), P(i + 1, j + 1)});
    for (int j = 0; j < NR; ++j)
    {
        r.idx.insert(r.idx.end(), {rear, P(0, j), P(0, j + 1)});
        r.idx.insert(r.idx.end(), {front, P(NB - 1, j + 1), P(NB - 1, j)});
    }
    // bill
    const Bill& Bl = sp.bill;
    auto billAt = [&](float t, float& cz, float& cy, float& hw, float& up, float& low)
    {
        cz = Bl.z + t * Bl.len * std::cos(Bl.pitch);
        cy = Bl.y + t * Bl.len * std::sin(Bl.pitch) - Bl.hook * Smooth01(0.72f, 1, t) * 1.3f;
        hw = Bl.w * (1 - 0.72f * std::pow(t, 1.3f));
        up = Bl.up * (1 - 0.65f * std::pow(t, 1.2f));
        low = Bl.low * (1 - 0.7f * std::pow(t, 1.1f)) + Bl.gonys * std::exp(-std::pow((t - 0.78f) / 0.08f, 2.0f)) +
              Bl.pouch * std::sin(Pi * std::min(1.0f, std::pow(t, 0.7f) * 1.05f)) * (1 - t * 0.6f);
    };
    const uint32_t firstBill = uint32_t(r.v.size());
    for (int i = 0; i < NBL; ++i)
    {
        float cz, cy, hw, up, low;
        billAt(BillT[i], cz, cy, hw, up, low);
        for (int j = 0; j <= NRB; ++j)
        {
            const float th = float(j) / NRB * Tau, c = std::cos(th), sn = std::sin(th);
            vtx({hw * Spow(sn, 0.8f), cy + (c > 0 ? up : low) * Spow(c, 0.8f), cz}, 3, 0, 0.5f + BillT[i] * 0.25f,
                float(j) / NRB * 0.5f);
        }
    }
    {
        float cz, cy, hw, up, low;
        billAt(1, cz, cy, hw, up, low);
        const uint32_t tip = vtx({0, cy - low * 0.2f, cz + 0.002f}, 3, 0, 0.75f, 0.25f);
        auto PB = [&](int i, int j) { return firstBill + uint32_t(i * (NRB + 1) + j); };
        for (int i = 0; i < NBL - 1; ++i)
            for (int j = 0; j < NRB; ++j)
                r.idx.insert(r.idx.end(), {PB(i, j), PB(i + 1, j), PB(i, j + 1), PB(i, j + 1), PB(i + 1, j), PB(i + 1, j + 1)});
        for (int j = 0; j < NRB; ++j)
            r.idx.insert(r.idx.end(), {tip, PB(NBL - 1, j + 1), PB(NBL - 1, j)});
    }
    // tail: a thin fan, upper and lower layer
    const Tail& T = sp.tail;
    r.tailBase = {0, T.y, T.z};
    const uint32_t firstTail = uint32_t(r.v.size());
    for (int layer = 0; layer < 2; ++layer)
    {
        const float lsign = layer == 0 ? 1.0f : -1.0f;
        for (int rr = 0; rr < 3; ++rr)
        {
            const float f = rr / 2.0f;
            for (int k = 0; k < NT; ++k)
            {
                const float a = float(k) / (NT - 1) * 2 - 1;
                const float len = T.len[int(std::lround(std::fabs(a) * 3))];
                const float ang = a * T.spread, bx = a * T.w;
                vtx({bx + std::sin(ang) * len * f,
                     T.y + lsign * 0.0012f * (1 - f * 0.6f) * (sp.length / 0.4f) - 0.004f * f * f * (sp.length / 0.4f),
                     T.z - std::cos(ang) * len * f},
                    4, 0, 0.75f + float(k) / (NT - 1) * 0.25f, f * 0.5f);
            }
        }
    }
    auto PT = [&](int layer, int rr, int k) { return firstTail + uint32_t(layer * 3 * NT + rr * NT + k); };
    for (int rr = 0; rr < 2; ++rr)
        for (int k = 0; k < NT - 1; ++k)
        {
            r.idx.insert(r.idx.end(), {PT(0, rr, k), PT(0, rr, k + 1), PT(0, rr + 1, k), PT(0, rr, k + 1), PT(0, rr + 1, k + 1),
                                       PT(0, rr + 1, k)});
            r.idx.insert(r.idx.end(), {PT(1, rr, k), PT(1, rr + 1, k), PT(1, rr, k + 1), PT(1, rr, k + 1), PT(1, rr + 1, k),
                                       PT(1, rr + 1, k + 1)});
        }
    // wings
    const Wing& Wg = sp.wing;
    const float e = Wg.elbow, wr = Wg.wrist;
    const float us[NS] = {0, e * 0.5f, e, (e + wr) / 2, wr, wr + (1 - wr) * 0.3f, wr + (1 - wr) * 0.58f, wr + (1 - wr) * 0.82f, 1};
    auto joint = [&](float u)
    {
        const float le = Prof(Wg.le, u), c = Prof(Wg.chord, u);
        return BV3{Wg.root[0] + u * Wg.span, Wg.root[1], Wg.root[2] + le - c * 0.22f};
    };
    r.S = joint(0), r.E = joint(Wg.elbow), r.W = joint(Wg.wrist);
    const std::array<std::pair<int, int>, 6> ring = {{{0, 1}, {1, 1}, {2, 1}, {3, 0}, {2, -1}, {1, -1}}};
    for (int sideI = 0; sideI < 2; ++sideI)
    {
        const float side = sideI == 0 ? 1.0f : -1.0f;
        const uint32_t first = uint32_t(r.v.size());
        for (int i = 0; i < NS; ++i)
        {
            const float u = us[i];
            const float le = Prof(Wg.le, u), c = Prof(Wg.chord, u);
            const float th = u < Wg.wrist ? Wg.thick[0] + (Wg.thick[1] - Wg.thick[0]) * u / Wg.wrist
                                          : Wg.thick[1] + (Wg.thick[2] - Wg.thick[1]) * (u - Wg.wrist) / (1 - Wg.wrist);
            const float x = Wg.root[0] + u * Wg.span;
            int pt;
            float w;
            if (u < Wg.elbow - 1e-6f)
                pt = 5, w = 0;
            else if (u < Wg.elbow + 1e-6f)
                pt = 6, w = 0.5f;
            else if (u < Wg.wrist - 1e-6f)
                pt = 6, w = 0;
            else if (u < Wg.wrist + 1e-6f)
                pt = 7, w = 0.5f;
            else
                pt = 7, w = 0;
            for (const auto& [k, s] : ring)
            {
                const float v = WV[k];
                const float ht = s >= 0 ? AirTop[k] : -AirBot[k];
                const float y = Wg.root[1] + (AirCamber[k] + ht * th) * c;
                // atlas: the upper surface from the leading edge (0.75) to the trailing edge (0.5),
                // the lower one on to 1
                const float av = s >= 0 ? 0.75f - 0.25f * v : 0.75f + 0.25f * v;
                vtx({side * x, y, Wg.root[2] + le - v * c}, pt, w, u, av);
            }
        }
        auto PW = [&](int i, int j) { return first + uint32_t(i * 6 + (j % 6)); };
        for (int i = 0; i < NS - 1; ++i)
            for (int j = 0; j < 6; ++j)
            {
                if (side < 0)
                    r.idx.insert(r.idx.end(), {PW(i, j), PW(i, j + 1), PW(i + 1, j), PW(i, j + 1), PW(i + 1, j + 1), PW(i + 1, j)});
                else
                    r.idx.insert(r.idx.end(), {PW(i, j), PW(i + 1, j), PW(i, j + 1), PW(i, j + 1), PW(i + 1, j), PW(i + 1, j + 1)});
            }
    }
    return r;
}

// BirdPose.js setWings / flapWings: the left wing's three cumulative bone rotations
struct WingPose
{
    BQ a, b, c;
};
WingPose SetWings(float elev, float sweep, float twist, float elbow, float wrist, float hand, float handTwist)
{
    WingPose p;
    p.a = QMul(QMul(QAxis(0, 0, 1, elev), QAxis(0, 1, 0, sweep)), QAxis(1, 0, 0, twist));
    p.b = QMul(p.a, QAxis(0, 1, 0, elbow));
    p.c = QMul(QMul(p.b, QAxis(0, 1, 0, wrist)), QAxis(0, 0, 1, hand));
    if (handTwist != 0)
        p.c = QMul(p.c, QAxis(1, 0, 0, handTwist));
    return p;
}
WingPose FlapWings(const Cfg& g, float ph, float amp)
{
    const float w = ph - 0.12f * std::sin(ph);
    const float c = std::cos(w), s = std::sin(w);
    const float up = amp * std::max(0.0f, -s);
    return SetWings(g.elev + amp * (0.9f * c + 0.12f), g.sweep - amp * 0.18f * s + up * 0.1f, g.twist + amp * 0.22f * s,
                    g.elbow + up * 0.55f, g.wrist + up * 0.9f, g.hand + amp * 0.55f * std::cos(w - 0.7f) - up * 0.25f,
                    -amp * 0.15f * s);
}

struct MeshOut
{
    std::vector<WgrMeshVertex> v;
    std::vector<uint32_t> idx;
};
// BirdBatch.js vertex stage, baked: the wings posed by their bones (mirrored for the right wing,
// blended at the joints), the tail spread about the centre line
MeshOut Pose(const Rest& r, const WingPose& wp, float tailSpread)
{
    MeshOut m;
    m.idx = r.idx;
    m.v.resize(r.v.size());
    for (size_t i = 0; i < r.v.size(); ++i)
    {
        const RV& a = r.v[i];
        BV3 p = a.p;
        if (a.part == 4)
        {
            const BV3 d = a.p - r.tailBase;
            p = r.tailBase + BV3{d.x * tailSpread, d.y, d.z};
        }
        else if (a.part >= 5)
        {
            const float side = a.p.x < 0 ? -1.0f : 1.0f;
            auto mir = [&](BQ q) { return BQ{q.x, q.y * side, q.z * side, q.w}; };
            const BV3 sx{side, 1, 1};
            const BV3 S{r.S.x * side, r.S.y, r.S.z}, E{r.E.x * side, r.E.y, r.E.z}, Wj{r.W.x * side, r.W.y, r.W.z};
            const BQ qa = mir(wp.a), qb = mir(wp.b), qc = mir(wp.c);
            const BV3 Ep = S + QRot(qa, E - S);
            const BV3 Wp = Ep + QRot(qb, Wj - E);
            const BV3 pa = S + QRot(qa, a.p - S);
            const BV3 pb = Ep + QRot(qb, a.p - E);
            const BV3 pc = Wp + QRot(qc, a.p - Wj);
            const int seg = a.part - 5;
            p = seg == 0 ? pa : seg == 1 ? Mix(pb, pa, a.w) : Mix(pc, pb, a.w);
            (void)sx;
        }
        WgrMeshVertex& w = m.v[i];
        w.pos = {p.x, p.y, p.z};
        w.normal = {0, 0, 0};
        w.uv = {a.u, a.v};
        w.uv1 = w.uv;
    }
    for (size_t i = 0; i + 2 < m.idx.size(); i += 3)
    {
        auto& A = m.v[m.idx[i]].pos;
        auto& B = m.v[m.idx[i + 1]].pos;
        auto& C = m.v[m.idx[i + 2]].pos;
        const float ux = B.x - A.x, uy = B.y - A.y, uz = B.z - A.z, wx = C.x - A.x, wy = C.y - A.y, wz = C.z - A.z;
        const float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
        for (int k = 0; k < 3; ++k)
        {
            auto& n = m.v[m.idx[i + k]].normal;
            n.x += nx, n.y += ny, n.z += nz;
        }
    }
    for (auto& w : m.v)
    {
        const float l = std::sqrt(w.normal.x * w.normal.x + w.normal.y * w.normal.y + w.normal.z * w.normal.z);
        w.normal = l > 1e-12f ? WgrVec3{w.normal.x / l, w.normal.y / l, w.normal.z / l} : WgrVec3{0, 1, 0};
    }
    return m;
}

// ---------------------------------------------------------------- the plumage (BirdBatch.js colours)

struct Rgb
{
    float r, g, b;
};
Rgb MixC(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
float Hash(float x, float y)
{
    const float h = std::sin(x * 127.1f + y * 311.7f) * 43758.5453f;
    return h - std::floor(h);
}

// The colour at an atlas texel (TW's fragment rules evaluated at the rest point the texel maps to).
Rgb Plumage(const Species& sp, int s, float tu, float tv)
{
    // which part, and its coordinates
    enum
    {
        Body,
        BillP,
        TailP,
        WingTop,
        WingBot
    } part;
    float u = 0, v = 0;
    BV3 P, N{0, 1, 0};
    if (tv >= 0.5f)
    {
        u = tu;
        part = tv < 0.75f ? WingTop : WingBot;
        v = part == WingTop ? (0.75f - tv) / 0.25f : (tv - 0.75f) / 0.25f;
    }
    else if (tu < 0.5f)
    {
        part = Body;
        const float sAlong = tu / 0.5f, th = tv / 0.5f * Tau;
        const float z0 = sp.body.front()[0], z1 = sp.body.back()[0], z = z0 + (z1 - z0) * sAlong;
        const Pts Y = Column(sp, 1), HW = Column(sp, 2), HT = Column(sp, 3), HB = Column(sp, 4);
        const float hw = Prof(HW, z), hh = std::cos(th) > 0 ? Prof(HT, z) : Prof(HB, z);
        const float c = std::cos(th), sn = std::sin(th);
        P = {hw * Spow(sn, 0.85f), Prof(Y, z) + hh * Spow(c, 0.85f), z};
        const float nx = sn / std::max(hw, 1e-4f), ny = c / std::max(hh, 1e-4f);
        const float l = std::sqrt(nx * nx + ny * ny);
        N = {nx / l, ny / l, 0};
    }
    else if (tu < 0.75f)
    {
        part = BillP;
        u = (tu - 0.5f) / 0.25f;
        v = tv / 0.5f;
    }
    else
    {
        part = TailP;
        u = (tu - 0.75f) / 0.25f;
        v = tv / 0.5f;
    }
    const bool isWing = part == WingTop || part == WingBot, top = part == WingTop;
    const float fnz = (Hash(tu * 97.0f, tv * 89.0f) - 0.5f) * 1.2f;
    Rgb c{0.5f, 0.5f, 0.5f};
    if (s == Gull)
    {
        // laughing gull (breeding): slate mantle and upperwing, black primaries with a white
        // trailing edge, white body and tail, black hood, dark red bill
        const Rgb white{0.93f, 0.93f, 0.92f}, slate{0.38f, 0.4f, 0.43f}, black{0.05f, 0.05f, 0.055f};
        const float mantle = Smooth01(0.1f, 0.45f, N.y) * Smooth01(-0.1f, -0.07f, P.z) * (1 - Smooth01(0.075f, 0.1f, P.z));
        const float head = Smooth01(0.1f, 0.115f, P.z);
        const float eyeArc = (1 - Smooth01(0.004f, 0.006f, std::hypot(P.z - 0.139f, P.y - 0.037f))) *
                             Smooth01(0.002f, 0.004f, std::hypot(P.z - 0.141f, P.y - 0.032f));
        const Rgb body = MixC(MixC(white, slate, mantle), black, head * (1 - eyeArc));
        const float tipK = Smooth01(0.68f, 0.76f, u);
        const float edge = Smooth01(0.9f, 0.97f, v) * (1 - tipK);
        const Rgb wingTop = MixC(MixC(slate, white, edge), black, tipK);
        const Rgb wingBot = MixC(MixC(white, Rgb{0.72f, 0.73f, 0.75f}, Smooth01(0.3f, 0.9f, u) * 0.6f), black, Smooth01(0.78f, 0.9f, u));
        c = isWing ? (top ? wingTop : wingBot) : part == TailP ? white : body;
        if (part == BillP)
            c = {0.42f, 0.07f, 0.07f};
    }
    else if (s == Tern)
    {
        // royal tern: pale grey mantle and upperwing with a dusky primary wedge, white body,
        // black crest behind a white forehead, orange bill
        const Rgb white{0.95f, 0.95f, 0.94f}, grey{0.74f, 0.77f, 0.8f};
        const float mantle = Smooth01(0.15f, 0.5f, N.y) * Smooth01(-0.095f, -0.07f, P.z) * (1 - Smooth01(0.075f, 0.1f, P.z));
        const float cap = Smooth01(0.108f, 0.12f, P.z) * (1 - Smooth01(0.145f, 0.158f, P.z)) * Smooth01(0.028f, 0.033f, P.y);
        const Rgb body = MixC(MixC(white, grey, mantle), Rgb{0.04f, 0.04f, 0.045f}, cap);
        const float wedge = Smooth01(0.66f, 0.8f, u) * ((1 - Smooth01(0.0f, 0.2f, v)) * 0.5f + 0.5f);
        const Rgb wingTop = MixC(grey, Rgb{0.32f, 0.33f, 0.35f}, wedge);
        const Rgb wingBot = MixC(white, Rgb{0.55f, 0.56f, 0.58f}, Smooth01(0.82f, 0.95f, u) * Smooth01(0.4f, 1.0f, v));
        c = isWing ? (top ? wingTop : wingBot) : part == TailP ? MixC(white, grey, 0.3f) : body;
        if (part == BillP)
            c = {0.95f, 0.45f, 0.1f};
    }
    else if (s == Pelican)
    {
        // brown pelican: silvery grey-brown back, dark belly, cream-white head and neck, dark
        // flight feathers, grey bill with a dark pouch
        const Rgb back = MixC(Rgb{0.44f, 0.42f, 0.38f}, Rgb{0.68f, 0.67f, 0.63f}, Smooth01(0, 0.6f, fnz) * Smooth01(0, 0.6f, N.y));
        const Rgb belly{0.24f, 0.22f, 0.2f};
        const float neckZone = Smooth01(0.12f, 0.17f, P.z);
        const Rgb head = MixC(Rgb{0.92f, 0.9f, 0.82f}, Rgb{0.95f, 0.86f, 0.55f}, Smooth01(0.27f, 0.3f, P.z) * Smooth01(0.13f, 0.15f, P.y));
        const Rgb neckCol = MixC(Rgb{0.9f, 0.88f, 0.82f}, head, Smooth01(0.2f, 0.235f, P.z));
        const Rgb body = MixC(MixC(belly, back, Smooth01(-0.35f, 0.25f, N.y)), MixC(neckCol, head, Smooth01(0.22f, 0.25f, P.z)), neckZone);
        const float flight = std::max(Smooth01(0.52f, 0.6f, u), Smooth01(0.45f, 0.62f, v));
        const Rgb wingTop = MixC(MixC(Rgb{0.56f, 0.55f, 0.51f}, Rgb{0.72f, 0.71f, 0.67f}, (fnz * 0.5f + 0.5f) * 0.6f),
                                 Rgb{0.1f, 0.09f, 0.085f}, flight);
        const Rgb wingBot = MixC(Rgb{0.2f, 0.19f, 0.18f}, Rgb{0.45f, 0.44f, 0.42f},
                                 Smooth01(0.2f, 0.35f, v) * (1 - Smooth01(0.4f, 0.6f, v)) * (1 - Smooth01(0.3f, 0.6f, u)));
        c = isWing ? (top ? wingTop : wingBot) : part == TailP ? Rgb{0.2f, 0.19f, 0.18f} : body;
        if (part == BillP)
        {
            const float pouch = Smooth01(0.3f, 0.38f, v) * (1 - Smooth01(0.62f, 0.7f, v));
            c = MixC(MixC(Rgb{0.58f, 0.54f, 0.5f}, Rgb{0.75f, 0.55f, 0.45f}, Smooth01(0.6f, 1.0f, u)), Rgb{0.22f, 0.21f, 0.2f}, pouch);
        }
    }
    else
    {
        // magnificent frigatebird (female): black with a white breast and a brown bar on the upperwing
        const Rgb black{0.035f, 0.035f, 0.04f}, white{0.9f, 0.9f, 0.88f};
        const float breast = (1 - Smooth01(-0.3f, 0.2f, N.y)) * Smooth01(-0.06f, 0.0f, P.z) * (1 - Smooth01(0.1f, 0.14f, P.z));
        const Rgb body = MixC(black, white, breast);
        const float bar = Smooth01(0.08f, 0.14f, u) * (1 - Smooth01(0.38f, 0.45f, u)) * Smooth01(0.12f, 0.22f, v) * (1 - Smooth01(0.4f, 0.5f, v));
        const Rgb wingTop = MixC(black, Rgb{0.32f, 0.25f, 0.18f}, bar);
        c = isWing ? (top ? wingTop : black) : part == TailP ? black : body;
        if (part == BillP)
            c = {0.5f, 0.52f, 0.56f};
    }
    // the eye on the head
    if (part == Body && std::fabs(P.x) > 0.004f)
    {
        const float ed = std::hypot(P.z - sp.eye[0], P.y - sp.eye[1]);
        const float iris = 1 - Smooth01(sp.eye[2] * 0.8f, sp.eye[2], ed);
        const float pupil = 1 - Smooth01(sp.eye[2] * 0.45f, sp.eye[2] * 0.6f, ed);
        c = MixC(MixC(c, s == Pelican ? Rgb{0.8f, 0.75f, 0.55f} : Rgb{0.05f, 0.03f, 0.02f}, iris), Rgb{0.01f, 0.01f, 0.01f}, pupil);
    }
    // fine feather texture, darker feather bases toward the trailing edge
    const float k = (fnz * 0.06f + 1.0f) * (isWing ? 1.0f - 0.08f * Smooth01(0.5f, 1.0f, v) : 1.0f);
    return {c.r * k, c.g * k, c.b * k};
}

std::uniform_real_distribution<float> Unit(0.0f, 1.0f);
} // namespace

// ---------------------------------------------------------------- meshes

void BirdFlocks::EnsureMeshes()
{
    if (_built)
        return;
    _built = true;
    EngineWgpu::AcquireProducerWindow("Bird meshes");
    _meshes.assign(size_t(SpeciesCount) * (Phases + 1), 0);
    _indexCounts.assign(_meshes.size(), 0);
    for (int s = 0; s < SpeciesCount; ++s)
    {
        const Rest rest = BuildRest(SpeciesTable[s]);
        const Cfg& g = FlightTable[s];
        for (int p = 0; p <= Phases; ++p)
        {
            const WingPose wp = p < Phases ? FlapWings(g, Tau * p / Phases, g.amp) : FlapWings(g, Pi / 2, 0);
            const MeshOut m = Pose(rest, wp, s == Frigate ? 1.6f : 1.15f);
            const size_t i = size_t(s) * (Phases + 1) + p;
            _meshes[i] = wgr_mesh_create(_renderer, WgrSlice<WgrMeshVertex>(m.v), WgrSlice<uint32_t>(m.idx));
            _indexCounts[i] = uint32_t(m.idx.size());
        }
        constexpr int TW = 128, TH = 128;
        std::vector<uint8_t> px(TW * TH * 4);
        for (int y = 0; y < TH; ++y)
            for (int x = 0; x < TW; ++x)
            {
                const Rgb c = Plumage(SpeciesTable[s], s, (x + 0.5f) / TW, (y + 0.5f) / TH);
                uint8_t* q = &px[(y * TW + x) * 4];
                q[0] = uint8_t(Clamp(c.r, 0, 1) * 255);
                q[1] = uint8_t(Clamp(c.g, 0, 1) * 255);
                q[2] = uint8_t(Clamp(c.b, 0, 1) * 255);
                q[3] = 255;
            }
        _textures.push_back(
            wgr_texture_create(_renderer, TW, TH, WGR_TEXTURE_RGBA8, 1, WGR_TEXTURE_GEN_MIPS, px.data(), uint32_t(px.size())));
    }
    LOG_INFO(Graphics, "Birds: {} species x {} flap phases + glide baked", int(SpeciesCount), Phases);
}

// ---------------------------------------------------------------- flight (Flight.js Flyer)

float BirdFlocks::Floor(float x, float z) const
{
    return std::max(Wildlife::Ground(x, z), Wildlife::SeaLevel(x, z));
}

void BirdFlocks::Steer(Flyer& f, float dt, float tx, float ty, float tz, float speed, float power, float turnGain,
                       float windX, float windZ)
{
    const Cfg& c = FlightTable[f.species];
    // heading toward the target, with a gentle wander so paths never look ruled
    f.wander += dt * 0.35f;
    const float wob = std::sin(f.wander * 1.3f) * 0.12f + std::sin(f.wander * 0.47f + 2) * 0.1f;
    const float want = std::atan2(tx - f.x, tz - f.z) + wob * turnGain;
    const float err = AngleDiff(want, f.yaw);
    const float V = std::max(f.speed, 1.0f);
    const float maxRate = Grav * std::tan(c.maxBank) / V;
    const float rate = Clamp(err * 1.2f * turnGain, -maxRate, maxRate);
    const float bankWant = std::atan(rate * V / Grav);
    f.bank += Clamp(bankWant - f.bank, -c.roll * dt, c.roll * dt);
    f.yaw += Grav * std::tan(f.bank) / V * dt;
    // height
    const float dy = ty - f.y;
    const float climbWant = Clamp(dy * 0.6f, -c.sink * 3.5f, c.climb * (power > 1.5f ? 1.6f : 1.0f));
    f.vy += (climbWant - f.vy) * Approach(2.5f, dt);
    f.gamma = std::asin(Clamp(f.vy / V, -0.9f, 0.9f));
    // speed: slower when climbing hard, faster when descending
    f.speed += (speed - f.vy * 0.4f - f.speed) * Approach(1.2f, dt);
    f.speed = std::max(f.speed, c.minSpeed * 0.5f);
    // power: flap when climbing or slow; otherwise the species' flap / glide rhythm
    float need = Clamp(f.vy / c.climb, 0, 1) * 1.1f + Clamp((speed - f.speed) / 2, 0, 1) * 0.6f;
    if (power > 1.5f)
        need = 1.3f;
    f.burst -= dt;
    if (f.burst <= 0)
    {
        f.bursting = Unit(_rng) < c.duty;
        f.burst = f.bursting ? 0.8f + Unit(_rng) * 2.2f : 1.5f + Unit(_rng) * 4.0f;
    }
    const float rhythm = f.bursting ? 0.75f : 0.0f;
    f.flapWant = power <= 0 ? 0 : Clamp(std::max(need, rhythm * power), 0, power > 1.5f ? 1.25f : 1.0f);
    // integrate (ground velocity = air velocity + some wind drift)
    const float h = std::cos(f.gamma) * f.speed;
    const float windK = f.species == Frigate ? 0.6f : 0.35f;
    f.x += (std::sin(f.yaw) * h + windX * windK) * dt;
    f.y += f.vy * dt;
    f.z += (std::cos(f.yaw) * h + windZ * windK) * dt;
}

void BirdFlocks::Animate(Flyer& f, float dt)
{
    const Cfg& c = FlightTable[f.species];
    f.flap += (f.flapWant - f.flap) * Approach(f.flapWant > f.flap ? 6.0f : 2.5f, dt);
    const float fr = c.freq * f.freqMul * (0.85f + 0.3f * std::min(f.flap, 1.25f));
    if (f.flap > 0.02f)
        f.phase = std::fmod(f.phase + Tau * fr * dt, Tau);
    else
    {
        // ease toward the glide phase (quarter stroke, wings level)
        const float d = AngleDiff(Pi / 2, f.phase);
        f.phase += Clamp(d, -Tau * fr * dt, Tau * fr * dt);
    }
    const float pitchWant = f.gamma * 0.7f;
    f.pitch += (pitchWant - f.pitch) * Approach(4, dt);
}

// ---------------------------------------------------------------- the birds around the camera

void BirdFlocks::Spawn(const Vector3& camera)
{
    const float sea = Wildlife::SeaLevel(camera.X(), camera.Z());
    auto water = [&](float x, float z) { return Wildlife::Ground(x, z) < Wildlife::SeaLevel(x, z) - 0.5f; };
    // a random point near the camera, over water if asked (a few tries)
    auto pick = [&](float rMin, float rMax, bool overWater, float& x, float& z)
    {
        for (int k = 0; k < 12; ++k)
        {
            const float a = Tau * Unit(_rng), r = rMin + (rMax - rMin) * Unit(_rng);
            x = camera.X() + std::cos(a) * r;
            z = camera.Z() + std::sin(a) * r;
            if (!overWater || water(x, z))
                return true;
        }
        return false;
    };
    auto make = [&](int species)
    {
        Flyer f;
        f.species = species;
        f.phase = Tau * Unit(_rng);
        f.burst = Unit(_rng) * 3;
        f.freqMul = 0.94f + Unit(_rng) * 0.12f;
        f.wander = Unit(_rng) * 100;
        f.scale = 0.92f + Unit(_rng) * 0.16f;
        f.speed = FlightTable[species].speed;
        f.dir = Unit(_rng) < 0.5f ? -1.0f : 1.0f;
        return f;
    };
    auto count = [&](int species)
    {
        int n = 0;
        for (const Flyer& f : _birds)
            n += f.species == species;
        return n;
    };
    // the first fill places birds all around; later ones arrive from 180-260 m out
    const bool first = _birds.empty();
    const float r0 = first ? 20.0f : 180.0f, r1 = first ? 220.0f : 260.0f;
    float x, z;
    // gulls: roaming, circling over the coast and the sea
    while (count(Gull) < 6 && pick(r0, r1, false, x, z))
    {
        Flyer f = make(Gull);
        f.mode = 0;
        f.cx = x, f.cz = z;
        f.radius = 20 + Unit(_rng) * 30;
        f.cy = 10 + Unit(_rng) * 28;
        f.t = Tau * Unit(_rng);
        f.x = f.cx + std::cos(f.t) * f.radius;
        f.z = f.cz + std::sin(f.t) * f.radius;
        f.y = Floor(f.x, f.z) + f.cy;
        f.yaw = f.t + f.dir * Pi / 2;
        _birds.push_back(f);
    }
    // terns: patrolling lanes beyond the surf, 8-12 m over the sea
    while (count(Tern) < 3 && pick(r0, r1, true, x, z))
    {
        Flyer f = make(Tern);
        f.mode = 1;
        const float a = Tau * Unit(_rng);
        f.lx0 = x, f.lz0 = z;
        f.lx1 = x + std::cos(a) * 140, f.lz1 = z + std::sin(a) * 140;
        f.cy = 8 + Unit(_rng) * 4;
        f.x = x, f.z = z, f.y = sea + f.cy;
        f.yaw = std::atan2(f.lx1 - x, f.lz1 - z);
        _birds.push_back(f);
    }
    // pelicans: a line of five skimming the swell in ground effect
    if (count(Pelican) == 0 && pick(first ? 40.0f : r0, r1, true, x, z))
    {
        const int leader = int(_birds.size());
        for (int k = 0; k < 5; ++k)
        {
            Flyer f = make(Pelican);
            f.mode = 2;
            f.leader = k == 0 ? -1 : leader;
            f.slot = k;
            f.cx = x, f.cz = z;
            f.radius = 90 + Unit(_rng) * 60;
            f.t = Tau * Unit(_rng);
            f.x = x + std::cos(f.t) * f.radius - k * 3.0f;
            f.z = z + std::sin(f.t) * f.radius;
            f.y = sea + 1.6f;
            f.yaw = f.t + Pi / 2;
            _birds.push_back(f);
        }
    }
    // frigatebirds: circling high on the wind
    while (count(Frigate) < 2 && pick(r0, r1 + 100, false, x, z))
    {
        Flyer f = make(Frigate);
        f.mode = 3;
        f.cx = x, f.cz = z;
        f.radius = 35 + Unit(_rng) * 35;
        f.cy = 70 + Unit(_rng) * 80;
        f.t = Tau * Unit(_rng);
        f.x = f.cx + std::cos(f.t) * f.radius;
        f.z = f.cz + std::sin(f.t) * f.radius;
        f.y = sea + f.cy;
        f.yaw = f.t + f.dir * Pi / 2;
        _birds.push_back(f);
    }
}

void BirdFlocks::Update(const Vector3& camera, float time)
{
    static const bool enabled = []
    {
        const char* v = std::getenv("WGR_BIRDS");
        return !(v && v[0] == '0');
    }();
    if (!enabled || !GLandscape || !_renderer)
        return;
    const float sea = Wildlife::SeaLevel(camera.X(), camera.Z());
    float dt = _lastTime < 0 ? 0.0f : time - _lastTime;
    _lastTime = time;
    dt = Clamp(dt, 0.0f, 0.1f);

    // seabirds keep to the coast: is there sea within ~600 m of the camera? (sampled every 2 s)
    _respawn -= dt;
    if (_respawn <= 0)
    {
        _respawn = 2.0f;
        _nearSea = 1e9f;
        for (float r : {0.0f, 80.0f, 200.0f, 400.0f, 600.0f})
        {
            for (int k = 0; k < 16 && _nearSea > 1e8f; ++k)
            {
                const float a = Tau * k / 16;
                const float px = camera.X() + std::cos(a) * r, pz = camera.Z() + std::sin(a) * r;
                if (Wildlife::Ground(px, pz) < Wildlife::SeaLevel(px, pz) - 0.5f)
                    _nearSea = r;
            }
            if (_nearSea < 1e8f)
                break;
        }
        // retire the far ones (a line goes together), then fill up near the sea
        _birds.erase(std::remove_if(_birds.begin(), _birds.end(),
                                    [&](const Flyer& f)
                                    {
                                        const float dx = f.x - camera.X(), dz = f.z - camera.Z();
                                        return dx * dx + dz * dz > SimRange * SimRange;
                                    }),
                     _birds.end());
        // a line whose leader went is re-formed under the next one
        for (size_t i = 0; i < _birds.size(); ++i)
            if (_birds[i].mode == 2)
            {
                int lead = -1;
                for (size_t j = 0; j < _birds.size(); ++j)
                    if (_birds[j].mode == 2 && _birds[j].leader < 0)
                        lead = int(j);
                if (lead < 0)
                    _birds[i].leader = -1, lead = int(i);
                if (int(i) != lead)
                    _birds[i].leader = lead;
            }
        if (_nearSea < 700.0f && camera.Y() < sea + 1200.0f)
            Spawn(camera);
    }
    if (_birds.empty())
        return;
    EnsureMeshes();

    const Vector3 wind = GLandscape->GetWind();
    for (size_t i = 0; i < _birds.size(); ++i)
    {
        Flyer& f = _birds[i];
        const Cfg& c = FlightTable[f.species];
        float tx = f.x, ty = f.y, tz = f.z, speed = c.speed, power = 1, turn = 1;
        if (f.mode == 0 || f.mode == 3)
        {
            // circling (gulls on updrafts, frigatebirds high on motionless wings); the centre drifts
            f.t += dt * f.dir * (c.speed / f.radius);
            tx = f.cx + std::cos(f.t + f.dir * 0.6f) * f.radius;
            tz = f.cz + std::sin(f.t + f.dir * 0.6f) * f.radius;
            f.cx += std::cos(f.wander * 0.07f) * dt * 0.8f;
            f.cz += std::sin(f.wander * 0.05f) * dt * 0.8f;
            ty = (f.mode == 3 ? sea : Floor(tx, tz)) + f.cy;
            power = f.mode == 3 ? 0.3f : 1.0f;
            turn = 1.4f;
        }
        else if (f.mode == 1)
        {
            // terns: back and forth along the lane
            tx = f.lx1, tz = f.lz1, ty = sea + f.cy;
            if (std::hypot(f.x - f.lx1, f.z - f.lz1) < 15)
            {
                std::swap(f.lx0, f.lx1);
                std::swap(f.lz0, f.lz1);
            }
        }
        else if (f.leader >= 0 && f.leader < int(_birds.size()))
        {
            // pelicans in line: each behind the one in front, low over the swell
            const Flyer& L = _birds[f.leader];
            const float back = f.slot * 3.4f;
            tx = L.x - std::sin(L.yaw) * back + std::cos(L.yaw) * 0.6f * (f.slot % 2 ? 1 : -1);
            tz = L.z - std::cos(L.yaw) * back - std::sin(L.yaw) * 0.6f * (f.slot % 2 ? 1 : -1);
            tx += std::sin(L.yaw) * 12, tz += std::cos(L.yaw) * 12; // aim ahead of the slot
            ty = sea + 1.6f;
            speed = L.speed + Clamp((std::hypot(tx - f.x, tz - f.z) - 12) * 0.3f, -2, 2);
            turn = 1.6f;
        }
        else
        {
            // the line's leader: a wide loop over the sea, flap bouts now and then
            f.t += dt * (c.speed / f.radius);
            tx = f.cx + std::cos(f.t + 0.6f) * f.radius;
            tz = f.cz + std::sin(f.t + 0.6f) * f.radius;
            ty = sea + 1.6f;
        }
        // keep clear of the land under it and ahead
        const float aheadX = f.x + std::sin(f.yaw) * 30, aheadZ = f.z + std::cos(f.yaw) * 30;
        const float ground = std::max(Wildlife::Ground(f.x, f.z), Wildlife::Ground(aheadX, aheadZ));
        if (ground > sea)
            ty = std::max(ty, ground + (f.mode == 2 ? 12.0f : 6.0f));
        Steer(f, dt, tx, ty, tz, speed, power, turn, wind.X(), wind.Z());
        f.y = std::max(f.y, Floor(f.x, f.z) + 0.8f);
        Animate(f, dt);
    }

    // draw
    // WGR_BIRDS_SCALE: a debug magnifier for checking the shapes in a capture
    static const float debugScale = []
    {
        const char* v = std::getenv("WGR_BIRDS_SCALE");
        return v ? std::max(0.1f, float(std::atof(v))) : 1.0f;
    }();
    int drawn = 0;
    for (const Flyer& f : _birds)
    {
        const float dx = f.x - camera.X(), dy = f.y - camera.Y(), dz = f.z - camera.Z();
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        const Species& sp = SpeciesTable[f.species];
        if (dist > DrawRange || sp.length * f.scale * debugScale / std::max(dist, 0.1f) < 0.0012f)
            continue;
        // pose: a phase of the flap cycle, or the glide pose
        const int pose = f.flap < 0.2f ? Phases : std::clamp(int(f.phase / Tau * Phases), 0, Phases - 1);
        const size_t mi = size_t(f.species) * (Phases + 1) + pose;
        // body: along the flight path, banked (Kit.js qYawPitchRoll), a small heave with the beat
        const BQ q = QMul(QMul(QAxis(0, 1, 0, f.yaw), QAxis(1, 0, 0, -f.pitch)), QAxis(0, 0, 1, -f.bank));
        const float s = f.scale * debugScale;
        const BV3 ax = QRot(q, {s, 0, 0}), up = QRot(q, {0, s, 0}), fw = QRot(q, {0, 0, s});
        const float heave = -std::cos(f.phase) * 0.012f * sp.length * f.flap;
        const float world[16] = {ax.x, ax.y, ax.z, 0, up.x, up.y, up.z, 0, fw.x, fw.y, fw.z, 0, f.x, f.y + heave, f.z, 1};
        _engine.SubmitDynamicMesh(_meshes[mi], _indexCounts[mi], world, _textures[f.species], 2);
        ++drawn;
    }
    static float nextLog = 0;
    if (time >= nextLog)
    {
        nextLog = time + 5.0f;
        LOG_INFO(Graphics, "Birds: {} flying, {} drawn, sea {:.0f} m from the camera; camera {:.0f} {:.0f} {:.0f}, first {:.0f} {:.0f} {:.0f}",
                 _birds.size(), drawn, _nearSea > 1e8f ? -1.0f : _nearSea, camera.X(), camera.Y(), camera.Z(), _birds[0].x,
                 _birds[0].y, _birds[0].z);
    }
}
} // namespace Poseidon
