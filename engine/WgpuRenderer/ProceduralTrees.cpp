// SPDX-License-Identifier: GPL-3.0-or-later
//
// The owner's tree test (2026-10-13): OFP's trees nearby the camera swapped for procedural trees.
// The generator is a port of dgreenheck/ez-tree (MIT, (c) 2024 Daniel Greenheck; see CREDITS.md),
// src/lib/tree.js: a trunk grown section by section -- each section's frame perturbed by the
// branch level's gnarliness, twisted, and bent toward (or away from) a growth force -- with child
// branches spawned stratified along the parent at the level's angle, and leaf quads (single or
// crossed) on the last level. Its presets (oak, ash, aspen, pine, bush) are the ones in
// src/lib/presets. Its RNG is reproduced exactly; its quirks too (a section's length is the
// branch length over its section count; a child's frame is slerped from the far section).
//
// What OP changes: the bark and leaf textures are drawn procedurally here instead of shipped
// images (ez-tree's leaf sheets are MIT, its bark CC0, but this keeps the test free of new assets);
// the trees do not sway; each variant is grown once and scaled to the OFP tree it replaces, the
// species chosen from the OFP model's name (spruce/pine -> pine, birch/poplar -> aspen, ash -> ash,
// small trees -> bushes and small oaks, the rest oaks).
//
// The swap: every half second the trees (MapTree / MapSmallTree) within the test's range are
// claimed -- their GPU-driven instance is dropped and their coverage set to Full so the CPU pass
// skips them -- and released (re-admitted) when they leave it or the test is switched off.

#include "ProceduralTrees.hpp"
#include "EngineWgpu.hpp"
#include "WildlifeWorld.hpp"
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Dev/Diag/ProceduralTreesTest.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/World/MapTypes.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>

