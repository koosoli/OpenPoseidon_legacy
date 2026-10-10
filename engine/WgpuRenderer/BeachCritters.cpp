// SPDX-License-Identifier: GPL-3.0-or-later
//
// Beach crabs around the camera: stage 4 of porting Tidewater's wildlife (dgreenheck/tidewater,
// MIT, (c) DRG Software Solutions LLC; see CREDITS.md). From src/world/wildlife/:
//   CritterShapes.js  the shared template: the ghost crab's carapace, the hermit crab's turban
//                     shell and the burrow mound as body grids, the limb tables (8 walking legs,
//                     2 claws, 2 eyestalks per species);
//   CritterBatch.js   the limb joints for a gait phase (the tetrapod gait: legs 0, 2, 5, 7 swing
//                     together, the others half a cycle later), the limb tubes, the colours;
//   Crabs.js          the behaviour: ghost crabs in burrows on the dry upper beach peek out, emerge,
//                     forage in stop-and-go bursts, run sideways, and dash home when someone comes
//                     within ~6.5 m; hermit crabs trundle about at the back of the beach and pull
//                     into their shells when approached. Sizes and speeds from there.
//
// What OP changes: the gait is baked into 8 phases plus a standing pose (and the hermit's
// retracted pose) instead of posed in a vertex shader; colours are a small per-species atlas;
// burrows and homes are placed deterministically per 12 m cell on low, gentle ground just above
// the sea (OFP has no sand mask), within 36 m of the camera.

#include "BeachCritters.hpp"
#include "EngineWgpu.hpp"
#include "WildlifeWorld.hpp"
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

