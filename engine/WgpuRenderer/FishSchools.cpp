// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fish around the camera: stage 1 of porting Tidewater's wildlife (dgreenheck/tidewater, MIT,
// (c) DRG Software Solutions LLC; see CREDITS.md). From src/world/Fish.js and src/world/fish/:
// the species anatomy tables (FishSpeciesTable.inc, generated from FishSpecies.js), the body built as
// rings of superellipse cross-sections between those profiles, the behaviour parameters (body lengths
// per second, boids weights and radii, depth band, preferred height in the water column, flee
// distance, tail-beat frequency) and the boids model with homes, wander, flight from the swimmer and
// the seabed / surface bounds.
//
// What OP changes: fins are single membranes per segment (no rays), the swim wave is baked into 12
// phases per mesh instead of bent in a vertex shader (each fish draws the phase it is at), the skin
// is a small per-species colour pattern texture instead of Tidewater's skin shader, and the habitats
// are chosen from the water depth anywhere in OFP's seas rather than an authored reef.

#include "FishSchools.hpp"
#include "EngineWgpu.hpp"
#include "WildlifeWorld.hpp"
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
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
constexpr float Tau = 6.2831853f;
constexpr int Lods = 3, Phases = 12;
constexpr float DrawRange = 45.0f, SimRange = 70.0f;
constexpr int MaxGroups = 10;

struct Fin
{
    float from, to, h0, h1, hmax;
    bool spiny;
};
struct Species
{
    const char* name;
    float body, sec;
    std::vector<std::pair<float, float>> top, bot, wid;
    struct
    {
        float u, y, r;
    } eye;
    struct
    {
        const char* shape;
        float len, span, fork;
    } caudal;
    struct
    {
        float u, y, len;
    } pectoral;
    std::vector<Fin> dorsal, anal;
};
const Species SpeciesTable[] = {
#include "FishSpeciesTable.inc"
};
constexpr int SpeciesCount = int(std::size(SpeciesTable));

// Tidewater's BEHAVIOUR rows for these species (Fish.js), plus the group sizes.
struct Behaviour
{
    int species;
    const char* mode;
    float lenMin, lenMax, cruise, max, burst, accel, sep, nbr, wSep, wAli, wCoh, wGoal, flee;
    float depthMin, depthMax, homeRadius, amp, freq0, freq1, bandMin, bandMax;
    int countMin, countMax;
    float weight;
};
const Behaviour Behaviours[] = {
    {0, "school", 0.06f, 0.09f, 1.6f, 3.5f, 9, 6, 1.8f, 7, 7, 2.6f, 1.4f, 0.7f, 4.5f, 0.35f, 0.8f, 18, 0.10f, 3.0f, 1.1f, 4, 20, 60, 120, 1.0f},
    {1, "hover", 0.08f, 0.11f, 0.8f, 2.5f, 7, 5, 2.2f, 6, 5, 0.5f, 0.5f, 1.0f, 3.0f, 0.12f, 0.45f, 3, 0.10f, 2.5f, 0.9f, 4, 16, 8, 16, 1.0f},
    {2, "mill", 0.16f, 0.24f, 0.45f, 1.8f, 5, 3, 1.6f, 4, 5, 1.8f, 1.0f, 0.9f, 3.5f, 0.04f, 0.2f, 1.5f, 0.08f, 1.6f, 0.9f, 2.5f, 10, 6, 14, 1.0f},
    {3, "school", 0.25f, 0.36f, 0.8f, 2.2f, 5, 3, 1.8f, 5, 5, 1.6f, 0.8f, 0.7f, 5.0f, 0.15f, 0.5f, 16, 0.08f, 1.5f, 0.9f, 3, 14, 8, 20, 1.0f},
    {4, "forage", 0.15f, 0.24f, 0.7f, 2.0f, 5, 3, 1.8f, 5, 5, 1.0f, 0.7f, 0.9f, 4.0f, 0.03f, 0.2f, 14, 0.07f, 1.6f, 0.9f, 2, 9, 6, 12, 0.8f},
    {5, "solo", 1.0f, 1.35f, 0.15f, 1.2f, 3, 1.2f, 1, 2, 1, 0, 0, 0.6f, 3.5f, 0.3f, 0.6f, 10, 0.035f, 0.5f, 0.8f, 4, 16, 1, 2, 0.6f},
    {6, "patrol", 0.4f, 0.6f, 1.1f, 2.6f, 5, 3, 2, 5, 5, 1.4f, 0.9f, 0.9f, 4.0f, 0.25f, 0.6f, 20, 0.07f, 1.6f, 0.8f, 6, 20, 4, 8, 0.7f},
};
constexpr int BehaviourCount = int(std::size(Behaviours));