namespace Poseidon
{
namespace
{
constexpr float Pi = 3.14159265f, Tau = 6.2831853f;

struct P3
{
    float x = 0, y = 0, z = 0;
};
P3 operator+(P3 a, P3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
P3 operator-(P3 a, P3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
P3 operator*(P3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(P3 a, P3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
P3 Cross(P3 a, P3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Len(P3 a) { return std::sqrt(Dot(a, a)); }
P3 Norm(P3 a)
{
    const float l = Len(a);
    return l > 1e-12f ? a * (1.0f / l) : P3{0, 1, 0};
}
P3 Lerp(P3 a, P3 b, float t) { return a + (b - a) * t; }

// three.js Quaternion / Euler (order XYZ), as ez-tree uses them
struct Quat
{
    float x = 0, y = 0, z = 0, w = 1;
};
struct Euler
{
    float x = 0, y = 0, z = 0;
};
Quat Mul(Quat a, Quat b)
{
    return {a.x * b.w + a.w * b.x + a.y * b.z - a.z * b.y, a.y * b.w + a.w * b.y + a.z * b.x - a.x * b.z,
            a.z * b.w + a.w * b.z + a.x * b.y - a.y * b.x, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Quat FromEuler(Euler e)
{
    const float c1 = std::cos(e.x / 2), c2 = std::cos(e.y / 2), c3 = std::cos(e.z / 2);
    const float s1 = std::sin(e.x / 2), s2 = std::sin(e.y / 2), s3 = std::sin(e.z / 2);
    return {s1 * c2 * c3 + c1 * s2 * s3, c1 * s2 * c3 - s1 * c2 * s3, c1 * c2 * s3 + s1 * s2 * c3, c1 * c2 * c3 - s1 * s2 * s3};
}
Euler ToEuler(Quat q)
{
    const float m11 = 1 - 2 * (q.y * q.y + q.z * q.z), m12 = 2 * (q.x * q.y - q.w * q.z), m13 = 2 * (q.x * q.z + q.w * q.y);
    const float m22 = 1 - 2 * (q.x * q.x + q.z * q.z), m23 = 2 * (q.y * q.z - q.w * q.x);
    const float m32 = 2 * (q.y * q.z + q.w * q.x), m33 = 1 - 2 * (q.x * q.x + q.y * q.y);
    Euler e;
    e.y = std::asin(std::clamp(m13, -1.0f, 1.0f));
    if (std::fabs(m13) < 0.9999999f)
    {
        e.x = std::atan2(-m23, m33);
        e.z = std::atan2(-m12, m11);
    }
    else
    {
        e.x = std::atan2(m32, m22);
        e.z = 0;
    }
    return e;
}
Quat AxisAngle(P3 axis, float a)
{
    const float s = std::sin(a / 2);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(a / 2)};
}
P3 Rotate(P3 v, Quat q)
{
    // v + 2w(q x v) + 2 q x (q x v)
    const P3 u{q.x, q.y, q.z};
    const P3 t = Cross(u, v) * 2.0f;
    return v + t * q.w + Cross(u, t);
}
Quat Slerp(Quat a, Quat b, float t)
{
    float c = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (c < 0)
    {
        b = {-b.x, -b.y, -b.z, -b.w};
        c = -c;
    }
    if (c >= 1.0f)
        return a;
    const float s2 = 1 - c * c;
    if (s2 <= 1e-12f)
    {
        const float s = 1 - t;
        Quat r{s * a.x + t * b.x, s * a.y + t * b.y, s * a.z + t * b.z, s * a.w + t * b.w};
        const float l = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
        return {r.x / l, r.y / l, r.z / l, r.w / l};
    }
    const float s = std::sqrt(s2), h = std::atan2(s, c);
    const float ra = std::sin((1 - t) * h) / s, rb = std::sin(t * h) / s;
    return {a.x * ra + b.x * rb, a.y * ra + b.y * rb, a.z * ra + b.z * rb, a.w * ra + b.w * rb};
}

// ez-tree rng.js (a multiply-with-carry generator), bit for bit
struct Rng
{
    uint32_t w, z;
    explicit Rng(uint32_t seed) : w(123456789u + seed), z(987654321u - seed) {}
    float Random(float max = 1, float min = 0)
    {
        z = 36969u * (z & 65535u) + (z >> 16);
        w = 18000u * (w & 65535u) + (w >> 16);
        const uint32_t r = (z << 16) + (w & 65535u);
        return (max - min) * float(double(r) / 4294967296.0) + min;
    }
};

struct Preset
{
    const char* name;
    uint32_t seed;
    bool deciduous;
    int levels;
    float angle[4];
    int children[4];
    float gnarliness[4], length[4], radius[4];
    int sections[4], segments[4];
    float start[4], taper[4], twist[4];
    P3 forceDir;
    float forceStrength;
    float leafAngle;
    int leafCount;
    float leafStart, leafSize, leafSizeVariance;
    bool leafDouble;
    int leaf; // 0 oak, 1 ash, 2 aspen, 3 pine
    int bark; // 0 oak, 1 ash, 2 aspen, 3 pine
};
// src/lib/presets/*.json
const Preset kPresets[] = {
    {"oak_medium", 35729, true, 3, {0, 54, 58, 32}, {6, 4, 3, 0}, {0, -0.1f, -0.15f, 0.09f}, {37.24f, 11.08f, 12.39f, 7.16f},
     {1.41f, 0.9f, 0.69f, 1.19f}, {8, 6, 3, 1}, {7, 5, 3, 3}, {0, 0.49f, 0.06f, 0.12f}, {0.73f, 0.42f, 0.69f, 0.75f},
     {-0.23f, 0.42f, 0, 0}, {0, 1, 0}, 0.02f, 42, 18, 0.16f, 2.5f, 0.7f, true, 0, 0},
    {"ash_medium", 36330, true, 3, {0, 48, 75, 60}, {7, 4, 3, 0}, {0.03f, 0.25f, 0.2f, 0.09f}, {43.47f, 27.14f, 9.51f, 4.6f},
     {2.0f, 0.63f, 0.76f, 0.7f}, {12, 8, 6, 4}, {12, 6, 4, 3}, {0, 0.23f, 0.33f, 0}, {0.7f, 0.7f, 0.7f, 0.7f},
     {0.09f, -0.07f, 0, 0}, {0, 1, 0}, 0.01f, 55, 16, 0, 2.67f, 0.72f, true, 1, 1},
    {"aspen_medium", 18020, true, 2, {0, 75, 32, 7}, {10, 3, 3, 0}, {0.05f, 0.12f, 0.12f, 0.02f}, {50, 6.07f, 11.19f, 1},
     {0.72f, 0.41f, 0.7f, 0.7f}, {12, 10, 8, 6}, {8, 6, 4, 3}, {0, 0.59f, 0.35f, 0}, {0.37f, 0.13f, 0.7f, 0.7f},
     {0, 0, 0, 0}, {0, 1, 0}, 0.0148f, 30, 11, 0.124f, 2.5f, 0.7f, true, 2, 2},
    {"pine_medium", 13977, false, 1, {0, 110, 16, 60}, {82, 3, 5, 0}, {0.05f, 0.08f, 0, 0}, {50, 23.87f, 14.08f, 1},
     {1.05f, 0.36f, 0.7f, 0.7f}, {12, 10, 8, 6}, {8, 6, 4, 3}, {0, 0.27f, 0.14f, 0.3f}, {0.7f, 0.7f, 0.7f, 0.7f},
     {0, 0, 0, 0}, {0, 1, 0}, -0.003f, 39, 30, 0.09f, 1.435f, 0.201f, true, 3, 3},
    {"oak_small", 30895, true, 3, {0, 54, 58, 32}, {4, 2, 3, 0}, {0.07f, -0.08f, 0.11f, 0.09f}, {28.08f, 4.55f, 9.78f, 7.16f},
     {1.0f, 1.02f, 0.69f, 1.19f}, {16, 9, 8, 1}, {7, 5, 3, 3}, {0, 0.49f, 0.06f, 0.12f}, {0.73f, 0.42f, 0.69f, 0.75f},
     {-0.23f, 0.42f, 0, 0}, {0, 1, 0}, 0.01f, 42, 14, 0.16f, 1.38f, 0.7f, true, 0, 0},
    {"pine_small", 11744, false, 1, {0, 117, 60, 60}, {91, 7, 5, 0}, {0.05f, 0.08f, 0, 0}, {39.55f, 12.12f, 10, 1},
     {0.55f, 0.41f, 0.7f, 0.7f}, {12, 10, 8, 6}, {8, 6, 4, 3}, {0, 0.16f, 0.3f, 0.3f}, {0.7f, 0.7f, 0.7f, 0.7f},
     {0, 0, 0, 0}, {0, 1, 0}, 0, 10, 21, 0, 0.965f, 0.7f, true, 3, 3},
    {"bush_1", 45590, true, 3, {0, 21.52f, 62.61f, 60}, {7, 3, 2, 0}, {0.11f, 0.09f, 0.05f, 0.09f}, {0.1f, 15.3f, 5.59f, 4.6f},
     {0.579f, 0.952f, 0.76f, 0.7f}, {6, 6, 10, 10}, {4, 4, 4, 3}, {0, 0.53f, 0.33f, 0}, {0.7f, 0.7f, 0.7f, 0.7f},
     {0.3f, -0.07f, 0, 0}, {0, 1, 0}, 0, 55, 12, 0, 2.45f, 0.717f, true, 1, 1},
};
// the grown variants: (preset, seed offset)
constexpr std::array<std::pair<int, int>, 9> kVariants = {{{0, 0}, {0, 101}, {4, 0}, {1, 0}, {2, 0}, {3, 0}, {3, 77}, {5, 0}, {6, 0}}};
enum : int
{
    VOak0 = 0, VOak1, VOakSmall, VAsh, VAspen, VPine0, VPine1, VPineSmall, VBush
};

struct Mesh
{
    std::vector<WgrMeshVertex> v;
    std::vector<uint32_t> idx;
};
void Push(Mesh& m, P3 p, P3 n, float u, float v)
{
    WgrMeshVertex w{};
    w.pos = {p.x, p.y, p.z};
    w.normal = {n.x, n.y, n.z};
    w.uv = {u, v};
    w.uv1 = w.uv;
    m.v.push_back(w);
}

// tree.js #generateSkeleton / #growBranch / generateChildBranches / generateLeaves / #mesh*
class Grower
{
  public:
    Grower(const Preset& p, uint32_t seed) : _p(p), _rng(seed) {}
    void Grow(Mesh& bark, Mesh& leaves)
    {
        _queue.push_back({{}, {}, _p.length[0], _p.radius[0], 0, _p.sections[0], _p.segments[0]});
        while (!_queue.empty())
        {
            Branch b = _queue.front();
            _queue.pop_front();
            GrowBranch(b, bark);
        }
        for (const Leaf& l : _leaves)
            MeshLeaf(leaves, l);
    }

  private:
    struct Branch
    {
        P3 origin;
        Euler orient;
        float length, radius;
        int level, sectionCount, segmentCount;
    };
    struct Section
    {
        P3 origin;
        Euler orient;
        float radius;
    };
    struct Leaf
    {
        P3 origin;
        Euler orient;
        float size;
    };

    std::vector<int> Shuffled(int count)
    {
        std::vector<int> a(size_t(std::max(count, 0)));
        for (int k = 0; k < count; ++k)
            a[size_t(k)] = k;
        for (int k = count - 1; k > 0; --k)
        {
            const int r = int(std::floor(_rng.Random() * (k + 1)));
            std::swap(a[size_t(k)], a[size_t(r)]);
        }
        return a;
    }
    void RecordLeaf(P3 origin, Euler orient)
    {
        const float size = _p.leafSize * (1 + _rng.Random(_p.leafSizeVariance, -_p.leafSizeVariance));
        _leaves.push_back({origin, orient, size});
    }
    void GrowBranch(const Branch& b, Mesh& bark)
    {
        Euler so = b.orient;
        P3 sorg = b.origin;
        const float sectionLength = b.length / std::max(b.sectionCount, 1); // ez-tree's 'Deciduous' test never matches
        std::vector<Section> sections;
        for (int i = 0; i <= b.sectionCount; ++i)
        {
            float r = b.radius;
            if (i == b.sectionCount && b.level == _p.levels)
                r = 0.001f;
            else if (_p.deciduous)
                r *= 1 - _p.taper[b.level] * (float(i) / b.sectionCount);
            else
                r *= 1 - float(i) / b.sectionCount;
            sections.push_back({sorg, so, r});
            sorg = sorg + Rotate({0, sectionLength, 0}, FromEuler(so));
            const float g = std::max(1.0f, 1.0f / std::sqrt(std::max(r, 1e-6f))) * _p.gnarliness[b.level];
            so.x += _rng.Random(g, -g);
            so.z += _rng.Random(g, -g);
            Quat qs = Mul(FromEuler(so), AxisAngle({0, 1, 0}, _p.twist[b.level]));
            const P3 up = Rotate({0, 1, 0}, qs), target = Norm(_p.forceDir);
            P3 axis = Cross(up, target);
            const float sinFull = Len(axis);
            if (sinFull > 1e-6f)
            {
                axis = axis * (1.0f / sinFull);
                const float full = std::atan2(sinFull, Dot(up, target));
                const float step = _p.forceStrength / std::max(r, 1e-6f);
                qs = Mul(AxisAngle(axis, std::clamp(step, -full, full)), qs);
            }
            so = ToEuler(qs);
        }
        MeshBranch(bark, sections, b.segmentCount, b.radius);
        if (_p.deciduous)
        {
            const Section& last = sections.back();
            if (b.level < _p.levels)
                _queue.push_back({last.origin, last.orient, _p.length[b.level + 1], last.radius, b.level + 1, b.sectionCount, b.segmentCount});
            else
                RecordLeaf(last.origin, last.orient);
        }
        if (b.level == _p.levels)
            Spawn(sections, _p.leafCount, b.level, true);
        else if (b.level < _p.levels)
            Spawn(sections, _p.children[b.level], b.level + 1, false);
    }
    // generateChildBranches / generateLeaves (the same stratified placement)
    void Spawn(const std::vector<Section>& sections, int count, int level, bool leaves)
    {
        if (count <= 0 || sections.size() < 2)
            return;
        const float radialOffset = _rng.Random();
        const float startMin = leaves ? _p.leafStart : _p.start[level];
        const float heightStep = (1.0f - startMin) / count;
        const std::vector<int> slots = Shuffled(count);
        const int n = int(sections.size());
        for (int i = 0; i < count; ++i)
        {
            const float start = startMin + (i + _rng.Random()) * heightStep;
            const int si = std::min(int(std::floor(start * (n - 1))), n - 1);
            const Section& A = sections[size_t(si)];
            const Section& B = si == n - 1 ? A : sections[size_t(si) + 1];
            const float alpha = (start - float(si) / (n - 1)) / (1.0f / (n - 1));
            const P3 origin = Lerp(A.origin, B.origin, alpha);
            const Euler parent = ToEuler(Slerp(FromEuler(B.orient), FromEuler(A.orient), alpha));
            const float jitter = _rng.Random(0.5f, -0.5f);
            const float radial = Tau * (radialOffset + (slots[size_t(i)] + jitter) / count);
            const float angle = (leaves ? _p.leafAngle : _p.angle[level]) * (Pi / 180.0f);
            const Quat q = Mul(FromEuler(parent), Mul(AxisAngle({0, 1, 0}, radial), AxisAngle({1, 0, 0}, angle)));
            if (leaves)
            {
                RecordLeaf(origin, ToEuler(q));
                continue;
            }
            const float radius = _p.radius[level] * ((1 - alpha) * A.radius + alpha * B.radius);
            const float length = _p.length[level] * (_p.deciduous ? 1.0f : 1.0f - start);
            _queue.push_back({origin, ToEuler(q), length, radius, level, _p.sections[level], _p.segments[level]});
        }
    }
    void MeshBranch(Mesh& m, const std::vector<Section>& sections, int segmentCount, float baseRadius)
    {
        const int segments = std::max(3, segmentCount);
        const float wraps = std::max(1.0f, std::round(baseRadius));
        const uint32_t base = uint32_t(m.v.size());
        for (size_t k = 0; k < sections.size(); ++k)
        {
            const Section& s = sections[k];
            const Quat q = FromEuler(s.orient);
            for (int j = 0; j <= segments; ++j)
            {
                const float a = Tau * float(j % segments) / segments;
                const P3 dir = Rotate({std::cos(a), 0, std::sin(a)}, q);
                Push(m, s.origin + dir * s.radius, Norm(dir), float(j) / segments * wraps, float(k) * 0.35f);
            }
        }
        const uint32_t N = uint32_t(segments + 1);
        for (uint32_t i = 0; i + 1 < sections.size(); ++i)
            for (uint32_t j = 0; j < uint32_t(segments); ++j)
            {
                const uint32_t v1 = base + i * N + j, v2 = v1 + 1, v3 = v1 + N, v4 = v2 + N;
                m.idx.insert(m.idx.end(), {v1, v3, v2, v2, v3, v4});
            }
    }
    // #meshLeaf, rounded normals, both faces (a quad must be seen from behind too)
    void MeshLeaf(Mesh& m, const Leaf& l)
    {
        const Quat q = FromEuler(l.orient);
        const float W = l.size, L = l.size;
        for (int pass = 0; pass < (_p.leafDouble ? 2 : 1); ++pass)
        {
            const Quat r = Mul(q, AxisAngle({0, 1, 0}, pass * Pi / 2));
            const P3 corners[4] = {{-W / 2, L, 0}, {-W / 2, 0, 0}, {W / 2, 0, 0}, {W / 2, L, 0}};
            const float uv[4][2] = {{0, 1}, {0, 0}, {1, 0}, {1, 1}};
            const P3 n = Rotate({0, 0, 1}, r);
            for (int face = 0; face < 2; ++face)
            {
                const uint32_t b = uint32_t(m.v.size());
                for (int c = 0; c < 4; ++c)
                {
                    const P3 p = Rotate(corners[c], r) + l.origin;
                    const P3 rn = Norm(n * (face == 0 ? 1.0f : -1.0f) + (p - l.origin) * (1.0f / std::max(l.size, 1e-3f)));
                    Push(m, p, rn, uv[c][0], uv[c][1]);
                }
                if (face == 0)
                    m.idx.insert(m.idx.end(), {b, b + 1, b + 2, b, b + 2, b + 3});
                else
                    m.idx.insert(m.idx.end(), {b, b + 2, b + 1, b, b + 3, b + 2});
            }
        }
    }

    const Preset& _p;
    Rng _rng;
    std::deque<Branch> _queue;
    std::vector<Leaf> _leaves;
};

float Hash01(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return (x >> 8) / 16777216.0f;
}
float Noise2(float x, float y, uint32_t s)
{
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    const float fx = x - ix, fy = y - iy, ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy);
    auto h = [&](int a, int b) { return Hash01(uint32_t(ix + a) * 73856093u ^ uint32_t(iy + b) * 19349663u ^ s); };
    const float a = h(0, 0) + (h(1, 0) - h(0, 0)) * ux, b = h(0, 1) + (h(1, 1) - h(0, 1)) * ux;
    return a + (b - a) * uy;
}

// A sheet of leaves on a twig, the stem at v = 0 (ez-tree's leaf quads hang from their base):
// oak (lobed), ash (narrow leaflets on a rachis), aspen (round), pine (needles).
std::vector<uint8_t> LeafTexture(int kind, int size)
{
    std::vector<uint8_t> px(size_t(size) * size * 4, 0);
    struct Blade
    {
        float ox, oy, dx, dy, len, width, lobes;
    };
    std::vector<Blade> blades;
    const float S = float(size);
    auto add = [&](float ox, float oy, float ang, float len, float width, float lobes)
    {
        blades.push_back({ox, oy, std::sin(ang), std::cos(ang), len, width, lobes});
    };
    uint32_t seed = 1234u + uint32_t(kind) * 77u;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
    if (kind == 3)
    {
        for (int i = 0; i < 70; ++i)
        {
            const float t = rnd();
            add(0.5f + (rnd() - 0.5f) * 0.06f, t * 0.85f, (rnd() - 0.5f) * 2.4f, 0.22f + rnd() * 0.15f, 0.012f, 0);
        }
    }
    else if (kind == 1)
    {
        for (int i = 0; i < 9; ++i)
        {
            const float t = 0.12f + i * 0.095f, side = (i % 2) ? 1.0f : -1.0f;
            add(0.5f, t, side * (0.9f + rnd() * 0.3f) + (i == 8 ? -side * 0.9f : 0), 0.28f, 0.06f, 0);
        }
    }
    else
    {
        const int n = kind == 2 ? 6 : 6;
        for (int i = 0; i < n; ++i)
        {
            const float ang = (float(i) / (n - 1) - 0.5f) * 1.9f + (rnd() - 0.5f) * 0.3f;
            add(0.5f + (rnd() - 0.5f) * 0.1f, 0.05f + rnd() * 0.25f, ang, kind == 2 ? 0.34f : 0.44f, kind == 2 ? 0.17f : 0.12f,
                kind == 0 ? 5.0f : 0.0f);
        }
    }
    struct Rgb
    {
        float r, g, b;
    };
    static const Rgb base[4] = {{0.25f, 0.33f, 0.12f}, {0.3f, 0.4f, 0.16f}, {0.36f, 0.46f, 0.17f}, {0.16f, 0.25f, 0.12f}};
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            const float u = (x + 0.5f) / S, v = (y + 0.5f) / S;
            float best = -1;
            for (const Blade& b : blades)
            {
                const float rx = u - b.ox, ry = v - b.oy;
                const float s = rx * b.dx + ry * b.dy, t = -rx * b.dy + ry * b.dx;
                if (s < 0 || s > b.len)
                    continue;
                const float f = s / b.len;
                float w = b.width * std::pow(std::sin(Pi * f), 0.7f);
                if (b.lobes > 0)
                    w *= 0.75f + 0.25f * std::fabs(std::sin(f * b.lobes * Pi));
                if (std::fabs(t) < w)
                    best = std::max(best, 1.0f - std::fabs(t) / std::max(w, 1e-4f) * 0.5f + (kind == 3 ? 0.3f : 0.0f) - (std::fabs(t) < w * 0.08f ? 0.35f : 0.0f));
            }
            // the twig
            const bool twig = std::fabs(u - 0.5f) < 0.012f && v < 0.85f;
            uint8_t* q = &px[(size_t(y) * size + x) * 4];
            if (best < 0 && !twig)
                continue;
            const float n = Noise2(u * 24, v * 24, uint32_t(kind) * 13u);
            Rgb c = twig && best < 0 ? Rgb{0.24f, 0.19f, 0.13f} : base[kind];
            const float k = (twig && best < 0) ? 1.0f : (0.8f + 0.35f * best) * (0.85f + 0.3f * n);
            q[0] = uint8_t(std::clamp(c.r * k, 0.0f, 1.0f) * 255);
            q[1] = uint8_t(std::clamp(c.g * k, 0.0f, 1.0f) * 255);
            q[2] = uint8_t(std::clamp(c.b * k, 0.0f, 1.0f) * 255);
            q[3] = 255;
        }
    return px;
}
std::vector<uint8_t> BarkTexture(int kind, int w, int h)
{
    std::vector<uint8_t> px(size_t(w) * h * 4);
    struct Rgb
    {
        float r, g, b;
    };
    static const Rgb base[4] = {{0.33f, 0.28f, 0.23f}, {0.42f, 0.4f, 0.37f}, {0.74f, 0.73f, 0.67f}, {0.42f, 0.28f, 0.19f}};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const float u = float(x) / w, v = float(y) / h;
            const float ridge = Noise2(u * 9.0f, v * 2.0f, 7u + uint32_t(kind)), fine = Noise2(u * 40.0f, v * 12.0f, 9u);
            float k = 0.7f + 0.45f * ridge + 0.15f * (fine - 0.5f);
            Rgb c = base[kind];
            // aspen and birch: dark lenticel marks on the pale bark
            if (kind == 2 && Noise2(u * 6.0f, v * 18.0f, 31u) > 0.78f)
                c = {0.12f, 0.11f, 0.1f}, k = 1.0f;
            uint8_t* q = &px[(size_t(y) * w + x) * 4];
            q[0] = uint8_t(std::clamp(c.r * k, 0.0f, 1.0f) * 255);
            q[1] = uint8_t(std::clamp(c.g * k, 0.0f, 1.0f) * 255);
            q[2] = uint8_t(std::clamp(c.b * k, 0.0f, 1.0f) * 255);
            q[3] = 255;
        }
    return px;
}

bool ContainsAny(const std::string& s, std::initializer_list<const char*> parts)
{
    for (const char* p : parts)
        if (s.find(p) != std::string::npos)
            return true;
    return false;
}
} // namespace

ProceduralTrees::~ProceduralTrees() = default;

ProceduralTrees::Variant& ProceduralTrees::Get(int variant)
{
    if (_variants.empty())
        _variants.resize(kVariants.size());
    Variant& v = _variants[size_t(variant)];
    if (v.built)
        return v;
    v.built = true;
    const auto [preset, seedOffset] = kVariants[size_t(variant)];
    const Preset& p = kPresets[preset];
    Mesh bark, leaves;
    Grower(p, p.seed + uint32_t(seedOffset)).Grow(bark, leaves);
    float top = 1.0f;
    for (const auto& w : bark.v)
        top = std::max(top, w.pos.y);
    for (const auto& w : leaves.v)
        top = std::max(top, w.pos.y);
    v.height = top;
    EngineWgpu::AcquireProducerWindow("ez-tree meshes");
    if (!bark.idx.empty())
        v.bark = wgr_mesh_create(_renderer, WgrSlice<WgrMeshVertex>(bark.v), WgrSlice<uint32_t>(bark.idx));
    if (!leaves.idx.empty())
        v.leaves = wgr_mesh_create(_renderer, WgrSlice<WgrMeshVertex>(leaves.v), WgrSlice<uint32_t>(leaves.idx));
    v.barkIndices = uint32_t(bark.idx.size());
    v.leafIndices = uint32_t(leaves.idx.size());
    const auto barkPx = BarkTexture(p.bark, 64, 128);
    v.barkTex = wgr_texture_create(_renderer, 64, 128, WGR_TEXTURE_RGBA8, 1, 0, barkPx.data(), uint32_t(barkPx.size()));
    const auto leafPx = LeafTexture(p.leaf, 128);
    v.leafTex = wgr_texture_create(_renderer, 128, 128, WGR_TEXTURE_RGBA8, 1, 0, leafPx.data(), uint32_t(leafPx.size()));
    LOG_INFO(Graphics, "ez-tree: grew '{}' (seed {}): {} bark and {} leaf triangles, {:.1f} units tall", p.name,
             p.seed + seedOffset, bark.idx.size() / 3, leaves.idx.size() / 3, top);
    return v;
}

void ProceduralTrees::Release(Claim& c)
{
    Object* obj = c.obj;
    if (!obj)
        return;
    obj->SetGpuCoverage(static_cast<int>(GpuDrawCoverage::None));
    if (c.retained)
        _engine.SceneObjectCreated(obj);
    else
        obj->SetGpuCoverage(c.coverage);
}

void ProceduralTrees::ReleaseAll()
{
    for (auto& [key, c] : _claims)
        Release(c);
    _claims.clear();
}

void ProceduralTrees::Update(const Vector3& camera)
{
    const auto& test = Dev::ProceduralTrees();
    if (!test.enabled || !GLandscape || !_renderer)
    {
        if (_wasEnabled)
        {
            ReleaseAll();
            LOG_INFO(Graphics, "ez-tree test: off, the original trees are back");
        }
        _wasEnabled = false;
        return;
    }
    if (!_wasEnabled)
        LOG_INFO(Graphics, "ez-tree test: on (range {:.0f} m, at most {} trees)", test.range, test.maxTrees);
    _wasEnabled = true;
    _scan -= 1.0f / 60.0f;
    if (_scan <= 0)
    {
        _scan = 0.5f;
        // the trees within range, nearest first
        std::vector<std::pair<float, Object*>> nearby;
        const float grid = GLandscape->GetLandGrid(), r = test.range;
        const int range = GLandscape->GetLandRange();
        const int x0 = std::max(0, int((camera.X() - r) / grid)), x1 = std::min(range - 1, int((camera.X() + r) / grid));
        const int z0 = std::max(0, int((camera.Z() - r) / grid)), z1 = std::min(range - 1, int((camera.Z() + r) / grid));
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x)
            {
                const ObjectList& list = GLandscape->GetObjects(z, x);
                for (int i = 0; i < list.Size(); ++i)
                {
                    Object* obj = list[i];
                    if (!obj || obj->IsDestroyed())
                        continue;
                    const LODShapeWithShadow* shape = obj->GetShape();
                    if (!shape || (shape->GetMapType() != MapTree && shape->GetMapType() != MapSmallTree))
                        continue;
                    const float d = std::hypot(obj->Position().X() - camera.X(), obj->Position().Z() - camera.Z());
                    if (d <= r)
                        nearby.emplace_back(d, obj);
                }
            }
        std::sort(nearby.begin(), nearby.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        if (int(nearby.size()) > test.maxTrees)
            nearby.resize(size_t(test.maxTrees));
        std::unordered_map<const Object*, bool> keep;
        for (const auto& [d, obj] : nearby)
            keep[obj] = true;
        for (auto it = _claims.begin(); it != _claims.end();)
        {
            Object* obj = it->second.obj;
            if (!obj || !keep.count(it->first) || obj->IsDestroyed())
            {
                Release(it->second);
                it = _claims.erase(it);
            }
            else
                ++it;
        }
        for (const auto& [d, obj] : nearby)
        {
            if (_claims.count(obj))
                continue;
            const LODShapeWithShadow* shape = obj->GetShape();
            std::string name = shape->Name() ? shape->Name() : "";
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            const Vector3 lo = obj->PositionModelToWorld(Vector3(0, shape->Min().Y(), 0));
            const Vector3 hi = obj->PositionModelToWorld(Vector3(0, shape->Max().Y(), 0));
            const float height = std::max(hi.Y() - lo.Y(), 1.0f);
            const uint32_t h = uint32_t(std::lround(obj->Position().X() * 7.0f)) * 73856093u ^ uint32_t(std::lround(obj->Position().Z() * 7.0f)) * 19349663u;
            const float pick = Hash01(h);
            const bool small = shape->GetMapType() == MapSmallTree || height < 5.0f;
            int variant;
            if (ContainsAny(name, {"smrk", "jedl", "borov", "modrin", "pine", "spruce", "fir", "jehl", "conif", "picea", "pinus"}))
                variant = small ? VPineSmall : (pick < 0.5f ? VPine0 : VPine1);
            else if (ContainsAny(name, {"briz", "topol", "osik", "aspen", "birch", "poplar"}))
                variant = VAspen;
            else if (ContainsAny(name, {"jasan", "ash"}))
                variant = VAsh;
            else if (small)
                variant = pick < 0.5f ? VBush : VOakSmall;
            else
                variant = pick < 0.4f ? VOak0 : pick < 0.8f ? VOak1 : VAsh;
            Claim c;
            c.obj = obj;
            c.key = obj;
            c.coverage = obj->GetGpuCoverage();
            c.retained = c.coverage != static_cast<int>(GpuDrawCoverage::None);
            c.variant = variant;
            c.x = obj->Position().X(), c.z = obj->Position().Z();
            c.y = std::min(lo.Y(), Wildlife::Ground(c.x, c.z)) ;
            c.yaw = pick * Tau;
            const Variant& v = Get(variant);
            c.scale = height / std::max(v.height, 1e-3f);
            if (c.retained)
                _engine.SceneObjectRemoved(obj);
            obj->SetGpuCoverage(static_cast<int>(GpuDrawCoverage::Full)); // the CPU pass skips it too
            _claims.emplace(obj, c);
        }
    }
    for (const auto& [key, c] : _claims)
    {
        const Variant& v = Get(c.variant);
        const float cs = std::cos(c.yaw) * c.scale, sn = std::sin(c.yaw) * c.scale, s = c.scale;
        const float world[16] = {cs, 0, -sn, 0, 0, s, 0, 0, sn, 0, cs, 0, c.x, c.y, c.z, 1};
        if (v.bark)
            _engine.SubmitDynamicMesh(v.bark, v.barkIndices, world, v.barkTex, 2);
        if (v.leaves)
            _engine.SubmitDynamicMesh(v.leaves, v.leafIndices, world, v.leafTex, 2, 0.5f);
    }
}
} // namespace Poseidon
