// SPDX-License-Identifier: GPL-3.0-or-later
//
// Whole-world trees near the camera, after Tidewater's vegetation (dgreenheck/tidewater, MIT,
// (c) DRG Software Solutions LLC; see CREDITS.md and docs/roadmap/modernisation/Tidewater/
// PARITY-AND-PORT-CANDIDATES-20261010.md section 5). The technique of src/world/Vegetation.js
// and vegetation/PlantGeometry.js, re-implemented here:
//   * procedural meshes built once in a local frame (base at the origin, +y up, unit height,
//     the instance's height applied as its scale);
//   * broadleaf: a buttressed trunk forking into curved limbs (tubes along quadratic Beziers)
//     that end in leaf lobes, each lobe filled with crossing leaf-cluster cards biased to its
//     shell, emitted outermost first so the depth test rejects the inner ones; card normals
//     blend the lobe sphere and the whole crown (volumetric shading);
//   * variants without more meshes in spirit: a lobe table per variant scales or drops lobes;
//   * conifers: tiers of drooping branch sprays (crossed cards) around a tapering trunk.
// Leaves, sprays and bark are one procedural RGBA atlas per foliage type, alpha-tested.
//
// What OP does not port yet: the impostors, the wind deformation and the Bayer cross-fade.
// Here a tree switches between two detail levels and the coarse batch (EarthTreeMesh.hpp)
// carries it beyond 180 m; the trees drawn here are collapsed out of that batch (Hide), so a
// coarse crown never shows through a detailed one.

#include "EarthTreeDetail.hpp"
#include "EngineWgpu.hpp"
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <random>

