#include <Poseidon/Asset/Formats/Enfusion/XobCollisionGeometry.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace Poseidon::Asset::Formats::Enfusion
{
namespace
{

// ---------------------------------------------------------------------------
// Small vector algebra. Kept local: the engine's Vector3 is a macro-laden type
// under Math3D.hpp and the format's is a bare struct; neither has the operators.
// ---------------------------------------------------------------------------

struct V3
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

V3 operator+(V3 a, V3 b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
V3 operator-(V3 a, V3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
V3 operator*(V3 a, float s)
{
    return {a.x * s, a.y * s, a.z * s};
}
float Dot(V3 a, V3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
V3 Cross(V3 a, V3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float Length(V3 a)
{
    return std::sqrt(Dot(a, a));
}
V3 Normalized(V3 a)
{
    const float len = Length(a);
    return len > 1e-12f ? a * (1.0f / len) : V3{0.0f, 0.0f, 0.0f};
}
V3 FromXob(const XobVec3& v)
{
    return {v.x, v.y, v.z};
}

// ---------------------------------------------------------------------------
// A component under construction: its own points and its faces (3 or 4 corners,
// [3] == -1 for a triangle). Where the winding comes from depends on the kind:
// a primitive, hull or slab is convex and Emit winds each face toward the
// component's centroid; a piece or chunk of a source mesh arrives pre-wound from
// the mesh's own orientation (InwardSign) and Emit keeps it (orientByCentroid).
// ---------------------------------------------------------------------------

enum class ComponentKind
{
    Primitive,
    Hull,
    Piece,
    Prism
};

struct Component
{
    ComponentKind kind = ComponentKind::Primitive;
    std::vector<V3> points;
    std::vector<std::array<int32_t, 4>> faces;
    float area = 0.0f; //!< prisms: the polygon's area, for the drop order
    //! Prisms only: faces [0, capA) are the first cap, [capA, capB) the second,
    //! the rest the sides. Emit must keep at least one face of each cap and every
    //! side, or the half-space intersection is open on that side.
    size_t capA = 0, capB = 0;
    //! Prisms only: the least triangle height a cap face may be written with. A
    //! cap merged from several triangles is planar only to the patch tolerance,
    //! and the plane the engine builds from three corners closer together than
    //! twice that tolerance points anywhere; a cap that is one source triangle
    //! is exact and only has to be non-degenerate.
    float capMinHeight = 1e-4f;
    //! Emit winds every face toward the component centroid -- right for anything
    //! convex. A piece or chunk of a source mesh is wound by the caller from the
    //! mesh's own orientation instead (a concave corner's faces point away from
    //! the centroid and must stay that way), and Emit keeps the winding as given.
    bool orientByCentroid = true;
};

void AddTriangle(Component& c, int32_t a, int32_t b, int32_t d)
{
    c.faces.push_back({a, b, d, -1});
}
void AddQuad(Component& c, int32_t a, int32_t b, int32_t d, int32_t e)
{
    c.faces.push_back({a, b, d, e});
}

//! A ring of 3+ point indices as one face if it is a triangle or a quad, else a
//! fan of triangles. The fan's planes are all the same plane, which the engine's
//! half-space intersection does not mind.
void AddPolygon(Component& c, const std::vector<int32_t>& ring)
{
    if (ring.size() < 3)
        return;
    if (ring.size() == 3)
        AddTriangle(c, ring[0], ring[1], ring[2]);
    else if (ring.size() == 4)
        AddQuad(c, ring[0], ring[1], ring[2], ring[3]);
    else
        for (size_t i = 1; i + 1 < ring.size(); ++i)
            AddTriangle(c, ring[0], ring[i], ring[i + 1]);
}

//! Newell's normal: the right-hand normal of the whole ring, robust where the
//! first three corners are collinear (a merged polygon keeps a vertex in the middle
//! of a straight edge; the cross of its first three corners is then noise).
V3 RingNormal(const Component& c, const std::array<int32_t, 4>& face, int corners)
{
    V3 n;
    for (int i = 0; i < corners; ++i)
    {
        const V3 p = c.points[static_cast<size_t>(face[static_cast<size_t>(i)])];
        const V3 q = c.points[static_cast<size_t>(face[static_cast<size_t>((i + 1) % corners)])];
        n.x += (p.y - q.y) * (p.z + q.z);
        n.y += (p.z - q.z) * (p.x + q.x);
        n.z += (p.x - q.x) * (p.y + q.y);
    }
    return n;
}

//! The height of the triangle (a, b, d) over its longest side: the conditioning
//! of the plane through those three points. A triple whose height is below the
//! patch's planarity tolerance gives a plane pointing anywhere.
float TripleHeight(V3 a, V3 b, V3 d)
{
    const float doubleArea = Length(Cross(b - a, d - a));
    const float longest = std::max({Length(b - a), Length(d - a), Length(d - b)});
    return longest > 0.0f ? doubleArea / longest : 0.0f;
}

//! The plane the ENGINE will build from a face written with corners w[0..n-1].
//! MLODLoader swaps corners 0/1 (and 2/3 on a quad) on load, and
//! Poly::CalculateNormal takes the loaded face's first three corners as
//! (p2-p0)x(p1-p0) through p0. In the written order that is the triple
//! (w[1], w[0], w[n-1]) -- for a triangle the same three corners, for a quad
//! corners 1, 0 and 3 -- and the resulting normal is the right-hand normal of the
//! written ring, i.e. (b-a)x(c-a) for a triangle written (a, b, c). Everything
//! Emit decides about a face -- which corner to start on, whether the component
//! survives its own inside test -- is decided on THIS triple and no other.
struct EnginePlane
{
    V3 normal; //!< unit, or zero for a degenerate triple
    V3 through;
};

EnginePlane EnginePlaneOf(const V3& w0, const V3& w1, const V3& wLast)
{
    EnginePlane plane;
    plane.normal = Normalized(Cross(wLast - w1, w0 - w1));
    plane.through = w1;
    return plane;
}

//! Writes one component into the LOD as points, normals, faces and a
//! `ComponentNN` selection. A face's written order is its stored ring or the
//! reverse: for a convex component (orientByCentroid) whichever puts the ring's
//! right-hand normal toward the centroid; for a pre-wound piece the ring as
//! given. Either way the engine's plane (EnginePlaneOf) then has the inward
//! normal ConvexComponent needs for IsInside (Plane::Distance >= 0 inside). The
//! corner the ring starts on is chosen so the engine's triple is the
//! best-conditioned one the ring has.
//!
//! Returns false without writing when the component would reach the engine
//! wrong: fewer than the four faces InitConvexComponents requires, or, for a
//! prism, a missing cap or side, which would leave the intersection open.
bool Emit(MLOD::WriteLod& lod, const Component& c, int index)
{
    if (c.points.size() < 4)
        return false;
    V3 centroid;
    for (const V3& p : c.points)
        centroid = centroid + p;
    centroid = centroid * (1.0f / static_cast<float>(c.points.size()));

    const bool prism = c.kind == ComponentKind::Prism;
    // A side is planar by construction and only has to be non-degenerate; a cap's
    // requirement is the prism's own (see Component::capMinHeight).
    constexpr float kMinHeight = 1e-4f;

    std::vector<MLOD::WriteFace> faces;
    faces.reserve(c.faces.size());
    size_t keptA = 0, keptB = 0, keptSides = 0;
    for (size_t f = 0; f < c.faces.size(); ++f)
    {
        const auto& face = c.faces[f];
        const int corners = face[3] < 0 ? 3 : 4;
        const bool isCap = prism && f < c.capB;
        const float minHeight = isCap ? std::max(c.capMinHeight, kMinHeight) : kMinHeight;

        const V3 ringNormal = RingNormal(c, face, corners);
        if (Dot(ringNormal, ringNormal) < 1e-20f)
            continue;
        const V3 a0 = c.points[static_cast<size_t>(face[0])];
        // Newell's normal is the right-hand normal of the ring as stored; the
        // written order is that ring or its reverse.
        const bool forward = !c.orientByCentroid || Dot(ringNormal, centroid - a0) >= 0.0f;
        auto pointAt = [&](int i) { return c.points[static_cast<size_t>(face[static_cast<size_t>(i)])]; };
        // Written corner i when the ring starts on `start` in the chosen direction.
        auto writtenCorner = [&](int start, int i)
        { return forward ? (start + i) % corners : (start + corners * 2 - i) % corners; };

        // Best-conditioned start corner: the one whose ENGINE triple (written
        // corners 1, 0, last) has the greatest height.
        int bestStart = -1;
        float bestHeight = 0.0f;
        for (int r = 0; r < corners; ++r)
        {
            const float height = TripleHeight(pointAt(writtenCorner(r, 0)), pointAt(writtenCorner(r, 1)),
                                              pointAt(writtenCorner(r, corners - 1)));
            if (height > bestHeight)
            {
                bestHeight = height;
                bestStart = r;
            }
        }
        if (bestStart < 0 || bestHeight < minHeight)
            continue;

        MLOD::WriteFace out;
        out.vertexCount = corners;
        for (int i = 0; i < corners; ++i)
        {
            const int source = writtenCorner(bestStart, i);
            out.vertices[i].point = face[static_cast<size_t>(source)];
            out.vertices[i].normal = face[static_cast<size_t>(source)];
        }
        faces.push_back(out);
        if (prism)
        {
            if (f < c.capA)
                ++keptA;
            else if (f < c.capB)
                ++keptB;
            else
                ++keptSides;
        }
    }
    if (faces.size() < 4)
        return false;
    if (prism && (keptA == 0 || keptB == 0 || keptSides != c.faces.size() - c.capB))
        return false;

    // The engine's own check (LogCollisionStats' planesOK), applied before the
    // component exists: a probe point must be inside the plane the engine will
    // build from each face (EnginePlaneOf). A component that fails has an empty
    // or wrongly-cut half-space intersection and is better absent. For a convex
    // component the probe is the centroid. A pre-wound mesh chunk may be
    // legitimately concave-cornered with its centroid outside, so its bounding-box
    // centre is tried as well; a chunk that contains neither is the box-minus-
    // solid of a concave corner, i.e. nothing.
    float radius = 0.0f;
    V3 mn = c.points[0], mx = c.points[0];
    for (const V3& p : c.points)
    {
        radius = std::max(radius, Length(p - centroid));
        mn = {std::min(mn.x, p.x), std::min(mn.y, p.y), std::min(mn.z, p.z)};
        mx = {std::max(mx.x, p.x), std::max(mx.y, p.y), std::max(mx.z, p.z)};
    }
    auto insideAll = [&](V3 probe)
    {
        for (const MLOD::WriteFace& face : faces)
        {
            const EnginePlane plane =
                EnginePlaneOf(c.points[static_cast<size_t>(face.vertices[0].point)],
                              c.points[static_cast<size_t>(face.vertices[1].point)],
                              c.points[static_cast<size_t>(face.vertices[face.vertexCount - 1].point)]);
            if (Dot(plane.normal, probe - plane.through) < -1e-3f * std::max(radius, 1e-3f))
                return false;
        }
        return true;
    };
    if (!insideAll(centroid) && (c.orientByCentroid || !insideAll((mn + mx) * 0.5f)))
        return false;

    const int32_t base = static_cast<int32_t>(lod.points.size());
    MLOD::WriteNamedSelection selection;
    char name[32];
    std::snprintf(name, sizeof(name), "Component%02d", index);
    selection.name = name;
    for (const V3& p : c.points)
    {
        MLOD::WritePoint point;
        point.position = {p.x, p.y, p.z};
        lod.points.push_back(point);
        // A per-point normal, so the loader's (point, normal, uv) merge yields one
        // vertex per point and the round-trip stays an identity. Radial from the
        // centroid, like the engine's own synthetic box; the geometry LOD's vertex
        // normals are never lit.
        V3 n = Normalized(p - centroid);
        if (Dot(n, n) < 0.5f)
            n = {0.0f, 1.0f, 0.0f};
        lod.normals.push_back({n.x, n.y, n.z});
        selection.points.push_back(base + static_cast<int32_t>(selection.points.size()));
    }
    for (MLOD::WriteFace face : faces)
    {
        for (int i = 0; i < face.vertexCount; ++i)
        {
            face.vertices[i].point += base;
            face.vertices[i].normal += base;
        }
        selection.faces.push_back(static_cast<int32_t>(lod.faces.size()));
        lod.faces.push_back(face);
    }
    lod.selections.push_back(std::move(selection));
    return true;
}

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

Component BoxComponent(const XobCollisionShape& s)
{
    Component c;
    const V3 h = FromXob(s.halfExtents);
    for (int i = 0; i < 8; ++i)
    {
        const XobVec3 local{(i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z};
        c.points.push_back(FromXob(s.ToModel(local)));
    }
    // Corner bit i&1 = +x, i&2 = +y, i&4 = +z.
    AddQuad(c, 0, 1, 3, 2); // -z
    AddQuad(c, 4, 6, 7, 5); // +z
    AddQuad(c, 0, 4, 5, 1); // -y
    AddQuad(c, 2, 3, 7, 6); // +y
    AddQuad(c, 0, 2, 6, 4); // -x
    AddQuad(c, 1, 5, 7, 3); // +x
    return c;
}

//! Capsule, cylinder and sphere alike: an N-gon prism along local Y. A capsule's
//! hemispheres are approximated by extending the prism half a radius past the
//! cylinder part; a sphere by a prism 0.75 r tall.
Component RoundComponent(const XobCollisionShape& s, int sides)
{
    Component c;
    float halfHeight = s.halfHeight;
    float radius = s.radius;
    if (s.kind == XobCollisionKind::Capsule)
        halfHeight += 0.5f * radius;
    else if (s.kind == XobCollisionKind::Sphere)
    {
        halfHeight = 0.75f * radius;
        radius *= 0.95f;
    }
    sides = std::max(sides, 3);
    const float step = 6.28318530718f / static_cast<float>(sides);
    for (int end = 0; end < 2; ++end)
    {
        const float y = end == 0 ? -halfHeight : halfHeight;
        for (int k = 0; k < sides; ++k)
        {
            const float angle = step * static_cast<float>(k);
            const XobVec3 local{radius * std::cos(angle), y, radius * std::sin(angle)};
            c.points.push_back(FromXob(s.ToModel(local)));
        }
    }
    std::vector<int32_t> ring;
    for (int k = 0; k < sides; ++k)
        ring.push_back(k);
    AddPolygon(c, ring);
    for (int k = 0; k < sides; ++k)
        ring[static_cast<size_t>(k)] = sides + k;
    AddPolygon(c, ring);
    for (int k = 0; k < sides; ++k)
    {
        const int32_t next = (k + 1) % sides;
        AddQuad(c, k, next, sides + next, sides + k);
    }
    return c;
}

Component HullComponent(const XobCollisionShape& s)
{
    Component c;
    c.kind = ComponentKind::Hull;
    c.points.reserve(s.vertices.size());
    for (const XobVec3& v : s.vertices)
        c.points.push_back(FromXob(s.ToModel(v)));
    std::vector<int32_t> ring;
    for (const auto& polygon : s.polygons)
    {
        ring.clear();
        for (uint32_t i = 0; i < polygon.second; ++i)
            ring.push_back(static_cast<int32_t>(s.indices[polygon.first + i]));
        AddPolygon(c, ring);
    }
    return c;
}

// ---------------------------------------------------------------------------
// Triangle-mesh decomposition
// ---------------------------------------------------------------------------

struct MeshTri
{
    int32_t v[3];
    V3 n;    //!< unit, right-hand from the file's winding
    float d; //!< Dot(n, p) == d on the plane
};

uint64_t EdgeKey(int32_t a, int32_t b)
{
    const uint32_t lo = static_cast<uint32_t>(std::min(a, b));
    const uint32_t hi = static_cast<uint32_t>(std::max(a, b));
    return (static_cast<uint64_t>(lo) << 32) | hi;
}

struct UnionFind
{
    std::vector<int32_t> parent;
    explicit UnionFind(size_t n) : parent(n)
    {
        for (size_t i = 0; i < n; ++i)
            parent[i] = static_cast<int32_t>(i);
    }
    int32_t Find(int32_t x)
    {
        while (parent[static_cast<size_t>(x)] != x)
        {
            parent[static_cast<size_t>(x)] = parent[static_cast<size_t>(parent[static_cast<size_t>(x)])];
            x = parent[static_cast<size_t>(x)];
        }
        return x;
    }
    void Unite(int32_t a, int32_t b)
    {
        a = Find(a);
        b = Find(b);
        if (a != b)
            parent[static_cast<size_t>(a)] = b;
    }
};

//! Welds coincident vertices (1e-4 m) so shared edges are shared by index. UTM
//! meshes are 2-manifold by index already; the weld is insurance against a mesh
//! that duplicated a vertex per face, which would make every triangle its own piece.
void WeldedMesh(const XobCollisionShape& s, std::vector<V3>& points, std::vector<MeshTri>& tris)
{
    struct Key
    {
        int64_t x, y, z;
        bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct KeyHash
    {
        size_t operator()(const Key& k) const
        {
            const uint64_t x = static_cast<uint64_t>(k.x) * 73856093ull;
            const uint64_t y = static_cast<uint64_t>(k.y) * 19349663ull;
            const uint64_t z = static_cast<uint64_t>(k.z) * 83492791ull;
            return static_cast<size_t>(x ^ y ^ z);
        }
    };
    std::unordered_map<Key, int32_t, KeyHash> seen;
    std::vector<int32_t> remap(s.vertices.size(), -1);
    for (size_t i = 0; i < s.vertices.size(); ++i)
    {
        const V3 p = FromXob(s.ToModel(s.vertices[i]));
        const Key key{static_cast<int64_t>(std::llround(p.x * 1e4)), static_cast<int64_t>(std::llround(p.y * 1e4)),
                      static_cast<int64_t>(std::llround(p.z * 1e4))};
        auto it = seen.find(key);
        if (it == seen.end())
        {
            it = seen.emplace(key, static_cast<int32_t>(points.size())).first;
            points.push_back(p);
        }
        remap[i] = it->second;
    }
    for (const auto& polygon : s.polygons)
    {
        // Meshes are triangles by construction (XobCollision normalises them); a
        // larger polygon would be fanned, but none exists in the format.
        for (uint32_t i = 1; i + 1 < polygon.second; ++i)
        {
            MeshTri t;
            t.v[0] = remap[s.indices[polygon.first]];
            t.v[1] = remap[s.indices[polygon.first + i]];
            t.v[2] = remap[s.indices[polygon.first + i + 1]];
            if (t.v[0] == t.v[1] || t.v[1] == t.v[2] || t.v[0] == t.v[2])
                continue;
            const V3 a = points[static_cast<size_t>(t.v[0])];
            const V3 b = points[static_cast<size_t>(t.v[1])];
            const V3 d = points[static_cast<size_t>(t.v[2])];
            const V3 n = Cross(b - a, d - a);
            // A sliver has no plane worth a half-space: the engine would compute
            // one from these three corners and it would point anywhere.
            if (Dot(n, n) < 1e-14f || TripleHeight(a, b, d) < 1e-4f)
                continue;
            t.n = Normalized(n);
            t.d = Dot(t.n, a);
            tris.push_back(t);
        }
    }
}

//! Do the inward normals positively span space? Tested against 26 directions: a
//! half-space intersection whose normals miss a direction is unbounded that way,
//! and an unbounded component makes the whole side of a wall "inside".
bool NormalsBound(const std::vector<V3>& normals)
{
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dz = -1; dz <= 1; ++dz)
            {
                if (dx == 0 && dy == 0 && dz == 0)
                    continue;
                const V3 u = Normalized({static_cast<float>(dx), static_cast<float>(dy), static_cast<float>(dz)});
                bool covered = false;
                for (const V3& n : normals)
                    if (Dot(n, u) > 0.3f)
                    {
                        covered = true;
                        break;
                    }
                if (!covered)
                    return false;
            }
    return true;
}

//! In-plane convexity of a ring: every consecutive turn is a left turn (or
//! straight) seen along `n`.
bool RingIsConvex(const std::vector<V3>& points, const std::vector<int32_t>& ring, V3 n)
{
    const size_t m = ring.size();
    for (size_t i = 0; i < m; ++i)
    {
        const V3 p0 = points[static_cast<size_t>(ring[i])];
        const V3 p1 = points[static_cast<size_t>(ring[(i + 1) % m])];
        const V3 p2 = points[static_cast<size_t>(ring[(i + 2) % m])];
        // Relative, not absolute: a millimetre-scale ring (a hinge, a bracket) has
        // cross products of 1e-6 and would pass an absolute test while concave.
        const V3 e1 = p1 - p0, e2 = p2 - p1;
        if (Dot(Cross(e1, e2), n) < -1e-4f * Length(e1) * Length(e2))
            return false;
    }
    return true;
}

float RingArea(const std::vector<V3>& points, const std::vector<int32_t>& ring)
{
    V3 sum;
    const V3 a = points[static_cast<size_t>(ring[0])];
    for (size_t i = 1; i + 1 < ring.size(); ++i)
        sum = sum + Cross(points[static_cast<size_t>(ring[i])] - a, points[static_cast<size_t>(ring[i + 1])] - a);
    return 0.5f * Length(sum);
}

//! Greedy merge of a planar patch's triangles into convex polygons: two polygons
//! sharing an edge merge when the union ring is simple and convex. Neighbours are
//! found through a directed-edge map, so the cost is linear in edges rather than
//! quadratic in polygons; a 400-triangle floor is fine.
struct MergedRing
{
    std::vector<int32_t> ring;
    bool merged = false; //!< built from more than one triangle
};

std::vector<MergedRing> MergeConvexPolygons(const std::vector<V3>& points, const std::vector<MeshTri>& tris,
                                            const std::vector<int32_t>& patch, V3 n)
{
    std::vector<std::vector<int32_t>> polys;
    polys.reserve(patch.size());
    for (int32_t t : patch)
    {
        std::vector<int32_t> ring = {tris[static_cast<size_t>(t)].v[0], tris[static_cast<size_t>(t)].v[1],
                                     tris[static_cast<size_t>(t)].v[2]};
        // Orient every ring the same way as the patch normal, so shared edges run
        // in opposite directions in the two polygons that share them.
        const V3 a = points[static_cast<size_t>(ring[0])];
        if (Dot(Cross(points[static_cast<size_t>(ring[1])] - a, points[static_cast<size_t>(ring[2])] - a), n) < 0.0f)
            std::swap(ring[1], ring[2]);
        polys.push_back(std::move(ring));
    }

    // directed edge (u -> v) -> polygon index that owns it
    std::unordered_map<uint64_t, int32_t> owner;
    auto directedKey = [](int32_t u, int32_t v)
    { return (static_cast<uint64_t>(static_cast<uint32_t>(u)) << 32) | static_cast<uint32_t>(v); };
    auto claim = [&](int32_t polyIndex)
    {
        const auto& ring = polys[static_cast<size_t>(polyIndex)];
        for (size_t i = 0; i < ring.size(); ++i)
            owner[directedKey(ring[i], ring[(i + 1) % ring.size()])] = polyIndex;
    };
    auto release = [&](int32_t polyIndex)
    {
        const auto& ring = polys[static_cast<size_t>(polyIndex)];
        for (size_t i = 0; i < ring.size(); ++i)
            owner.erase(directedKey(ring[i], ring[(i + 1) % ring.size()]));
    };
    for (size_t i = 0; i < polys.size(); ++i)
        claim(static_cast<int32_t>(i));

    std::vector<bool> alive(polys.size(), true);
    std::vector<bool> wasMerged(polys.size(), false);
    bool merged = true;
    while (merged)
    {
        merged = false;
        for (size_t a = 0; a < polys.size(); ++a)
        {
            if (!alive[a])
                continue;
            bool mergedThis = false;
            const std::vector<int32_t>& A = polys[a];
            for (size_t i = 0; i < A.size() && !mergedThis; ++i)
            {
                const int32_t u = A[i];
                const int32_t v = A[(i + 1) % A.size()];
                auto it = owner.find(directedKey(v, u));
                if (it == owner.end() || it->second == static_cast<int32_t>(a))
                    continue;
                const size_t b = static_cast<size_t>(it->second);
                const std::vector<int32_t>& B = polys[b];
                size_t j = 0;
                for (; j < B.size(); ++j)
                    if (B[j] == v && B[(j + 1) % B.size()] == u)
                        break;
                if (j == B.size())
                    continue;
                // Union: A up to u, then B's interior run from after u back to
                // before v, then the rest of A.
                std::vector<int32_t> ring(A.begin(), A.begin() + static_cast<std::ptrdiff_t>(i) + 1);
                for (size_t k = 2; k < B.size(); ++k)
                    ring.push_back(B[(j + k) % B.size()]);
                ring.insert(ring.end(), A.begin() + static_cast<std::ptrdiff_t>(i) + 1, A.end());
                // Simple ring only: a repeated vertex means the union wraps a hole.
                bool simple = true;
                for (size_t p = 0; p < ring.size() && simple; ++p)
                    for (size_t q = p + 1; q < ring.size(); ++q)
                        if (ring[p] == ring[q])
                        {
                            simple = false;
                            break;
                        }
                if (!simple || !RingIsConvex(points, ring, n))
                    continue;
                release(static_cast<int32_t>(a));
                release(static_cast<int32_t>(b));
                alive[b] = false;
                wasMerged[a] = true;
                polys[a] = std::move(ring);
                claim(static_cast<int32_t>(a));
                mergedThis = true;
                merged = true;
            }
        }
    }

    // Clean each ring: drop a vertex that repeats its predecessor (closer than a
    // millimetre) or that lies on the straight line through its neighbours (the
    // merge keeps such vertices; a prism side over a zero-length edge, or a cap
    // whose first corners are collinear, is a plane the engine cannot compute).
    std::vector<MergedRing> out;
    for (size_t i = 0; i < polys.size(); ++i)
    {
        if (!alive[i])
            continue;
        std::vector<int32_t>& ring = polys[i];
        bool changed = true;
        while (changed && ring.size() >= 3)
        {
            changed = false;
            for (size_t k = 0; k < ring.size(); ++k)
            {
                const V3 prev = points[static_cast<size_t>(ring[(k + ring.size() - 1) % ring.size()])];
                const V3 here = points[static_cast<size_t>(ring[k])];
                const V3 next = points[static_cast<size_t>(ring[(k + 1) % ring.size()])];
                const V3 e1 = here - prev;
                const V3 e2 = next - here;
                const float l1 = Length(e1), l2 = Length(e2);
                if (l1 < 1e-3f || Length(Cross(e1, e2)) < 1e-3f * l1 * l2)
                {
                    ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(k));
                    changed = true;
                    break;
                }
            }
        }
        if (ring.size() >= 3 && RingArea(points, ring) > 1e-6f)
            out.push_back({std::move(ring), wasMerged[i]});
    }
    return out;
}

//! How coplanar two adjacent triangles must be to share a planar patch: a wall
//! panel, to the centimetre. A looser tolerance was tried as a fallback for meshes
//! over the component cap and rejected -- a cap merged across a curved region has
//! no plane three of its corners can name, and the slabs came out worse than the
//! cap dropping the smallest faces.
struct PatchTolerance
{
    float cosAngle;
    float planeDistance;
};
constexpr PatchTolerance kPatchTolerance = {0.999f, 0.01f};

//! Which way is "in" for a piece's triangles, from the mesh itself rather than
//! from where a centroid happens to be: +1 when the file's right-hand normals
//! point INTO the solid, -1 when they point out. A closed piece answers exactly
//! (signed volume); an open piece by majority vote of its faces against the
//! centroid, which is decisive for a boulder with an open underside. 0 when the
//! vote is not decisive -- the caller then falls back to per-face centroid
//! orientation, which is right for convex pieces and all a sheet deserves.
int InwardSign(const std::vector<V3>& points, const std::vector<MeshTri>& tris, const std::vector<int32_t>& subset,
               V3 centroid, bool closed)
{
    if (closed)
    {
        float signedVolume = 0.0f;
        for (int32_t t : subset)
        {
            const MeshTri& tr = tris[static_cast<size_t>(t)];
            signedVolume += Dot(points[static_cast<size_t>(tr.v[0])],
                                Cross(points[static_cast<size_t>(tr.v[1])], points[static_cast<size_t>(tr.v[2])]));
        }
        if (std::fabs(signedVolume) > 1e-9f)
            return signedVolume > 0.0f ? -1 : 1;
    }
    size_t towardCentroid = 0;
    for (int32_t t : subset)
    {
        const MeshTri& tr = tris[static_cast<size_t>(t)];
        if (Dot(tr.n, centroid) - tr.d >= 0.0f)
            ++towardCentroid;
    }
    if (towardCentroid * 10 >= subset.size() * 8)
        return 1;
    if ((subset.size() - towardCentroid) * 10 >= subset.size() * 8)
        return -1;
    return 0;
}

//! The inward normal of a triangle under an orientation sign, or toward the
//! centroid when the sign is 0.
V3 InwardNormal(const MeshTri& tr, int sign, V3 centroid, float& d)
{
    V3 n = tr.n;
    d = tr.d;
    const bool flip = sign != 0 ? sign < 0 : (Dot(n, centroid) - d < 0.0f);
    if (flip)
    {
        n = n * -1.0f;
        d = -d;
    }
    return n;
}

//! Near-convexity of a set of triangles: every inward half-space has every vertex
//! on its inside within `tolerance`. Returns the worst violation.
float WorstViolation(const std::vector<V3>& points, const std::vector<MeshTri>& tris,
                     const std::vector<int32_t>& subset, const std::vector<int32_t>& verts, V3 centroid, int sign,
                     float tolerance, std::vector<V3>* inward)
{
    float worst = 0.0f;
    for (int32_t t : subset)
    {
        float d = 0.0f;
        const V3 n = InwardNormal(tris[static_cast<size_t>(t)], sign, centroid, d);
        if (inward)
            inward->push_back(n);
        for (int32_t v : verts)
            worst = std::max(worst, d - Dot(n, points[static_cast<size_t>(v)]));
        if (worst > tolerance && !inward)
            break;
    }
    return worst;
}

std::vector<int32_t> UniqueVerts(const std::vector<MeshTri>& tris, const std::vector<int32_t>& subset)
{
    std::vector<int32_t> verts;
    verts.reserve(subset.size() * 3);
    for (int32_t t : subset)
        for (int k = 0; k < 3; ++k)
            verts.push_back(tris[static_cast<size_t>(t)].v[k]);
    std::sort(verts.begin(), verts.end());
    verts.erase(std::unique(verts.begin(), verts.end()), verts.end());
    return verts;
}

//! A component from a set of source triangles used whole -- the intersection of
//! their inward half-spaces -- with an optional floor cap for an open piece. Every
//! face is wound inward here (right-hand normal into the solid) and Emit is told
//! to keep it: a concave corner's faces point away from the centroid, and
//! re-orienting them toward it would fill the corner.
Component WholeComponent(const std::vector<V3>& points, const std::vector<MeshTri>& tris,
                         const std::vector<int32_t>& subset, const std::vector<int32_t>& verts, V3 centroid, int sign,
                         bool withFloorCap)
{
    Component c;
    c.kind = ComponentKind::Piece;
    c.orientByCentroid = false;
    std::unordered_map<int32_t, int32_t> local;
    for (int32_t v : verts)
    {
        local[v] = static_cast<int32_t>(c.points.size());
        c.points.push_back(points[static_cast<size_t>(v)]);
    }
    for (int32_t t : subset)
    {
        const MeshTri& tr = tris[static_cast<size_t>(t)];
        float d = 0.0f;
        const bool flipped = Dot(InwardNormal(tr, sign, centroid, d), tr.n) < 0.0f;
        if (flipped)
            AddTriangle(c, local[tr.v[0]], local[tr.v[2]], local[tr.v[1]]);
        else
            AddTriangle(c, local[tr.v[0]], local[tr.v[1]], local[tr.v[2]]);
    }
    if (withFloorCap)
    {
        V3 mn = c.points[0], mx = c.points[0];
        for (const V3& p : c.points)
        {
            mn = {std::min(mn.x, p.x), std::min(mn.y, p.y), std::min(mn.z, p.z)};
            mx = {std::max(mx.x, p.x), std::max(mx.y, p.y), std::max(mx.z, p.z)};
        }
        // A hair below, so the cap is never coplanar with a source face it would
        // otherwise duplicate with a differently-conditioned triple.
        mn.y -= 1e-3f;
        // The floor cap, right-hand normal +y (up, into the piece).
        const int32_t b = static_cast<int32_t>(c.points.size());
        c.points.push_back({mn.x, mn.y, mn.z});
        c.points.push_back({mx.x, mn.y, mn.z});
        c.points.push_back({mx.x, mn.y, mx.z});
        c.points.push_back({mn.x, mn.y, mx.z});
        AddQuad(c, b, b + 3, b + 2, b + 1);
    }
    return c;
}

//! Solid-mesh fallback for a concave piece: a kd-split of the piece's bounding box
//! into cells, each cell taking EVERY triangle whose bounds touch it, down to cells
//! whose triangles are near-convex (or that are small). Each leaf becomes one
//! component: the cell box intersected with its triangles' inward half-spaces.
//!
//! Why the cell must take every triangle that touches it, not the ones centred in
//! it: the component is the box minus everything outside the surface, so a notch
//! inside the cell is carved out only by the notch's own walls. Assigning by
//! centre lets a wall belong to the neighbouring cell and the notch fills in
//! (measured on a synthetic L: the inner corner reported inside). With every
//! touching triangle present the intersection can only UNDER-fill -- a concave
//! corner's two walls cut both arms inside their cell -- which is what the
//! near-convexity recursion bounds.
//!
//! This is what a lumpy boulder or a cliff wants -- its interior is solid -- and
//! what a building must never get: a cell around a room takes the room's walls,
//! whose inward side is the wall material, and the room's air is not carved out.
void ChunkSolid(const std::vector<V3>& points, const std::vector<MeshTri>& tris, const std::vector<int32_t>& subset,
                V3 cellMin, V3 cellMax, int sign, float tolerance, const GeometryLodOptions& options, int depth,
                std::vector<Component>& components, GeometryLodStats& stats)
{
    if (subset.empty())
        return;
    const std::vector<int32_t> verts = UniqueVerts(tris, subset);
    // The cell's own centre, not the triangles': a triangle reaching far outside
    // the cell says nothing about the cell.
    const V3 centroid = (cellMin + cellMax) * 0.5f;
    const V3 extent = cellMax - cellMin;
    const float longest = std::max({extent.x, extent.y, extent.z});

    // Violation measured on the vertices clamped into the cell, so a triangle's
    // far end outside the cell does not disqualify the cell.
    float worst = 0.0f;
    for (int32_t t : subset)
    {
        float d = 0.0f;
        const V3 n = InwardNormal(tris[static_cast<size_t>(t)], sign, centroid, d);
        for (int32_t v : verts)
        {
            V3 p = points[static_cast<size_t>(v)];
            p = {std::min(std::max(p.x, cellMin.x), cellMax.x), std::min(std::max(p.y, cellMin.y), cellMax.y),
                 std::min(std::max(p.z, cellMin.z), cellMax.z)};
            worst = std::max(worst, d - Dot(n, p));
        }
        if (worst > tolerance)
            break;
    }
    const bool nearConvex = worst <= tolerance;

    // Split at the median triangle centre along the longest cell axis; children
    // take every triangle whose bounds overlap them.
    const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : (extent.y >= extent.z ? 1 : 2);
    auto component = [axis](V3 v) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; };
    std::vector<float> centres;
    if (!nearConvex && subset.size() > 2 && depth < 14 && longest >= 0.25f)
    {
        centres.reserve(subset.size());
        for (int32_t t : subset)
        {
            const MeshTri& tr = tris[static_cast<size_t>(t)];
            const V3 sum = points[static_cast<size_t>(tr.v[0])] + points[static_cast<size_t>(tr.v[1])] +
                           points[static_cast<size_t>(tr.v[2])];
            centres.push_back(component(sum) / 3.0f);
        }
        std::sort(centres.begin(), centres.end());
    }
    float split = 0.0f;
    bool canSplit = false;
    if (!centres.empty())
    {
        split = centres[centres.size() / 2];
        // Inside the cell with a margin, or the split is a no-op.
        const float lo = component(cellMin), hi = component(cellMax);
        split = std::min(std::max(split, lo + 0.05f * (hi - lo)), hi - 0.05f * (hi - lo));
        canSplit = split > lo && split < hi;
    }
    if (canSplit)
    {
        V3 lowMax = cellMax, highMin = cellMin;
        (axis == 0 ? lowMax.x : axis == 1 ? lowMax.y : lowMax.z) = split;
        (axis == 0 ? highMin.x : axis == 1 ? highMin.y : highMin.z) = split;
        std::vector<int32_t> low, high;
        for (int32_t t : subset)
        {
            const MeshTri& tr = tris[static_cast<size_t>(t)];
            float tmin = component(points[static_cast<size_t>(tr.v[0])]), tmax = tmin;
            for (int k = 1; k < 3; ++k)
            {
                const float c = component(points[static_cast<size_t>(tr.v[k])]);
                tmin = std::min(tmin, c);
                tmax = std::max(tmax, c);
            }
            if (tmin <= split)
                low.push_back(t);
            if (tmax >= split)
                high.push_back(t);
        }
        // Progress guard: a split that leaves both children with the whole set
        // (every triangle straddles the plane) would recurse to the depth limit
        // for nothing.
        if (low.size() < subset.size() || high.size() < subset.size())
        {
            ChunkSolid(points, tris, low, cellMin, lowMax, sign, tolerance, options, depth + 1, components, stats);
            ChunkSolid(points, tris, high, highMin, cellMax, sign, tolerance, options, depth + 1, components, stats);
            return;
        }
    }

    // Leaf: the cell box and the triangles' inward half-spaces.
    Component c = WholeComponent(points, tris, subset, verts, centroid, sign, false);
    const V3 pad{1e-3f, 1e-3f, 1e-3f};
    const V3 mn = cellMin - pad, mx = cellMax + pad;
    const int32_t b = static_cast<int32_t>(c.points.size());
    for (int i = 0; i < 8; ++i)
        c.points.push_back({(i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z});
    AddQuad(c, b + 0, b + 1, b + 3, b + 2); // -z, normal +z
    AddQuad(c, b + 4, b + 6, b + 7, b + 5); // +z, normal -z
    AddQuad(c, b + 0, b + 4, b + 5, b + 1); // -y, normal +y
    AddQuad(c, b + 2, b + 3, b + 7, b + 6); // +y, normal -y
    AddQuad(c, b + 0, b + 2, b + 6, b + 4); // -x, normal +x
    AddQuad(c, b + 1, b + 5, b + 7, b + 3); // +x, normal -x
    components.push_back(std::move(c));
    ++stats.meshChunks;
}

void DecomposeMesh(const XobCollisionShape& s, const GeometryLodOptions& options, std::vector<Component>& components,
                   GeometryLodStats& stats)
{
    const PatchTolerance& patchTolerance = kPatchTolerance;
    std::vector<V3> points;
    std::vector<MeshTri> tris;
    WeldedMesh(s, points, tris);
    stats.meshTriangles += tris.size();
    if (tris.empty())
        return;

    // Edge -> triangles, and connected pieces over shared edges.
    std::unordered_map<uint64_t, std::vector<int32_t>> edgeTris;
    for (size_t t = 0; t < tris.size(); ++t)
        for (int k = 0; k < 3; ++k)
            edgeTris[EdgeKey(tris[t].v[k], tris[t].v[(k + 1) % 3])].push_back(static_cast<int32_t>(t));
    UnionFind pieces(tris.size());
    for (const auto& entry : edgeTris)
        for (size_t i = 1; i < entry.second.size(); ++i)
            pieces.Unite(entry.second[0], entry.second[i]);
    std::unordered_map<int32_t, std::vector<int32_t>> byPiece;
    for (size_t t = 0; t < tris.size(); ++t)
        byPiece[pieces.Find(static_cast<int32_t>(t))].push_back(static_cast<int32_t>(t));

    // Deterministic order: by lowest triangle index.
    std::vector<std::vector<int32_t>> pieceList;
    for (auto& entry : byPiece)
        pieceList.push_back(std::move(entry.second));
    std::sort(pieceList.begin(), pieceList.end(),
              [](const std::vector<int32_t>& a, const std::vector<int32_t>& b) { return a.front() < b.front(); });

    for (const std::vector<int32_t>& piece : pieceList)
    {
        // Piece vertices, centroid, radius, closure.
        const std::vector<int32_t> verts = UniqueVerts(tris, piece);
        V3 centroid;
        for (int32_t v : verts)
            centroid = centroid + points[static_cast<size_t>(v)];
        centroid = centroid * (1.0f / static_cast<float>(verts.size()));
        float radius = 0.0f;
        for (int32_t v : verts)
            radius = std::max(radius, Length(points[static_cast<size_t>(v)] - centroid));
        bool closed = true;
        for (int32_t t : piece)
            for (int k = 0; k < 3 && closed; ++k)
                closed =
                    edgeTris[EdgeKey(tris[static_cast<size_t>(t)].v[k], tris[static_cast<size_t>(t)].v[(k + 1) % 3])]
                        .size() == 2;

        // 2. Near-convex whole piece: every plane, oriented toward the centroid,
        // has every vertex on its inside within tolerance, and the inward normals
        // bound a volume (with a floor cap when the piece is open).
        //
        // Closed pieces only, unless the mesh is a solid. A hollow room built from
        // single-sided sheets with a door opening is an OPEN piece whose planes
        // are all mutually consistent -- the near-convex test cannot tell it from
        // a boulder, and treating it whole seals the door. A boulder with an open
        // underside is the case `solidMesh` exists for.
        ++stats.meshPieces;
        if (closed)
            ++stats.meshPiecesClosed;
        const int sign = InwardSign(points, tris, piece, centroid, closed);
        if (piece.size() >= 4 && radius > 1e-3f && (closed || options.solidMesh))
        {
            const float tolerance = std::min(options.nearConvexTolerance * radius, options.nearConvexMaxTolerance);
            std::vector<V3> inward;
            inward.reserve(piece.size() + 1);
            const float worst = WorstViolation(points, tris, piece, verts, centroid, sign, tolerance, &inward);
            if (!closed)
                inward.push_back({0.0f, 1.0f, 0.0f}); // the floor cap's inward normal
            const bool nearConvex = worst <= tolerance;
            const bool bounded = nearConvex && NormalsBound(inward);
            if (!nearConvex)
                ++stats.meshPiecesConcave;
            else if (!bounded)
                ++stats.meshPiecesUnbounded;
            if (nearConvex && bounded)
            {
                components.push_back(WholeComponent(points, tris, piece, verts, centroid, sign, !closed));
                continue;
            }
            // A solid that is not near-convex is still solid: chunk it rather
            // than slab it, and rather than let the cap drop half of it.
            if (options.solidMesh && !nearConvex)
            {
                V3 mn = points[static_cast<size_t>(verts[0])], mx = mn;
                for (int32_t v : verts)
                {
                    const V3 p = points[static_cast<size_t>(v)];
                    mn = {std::min(mn.x, p.x), std::min(mn.y, p.y), std::min(mn.z, p.z)};
                    mx = {std::max(mx.x, p.x), std::max(mx.y, p.y), std::max(mx.z, p.z)};
                }
                // The cells' tolerance is the PIECE's scale, held constant down the
                // split: half the whole-piece allowance, so a boulder stops splitting
                // once its crevices are shallower than a quarter of its radius rather
                // than going down to cells the size of the tolerance they would earn.
                ChunkSolid(points, tris, piece, mn, mx, sign, 0.5f * tolerance, options, 0, components, stats);
                continue;
            }
        }

        // 3. Planar patches -> convex polygons -> slabs.
        UnionFind patches(tris.size());
        for (int32_t t : piece)
            for (int k = 0; k < 3; ++k)
            {
                for (int32_t o :
                     edgeTris[EdgeKey(tris[static_cast<size_t>(t)].v[k], tris[static_cast<size_t>(t)].v[(k + 1) % 3])])
                {
                    if (o == t)
                        continue;
                    const MeshTri& a = tris[static_cast<size_t>(t)];
                    const MeshTri& b = tris[static_cast<size_t>(o)];
                    const float align = Dot(a.n, b.n);
                    if (std::fabs(align) < patchTolerance.cosAngle)
                        continue;
                    const float db = align < 0.0f ? -b.d : b.d;
                    if (std::fabs(a.d - db) < patchTolerance.planeDistance)
                        patches.Unite(t, o);
                }
            }
        std::unordered_map<int32_t, std::vector<int32_t>> byPatch;
        for (int32_t t : piece)
            byPatch[patches.Find(t)].push_back(t);
        std::vector<std::vector<int32_t>> patchList;
        for (auto& entry : byPatch)
            patchList.push_back(std::move(entry.second));
        std::sort(patchList.begin(), patchList.end(),
                  [](const std::vector<int32_t>& a, const std::vector<int32_t>& b) { return a.front() < b.front(); });

        // Which way is in, for a closed shell: the signed volume of the file's
        // winding. Positive means right-hand normals point out.
        float signedVolume = 0.0f;
        if (closed)
            for (int32_t t : piece)
            {
                const MeshTri& tr = tris[static_cast<size_t>(t)];
                signedVolume += Dot(points[static_cast<size_t>(tr.v[0])],
                                    Cross(points[static_cast<size_t>(tr.v[1])], points[static_cast<size_t>(tr.v[2])]));
            }
        // A patch merged at a loose tolerance is curved by up to its plane
        // distance; the slab must be at least that thick or its two caps cross.
        const float halfThickness = std::max(options.slabHalfThickness, patchTolerance.planeDistance);
        const float thickness = 2.0f * halfThickness;

        for (const std::vector<int32_t>& patch : patchList)
        {
            const V3 n = tris[static_cast<size_t>(patch.front())].n;
            for (const MergedRing& mergedRing : MergeConvexPolygons(points, tris, patch, n))
            {
                const std::vector<int32_t>& ring = mergedRing.ring;
                V3 offsetA, offsetB;
                if (closed && std::fabs(signedVolume) > 1e-9f)
                {
                    // Extrude into the solid.
                    const V3 inward = signedVolume > 0.0f ? n * -1.0f : n;
                    offsetB = inward * thickness;
                }
                else if (std::fabs(n.y) > 0.7f)
                {
                    // A floor or a roof plate: straight down, never a step up.
                    offsetB = {0.0f, -thickness, 0.0f};
                }
                else
                {
                    offsetA = n * -halfThickness;
                    offsetB = n * halfThickness;
                }
                Component c;
                c.kind = ComponentKind::Prism;
                c.area = RingArea(points, ring);
                c.capMinHeight = mergedRing.merged ? 2.0f * patchTolerance.planeDistance : 1e-4f;
                const int32_t k = static_cast<int32_t>(ring.size());
                for (int32_t v : ring)
                    c.points.push_back(points[static_cast<size_t>(v)] + offsetA);
                for (int32_t v : ring)
                    c.points.push_back(points[static_cast<size_t>(v)] + offsetB);
                std::vector<int32_t> cap(ring.size());
                for (int32_t i = 0; i < k; ++i)
                    cap[static_cast<size_t>(i)] = i;
                AddPolygon(c, cap);
                c.capA = c.faces.size();
                for (int32_t i = 0; i < k; ++i)
                    cap[static_cast<size_t>(i)] = k + i;
                AddPolygon(c, cap);
                c.capB = c.faces.size();
                for (int32_t i = 0; i < k; ++i)
                {
                    const int32_t next = (i + 1) % k;
                    AddQuad(c, i, next, k + next, k + i);
                }
                components.push_back(std::move(c));
            }
        }
    }
}

} // namespace

bool BuildGeometryLod(const XobCollision& collision, const GeometryLodOptions& options, MLOD::WriteLod& out,
                      GeometryLodStats& stats)
{
    out = MLOD::WriteLod();
    stats = GeometryLodStats();
    stats.shapesTotal = collision.shapes.size();

    std::vector<Component> components;
    for (const XobCollisionShape& shape : collision.shapes)
    {
        if (!options.allLayers && !XobLayerBlocksCharacters(shape.layer))
            continue;
        ++stats.shapesBlocking;
        const size_t before = components.size();
        switch (shape.kind)
        {
            case XobCollisionKind::Box:
                if (shape.halfExtents.x > 1e-4f && shape.halfExtents.y > 1e-4f && shape.halfExtents.z > 1e-4f)
                    components.push_back(BoxComponent(shape));
                break;
            case XobCollisionKind::Capsule:
            case XobCollisionKind::Cylinder:
            case XobCollisionKind::Sphere:
                if (shape.radius > 1e-4f && (shape.kind == XobCollisionKind::Sphere || shape.halfHeight > 1e-4f))
                    components.push_back(RoundComponent(shape, options.roundSides));
                break;
            case XobCollisionKind::ConvexHull:
                if (shape.vertices.size() >= 4 && !shape.polygons.empty())
                    components.push_back(HullComponent(shape));
                break;
            case XobCollisionKind::TriMesh:
            case XobCollisionKind::TriMeshGrouped:
                DecomposeMesh(shape, options, components, stats);
                break;
            default:
                break;
        }
        if (components.size() > before)
            ++stats.shapesUsed;
    }

    // Convex-by-construction components first, then slabs largest first, so the
    // cap drops trims and bevels before walls. stable_sort keeps file order inside
    // each class, which keeps ComponentNN numbering reproducible.
    std::stable_sort(components.begin(), components.end(),
                     [](const Component& a, const Component& b)
                     {
                         const bool prismA = a.kind == ComponentKind::Prism;
                         const bool prismB = b.kind == ComponentKind::Prism;
                         if (prismA != prismB)
                             return !prismA;
                         if (prismA)
                             return a.area > b.area;
                         return false;
                     });
    // The cap counts WRITTEN components: an empty solid-mesh cell consumes no
    // number, so it must not consume a place under the cap either.
    int index = 1;
    for (const Component& c : components)
    {
        if (static_cast<size_t>(index) > options.maxComponents)
        {
            ++stats.componentsDropped;
            continue;
        }
        if (!Emit(out, c, index))
        {
            // A solid-mesh cell whose box lies entirely outside the surface (the
            // air in a notch) is expected to come out empty; that is the kd-split
            // working, not a loss.
            if (c.kind == ComponentKind::Piece && !c.orientByCentroid)
                ++stats.meshChunksEmpty;
            else
                ++stats.componentsDropped;
            continue;
        }
        ++index;
        switch (c.kind)
        {
            case ComponentKind::Primitive:
                ++stats.componentsPrimitive;
                break;
            case ComponentKind::Hull:
                ++stats.componentsHull;
                break;
            case ComponentKind::Piece:
                ++stats.componentsPiece;
                break;
            case ComponentKind::Prism:
                ++stats.componentsPrism;
                break;
        }
    }

    if (out.points.empty() || out.selections.empty())
    {
        out = MLOD::WriteLod();
        return false;
    }
    out.resolution = 1.0e13f;
    out.mass.assign(out.points.size(), options.totalMass / static_cast<float>(out.points.size()));
    stats.points = out.points.size();
    stats.faces = out.faces.size();
    return true;
}

} // namespace Poseidon::Asset::Formats::Enfusion
