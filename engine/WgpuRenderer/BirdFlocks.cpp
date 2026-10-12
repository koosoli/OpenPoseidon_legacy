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
//                  pelicans skimming the swell in a line, frigatebirds soaring high;
//   Shorebirds.js  (2026-10-12, from the island-expansion branch) sanderlings working the swash:
//                  small flocks chasing the backwash down the wet sand, probing as they go,
//                  scurrying up the beach ahead of every uprush, running from someone walking up
//                  and flushing when pressed. BirdShapes.js's folded wings and two-bone legs and
//                  BirdPose.js's groundPose / solveLeg / setHead come with them. The edge of the
//                  water is the shore waves' own run-up (water_tw/swash.rs, read back like
//                  Tidewater's SwashProbe); the beaches are wherever those waves run.
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
    // BirdShapes.js pivot (the neck joint the head turns about), fold (front, tip, top, h, minX:
    // the wing laid along the flank) and legs (hip x y z, tibia, tarsus, radius, toe, web); only
    // the sanderling stands, so only it carries them
    float pivot[3] = {};
    float fold[5] = {};
    float legs[8] = {};
};
enum
{
    Gull,
    Tern,
    Pelican,
    Frigate,
    Sanderling,
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
    {"sanderling", 0.2f, 0.38f,
     {{-0.066f, 0.007f, 0.011f, 0.007f, 0.007f}, {-0.046f, 0.004f, 0.021f, 0.017f, 0.019f}, {-0.016f, 0, 0.029f, 0.025f, 0.029f},
      {0.014f, 0, 0.03f, 0.026f, 0.029f}, {0.038f, 0.004f, 0.025f, 0.023f, 0.024f}, {0.056f, 0.012f, 0.017f, 0.018f, 0.016f},
      {0.069f, 0.017f, 0.016f, 0.018f, 0.015f}, {0.082f, 0.019f, 0.015f, 0.017f, 0.013f}, {0.093f, 0.017f, 0.011f, 0.013f, 0.009f},
      {0.1f, 0.014f, 0.006f, 0.007f, 0.005f}},
     {0.043f, 0.062f},
     {0.096f, 0.0135f, 0.027f, -0.06f, 0.0026f, 0.0028f, 0.0024f, 0.0004f, 0, 0},
     {-0.062f, 0.008f, 0.01f, {0.05f, 0.048f, 0.045f, 0.041f}, 0.3f},
     {{0.016f, 0.012f, 0.012f}, 0.174f, 0.22f, 0.46f, {0.15f, 0.08f, 0.035f},
      {{0, 0.016f}, {0.22f, 0.019f}, {0.46f, 0.017f}, {0.7f, 0.001f}, {0.88f, -0.016f}, {1, -0.038f}},
      {{0, 0.06f}, {0.22f, 0.058f}, {0.46f, 0.052f}, {0.7f, 0.04f}, {0.88f, 0.025f}, {1, 0.005f}}},
     {0.081f, 0.025f, 0.0024f},
     {0, 0.008f, 0.05f},
     {0.035f, -0.1f, 0.022f, 0.03f, 0.005f},
     {0.011f, -0.021f, 0.004f, 0.02f, 0.024f, 0.0019f, 0.017f, 0}},
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
    {13, 6, 1.0f, 4, 2.5f, 1.2f, 11, 0.8f, 0.08f, -0.05f, 0.02f, 0.3f, 0.55f, -0.15f, 0.8f},
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
    // (Shorebirds) wings: the folded position; legs (part 8): side, segment (0 hip, 1 knee, 2 ankle
    // ring, 3 foot), and the ring direction / radius factor -- or, on the foot, its offset in f
    BV3 f{};
    float legSide = 0, legSeg = 0, rx = 0, ry = 0, rk = 0;
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
Rest BuildRest(const Species& sp, bool withLegs)
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
    // BirdShapes.js flankX: half width of the body at (y, z), 0 outside it
    auto flankX = [&](float y, float z)
    {
        if (z < z0 || z > z1)
            return 0.0f;
        const float yc = Prof(Y, z), hw = Prof(HW, z), hh = y > yc ? Prof(HT, z) : Prof(HB, z);
        const float d = (y - yc) / std::max(hh, 1e-5f);
        return std::fabs(d) >= 1 ? 0.0f : hw * std::sqrt(1 - d * d);
    };
    const float* Fd = sp.fold;
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
                const uint32_t vi = vtx({side * x, y, Wg.root[2] + le - v * c}, pt, w, u, av);
                // folded: laid back along the upper flank, primaries bundled over the tail
                const float a = std::pow(u, 0.85f);
                const float zfA = Fd[0] + (Fd[1] - Fd[0]) * a;
                const float hA = Fd[3] * std::pow(1 - a, 0.55f) * (1 - 0.25f * a) + Fd[3] * 0.08f;
                const float yTop = Prof(Y, std::min(std::max(zfA, z0), z1)) + Fd[2] * (1 - 0.55f * a) - 0.004f * a;
                const float yf = yTop - v * hA, zf = zfA - v * hA * 0.35f;
                const float layer = s >= 0 ? 0.0035f : -0.001f;
                const float xf = std::max(flankX(yf, zf) * 1.06f, Fd[4] * (1 - 0.3f * v)) + layer * (sp.length / 0.4f) +
                                 0.0015f * (1 - u);
                r.v[vi].f = {side * xf, yf, zf};
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
    // legs (BirdShapes.js): a 4-sided tube hip -> knee -> ankle and a slender foot at the ankle; the
    // posing puts them where the solved knee and foot are. Coloured from the bill's atlas texels.
    if (withLegs && sp.legs[3] > 0)
    {
        for (int sideI = 0; sideI < 2; ++sideI)
        {
            const float side = sideI == 0 ? 1.0f : -1.0f;
            const uint32_t first = uint32_t(r.v.size());
            for (int rr = 0; rr < 3; ++rr)
                for (int j = 0; j < 4; ++j)
                {
                    const float a = float(j) / 4 * Tau + Pi / 4;
                    const uint32_t i = vtx({}, 8, 0, 0.6f, 0.25f);
                    RV& q = r.v[i];
                    q.legSide = side, q.legSeg = float(rr);
                    q.rx = std::cos(a), q.ry = std::sin(a), q.rk = rr == 0 ? 1.25f : rr == 1 ? 1.1f : 0.9f;
                }
            auto PL = [&](int rr, int j) { return first + uint32_t(rr * 4 + (j % 4)); };
            for (int rr = 0; rr < 2; ++rr)
                for (int j = 0; j < 4; ++j)
                    r.idx.insert(r.idx.end(), {PL(rr, j), PL(rr, j + 1), PL(rr + 1, j), PL(rr, j + 1), PL(rr + 1, j + 1), PL(rr + 1, j)});
            const float toe = sp.legs[6], web = sp.legs[7];
            const float toes[3] = {-0.5f, 0, 0.5f};
            std::vector<BV3> footPts{{0, 0, -toe * 0.12f}};
            for (int k = 0; k < 3; ++k)
            {
                const float a = toes[k] * side;
                footPts.push_back({std::sin(a) * toe, 0, std::cos(a) * toe * (k == 1 ? 1.08f : 0.92f)});
                if (k < 2)
                {
                    const float b = (toes[k] + 0.25f) * side, wr = toe * (0.22f + 0.55f * web);
                    footPts.push_back({std::sin(b) * wr, 0, std::cos(b) * wr});
                }
            }
            const uint32_t fc = uint32_t(r.v.size());
            for (const BV3& f : footPts)
            {
                const uint32_t i = vtx({}, 8, 0, 0.6f, 0.25f);
                r.v[i].legSide = side, r.v[i].legSeg = 3, r.v[i].f = f;
            }
            for (uint32_t k = 1; k + 1 < uint32_t(footPts.size()); ++k)
            {
                if (side > 0)
                    r.idx.insert(r.idx.end(), {fc, fc + k + 1, fc + k});
                else
                    r.idx.insert(r.idx.end(), {fc, fc + k, fc + k + 1});
            }
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

// (Shorebirds) the rest of a pose: wings morphed toward their folded shape, the legs' knees and
// feet (body frame; left = +x), the head bone (about the species' neck pivot) and the tail's pitch
struct BodyPose
{
    float fold = 0;
    BV3 knee[2]{}, foot[2]{}; // [0] left (+x), [1] right; knee = foot = hip collapses the legs
    BQ head{};
    BV3 headOff{};
    float tailPitch = 0;
};

// BirdPose.js solveLeg: two-bone leg hip -> knee -> foot (body frame), the knee bending backward
void SolveLeg(BV3& knee, BV3& foot, BV3 hip, BV3 f, float a, float b)
{
    BV3 d = f - hip;
    float dl = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    const float maxD = (a + b) * 0.999f;
    if (dl > maxD)
    {
        d = d * (maxD / dl);
        dl = maxD;
        f = hip + d;
    }
    foot = f;
    const float x = (a * a - b * b + dl * dl) / (2 * std::max(dl, 1e-6f));
    const float h = std::sqrt(std::max(0.0f, a * a - x * x));
    const BV3 u = d * (1 / std::max(dl, 1e-6f));
    BV3 bd{0, 0, -1};
    const float dp = bd.x * u.x + bd.y * u.y + bd.z * u.z;
    bd = bd - u * dp;
    const float bl = std::sqrt(bd.x * bd.x + bd.y * bd.y + bd.z * bd.z);
    knee = hip + u * x + bd * (h / (bl > 0 ? bl : 1));
}

// BirdPose.js standHeight: the body origin above the feet, legs slightly bent
float StandHeight(const Species& sp, float bend = 0.9f) { return -sp.legs[1] + (sp.legs[3] + sp.legs[4]) * bend * 0.93f; }

// BirdPose.js groundPose (the legs only; the body's place and yaw / pitch go into the draw's matrix):
// feet on the ground plane `height` below the body origin, the stride's phase (the feet alternate),
// stride length, step height and how far apart the feet are
void GroundLegs(const Species& sp, BodyPose& bp, float pitch, float height, float phase, float stride, float lift, float spread = 1)
{
    const float c = std::cos(pitch), sn = std::sin(pitch);
    const float fx = sp.legs[0] * 0.9f * spread;
    const float base = sp.legs[2] * c + 0.25f * sp.legs[4] * sn;
    for (int k = 0; k < 2; ++k)
    {
        const float ph = phase + k * Pi;
        const float fz = base + std::sin(ph) * stride * 0.5f;
        const float fy = -height + std::max(0.0f, std::cos(ph)) * lift;
        const float lx = k == 0 ? fx : -fx;
        const float by = fy * c - fz * sn, bz = fy * sn + fz * c;
        const BV3 hip{k == 0 ? sp.legs[0] : -sp.legs[0], sp.legs[1], sp.legs[2]};
        SolveLeg(bp.knee[k], bp.foot[k], hip, {lx, by, bz}, sp.legs[3], sp.legs[4]);
    }
}

// The sanderling's ground poses: GroundRunPhases running (the stride's phases), then standing, then
// probing (the bill jabbed into the sand). Shorebirds.js groundPoseOf: body pitched a little down while
// running, up while standing; head lowered and pushed forward to probe; wings folded; tail pitched.
constexpr int GroundRunPhases = 8;
constexpr int GroundStand = GroundRunPhases, GroundProbe = GroundRunPhases + 1, GroundPoses = GroundRunPhases + 2;
float GroundPitch(int pose) { return pose < GroundRunPhases ? -0.05f : pose == GroundProbe ? -0.04f : 0.04f; }
BodyPose SanderlingGround(int pose)
{
    const Species& sp = SpeciesTable[Sanderling];
    BodyPose bp;
    bp.fold = 1;
    bp.tailPitch = 0.05f;
    const float head = pose == GroundProbe ? 1.0f : 0.0f;
    bp.head = QAxis(1, 0, 0, -0.1f + head * 1.15f);
    bp.headOff = {0, -0.006f * head, 0.004f * head};
    const bool run = pose < GroundRunPhases;
    GroundLegs(sp, bp, GroundPitch(pose), StandHeight(sp), run ? Tau * pose / GroundRunPhases : 0, run ? 0.06f : 0, run ? 0.012f : 0);
    return bp;
}

struct MeshOut
{
    std::vector<WgrMeshVertex> v;
    std::vector<uint32_t> idx;
};

// BirdBatch.js vertex stage, baked: the wings posed by their bones (mirrored for the right wing,
// blended at the joints), the tail spread about the centre line
MeshOut Pose(const Rest& r, const WingPose& wp, float tailSpread, const Species* spB = nullptr, const BodyPose* bp = nullptr)
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
            const float tp = bp ? bp->tailPitch : 0.0f, c = std::cos(tp), sn = std::sin(tp);
            const BV3 ds{d.x * tailSpread, d.y, d.z};
            p = r.tailBase + BV3{ds.x, ds.y * c + ds.z * sn, ds.z * c - ds.y * sn};
        }
        else if (a.part <= 3 && bp && spB)
        {
            // the head bone turns about the neck pivot: the body stays, the neck blends, the head
            // and the bill turn whole
            const BV3 piv{spB->pivot[0], spB->pivot[1], spB->pivot[2]};
            const BV3 hp = piv + bp->headOff + QRot(bp->head, a.p - piv);
            const float w = a.part == 3 ? 0.0f : 1.0f - Smooth01(spB->neck[0], spB->neck[1], a.p.z);
            p = Mix(hp, a.p, w);
        }
        else if (a.part == 8)
        {
            // legs: tube hip -> knee -> ankle, the foot at the ankle (collapsed when tucked)
            const int L = a.legSide > 0 ? 0 : 1;
            const BV3 hip{spB ? spB->legs[0] * a.legSide : 0, spB ? spB->legs[1] : 0, spB ? spB->legs[2] : 0};
            const BV3 knee = bp ? bp->knee[L] : hip, foot = bp ? bp->foot[L] : hip;
            const auto len = [](BV3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); };
            const float reach = Smooth01(0, 0.01f, len(knee - hip) + len(foot - knee));
            if (a.legSeg > 2.5f)
                p = foot + a.f * Smooth01(0, 0.01f, len(foot - knee));
            else
            {
                const BV3 c = a.legSeg < 0.5f ? hip : a.legSeg < 1.5f ? knee : foot;
                const BV3 d0 = a.legSeg < 0.5f ? knee - hip : a.legSeg < 1.5f ? foot - hip : foot - knee;
                const float dl = std::max(len(d0), 1e-5f);
                const BV3 dir = d0 * (1 / dl);
                auto cross = [](BV3 u, BV3 v) { return BV3{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x}; };
                BV3 ax = cross(dir, {1, 0, 0.001f});
                ax = ax * (1 / std::max(len(ax), 1e-6f));
                const BV3 ay = cross(dir, ax);
                const float rad = (spB ? spB->legs[5] : 0) * a.rk * reach;
                p = c + (ax * a.rx + ay * a.ry) * rad;
            }
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
            if (bp && bp->fold > 0)
                p = Mix(p, a.f, bp->fold);
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
    else if (s == Sanderling)
    {
        // sanderling (winter): pale grey above with dark feather centres, white below, dark
        // shoulder and primaries, bold white wing bar, black bill and legs
        const Rgb white{0.95f, 0.95f, 0.94f};
        const Rgb grey = MixC(Rgb{0.62f, 0.62f, 0.6f}, Rgb{0.48f, 0.48f, 0.47f}, Smooth01(0.2f, 0.7f, fnz));
        const float upper = Smooth01(-0.05f, 0.35f, N.y) * std::max(1 - Smooth01(0.08f, 0.092f, P.z), Smooth01(0.2f, 0.6f, N.y));
        const Rgb body = MixC(white, grey, upper);
        const float bar = Smooth01(0.18f, 0.26f, u) * (1 - Smooth01(0.7f, 0.78f, u)) * Smooth01(0.42f, 0.48f, v) * (1 - Smooth01(0.6f, 0.66f, v));
        const float shoulder = Smooth01(0.2f, 0.3f, u) * (1 - Smooth01(0.45f, 0.55f, u)) * (1 - Smooth01(0.1f, 0.3f, v));
        const Rgb wingTop = MixC(MixC(MixC(grey, Rgb{0.12f, 0.12f, 0.12f}, Smooth01(0.62f, 0.72f, u)), white, bar), Rgb{0.1f, 0.1f, 0.1f},
                                 shoulder * 0.8f);
        c = isWing ? (top ? wingTop : white)
                   : part == TailP ? MixC(grey, Rgb{0.2f, 0.2f, 0.2f}, 1 - Smooth01(0.0f, 0.25f, std::fabs(u - 0.5f))) : body;
        if (part == BillP)
            c = {0.04f, 0.04f, 0.04f};
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
        const Rest rest = BuildRest(SpeciesTable[s], s == Sanderling);
        const Cfg& g = FlightTable[s];
        for (int p = 0; p <= Phases; ++p)
        {
            const WingPose wp = p < Phases ? FlapWings(g, Tau * p / Phases, g.amp) : FlapWings(g, Pi / 2, 0);
            const MeshOut m = Pose(rest, wp, s == Frigate ? 1.6f : 1.15f);
            const size_t i = size_t(s) * (Phases + 1) + p;
            _meshes[i] = wgr_mesh_create(_renderer, WgrSlice<WgrMeshVertex>(m.v), WgrSlice<uint32_t>(m.idx));
            _indexCounts[i] = uint32_t(m.idx.size());
        }
        if (s == Sanderling)
        {
            // folded and standing on its legs: the running phases, standing, probing
            for (int g = 0; g < GroundPoses; ++g)
            {
                const BodyPose bp = SanderlingGround(g);
                const MeshOut m = Pose(rest, FlapWings(FlightTable[s], Pi / 2, 0), 1.0f, &SpeciesTable[s], &bp);
                _groundMeshes.push_back(wgr_mesh_create(_renderer, WgrSlice<WgrMeshVertex>(m.v), WgrSlice<uint32_t>(m.idx)));
                _groundCounts.push_back(uint32_t(m.idx.size()));
            }
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
    while (count(Gull) < Wildlife::Scaled(6) && pick(r0, r1, false, x, z))
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
    while (count(Tern) < Wildlife::Scaled(3) && pick(r0, r1, true, x, z))
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
    // one line of five per unit of density
    if (count(Pelican) < 5 * (Wildlife::Scaled(1)) && pick(first ? 40.0f : r0, r1, true, x, z))
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
    while (count(Frigate) < Wildlife::Scaled(2) && pick(r0, r1 + 100, false, x, z))
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
    // (Shorebirds.js) the sanderlings on the beaches near the camera
    static const bool shorebirds = []
    {
        const char* v = std::getenv("WGR_SHOREBIRDS");
        return !(v && v[0] == '0');
    }();
    int sandDrawn = 0;
    if (shorebirds)
    {
        UpdateShorebirds(camera, dt, time);
        if (!_sand.empty())
        {
            EnsureMeshes();
            DrawSand(camera, sandDrawn);
        }
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
        LOG_INFO(Graphics, "Birds: {} flying, {} drawn, sea {:.0f} m from the camera; camera {:.0f} {:.0f} {:.0f}, first {:.0f} {:.0f} {:.0f}; "
                           "sanderlings {} in {} flocks ({} drawn, swash field {} texels, first at {:.1f} {:.2f} {:.1f})",
                 _birds.size(), drawn, _nearSea > 1e8f ? -1.0f : _nearSea, camera.X(), camera.Y(), camera.Z(), _birds[0].x,
                 _birds[0].y, _birds[0].z, _sand.size(), _flocks.size(), sandDrawn, _swashTexels,
                 _sand.empty() ? 0.0f : _sand[0].x, _sand.empty() ? 0.0f : _sand[0].y, _sand.empty() ? 0.0f : _sand[0].z);
    }
}
// ---------------------------------------------------------------- sanderlings (Shorebirds.js)

namespace
{
constexpr float SandDraw = 190.0f;            // Shorebirds.js DRAW
constexpr float SandWary = 15.0f, SandFlush = 8.0f; // m
constexpr float BeachSlope = 0.066f;          // ShoreWaves' nominal beach slope (tw_shore.wgsl)
} // namespace

bool BirdFlocks::SwashAt(float x, float z, float& runup, float& speed) const
{
    if (_swashTexels <= 0 || _swashLanes[2] <= 0)
        return false;
    const int n = int(_swashLanes[3]);
    const int i = int(std::floor((x - _swashLanes[0]) / _swashLanes[2] + 0.5f));
    const int j = int(std::floor((z - _swashLanes[1]) / _swashLanes[2] + 0.5f));
    if (i < 0 || j < 0 || i >= n || j >= n || j * n + i >= _swashTexels)
        return false;
    const float* t = &_swash[size_t(j * n + i) * 4];
    if (t[3] < 0)
        return false;
    runup = t[0];
    speed = t[2];
    return true;
}

// A new flock where the shore waves run onto a gentle beach near the camera: a swash-field texel
// whose ground is a little above the sea, on a slope a sanderling can run on, away from other flocks.
void BirdFlocks::SpawnShoreFlock(const Vector3& camera)
{
    const int n = int(_swashLanes[3]);
    if (_swashTexels <= 0 || n <= 0)
        return;
    for (int tries = 0; tries < 40; ++tries)
    {
        const int i = int(Unit(_rng) * n), j = int(Unit(_rng) * n);
        const float* t = &_swash[size_t(j * n + i) * 4];
        if (t[3] < 0 || t[0] < 0.6f)
            continue;
        const float x = _swashLanes[0] + i * _swashLanes[2], z = _swashLanes[1] + j * _swashLanes[2];
        const float d = std::hypot(x - camera.X(), z - camera.Z());
        if (d < 25 || d > 150)
            continue;
        const float sea = Wildlife::SeaLevel(x, z), h = Wildlife::Ground(x, z);
        if (h < sea + 0.05f || h > sea + 1.2f)
            continue;
        const float e = 1.0f;
        const float gx = (Wildlife::Ground(x + e, z) - Wildlife::Ground(x - e, z)) / (2 * e);
        const float gz = (Wildlife::Ground(x, z + e) - Wildlife::Ground(x, z - e)) / (2 * e);
        const float g = std::hypot(gx, gz);
        if (g < 0.01f || g > 0.2f)
            continue;
        bool clear = true;
        for (const ShoreFlock& o : _flocks)
            clear &= std::hypot(o.cx - x, o.cz - z) > 90;
        if (!clear)
            continue;
        ShoreFlock F;
        F.cx = x, F.cz = z;
        F.tx = -gz / g, F.tz = gx / g; // along the shore: across the uphill direction
        F.drift = Unit(_rng) < 0.5f ? -1.0f : 1.0f;
        F.range = 40 + Unit(_rng) * 25;
        const int fi = int(_flocks.size());
        _flocks.push_back(F);
        const int birds = 7 + int(Unit(_rng) * 6 * std::min(Wildlife::Density(), 2.0f));
        for (int k = 0; k < birds; ++k)
        {
            Sand b;
            b.flock = fi;
            b.offset = (Unit(_rng) - 0.5f) * 11;
            b.margin = Unit(_rng);
            b.phase = Unit(_rng) * Tau;
            b.peckT = Unit(_rng);
            const float along = b.offset + (Unit(_rng) - 0.5f) * 4;
            b.x = x + F.tx * along + gx / g * (Unit(_rng) * 2 - 1);
            b.z = z + F.tz * along + gz / g * (Unit(_rng) * 2 - 1);
            b.y = Wildlife::Ground(b.x, b.z);
            b.yaw = Unit(_rng) * Tau;
            for (float& v : b.form)
                v = Unit(_rng) - 0.5f;
            b.form[0] *= 4, b.form[1] *= 1.2f, b.form[2] *= 3;
            b.f.species = Sanderling;
            b.f.freqMul = 0.94f + Unit(_rng) * 0.12f;
            b.f.scale = 0.92f + Unit(_rng) * 0.16f;
            b.f.wander = Unit(_rng) * 100;
            _sand.push_back(b);
        }
        LOG_INFO(Graphics, "Sanderlings: a flock of {} at {:.0f} {:.0f} ({:.0f} m from the camera)", birds, x, z, d);
        return;
    }
}

void BirdFlocks::UpdateShorebirds(const Vector3& camera, float dt, float time)
{
    // the swash field (1-3 frames old on the GPU side; re-read every few frames here)
    _shoreScan -= dt;
    static float swashRead = 0;
    swashRead -= dt;
    if (swashRead <= 0)
    {
        swashRead = 0.1f;
        const uint32_t cap = 64 * 64;
        _swash.resize(size_t(cap) * 4);
        float age = 0;
        const uint32_t got = wgr_water_swash_field(_swash.data(), cap, _swashLanes, &age);
        _swashTexels = got > 0 && age < 1500.0f ? int(got) : 0;
    }
    // retire flocks the camera left behind; found new ones now and then
    if (_shoreScan <= 0)
    {
        _shoreScan = 3.0f;
        for (int fi = int(_flocks.size()) - 1; fi >= 0; --fi)
        {
            if (std::hypot(_flocks[fi].cx - camera.X(), _flocks[fi].cz - camera.Z()) < 320)
                continue;
            _sand.erase(std::remove_if(_sand.begin(), _sand.end(), [&](const Sand& b) { return b.flock == fi; }), _sand.end());
            for (Sand& b : _sand)
                if (b.flock > fi)
                    --b.flock;
            _flocks.erase(_flocks.begin() + fi);
        }
        const float sea = Wildlife::SeaLevel(camera.X(), camera.Z());
        if (_flocks.size() < 2 && camera.Y() < sea + 120 && !Wildlife::Earth())
            SpawnShoreFlock(camera);
    }
    if (_flocks.empty())
        return;
    // the viewer (the camera near the ground): wary birds run off along the beach, pressed ones flush
    static Vector3 lastCam = camera;
    const float camSpeed = dt > 0 ? (camera - lastCam).Size() / dt : 0;
    lastCam = camera;
    const bool viewerLow = camera.Y() < Wildlife::Ground(camera.X(), camera.Z()) + 3.5f;
    for (int fi = 0; fi < int(_flocks.size()); ++fi)
    {
        ShoreFlock& F = _flocks[fi];
        float threat = 1e9f;
        float cs = 0;
        int nb = 0;
        for (const Sand& b : _sand)
        {
            if (b.flock != fi)
                continue;
            cs += (b.x - F.cx) * F.tx + (b.z - F.cz) * F.tz;
            ++nb;
            if (!b.flying && viewerLow)
                threat = std::min(threat, std::hypot(b.x - camera.X(), b.z - camera.Z()));
        }
        if (nb == 0)
            continue;
        cs /= nb;
        const float viewerS = (camera.X() - F.cx) * F.tx + (camera.Z() - F.cz) * F.tz;
        F.away = cs >= viewerS ? 1.0f : -1.0f;
        const bool running = camSpeed > 3.5f;
        const bool flush = threat < SandFlush || (running && threat < SandFlush * 1.7f);
        if (!F.flying && flush)
        {
            F.alarm += dt;
            if (F.alarm > 0.15f)
                FlushFlock(fi, camera);
        }
        else
            F.alarm = std::max(0.0f, F.alarm - dt);
        F.wary = threat < SandWary + (running ? 6 : 0);
        if (!F.flying)
        {
            if (Unit(_rng) < dt / 25)
                F.drift = -F.drift;
            F.s += F.drift * 0.25f * dt + (F.wary ? F.away * 1.4f * dt : 0);
            F.s = Clamp(F.s, -F.range + 8, F.range - 8);
        }
        else if (!F.path.empty())
        {
            // the flock point runs along the flush path at 12.5 m/s
            F.pathS = std::min(F.pathLen, F.pathS + dt * 12.5f);
            float acc = 0;
            for (size_t k = 1; k < F.path.size(); ++k)
            {
                const auto& a = F.path[k - 1];
                const auto& b2 = F.path[k];
                const float l = std::hypot(b2[0] - a[0], b2[2] - a[2]);
                if (acc + l >= F.pathS || k + 1 == F.path.size())
                {
                    const float t = l > 0 ? Clamp((F.pathS - acc) / l, 0, 1) : 1;
                    F.px = a[0] + (b2[0] - a[0]) * t, F.py = a[1] + (b2[1] - a[1]) * t, F.pz = a[2] + (b2[2] - a[2]) * t;
                    F.hx = (b2[0] - a[0]) / std::max(l, 1e-3f), F.hz = (b2[2] - a[2]) / std::max(l, 1e-3f);
                    break;
                }
                acc += l;
            }
        }
        bool anyFlying = false;
        for (Sand& b : _sand)
        {
            if (b.flock != fi)
                continue;
            if (b.flying)
                FlySand(b, F, dt);
            else
                GroundSand(b, F, dt);
            anyFlying |= b.flying;
        }
        if (F.flying && !anyFlying)
            F.flying = false;
    }
    (void)time;
}

void BirdFlocks::GroundSand(Sand& b, ShoreFlock& F, float dt)
{
    const float e = 0.6f;
    const float gx = (Wildlife::Ground(b.x + e, b.z) - Wildlife::Ground(b.x - e, b.z)) / (2 * e);
    const float gz = (Wildlife::Ground(b.x, b.z + e) - Wildlife::Ground(b.x, b.z - e)) / (2 * e);
    const float gl = std::hypot(gx, gz);
    const float ux = gl > 1e-4f ? gx / gl : -F.tz, uz = gl > 1e-4f ? gz / gl : F.tx; // uphill
    const float g = std::max(gl, 0.015f);
    const float tx = -uz, tz = ux; // along the shore here
    const float sea = Wildlife::SeaLevel(b.x, b.z);
    const float h = Wildlife::Ground(b.x, b.z);
    // where the water's edge is relative to this bird (m along the ground, > 0: the bird is dry)
    float dm = 3, vf = 0;
    bool up = false;
    float runup = 0, speed = 0;
    if (SwashAt(b.x, b.z, runup, speed))
    {
        const float inland = std::max(h - sea, 0.0f) / BeachSlope;
        dm = (inland - runup) * BeachSlope / g;
        vf = speed * BeachSlope / g;
        up = speed > 0.05f;
    }
    else
        dm = (h - sea - 0.35f) / g; // no field: keep to a fixed band of the wet sand
    // wanted place relative to the edge: just above the retreating water, well ahead of an uprush
    const float want = up ? 1.0f + b.margin * 1.2f : 0.15f + b.margin * 0.55f;
    float vUp = Clamp((up ? vf : 0) + (want - dm) * 2.2f, -1.3f, 2.4f);
    // along the shore: keep the flock together, spaced out, running off when wary
    const float targetX = F.cx + F.tx * (F.s + b.offset), targetZ = F.cz + F.tz * (F.s + b.offset);
    float vAl = Clamp(((targetX - b.x) * tx + (targetZ - b.z) * tz) * 0.6f, -0.9f, 0.9f);
    if (F.wary)
        vAl = F.away * (F.tx * tx + F.tz * tz >= 0 ? 1.0f : -1.0f) * (1.3f + b.margin * 0.5f);
    for (const Sand& o : _sand)
    {
        if (&o == &b || o.flying || o.flock != b.flock)
            continue;
        const float dx = b.x - o.x, dz = b.z - o.z, d2 = dx * dx + dz * dz;
        if (d2 < 0.25f && d2 > 1e-6f)
        {
            const float d = std::sqrt(d2);
            vAl += (dx * tx + dz * tz) / d * (0.5f - d) * 3;
            vUp += (dx * ux + dz * uz) / d * (0.5f - d) * 3;
        }
    }
    // stop-and-go: quick spurts, then a pause to probe the sand
    const float err = want - dm;
    const bool caught = dm < 0.25f && up;
    b.stop = std::max(0.0f, b.stop - dt);
    if (b.stop > 0)
    {
        if (caught || F.wary || std::fabs(err) > 0.8f + b.margin * 0.5f || std::fabs(vAl) > 0.7f)
            b.stop = 0;
    }
    else if (std::fabs(err) < 0.3f && std::fabs(vAl) < 0.55f && !caught && !F.wary)
        b.stop = 0.5f + Unit(_rng) * 1.6f;
    const float go = b.stop > 0 ? 0.0f : 1.0f;
    const float vx = (ux * vUp + tx * vAl) * go, vz = (uz * vUp + tz * vAl) * go;
    const float k = Approach(14, dt);
    b.vx += (vx - b.vx) * k;
    b.vz += (vz - b.vz) * k;
    b.v = std::hypot(b.vx, b.vz);
    b.x += b.vx * dt;
    b.z += b.vz * dt;
    if (b.v > 0.15f)
        b.yaw += AngleDiff(std::atan2(b.vx, b.vz), b.yaw) * Approach(16, dt);
    // caught by the water: a flutter hop up the beach
    if (caught && dm < -0.35f && b.hop <= 0)
        b.hop = 0.45f;
    b.hop = std::max(0.0f, b.hop - dt);
    // probing: quick jabs of the bill while stopped
    b.peckT += dt * (b.stop > 0 ? 4.5f : 0.0f);
    const float jab = b.stop > 0 ? std::pow(std::max(0.0f, std::sin(b.peckT * Pi)), 0.6f) : 0.0f;
    b.head += (jab - b.head) * Approach(30, dt);
    // legs: a blur of tiny, very fast steps
    const float f = b.v > 0.05f ? Clamp(5 + b.v * 5, 5, 14) : 0.0f;
    b.phase = std::fmod(b.phase + Tau * f * dt, Tau);
    b.y = Wildlife::Ground(b.x, b.z);
}

// Shorebirds.js flush: the whole flock bursts up, out over the surf, along the beach, back in to land
void BirdFlocks::FlushFlock(int fi, const Vector3& camera)
{
    ShoreFlock& F = _flocks[fi];
    F.flying = true;
    // where along the shore: away from the viewer, or back past him when there is no room
    float cs = 0;
    int nb = 0;
    for (const Sand& b : _sand)
        if (b.flock == fi)
            cs += (b.x - F.cx) * F.tx + (b.z - F.cz) * F.tz, ++nb;
    cs /= std::max(nb, 1);
    float dir = F.away;
    float dest = cs + dir * (55 + Unit(_rng) * 50);
    if (dest < -F.range + 10 || dest > F.range - 10)
    {
        dir = -dir;
        const float viewerS = (camera.X() - F.cx) * F.tx + (camera.Z() - F.cz) * F.tz;
        dest = Clamp(viewerS + dir * (50 + Unit(_rng) * 40), -F.range + 10, F.range - 10);
    }
    // seaward: down the beach at the flock's centre
    const float e = 1.0f;
    const float gx = (Wildlife::Ground(F.cx + e, F.cz) - Wildlife::Ground(F.cx - e, F.cz)) / (2 * e);
    const float gz = (Wildlife::Ground(F.cx, F.cz + e) - Wildlife::Ground(F.cx, F.cz - e)) / (2 * e);
    const float gl = std::max(std::hypot(gx, gz), 1e-4f);
    const float sx = -gx / gl, sz = -gz / gl;
    const float mid = (cs + dest) / 2;
    auto at = [&](float along, float hgt, float seaward)
    {
        const float x = F.cx + F.tx * along + sx * seaward, z = F.cz + F.tz * along + sz * seaward;
        return std::array<float, 3>{x, std::max(Wildlife::Ground(x, z), Wildlife::SeaLevel(x, z)) + hgt, z};
    };
    F.path = {at(cs, 0.6f, 0), at(cs + dir * 10, 2.2f, 7), at(mid - dir * 12, 3 + Unit(_rng) * 2, 14 + Unit(_rng) * 8),
              at(mid + dir * 14, 2.5f + Unit(_rng) * 2, 12 + Unit(_rng) * 8), at(dest - dir * 12, 1.4f, 4), at(dest, 0.4f, -1.5f)};
    F.pathLen = 0;
    for (size_t k = 1; k < F.path.size(); ++k)
        F.pathLen += std::hypot(F.path[k][0] - F.path[k - 1][0], F.path[k][2] - F.path[k - 1][2]);
    F.pathS = 0;
    F.s = dest;
    for (Sand& b : _sand)
    {
        if (b.flock != fi)
            continue;
        b.flying = true;
        b.t = -Unit(_rng) * 0.3f; // take-off spread over a moment
        const auto land = at(dest + b.offset * 0.8f, 0, -0.5f - b.margin * 2);
        b.lx = land[0], b.lz = land[2];
        Flyer& f = b.f;
        f.x = b.x, f.y = b.y + 0.06f, f.z = b.z;
        f.yaw = std::atan2(F.path[1][0] - b.x, F.path[1][2] - b.z);
        f.speed = 3, f.vy = 1.5f, f.flap = 0, f.bank = 0, f.pitch = 0.3f;
    }
    LOG_INFO(Graphics, "Sanderlings: a flock flushed ({} birds)", nb);
}

void BirdFlocks::FlySand(Sand& b, ShoreFlock& F, float dt)
{
    b.t += dt;
    if (b.t < 0)
        return; // not off yet: keep standing
    Flyer& f = b.f;
    const float hl = std::max(std::hypot(F.hx, F.hz), 1e-4f);
    const float hx = F.hx / hl, hz = F.hz / hl;
    float tx = F.px + hx * b.form[2] - hz * b.form[0], ty = F.py + b.form[1] * 0.6f, tz = F.pz + hz * b.form[2] + hx * b.form[0];
    const bool end = F.pathS >= F.pathLen - 0.01f;
    const float dl = std::hypot(b.lx - f.x, b.lz - f.z);
    if (end || F.pathS > F.pathLen - 14)
    {
        tx = b.lx, tz = b.lz;
        ty = Wildlife::Ground(b.lx, b.lz) + std::min(1.2f, dl * 0.25f);
    }
    const float ahead = std::hypot(tx - f.x, tz - f.z);
    const float speed = Clamp(9 + (ahead - 2) * 1.2f, 5, 16);
    const Vector3 wind = GLandscape ? GLandscape->GetWind() : Vector3(0, 0, 0);
    Steer(f, dt, tx, ty, tz, speed, b.t < 0.6f ? 2.0f : 1.0f, 2.2f, wind.X() * 0.7f, wind.Z() * 0.7f);
    const float ground = std::max(Wildlife::Ground(f.x, f.z), Wildlife::SeaLevel(f.x, f.z));
    f.y = std::max(f.y, ground + 0.25f);
    Animate(f, dt);
    if (dl < 0.6f && f.y < ground + 0.45f && F.pathS > F.pathLen - 14)
    {
        // touch down, running on a few steps
        b.flying = false;
        b.x = f.x, b.z = f.z, b.y = Wildlife::Ground(b.x, b.z);
        b.vx = std::sin(f.yaw) * f.speed * 0.3f, b.vz = std::cos(f.yaw) * f.speed * 0.3f;
        b.yaw = f.yaw;
        b.stop = 0;
    }
}

void BirdFlocks::DrawSand(const Vector3& camera, int& drawn)
{
    const Species& sp = SpeciesTable[Sanderling];
    const float H = StandHeight(sp);
    for (const Sand& b : _sand)
    {
        const bool air = b.flying && b.t >= 0;
        const float x = air ? b.f.x : b.x, y = air ? b.f.y : b.y, z = air ? b.f.z : b.z;
        const float dist = std::hypot(x - camera.X(), y - camera.Y(), z - camera.Z());
        if (dist > SandDraw || sp.length * b.f.scale / std::max(dist, 0.1f) < 0.0012f)
            continue;
        const float s = b.f.scale * Smooth01(SandDraw, SandDraw * 0.8f, dist);
        size_t mi;
        uint32_t count;
        WgrMesh mesh;
        BQ q;
        float px = x, py = y, pz = z;
        if (air || b.hop > 0)
        {
            // flying (or a flutter hop clear of the water): the flap cycle
            const Flyer& f = b.f;
            const float phase = air ? f.phase : std::fmod(b.hop * 60, Tau);
            const int pose = air && f.flap < 0.2f ? Phases : std::clamp(int(phase / Tau * Phases), 0, Phases - 1);
            mi = size_t(Sanderling) * (Phases + 1) + pose;
            mesh = _meshes[mi], count = _indexCounts[mi];
            q = air ? QMul(QMul(QAxis(0, 1, 0, f.yaw), QAxis(1, 0, 0, -f.pitch)), QAxis(0, 0, 1, -f.bank)) : QAxis(0, 1, 0, b.yaw);
            if (!air)
                py += H + std::sin(Pi * b.hop / 0.45f) * 0.12f;
        }
        else
        {
            const int pose = b.v > 0.05f ? std::clamp(int(b.phase / Tau * GroundRunPhases), 0, GroundRunPhases - 1)
                                         : (b.stop > 0 && b.head > 0.5f ? GroundProbe : GroundStand);
            if (size_t(pose) >= _groundMeshes.size())
                continue;
            mesh = _groundMeshes[pose], count = _groundCounts[pose];
            q = QMul(QAxis(0, 1, 0, b.yaw), QAxis(1, 0, 0, -GroundPitch(pose)));
            py += H;
        }
        const BV3 ax = QRot(q, {s, 0, 0}), upv = QRot(q, {0, s, 0}), fw = QRot(q, {0, 0, s});
        const float world[16] = {ax.x, ax.y, ax.z, 0, upv.x, upv.y, upv.z, 0, fw.x, fw.y, fw.z, 0, px, py, pz, 1};
        _engine.SubmitDynamicMesh(mesh, count, world, _textures[Sanderling], 2);
        ++drawn;
    }
}
} // namespace Poseidon