namespace Poseidon
{
namespace
{
constexpr float Tau = 6.2831853f, Pi = 3.14159265f;
constexpr int BroadleafVariants = 3, ConiferVariants = 2, Variants = BroadleafVariants + ConiferVariants, Lods = 2;
constexpr float DrawRange = 180.0f, NearRange = 70.0f, Rehide = 25.0f;

struct TV
{
    float x = 0, y = 0, z = 0;
};
TV operator+(TV a, TV b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
TV operator-(TV a, TV b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
TV operator*(TV a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(TV a, TV b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
TV Cross(TV a, TV b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
TV Norm(TV a)
{
    const float l = std::sqrt(Dot(a, a));
    return l > 1e-9f ? a * (1.0f / l) : TV{0, 1, 0};
}
TV Perp(TV v) { return Norm(Cross(v, std::fabs(v.y) < 0.9f ? TV{0, 1, 0} : TV{1, 0, 0})); }
float Len(TV a) { return std::sqrt(Dot(a, a)); }

struct Rng
{
    std::mt19937 g;
    explicit Rng(uint32_t s) : g(s) {}
    float operator()() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(g); }
    float R(float a, float b) { return a + (b - a) * (*this)(); }
    TV Unit()
    {
        const float z = R(-1, 1), a = R(0, Tau), r = std::sqrt(std::max(0.0f, 1 - z * z));
        return {r * std::cos(a), z, r * std::sin(a)};
    }
};

struct Mesh
{
    std::vector<WgrMeshVertex> v;
    std::vector<uint32_t> idx;
    uint32_t Add(TV p, TV n, float u, float tv)
    {
        WgrMeshVertex w{};
        w.pos = {p.x, p.y, p.z};
        w.normal = {n.x, n.y, n.z};
        w.uv = {u, tv};
        w.uv1 = w.uv;
        v.push_back(w);
        return uint32_t(v.size() - 1);
    }
};

// a bark tube along points, radii per point; bark in the atlas' right half
void Tube(Mesh& m, const std::vector<TV>& pts, const std::vector<float>& radii, int sides, float v0, float v1,
          float buttress = 0)
{
    const int n = int(pts.size());
    TV prevN{};
    bool have = false;
    const uint32_t base = uint32_t(m.v.size());
    for (int i = 0; i < n; ++i)
    {
        const TV T = Norm(pts[std::min(n - 1, i + 1)] - pts[std::max(0, i - 1)]);
        TV N = have ? prevN : Perp(T);
        N = Norm(N - T * Dot(N, T));
        const TV B = Cross(T, N);
        prevN = N, have = true;
        const float t = n > 1 ? float(i) / (n - 1) : 0;
        for (int j = 0; j <= sides; ++j)
        {
            const float a = float(j) / sides * Tau;
            // buttress roots: flared lobes near the ground
            const float flare = 1 + buttress * std::max(0.0f, 1 - t * 4) * std::fabs(std::sin(a * 2.5f));
            const TV dir = N * std::cos(a) + B * std::sin(a);
            m.Add(pts[i] + dir * (radii[i] * flare), dir, 0.5f + 0.5f * float(j) / sides, v0 + (v1 - v0) * t);
        }
    }
    for (int i = 0; i + 1 < n; ++i)
        for (int j = 0; j < sides; ++j)
        {
            const uint32_t a = base + i * (sides + 1) + j, b = a + 1, c = a + sides + 1, d = c + 1;
            m.idx.insert(m.idx.end(), {a, c, b, b, c, d});
        }
}

// one card (both faces drawn: the pipeline is double-sided), corners' normals blended with `n0`
void Card(Mesh& m, TV centre, TV t, TV b, float halfT, float halfB, TV centreA, TV centreB, float blend, float u0 = 0,
          float u1 = 0.5f, float v0 = 0, float v1 = 1)
{
    const TV corners[4] = {centre - t * halfT - b * halfB, centre + t * halfT - b * halfB, centre + t * halfT + b * halfB,
                           centre - t * halfT + b * halfB};
    const float us[4] = {u0, u1, u1, u0}, vs[4] = {v0, v0, v1, v1};
    const uint32_t base = uint32_t(m.v.size());
    for (int k = 0; k < 4; ++k)
    {
        const TV n = Norm(Norm(corners[k] - centreA) * (1 - blend) + Norm(corners[k] - centreB) * blend);
        m.Add(corners[k], n, us[k], vs[k]);
    }
    m.idx.insert(m.idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

TV Bezier(TV a, TV c, TV b, float t) { return a * ((1 - t) * (1 - t)) + c * (2 * (1 - t) * t) + b * (t * t); }

// Broadleaf (Vegetation.js broadleaf): trunk, limbs, lobes of leaf-cluster cards
Mesh BuildBroadleaf(int variant, int lod)
{
    Rng rng(1000u + variant * 77u);
    Mesh m;
    const TV crownC{0, 0.66f, 0};
    const TV crownR{0.31f, 0.25f, 0.31f};
    const float lean = rng.R(-0.02f, 0.02f);
    // trunk
    {
        std::vector<TV> pts;
        std::vector<float> radii;
        for (int i = 0; i <= 5; ++i)
        {
            const float y = 0.46f * i / 5;
            pts.push_back({lean * y / 0.46f, y, 0});
            radii.push_back(0.032f - 0.012f * float(i) / 5);
        }
        Tube(m, pts, radii, lod == 0 ? 8 : 5, 0, 1, 0.45f);
    }
    // lobes: the variant's table drops some and scales the rest
    struct Lobe
    {
        TV c;
        float r;
    };
    std::vector<Lobe> lobes;
    const int nl = 8;
    for (int i = 0; i < nl; ++i)
    {
        TV d = rng.Unit();
        d.y = d.y * 0.7f + 0.25f;
        d = Norm(d);
        const float k = rng.R(0.5f, 0.78f);
        const float keep = rng(), scale = rng.R(0.8f, 1.25f);
        if (keep < 0.12f + 0.08f * variant && i > 2)
            continue;
        lobes.push_back({crownC + TV{d.x * crownR.x * k, d.y * crownR.y * k, d.z * crownR.z * k}, rng.R(0.12f, 0.16f) * scale});
    }
    // a central top lobe keeps the crown closed
    lobes.push_back({crownC + TV{0, crownR.y * 0.55f, 0}, 0.15f});
    // limbs: quadratic Beziers from the trunk top out to each lobe
    for (const Lobe& l : lobes)
    {
        const TV a{lean, rng.R(0.36f, 0.45f), 0};
        const TV c{l.c.x * 0.35f, (a.y + l.c.y) * 0.5f + 0.05f, l.c.z * 0.35f};
        std::vector<TV> pts;
        std::vector<float> radii;
        const int segs = lod == 0 ? 5 : 3;
        for (int i = 0; i <= segs; ++i)
        {
            const float t = float(i) / segs;
            pts.push_back(Bezier(a, c, l.c, t * 0.85f));
            radii.push_back(0.016f - 0.010f * t);
        }
        Tube(m, pts, radii, lod == 0 ? 5 : 3, 0.2f, 0.9f);
    }
    // leaf-cluster cards, biased to each lobe's shell, emitted outermost first
    struct C
    {
        TV p, t, b;
        float s;
        TV lobe;
        float dist;
    };
    std::vector<C> cards;
    const int perLobe = lod == 0 ? 11 : 5;
    for (const Lobe& l : lobes)
        for (int k = 0; k < perLobe; ++k)
        {
            const TV dir = rng.Unit();
            const float rr = l.r * (0.45f + 0.5f * std::sqrt(rng()));
            const TV p = l.c + dir * rr;
            const TV out = Norm(p - l.c);
            const TV n = Norm(rng.Unit() * 0.6f + out * 0.8f);
            const float spin = rng.R(0, Tau);
            const TV t0 = Perp(n), b0 = Cross(n, t0);
            const TV t = t0 * std::cos(spin) + b0 * std::sin(spin), b = Cross(n, t);
            const float s = l.r * rng.R(0.85f, 1.25f) * (lod == 0 ? 1.0f : 1.35f);
            cards.push_back({p, t, b, s, l.c, Len(p - crownC)});
        }
    std::sort(cards.begin(), cards.end(), [](const C& a, const C& b) { return a.dist > b.dist; });
    for (const C& c : cards)
        Card(m, c.p, c.t, c.b, c.s * 0.5f, c.s * 0.5f, c.lobe, crownC, 0.5f);
    return m;
}

// Conifer: a tapering trunk and tiers of drooping branch sprays (crossed cards)
Mesh BuildConifer(int variant, int lod)
{
    Rng rng(2000u + variant * 31u);
    Mesh m;
    {
        std::vector<TV> pts;
        std::vector<float> radii;
        for (int i = 0; i <= 6; ++i)
        {
            pts.push_back({0, float(i) / 6, 0});
            radii.push_back(0.022f - 0.018f * float(i) / 6);
        }
        Tube(m, pts, radii, lod == 0 ? 6 : 4, 0, 1, 0.25f);
    }
    const int tiers = lod == 0 ? 12 : 8;
    const float slender = variant == 0 ? 1.0f : 0.82f;
    for (int k = 0; k < tiers; ++k)
    {
        const float y = 0.14f + (0.97f - 0.14f) * k / (tiers - 1);
        const float rTier = (0.30f * std::pow(1 - (y - 0.14f) / 0.86f, 0.85f) + 0.04f) * slender;
        const int branches = (lod == 0 ? 7 : 5) + (k < tiers / 3 ? 1 : 0);
        const float off = rng.R(0, Tau);
        for (int j = 0; j < branches; ++j)
        {
            const float a = off + Tau * j / branches + rng.R(-0.2f, 0.2f);
            const TV root{0, y, 0};
            // the branch droops toward its tip, which makes the tiers read as a cone from the side
            const TV tip{std::cos(a) * rTier, y - 0.32f * rTier - 0.02f, std::sin(a) * rTier};
            const TV d = Norm(tip - root);
            const TV side = Norm(Cross(d, TV{0, 1, 0}));
            const TV up = Cross(side, d);
            const TV mid = (root + tip) * 0.5f;
            const float len = Len(tip - root) * 0.62f, w = 0.06f + rTier * 0.5f;
            // the spray, rolled about the branch so it is never seen edge-on from the ground, and
            // a second one crossing it; normals lean away from a point below the tier, so the
            // drooping sprays take the sky light the way a needle mass does instead of facing down
            const float roll = rng.R(0.45f, 0.8f) * (j % 2 ? 1.0f : -1.0f);
            const TV across = side * std::cos(roll) + up * std::sin(roll);
            const TV across2 = side * std::cos(roll + 1.5708f) + up * std::sin(roll + 1.5708f);
            Card(m, mid, across, d, w * 0.5f, len, root, root + TV{0, -0.6f, 0}, 0.6f);
            Card(m, mid, across2, d, w * (lod == 0 ? 0.35f : 0.42f), len, root, root + TV{0, -0.6f, 0}, 0.6f);
        }
    }
    return m;
}
// ---------------------------------------------------------------- the atlas

struct Px
{
    std::vector<uint8_t> d;
    int w, h;
    void Set(int x, int y, float r, float g, float b, float a)
    {
        if (x < 0 || y < 0 || x >= w || y >= h)
            return;
        uint8_t* p = &d[(y * w + x) * 4];
        p[0] = uint8_t(std::clamp(r, 0.0f, 1.0f) * 255), p[1] = uint8_t(std::clamp(g, 0.0f, 1.0f) * 255);
        p[2] = uint8_t(std::clamp(b, 0.0f, 1.0f) * 255), p[3] = uint8_t(std::clamp(a, 0.0f, 1.0f) * 255);
    }
};
// The left half: a leaf cluster (broadleaf) or a needle spray (conifer) with alpha; the right
// half: bark.
std::vector<uint8_t> Atlas(bool conifer)
{
    constexpr int W = 256, H = 256;
    Px px{std::vector<uint8_t>(W * H * 4, 0), W, H};
    Rng rng(conifer ? 77u : 55u);
    // the foliage's own colour range (sRGB): a broadleaf's fresh greens, a conifer's dark ones
    const float base[3] = {conifer ? 0.13f : 0.25f, conifer ? 0.23f : 0.38f, conifer ? 0.15f : 0.13f};
    auto leafColour = [&](float k, float r[3])
    {
        for (int c = 0; c < 3; ++c)
            r[c] = base[c] * (0.7f + 0.6f * k);
        r[0] += (rng() - 0.5f) * 0.05f, r[1] += (rng() - 0.5f) * 0.06f;
    };
    const int half = W / 2;
    if (!conifer)
    {
        // twigs from the centre, then ~110 leaves along them and around
        for (int k = 0; k < 110; ++k)
        {
            const float a = rng.R(0, Tau), rr = 0.46f * std::sqrt(rng());
            const float cx = 0.5f + std::cos(a) * rr, cy = 0.5f + std::sin(a) * rr;
            const float len = rng.R(0.07f, 0.12f), wid = len * rng.R(0.38f, 0.5f), ang = rng.R(0, Tau);
            float col[3];
            leafColour(rng(), col);
            const float ca = std::cos(ang), sa = std::sin(ang);
            const int x0 = int((cx - len) * half), x1 = int((cx + len) * half), y0 = int((cy - len) * H), y1 = int((cy + len) * H);
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    const float fx = (x + 0.5f) / half - cx, fy = (y + 0.5f) / H - cy;
                    const float lx = fx * ca + fy * sa, ly = -fx * sa + fy * ca;
                    // a pointed leaf: an ellipse narrowing to its tip
                    const float t = lx / len;
                    const float halfW = wid * std::sqrt(std::max(0.0f, 1 - t * t)) * (1 - 0.35f * std::max(0.0f, t));
                    if (std::fabs(t) <= 1 && std::fabs(ly) <= halfW && x < half)
                    {
                        const float vein = std::fabs(ly) < wid * 0.08f ? 0.85f : 1.0f;
                        const float shade = 0.85f + 0.3f * (0.5f - ly / std::max(halfW, 1e-4f) * 0.5f);
                        px.Set(x, y, col[0] * vein * shade, col[1] * vein * shade, col[2] * vein * shade, 1);
                    }
                }
        }
    }
    else
    {
        // a spray: a twig up the middle (v = 0 at the branch root), a tufted envelope narrowing to
        // the tip, filled with needles slanting tipward. Filled rather than drawn as 1-pixel needles:
        // alpha-tested thin lines vanish in the mips, which is what left the first cut's conifers bare.
        const float tuftPhase = rng.R(0, Tau);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < half; ++x)
            {
                const float v = (y + 0.5f) / H, lat = (x + 0.5f) / half - 0.5f, al = std::fabs(lat);
                const float tuft = 0.82f + 0.18f * std::sin(v * 46 + tuftPhase) * std::sin(v * 17);
                const float halfW = 0.47f * (1 - 0.6f * v) * tuft * std::min(1.0f, v * 9 + 0.35f);
                if (al > halfW)
                    continue;
                // needles: stripes slanting toward the tip, gaps opening toward the envelope's edge
                const float stripe = 0.5f + 0.5f * std::sin((v * 1.0f - al * 0.55f) * H * 0.55f + std::sin(lat * 31) * 0.8f);
                const float edge = al / std::max(halfW, 1e-4f);
                if (edge > 0.55f && stripe < 0.18f + 0.5f * (edge - 0.55f))
                    continue;
                float col[3];
                leafColour(0.35f + 0.65f * stripe, col);
                const float inner = 0.74f + 0.26f * std::min(1.0f, edge * 1.6f);
                if (al < 0.012f)
                    px.Set(x, y, 0.25f, 0.18f, 0.1f, 1);
                else
                    px.Set(x, y, col[0] * inner, col[1] * inner, col[2] * inner, 1);
            }
    }
    // bark: vertical furrows, grey-brown
    for (int y = 0; y < H; ++y)
        for (int x = half; x < W; ++x)
        {
            const float u = float(x - half) / half;
            const float furrow = 0.5f + 0.5f * std::sin(u * Tau * 9 + std::sin(y * 0.07f) * 1.5f);
            const float n = (rng() - 0.5f) * 0.12f;
            const float k = 0.65f + 0.35f * furrow + n;
            px.Set(x, y, (conifer ? 0.36f : 0.33f) * k, (conifer ? 0.26f : 0.28f) * k, (conifer ? 0.18f : 0.22f) * k, 1);
        }
    return px.d;
}
} // namespace

void EarthTreeDetail::EnsureMeshes()
{
    if (_built)
        return;
    _built = true;
    EngineWgpu::AcquireProducerWindow("Earth detail trees");
    _meshes.assign(size_t(Variants) * Lods, 0);
    _indexCounts.assign(_meshes.size(), 0);
    size_t tris = 0;
    for (int v = 0; v < Variants; ++v)
        for (int l = 0; l < Lods; ++l)
        {
            const Mesh m = v < BroadleafVariants ? BuildBroadleaf(v, l) : BuildConifer(v - BroadleafVariants, l);
            _meshes[size_t(v) * Lods + l] = wgr_mesh_create(_renderer, WgrSlice<WgrMeshVertex>(m.v), WgrSlice<uint32_t>(m.idx));
            _indexCounts[size_t(v) * Lods + l] = uint32_t(m.idx.size());
            tris += m.idx.size() / 3;
        }
    const auto broad = Atlas(false), con = Atlas(true);
    _broadleaf = wgr_texture_create(_renderer, 256, 256, WGR_TEXTURE_RGBA8, 1, WGR_TEXTURE_GEN_MIPS, broad.data(), uint32_t(broad.size()));
    _conifer = wgr_texture_create(_renderer, 256, 256, WGR_TEXTURE_RGBA8, 1, WGR_TEXTURE_GEN_MIPS, con.data(), uint32_t(con.size()));
    LOG_INFO(Graphics, "Earth detail trees: {} broadleaf + {} conifer variants x {} levels ({} triangles)", BroadleafVariants,
             ConiferVariants, Lods, tris);
}

void EarthTreeDetail::SetTrees(std::vector<EarthFeatureStream::TreeInstance> trees, WgrMesh coarse,
                               std::vector<WgrMeshVertex> coarseVertices)
{
    _trees = std::move(trees);
    _coarse = coarse;
    _coarseVertices = std::move(coarseVertices);
    _hidden.assign(_trees.size(), 0);
    _hasCentre = false;
    if (_coarseVertices.size() != _trees.size() * EarthTrees::VertexCount)
    {
        LOG_WARN(Graphics, "Earth detail trees: batch has {} vertices for {} trees; coarse trees stay drawn",
                 _coarseVertices.size(), _trees.size());
        _coarse = 0;
    }
}

void EarthTreeDetail::Clear()
{
    _trees.clear();
    _hidden.clear();
    _coarse = 0;
    _coarseVertices.clear();
    _hasCentre = false;
}

// Collapse the coarse trees within DrawRange of `centre` to a point and re-upload the batch if
// that set changed. The detail tier then draws the same set, so with the centre lagging the camera
// by up to Rehide a tree is drawn once, in detail out to DrawRange + Rehide.
void EarthTreeDetail::Hide(const Vector3& centre)
{
    _centre = centre;
    _hasCentre = true;
    bool changed = false;
    for (size_t i = 0; i < _trees.size(); ++i)
    {
        const auto& t = _trees[i];
        const float dx = t.x - centre.X(), dy = t.y - centre.Y(), dz = t.z - centre.Z();
        const uint8_t hide = dx * dx + dy * dy + dz * dz < DrawRange * DrawRange ? 1 : 0;
        changed |= hide != _hidden[i];
        _hidden[i] = hide;
    }
    if (!changed || !_coarse)
        return;
    _upload = _coarseVertices;
    for (size_t i = 0; i < _trees.size(); ++i)
        if (_hidden[i])
            for (size_t v = 0; v < EarthTrees::VertexCount; ++v)
                _upload[i * EarthTrees::VertexCount + v].pos = {_trees[i].x, _trees[i].y, _trees[i].z};
    EngineWgpu::AcquireProducerWindow("Earth detail trees hide");
    wgr_mesh_update(_renderer, _coarse, WgrSlice<WgrMeshVertex>(_upload));
}

void EarthTreeDetail::Draw(const Vector3& camera, float time)
{
    static const bool enabled = []
    {
        const char* v = std::getenv("WGR_EARTH_TREES_DETAIL");
        return !(v && v[0] == '0');
    }();
    if (!enabled || !_renderer || _trees.empty())
        return;
    EnsureMeshes();
    if (!_hasCentre || (camera - _centre).SquareSize() > Rehide * Rehide)
        Hide(camera);
    int drawn = 0;
    for (size_t i = 0; i < _trees.size(); ++i)
    {
        if (!_hidden[i])
            continue;
        const auto& t = _trees[i];
        const float dx = t.x - camera.X(), dy = t.y - camera.Y(), dz = t.z - camera.Z();
        const float dist2 = dx * dx + dy * dy + dz * dz;
        // the species mix by latitude: conifers take over toward the boreal belt
        const uint64_t s = t.seed;
        const float lat = float(std::fabs(t.latitude));
        const float conifers = std::clamp((lat - 38.0f) / 22.0f, 0.0f, 0.85f);
        const bool conifer = float((s >> 8) & 0xffff) / 65535.0f < conifers;
        const int variant = conifer ? BroadleafVariants + int((s >> 24) % ConiferVariants) : int((s >> 24) % BroadleafVariants);
        const int lod = dist2 < NearRange * NearRange ? 0 : 1;
        const size_t mi = size_t(variant) * Lods + lod;
        const float yaw = float((s >> 32) & 0xffff) / 65535.0f * Tau;
        const float h = t.height * (conifer ? 1.25f : 1.0f);
        const float c = std::cos(yaw) * h, sn = std::sin(yaw) * h;
        const float world[16] = {c, 0, -sn, 0, 0, h, 0, 0, sn, 0, c, 0, t.x, t.y, t.z, 1};
        _engine.SubmitDynamicMesh(_meshes[mi], _indexCounts[mi], world, conifer ? _conifer : _broadleaf, 2, 0.5f);
        ++drawn;
    }
    static float nextLog = 0;
    if (time >= nextLog)
    {
        nextLog = time + 5.0f;
        LOG_INFO(Graphics, "Earth detail trees: {} of {} drawn", drawn, _trees.size());
    }
}
} // namespace Poseidon
