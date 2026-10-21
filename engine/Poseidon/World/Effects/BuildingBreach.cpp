// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/World/Effects/BuildingBreach.hpp>

#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Dev/Diag/ProbeMeshes.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/Graphics/Textures/TexturePreload.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/Graphics/Rendering/Effects/Smokes.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Effects/FireVolumes.hpp>
#include <Poseidon/World/Physics/PhysicsBackend.hpp>
#include <Poseidon/World/Physics/PhysicsWorld.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>

#include <algorithm>
#include <cstdlib>
#include <array>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace Poseidon::BuildingBreach
{
namespace
{
Settings s_settings;

struct Hole
{
    Vector3 centre; // model space
    Vector3 axis;   // the wall's normal there, unit
    float radius;   // the outline's smallest radius: what is open all round
};
struct BreachState
{
    Ref<LODShapeWithShadow> original; // kept alive: the scene's other houses still use it
    Ref<LODShapeWithShadow> clone;
    Ref<Object> object; // the building itself, to give its model back on a restart
    int holes = 0;
    std::vector<Hole> openings;
};
// Never destroyed: these hold models and scene objects, and static destruction runs after the
// engine has shut down (a crash at exit otherwise).
std::unordered_map<const Object*, BreachState>& s_breached = *new std::unordered_map<const Object*, BreachState>;

struct Chunk
{
    Physics::BodyId body;
    Vector3 half;
    LODShapeWithShadow* mesh = nullptr; // a box textured with the wall it came from
    float born = 0;
};
std::vector<Chunk>& s_chunks = *new std::vector<Chunk>;
std::vector<Ref<ObjectColored>>& s_chunkObjects = *new std::vector<Ref<ObjectColored>>;

constexpr int HoleSides = 24;
constexpr float TwoPi = 6.2831853f;

float Hash01(uint32_t seed, uint32_t salt)
{
    uint32_t h = seed * 0x9E3779B1u ^ (salt + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return float(h & 0xFFFFFFu) / float(0x1000000);
}

/// The ragged outline: a radius per side around the hole centre.
std::array<float, HoleSides> HoleOutline(float radius, uint32_t seed)
{
    std::array<float, HoleSides> r{};
    const float p1 = TwoPi * Hash01(seed, 1), p2 = TwoPi * Hash01(seed, 2), p3 = TwoPi * Hash01(seed, 3);
    for (int k = 0; k < HoleSides; ++k)
    {
        const float a = TwoPi * k / HoleSides;
        const float jag = 0.16f * (Hash01(seed, 100 + k) - 0.5f) * 2.0f;
        r[k] = radius * std::max(0.45f, 1.0f + 0.2f * std::sin(3.0f * a + p1) + 0.1f * std::sin(5.0f * a + p2) +
                                            0.06f * std::sin(9.0f * a + p3) + jag);
    }
    return r;
}

/// Attributes of a point in a face's plane: barycentric in the face's fan triangle holding it.
struct FacePoint
{
    Vector3 pos;
    Vector3 norm;
    float u = 0, v = 0;
    int source = 0; // the face vertex the new vertex is copied from (clip flags, tangents)
};

bool Barycentric(Vector3Par p, Vector3Par a, Vector3Par b, Vector3Par c, float& wa, float& wb, float& wc)
{
    const Vector3 v0 = b - a, v1 = c - a, v2 = p - a;
    const float d00 = v0.DotProduct(v0), d01 = v0.DotProduct(v1), d11 = v1.DotProduct(v1);
    const float d20 = v2.DotProduct(v0), d21 = v2.DotProduct(v1);
    const float den = d00 * d11 - d01 * d01;
    if (std::abs(den) < 1e-12f) return false;
    wb = (d11 * d20 - d01 * d21) / den;
    wc = (d00 * d21 - d01 * d20) / den;
    wa = 1.0f - wb - wc;
    return true;
}

FacePoint Interpolate(const Shape& lod, const Poly& face, Vector3Par p)
{
    FacePoint out;
    out.pos = p;
    const int n = face.N();
    int best = 1;
    float bwa = 1, bwb = 0, bwc = 0, worst = -1e9f;
    for (int t = 1; t + 1 < n; ++t)
    {
        float wa, wb, wc;
        if (!Barycentric(p, lod.Pos(face.GetVertex(0)), lod.Pos(face.GetVertex(t)), lod.Pos(face.GetVertex(t + 1)), wa,
                         wb, wc))
            continue;
        const float m = std::min({wa, wb, wc});
        if (m > worst) { worst = m; best = t; bwa = wa; bwb = wb; bwc = wc; }
    }
    const int ia = face.GetVertex(0), ib = face.GetVertex(best), ic = face.GetVertex(best + 1);
    out.norm = lod.Norm(ia) * bwa + lod.Norm(ib) * bwb + lod.Norm(ic) * bwc;
    out.u = lod.U(ia) * bwa + lod.U(ib) * bwb + lod.U(ic) * bwc;
    out.v = lod.V(ia) * bwa + lod.V(ib) * bwb + lod.V(ic) * bwc;
    out.source = bwa >= bwb && bwa >= bwc ? ia : (bwb >= bwc ? ib : ic);
    return out;
}

/// Appends the points as vertices of `lod`; returns their indices.
std::vector<int> AddVertices(Shape& lod, const std::vector<FacePoint>& pts)
{
    std::vector<int> sources(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) sources[i] = pts[i].source;
    const int first = lod.AppendVertexCopies(sources.data(), int(sources.size()));
    std::vector<int> idx(pts.size());
    for (size_t i = 0; i < pts.size(); ++i)
    {
        const int v = first + int(i);
        lod.SetPos(v) = pts[i].pos;
        Vector3 n = pts[i].norm;
        if (n.SquareSize() > 1e-12f) n.Normalize();
        lod.SetNorm(v) = n;
        lod.SetUV(v, pts[i].u, pts[i].v);
        idx[i] = v;
    }
    return idx;
}

/// The frame every cut of one hole shares: the wall normal `g` turned to point along the shot
/// (`along`), `ax` horizontal in the wall, `ay` up it. Both skins of a wall and every face of
/// it then lay the same ragged outline in the same place (each face used its own edge as x,
/// and the far skin's frame was mirrored -- the two skins' holes did not line up).
void WallFrame(Vector3Par g, Vector3Par along, Vector3& gg, Vector3& ax, Vector3& ay)
{
    gg = g.DotProduct(along) >= 0.0f ? g : -g;
    ax = Vector3(0, 1, 0).CrossProduct(gg);
    if (ax.SquareSize() < 1e-6f) ax = Vector3(1, 0, 0); else ax.Normalize();
    ay = gg.CrossProduct(ax);
}

/// One breached face: the convex face (3 or 4 vertices) re-triangulated as a ring between its
/// edges and the hole outline, and a rim extruded into the wall. Returns false when the face
/// is not one this method can cut (the hole crosses its edge), and leaves it alone.
bool CutFace(Shape& lod, Poly& face, std::vector<Poly>& added, Vector3Par centre, const std::array<float, HoleSides>& outline,
             float rimDepth, Vector3Par along)
{
    const int n = face.N();
    if (n < 3 || n > 4) return false;
    Vector3 corner[4];
    for (int i = 0; i < n; ++i) corner[i] = lod.Pos(face.GetVertex(i));
    Vector3 g = (corner[1] - corner[0]).CrossProduct(corner[2] - corner[0]);
    if (g.SquareSize() < 1e-10f) return false;
    g.Normalize();
    // the hole centre projected onto the face
    const Vector3 q = centre - g * g.DotProduct(centre - corner[0]);
    // in-plane frame: the wall's shared one (WallFrame)
    Vector3 gg, ax, ay;
    WallFrame(g, along, gg, ax, ay);
    auto toPlane = [&](Vector3Par p) { const Vector3 d = p - q; return std::array<float, 2>{d.DotProduct(ax), d.DotProduct(ay)}; };
    std::array<std::array<float, 2>, 4> c2{};
    for (int i = 0; i < n; ++i) c2[i] = toPlane(corner[i]);
    // convex polygon, either winding: inside = same side of every edge
    float windSign = 0;
    for (int i = 0; i < n; ++i)
    {
        const auto& a = c2[i]; const auto& b = c2[(i + 1) % n];
        windSign += (b[0] - a[0]) * (b[1] + a[1]);
    }
    const float s = windSign < 0 ? 1.0f : -1.0f;
    auto inside = [&](float x, float y, float margin)
    {
        for (int i = 0; i < n; ++i)
        {
            const auto& a = c2[i]; const auto& b = c2[(i + 1) % n];
            float ex = b[0] - a[0], ey = b[1] - a[1];
            const float len = std::sqrt(ex * ex + ey * ey);
            if (len < 1e-6f) continue;
            const float side = s * (ex * (y - a[1]) - ey * (x - a[0])) / len;
            if (side < margin) return false;
        }
        return true;
    };
    // the whole hole must sit inside the face, a few centimetres off its edges
    std::array<Vector3, HoleSides> hole3;
    std::array<float, HoleSides> angle{};
    for (int k = 0; k < HoleSides; ++k)
    {
        angle[k] = TwoPi * k / HoleSides;
        const float x = std::cos(angle[k]) * outline[k], y = std::sin(angle[k]) * outline[k];
        if (!inside(x, y, 0.04f)) return false;
        hole3[k] = q + ax * x + ay * y;
    }
    if (!inside(0, 0, 0.04f)) return false;

    // the outer ring: the face's corners and, at every hole angle, the ray's exit from the face
    struct RingPoint { float a; Vector3 p; };
    std::vector<RingPoint> ring;
    for (int i = 0; i < n; ++i) ring.push_back({std::atan2(c2[i][1], c2[i][0]), corner[i]});
    for (int k = 0; k < HoleSides; ++k)
    {
        const float dx = std::cos(angle[k]), dy = std::sin(angle[k]);
        float best = 1e9f;
        for (int i = 0; i < n; ++i)
        {
            const auto& a = c2[i]; const auto& b = c2[(i + 1) % n];
            const float ex = b[0] - a[0], ey = b[1] - a[1];
            const float den = dx * ey - dy * ex;
            if (std::abs(den) < 1e-9f) continue;
            const float t = (a[0] * ey - a[1] * ex) / den;          // along the ray
            const float u = (a[0] * dy - a[1] * dx) / den;          // along the edge
            if (t > 0 && u >= -1e-4f && u <= 1.0001f) best = std::min(best, t);
        }
        if (best < 1e8f) ring.push_back({angle[k], q + ax * (dx * best) + ay * (dy * best)});
    }
    for (auto& r : ring) if (r.a < 0) r.a += TwoPi;
    std::sort(ring.begin(), ring.end(), [](const RingPoint& a, const RingPoint& b) { return a.a < b.a; });

    // vertices: hole outline, outer ring, rim (outline pushed into the wall)
    Vector3 inward = VZero; // Shape stores inward normals: into the wall, away from the viewer
    for (int i = 0; i < n; ++i) inward += lod.Norm(face.GetVertex(i));
    if (inward.SquareSize() < 1e-10f) inward = -g; else inward.Normalize();
    std::vector<FacePoint> pts;
    for (int k = 0; k < HoleSides; ++k) pts.push_back(Interpolate(lod, face, hole3[k]));
    for (const auto& r : ring) pts.push_back(Interpolate(lod, face, r.p));
    const int holeBase = 0, ringBase = HoleSides;
    const int rimBase = int(pts.size());
    for (int k = 0; k < HoleSides; ++k)
    {
        FacePoint fp = pts[k];
        fp.pos = hole3[k] + inward * rimDepth;
        // the rim's surface faces the hole's axis (stored normals point into the material)
        Vector3 out = hole3[k] - q;
        if (out.SquareSize() > 1e-10f) out.Normalize();
        fp.norm = out;
        fp.v += rimDepth * 0.5f;
        pts.push_back(fp);
        pts[k].norm = pts[k].norm; // the hole edge keeps the wall's normal
    }
    const std::vector<int> idx = AddVertices(lod, pts);

    // front = the original face's winding
    auto addTri = [&](int a, int b, int c, Vector3Par wantFront)
    {
        const Vector3 pa = lod.Pos(a), pb = lod.Pos(b), pc = lod.Pos(c);
        Vector3 cr = (pb - pa).CrossProduct(pc - pa);
        if (cr.SquareSize() < 1e-14f) return;
        if (cr.DotProduct(wantFront) < 0) std::swap(b, c);
        Poly p = face;
        p.SetN(3);
        p.Set(0, VertexIndex(a)); p.Set(1, VertexIndex(b)); p.Set(2, VertexIndex(c));
        added.push_back(p);
    };
    // the ring between the hole and the face edge, zipped by angle
    const int nr = int(ring.size());
    int h = 0, o = 0;
    auto holeAngle = [&](int k) { return angle[k % HoleSides] + (k >= HoleSides ? TwoPi : 0.0f); };
    auto ringAngle = [&](int k) { return ring[k % nr].a + (k >= nr ? TwoPi : 0.0f); };
    // start the outer ring at the first point at or after the hole's first angle (0)
    while (h < HoleSides || o < nr)
    {
        const float nextH = h < HoleSides ? holeAngle(h + 1) : 1e9f;
        const float nextO = o < nr ? ringAngle(o + 1) : 1e9f;
        const int hi = idx[holeBase + h % HoleSides], oi = idx[ringBase + o % nr];
        if (nextH <= nextO)
        {
            addTri(oi, hi, idx[holeBase + (h + 1) % HoleSides], g);
            ++h;
        }
        else
        {
            addTri(oi, hi, idx[ringBase + (o + 1) % nr], g);
            ++o;
        }
    }
    // the rim: quads from the hole edge into the wall, facing the hole's axis
    for (int k = 0; k < HoleSides; ++k)
    {
        const int a = idx[holeBase + k], b = idx[holeBase + (k + 1) % HoleSides];
        const int c = idx[rimBase + (k + 1) % HoleSides], d = idx[rimBase + k];
        Vector3 toAxis = q - (hole3[k] + hole3[(k + 1) % HoleSides]) * 0.5f;
        // the rim is seen from inside the hole: its front faces the axis, with the original
        // winding convention (g is the front of the original face)
        const float conv = g.DotProduct(-inward) >= 0 ? 1.0f : -1.0f;
        addTri(a, b, c, toAxis * conv);
        addTri(a, c, d, toAxis * conv);
    }
    // the original face itself becomes nothing: a degenerate polygon of its own vertex count,
    // so the face stream keeps its size and every later face its offset
    const int v0 = face.GetVertex(0);
    for (int i = 0; i < n; ++i) face.Set(i, VertexIndex(v0));
    return true;
}

/// Radius of the ragged outline at an angle (linear between its sides).
float OutlineAt(const std::array<float, HoleSides>& outline, float a)
{
    float f = a / TwoPi * HoleSides;
    f -= std::floor(f / HoleSides) * HoleSides;
    const int i = int(f) % HoleSides;
    const float w = f - std::floor(f);
    return outline[i] * (1 - w) + outline[(i + 1) % HoleSides] * w;
}

/// The fallback for a hole that does not fit inside one face (it spans several, or crosses
/// an edge): the face is subdivided into a triangle grid of about `cell` metres and every
/// small triangle inside the ragged outline is dropped. Returns false if nothing was inside.
bool GridCutFace(Shape& lod, Poly& face, std::vector<Poly>& added, Vector3Par centre,
                 const std::array<float, HoleSides>& outline, float cell, Vector3Par along)
{
    const int n = face.N();
    if (n < 3 || n > 4) return false;
    Vector3 corner[4];
    for (int i = 0; i < n; ++i) corner[i] = lod.Pos(face.GetVertex(i));
    Vector3 g = (corner[1] - corner[0]).CrossProduct(corner[2] - corner[0]);
    if (g.SquareSize() < 1e-10f) return false;
    g.Normalize();
    const Vector3 q = centre - g * g.DotProduct(centre - corner[0]);
    // one frame for every face of the wall (horizontal and up), so a hole spanning several
    // faces keeps one outline across their seams
    Vector3 gg, ax, ay;
    WallFrame(g, along, gg, ax, ay);
    auto inHole = [&](Vector3Par p)
    {
        const Vector3 d = p - q;
        const float r = d.Size();
        if (r < 1e-6f) return true;
        return r < OutlineAt(outline, std::atan2(d.DotProduct(ay), d.DotProduct(ax)));
    };
    std::vector<std::array<Vector3, 3>> keep;
    bool any = false;
    for (int t = 1; t + 1 < n; ++t)
    {
        const Vector3 A = corner[0], B = corner[t], C = corner[t + 1];
        const float longest = std::max({(B - A).Size(), (C - B).Size(), (A - C).Size()});
        const int m = std::clamp(int(std::ceil(longest / cell)), 1, 40);
        auto P = [&](int i, int j) { return A + (B - A) * (float(i) / m) + (C - A) * (float(j) / m); };
        for (int i = 0; i < m; ++i)
            for (int j = 0; i + j < m; ++j)
            {
                std::array<Vector3, 3> up = {P(i, j), P(i + 1, j), P(i, j + 1)};
                if (inHole((up[0] + up[1] + up[2]) / 3.0f)) any = true; else keep.push_back(up);
                if (i + j + 1 < m)
                {
                    std::array<Vector3, 3> dn = {P(i + 1, j), P(i + 1, j + 1), P(i, j + 1)};
                    if (inHole((dn[0] + dn[1] + dn[2]) / 3.0f)) any = true; else keep.push_back(dn);
                }
            }
    }
    if (!any) return false;
    if (lod.NPos() + int(keep.size()) * 3 > 60000) return false; // 16-bit vertex indices
    std::vector<FacePoint> pts;
    pts.reserve(keep.size() * 3);
    for (const auto& tri : keep)
        for (const Vector3& p : tri) pts.push_back(Interpolate(lod, face, p));
    const std::vector<int> idx = AddVertices(lod, pts);
    for (size_t k = 0; k < keep.size(); ++k)
    {
        int a = idx[k * 3], b = idx[k * 3 + 1], c = idx[k * 3 + 2];
        const Vector3 cr = (lod.Pos(b) - lod.Pos(a)).CrossProduct(lod.Pos(c) - lod.Pos(a));
        if (cr.DotProduct(g) < 0) std::swap(b, c);
        Poly p = face;
        p.SetN(3);
        p.Set(0, VertexIndex(a)); p.Set(1, VertexIndex(b)); p.Set(2, VertexIndex(c));
        added.push_back(p);
    }
    const int v0 = face.GetVertex(0);
    for (int i = 0; i < n; ++i) face.Set(i, VertexIndex(v0));
    return true;
}

/// Breaches one visual LOD. Returns the number of faces cut.
int BreachLevel(Shape& lod, Vector3Par centre, Vector3Par dirModel, float radius, uint32_t seed, Texture*& wallTexture,
                Vector3& wallNormal)
{
    const auto outline = HoleOutline(radius, seed);
    std::vector<Poly> added;
    int cut = 0;
    struct Skin { float along; Poly templ; Vector3 g, inward, point; };
    std::vector<Skin> skins;
    // faces near the hole: the blast's sphere reaches both skins of a wall
    // (owner, 2026-10-19: "a hole in the wall is invisible from the other side" -- the garage's
    // inner skin is 1.01 m behind its outer one; 0.45 + 0.6 r stopped at 0.9 m)
    const float reach = radius * 0.6f + 1.2f;
    static const bool trace = [] { const char* v = std::getenv("POSEIDON_BREACH_TRACE"); return v && v[0] == '1'; }();
    for (Offset o = lod.BeginFaces(); o < lod.EndFaces(); lod.NextFace(o))
    {
        Poly& face = lod.Face(o);
        const int n = face.N();
        if (n < 3 || n > 4) continue;
        const Vector3 a = lod.Pos(face.GetVertex(0));
        Vector3 g = (lod.Pos(face.GetVertex(1)) - a).CrossProduct(lod.Pos(face.GetVertex(2)) - a);
        if (g.SquareSize() < 1e-10f) continue;
        g.Normalize();
        // a wall, roughly facing the shot (either side): floors and roofs are left alone
        if (std::abs(g.Y()) > 0.6f || std::abs(g.DotProduct(dirModel)) < 0.25f) continue;
        // (trace: every wall face near enough -- its plane's distance from the hole centre, which
        // way it faces, its size -- to see which skins of a wall a cut reaches)
        const float planeDist = g.DotProduct(centre - a);
        if (std::abs(planeDist) > reach)
        {
            if (trace && std::abs(planeDist) < reach + 1.5f)
            {
                Vector3 mid = VZero;
                for (int i = 0; i < n; ++i) mid += lod.Pos(face.GetVertex(i));
                mid = mid * (1.0f / float(n));
                const Vector3 off = (mid - centre) - g * g.DotProduct(mid - centre);
                if (off.Size() < 4.0f)
                    LOG_INFO(World, "Breach trace: face plane {:+.2f} m facing {:+.2f}, {:.2f} m off-centre: beyond reach {:.2f}",
                             planeDist, g.DotProduct(dirModel), off.Size(), reach);
            }
            continue;
        }
        // area check: tiny faces (trim, window frames) are not walls
        Vector3 mn = a, mx = a;
        for (int i = 1; i < n; ++i)
        {
            const Vector3 p = lod.Pos(face.GetVertex(i));
            mn = Vector3(std::min(mn.X(), p.X()), std::min(mn.Y(), p.Y()), std::min(mn.Z(), p.Z()));
            mx = Vector3(std::max(mx.X(), p.X()), std::max(mx.Y(), p.Y()), std::max(mx.Z(), p.Z()));
        }
        if ((mx - mn).Size() < 0.1f) continue;
        if (lod.NPos() + 3 * HoleSides + 16 > 60000) break; // 16-bit vertex indices
        // a hole inside one big face: a clean ring and a rim; otherwise the grid fallback
        Texture* texture = face.GetTexture();
        const Poly templ = face; // before the cut turns it into a degenerate polygon
        Vector3 inwardN = VZero;
        for (int i = 0; i < n; ++i) inwardN += lod.Norm(face.GetVertex(i));
        bool cutThis = false;
        // (no rim of their own: the tunnel below runs through the whole wall)
        if ((mx - mn).Size() >= radius * 1.5f && CutFace(lod, face, added, centre, outline, 0.0f, dirModel))
            cutThis = true;
        else if (GridCutFace(lod, face, added, centre, outline, std::max(0.08f, radius / 5.0f), dirModel))
            cutThis = true;
        if (cutThis)
        {
            // where this skin crosses the hole's axis, along the shot
            Vector3 gg = g.DotProduct(dirModel) >= 0.0f ? g : -g;
            skins.push_back({gg.DotProduct(a - centre), templ, g, inwardN, a});
        }
        if (trace)
        {
            // the face's extent, in-plane distance of its centre from the hole
            Vector3 mid = VZero;
            for (int i = 0; i < n; ++i) mid += lod.Pos(face.GetVertex(i));
            mid = mid * (1.0f / float(n));
            const Vector3 off = (mid - centre) - g * g.DotProduct(mid - centre);
            LOG_INFO(World, "Breach trace: face plane {:+.2f} m facing {:+.2f} size {:.2f} m, {:.2f} m off-centre: {}",
                     planeDist, g.DotProduct(dirModel), (mx - mn).Size(), off.Size(), cutThis ? "cut" : "NOT cut");
        }
        if (cutThis)
        {
            ++cut;
            if (!wallTexture) { wallTexture = texture; wallNormal = g; }
        }
    }
    // The tunnel (owner, 2026-10-19: "those holes look 2d, they are not as deep as the real
    // wall and still not visible from the other side"): a ring of wall from the first skin the
    // round met to the last, along the same ragged outline, textured like the wall. A wall of
    // one skin gets 25 cm of depth into it.
    if (!skins.empty() && lod.NPos() + 2 * (HoleSides + 1) + 8 < 60000)
    {
        float s0 = 1e9f, s1 = -1e9f;
        const Skin* front = &skins[0];
        for (const Skin& s : skins)
        {
            if (s.along < s0) { s0 = s.along; front = &s; }
            s1 = std::max(s1, s.along);
        }
        if (s1 - s0 < 0.05f) s1 = s0 + 0.25f;
        Vector3 axis, ax, ay;
        WallFrame(front->g, dirModel, axis, ax, ay);
        // the stored-normal convention of the wall's faces (see CutFace's rim)
        Vector3 inward = front->inward;
        if (inward.SquareSize() < 1e-10f) inward = -front->g; else inward.Normalize();
        const float conv = front->g.DotProduct(-inward) >= 0 ? 1.0f : -1.0f;
        std::vector<FacePoint> pts;
        float perimeter = 0.0f;
        for (int k = 0; k <= HoleSides; ++k)
        {
            const float ang = TwoPi * float(k) / HoleSides;
            const float r = outline[size_t(k % HoleSides)];
            const Vector3 radial = ax * std::cos(ang) + ay * std::sin(ang);
            if (k > 0)
            {
                const float rp = outline[size_t(k - 1)];
                const float ap = TwoPi * float(k - 1) / HoleSides;
                perimeter += ((ax * std::cos(ang) + ay * std::sin(ang)) * r - (ax * std::cos(ap) + ay * std::sin(ap)) * rp).Size();
            }
            for (int side = 0; side < 2; ++side)
            {
                FacePoint fp;
                fp.pos = centre + axis * (side == 0 ? s0 : s1) + radial * r;
                fp.norm = radial; // stored normals point into the material: out from the axis
                fp.u = perimeter * 0.5f;
                fp.v = (side == 0 ? 0.0f : (s1 - s0)) * 0.5f;
                fp.source = front->templ.GetVertex(0);
                pts.push_back(fp);
            }
        }
        const std::vector<int> idx = AddVertices(lod, pts);
        for (int k = 0; k < HoleSides; ++k)
        {
            const int a0 = idx[size_t(2 * k)], a1 = idx[size_t(2 * k + 1)];
            const int b0 = idx[size_t(2 * k + 2)], b1 = idx[size_t(2 * k + 3)];
            const float ang = TwoPi * (float(k) + 0.5f) / HoleSides;
            const Vector3 toAxis = -(ax * std::cos(ang) + ay * std::sin(ang));
            auto tri = [&](int a, int b, int c)
            {
                const Vector3 pa = lod.Pos(a), pb = lod.Pos(b), pc = lod.Pos(c);
                Vector3 cr = (pb - pa).CrossProduct(pc - pa);
                if (cr.SquareSize() < 1e-14f) return;
                if (cr.DotProduct(toAxis * conv) < 0) std::swap(b, c);
                Poly p = front->templ;
                p.SetN(3);
                p.Set(0, VertexIndex(a)); p.Set(1, VertexIndex(b)); p.Set(2, VertexIndex(c));
                added.push_back(p);
            };
            tri(a0, b0, b1);
            tri(a0, b1, a1);
        }
    }
    for (const Poly& p : added) lod.AddFace(p);
    if (cut > 0)
    {
        lod.RecalculateAreas();
        lod.FindSections();
        lod.CalculateHints();
        lod.InvalidateNormals();
    }
    return cut;
}

// ---- BOOM-007: bullet marks -------------------------------------------------------------------
// A rifle round into masonry or plaster: a small dark puncture, round it a crater 3-8 cm across
// where the surface spalled off (paler, rough), its edge ragged, a few fine cracks and a little
// soot. Drawn as a quad on the wall (its normal = the wall's, so it is lit like the wall), from
// one procedural RGBA atlas of four different marks; the pale ring is half transparent so the
// wall's own colour shows through it (per-object tints do not reach the WGPU object path).
struct Mark
{
    Vector3 pos, aside, up, normal;
    int variant = 0;
    LODShapeWithShadow* mesh = nullptr; // its wall colour's atlas cell (meshes are never freed)
};
constexpr size_t MaxMarks = 400;
std::vector<Mark>& s_marks = *new std::vector<Mark>;
size_t s_markHead = 0;
std::vector<Ref<ObjectColored>>& s_markObjects = *new std::vector<Ref<ObjectColored>>;

// One atlas per wall colour (bucketed to 4 bits a channel): the spall is that wall's material, a
// little paler -- a grey ring on an ochre wall read as a white sticker (owner capture, 2026-10-19).
Texture* MarkTexture(const Color& wall)
{
    const auto q = [](float v) { return int(std::clamp(v, 0.0f, 1.0f) * 15.0f + 0.5f); };
    const int key = q(wall.R()) | q(wall.G()) << 4 | q(wall.B()) << 8;
    static auto& textures = *new std::unordered_map<int, Ref<Texture>>;
    if (auto found = textures.find(key); found != textures.end()) return found->second.GetRef();
    if (!GEngine || !GEngine->TextBank() || textures.size() >= 48) return nullptr;
    const float tint[3] = {float(key & 15) / 15.0f, float((key >> 4) & 15) / 15.0f, float((key >> 8) & 15) / 15.0f};
    Ref<Texture>& texture = textures[key];
    constexpr int cell = 128, side = cell * 2;
    std::vector<std::uint8_t> px(size_t(side * side * 4), 0);
    for (int v = 0; v < 4; ++v)
    {
        const uint32_t seed = 0x9e3779b9u * uint32_t(v + 1);
        // the crater's ragged outline: a few low harmonics
        float amp[5], ph[5];
        for (int i = 0; i < 5; ++i)
        {
            amp[i] = (0.05f + 0.10f * Hash01(seed, uint32_t(i))) / float(i + 1);
            ph[i] = TwoPi * Hash01(seed, uint32_t(10 + i));
        }
        const float hole = 0.14f + 0.06f * Hash01(seed, 40);
        const int cracks = 3 + int(Hash01(seed, 41) * 3.0f);
        const int ox = (v & 1) * cell, oy = (v >> 1) * cell;
        for (int y = 0; y < cell; ++y)
            for (int x = 0; x < cell; ++x)
            {
                const float u = (float(x) + 0.5f) / cell * 2.0f - 1.0f, w = (float(y) + 0.5f) / cell * 2.0f - 1.0f;
                const float r = std::sqrt(u * u + w * w);
                const float a = std::atan2(w, u);
                float edge = 0.62f;
                for (int i = 0; i < 5; ++i) edge += amp[i] * std::sin(float(i + 2) * a + ph[i]);
                // fine grain of the broken surface
                const uint32_t g = uint32_t(x * 73856093) ^ uint32_t(y * 19349663) ^ seed;
                const float grain = Hash01(g, 3);
                float rgb = 0.0f, alpha = 0.0f;
                if (r < hole * (0.85f + 0.3f * grain))
                {
                    rgb = 0.035f; alpha = 1.0f; // the puncture
                }
                else if (r < edge)
                {
                    // spalled crater: paler and rougher toward the middle, a dark rim of shadow
                    // (first capture: an opaque near-white ring read as a sticker on the wall --
                    // spall is only a little paler than the wall, rough and grainy, and the
                    // crater's lower lip is in shadow)
                    const float t = (r - hole) / std::max(edge - hole, 1e-3f);
                    const float lower = std::clamp(-w * 1.5f, 0.0f, 1.0f); // the lip below the hole
                    rgb = 0.48f + 0.16f * (1.0f - t) + 0.22f * (grain - 0.5f) - 0.18f * lower * (1.0f - t);
                    alpha = 0.30f + 0.22f * (1.0f - t) + 0.18f * grain;
                    if (t > 0.84f) { rgb *= 0.6f; alpha = 0.45f; }
                }
                else
                {
                    // hairline cracks and a little soot/dust beyond the edge
                    float crack = 0.0f;
                    for (int c = 0; c < cracks; ++c)
                    {
                        const float ca = TwoPi * Hash01(seed, uint32_t(50 + c));
                        float d = std::fabs(std::sin(a - ca)) * r;
                        if (std::cos(a - ca) < 0.0f) d = 1.0f;
                        const float len = 0.75f + 0.25f * Hash01(seed, uint32_t(60 + c));
                        if (r < len && d < 0.012f + 0.01f * (1.0f - r)) crack = 1.0f;
                    }
                    const float soot = std::max(0.0f, 1.0f - (r - edge) / 0.25f) * 0.25f;
                    if (crack > 0.0f) { rgb = 0.08f; alpha = 0.7f; }
                    else if (soot > 0.0f && r < 1.0f) { rgb = 0.10f; alpha = soot * (0.6f + 0.8f * grain); }
                }
                const size_t o = size_t(((oy + y) * side + ox + x) * 4);
                // the crater's pale part takes the wall's colour, lifted toward its broken-off
                // (paler, less saturated) inside; the puncture, rim shadow and soot stay dark
                for (int ch = 0; ch < 3; ++ch)
                {
                    const float pale = rgb > 0.3f ? std::min(1.0f, tint[ch] * 0.75f + 0.35f) * (rgb / 0.56f) * 0.8f : rgb;
                    px[o + size_t(ch)] = std::uint8_t(std::lround(255.0f * std::clamp(pale, 0.0f, 1.0f)));
                }
                px[o + 3] = std::uint8_t(std::lround(255.0f * std::clamp(alpha, 0.0f, 1.0f)));
            }
    }
    texture = GEngine->TextBank()->CreateDynamic(side, side, px.data(), uint32_t(px.size()), true, AlphaStats::Blend);
    return texture.GetRef();
}

/// A unit quad (-1..1 in x, y; facing +z) showing atlas cell `variant` of `texture`.
LODShapeWithShadow* MarkMesh(Texture* texture, int variant)
{
    static auto& meshes = *new std::unordered_map<const Texture*, std::array<Ref<LODShapeWithShadow>, 4>>;
    if (!texture) return nullptr;
    Ref<LODShapeWithShadow>& mesh = meshes[texture][size_t(variant & 3)];
    if (mesh) return mesh;
    LODShapeWithShadow* lShape = new LODShapeWithShadow;
    lShape->SetAutoCenter(false);
    Shape* shape = new Shape;
    lShape->AddShape(shape, 0);
    const float u0 = 0.5f * float(variant & 1), v0 = 0.5f * float((variant >> 1) & 1);
    const float corner[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    shape->ReallocTable(4);
    for (int i = 0; i < 4; ++i)
    {
        shape->SetPos(i) = Vector3(corner[i][0], corner[i][1], 0.0f);
        shape->SetNorm(i) = Vector3(0, 0, -1); // Shape stores inward normals
        shape->SetUV(i, u0 + 0.5f * (corner[i][0] * 0.5f + 0.5f), v0 + 0.5f * (corner[i][1] * 0.5f + 0.5f));
        shape->SetClip(i, ClipAll);
    }
    Poly face;
    face.Init();
    face.SetN(4);
    for (int i = 0; i < 4; ++i) face.Set(i, VertexIndex(i));
    face.SetTexture(texture);
    // blended over the wall, never writing depth (it lies 4 mm in front of it)
    face.SetSpecial(IsAlpha | IsAlphaFog | NoZWrite | NoShadow);
    shape->AddFace(face);
    shape->OrSpecial(IsAlpha | IsAlphaFog | NoZWrite | NoShadow);
    shape->FindSections();
    shape->CalculateHints();
    lShape->CalculateMinMax(true);
    mesh = lShape;
    return mesh;
}

void ImpactParticle(LODShapeWithShadow* shape, float life, Vector3Par at, Vector3Par vel, float weight, float rubbing,
                    float size0, float size1, const Color& c, bool hang)
{
    AutoArray<float> size;
    size.Add(size0);
    size.Add(size1);
    AutoArray<Color> color;
    color.Add(c);
    color.Add(c);
    color.Add(Color(c.R(), c.G(), c.B(), 0.0f));
    AutoArray<float> phase;
    phase.Add(0.0f);
    GLOB_WORLD->AddCloudlet(new CParticle(shape, "", PT_Billboard, 1000.0f, life, at, vel, GRandGen.RandomValue() * 2.0f - 1.0f,
                                          weight, hang ? weight / 1.275f : 0.0f, rubbing, size, color, phase, 1000.0f,
                                          0.0f, "", ""));
}

ObjectColored* ChunkObject(int i)
{
    while (int(s_chunkObjects.size()) <= i) s_chunkObjects.push_back(new ObjectColored(Dev::ProbeBoxMesh(), -1));
    return s_chunkObjects[size_t(i)];
}

/// A unit box (-1..1) whose faces carry `texture`, each face a small patch of it: a chunk of
/// that wall. (Per-object constant colours do not reach the WGPU object path, so tinting the
/// probe box left the chunks white.) Same recipe as the probe meshes (ProbeMeshes.cpp).
LODShapeWithShadow* ChunkMesh(Texture* texture)
{
    static auto& meshes = *new std::unordered_map<Texture*, Ref<LODShapeWithShadow>>;
    auto found = meshes.find(texture);
    if (found != meshes.end()) return found->second;
    LODShapeWithShadow* lShape = new LODShapeWithShadow;
    lShape->SetAutoCenter(false);
    Shape* shape = new Shape;
    lShape->AddShape(shape, 0);
    static const float axis[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    std::vector<Vector3> pos, nrm;
    std::vector<std::array<float, 2>> uv;
    for (int a = 0; a < 3; ++a)
    {
        const int b = (a + 1) % 3, c = (a + 2) % 3;
        for (int sign = -1; sign <= 1; sign += 2)
        {
            const Vector3 n(axis[a][0] * sign, axis[a][1] * sign, axis[a][2] * sign);
            const Vector3 u(axis[b][0], axis[b][1], axis[b][2]), v(axis[c][0], axis[c][1], axis[c][2]);
            const float su = float(sign);
            const float u0 = 0.13f * a + 0.31f * (sign > 0), v0 = 0.21f * a;
            pos.push_back(n - u * su - v); uv.push_back({u0, v0});
            pos.push_back(n + u * su - v); uv.push_back({u0 + 0.12f, v0});
            pos.push_back(n + u * su + v); uv.push_back({u0 + 0.12f, v0 + 0.12f});
            pos.push_back(n - u * su + v); uv.push_back({u0, v0 + 0.12f});
            for (int i = 0; i < 4; ++i) nrm.push_back(n);
        }
    }
    shape->ReallocTable(int(pos.size()));
    for (size_t i = 0; i < pos.size(); ++i)
    {
        shape->SetPos(int(i)) = pos[i];
        shape->SetNorm(int(i)) = -nrm[i]; // Shape stores inward normals
        shape->SetUV(int(i), uv[i][0], uv[i][1]);
        shape->SetClip(int(i), ClipAll);
    }
    for (int f = 0; f < 6; ++f)
    {
        Poly face;
        face.Init();
        face.SetN(4);
        for (int i = 0; i < 4; ++i) face.Set(i, VertexIndex(f * 4 + i));
        face.SetTexture(texture ? texture : GScene->Preloaded(TextureWhite));
        shape->AddFace(face);
    }
    shape->FindSections();
    shape->CalculateHints();
    lShape->CalculateMinMax(true);
    meshes[texture] = lShape;
    return lShape;
}

void ThrowChunks(Object* building, Vector3Par worldPos, Vector3Par worldDir, float radius, LODShapeWithShadow* mesh, uint32_t seed)
{
    auto& world = Physics::EnsurePhysicsWorld();
    if (!world.Create()) return;
    // the house the chunks land in and bounce off
    static auto& statics = *new std::unordered_map<const Object*, Physics::BodyId>;
    if (!statics.count(building)) statics[building] = world.RegisterStatic(building->GetShape(), building->WorldTransform());
    const float now = Glob.time.toFloat();
    // the oldest go first when the budget is spent
    const int count = std::clamp(int(10 + 14 * radius), 8, 30);
    while (int(s_chunks.size()) + count > s_settings.maxLiveChunks && !s_chunks.empty())
    {
        world.Remove(s_chunks.front().body);
        s_chunks.erase(s_chunks.begin());
    }
    Vector3 dir = worldDir;
    if (dir.SquareSize() < 1e-6f) dir = Vector3(0, 0, 1);
    dir.Normalize();
    for (int i = 0; i < count; ++i)
    {
        const float r1 = Hash01(seed, 300 + i), r2 = Hash01(seed, 400 + i), r3 = Hash01(seed, 500 + i);
        const Vector3 half(0.05f + 0.13f * r1, 0.04f + 0.08f * r2, 0.05f + 0.12f * r3);
        std::array<Vector3, 8> pts;
        for (int k = 0; k < 8; ++k)
            pts[k] = Vector3((k & 1 ? 1 : -1) * half.X(), (k & 2 ? 1 : -1) * half.Y(), (k & 4 ? 1 : -1) * half.Z());
        Physics::ConvexPiece piece;
        piece.points = pts.data();
        piece.count = 8;
        // most of the wall goes in with the blast, some spalls back towards the shooter
        const bool back = Hash01(seed, 600 + i) < 0.3f;
        Vector3 spray(Hash01(seed, 700 + i) - 0.5f, Hash01(seed, 800 + i) * 0.6f, Hash01(seed, 900 + i) - 0.5f);
        Vector3 v = (back ? -dir : dir) * (4.0f + 10.0f * Hash01(seed, 1000 + i)) + spray * 8.0f;
        Matrix4 frame(MIdentity);
        frame.SetOrientation(Matrix3(MRotationX, TwoPi * r1) * Matrix3(MRotationY, TwoPi * r2));
        const Vector3 jitter(Hash01(seed, 1100 + i) - 0.5f, Hash01(seed, 1200 + i) - 0.5f, Hash01(seed, 1300 + i) - 0.5f);
        frame.SetPosition(worldPos + (back ? -dir : dir) * 0.35f + jitter * radius);
        Physics::ArticulatedInitialMotion motion;
        motion.linear = v;
        motion.angular = Vector3(r1 - 0.5f, r2 - 0.5f, r3 - 0.5f) * 20.0f;
        const float mass = 2000.0f * 8.0f * half.X() * half.Y() * half.Z(); // brick and plaster
        const Physics::BodyId body = world.SpawnArticulatedPiece(piece, frame, std::max(mass, 0.2f), motion);
        if (!body.IsValid()) continue;
        s_chunks.push_back({body, half, mesh, now});
    }
}
} // namespace

int g_breachedCount = 0;
Settings& Get() { return s_settings; }

bool PassesThroughHole(const Object* building, Vector3Par bModel, Vector3Par eModel)
{
    if (g_breachedCount == 0) return false;
    const auto it = s_breached.find(building);
    if (it == s_breached.end()) return false;
    for (const Hole& h : it->second.openings)
    {
        // both ends of the stretch inside the wall lie in the tunnel: within the open radius of
        // the hole's axis and within a wall's thickness of its centre along it
        auto inTunnel = [&](Vector3Par p)
        {
            const Vector3 d = p - h.centre;
            const float along = d.DotProduct(h.axis);
            if (std::abs(along) > 1.5f) return false;
            const float open = h.radius * 0.9f;
            return (d - h.axis * along).SquareSize() < open * open;
        };
        if (inTunnel(bModel) && inTunnel(eModel))
        {
            static int s_logged = 0;
            if (s_logged < 40)
            {
                ++s_logged;
                LOG_INFO(World, "Breach: a line passes through a hole in {} (model [{:.2f},{:.2f},{:.2f}])",
                         building->GetShape() ? building->GetShape()->Name() : "?", bModel.X(), bModel.Y(), bModel.Z());
            }
            return true;
        }
    }
    return false;
}

float HoleRadius(float indirectHit)
{
    if (!std::isfinite(indirectHit) || indirectHit <= 0) return 0;
    return std::clamp(0.75f * std::cbrt(indirectHit / 150.0f), 0.3f, 2.4f);
}

float CapBlastDamage(const Object* building, float before, float after)
{
    if (!s_settings.enabled || !building || after <= before || !building->Static()) return after;
    if (building->GetDestructType() != DestructBuilding) return after;
    const auto found = s_breached.find(building);
    if (found != s_breached.end() && found->second.holes >= s_settings.maxHolesPerBuilding) return after;
    struct Blast { float time; float start; };
    static auto& blasts = *new std::unordered_map<const Object*, Blast>;
    const float now = Glob.time.toFloat();
    Blast& b = blasts[building];
    if (now - b.time > 0.5f || now < b.time) b.start = before; // a new blast
    b.time = now;
    return std::min(after, b.start + 0.12f);
}

float StructureDamageShare(const Object* building, float indirectHit, bool explosive)
{
    if (!s_settings.enabled || !explosive || indirectHit < 40.0f || !building || !building->Static() ||
        building->IsDestroyed())
        return 1.0f;
    const DestructType destruct = building->GetDestructType();
    if (destruct != DestructBuilding) return 1.0f;
    const auto found = s_breached.find(const_cast<Object*>(building));
    if (found != s_breached.end() && found->second.holes >= s_settings.maxHolesPerBuilding) return 1.0f;
    // a fifth: a house takes a few rockets (a hole each) before it comes down
    return 0.2f;
}

bool Breach(Object* building, Vector3Par worldPos, Vector3Par worldDir, float indirectHit, float indirectHitRange)
{
    if (!s_settings.enabled || !building || !building->Static() || building->IsDestroyed()) return false;
    const DestructType destruct = building->GetDestructType();
    if (destruct != DestructBuilding && destruct != DestructNo) return false;
    LODShapeWithShadow* shape = building->GetShape();
    if (!shape || shape->DeferredVisualRendering() || shape->BoundingSphere() < 2.0f) return false;
    const float radius = HoleRadius(indirectHit);
    if (radius <= 0) return false;
    BreachState& state = s_breached[building];
    if (state.holes >= s_settings.maxHolesPerBuilding) return false;

    // The building's own copy of its model, a fresh one for every hole: the renderer keeps the
    // mesh it uploaded for a model, so cutting the same copy again changed nothing on screen --
    // the second and third holes were in the model and in the collision, but not drawn (owner,
    // 2026-10-19: "only one tiny hole before the entire building collapsed")
    Ref<LODShapeWithShadow> clone = new LODShapeWithShadow(*shape);
    for (int level = 0; level < shape->NLevels(); ++level)
        if (shape->Level(level) && clone->Level(level))
            clone->Level(level)->CopyProxiesFrom(*shape->Level(level));
    {
        const LODShapeWithShadow* named = state.original ? state.original.GetRef() : shape;
        clone->SetName((RString(named->Name()) + "#breach").Data());
    }
    const Matrix4 toModel = building->WorldInvTransform();
    Vector3 centre = toModel.FastTransform(worldPos);
    Vector3 dirModel = toModel.Rotate(worldDir);
    if (dirModel.SquareSize() < 1e-8f) return false;
    dirModel.Normalize();
    const uint32_t seed = uint32_t(int32_t(std::floor(worldPos.X() * 16.0f))) * 73856093u ^
                          uint32_t(int32_t(std::floor(worldPos.Y() * 16.0f))) * 19349663u ^
                          uint32_t(int32_t(std::floor(worldPos.Z() * 16.0f))) * 83492791u;
    int cut = 0;
    Texture* wallTexture = nullptr;
    Vector3 wallNormal = dirModel;
    // The round's flight line through the model, to the first wall face it meets: the point the
    // engine reports is not always on the wall -- a rocket's explosion is pulled back along its
    // flight first (2 m before the garage wall in the owner's session, 2026-10-19: every LAW "no
    // wall face to cut" while the house took the damage and collapsed).
    if (Shape* finest = clone->Level(0))
    {
        const Vector3 from = centre - dirModel * 1.5f;
        float bestT = 1e9f;
        for (Offset o = finest->BeginFaces(); o < finest->EndFaces(); finest->NextFace(o))
        {
            const Poly& face = finest->Face(o);
            const int n = face.N();
            if (n < 3) continue;
            const Vector3 a = finest->Pos(face.GetVertex(0));
            for (int k = 1; k + 1 < n; ++k)
            {
                const Vector3 b = finest->Pos(face.GetVertex(k)), c = finest->Pos(face.GetVertex(k + 1));
                Vector3 g = (b - a).CrossProduct(c - a);
                if (g.SquareSize() < 1e-10f) continue;
                g.Normalize();
                if (std::abs(g.Y()) > 0.6f) continue; // walls only
                const float den = g.DotProduct(dirModel);
                if (std::abs(den) < 0.2f) continue;
                const float tt = g.DotProduct(a - from) / den;
                if (tt < 0.0f || tt > 6.0f || tt >= bestT) continue;
                const Vector3 p = from + dirModel * tt;
                float wa, wb, wc;
                if (Barycentric(p, a, b, c, wa, wb, wc) && wa >= -1e-3f && wb >= -1e-3f && wc >= -1e-3f) bestT = tt;
            }
        }
        if (bestT < 1e8f) centre = from + dirModel * bestT;
    }
    // A hit near the end of a wall lands on the corner or trim, where no wall face holds the hole
    // (three of the owner's LAW hits on 2026-10-19: "no wall face to cut"). Try again with the
    // centre moved along the wall towards the middle of the building.
    Vector3 inward(-centre.X(), 0.0f, -centre.Z());
    inward -= dirModel * inward.DotProduct(dirModel);
    inward[1] = 0.0f;
    const bool canNudge = inward.SquareSize() > 1e-4f;
    if (canNudge) inward.Normalize();
    const float nudges[] = {0.0f, 0.6f, 1.2f};
    const Vector3 hitCentre = centre;
    for (float nudge : nudges)
    {
        if (nudge > 0.0f && !canNudge) break;
        centre = hitCentre + inward * nudge;
        for (int level = 0; level < clone->NLevels(); ++level)
        {
            if (clone->Resolution(level) >= 900.0f) continue; // geometry, memory, shadow volumes...
            Shape* lod = clone->Level(level);
            if (lod) cut += BreachLevel(*lod, centre, dirModel, radius, seed, wallTexture, wallNormal);
        }
        if (cut > 0) break;
    }
    if (cut == 0)
    {
        LOG_INFO(World, "Breach: {} at [{:.1f},{:.1f},{:.1f}]: no wall face to cut", shape->Name(), worldPos.X(),
                 worldPos.Y(), worldPos.Z());
        if (!state.clone) s_breached.erase(building);
        return false;
    }
    clone->CalculateMinMax(true);
    if (!state.original)
    {
        state.original = shape;
        state.object = building;
    }
    state.clone = clone;
    // hand the building back to the renderer with its new model
    if (GEngine) GEngine->SceneObjectRemoved(building);
    building->SetShape(clone);
    if (GEngine) GEngine->SceneObjectCreated(building);
    ++state.holes;
    {
        const auto outline = HoleOutline(radius, seed);
        Vector3 axis = wallNormal;
        if (axis.SquareSize() < 1e-8f) axis = dirModel;
        axis.Normalize();
        state.openings.push_back({centre, axis, *std::min_element(outline.begin(), outline.end())});
    }
    g_breachedCount = int(s_breached.size());

    // the wall's colour for the chunks and the dust: the model's average texture colour
    // (the model's average texture colour when the load computed one; an unset colour reads as
    // white, which made the chunks white cubes)
    const Shape* lod0 = clone->Level(0);
    PackedColor color = lod0 ? lod0->GetColor() : PackedColor(0, 0, 0, 0);
    const int lum = color.R8() + color.G8() + color.B8();
    if (lum < 30 || lum > 600)
        color = PackedColor(Color(0.36f, 0.34f, 0.31f, 1.0f));
    if (s_settings.debris) ThrowChunks(building, worldPos, worldDir, radius, ChunkMesh(wallTexture), seed);
    if (FireVolumesEnabled())
    {
        const float dust[4] = {std::max(0.18f, color.R8() / 255.0f * 0.7f), std::max(0.17f, color.G8() / 255.0f * 0.7f),
                               std::max(0.16f, color.B8() / 255.0f * 0.7f), 0.7f};
        // a short puff of pale masonry dust, not a smoke column
        SpawnFireVolume(worldPos - worldDir * 0.5f, 0.5f + 0.6f * radius, 0.6f, 0.8f, FireKindSmoke, dust);
    }
    LOG_INFO(World, "Breach: {} hole {} at [{:.1f},{:.1f},{:.1f}] radius {:.2f} m, {} faces cut, {} chunks live",
             shape->Name(), state.holes, worldPos.X(), worldPos.Y(), worldPos.Z(), radius, cut, s_chunks.size());
    return true;
}

void BulletImpact(Object* object, Vector3Par worldPos, Vector3Par normal, Vector3Par dir, float hit, Texture* surface)
{
    if (!s_settings.enabled || !object || !object->Static() || !GScene || !GLOB_WORLD) return;
    if (!std::isfinite(worldPos.X()) || !std::isfinite(worldPos.Y()) || !std::isfinite(worldPos.Z())) return;
    Vector3 fly = dir;
    if (fly.SquareSize() < 1e-8f) return;
    fly.Normalize();
    Vector3 n = normal;
    if (!std::isfinite(n.X()) || n.SquareSize() < 1e-6f) n = -fly;
    n.Normalize();
    if (n.DotProduct(fly) > 0.0f) n = -n; // the side the round came from
    // the face's colour: the dust and chips are made of it
    Color wall(0.55f, 0.52f, 0.47f, 1.0f);
    if (surface)
    {
        const Color c = surface->GetColor();
        if (std::isfinite(c.R()) && c.R() + c.G() + c.B() > 0.01f) wall = Color(c.R(), c.G(), c.B(), 1.0f);
    }
    const uint32_t seed = uint32_t(int32_t(std::floor(worldPos.X() * 64.0f))) * 73856093u ^
                          uint32_t(int32_t(std::floor(worldPos.Y() * 64.0f))) * 19349663u ^
                          uint32_t(int32_t(std::floor(worldPos.Z() * 64.0f))) * 83492791u;
    // the mark: a rifle round (hit ~8) spalls a 3-8 cm crater; bigger rounds more
    const Camera* camera = GScene->GetCamera();
    if (LODShapeWithShadow* markMesh = MarkMesh(MarkTexture(wall), int(Hash01(seed, 3) * 4.0f) & 3))
    {
        const float r = std::clamp(0.018f + 0.0028f * hit, 0.02f, 0.06f) * (0.8f + 0.45f * Hash01(seed, 1));
        Vector3 ref = std::fabs(n.Y()) < 0.9f ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
        Vector3 a = ref.CrossProduct(n);
        a.Normalize();
        Vector3 b = n.CrossProduct(a);
        const float turn = TwoPi * Hash01(seed, 2);
        const Vector3 aside = (a * std::cos(turn) + b * std::sin(turn)) * r;
        const Vector3 up = (b * std::cos(turn) - a * std::sin(turn)) * r;
        Mark m{worldPos + n * 0.004f, aside, up, n, 0, markMesh};
        if (s_marks.size() < MaxMarks) s_marks.push_back(m);
        else { s_marks[s_markHead] = m; s_markHead = (s_markHead + 1) % MaxMarks; }
    }
    // the impact: a puff of the wall's dust out of the hole and a few chips thrown off it,
    // near the camera only (it is a few centimetres of dust)
    static LODShapeWithShadow* puff = Shapes.New(GetShapeName("cl_basic"), false, false);
    if (!puff || !camera || camera->Position().Distance2(worldPos) > Square(250.0f)) return;
    const Vector3 bounce = fly - n * (2.0f * fly.DotProduct(n)); // off the wall
    const Color dust(wall.R() * 1.1f, wall.G() * 1.1f, wall.B() * 1.1f, 0.55f);
    for (int i = 0; i < 3; ++i)
    {
        const Vector3 vel = n * (1.2f + 1.6f * GRandGen.RandomValue()) + bounce * 0.6f +
                            Vector3(GRandGen.RandomValue() - 0.5f, GRandGen.RandomValue() * 0.4f, GRandGen.RandomValue() - 0.5f);
        ImpactParticle(puff, 1.4f + 1.2f * GRandGen.RandomValue(), worldPos + n * 0.05f, vel, 0.6f, 0.08f,
                       0.08f + 0.05f * GRandGen.RandomValue(), 0.45f + 0.35f * GRandGen.RandomValue(), dust, true);
    }
    const Color chip(wall.R() * 0.8f, wall.G() * 0.8f, wall.B() * 0.8f, 1.0f);
    const int chips = 4 + int(GRandGen.RandomValue() * 5.0f);
    for (int i = 0; i < chips; ++i)
    {
        Vector3 out = n * (0.6f + GRandGen.RandomValue()) + bounce * 0.5f +
                      Vector3(GRandGen.RandomValue() - 0.5f, GRandGen.RandomValue() - 0.3f, GRandGen.RandomValue() - 0.5f) * 1.2f;
        out.Normalize();
        ImpactParticle(puff, 0.6f + 0.6f * GRandGen.RandomValue(), worldPos + n * 0.02f,
                       out * (3.0f + 6.0f * GRandGen.RandomValue()), 2.5f, 0.002f,
                       0.012f + 0.02f * GRandGen.RandomValue(), 0.012f, chip, false);
    }
}

int BulletMarks() { return int(s_marks.size()); }

void DrawMarks()
{
    if (s_marks.empty() || !GScene || !GScene->GetCamera()) return;
    const Vector3 eye = GScene->GetCamera()->Position();
    int used = 0;
    for (const Mark& m : s_marks)
    {
        if (eye.Distance2(m.pos) > Square(150.0f)) continue;
        LODShapeWithShadow* mesh = m.mesh;
        if (!mesh) continue;
        while (int(s_markObjects.size()) <= used) s_markObjects.push_back(new ObjectColored(mesh, -1));
        ObjectColored* obj = s_markObjects[size_t(used++)];
        obj->SetShape(mesh);
        Matrix3 orientation;
        orientation.SetDirectionAside(m.aside);
        orientation.SetDirectionUp(m.up);
        orientation.SetDirection(m.normal);
        obj->SetOrientation(orientation);
        obj->SetPosition(m.pos);
        GScene->ObjectForDrawing(obj);
    }
}

void DrawDebris()
{
    DrawMarks();
    if (s_chunks.empty() || !GScene) return;
    auto* world = Physics::GetPhysicsWorld();
    if (!world || !world->IsCreated()) return;
    const float now = Glob.time.toFloat();
    int used = 0;
    for (size_t i = 0; i < s_chunks.size();)
    {
        const Chunk& c = s_chunks[i];
        Physics::BodyMotion m;
        if (now - c.born > s_settings.chunkLifeSeconds || now < c.born - 1.0f || !world->GetBodyMotion(c.body, m))
        {
            world->Remove(c.body);
            s_chunks.erase(s_chunks.begin() + long(i));
            continue;
        }
        ObjectColored* obj = ChunkObject(used++);
        obj->SetShape(c.mesh ? c.mesh : Dev::ProbeBoxMesh());
        Matrix3 orientation;
        orientation.SetDirectionAside(m.axisX * c.half.X());
        orientation.SetDirectionUp(m.axisY * c.half.Y());
        orientation.SetDirection(m.axisZ * c.half.Z());
        obj->SetOrientation(orientation);
        obj->SetPosition(m.position);
        GScene->ObjectForDrawing(obj);
        ++i;
    }
}

void Reset()
{
    // (owner, 2026-10-19: "when i reset the map the holes are there") -- the houses keep their
    // objects across a restart, and with them the breached models: give each its original back.
    // Only while the world still holds the building too (the reference here is not the last).
    for (auto& [building, state] : s_breached)
    {
        Object* obj = state.object.GetRef();
        if (!obj || !state.original || obj->RefCounter() <= 1) continue;
        if (GEngine) GEngine->SceneObjectRemoved(obj);
        obj->SetShape(state.original);
        if (GEngine) GEngine->SceneObjectCreated(obj);
    }
    if (auto* world = Physics::GetPhysicsWorld())
        for (const Chunk& c : s_chunks) world->Remove(c.body);
    s_chunks.clear();
    s_breached.clear();
    s_marks.clear();
    s_markHead = 0;
    g_breachedCount = 0;
}

int LiveChunks() { return int(s_chunks.size()); }
int BreachedBuildings() { return int(s_breached.size()); }
} // namespace Poseidon::BuildingBreach
