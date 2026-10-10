// SPDX-License-Identifier: GPL-3.0-or-later
//
// Life on the sea floor around the camera: stage 3 of porting Tidewater's wildlife
// (dgreenheck/tidewater, MIT, (c) DRG Software Solutions LLC; see CREDITS.md). From
// src/world/reef/ReefGeometry.js: the procedural builders (tube, lathe, icosphere blob, sheet,
// path, branch, waveNoise, smoothNormals, backfaces) and the organisms -- seagrass meadow
// (createMeadow), sargassum (createSargassum), common sea fan (createSeaFan), sea rod
// (createSeaRod), brain coral dome (createDome), star coral boulder (createMound 'boulder'),
// giant barrel sponge (createBarrel), tube sponges (createTubes), urchin (createUrchin),
// giant anemone (createAnemone) and coral rubble (createRubble); from src/world/Reef.js the
// species' sizes and colour palettes (c1 / c2).
//
// What OP changes: no authored reef -- items are placed deterministically per 6 m cell wherever
// OFP's sea is between 0.8 and 25 m deep, the species mix by depth; the colour comes from the
// palettes as a small per-species texture (height along the organism x variant) instead of
// Tidewater's procedural surface shaders; the surge sway of the flexible ones (meadow, algae,
// fans, rods, anemones) is baked into 8 phases instead of displaced in a vertex shader.

#include "ReefLife.hpp"
#include "EngineWgpu.hpp"
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <random>

