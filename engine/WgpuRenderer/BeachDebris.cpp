// SPDX-License-Identifier: GPL-3.0-or-later
//
// Beach debris around the camera: the Tidewater wrack line and shore clutter (dgreenheck/tidewater,
// MIT, (c) DRG Software Solutions LLC; see CREDITS.md). From src/world/debris/:
//   DebrisShapes.js     the procedural shapes: shells (a ribbed scallop fan, a spiral whelk),
//                       stones (perturbed icospheres), driftwood (a bent, knotted taper tube),
//                       weed mats (a lumpy flat mound);
//   DebrisPlacement.js  where they lie: the high-water band collects the wrack (weed, driftwood,
//                       shells), the upper beach holds scattered shells and pebbles, the wet lower
//                       beach mostly pebbles and a few shells;
//   PebbleField.js      the camera-following field: cells near the viewer, positions hashed from
//                       the world so nothing moves while the camera does, shrinking out with distance.
//
// What OP changes: OFP has no beach mask, so the band is read from the height above the sea and
// the ground's surface (Wildlife::Sand); the shapes are baked once (three variants each) and drawn
// as dynamic meshes, like the crabs (BeachCritters.cpp). The tropical items (coconuts, palm fronds,
// coral, litter) are left out: OFP's coasts are temperate. WGR_BEACH_DEBRIS=0 turns it off.

#include "BeachDebris.hpp"
#include "EngineWgpu.hpp"
#include "WildlifeWorld.hpp"
#include <Poseidon/Core/Global.hpp>
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
constexpr float CellSize = 6.0f, Active = 60.0f;
constexpr int Variants = 3;
constexpr size_t MaxDrawn = 900;
// how far each kind is drawn (it shrinks out over the last quarter)
constexpr float DrawRange[BeachDebris::KindCount] = {22.0f, 22.0f, 26.0f, 60.0f, 45.0f};