namespace Poseidon
{
namespace
{
constexpr float Tau = 6.2831853f, Pi = 3.14159265f;
constexpr int Phases = 8;           // gait; then standing, then (hermit) retracted
constexpr int PosesPerSpecies = Phases + 2;
constexpr float Active = 36.0f, Draw = 34.0f, CellSize = 12.0f;
constexpr int MaxGhost = 32, MaxHermit = 14;
constexpr int NU = 12, NV = 9, Limbs = 12;

struct CV
{
    float x = 0, y = 0, z = 0;
};
CV operator+(CV a, CV b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
CV operator-(CV a, CV b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
CV operator*(CV a, float s) { return {a.x * s, a.y * s, a.z * s}; }
CV Mix(CV a, CV b, float t) { return a + (b - a) * t; }
float Dot(CV a, CV b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
CV Cross(CV a, CV b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
CV Norm(CV a)
{
    const float l = std::sqrt(Dot(a, a));
    return l > 1e-9f ? a * (1.0f / l) : CV{0, 1, 0};
}
float Clamp(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
float Smooth(float a, float b, float x)
{
    const float t = Clamp((x - a) / (b - a), 0, 1);
    return t * t * (3 - 2 * t);
}
float Spow(float x, float e) { return (x < 0 ? -1.0f : 1.0f) * std::pow(std::fabs(x), e); }
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

// ---- CritterShapes.js body grids
CV Carapace(float u, float v)
{
    const float W = 0.5f, L = 0.43f, HT = 0.2f, HB = 0.1f;
    const float th = u * Tau, cu = std::cos(th), su = std::sin(th);
    const float phi = v * Pi, rh = std::pow(std::sin(phi), 0.55f), cy = std::cos(phi);
    float z = L * Spow(cu, 0.35f) * rh;
    const float x = W * Spow(su, 0.35f) * rh * (1 - 0.12f * std::max(0.0f, -cu));
    if (z > 0)
        z *= 1 - 0.05f * (1 - std::fabs(su));
    const float y = cy > 0 ? HT * Spow(cy, 0.6f) : HB * Spow(cy, 0.8f);
    return {x, y + 0.02f * std::cos(th) * rh, z};
}
float Prof(const std::vector<std::pair<float, float>>& pts, float x)
{
    if (x <= pts.front().first)
        return pts.front().second;
    for (size_t i = 1; i < pts.size(); ++i)
        if (x <= pts[i].first)
        {
            const auto a = pts[i - 1], b = pts[i];
            const float t = (x - a.first) / (b.first - a.first);
            return a.second + (b.second - a.second) * (t * t * (3 - 2 * t));
        }
    return pts.back().second;
}
CV Shell(float u, float v)
{
    static const std::vector<std::pair<float, float>> A = {{0, -0.9f}, {0.15f, -0.7f}, {0.35f, -0.42f}, {0.55f, -0.12f},
                                                          {0.72f, 0.12f}, {0.86f, 0.32f}, {0.95f, 0.46f}, {1, 0.5f}};
    static const std::vector<std::pair<float, float>> R = {{0, 0}, {0.15f, 0.2f}, {0.35f, 0.42f}, {0.55f, 0.62f},
                                                          {0.72f, 0.72f}, {0.86f, 0.66f}, {0.95f, 0.44f}, {1, 0.18f}};
    const float th = u * Tau, a = Prof(A, v), r = Prof(R, v);
    const float px = r * std::sin(th), py = r * std::cos(th);
    const float t = 0.66f, ca = std::cos(t), sa = std::sin(t);
    return {px, 0.74f - a * sa + py * ca, 0.05f + a * ca + py * sa};
}
CV Burrow(float u, float v)
{
    const float th = u * Tau;
    const float fan = 1 + 1.4f * std::pow(std::max(0.0f, std::cos(th)), 2.0f);
    const float r = v < 0.34f ? 0.5f * v / 0.34f : 0.5f + (v - 0.34f) / 0.66f * 1.6f * fan;
    const float lip = std::exp(-std::pow((v - 0.4f) / 0.1f, 2.0f)) * 0.2f;
    const float mound = v > 0.34f ? 0.12f * std::sin(Pi * (v - 0.34f) / 0.66f) * (fan - 0.6f) : 0;
    const float y = v < 0.34f ? -0.05f + 0.2f * std::pow(v / 0.34f, 2.0f) : lip + mound;
    return {r * std::sin(th), std::max(y, 0.0f) * 0.9f + 0.01f, r * std::cos(th)};
}

// ---- limb tables: attach (x, y, z, azimuth), lengths (l1, l2, l3, radius)
struct Limb
{
    float ax, ay, az, azim, l1, l2, l3, r;
};
std::vector<Limb> LimbTable(int species)
{
    std::vector<Limb> out;
    auto leg = [&](float s, int j, float L0, float L1, float L2, float r)
    {
        const float azs[4] = {0.62f, 0.2f, -0.18f, -0.6f};
        const float a = s > 0 ? azs[j] : Pi - azs[j];
        out.push_back({s * 0.44f, 0.06f, 0.2f - j * 0.14f, a, L0, L1, L2, r});
    };
    if (species == 0)
    {
        for (float s : {1.0f, -1.0f})
            for (int j = 0; j < 4; ++j)
                leg(s, j, 0.58f - j * 0.02f, 0.28f, 0.55f - std::fabs(j - 1.5f) * 0.05f, 0.045f);
        out.push_back({0.22f, 0.05f, 0.38f, 1.25f, 0.3f, 0.22f, 0.34f, 0.07f});
        out.push_back({-0.22f, 0.05f, 0.38f, Pi - 1.25f, 0.26f, 0.18f, 0.27f, 0.055f});
        out.push_back({0.36f, 0.17f, 0.4f, 1.35f, 0.2f, 0.18f, 0, 0.05f});
        out.push_back({-0.36f, 0.17f, 0.4f, Pi - 1.35f, 0.2f, 0.18f, 0, 0.05f});
    }
    else
    {
        for (float s : {1.0f, -1.0f})
            for (int j = 0; j < 4; ++j)
            {
                if (j < 2)
                    leg(s, j, 0.36f, 0.22f, 0.34f, 0.06f);
                else
                    leg(s, j, 0, 0, 0, 0.06f);
            }
        for (int k = 0; k < 8; k += 2)
            out[k].az += 0.32f;
        out.push_back({0.14f, 0.12f, 0.62f, 1.35f, 0.18f, 0.14f, 0.34f, 0.1f});
        out.push_back({-0.14f, 0.12f, 0.6f, Pi - 1.35f, 0.14f, 0.1f, 0.2f, 0.065f});
        out.push_back({0.08f, 0.3f, 0.66f, 1.45f, 0.18f, 0.1f, 0, 0.035f});
        out.push_back({-0.08f, 0.3f, 0.66f, Pi - 1.45f, 0.18f, 0.1f, 0, 0.035f});
    }
    return out;
}
const float Gait[8] = {0, Pi, 0, Pi, Pi, 0, Pi, 0};
CV Dir(float az, float el) { return {std::cos(az) * std::cos(el), std::sin(el), std::sin(az) * std::cos(el)}; }

// CritterBatch.js critterJoints (eyes 1, claws 0)
struct Joints
{
    CV j0, j1, j2, j3;
    float r;
};
Joints JointsOf(int si, const Limb& a, int k, float ph, float stride, float lift, float outK)
{
    const bool isLeg = k < 8, isClaw = k == 8 || k == 9;
    const float side = a.ax > 0 ? 1.0f : -1.0f;
    const float p = ph + Gait[std::min(k, 7)];
    const float swing = std::max(std::sin(p), 0.0f) * stride;
    const float az = a.azim + std::cos(p) * stride * 0.28f * (isLeg ? 1.0f : 0.0f);
    Joints J;
    J.j0 = {a.ax, a.ay + lift, a.az};
    if (isLeg)
    {
        J.j1 = J.j0 + Dir(az, 0.62f + swing * 0.22f) * a.l1;
        J.j2 = J.j1 + Dir(az, -0.15f + swing * 0.15f) * a.l2;
        const float e3 = std::asin(Clamp(-J.j2.y / std::max(a.l3, 1e-4f), -0.99f, -0.25f)) + swing * 0.25f;
        J.j3 = J.j2 + Dir(az, e3) * a.l3;
    }
    else if (isClaw)
    {
        const float inward = side * 0.95f;
        J.j1 = J.j0 + Dir(az, -0.35f) * a.l1;
        J.j2 = J.j1 + Dir(az + inward, 0.25f) * a.l2;
        J.j3 = J.j2 + Dir(az + inward * 1.55f, -0.25f) * a.l3;
    }
    else
    {
        const float el = 1.3f;
        J.j1 = J.j0 + Dir(az, el * 0.8f) * a.l1;
        J.j2 = J.j1 + Dir(az, el) * a.l2;
        J.j3 = J.j2;
    }
    const CV hole{0, lift + 0.32f, 0.5f};
    const float k2 = si == 1 ? outK : 1.0f;
    J.j0 = Mix(hole, J.j0, k2), J.j1 = Mix(hole, J.j1, k2), J.j2 = Mix(hole, J.j2, k2), J.j3 = Mix(hole, J.j3, k2);
    J.r = a.r * (si == 1 ? outK * 0.7f + 0.3f : 1.0f);
    return J;
}

struct MeshOut
{
    std::vector<WgrMeshVertex> v;
    std::vector<uint32_t> idx;
};
void AddVertex(MeshOut& m, CV p, CV n, float u, float tv)
{
    WgrMeshVertex w{};
    w.pos = {p.x, p.y, p.z};
    w.normal = {n.x, n.y, n.z};
    w.uv = {u, tv};
    w.uv1 = w.uv;
    m.v.push_back(w);
}

// One posed critter. Atlas: body grid (u around, v pole to pole) in the upper half; the limbs in
// the lower half, x along the limb (0..3.2 / 3.2), rows: legs, claws, eyes.
MeshOut Build(int si, float ph, float stride, float lift, float outK)
{
    MeshOut m;
    CV (*body)(float, float) = si == 0 ? Carapace : si == 1 ? Shell : Burrow;
    const float up = si == 2 ? 0.0f : lift;
    const uint32_t first = 0;
    // body grid (seam column doubled for the atlas), poles at the end
    for (int i = 0; i < NV; ++i)
    {
        const float v = (i + 1.0f) / (NV + 1);
        for (int j = 0; j <= NU; ++j)
        {
            CV p = body(float(j % NU) / NU, v);
            p.y += up;
            AddVertex(m, p, {0, 0, 0}, float(j) / NU, v * 0.5f);
        }
    }
    const uint32_t top = uint32_t(m.v.size());
    {
        CV p = body(0, 0);
        p.y += up;
        AddVertex(m, p, {0, 0, 0}, 0.5f, 0.0f);
        CV q = body(0, 1);
        q.y += up;
        AddVertex(m, q, {0, 0, 0}, 0.5f, 0.499f);
    }
    const uint32_t bot = top + 1;
    auto G = [&](int i, int j) { return first + uint32_t(i * (NU + 1) + j); };
    for (int i = 0; i < NV - 1; ++i)
        for (int j = 0; j < NU; ++j)
            m.idx.insert(m.idx.end(), {G(i, j), G(i + 1, j), G(i, j + 1), G(i, j + 1), G(i + 1, j), G(i + 1, j + 1)});
    for (int j = 0; j < NU; ++j)
    {
        m.idx.insert(m.idx.end(), {top, G(0, j), G(0, j + 1)});
        m.idx.insert(m.idx.end(), {bot, G(NV - 1, j + 1), G(NV - 1, j)});
    }
    // smooth normals of the body
    std::vector<CV> acc(m.v.size());
    for (size_t i = 0; i + 2 < m.idx.size(); i += 3)
    {
        auto P = [&](uint32_t k) { return CV{m.v[k].pos.x, m.v[k].pos.y, m.v[k].pos.z}; };
        const CV c = Cross(P(m.idx[i + 1]) - P(m.idx[i]), P(m.idx[i + 2]) - P(m.idx[i]));
        for (int k = 0; k < 3; ++k)
            acc[m.idx[i + k]] = acc[m.idx[i + k]] + c;
    }
    for (size_t i = 0; i < m.v.size(); ++i)
    {
        const CV n = Norm(acc[i]);
        m.v[i].normal = {n.x, n.y, n.z};
    }
    if (si == 2)
        return m;
    // limbs: rings of 4 along the joints, a tip
    const std::vector<Limb> table = LimbTable(si);
    for (int k = 0; k < Limbs; ++k)
    {
        const Limb& L = table[k];
        if (L.l1 + L.l2 + L.l3 <= 0)
            continue;
        const bool isClaw = k == 8 || k == 9, isEye = k >= 10;
        const Joints J = JointsOf(si, L, k, ph, stride, lift, outK);
        struct St
        {
            int seg;
            float t, r;
        };
        std::vector<St> stations = isClaw ? std::vector<St>{{0, 0, 1}, {1, 0, 1}, {2, 0, 1.6f}, {2, 0.45f, 2.1f}, {2, 1, 0.9f}}
                                   : isEye ? std::vector<St>{{0, 0, 1}, {1, 0, 0.8f}, {1, 0.5f, 1.6f}, {1, 1, 1.3f}}
                                           : std::vector<St>{{0, 0, 1.15f}, {1, 0, 1}, {2, 0, 0.8f}};
        const float row = isClaw ? 0.75f : isEye ? 0.92f : 0.58f;
        const uint32_t base = uint32_t(m.v.size());
        auto seg = [&](int s, CV& a, CV& b)
        {
            a = s == 0 ? J.j0 : s == 1 ? J.j1 : J.j2;
            b = s == 0 ? J.j1 : s == 1 ? J.j2 : J.j3;
        };
        for (const St& st : stations)
        {
            CV a, b;
            seg(st.seg, a, b);
            const CV d = Norm(b - a);
            const CV rf = std::fabs(d.y) > 0.9f ? CV{1, 0, 0} : CV{0, 1, 0};
            const CV ax = Norm(Cross(d, rf)), ay = Cross(ax, d);
            for (int j = 0; j < 4; ++j)
            {
                const float ang = j / 4.0f * Tau + Pi / 4;
                const CV radial = ax * std::cos(ang) + ay * std::sin(ang);
                AddVertex(m, Mix(a, b, st.t) + radial * (J.r * st.r), radial, Clamp((st.seg + st.t) / 3.2f, 0.01f, 0.99f), row);
            }
        }
        {
            CV a, b;
            seg(isEye ? 1 : 2, a, b);
            const float t = isClaw ? 1.18f : isEye ? 1.12f : 1.0f;
            AddVertex(m, Mix(a, b, t), Norm(b - a), isEye ? 0.99f : 0.99f, row);
        }
        const uint32_t tip = uint32_t(m.v.size() - 1);
        const uint32_t n = uint32_t(stations.size());
        for (uint32_t i = 0; i + 1 < n; ++i)
            for (uint32_t j = 0; j < 4; ++j)
            {
                const uint32_t a = base + i * 4 + j, b = base + i * 4 + (j + 1) % 4;
                m.idx.insert(m.idx.end(), {a, a + 4, b, b, a + 4, b + 4});
            }
        for (uint32_t j = 0; j < 4; ++j)
            m.idx.insert(m.idx.end(), {base + (n - 1) * 4 + j, tip, base + (n - 1) * 4 + (j + 1) % 4});
    }
    return m;
}

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
// CritterBatch.js colours at an atlas texel
Rgb Colour(int si, float tu, float tv)
{
    const float n = Hash(tu * 61, tv * 53) * 2 - 1;
    if (tv < 0.5f)
    {
        const float u = tu, v = tv / 0.5f;
        if (si == 0)
        {
            // ghost crab: pale straw carapace with fine granules
            const Rgb straw = MixC({0.8f, 0.73f, 0.58f}, {0.88f, 0.83f, 0.7f}, n * 0.5f + 0.5f);
            const float k = 0.86f + 0.18f * Smooth(0.0f, 0.5f, 1 - v);
            return {straw.r * k, straw.g * k, straw.b * k};
        }
        if (si == 1)
        {
            // hermit crab: banded turban shell, the aperture showing the dark body
            const float turns = u + v * 3.2f;
            const float suture = Smooth(0.9f, 0.97f, turns - std::floor(turns)) * Smooth(0.05f, 0.3f, v);
            const float bands = Smooth(0.2f, 0.8f, std::sin(v * 31.0f + n * 2.0f));
            const Rgb shell = MixC({0.72f, 0.42f, 0.2f}, {0.35f, 0.18f, 0.08f}, bands * 0.8f);
            const float k = (1 - suture * 0.5f) * (n * 0.12f + 0.94f);
            return MixC({shell.r * k, shell.g * k, shell.b * k}, {0.35f, 0.12f, 0.08f}, Smooth(0.86f, 0.93f, v));
        }
        // burrow: dark shaft, damp dug-out sand around the lip, dry beyond
        const Rgb dry{0.86f * (n * 0.1f + 0.95f), 0.79f * (n * 0.1f + 0.95f), 0.64f * (n * 0.1f + 0.95f)};
        const Rgb sand = MixC({0.66f, 0.58f, 0.45f}, dry, Smooth(0.4f, 0.75f, v));
        return MixC(sand, {0.035f, 0.03f, 0.025f}, 1 - Smooth(0.2f, 0.36f, v));
    }
    const float u = tu * 3.2f; // along the limb (segment + t)
    const int part = tv < 0.66f ? 1 : tv < 0.84f ? 2 : 3;
    if (si == 0)
    {
        if (part == 1)
            return MixC({0.86f, 0.82f, 0.72f}, {0.7f, 0.62f, 0.5f}, Smooth(0.8f, 1.0f, u - std::floor(u)) * 0.5f);
        if (part == 2)
            return MixC({0.9f, 0.87f, 0.8f}, {0.78f, 0.7f, 0.75f}, Smooth(1.6f, 2.3f, u));
        return MixC({0.8f, 0.75f, 0.62f}, {0.04f, 0.04f, 0.045f}, Smooth(1.25f, 1.4f, u));
    }
    if (part == 1)
        return MixC({0.72f, 0.28f, 0.12f}, {0.9f, 0.78f, 0.6f}, Smooth(2.6f, 3.0f, u));
    if (part == 2)
        return MixC({0.42f, 0.14f, 0.4f}, {0.85f, 0.5f, 0.2f}, Smooth(2.5f, 3.1f, u));
    return {0.1f, 0.08f, 0.06f};
}

uint32_t HashCell(int x, int z, uint32_t salt)
{
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(z) * 668265263u + salt * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
} // namespace

void BeachCritters::EnsureMeshes()
{
    if (_built)
        return;
    _built = true;
    EngineWgpu::AcquireProducerWindow("Crab meshes");
    _meshes.assign(3 * PosesPerSpecies, 0);
    _indexCounts.assign(_meshes.size(), 0);
    auto make = [&](size_t i, const MeshOut& m)
    {
        _meshes[i] = wgr_mesh_create(_renderer, WgrSlice<WgrMeshVertex>(m.v), WgrSlice<uint32_t>(m.idx));
        _indexCounts[i] = uint32_t(m.idx.size());
    };
    for (int si = 0; si < 2; ++si)
    {
        const float runLift = si == 0 ? 0.4f : 0.14f, standLift = si == 0 ? 0.32f : 0.14f;
        for (int p = 0; p < Phases; ++p)
            make(size_t(si) * PosesPerSpecies + p, Build(si, Tau * p / Phases, 1.0f, runLift, 1.0f));
        make(size_t(si) * PosesPerSpecies + Phases, Build(si, 0, 0, standLift, 1.0f));
        make(size_t(si) * PosesPerSpecies + Phases + 1, Build(si, 0, 0, 0.0f, 0.0f)); // hermit: in its shell
    }
    make(size_t(2) * PosesPerSpecies, Build(2, 0, 0, 0, 1));
    for (int si = 0; si < 3; ++si)
    {
        constexpr int TW = 64, TH = 64;
        std::vector<uint8_t> px(TW * TH * 4);
        for (int y = 0; y < TH; ++y)
            for (int x = 0; x < TW; ++x)
            {
                const Rgb c = Colour(si, (x + 0.5f) / TW, (y + 0.5f) / TH);
                uint8_t* q = &px[(y * TW + x) * 4];
                q[0] = uint8_t(Clamp(c.r, 0, 1) * 255), q[1] = uint8_t(Clamp(c.g, 0, 1) * 255), q[2] = uint8_t(Clamp(c.b, 0, 1) * 255);
                q[3] = 255;
            }
        _textures.push_back(wgr_texture_create(_renderer, TW, TH, WGR_TEXTURE_RGBA8, 1, 0, px.data(), uint32_t(px.size())));
    }
    LOG_INFO(Graphics, "Crabs: ghost and hermit crab gaits ({} phases) and the burrow baked", Phases);
}

// The homes within reach of the camera: per 12 m cell, burrows on the dry upper beach (0.5-2.5 m
// above the sea), hermit homes behind them (1.5-5 m), on gentle ground only.
void BeachCritters::Scan(const Vector3& camera)
{
    _homes.clear();
    const int r = int(std::ceil(Active / CellSize));
    const int ccx = int(std::floor(camera.X() / CellSize)), ccz = int(std::floor(camera.Z() / CellSize));
    for (int dz = -r; dz <= r; ++dz)
        for (int dx = -r; dx <= r; ++dx)
        {
            const int cx = ccx + dx, cz = ccz + dz;
            const int64_t key = (int64_t(cx) << 32) ^ int64_t(uint32_t(cz));
            auto it = _cells.find(key);
            if (it == _cells.end())
            {
                std::vector<Home> homes;
                uint32_t h = HashCell(cx, cz, 3);
                auto rnd = [&]
                {
                    h = h * 1664525u + 1013904223u;
                    return (h >> 8) / 16777216.0f;
                };
                const int tries = 5;
                for (int i = 0; i < tries; ++i)
                {
                    const float x = (cx + rnd()) * CellSize, z = (cz + rnd()) * CellSize;
                    const float yaw = rnd() * Tau, seed = rnd(), size = 0.85f + rnd() * 0.35f, kind = rnd();
                    const float g = Wildlife::Ground(x, z), sea = Wildlife::SeaLevel(x, z);
                    const float e = g - sea;
                    const float slope = std::max(std::fabs(Wildlife::Ground(x + 1, z) - g), std::fabs(Wildlife::Ground(x, z + 1) - g));
                    if (slope > 0.25f)
                        continue;
                    static int logged = 0;
                    if (logged < 12 && e > 0 && e < 5 && GLandscape && !Wildlife::Earth())
                    {
                        ++logged;
                        const SurfaceInfo& si = GLandscape->SurfaceAt(x, z);
                        LOG_INFO(Graphics, "Crabs: surface at {:.0f} {:.0f} (+{:.1f} m): class '{}' sound '{}' files '{}'", x, z, e,
                                 (const char*)si._class, (const char*)si._soundEnv, (const char*)si._name);
                    }
                    // ghost crabs dig in bare sand only; hermit crabs keep to the back of the beach
                    if (e > 0.5f && e < 2.5f && kind < 0.7f && Wildlife::Sand(x, z))
                        homes.push_back({x, g, z, yaw, seed, size, false});
                    else if (e > 1.5f && e < 5.0f && kind >= 0.7f)
                        homes.push_back({x, g, z, yaw, seed, size, true});
                }
                it = _cells.emplace(key, std::move(homes)).first;
            }
            for (const Home& h : it->second)
            {
                const float ex = h.x - camera.X(), ez = h.z - camera.Z();
                if (ex * ex + ez * ez < Active * Active)
                    _homes.push_back(h);
            }
        }
    if (_cells.size() > 3000)
        _cells.clear();
    // crabs whose home went out of reach go; free homes near the camera get one
    _crabs.erase(std::remove_if(_crabs.begin(), _crabs.end(),
                                [&](const Crab& c)
                                {
                                    const float ex = c.h.x - camera.X(), ez = c.h.z - camera.Z();
                                    return ex * ex + ez * ez > (Active + 4) * (Active + 4);
                                }),
                 _crabs.end());
    int ghosts = 0, hermits = 0;
    for (const Crab& c : _crabs)
        (c.hermit ? hermits : ghosts)++;
    for (const Home& h : _homes)
    {
        const bool taken = std::any_of(_crabs.begin(), _crabs.end(), [&](const Crab& c) { return c.h.x == h.x && c.h.z == h.z; });
        if (taken || (h.hermit ? hermits >= MaxHermit : ghosts >= MaxGhost))
            continue;
        Crab c;
        c.h = h;
        c.hermit = h.hermit;
        c.seed = h.seed;
        c.size = h.hermit ? 0.028f + h.seed * 0.016f : (0.034f + h.seed * 0.022f) * h.size;
        c.x = h.x, c.z = h.z, c.yaw = h.yaw;
        c.state = h.hermit ? 1 : (Rand() < 0.5f ? 0 : 3);
        c.out = h.hermit || c.state == 3 ? 1.0f : 0.0f;
        c.t = Rand() * 5;
        if (!h.hermit && c.state == 3)
        {
            const float a = Rand() * Tau, rr = 0.5f + Rand() * 3;
            c.x = h.x + std::cos(a) * rr, c.z = h.z + std::sin(a) * rr;
        }
        (h.hermit ? hermits : ghosts)++;
        _crabs.push_back(c);
    }
}

namespace
{
// Crabs.js moveTo: toward (tx, tz) at v, sideways like a crab (the hermit crab faces its way)
float MoveTo(float& x, float& z, float tx, float tz, float& speed, float& yaw, float dt, float v, float accel, bool sideways)
{
    const float dx = tx - x, dz = tz - z, d = std::hypot(dx, dz);
    const float want = d < 0.02f ? 0 : std::min(v, d * 6);
    speed += Clamp(want - speed, -accel * 2 * dt, accel * dt);
    if (d > 1e-4f)
    {
        const float s = std::min(speed * dt, d);
        x += dx / d * s, z += dz / d * s;
        const float heading = std::atan2(dx, dz);
        if (sideways)
        {
            const float a = heading + Pi / 2, b = heading - Pi / 2;
            const float target = std::fabs(AngleDiff(a, yaw)) < std::fabs(AngleDiff(b, yaw)) ? a : b;
            yaw += AngleDiff(target, yaw) * Approach(14, dt);
        }
        else
            yaw += AngleDiff(heading, yaw) * Approach(3, dt);
    }
    return d;
}
float NearestThreat(float x, float z, const Vector3* threats, int n)
{
    float d = 1e9f;
    for (int i = 0; i < n; ++i)
        d = std::min(d, std::hypot(x - threats[i].X(), z - threats[i].Z()));
    return d;
}
} // namespace

void BeachCritters::StepGhost(Crab& c, float dt, const Vector3* threats, int nThreats)
{
    const float d = NearestThreat(c.x, c.z, threats, nThreats);
    const bool scary = d < 6.5f, alert = d < 10.0f;
    c.t -= dt;
    switch (c.state)
    {
    case 0: // hidden in the burrow
        c.out = 0;
        if (d < 10)
            c.t = std::max(c.t, 4 + Rand() * 6);
        if (c.t <= 0)
            c.state = 1, c.t = 1.5f + Rand() * 3, c.x = c.h.x, c.z = c.h.z;
        break;
    case 1: // eyestalks just above the rim
        c.out += (0.45f - c.out) * Approach(3, dt);
        if (alert)
            c.state = 6;
        else if (c.t <= 0)
            c.state = 2;
        break;
    case 2: // emerging
        c.out = std::min(1.0f, c.out + dt * 1.6f);
        if (alert)
            c.state = 6;
        else if (c.out >= 1)
            c.state = 4, c.t = 0.5f + Rand();
        break;
    case 3: // foraging: a burst to a spot near the burrow
    {
        const float rem = MoveTo(c.x, c.z, c.tx, c.tz, c.speed, c.yaw, dt, 0.3f + c.seed * 0.2f, 12, true);
        if (scary)
            c.state = 5;
        else if (rem < 0.03f)
            c.state = 4, c.t = 0.6f + Rand() * 2.5f;
        break;
    }
    case 4: // pausing, then the next burst
        MoveTo(c.x, c.z, c.x, c.z, c.speed, c.yaw, dt, 0, 30, true);
        if (scary)
            c.state = 5;
        else if (c.t <= 0)
        {
            const float a = Rand() * Tau, rr = 0.4f + Rand() * 3.5f;
            c.tx = c.h.x + std::cos(a) * rr, c.tz = c.h.z + std::sin(a) * rr, c.state = 3;
            if (Rand() < 0.12f)
                c.tx = c.h.x, c.tz = c.h.z; // home again now and then
        }
        break;
    case 5: // the dash home
    {
        const float rem = MoveTo(c.x, c.z, c.h.x, c.h.z, c.speed, c.yaw, dt, 2.1f + c.seed * 0.9f, 25, true);
        if (rem < 0.05f)
            c.state = 6;
        break;
    }
    case 6: // going down
        c.out = std::max(0.0f, c.out - dt * 4);
        c.speed = 0;
        if (c.out <= 0)
            c.state = 0, c.t = 4 + Rand() * 8;
        break;
    }
    // Crabs.js gait: the stride frequency grows with speed
    const float f = Clamp(c.speed / (c.size * 2.4f), 0, 13);
    c.phase = std::fmod(c.phase + Tau * f * dt, Tau);
    c.stride += (Clamp(c.speed / (c.size * 10), 0, 1) - c.stride) * Approach(10, dt);
}

void BeachCritters::StepHermit(Crab& c, float dt, const Vector3* threats, int nThreats)
{
    const float d = NearestThreat(c.x, c.z, threats, nThreats);
    c.t -= dt;
    if (d < 3.0f && c.state != 2)
        c.state = 2, c.t = 5 + Rand() * 5;
    switch (c.state)
    {
    case 0:
    {
        const float rem = MoveTo(c.x, c.z, c.tx, c.tz, c.speed, c.yaw, dt, 0.035f + c.seed * 0.03f, 0.3f, false);
        if (rem < 0.02f)
            c.state = 1, c.t = 2 + Rand() * 6;
        c.out += (1 - c.out) * Approach(2, dt);
        break;
    }
    case 1:
        MoveTo(c.x, c.z, c.x, c.z, c.speed, c.yaw, dt, 0, 1, false);
        c.out += (1 - c.out) * Approach(2, dt);
        if (c.t <= 0)
        {
            const float a = Rand() * Tau, rr = Rand() * 2.5f;
            c.tx = c.h.x + std::cos(a) * rr, c.tz = c.h.z + std::sin(a) * rr, c.state = 0;
        }
        break;
    case 2: // pulled in, coming out slowly once it is quiet
        c.speed = 0;
        c.out = std::max(0.0f, c.out - dt * 3);
        if (c.t <= 0 && d > 4)
            c.state = 1, c.t = 1;
        break;
    }
    const float f = Clamp(c.speed / (c.size * 1.2f), 0, 4);
    c.phase = std::fmod(c.phase + Tau * f * dt, Tau);
    c.stride += (Clamp(c.speed / (c.size * 3), 0, 1) - c.stride) * Approach(6, dt);
}

void BeachCritters::Update(const Vector3& camera, float time)
{
    static const bool enabled = []
    {
        const char* v = std::getenv("WGR_CRABS");
        return !(v && v[0] == '0');
    }();
    if (!enabled || !GLandscape || !_renderer)
        return;
    float dt = _lastTime < 0 ? 0.0f : time - _lastTime;
    _lastTime = time;
    dt = Clamp(dt, 0.0f, 0.1f);
    // only for a viewer near the ground: from the air crabs are invisible anyway
    const float ground = Wildlife::Ground(camera.X(), camera.Z());
    if (camera.Y() - std::max(ground, Wildlife::SeaLevel(camera.X(), camera.Z())) > 40.0f)
        return;
    _scan -= dt;
    if (_scan <= 0)
    {
        _scan = 0.5f;
        Scan(camera);
    }
    if (_crabs.empty() && _homes.empty())
        return;
    EnsureMeshes();
    // who scares them: the camera near the ground, the player
    Vector3 threats[2];
    int nThreats = 0;
    if (camera.Y() - ground < 6.0f)
        threats[nThreats++] = camera;
    if (GWorld && GWorld->GetRealPlayer())
        threats[nThreats++] = GWorld->GetRealPlayer()->Position();
    for (Crab& c : _crabs)
        c.hermit ? StepHermit(c, dt, threats, nThreats) : StepGhost(c, dt, threats, nThreats);

    // WGR_CRABS_SCALE: a debug magnifier for captures
    static const float debugScale = []
    {
        const char* v = std::getenv("WGR_CRABS_SCALE");
        return v ? std::max(0.1f, float(std::atof(v))) : 1.0f;
    }();
    auto submit = [&](size_t mi, float x, float y, float z, float yaw, float s0, int tex)
    {
        const float s = s0 * debugScale;
        const float c = std::cos(yaw) * s, sn = std::sin(yaw) * s;
        const float world[16] = {c, 0, -sn, 0, 0, s, 0, 0, sn, 0, c, 0, x, y, z, 1};
        _engine.SubmitDynamicMesh(_meshes[mi], _indexCounts[mi], world, _textures[tex], 2);
    };
    int drawn = 0;
    // burrows
    for (const Home& h : _homes)
    {
        if (h.hermit)
            continue;
        const float dist = std::hypot(h.x - camera.X(), h.z - camera.Z());
        if (dist > Draw)
            continue;
        submit(size_t(2) * PosesPerSpecies, h.x, h.y - 0.005f, h.z, h.yaw, 0.05f * h.size, 2);
    }
    for (const Crab& c : _crabs)
    {
        if (!c.hermit && c.out <= 0.01f)
            continue;
        const float y = Wildlife::Ground(c.x, c.z);
        const float dist = std::sqrt((c.x - camera.X()) * (c.x - camera.X()) + (y - camera.Y()) * (y - camera.Y()) +
                                     (c.z - camera.Z()) * (c.z - camera.Z()));
        if (dist > Draw)
            continue;
        const int si = c.hermit ? 1 : 0;
        int pose = c.stride > 0.15f ? std::clamp(int(c.phase / Tau * Phases), 0, Phases - 1) : Phases;
        if (c.hermit && c.out < 0.5f)
            pose = Phases + 1;
        // a ghost crab sinks into its burrow (the sand hides what is below)
        const float sink = c.hermit ? 0.0f : (1 - c.out) * 1.3f * c.size;
        const float s = c.size * Smooth(Draw, Draw * 0.72f, dist);
        submit(size_t(si) * PosesPerSpecies + pose, c.x, y - sink, c.z, c.yaw, s, si);
        ++drawn;
    }
    static float nextLog = 0;
    if (time >= nextLog)
    {
        nextLog = time + 5.0f;
        LOG_INFO(Graphics, "Crabs: {} homes near, {} crabs, {} drawn; first at {:.1f} {:.1f} {:.1f} state {} out {:.2f}",
                 _homes.size(), _crabs.size(), drawn, _crabs.empty() ? 0.0f : _crabs[0].x, _crabs.empty() ? 0.0f : _crabs[0].h.y,
                 _crabs.empty() ? 0.0f : _crabs[0].z, _crabs.empty() ? -1 : _crabs[0].state, _crabs.empty() ? 0.0f : _crabs[0].out);
    }
}
} // namespace Poseidon