namespace Poseidon
{
namespace
{
constexpr float Tau = 6.2831853f, Pi = 3.14159265f;
constexpr int Variants = 4, Lods = 2, Sway = 8;
constexpr float CellSize = 6.0f, DrawRange = 46.0f;

struct RV
{
    float x = 0, y = 0, z = 0;
};
RV operator+(RV a, RV b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
RV operator-(RV a, RV b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
RV operator*(RV a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(RV a, RV b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
RV Cross(RV a, RV b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Len(RV a) { return std::sqrt(Dot(a, a)); }
RV Norm(RV a)
{
    const float l = Len(a);
    return l > 1e-9f ? a * (1.0f / l) : RV{0, 1, 0};
}
float Clamp(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
float Smooth(float a, float b, float x)
{
    const float t = Clamp((x - a) / (b - a), 0, 1);
    return t * t * (3 - 2 * t);
}
// rotate v about the unit axis by angle (Rodrigues)
RV Rotate(RV v, RV k, float a)
{
    const float c = std::cos(a), s = std::sin(a);
    return v * c + Cross(k, v) * s + k * (Dot(k, v) * (1 - c));
}
RV Perpendicular(RV v)
{
    return Norm(Cross(v, std::fabs(v.y) < 0.9f ? RV{0, 1, 0} : RV{1, 0, 0}));
}

struct Rng
{
    std::mt19937 g;
    explicit Rng(uint32_t seed) : g(seed) {}
    float operator()() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(g); }
    float Rand(float a, float b) { return a + (b - a) * (*this)(); }
};
RV RandomUnit(Rng& rng)
{
    const float z = rng() * 2 - 1, a = rng() * Tau, r = std::sqrt(std::max(0.0f, 1 - z * z));
    return {r * std::cos(a), z, r * std::sin(a)};
}
// ReefGeometry.js waveNoise: a sum of random plane waves
std::function<float(float, float, float)> WaveNoise(Rng& rng, int count, float fMin, float fMax)
{
    std::vector<float> w;
    float norm = 0;
    for (int i = 0; i < count; ++i)
    {
        const RV d = RandomUnit(rng) * rng.Rand(fMin, fMax);
        const float a = 1.0f / (1 + i * 0.5f);
        norm += a * a;
        w.insert(w.end(), {d.x, d.y, d.z, rng() * Tau, a});
    }
    norm = 1.0f / std::sqrt(norm * 0.5f);
    return [w, norm](float x, float y, float z)
    {
        float s = 0;
        for (size_t i = 0; i < w.size(); i += 5)
            s += std::sin(x * w[i] + y * w[i + 1] + z * w[i + 2] + w[i + 3]) * w[i + 4];
        return s * norm;
    };
}

// ReefGeometry.js MeshBuilder: aData = (t along the organism, -, flag, random)
struct Builder
{
    std::vector<RV> p, n;
    std::vector<std::array<float, 3>> d; // t, flag, random
    std::vector<uint32_t> idx;
    uint32_t Count() const { return uint32_t(p.size()); }
    uint32_t Vertex(RV pos, RV nor, float t, float flag, float w)
    {
        p.push_back(pos), n.push_back(nor), d.push_back({t, flag, w});
        return Count() - 1;
    }
    void Tri(uint32_t a, uint32_t b, uint32_t c) { idx.insert(idx.end(), {a, b, c}); }
    void Quad(uint32_t a, uint32_t b, uint32_t c, uint32_t e) { idx.insert(idx.end(), {a, b, c, a, c, e}); }
    // a copy of the triangles in [first, end) facing the other way (thin sheets)
    void Backfaces(uint32_t firstVertex, size_t firstIndex)
    {
        const uint32_t nv = Count();
        const size_t ni = idx.size();
        std::map<uint32_t, uint32_t> map;
        for (uint32_t v = firstVertex; v < nv; ++v)
            map[v] = Vertex(p[v], n[v] * -1.0f, d[v][0], d[v][1], d[v][2]);
        for (size_t i = firstIndex; i < ni; i += 3)
            Tri(map[idx[i]], map[idx[i + 2]], map[idx[i + 1]]);
    }
    void SmoothNormals(uint32_t first = 0, size_t firstIndex = 0)
    {
        for (uint32_t v = first; v < Count(); ++v)
            n[v] = {0, 0, 0};
        for (size_t i = firstIndex; i + 2 < idx.size(); i += 3)
        {
            const RV c = Cross(p[idx[i + 1]] - p[idx[i]], p[idx[i + 2]] - p[idx[i]]);
            for (int k = 0; k < 3; ++k)
                if (idx[i + k] >= first)
                    n[idx[i + k]] = n[idx[i + k]] + c;
        }
        for (uint32_t v = first; v < Count(); ++v)
            n[v] = Norm(n[v]);
    }
};

using DataFn = std::function<std::array<float, 3>(int, float)>;
// ReefGeometry.js tube: a swept ring along a polyline, optionally capped
void Tube(Builder& b, const std::vector<RV>& pts, const std::vector<float>& radii, int sides, bool cap, const DataFn& data)
{
    const int n = int(pts.size());
    std::vector<float> s(n, 0);
    for (int i = 1; i < n; ++i)
        s[i] = s[i - 1] + Len(pts[i] - pts[i - 1]);
    const float L = s[n - 1] > 0 ? s[n - 1] : 1;
    std::vector<RV> T(n), N(n), B(n);
    for (int i = 0; i < n; ++i)
        T[i] = Norm(pts[std::min(n - 1, i + 1)] - pts[std::max(0, i - 1)]);
    RV prev{};
    bool havePrev = false;
    for (int i = 0; i < n; ++i)
    {
        const RV hint = havePrev ? prev : Perpendicular(T[i]);
        RV nn = hint - T[i] * Dot(hint, T[i]);
        if (Dot(nn, nn) < 1e-8f)
            nn = Perpendicular(T[i]);
        nn = Norm(nn);
        N[i] = nn, B[i] = Cross(T[i], nn), prev = nn, havePrev = true;
    }
    const uint32_t base = b.Count();
    for (int i = 0; i < n; ++i)
    {
        const float r = radii[i];
        const int i0 = std::max(0, i - 1), i1 = std::min(n - 1, i + 1);
        const float slope = (radii[i0] - radii[i1]) / std::max(1e-5f, s[i1] - s[i0]);
        const auto dd = data(i, s[i] / L);
        for (int j = 0; j < sides; ++j)
        {
            const float a = float(j) / sides * Tau, ca = std::cos(a), sa = std::sin(a);
            const RV pos = pts[i] + N[i] * (ca * r) + B[i] * (sa * r);
            const RV nor = Norm(Norm(N[i] * ca + B[i] * sa) + T[i] * slope);
            b.Vertex(pos, nor, dd[0], dd[1], dd[2]);
        }
    }
    for (int i = 0; i < n - 1; ++i)
        for (int j = 0; j < sides; ++j)
        {
            const uint32_t a = base + i * sides + j, a1 = base + i * sides + (j + 1) % sides;
            b.Tri(a, a1, a + sides);
            b.Tri(a1, a1 + sides, a + sides);
        }
    if (cap)
    {
        const int i = n - 1;
        const float r = radii[i];
        const auto dd = data(i, 1);
        const uint32_t ring = base + i * sides, mid = b.Count();
        for (int j = 0; j < sides; ++j)
        {
            const float a = float(j) / sides * Tau, ca = std::cos(a), sa = std::sin(a);
            const RV pos = pts[i] + T[i] * (r * 0.95f * 0.63f) + N[i] * (ca * r * 0.75f) + B[i] * (sa * r * 0.75f);
            b.Vertex(pos, Norm(Norm(N[i] * ca + B[i] * sa) * 0.7f + T[i] * 0.72f), dd[0], dd[1], dd[2]);
        }
        for (int j = 0; j < sides; ++j)
        {
            const uint32_t j1 = (j + 1) % sides;
            b.Tri(ring + j, ring + j1, mid + j);
            b.Tri(ring + j1, mid + j1, mid + j);
        }
        const uint32_t tip = b.Vertex(pts[i] + T[i] * (r * 0.95f), T[i], dd[0], dd[1], dd[2]);
        for (int j = 0; j < sides; ++j)
            b.Tri(mid + j, mid + (j + 1) % sides, tip);
    }
}

struct ProfilePt
{
    float r, y, t, flag, w;
};
// ReefGeometry.js lathe: a surface of revolution about +y
void Lathe(Builder& b, const std::vector<ProfilePt>& prof, int segments, const std::function<float(float, int)>& radial,
           float ox = 0, float oz = 0)
{
    const uint32_t base = b.Count();
    for (size_t i = 0; i < prof.size(); ++i)
        for (int j = 0; j < segments; ++j)
        {
            const float phi = float(j) / segments * Tau;
            const float k = radial ? radial(phi, int(i)) : 1.0f;
            b.Vertex({ox + prof[i].r * k * std::cos(phi), prof[i].y, oz - prof[i].r * k * std::sin(phi)}, {0, 1, 0}, prof[i].t,
                     prof[i].flag, prof[i].w);
        }
    for (size_t i = 0; i + 1 < prof.size(); ++i)
        for (int j = 0; j < segments; ++j)
        {
            const uint32_t a = base + uint32_t(i * segments + j), a1 = base + uint32_t(i * segments + (j + 1) % segments);
            b.Tri(a, a1, a + segments);
            b.Tri(a1, a1 + segments, a + segments);
        }
}

// ReefGeometry.js icosphere / blob: a deformed icosphere
struct Ico
{
    std::vector<RV> v;
    std::vector<std::array<uint32_t, 3>> f;
};
const Ico& Icosphere(int detail)
{
    static std::map<int, Ico> cache;
    auto it = cache.find(detail);
    if (it != cache.end())
        return it->second;
    const float t = (1 + std::sqrt(5.0f)) / 2;
    Ico s;
    for (auto q : std::vector<RV>{{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t},
                                  {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}})
        s.v.push_back(Norm(q));
    s.f = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
           {3, 9, 4},  {3, 4, 2}, {3, 2, 6}, {3, 6, 8},  {3, 8, 9},   {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
    for (int dd = 0; dd < detail; ++dd)
    {
        std::map<uint64_t, uint32_t> mids;
        auto mid = [&](uint32_t a, uint32_t b)
        {
            const uint64_t key = a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a;
            auto m = mids.find(key);
            if (m != mids.end())
                return m->second;
            s.v.push_back(Norm(s.v[a] + s.v[b]));
            return mids[key] = uint32_t(s.v.size() - 1);
        };
        std::vector<std::array<uint32_t, 3>> next;
        for (auto [a, b, c] : s.f)
        {
            const uint32_t ab = mid(a, b), bc = mid(b, c), ca = mid(c, a);
            next.push_back({a, ab, ca}), next.push_back({b, bc, ab}), next.push_back({c, ca, bc}), next.push_back({ab, bc, ca});
        }
        s.f = next;
    }
    for (auto& f : s.f)
        if (Dot(Cross(s.v[f[1]] - s.v[f[0]], s.v[f[2]] - s.v[f[0]]), s.v[f[0]]) < 0)
            std::swap(f[1], f[2]);
    return cache[detail] = s;
}
void Blob(Builder& b, int detail, const std::function<std::array<float, 3>(RV, RV&)>& fn)
{
    const Ico& ico = Icosphere(detail);
    const uint32_t base = b.Count();
    for (const RV& v : ico.v)
    {
        RV out;
        const auto dd = fn(v, out);
        b.Vertex(out, v, dd[0], dd[1], dd[2]);
    }
    for (const auto& f : ico.f)
        b.Tri(base + f[0], base + f[1], base + f[2]);
}
// ReefGeometry.js sheet: a grid over (u, v)
void Sheet(Builder& b, int nu, int nv, const std::function<std::array<float, 3>(float, float, RV&)>& fn, bool twoSided)
{
    const uint32_t first = b.Count();
    const size_t firstIndex = b.idx.size();
    for (int j = 0; j <= nv; ++j)
        for (int i = 0; i <= nu; ++i)
        {
            RV out;
            const auto dd = fn(float(i) / nu, float(j) / nv, out);
            b.Vertex(out, {0, 1, 0}, dd[0], dd[1], dd[2]);
        }
    for (int j = 0; j < nv; ++j)
        for (int i = 0; i < nu; ++i)
        {
            const uint32_t a = first + j * (nu + 1) + i;
            b.Quad(a, a + 1, a + nu + 2, a + nu + 1);
        }
    auto at = [&](int i, int j) { return first + std::clamp(j, 0, nv) * (nu + 1) + std::clamp(i, 0, nu); };
    for (int j = 0; j <= nv; ++j)
        for (int i = 0; i <= nu; ++i)
        {
            const RV t = b.p[at(i + 1, j)] - b.p[at(i - 1, j)];
            const RV q = b.p[at(i, j + 1)] - b.p[at(i, j - 1)];
            b.n[at(i, j)] = Norm(Cross(t, q));
        }
    if (twoSided)
        b.Backfaces(first, firstIndex);
}
// ReefGeometry.js path: points of a gently curving path
std::vector<RV> Path(RV p, RV dir, float length, int steps, const std::function<void(int, RV&)>& bend)
{
    std::vector<RV> pts{p};
    RV d = Norm(dir);
    const float step = length / steps;
    for (int i = 1; i <= steps; ++i)
    {
        if (bend)
            bend(i, d);
        d = Norm(d);
        pts.push_back(pts.back() + d * step);
    }
    return pts;
}
// ReefGeometry.js blade: a tapering ribbon, both faces
void Blade(Builder& b, RV p, RV dir, RV out, float len, float w, float lean, int segs, float flag, float rnd)
{
    const uint32_t first = b.Count();
    const size_t fi = b.idx.size();
    const RV side = Norm(Cross(dir, out));
    std::vector<RV> pts;
    for (int k = 0; k <= segs; ++k)
    {
        const float t = float(k) / segs;
        pts.push_back(p + dir * (t * len) + out * (lean * t * t));
    }
    for (int k = 0; k <= segs; ++k)
    {
        const float t = float(k) / segs;
        const RV T = Norm(pts[std::min(segs, k + 1)] - pts[std::max(0, k - 1)]);
        const RV nor = Norm(Cross(side, T));
        const float ww = w * (k == segs ? 0.35f : 1 - 0.25f * t);
        for (float s : {-1.0f, 1.0f})
            b.Vertex(pts[k] + side * (s * ww * 0.5f), nor, t, flag, rnd);
    }
    for (int k = 0; k < segs; ++k)
        b.Quad(first + k * 2, first + k * 2 + 1, first + k * 2 + 3, first + k * 2 + 2);
    b.Backfaces(first, fi);
}

// ---------------------------------------------------------------- the organisms

enum Species
{
    Meadow,
    Sargassum,
    Fan,
    Rod,
    Brain,
    Boulder,
    Barrel,
    Tubes,
    Urchin,
    Anemone,
    Rubble,
    SpeciesCount
};
bool Flexible(int s) { return s == Meadow || s == Sargassum || s == Fan || s == Rod || s == Anemone; }

void BuildMeadow(Builder& b, Rng& rng, int lod)
{
    struct Shoot
    {
        float x, z, yaw;
        bool manatee;
        int nb;
        float bl[4][4];
    };
    const int N = 140;
    std::vector<Shoot> shoots(N);
    for (Shoot& s : shoots)
    {
        const float a = rng() * Tau, r = std::sqrt(rng()) * 0.97f;
        s.manatee = rng() < 0.2f;
        s.nb = 2 + int(rng() * 3);
        s.yaw = rng() * Pi;
        for (auto& bl : s.bl)
            for (float& v : bl)
                v = rng();
        s.x = std::cos(a) * r, s.z = std::sin(a) * r;
    }
    const int keep = lod == 0 ? N : 46, segs = lod == 0 ? 2 : 1, maxBlades = lod == 0 ? 4 : 2;
    const float widen = lod == 0 ? 1.0f : 1.45f;
    for (int i = 0; i < keep; ++i)
    {
        const Shoot& s = shoots[i];
        const int nb = std::min(s.nb, maxBlades);
        for (int k = 0; k < nb; ++k)
        {
            const float r1 = s.bl[k][0], r2 = s.bl[k][1], r3 = s.bl[k][2], r4 = s.bl[k][3];
            const float len = s.manatee ? 0.22f + r1 * 0.26f : 0.1f + r1 * 0.26f;
            const float w = (s.manatee ? 0.0035f : 0.008f + r2 * 0.005f) * widen;
            const float spread = k - (nb - 1) / 2.0f;
            const RV out{std::cos(s.yaw), 0, std::sin(s.yaw)};
            const RV dir = Norm(RV{0, 1, 0} + out * (spread * 0.14f + (r3 - 0.5f) * 0.1f));
            const float lean = (r4 - 0.3f) * 0.35f * len * (spread >= 0 ? 1.0f : -1.0f);
            Blade(b, {s.x + out.x * 0.003f * spread, -0.01f, s.z + out.z * 0.003f * spread}, dir, out, len, w, lean, segs, 0, r3);
        }
    }
}

void BuildSargassum(Builder& b, Rng& rng, int lod)
{
    const int axes = 2 + int(rng() * 3), segs = lod == 0 ? 7 : 4;
    const float H = 0.45f, leafKeep = lod == 0 ? 1.0f : 0.4f, widen = lod == 0 ? 1.0f : 1.5f;
    for (int i = 0; i < axes; ++i)
    {
        const float a = rng() * Tau, lean = rng.Rand(0.1f, 0.4f);
        const RV dir{std::cos(a) * lean, 1, std::sin(a) * lean};
        const float len = rng.Rand(0.3f, 0.5f), wig = rng.Rand(0.1f, 0.25f);
        const RV p0{rng.Rand(-0.02f, 0.02f), -0.01f, rng.Rand(-0.02f, 0.02f)};
        auto pts = Path(p0, dir, len, segs, [&](int, RV& d) { d.x += (rng() - 0.5f) * wig, d.z += (rng() - 0.5f) * wig; });
        const float w = rng();
        std::vector<float> radii;
        for (int k = 0; k <= segs; ++k)
            radii.push_back(0.0028f * widen * (1 - 0.3f * k / segs));
        Tube(b, pts, radii, 3, false, [&](int k, float) { return std::array<float, 3>{Clamp(pts[k].y / H, 0, 1), 0, w}; });
        for (int k = 0; k < 14; ++k)
        {
            const float t = 0.15f + 0.85f * (k + rng()) / 14;
            const float f = t * segs;
            const int i0 = std::min(segs - 1, int(f));
            const RV p = pts[i0] + (pts[i0 + 1] - pts[i0]) * (f - i0);
            const float ya = rng() * Tau, el = rng.Rand(0.3f, 1.1f), ll = rng.Rand(0.025f, 0.045f), lw = rng.Rand(0.005f, 0.009f);
            const bool bladder = rng() < 0.35f;
            const bool keep = rng() < leafKeep;
            const float rnd = rng();
            if (!keep)
                continue;
            const RV ld{std::cos(ya) * std::cos(el), std::sin(el), std::sin(ya) * std::cos(el)};
            RV face = Cross(ld, RV{0, 1, 0});
            face = Dot(face, face) < 0.1f ? RV{1, 0, 0} : Norm(face);
            const uint32_t first = b.Count();
            Blade(b, p, ld, face, ll, lw * widen, ll * 0.3f, 1, 1, rnd);
            for (uint32_t v = first; v < b.Count(); ++v)
                b.d[v][0] = Clamp(p.y / H, 0, 1);
            if (bladder && lod == 0)
            {
                const uint32_t f0 = b.Count();
                const size_t fi = b.idx.size();
                const RV c = p + ld * -0.004f + face * 0.006f;
                Blob(b, 0, [&](RV d, RV& o)
                     {
                         o = c + d * 0.0035f;
                         return std::array<float, 3>{Clamp(p.y / H, 0, 1), 2, w};
                     });
                b.SmoothNormals(f0, fi);
            }
        }
    }
}

void BuildFan(Builder& b, Rng& rng, int lod)
{
    const int nu = lod == 0 ? 18 : 10, nv = lod == 0 ? 10 : 5;
    const float H = 0.8f, th = rng.Rand(1.0f, 1.25f);
    auto lobe = WaveNoise(rng, 5, 2, 6);
    auto ruffle = WaveNoise(rng, 4, 3, 7);
    const float bow = rng.Rand(-0.25f, 0.25f), w = rng(), y0 = 0.06f;
    Sheet(b, nu, nv,
          [&](float u, float v, RV& out)
          {
              const float a = (u - 0.5f) * 2 * th;
              const float R = H * (0.88f + 0.12f * lobe(std::sin(a), std::cos(a), 0)) * (1 - 0.22f * std::pow(std::fabs(a) / th, 3.0f));
              const float r = 0.02f + v * (R - 0.02f);
              const float x = std::sin(a) * r * 1.1f, y = y0 + std::cos(a) * r;
              out = {x, y, bow * x * x + 0.03f * v * ruffle(x * 2, y * 2, 0)};
              return std::array<float, 3>{v, 0, w};
          },
          true);
    Tube(b, {{0, -0.02f, 0}, {0, y0 + 0.02f, 0}}, {0.018f, 0.012f}, lod == 0 ? 5 : 3, false,
         [&](int, float) { return std::array<float, 3>{0, 1, w}; });
}

void BuildRod(Builder& b, Rng& rng, int lod)
{
    const int sides = lod == 0 ? 5 : 4, rings = lod == 0 ? 3 : 2;
    const float thick = lod == 0 ? 1.0f : 1.2f, H = 0.85f;
    const float plane = rng.Rand(0, Tau);
    const RV pn{std::cos(plane), 0, std::sin(plane)};
    struct Spec
    {
        RV p, dir;
        float r, len;
        int depth;
    };
    std::vector<Spec> queue{{{0, -0.02f, 0}, {rng.Rand(-0.1f, 0.1f), 1, rng.Rand(-0.1f, 0.1f)}, 0.02f, rng.Rand(0.1f, 0.16f), 0}};
    int made = 0;
    const int budget = lod == 0 ? 36 : 20;
    for (size_t qi = 0; qi < queue.size() && made < budget; ++qi, ++made)
    {
        const Spec s = queue[qi];
        auto pts = Path(s.p, s.dir, s.len, rings, [&](int, RV& d) { d.y += 0.08f, d = d + pn * ((rng() - 0.5f) * 0.2f); });
        const bool last = s.depth >= 5 || pts.back().y > H * 0.92f;
        const float rEnd = s.r * 0.9f, w = rng();
        std::vector<float> radii;
        for (int i = 0; i <= rings; ++i)
            radii.push_back((s.r + (rEnd - s.r) * i / rings) * thick);
        Tube(b, pts, radii, sides, last, [&](int i, float) { return std::array<float, 3>{Clamp(pts[i].y / H, 0, 1), 0, w}; });
        if (last)
            continue;
        const RV end = pts.back(), dir = Norm(pts.back() - pts[pts.size() - 2]);
        for (float sgn : {-1.0f, 1.0f})
        {
            const float a = sgn * rng.Rand(0.3f, 0.6f);
            RV axis = Cross(dir, pn);
            axis = Dot(axis, axis) < 0.1f ? RV{1, 0, 0} : Norm(axis);
            RV d = Rotate(Rotate(dir, axis, a), dir, rng.Rand(-0.5f, 0.5f));
            d = Norm(d + RV{0, 0.3f, 0});
            queue.push_back({end, d, std::max(0.011f, rEnd * 0.92f), rng.Rand(0.09f, 0.19f), s.depth + 1});
        }
    }
}

void BuildDome(Builder& b, Rng& rng, int lod)
{
    const int segs = lod == 0 ? 30 : 12, rings = lod == 0 ? 10 : 5;
    const float H = rng.Rand(0.42f, 0.58f);
    auto n = WaveNoise(rng, 5, 1.5f, 3.5f);
    std::vector<ProfilePt> prof{{0.5f, -0.03f, 0, 0, 0}, {0.515f, 0.0f, 0, 0, 0}};
    for (int i = 1; i <= rings; ++i)
    {
        const float a = float(i) / rings * Pi * 0.5f;
        prof.push_back({0.5f * std::pow(std::cos(a), 0.8f) + 1e-3f, H * std::pow(std::sin(a), 1.15f), 0, 0, 0});
    }
    const uint32_t first = b.Count();
    Lathe(b, prof, segs, [&](float phi, int) { return 1 + 0.05f * n(std::cos(phi), 0, std::sin(phi)); });
    for (uint32_t v = first; v < b.Count(); ++v)
    {
        const float x = b.p[v].x, y = b.p[v].y, z = b.p[v].z;
        b.p[v].y = y * (1 + 0.08f * n(x * 2, 0.5f, z * 2));
        b.d[v][0] = Clamp(y / H, 0, 1);
    }
    b.SmoothNormals();
}

// lumps for createMound: distance along d to the far side of the union of ellipsoids
float LumpRadius(RV d, const std::vector<float>& lumps, float fallback)
{
    float best = fallback;
    for (size_t i = 0; i < lumps.size(); i += 6)
    {
        const float dx = d.x / lumps[i + 3], dy = d.y / lumps[i + 4], dz = d.z / lumps[i + 5];
        const float ox = lumps[i] / lumps[i + 3], oy = lumps[i + 1] / lumps[i + 4], oz = lumps[i + 2] / lumps[i + 5];
        const float A = dx * dx + dy * dy + dz * dz, B = -2 * (dx * ox + dy * oy + dz * oz), C = ox * ox + oy * oy + oz * oz - 1;
        const float disc = B * B - 4 * A * C;
        if (disc < 0)
            continue;
        best = std::max(best, (-B + std::sqrt(disc)) / (2 * A));
    }
    return best;
}
void BuildBoulder(Builder& b, Rng& rng, int lod)
{
    // createMound 'boulder': smooth big lobes, spreading margin
    std::vector<float> lumps;
    const int n = 3 + int(rng() * 4);
    for (int i = 0; i < n; ++i)
    {
        const float a = rng() * Tau, r = std::sqrt(rng()) * 0.18f * (i == 0 ? 0.3f : 1.0f);
        const float size = rng.Rand(0.24f, 0.38f) * (i == 0 ? 1.15f : 1.0f), fl = rng.Rand(0.6f, 0.9f);
        lumps.insert(lumps.end(), {std::cos(a) * r, 0.08f + rng.Rand(-0.05f, 0.06f), std::sin(a) * r, size * rng.Rand(0.85f, 1.2f),
                                   size * fl * 1.6f, size * rng.Rand(0.85f, 1.2f)});
    }
    auto bump = WaveNoise(rng, 7, 3, 7);
    auto fine = WaveNoise(rng, 8, 14, 26);
    float Hmax = 0;
    Blob(b, lod == 0 ? 3 : 2, [&](RV d, RV& out)
         {
             const RV dir = Norm({d.x, d.y >= 0 ? d.y : d.y * 0.3f, d.z});
             float r = LumpRadius(dir, lumps, 0.12f);
             r *= 1 + 0.06f * bump(d.x, d.y, d.z) + (lod == 0 ? 0.012f * fine(d.x, d.y, d.z) : 0);
             out = dir * r;
             const float nearBase = 1 - Smooth(0.0f, 0.14f, out.y);
             out.x *= 1 - 0.25f * nearBase * 0.5f;
             out.z *= 1 - 0.25f * nearBase * 0.5f;
             if (d.y < 0)
                 out.y = std::max(out.y, -0.05f);
             Hmax = std::max(Hmax, out.y);
             return std::array<float, 3>{0, 0, bump(d.x * 0.5f, d.y * 0.5f, d.z * 0.5f) * 0.5f + 0.5f};
         });
    for (uint32_t v = 0; v < b.Count(); ++v)
        b.d[v][0] = Clamp(b.p[v].y / std::max(Hmax, 1e-3f), 0, 1);
    b.SmoothNormals();
}

void BuildBarrel(Builder& b, Rng& rng, int lod)
{
    const int segs = lod == 0 ? 48 : 24, ridges = 9 + int(rng() * 6);
    const float ph = rng() * Tau;
    auto knob = WaveNoise(rng, 5, 8, 16);
    const float H = rng.Rand(0.65f, 0.9f), rim = rng.Rand(0.28f, 0.34f);
    const float outer[6][2] = {{0.16f, -0.02f}, {0.24f, 0.1f}, {0.33f, 0.3f}, {0.35f, 0.5f}, {rim + 0.01f, 0.75f}, {rim, 1}};
    std::vector<ProfilePt> prof;
    for (auto& o : outer)
        prof.push_back({o[0], o[1] * H, o[1], 0, 0});
    const int nOuter = int(prof.size());
    prof.push_back({rim - 0.035f, H - 0.01f, 1, 1, 0});
    prof.push_back({rim - 0.06f, H * 0.7f, 0.7f, 1, 0});
    prof.push_back({rim - 0.1f, H * 0.35f, 0.35f, 1, 0});
    prof.push_back({0.02f, H * 0.18f, 0.2f, 1, 0});
    Lathe(b, prof, segs,
          [&](float phi, int i)
          {
              if (i >= nOuter)
                  return 1.0f;
              const float t = prof[i].y / H;
              const float rid = std::pow(std::fabs(std::cos(phi * ridges / 2 + ph)), 2.5f);
              return 1 + (0.1f * rid - 0.04f) * Smooth(0.05f, 0.3f, t) + 0.04f * knob(std::cos(phi), t * 2, std::sin(phi));
          });
    b.SmoothNormals();
}

void BuildTubes(Builder& b, Rng& rng, int lod)
{
    const int segs = lod == 0 ? 12 : 6, n = 2 + int(rng() * 5);
    for (int k = 0; k < n; ++k)
    {
        const float a = rng() * Tau, off = k == 0 ? 0 : rng.Rand(0.03f, 0.09f);
        const float ox = std::cos(a) * off, oz = std::sin(a) * off;
        const float h = rng.Rand(0.15f, 0.42f), r = rng.Rand(0.022f, 0.04f), flare = rng.Rand(1.0f, 1.25f);
        const float lean = rng.Rand(0.05f, 0.25f), la = std::atan2(oz, ox) + rng.Rand(-0.4f, 0.4f), w = rng();
        const int rings = lod == 0 ? 4 : 2;
        std::vector<ProfilePt> prof;
        for (int i = 0; i <= rings; ++i)
        {
            const float t = float(i) / rings;
            prof.push_back({r * (0.8f + (flare - 0.8f) * t), -0.02f + t * h, t, 0, w});
        }
        const ProfilePt top = prof.back();
        const float wall = r * 0.28f;
        prof.push_back({top.r - wall, top.y - 0.004f, 1, 1, w});
        prof.push_back({(top.r - wall) * 0.85f, top.y - h * 0.3f, 0.7f, 1, w});
        prof.push_back({0.004f, top.y - h * 0.6f, 0.4f, 1, w});
        const uint32_t first = b.Count();
        const size_t fi = b.idx.size();
        auto bumpy = WaveNoise(rng, 3, 10, 20);
        Lathe(b, prof, segs, [&](float phi, int i) { return 1 + 0.05f * bumpy(std::cos(phi), i * 0.4f, std::sin(phi)); });
        for (uint32_t v = first; v < b.Count(); ++v)
        {
            const float y = std::max(0.0f, b.p[v].y);
            b.p[v].x += ox + std::cos(la) * lean * y;
            b.p[v].z += oz + std::sin(la) * lean * y;
        }
        b.SmoothNormals(first, fi);
    }
}

void BuildUrchin(Builder& b, Rng& rng, int lod)
{
    Blob(b, lod == 0 ? 2 : 1, [&](RV d, RV& out)
         {
             out = {d.x * 0.045f, 0.035f + d.y * (d.y > 0 ? 0.035f : 0.02f), d.z * 0.045f};
             return std::array<float, 3>{0, 0, 0};
         });
    b.SmoothNormals();
    const int n = lod == 0 ? 56 : 18;
    for (int i = 0; i < n; ++i)
    {
        RV dir = RandomUnit(rng);
        dir.y = std::fabs(dir.y) * 0.9f + 0.1f;
        dir = Norm(dir);
        const float len = rng.Rand(0.12f, 0.22f) * (0.6f + 0.4f * dir.y), r = 0.0032f;
        const RV base = RV{0, 0.035f, 0} + dir * 0.04f;
        const RV side = Perpendicular(dir), side2 = Cross(dir, side);
        const float w = rng();
        uint32_t ids[3];
        for (int k = 0; k < 3; ++k)
        {
            const float a = k / 3.0f * Tau;
            const RV nor = side * std::cos(a) + side2 * std::sin(a);
            ids[k] = b.Vertex(base + nor * r, nor, 0, 1, w);
        }
        const uint32_t tip = b.Vertex(base + dir * len, dir, 1, 1, w);
        for (int k = 0; k < 3; ++k)
            b.Tri(ids[k], ids[(k + 1) % 3], tip);
    }
}

void BuildAnemone(Builder& b, Rng& rng, int lod)
{
    const int segs = lod == 0 ? 12 : 7;
    Lathe(b, {{0.045f, -0.01f, 0, 0, 0}, {0.04f, 0.03f, 0, 0, 0}, {0.05f, 0.05f, 0, 0, 0}, {0.03f, 0.058f, 0, 0, 0}, {0.002f, 0.06f, 0, 0, 0}},
          segs, nullptr);
    b.SmoothNormals();
    const int n = lod == 0 ? 44 : 18, sides = lod == 0 ? 4 : 3, rings = lod == 0 ? 4 : 2;
    for (int i = 0; i < n; ++i)
    {
        const float a = float(i) / n * Tau * 3.1f + rng() * 0.3f;
        const float ring = (i % 3) / 3.0f, r0 = 0.018f + ring * 0.025f, el = rng.Rand(0.5f, 1.2f) - ring * 0.4f;
        const RV dir{std::cos(a) * std::cos(el), std::sin(el), std::sin(a) * std::cos(el)};
        const RV p{std::cos(a) * r0, 0.055f, std::sin(a) * r0};
        const float len = rng.Rand(0.08f, 0.14f);
        auto pts = Path(p, dir, len, rings, [&](int, RV& d) { d.y -= 0.12f, d.x += std::cos(a) * 0.1f, d.z += std::sin(a) * 0.1f; });
        const float w = rng();
        std::vector<float> radii;
        for (int k = 0; k <= rings; ++k)
            radii.push_back(0.0065f * (1 - 0.35f * k / rings));
        Tube(b, pts, radii, sides, true, [&](int, float t) { return std::array<float, 3>{t, 1, w}; });
    }
}

void BuildRubble(Builder& b, Rng& rng, int lod)
{
    const int n = lod == 0 ? 12 : 6;
    for (int i = 0; i < n; ++i)
    {
        const float a = rng() * Tau, r = std::sqrt(rng()) * 0.28f;
        const float x = std::cos(a) * r, z = std::sin(a) * r, w = rng();
        const float bleached = rng() < 0.4f ? 1.0f : 0.0f;
        if (rng() < 0.55f)
        {
            const float ya = rng() * Tau, len = rng.Rand(0.05f, 0.14f), rad = rng.Rand(0.008f, 0.014f);
            const RV d = Norm({std::cos(ya), rng.Rand(-0.1f, 0.25f), std::sin(ya)});
            const RV p0{x, rad * 0.7f, z};
            Tube(b, {p0, p0 + d * len}, {rad, rad * 0.8f}, lod == 0 ? 5 : 3, true,
                 [&](int, float) { return std::array<float, 3>{0, bleached, w}; });
        }
        else
        {
            const float s = rng.Rand(0.025f, 0.07f);
            auto nz = WaveNoise(rng, 3, 2, 4);
            const uint32_t first = b.Count();
            const size_t fi = b.idx.size();
            Blob(b, lod == 0 ? 1 : 0, [&](RV d, RV& out)
                 {
                     const float k = 1 + 0.3f * nz(d.x, d.y, d.z);
                     out = {x + d.x * s * k, std::max(0.3f * s + d.y * s * 0.6f * k, -0.01f), z + d.z * s * k};
                     return std::array<float, 3>{0, bleached * 0.5f, w};
                 });
            b.SmoothNormals(first, fi);
        }
    }
}

void Build(int s, Builder& b, Rng& rng, int lod)
{
    switch (s)
    {
    case Meadow: BuildMeadow(b, rng, lod); break;
    case Sargassum: BuildSargassum(b, rng, lod); break;
    case Fan: BuildFan(b, rng, lod); break;
    case Rod: BuildRod(b, rng, lod); break;
    case Brain: BuildDome(b, rng, lod); break;
    case Boulder: BuildBoulder(b, rng, lod); break;
    case Barrel: BuildBarrel(b, rng, lod); break;
    case Tubes: BuildTubes(b, rng, lod); break;
    case Urchin: BuildUrchin(b, rng, lod); break;
    case Anemone: BuildAnemone(b, rng, lod); break;
    default: BuildRubble(b, rng, lod); break;
    }
}

// ---------------------------------------------------------------- placement and colour (Reef.js)

struct Kind
{
    float scaleMin, scaleMax;   // Reef.js `scale`
    float depthMin, depthMax;   // where it lives here (m of water)
    float weight;               // how common
    uint32_t c1[4], c2;         // Reef.js palettes (sRGB): variants, and the accent
};
const Kind Kinds[SpeciesCount] = {
    {0.85f, 1.2f, 0.8f, 9, 3.0f, {0x6a8a3a, 0x78963e, 0x5e7e34, 0x82983f}, 0x9a8a52},   // meadow
    {0.7f, 1.5f, 0.8f, 7, 1.2f, {0x8a6a2a, 0x9a7a32, 0x7a5e26, 0xa08036}, 0xa87a30},    // sargassum
    {0.4f, 1.2f, 2.0f, 18, 0.8f, {0x7a4a80, 0x8a5890, 0x9a6a92, 0xa89458}, 0x5e2e60},   // sea fan
    {0.65f, 1.6f, 2.0f, 18, 0.8f, {0x9a7a66, 0x8c6c70, 0x9a7486, 0x8a5a8c}, 0xc0b0a0},  // sea rod
    {0.35f, 1.3f, 2.0f, 14, 0.6f, {0xb89c60, 0xac9258, 0xc0a468, 0xa0925c}, 0x6e6e4c},  // brain coral
    {0.8f, 2.2f, 1.5f, 22, 0.9f, {0xae8e56, 0xa4885a, 0x98885a, 0xb49660}, 0x8c9068},   // star coral boulder
    {0.7f, 1.5f, 5.0f, 25, 0.35f, {0x8c5040, 0x9a6050, 0x7c5448, 0x8a6258}, 0x60342a},  // barrel sponge
    {0.8f, 1.6f, 3.0f, 18, 0.5f, {0xd0b040, 0xd07030, 0x8a5aa0, 0x9a6040}, 0x5a4020},   // tube sponges
    {0.8f, 1.2f, 1.0f, 12, 0.5f, {0x141014, 0x181418, 0x100c10, 0x141014}, 0x2a1a2a},   // urchin
    {0.9f, 1.5f, 2.0f, 14, 0.35f, {0xdccca8, 0xd0c0a0, 0xd8c8c0, 0xdccca8}, 0xc870a4},  // anemone
    {0.8f, 1.8f, 1.0f, 20, 0.8f, {0xa09484, 0x968a7a, 0xa89c8c, 0x9a8e7e}, 0xd4ccbc},   // rubble
};
struct Rgb
{
    float r, g, b;
};
Rgb Hex(uint32_t h) { return {((h >> 16) & 255) / 255.0f, ((h >> 8) & 255) / 255.0f, (h & 255) / 255.0f}; }
Rgb Mix(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }

// The colour along an organism (t: base 0 .. tip 1) for a variant, plain (flag 0) or the
// species' second part (flag 1: inner wall, stalk, tentacle, leaf; flag 2: bladder)
Rgb Colour(int s, int variant, int flag, float t)
{
    const Kind& k = Kinds[s];
    const Rgb c1 = Hex(k.c1[variant % 4]), c2 = Hex(k.c2);
    Rgb c = c1;
    switch (s)
    {
    case Meadow: c = Mix(Mix(c1, Rgb{0.2f, 0.26f, 0.12f}, (1 - t) * 0.4f), Mix(c1, c2, 0.6f), Smooth(0.6f, 1.0f, t)); break; // dark bases, aged tips
    case Sargassum: c = flag == 2 ? Hex(0xa87a30) : flag == 1 ? Mix(c1, Rgb{0.8f, 0.6f, 0.25f}, 0.25f) : Mix(c1, c1, 0); break;
    case Fan: c = flag == 1 ? c2 : Mix(c1, Mix(c1, Rgb{1, 1, 1}, 0.2f), t); break;
    case Rod: c = Mix(c1, c2, Smooth(0.85f, 1.0f, t) * 0.5f); break; // tips a little paler
    case Brain:
    case Boulder: c = Mix(Mix(c1, c2, 0.25f), c1, Smooth(0.2f, 0.8f, t)); break;
    case Barrel:
    case Tubes: c = flag == 1 ? Mix(c1, Hex(k.c2), 0.75f) : c1; break; // dark inner wall
    case Urchin: c = c1; break;
    case Anemone: c = flag == 1 ? Mix(c1, c2, Smooth(0.55f, 1.0f, t)) : Mix(c1, Rgb{0.6f, 0.55f, 0.45f}, 0.3f); break;
    default: c = flag >= 1 ? c2 : c1; break; // rubble: bleached fragments
    }
    // a little darker toward the base (ambient occlusion on the sea bed)
    const float ao = 0.72f + 0.28f * Smooth(0.0f, 0.35f, t);
    return {c.r * ao, c.g * ao, c.b * ao};
}

uint32_t HashCell(int x, int z, uint32_t salt)
{
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(z) * 668265263u + salt * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
} // namespace

void ReefLife::EnsureMeshes()
{
    if (_built)
        return;
    _built = true;
    EngineWgpu::AcquireProducerWindow("Reef meshes");
    _meshes.assign(size_t(SpeciesCount) * Variants * Lods * Sway, 0);
    _indexCounts.assign(_meshes.size(), 0);
    size_t tris = 0;
    for (int s = 0; s < SpeciesCount; ++s)
    {
        for (int v = 0; v < Variants; ++v)
            for (int l = 0; l < Lods; ++l)
            {
                Rng rng(HashCell(s, v, 77));
                Builder b;
                Build(s, b, rng, l);
                const int phases = Flexible(s) ? Sway : 1;
                for (int ph = 0; ph < phases; ++ph)
                {
                    // the surge sway: tips displaced along the model's x by up to 6% of the height,
                    // growing with the square of the height above the bed
                    const float sway = Flexible(s) ? std::sin(Tau * ph / Sway) : 0.0f;
                    std::vector<WgrMeshVertex> vtx(b.Count());
                    for (uint32_t i = 0; i < b.Count(); ++i)
                    {
                        const float h = std::max(0.0f, b.p[i].y);
                        const float dx = sway * 0.06f * h * h / 0.5f * (s == Meadow ? 1.6f : 1.0f);
                        WgrMeshVertex& w = vtx[i];
                        w.pos = {b.p[i].x + dx, b.p[i].y, b.p[i].z};
                        w.normal = {b.n[i].x, b.n[i].y, b.n[i].z};
                        // atlas: u along the organism, v the variant's two rows (plain, second part)
                        const int flag = std::min(1, int(b.d[i][1] + 0.5f)) + (b.d[i][1] > 1.5f ? 1 : 0);
                        const int row = v * 3 + std::min(flag, 2);
                        w.uv = {Clamp(b.d[i][0], 0.02f, 0.98f), (row + 0.5f) / (Variants * 3)};
                        w.uv1 = w.uv;
                    }
                    const size_t mi = ((size_t(s) * Variants + v) * Lods + l) * Sway + ph;
                    _meshes[mi] = wgr_mesh_create(_renderer, WgrSlice<WgrMeshVertex>(vtx), WgrSlice<uint32_t>(b.idx));
                    _indexCounts[mi] = uint32_t(b.idx.size());
                    tris += b.idx.size() / 3;
                }
            }
        constexpr int TW = 32, TH = Variants * 3;
        std::vector<uint8_t> px(TW * TH * 4);
        for (int y = 0; y < TH; ++y)
            for (int x = 0; x < TW; ++x)
            {
                const Rgb c = Colour(s, y / 3, y % 3, (x + 0.5f) / TW);
                uint8_t* q = &px[(y * TW + x) * 4];
                q[0] = uint8_t(Clamp(c.r, 0, 1) * 255), q[1] = uint8_t(Clamp(c.g, 0, 1) * 255), q[2] = uint8_t(Clamp(c.b, 0, 1) * 255);
                q[3] = 255;
            }
        _textures.push_back(wgr_texture_create(_renderer, TW, TH, WGR_TEXTURE_RGBA8, 1, 0, px.data(), uint32_t(px.size())));
    }
    LOG_INFO(Graphics, "Reef: {} species x {} variants x {} levels baked ({} triangles)", int(SpeciesCount), Variants, Lods, tris);
}

// Zonation (Reef.js: patch reefs, seagrass on the back reef, sand between): a smooth value-noise
// field over ~30 m decides what a cell is. 0 sand .. 1 reef; meadow where it is low.
namespace
{
float ZoneAt(float x, float z)
{
    constexpr float Scale = 30.0f;
    const float fx = x / Scale, fz = z / Scale;
    const int ix = int(std::floor(fx)), iz = int(std::floor(fz));
    const float tx = fx - ix, tz = fz - iz;
    auto h = [](int a, int b) { return (HashCell(a, b, 9) & 0xffff) / 65535.0f; };
    const float sx = tx * tx * (3 - 2 * tx), sz = tz * tz * (3 - 2 * tz);
    const float a = h(ix, iz) + (h(ix + 1, iz) - h(ix, iz)) * sx;
    const float b = h(ix, iz + 1) + (h(ix + 1, iz + 1) - h(ix, iz + 1)) * sx;
    return a + (b - a) * sz;
}
} // namespace

// The items of one 6 m cell, placed once from the cell's own hash: what the cell is (patch reef,
// meadow, sand) from the zone field, the species mix by the water depth within that.
const std::vector<ReefLife::Item>& ReefLife::Cell(int cx, int cz)
{
    const int64_t key = (int64_t(cx) << 32) ^ int64_t(uint32_t(cz));
    auto it = _cells.find(key);
    if (it != _cells.end())
        return it->second;
    std::vector<Item>& items = _cells[key];
    const float sea = GLandscape->GetSeaLevel();
    Rng rng(HashCell(cx, cz, 1));
    const float zone = ZoneAt((cx + 0.5f) * CellSize, (cz + 0.5f) * CellSize);
    const bool reef = zone > 0.6f, meadow = zone < 0.36f;
    // how many: dense on a patch reef and in a meadow, a scatter on the sand
    const int n = reef ? 7 + int(rng() * 6) : meadow ? 6 + int(rng() * 4) : int(rng() * 2.2f);
    for (int i = 0; i < n; ++i)
    {
        const float x = (cx + rng()) * CellSize, z = (cz + rng()) * CellSize;
        const float ground = GLandscape->SurfaceY(x, z);
        const float depth = sea - ground;
        const float pick01 = rng(), yaw = rng() * Tau, sr = rng(), ph = rng(), vr = rng();
        // the zone's own species list
        auto allowed = [&](int s)
        {
            if (reef)
                return s != Meadow;
            if (meadow)
                return s == Meadow || s == Sargassum || s == Urchin || s == Anemone;
            return s == Rubble || s == Urchin || s == Boulder || s == Sargassum;
        };
        auto weight = [&](int s) { return s == Meadow && meadow ? 6.0f : Kinds[s].weight; };
        float total = 0;
        for (int s = 0; s < SpeciesCount; ++s)
            if (allowed(s) && depth >= Kinds[s].depthMin && depth <= Kinds[s].depthMax)
                total += weight(s);
        if (total <= 0)
            continue;
        const float pick = pick01 * total;
        float acc = 0;
        for (int s = 0; s < SpeciesCount; ++s)
        {
            const Kind& k = Kinds[s];
            if (!allowed(s) || depth < k.depthMin || depth > k.depthMax)
                continue;
            acc += weight(s);
            if (pick <= acc)
            {
                const float scale = k.scaleMin + (k.scaleMax - k.scaleMin) * sr;
                // nothing rises out of the water (Reef.js MAX_TOP)
                if (ground + scale * 0.9f > sea - 0.4f)
                    break;
                items.push_back({s, int(vr * Variants) % Variants, x, ground - 0.02f, z, yaw, scale, ph});
                break;
            }
        }
    }
    return items;
}

void ReefLife::Update(const Vector3& camera, float time)
{
    static const bool enabled = []
    {
        const char* v = std::getenv("WGR_REEF");
        return !(v && v[0] == '0');
    }();
    if (!enabled || !GLandscape || !_renderer)
        return;
    const float sea = GLandscape->GetSeaLevel();
    // under the water, or near it looking in (from high above the floor is not seen)
    if (camera.Y() > sea + 30.0f)
        return;
    // is there sea around at all? (cheap: the floor under and around the camera)
    bool anySea = false;
    for (int k = 0; k < 9 && !anySea; ++k)
        anySea = GLandscape->SurfaceY(camera.X() + (k % 3 - 1) * 30.0f, camera.Z() + (k / 3 - 1) * 30.0f) < sea - 0.8f;
    if (!anySea)
        return;
    EnsureMeshes();
    // forget cells far away (they are rebuilt identically when you come back)
    if (_cells.size() > 4000)
        _cells.clear();
    const int r = int(std::ceil(DrawRange / CellSize));
    const int ccx = int(std::floor(camera.X() / CellSize)), ccz = int(std::floor(camera.Z() / CellSize));
    int drawn = 0;
    for (int dz = -r; dz <= r; ++dz)
        for (int dx = -r; dx <= r; ++dx)
        {
            if ((dx * dx + dz * dz) * CellSize * CellSize > (DrawRange + CellSize) * (DrawRange + CellSize))
                continue;
            for (const Item& it : Cell(ccx + dx, ccz + dz))
            {
                const float ex = it.x - camera.X(), ey = it.y - camera.Y(), ez = it.z - camera.Z();
                const float dist = std::sqrt(ex * ex + ey * ey + ez * ez);
                if (dist > DrawRange || it.scale / std::max(dist, 0.1f) < 0.004f)
                    continue;
                const int lod = it.scale / std::max(dist, 0.1f) > 0.06f ? 0 : 1;
                // the surge: a slow swell travelling across the bed
                int ph = 0;
                if (Flexible(it.species))
                {
                    const float w = time * 0.55f + it.x * 0.07f + it.z * 0.04f + it.phase * 0.6f;
                    ph = int(std::floor((w - std::floor(w)) * Sway)) % Sway;
                }
                const size_t mi = ((size_t(it.species) * Variants + it.variant) * Lods + lod) * Sway + ph;
                const float c = std::cos(it.yaw) * it.scale, s = std::sin(it.yaw) * it.scale;
                const float world[16] = {c, 0, -s, 0, 0, it.scale, 0, 0, s, 0, c, 0, it.x, it.y, it.z, 1};
                _engine.SubmitDynamicMesh(_meshes[mi], _indexCounts[mi], world, _textures[it.species], 2);
                ++drawn;
            }
        }
    static float nextLog = 0;
    if (time >= nextLog)
    {
        nextLog = time + 5.0f;
        LOG_INFO(Graphics, "Reef: {} items drawn, {} cells known, camera {:.1f} m from sea level", drawn, _cells.size(),
                 camera.Y() - sea);
    }
}
} // namespace Poseidon