struct DV
{
    float x = 0, y = 0, z = 0;
};
DV operator+(DV a, DV b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
DV operator-(DV a, DV b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
DV operator*(DV a, float s) { return {a.x * s, a.y * s, a.z * s}; }
DV Cross(DV a, DV b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Len(DV a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
DV Norm(DV a)
{
    const float l = Len(a);
    return l > 1e-9f ? a * (1.0f / l) : DV{0, 1, 0};
}
float Clamp(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
float Smooth(float a, float b, float x)
{
    const float t = Clamp((x - a) / (b - a), 0, 1);
    return t * t * (3 - 2 * t);
}
uint32_t Hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float Hash01(int i, int j, int k, int s)
{
    return (Hash(uint32_t(i) * 73856093u ^ uint32_t(j) * 19349663u ^ uint32_t(k) * 83492791u ^ uint32_t(s) * 2654435761u) >> 8) /
           16777216.0f;
}
// smooth value noise (DebrisShapes.js vnoise3)
float Noise3(float x, float y, float z, int s)
{
    const int ix = int(std::floor(x)), iy = int(std::floor(y)), iz = int(std::floor(z));
    const float fx = x - ix, fy = y - iy, fz = z - iz;
    const float ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy), uz = fz * fz * (3 - 2 * fz);
    auto h = [&](int a, int b, int c) { return Hash01(ix + a, iy + b, iz + c, s); };
    const float x00 = h(0, 0, 0) + (h(1, 0, 0) - h(0, 0, 0)) * ux, x10 = h(0, 1, 0) + (h(1, 1, 0) - h(0, 1, 0)) * ux;
    const float x01 = h(0, 0, 1) + (h(1, 0, 1) - h(0, 0, 1)) * ux, x11 = h(0, 1, 1) + (h(1, 1, 1) - h(0, 1, 1)) * ux;
    const float y0 = x00 + (x10 - x00) * uy, y1 = x01 + (x11 - x01) * uy;
    return y0 + (y1 - y0) * uz;
}

struct MeshOut
{
    std::vector<WgrMeshVertex> v;
    std::vector<uint32_t> idx;
};
uint32_t AddVertex(MeshOut& m, DV p, float u, float tv)
{
    WgrMeshVertex w{};
    w.pos = {p.x, p.y, p.z};
    w.uv = {u, tv};
    w.uv1 = w.uv;
    m.v.push_back(w);
    return uint32_t(m.v.size() - 1);
}
// smooth normals from the triangles
void Normals(MeshOut& m)
{
    std::vector<DV> n(m.v.size());
    for (size_t i = 0; i + 2 < m.idx.size(); i += 3)
    {
        const auto P = [&](uint32_t k) { return DV{m.v[k].pos.x, m.v[k].pos.y, m.v[k].pos.z}; };
        const DV f = Cross(P(m.idx[i + 1]) - P(m.idx[i]), P(m.idx[i + 2]) - P(m.idx[i]));
        for (int c = 0; c < 3; ++c)
            n[m.idx[i + c]] = n[m.idx[i + c]] + f;
    }
    for (size_t k = 0; k < m.v.size(); ++k)
    {
        const DV q = Norm(n[k]);
        m.v[k].normal = {q.x, q.y, q.z};
    }
}
// a (cols+1) x (rows+1) grid of fn(u, v), two triangles a quad (wound so +y faces out for a mound)
template <class F>
void Grid(MeshOut& m, int cols, int rows, F fn, bool flip = false)
{
    const uint32_t base = uint32_t(m.v.size());
    for (int j = 0; j <= rows; ++j)
        for (int i = 0; i <= cols; ++i)
        {
            const float u = float(i) / cols, v = float(j) / rows;
            AddVertex(m, fn(u, v), u, v);
        }
    for (int j = 0; j < rows; ++j)
        for (int i = 0; i < cols; ++i)
        {
            const uint32_t a = base + j * (cols + 1) + i, b = a + 1, c = a + cols + 1, d = c + 1;
            if (!flip)
                m.idx.insert(m.idx.end(), {a, c, b, b, c, d});
            else
                m.idx.insert(m.idx.end(), {a, b, c, b, d, c});
        }
}

// DebrisShapes.js shell(): a ribbed scallop fan, lying dome up, 1 across (z -0.5..0.5)
MeshOut BuildScallop(int variant)
{
    MeshOut m;
    const float span = 2.2f + 0.25f * variant, ribs = 9.0f + 3.0f * variant;
    auto top = [&](float u, float v)
    {
        const float a = (u - 0.5f) * span, r = 0.08f + 0.92f * v;
        const float dome = 0.2f * std::pow(std::max(0.0f, 1.0f - std::pow(2.0f * v - 0.9f, 2.0f)), 0.6f) * std::cos(a * 0.45f);
        const float rib = 0.022f * v * std::pow(std::fabs(std::cos(a * ribs * 0.5f)), 0.6f);
        // the scalloped rim
        const float rim = 1.0f + 0.03f * std::cos(a * ribs) * Smooth(0.85f, 1.0f, v);
        return DV{r * rim * std::sin(a), (dome + rib) * Smooth(0.0f, 0.12f, v) * Smooth(1.02f, 0.9f, v), r * rim * std::cos(a) - 0.5f};
    };
    Grid(m, 20, 10, top);
    Grid(m, 20, 10, [&](float u, float v) { DV p = top(u, v); return DV{p.x, 0.0f, p.z}; }, true);
    Normals(m);
    return m;
}
// a spiral whelk: a ridged cone along x, lying on its side
MeshOut BuildWhelk(int variant)
{
    MeshOut m;
    const float r0 = 0.3f + 0.04f * variant, turns = 4.0f + variant;
    auto rad = [&](float t) { return r0 * std::pow(1.0f - t, 0.85f) + 0.025f; };
    Grid(m, 12, 18, [&](float u, float v)
    {
        const float th = u * Tau, t = v;
        const float r = rad(t) * (1.0f + 0.1f * std::pow(std::fabs(std::sin(t * turns * Pi + th * 0.5f)), 3.0f));
        return DV{t - 0.5f, r * std::cos(th) + r0, r * std::sin(th)};
    }, true);
    // the aperture: a disc at the wide end
    const uint32_t c = AddVertex(m, DV{-0.5f, r0, 0}, 0.5f, 0.0f);
    for (int i = 0; i < 12; ++i)
    {
        const float a = Tau * i / 12, b = Tau * (i + 1) / 12;
        const uint32_t p = AddVertex(m, DV{-0.5f, rad(0) * std::cos(a) + r0, rad(0) * std::sin(a)}, 0.5f, 0.02f);
        const uint32_t q = AddVertex(m, DV{-0.5f, rad(0) * std::cos(b) + r0, rad(0) * std::sin(b)}, 0.5f, 0.02f);
        m.idx.insert(m.idx.end(), {c, q, p});
    }
    Normals(m);
    return m;
}
// a beach pebble: a noisy, flattened sphere resting on its flat side
MeshOut BuildPebble(int variant)
{
    MeshOut m;
    const float flat = 0.5f + 0.12f * variant;
    Grid(m, 14, 10, [&](float u, float v)
    {
        const float th = u * Tau, ph = v * Pi;
        DV d{std::sin(ph) * std::cos(th), std::cos(ph), std::sin(ph) * std::sin(th)};
        const float r = 0.5f * (1.0f + 0.16f * (Noise3(d.x * 1.8f + variant * 7, d.y * 1.8f, d.z * 1.8f, 11) - 0.5f) * 2.0f +
                                0.06f * (Noise3(d.x * 4.3f, d.y * 4.3f + variant * 3, d.z * 4.3f, 23) - 0.5f) * 2.0f);
        return DV{d.x * r * (1.0f + 0.15f * variant), d.y * r * flat + 0.5f * flat * 0.85f, d.z * r};
    }, true);
    Normals(m);
    return m;
}
// driftwood: a bent, knotted taper tube along x (-0.5..0.5), unit radius, lying on the ground
MeshOut BuildDriftwood(int variant)
{
    MeshOut m;
    auto centre = [&](float t) { return DV{t - 0.5f, 0.6f * std::sin(Pi * t) * (0.3f + 0.2f * variant), 2.0f * std::sin(Tau * t + variant)}; };
    auto radius = [&](float t, float th)
    {
        const float knot = 0.25f * Smooth(0.75f, 1.0f, Noise3(t * 6.0f + variant * 13, th, 0, 31));
        return (1.0f - 0.4f * t) * (1.0f + 0.1f * (Noise3(t * 9.0f, th * 1.5f, variant, 37) - 0.5f) + knot);
    };
    Grid(m, 8, 16, [&](float u, float v)
    {
        const float th = u * Tau, t = v;
        const DV c = centre(t);
        const float r = radius(t, th);
        return DV{c.x, c.y + r * std::cos(th) + 1.0f, c.z + r * std::sin(th)};
    }, true);
    for (int end = 0; end < 2; ++end)
    {
        const float t = float(end);
        const DV c = centre(t);
        const uint32_t ci = AddVertex(m, DV{c.x, c.y + 1.0f, c.z}, 0.5f, t);
        for (int i = 0; i < 8; ++i)
        {
            const float a = Tau * i / 8, b = Tau * (i + 1) / 8;
            const uint32_t p = AddVertex(m, DV{c.x, c.y + radius(t, a) * std::cos(a) + 1.0f, c.z + radius(t, a) * std::sin(a)}, 0.5f, t);
            const uint32_t q = AddVertex(m, DV{c.x, c.y + radius(t, b) * std::cos(b) + 1.0f, c.z + radius(t, b) * std::sin(b)}, 0.5f, t);
            if (end == 0)
                m.idx.insert(m.idx.end(), {ci, q, p});
            else
                m.idx.insert(m.idx.end(), {ci, p, q});
        }
    }
    Normals(m);
    return m;
}
// a weed mat: a lumpy, ragged flat mound of unit radius, a few centimetres high
MeshOut BuildWrack(int variant)
{
    MeshOut m;
    Grid(m, 24, 5, [&](float u, float v)
    {
        const float th = u * Tau;
        const float edge = 0.65f + 0.35f * Noise3(std::cos(th) * 2.0f + variant * 5, std::sin(th) * 2.0f, 0, 41);
        const float r = v * edge;
        const float h = 0.06f * (1.0f - v * v) * (0.5f + Noise3(std::cos(th) * r * 4.0f, std::sin(th) * r * 4.0f, variant, 43));
        return DV{r * std::cos(th), h, r * std::sin(th)};
    }, true);
    Normals(m);
    return m;
}

struct Rgb
{
    float r, g, b;
};
Rgb Mix(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
// the texture of one kind and variant (sRGB), at uv
Rgb Colour(int kind, int variant, float u, float v)
{
    const float n = Noise3(u * 16.0f, v * 16.0f, float(kind * 3 + variant), 51);
    const float f = Noise3(u * 48.0f, v * 48.0f, float(kind * 3 + variant), 53);
    switch (kind)
    {
    case BeachDebris::Scallop:
    {
        static const Rgb base[Variants] = {{0.93f, 0.88f, 0.79f}, {0.88f, 0.62f, 0.5f}, {0.76f, 0.6f, 0.44f}};
        // growth bands across the fan, a paler hinge
        const float band = 0.5f + 0.5f * std::sin(v * 44.0f + n * 3.0f);
        Rgb c = Mix(base[variant], Mix(base[variant], {0.55f, 0.38f, 0.28f}, 0.5f), band * 0.45f * Smooth(0.15f, 0.4f, v));
        return Mix(c, {0.97f, 0.95f, 0.9f}, Smooth(0.2f, 0.0f, v) * 0.6f + (f - 0.5f) * 0.1f);
    }
    case BeachDebris::Whelk:
    {
        static const Rgb base[Variants] = {{0.84f, 0.76f, 0.64f}, {0.74f, 0.66f, 0.56f}, {0.9f, 0.84f, 0.74f}};
        const float spiral = 0.5f + 0.5f * std::sin(v * 60.0f + u * 6.0f);
        return Mix(base[variant], {0.5f, 0.36f, 0.24f}, spiral * 0.35f + (n - 0.5f) * 0.2f);
    }
    case BeachDebris::Pebble:
    {
        static const Rgb base[Variants] = {{0.56f, 0.54f, 0.51f}, {0.43f, 0.42f, 0.41f}, {0.7f, 0.65f, 0.58f}};
        const float speck = f > 0.72f ? 0.25f : 0.0f;
        const Rgb c = Mix(base[variant], {base[variant].r * 0.75f, base[variant].g * 0.75f, base[variant].b * 0.75f}, n * 0.6f);
        return Mix(c, {0.86f, 0.85f, 0.82f}, speck);
    }
    case BeachDebris::Driftwood:
    {
        static const Rgb base[Variants] = {{0.66f, 0.63f, 0.58f}, {0.58f, 0.55f, 0.5f}, {0.72f, 0.68f, 0.6f}};
        // silver, sea-bleached wood with dark grain along its length
        const float grain = std::pow(std::fabs(std::sin(u * 40.0f + n * 6.0f)), 8.0f);
        return Mix(base[variant], {0.32f, 0.29f, 0.25f}, grain * 0.5f + (f - 0.5f) * 0.15f);
    }
    default:
    {
        static const Rgb base[Variants] = {{0.2f, 0.18f, 0.09f}, {0.4f, 0.35f, 0.21f}, {0.26f, 0.2f, 0.11f}};
        // wrack: kelp and wrack weed, eelgrass straw, sun-dried tips
        return Mix(base[variant], {0.33f, 0.36f, 0.16f}, Smooth(0.55f, 0.8f, n) * 0.5f + (f - 0.5f) * 0.25f);
    }
    }
}
} // namespace

void BeachDebris::EnsureMeshes()
{
    if (_built)
        return;
    _built = true;
    EngineWgpu::AcquireProducerWindow("Beach debris meshes");
    _meshes.assign(KindCount * Variants, 0);
    _indexCounts.assign(_meshes.size(), 0);
    for (int k = 0; k < KindCount; ++k)
        for (int v = 0; v < Variants; ++v)
        {
            const MeshOut m = k == Scallop ? BuildScallop(v) : k == Whelk ? BuildWhelk(v) : k == Pebble ? BuildPebble(v)
                              : k == Driftwood ? BuildDriftwood(v) : BuildWrack(v);
            const size_t i = size_t(k) * Variants + v;
            _meshes[i] = wgr_mesh_create(_renderer, WgrSlice<WgrMeshVertex>(m.v), WgrSlice<uint32_t>(m.idx));
            _indexCounts[i] = uint32_t(m.idx.size());
            constexpr int TW = 64, TH = 64;
            std::vector<uint8_t> px(TW * TH * 4);
            for (int y = 0; y < TH; ++y)
                for (int x = 0; x < TW; ++x)
                {
                    const Rgb c = Colour(k, v, (x + 0.5f) / TW, (y + 0.5f) / TH);
                    uint8_t* q = &px[(y * TW + x) * 4];
                    q[0] = uint8_t(Clamp(c.r, 0, 1) * 255), q[1] = uint8_t(Clamp(c.g, 0, 1) * 255), q[2] = uint8_t(Clamp(c.b, 0, 1) * 255);
                    q[3] = 255;
                }
            _textures.push_back(wgr_texture_create(_renderer, TW, TH, WGR_TEXTURE_RGBA8, 1, 0, px.data(), uint32_t(px.size())));
        }
    LOG_INFO(Graphics, "Beach debris: shells, pebbles, driftwood and wrack baked ({} variants each)", Variants);
}

// DebrisPlacement.js, for OFP's shores: per 6 m cell, what lies there, by the band of beach it is in
const std::vector<BeachDebris::Item>& BeachDebris::Cell(int cx, int cz)
{
    const int64_t key = (int64_t(cx) << 32) ^ int64_t(uint32_t(cz));
    auto it = _cells.find(key);
    if (it != _cells.end())
        return it->second;
    std::vector<Item> items;
    int n = 0;
    auto rnd = [&] { return Hash01(cx, cz, n++, 977); };
    for (int i = 0; i < 16; ++i)
    {
        const float x = (cx + rnd()) * CellSize, z = (cz + rnd()) * CellSize;
        const float pick = rnd(), size = rnd(), yaw = rnd() * Tau, var = rnd();
        const float g = Wildlife::Ground(x, z), sea = Wildlife::SeaLevel(x, z), e = g - sea;
        if (e < -0.15f || e > 3.5f)
            continue;
        const float slope = std::max(std::fabs(Wildlife::Ground(x + 1, z) - g), std::fabs(Wildlife::Ground(x, z + 1) - g));
        if (slope > 0.3f)
            continue;
        const bool sand = Wildlife::Sand(x, z);
        // the high-water band collects the wrack; above it scattered shells and pebbles; below, on
        // the wet sand, pebbles and a few shells; a rocky shore keeps only its stones
        const bool wrackBand = e > 0.5f && e < 1.8f;
        float keep = !sand ? 0.25f : wrackBand ? 0.75f : e < 0.5f ? 0.35f : 0.45f;
        if (rnd() > keep)
            continue;
        Kind kind = Pebble;
        if (sand)
        {
            if (wrackBand)
                kind = pick < 0.12f ? Wrack : pick < 0.18f ? Driftwood : pick < 0.5f ? Scallop : pick < 0.64f ? Whelk : Pebble;
            else if (e >= 1.8f)
                kind = pick < 0.2f ? Scallop : pick < 0.28f ? Whelk : pick < 0.33f ? Driftwood : pick < 0.36f ? Wrack : Pebble;
            else
                kind = pick < 0.25f ? Scallop : pick < 0.3f ? Whelk : Pebble;
        }
        Item it{};
        it.x = x, it.z = z, it.yaw = yaw, it.kind = uint8_t(kind), it.variant = uint8_t(std::min(int(var * Variants), Variants - 1));
        switch (kind)
        {
        case Scallop: it.sx = it.sy = it.sz = 0.035f + 0.04f * size; break;
        case Whelk: it.sx = it.sy = it.sz = 0.04f + 0.045f * size; break;
        case Pebble:
        {
            const float s = size < 0.8f ? 0.025f + 0.07f * size : 0.12f + 0.5f * (size - 0.8f);
            it.sx = it.sy = it.sz = s;
            break;
        }
        case Driftwood:
        {
            const float r = 0.035f + 0.08f * size;
            it.sx = 0.6f + 1.8f * rnd(), it.sy = it.sz = r;
            break;
        }
        default: it.sx = it.sz = 0.25f + 0.55f * size, it.sy = 1.0f; break;
        }
        // seated a little into the sand
        it.y = g - 0.12f * it.sy;
        items.push_back(it);
    }
    if (_cells.size() > 6000)
        _cells.clear();
    return _cells.emplace(key, std::move(items)).first->second;
}

void BeachDebris::Update(const Vector3& camera)
{
    static const bool enabled = []
    {
        const char* v = std::getenv("WGR_BEACH_DEBRIS");
        return !(v && v[0] == '0');
    }();
    if (!enabled || !GLandscape || !_renderer)
        return;
    const float ground = Wildlife::Ground(camera.X(), camera.Z());
    if (camera.Y() - std::max(ground, Wildlife::SeaLevel(camera.X(), camera.Z())) > 60.0f)
        return;
    const int r = int(std::ceil(Active / CellSize));
    const int ccx = int(std::floor(camera.X() / CellSize)), ccz = int(std::floor(camera.Z() / CellSize));
    _near.clear();
    for (int dz = -r; dz <= r; ++dz)
        for (int dx = -r; dx <= r; ++dx)
            for (const Item& it : Cell(ccx + dx, ccz + dz))
            {
                const float d = std::hypot(it.x - camera.X(), it.z - camera.Z());
                if (d < DrawRange[it.kind])
                    _near.emplace_back(d, &it);
            }
    if (_near.empty())
        return;
    EnsureMeshes();
    if (_near.size() > MaxDrawn)
    {
        std::nth_element(_near.begin(), _near.begin() + MaxDrawn, _near.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        _near.resize(MaxDrawn);
    }
    for (const auto& [d, it] : _near)
    {
        const float range = DrawRange[it->kind];
        const float k = Smooth(range, range * 0.75f, d);
        if (k <= 0.01f)
            continue;
        const float c = std::cos(it->yaw), s = std::sin(it->yaw);
        const float sx = it->sx * k, sy = it->sy * k, sz = it->sz * k;
        const float world[16] = {c * sx, 0, -s * sx, 0, 0, sy, 0, 0, s * sz, 0, c * sz, 0, it->x, it->y, it->z, 1};
        const size_t i = size_t(it->kind) * Variants + it->variant;
        _engine.SubmitDynamicMesh(_meshes[i], _indexCounts[i], world, _textures[i], 2);
    }
}
} // namespace Poseidon