// Tidewater's profile interpolation (FishGeometry.js prof): smoothstep-eased between the points.
float Prof(const std::vector<std::pair<float, float>>& pts, float u)
{
    if (u <= pts.front().first)
        return pts.front().second;
    for (size_t i = 1; i < pts.size(); ++i)
    {
        if (u <= pts[i].first)
        {
            const auto a = pts[i - 1], b = pts[i];
            const float t = (u - a.first) / (b.first - a.first);
            const float s = t * t * (3 - 2 * t);
            return a.second + (b.second - a.second) * (0.5f * t + 0.5f * s);
        }
    }
    return pts.back().second;
}

struct MeshBuilder
{
    std::vector<WgrMeshVertex> v;
    std::vector<uint32_t> idx;
    uint32_t add(float x, float y, float z, float u, float tv)
    {
        WgrMeshVertex w{};
        w.pos = {x, y, z};
        w.normal = {0, 0, 0};
        w.uv = {u, tv};
        v.push_back(w);
        return uint32_t(v.size() - 1);
    }
    void tri(uint32_t a, uint32_t b, uint32_t c) { idx.insert(idx.end(), {a, b, c}); }
    void quad(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { tri(a, b, d), tri(b, c, d); }
    void normals()
    {
        for (size_t i = 0; i + 2 < idx.size(); i += 3)
        {
            auto& a = v[idx[i]].pos;
            auto& b = v[idx[i + 1]].pos;
            auto& c = v[idx[i + 2]].pos;
            const float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, wx = c.x - a.x, wy = c.y - a.y, wz = c.z - a.z;
            const float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
            for (int k = 0; k < 3; ++k)
            {
                auto& n = v[idx[i + k]].normal;
                n.x += nx, n.y += ny, n.z += nz;
            }
        }
        for (auto& w : v)
        {
            const float l = std::sqrt(w.normal.x * w.normal.x + w.normal.y * w.normal.y + w.normal.z * w.normal.z);
            if (l > 1e-12f)
                w.normal = {w.normal.x / l, w.normal.y / l, w.normal.z / l};
            else
                w.normal = {0, 1, 0};
        }
    }
};

// Texture layout: u along the fish (0 snout .. 1 tail tip); v 0 .. 0.88 the body from the belly to the
// back; v 0.9 .. 1 the fins.
constexpr float FinV = 0.95f;

// Lateral swim displacement at local z (snout +0.5, tail -0.5) for a phase in 0..1: Tidewater's
// carangiform wave -- small at the head, growing towards the tail, travelling backwards.
float SwimOffset(float z, float phase, float amp)
{
    const float u = 0.5f - z;
    return amp * (0.12f + 0.88f * u * u) * std::sin(Tau * phase - Tau * 0.85f * u);
}

void BuildFish(const Species& S, int lod, float phase, float amp, MeshBuilder& m)
{
    static const int RingsByLod[Lods] = {22, 12, 6}, AroundByLod[Lods] = {14, 8, 6};
    const int rings = RingsByLod[lod], around = AroundByLod[lod];
    const float e = 2.0f / S.sec;
    const auto bend = [&](float x, float z) { return x + SwimOffset(z, phase, amp); };
    // body rings
    std::vector<uint32_t> ring((rings + 1) * (around + 1));
    for (int i = 0; i <= rings; ++i)
    {
        const float t = float(i) / rings;
        const float u = std::max(0.6f * std::pow(t, 1.55f) + 0.4f * t, 0.01f);
        const float T = Prof(S.top, u), B = Prof(S.bot, u), W = Prof(S.wid, u);
        const float z = 0.5f - u * S.body;
        for (int j = 0; j <= around; ++j)
        {
            const float phi = Tau * j / around;
            const float s = std::sin(phi), k = std::cos(phi);
            const float x = W * (s >= 0 ? 1.0f : -1.0f) * std::pow(std::fabs(s), e);
            const float y = (k >= 0 ? T : B) * (k >= 0 ? 1.0f : -1.0f) * std::pow(std::fabs(k), e);
            const float hv = 0.44f + 0.44f * (y >= 0 ? y / std::max(T, 1e-4f) : y / std::max(B, 1e-4f));
            ring[i * (around + 1) + j] = m.add(bend(x, z), y, z, u * S.body, hv);
        }
    }
    for (int i = 0; i < rings; ++i)
        for (int j = 0; j < around; ++j)
        {
            const uint32_t a = ring[i * (around + 1) + j], b = ring[i * (around + 1) + j + 1],
                           c = ring[(i + 1) * (around + 1) + j + 1], d = ring[(i + 1) * (around + 1) + j];
            m.quad(a, b, c, d);
        }
    // snout cap
    const uint32_t tip = m.add(bend(0, 0.5f), 0, 0.5f, 0, 0.44f);
    for (int j = 0; j < around; ++j)
        m.tri(tip, ring[j + 1], ring[j]);

    // a fin membrane between a base line and its tip line, double sided (the draw is two-sided)
    const auto membrane = [&](float u0, float u1, float h0, float h1, float hmax, bool up, int steps)
    {
        std::vector<std::pair<uint32_t, uint32_t>> col;
        for (int k = 0; k <= steps; ++k)
        {
            const float t = float(k) / steps;
            const float u = u0 + (u1 - u0) * t;
            const float h = h0 + (h1 - h0) * t + (hmax - std::max(h0, h1)) * std::sin(3.14159f * t);
            const float z = 0.5f - u * S.body;
            const float base = up ? Prof(S.top, u) * 0.92f : -Prof(S.bot, u) * 0.92f;
            const float rake = 0.35f * h;
            const float zt = z - rake;
            const uint32_t b = m.add(bend(0, z), base, z, u * S.body, FinV);
            const uint32_t tp = m.add(bend(0, zt), base + (up ? h : -h), zt, u * S.body, 1.0f);
            col.push_back({b, tp});
        }
        for (size_t k = 0; k + 1 < col.size(); ++k)
            m.quad(col[k].first, col[k + 1].first, col[k + 1].second, col[k].second);
    };
    const int finSteps = lod == 0 ? 6 : 3;
    if (lod <= 1)
    {
        for (const Fin& f : S.dorsal)
            membrane(f.from, f.to, f.h0, f.h1, f.hmax, true, finSteps);
        for (const Fin& f : S.anal)
            membrane(f.from, f.to, f.h0, f.h1, f.hmax, false, finSteps);
    }
    // caudal fin: a fan from the peduncle to the tail tip, its trailing edge shaped by the species
    {
        const float zp = 0.5f - S.body;
        const float hp = 0.8f * std::max(Prof(S.top, 1.0f), Prof(S.bot, 1.0f));
        const int across = lod == 0 ? 8 : 4, along = lod == 0 ? 4 : 2;
        const std::string shape = S.caudal.shape;
        std::vector<uint32_t> g((along + 1) * (across + 1));
        for (int a = 0; a <= along; ++a)
            for (int c = 0; c <= across; ++c)
            {
                const float t = float(a) / along, s = 2.0f * c / across - 1.0f;
                float reach;
                if (shape == "forked" || shape == "lunate")
                    reach = S.caudal.fork + (1.0f - S.caudal.fork) * std::fabs(s);
                else if (shape == "rounded")
                    reach = 1.0f - 0.3f * s * s;
                else
                    reach = 1.0f;
                const float z = zp - t * S.caudal.len * reach;
                const float y = s * (hp + (S.caudal.span - hp) * t);
                g[a * (across + 1) + c] = m.add(bend(0, z), y, z, (0.5f - z), 0.9f + 0.1f * t);
            }
        for (int a = 0; a < along; ++a)
            for (int c = 0; c < across; ++c)
                m.quad(g[a * (across + 1) + c], g[a * (across + 1) + c + 1], g[(a + 1) * (across + 1) + c + 1],
                       g[(a + 1) * (across + 1) + c]);
    }
    // pectoral fins: small triangles swept back from behind the gill cover
    if (lod == 0)
    {
        for (int side = -1; side <= 1; side += 2)
        {
            const float u = S.pectoral.u, z = 0.5f - u * S.body, y = S.pectoral.y;
            const float w = Prof(S.wid, u) * 0.9f * side;
            const float len = S.pectoral.len;
            const uint32_t a = m.add(bend(w, z), y + 0.01f, z, u * S.body, FinV);
            const uint32_t b = m.add(bend(w, z - 0.02f), y - 0.01f, z - 0.02f, u * S.body, FinV);
            const uint32_t c = m.add(bend(w + side * len * 0.45f, z - len * 0.85f), y - len * 0.25f, z - len * 0.85f,
                                     (u + len) * S.body, 1.0f);
            m.tri(a, b, c);
        }
    }
    m.normals();
}

// Skin patterns (OP's own approximations of the species; Tidewater draws them in FishMaterial.js).
struct Rgb
{
    float r, g, b;
};
Rgb Mix(Rgb a, Rgb b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
float Band(float x, float a, float b, float soft = 0.03f)
{
    const float s0 = std::clamp((x - a) / soft + 0.5f, 0.0f, 1.0f), s1 = std::clamp((b - x) / soft + 0.5f, 0.0f, 1.0f);
    return s0 * s1;
}
Rgb Skin(int species, float u, float v)
{
    const bool fin = v > 0.89f;
    const float h = std::clamp(v / 0.88f, 0.0f, 1.0f); // 0 belly .. 1 back
    switch (species)
    {
        case 0: // silverside: olive back, a silver band along the flank, white belly; pale fins
            if (fin) return {0.75f, 0.78f, 0.74f};
            return Mix(Mix({0.93f, 0.93f, 0.9f}, {0.42f, 0.52f, 0.44f}, (h - 0.55f) / 0.3f), {0.88f, 0.9f, 0.94f}, Band(h, 0.48f, 0.6f));
        case 1: // blue chromis
            if (fin) return {0.18f, 0.32f, 0.62f};
            return Mix({0.38f, 0.55f, 0.8f}, {0.12f, 0.24f, 0.58f}, h);
        case 2: // French grunt: blue-grey with slanted yellow stripes
        {
            if (fin) return {0.85f, 0.72f, 0.22f};
            const Rgb base = Mix({0.78f, 0.8f, 0.82f}, {0.42f, 0.48f, 0.56f}, h);
            const float stripe = 0.5f + 0.5f * std::sin((u * 34.0f + h * 9.0f));
            return Mix(base, {0.96f, 0.8f, 0.2f}, stripe > 0.7f ? 0.85f : 0.0f);
        }
        case 3: // yellowtail snapper: blue-grey back with yellow spots, a yellow stripe to a yellow tail
        {
            if (fin || u > 0.78f) return {0.97f, 0.83f, 0.2f};
            Rgb c = Mix({0.94f, 0.88f, 0.86f}, {0.45f, 0.55f, 0.72f}, (h - 0.4f) / 0.4f);
            const float spots = (std::sin(u * 60.0f) * std::sin(h * 40.0f) > 0.55f && h > 0.62f) ? 1.0f : 0.0f;
            c = Mix(c, {0.95f, 0.85f, 0.3f}, spots * 0.8f);
            return Mix(c, {0.98f, 0.82f, 0.18f}, Band(h, 0.45f - 0.08f * u, 0.55f + 0.12f * u));
        }
        case 4: // blue tang: bright blue with dark lines, dark-edged fins
            if (fin) return {0.08f, 0.18f, 0.5f};
            return Mix(Mix({0.2f, 0.38f, 0.85f}, {0.1f, 0.25f, 0.75f}, h), {0.05f, 0.1f, 0.3f},
                       std::fabs(std::sin(u * 26.0f + h * 4.0f)) > 0.94f ? 0.7f : 0.0f);
        case 5: // barracuda: dark grey-green back, silver flank with dark bars, white belly, dark fins
        {
            if (fin) return {0.22f, 0.25f, 0.25f};
            Rgb c = Mix(Mix({0.95f, 0.95f, 0.93f}, {0.78f, 0.8f, 0.82f}, h / 0.5f), {0.28f, 0.33f, 0.33f}, (h - 0.62f) / 0.25f);
            const float bar = (std::sin(u * 46.0f) > 0.55f && h > 0.5f && u > 0.2f && u < 0.75f) ? 0.55f : 0.0f;
            return Mix(c, {0.25f, 0.28f, 0.3f}, bar);
        }
        default: // bar jack: blue-grey back with a dark stripe along it, silver flank
        {
            if (fin) return u > 0.8f ? Rgb{0.2f, 0.22f, 0.28f} : Rgb{0.55f, 0.58f, 0.62f};
            Rgb c = Mix({0.9f, 0.9f, 0.9f}, {0.38f, 0.45f, 0.55f}, (h - 0.45f) / 0.4f);
            return Mix(c, {0.12f, 0.14f, 0.2f}, Band(h, 0.82f, 0.9f));
        }
    }
}
} // namespace

FishSchools::~FishSchools()
{
    // Meshes and textures go with the renderer; nothing to release here explicitly.
}

void FishSchools::EnsureMeshes()
{
    if (_built)
        return;
    _built = true;
    EngineWgpu::AcquireProducerWindow("Fish meshes");
    _meshes.assign(size_t(SpeciesCount) * Lods * Phases, 0);
    _indexCounts.assign(_meshes.size(), 0);
    for (int s = 0; s < SpeciesCount; ++s)
    {
        float amp = 0.08f;
        for (const Behaviour& b : Behaviours)
            if (b.species == s)
                amp = b.amp;
        for (int l = 0; l < Lods; ++l)
            for (int p = 0; p < Phases; ++p)
            {
                MeshBuilder m;
                BuildFish(SpeciesTable[s], l, float(p) / Phases, amp, m);
                const size_t i = (size_t(s) * Lods + l) * Phases + p;
                _meshes[i] = wgr_mesh_create(_renderer, WgrSlice<WgrMeshVertex>(m.v), WgrSlice<uint32_t>(m.idx));
                _indexCounts[i] = uint32_t(m.idx.size());
            }
        constexpr int TW = 64, TH = 64;
        std::vector<uint8_t> px(TW * TH * 4);
        for (int y = 0; y < TH; ++y)
            for (int x = 0; x < TW; ++x)
            {
                const Rgb c = Skin(s, (x + 0.5f) / TW, (y + 0.5f) / TH);
                uint8_t* q = &px[(y * TW + x) * 4];
                q[0] = uint8_t(std::clamp(c.r, 0.0f, 1.0f) * 255);
                q[1] = uint8_t(std::clamp(c.g, 0.0f, 1.0f) * 255);
                q[2] = uint8_t(std::clamp(c.b, 0.0f, 1.0f) * 255);
                q[3] = 255;
            }
        _textures.push_back(wgr_texture_create(_renderer, TW, TH, WGR_TEXTURE_RGBA8, 1, WGR_TEXTURE_GEN_MIPS, px.data(),
                                               uint32_t(px.size())));
    }
    LOG_INFO(Graphics, "Fish: {} species x {} levels x {} swim phases baked", SpeciesCount, Lods, Phases);
}

bool FishSchools::SpawnGroup(const Vector3& camera)
{
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const float sea = Wildlife::SeaLevel(camera.X(), camera.Z());
    const float angle = Tau * unit(_rng), radius = 3.0f + 25.0f * unit(_rng);
    const Vector3 home(camera.X() + std::cos(angle) * radius, 0, camera.Z() + std::sin(angle) * radius);
    const float floor = Wildlife::Ground(home.X(), home.Z());
    const float depth = sea - floor;
    if (depth < 1.5f)
        return false;
    float total = 0;
    for (const Behaviour& b : Behaviours)
        if (depth >= b.bandMin && depth <= b.bandMax)
            total += b.weight;
    if (total <= 0)
        return false;
    float pick = unit(_rng) * total;
    int chosen = -1;
    for (int i = 0; i < BehaviourCount && chosen < 0; ++i)
    {
        const Behaviour& b = Behaviours[i];
        if (depth >= b.bandMin && depth <= b.bandMax)
        {
            pick -= b.weight;
            if (pick <= 0)
                chosen = i;
        }
    }
    if (chosen < 0)
        return false;
    const Behaviour& b = Behaviours[chosen];
    Group g;
    g.behaviour = chosen;
    g.home = home;
    g.wanderAngle = Tau * unit(_rng);
    const float ceiling = sea - 0.7f, column = std::max(ceiling - (floor + 0.3f), 0.2f);
    g.homeY = floor + 0.3f + column * (b.depthMin + (b.depthMax - b.depthMin) * unit(_rng));
    const int count = b.countMin + int(unit(_rng) * (b.countMax - b.countMin + 1));
    for (int i = 0; i < count; ++i)
    {
        Fish f;
        f.length = b.lenMin + (b.lenMax - b.lenMin) * unit(_rng);
        const float a = Tau * unit(_rng), r = std::min(b.homeRadius * 0.4f, 3.0f) * std::sqrt(unit(_rng));
        f.pos = Vector3(home.X() + std::cos(a) * r, g.homeY + (unit(_rng) - 0.5f) * 0.6f, home.Z() + std::sin(a) * r);
        const float h = Tau * unit(_rng);
        f.vel = Vector3(std::cos(h), 0, std::sin(h)) * (b.cruise * f.length);
        f.phase = unit(_rng);
        f.wander = unit(_rng);
        g.fish.push_back(f);
    }
    _groups.push_back(std::move(g));
    return true;
}

void FishSchools::Step(Group& g, const Vector3& camera, float dt)
{
    const Behaviour& b = Behaviours[g.behaviour];
    const float sea = Wildlife::SeaLevel(camera.X(), camera.Z());
    g.wanderAngle += dt * 0.08f;
    const Vector3 goal(g.home.X() + std::cos(g.wanderAngle) * b.homeRadius * 0.6f, g.homeY,
                       g.home.Z() + std::sin(g.wanderAngle) * b.homeRadius * 0.6f);
    // threats: the camera under the water, and the player when he is in it (Tidewater: the swimmer)
    Vector3 threats[2];
    int nThreats = 0;
    if (camera.Y() < sea + 0.3f)
        threats[nThreats++] = camera;
    if (GWorld && GWorld->GetRealPlayer())
    {
        const Vector3 p = GWorld->GetRealPlayer()->Position();
        if (p.Y() < sea + 0.5f)
            threats[nThreats++] = p + Vector3(0, 1.0f, 0);
    }
    const size_t n = g.fish.size();
    for (size_t i = 0; i < n; ++i)
    {
        Fish& f = g.fish[i];
        const float len = f.length;
        Vector3 sep(VZero), ali(VZero), coh(VZero);
        int nn = 0;
        const float sepR = b.sep * len, nbrR = b.nbr * len;
        for (size_t j = 0; j < n; ++j)
        {
            if (j == i)
                continue;
            const Vector3 d = f.pos - g.fish[j].pos;
            const float d2 = d.SquareSize();
            if (d2 < nbrR * nbrR)
            {
                ali += g.fish[j].vel;
                coh += g.fish[j].pos;
                ++nn;
                if (d2 < sepR * sepR && d2 > 1e-8f)
                    sep += d * (1.0f / d2);
            }
        }
        Vector3 steer(VZero);
        if (nn > 0)
        {
            ali = ali * (1.0f / nn);
            coh = coh * (1.0f / nn) - f.pos;
            steer += ali * b.wAli + coh * b.wCoh;
        }
        steer += sep * (b.wSep * len);
        Vector3 toGoal = goal - f.pos;
        steer += toGoal * (b.wGoal * 0.25f);
        bool fleeing = false;
        for (int t = 0; t < nThreats; ++t)
        {
            const Vector3 d = f.pos - threats[t];
            const float dist = d.Size();
            if (dist < b.flee && dist > 1e-4f)
            {
                steer += d * ((1.0f - dist / b.flee) * 8.0f / dist);
                fleeing = true;
            }
        }
        // the water column: above the seabed, below the surface
        const float floor = Wildlife::Ground(f.pos.X(), f.pos.Z()) + 0.18f + len * 0.5f;
        const float ceiling = sea - 0.7f;
        if (f.pos.Y() < floor + 0.4f)
            steer += Vector3(0, (floor + 0.4f - f.pos.Y()) * 3.0f, 0);
        if (f.pos.Y() > ceiling - 0.3f)
            steer -= Vector3(0, (f.pos.Y() - ceiling + 0.3f) * 3.0f, 0);
        // a fish swims mostly level
        steer[1] *= 0.6f;
        const float speed = (fleeing ? b.burst : b.cruise) * len;
        Vector3 desired = steer.SquareSize() > 1e-8f ? steer.Normalized() * speed : f.vel;
        Vector3 dv = desired - f.vel;
        const float maxDv = b.accel * len * dt * (fleeing ? 2.0f : 1.0f);
        if (dv.SquareSize() > maxDv * maxDv)
            dv = dv.Normalized() * maxDv;
        f.vel += dv;
        const float maxSpeed = (fleeing ? b.burst : b.max) * len;
        if (f.vel.SquareSize() > maxSpeed * maxSpeed)
            f.vel = f.vel.Normalized() * maxSpeed;
        f.pos += f.vel * dt;
        f.pos[1] = std::clamp(f.pos.Y(), std::min(floor, ceiling), std::max(ceiling, floor));
        f.phase += dt * (b.freq0 + b.freq1 * f.vel.Size() / len);
        f.phase -= std::floor(f.phase);
    }
}

void FishSchools::Update(const Vector3& camera, float time)
{
    static const bool enabled = []
    {
        const char* v = std::getenv("WGR_FISH");
        return !(v && v[0] == '0');
    }();
    if (!enabled || !GLandscape || !_renderer)
        return;
    const float sea = Wildlife::SeaLevel(camera.X(), camera.Z());
    // Only near the sea: no simulation for a camera high above it or far inland.
    if (camera.Y() > sea + 60.0f)
        return;
    EnsureMeshes();
    float dt = _lastTime < 0 ? 0.0f : time - _lastTime;
    _lastTime = time;
    dt = std::clamp(dt, 0.0f, 0.1f);

    // retire far groups, then fill up
    _groups.erase(std::remove_if(_groups.begin(), _groups.end(),
                                 [&](const Group& g)
                                 {
                                     const float dx = g.home.X() - camera.X(), dz = g.home.Z() - camera.Z();
                                     return dx * dx + dz * dz > SimRange * SimRange;
                                 }),
                  _groups.end());
    _spawnCooldown -= dt;
    if (int(_groups.size()) < Wildlife::Scaled(MaxGroups) && _spawnCooldown <= 0)
    {
        for (int attempt = 0; attempt < 4; ++attempt)
            if (SpawnGroup(camera))
                break;
        _spawnCooldown = 0.25f;
    }

    if (dt > 0)
    {
        const int sub = int(std::ceil(dt / (1.0f / 30.0f)));
        for (Group& g : _groups)
            for (int k = 0; k < sub; ++k)
                Step(g, camera, dt / sub);
    }

    static float nextLog = 0;
    const bool logNow = time >= nextLog;
    int drawn = 0, total = 0;
    float nearest = 1e9f;
    for (const Group& g : _groups)
    {
        const Behaviour& b = Behaviours[g.behaviour];
        for (const Fish& f : g.fish)
        {
            ++total;
            nearest = std::min(nearest, (f.pos - camera).Size());
            const Vector3 d = f.pos - camera;
            const float dist2 = d.SquareSize();
            if (dist2 > DrawRange * DrawRange)
                continue;
            const float dist = std::sqrt(dist2);
            // screen size decides the level of detail (Tidewater LOD_PX by length / distance)
            const float sizeRatio = f.length / std::max(dist, 0.1f);
            const int lod = sizeRatio > 0.06f ? 0 : sizeRatio > 0.015f ? 1 : 2;
            if (sizeRatio < 0.002f)
                continue;
            Vector3 fwd = f.vel.SquareSize() > 1e-6f ? f.vel.Normalized() : Vector3(0, 0, 1);
            fwd[1] = std::clamp(fwd.Y(), -0.5f, 0.5f);
            fwd.Normalize();
            Matrix3 o;
            o.SetDirectionAndUp(fwd, VUp);
            const float s = f.length;
            const Vector3 ax = o.DirectionAside() * s, up = o.DirectionUp() * s, dir = o.Direction() * s;
            const float world[16] = {ax.X(), ax.Y(), ax.Z(), 0, up.X(), up.Y(), up.Z(), 0,
                                     dir.X(), dir.Y(), dir.Z(), 0, f.pos.X(), f.pos.Y(), f.pos.Z(), 1};
            const int phase = std::clamp(int(f.phase * Phases), 0, Phases - 1);
            const size_t i = (size_t(SpeciesTable[b.species].name ? b.species : 0) * Lods + lod) * Phases + phase;
            _engine.SubmitDynamicMesh(_meshes[i], _indexCounts[i], world, _textures[b.species], 2);
            ++drawn;
        }
    }
    if (logNow)
    {
        nextLog = time + 5.0f;
        LOG_INFO(Graphics, "Fish: {} groups, {} fish, {} drawn, nearest {:.1f} m, camera {:.1f} m from sea level",
                 _groups.size(), total, drawn, nearest, camera.Y() - sea);
    }
}
} // namespace Poseidon
