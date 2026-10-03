// Malprave terrainHole1 (Claude for Dec, 2 Oct 2026): object-cut terrain holes, Torque3D style.
// A building model marks the ground it removes with memory LOD points: selection "terrain_hole" or "terrain_hole1" ..
// "terrain_hole8", each the corners of one convex footprint (model X/Z; heights are ignored). The object must also have
// a roadway LOD (the floor / stairs that replace the ground). Every "where is the ground" query asks here, so the
// standing height, ground collision and (in later patches) drawing, rays and the AI map all use the same holes.
// Models without such a selection are unaffected; nothing changes for existing content.

// terrainHole8: "terrain_hole_hidden" / "terrain_hole_hidden1" .. "terrain_hole_hidden8": a hole for everything except
// drawing (standing, collision, rays, sight, AI, camera, sound) - the terrain is still drawn over it, so a cave's
// closed roof shows the normal ground from above; only the openings use the drawn "terrain_hole" selections.
// terrainHole7: InTerrainHoleRoom (cellar reverb).
// terrainHole6: DecalSurfaceY, TrackSurfaceY (ground decals and tracks down in a hole).
// terrainHole5: InTerrainHoleBelow, LeavesTerrainHoleUnderground, CutsTerrainHole (explosions, men, formations).
// terrainHole4: CameraFloorY lets the camera below the terrain inside a hole (World, camera effects, free camera).

// terrainHole3: a registry of the hole-cutting objects found so far (point queries no longer scan the object grid),
// and a floor under every hole: the lowest point of the object's roadway, so nothing falls through a gap in a floor.

#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Poseidon
{
namespace
{
struct ShapeHolePoly
{
    int n = 0;
    float x[16], z[16]; // model XZ, convex, counter-clockwise
    float floorY = 0;   // terrainHole3: lowest roadway point of the model (model Y)
    float ceilingY = 1e20f;
    bool draw = true;   // terrainHole8: false for terrain_hole_hidden*: the terrain is still drawn there
};

struct ShapeHoles
{
    std::string name; // the cache is keyed by pointer; the name catches a reused address
    std::vector<ShapeHolePoly> polys;
};

std::mutex GHoleMutex;
std::unordered_map<const LODShape*, ShapeHoles> GHoleCache;
// terrainHole2: number of models seen so far that cut holes. While it is 0 the ray / line-of-sight checks skip the
// hole lookup entirely. Models are found by any gather: ground collision of a unit near them, or the terrain draw
// (every model within 400 m of the camera).
std::atomic<int> GHoleShapeCount{0};
// terrainHole3: objects whose model cuts holes (weak links: a deleted object drops out by itself)
std::vector<OLink<Object>> GHoleObjects;

void RegisterHoleObject(const Object* obj)
{
    for (const OLink<Object>& o : GHoleObjects)
    {
        if (o == obj)
        {
            return;
        }
    }
    OLink<Object> link;
    link = const_cast<Object*>(obj);
    GHoleObjects.push_back(link);
}

float Cross2(float ox, float oz, float ax, float az, float bx, float bz)
{
    return (ax - ox) * (bz - oz) - (az - oz) * (bx - ox);
}

// convex hull (Andrew's monotone chain), counter-clockwise in the X/Z plane, at most 16 points
bool HullXZ(std::vector<std::pair<float, float>> pts, ShapeHolePoly& poly)
{
    std::sort(pts.begin(), pts.end());
    pts.erase(std::unique(pts.begin(), pts.end(),
                          [](const std::pair<float, float>& a, const std::pair<float, float>& b)
                          { return std::fabs(a.first - b.first) < 1e-4f && std::fabs(a.second - b.second) < 1e-4f; }),
              pts.end());
    if (pts.size() < 3)
    {
        return false;
    }
    std::vector<std::pair<float, float>> h(pts.size() * 2);
    size_t k = 0;
    for (size_t i = 0; i < pts.size(); i++)
    {
        while (k >= 2 && Cross2(h[k - 2].first, h[k - 2].second, h[k - 1].first, h[k - 1].second, pts[i].first,
                                pts[i].second) <= 0)
        {
            k--;
        }
        h[k++] = pts[i];
    }
    for (size_t i = pts.size() - 1, t = k + 1; i > 0; i--)
    {
        while (k >= t && Cross2(h[k - 2].first, h[k - 2].second, h[k - 1].first, h[k - 1].second, pts[i - 1].first,
                                pts[i - 1].second) <= 0)
        {
            k--;
        }
        h[k++] = pts[i - 1];
    }
    h.resize(k - 1);
    if (h.size() < 3 || h.size() > 16)
    {
        return false;
    }
    poly.n = (int)h.size();
    for (int i = 0; i < poly.n; i++)
    {
        poly.x[i] = h[i].first;
        poly.z[i] = h[i].second;
    }
    return true;
}

// call with GHoleMutex held
const std::vector<ShapeHolePoly>& ShapeHolesOf(const LODShape* shape)
{
    const char* shapeName = shape->Name() ? shape->Name() : "";
    auto it = GHoleCache.find(shape);
    if (it != GHoleCache.end() && it->second.name == shapeName)
    {
        return it->second.polys;
    }
    ShapeHoles& holes = GHoleCache[shape];
    holes.name = shapeName;
    holes.polys.clear();
    float floorY = 0;
    if (const Shape* roadway = shape->RoadwayLevel())
    {
        floorY = 1e10f;
        for (int i = 0; i < roadway->NPos(); i++)
        {
            floorY = std::min(floorY, roadway->Pos(i).Y());
        }
        if (floorY > 1e9f)
        {
            floorY = 0;
        }
    }
    const Shape* mem = shape->MemoryLevel();
    if (mem)
    {
        // Explicit opt-in; existing footprint point heights remain ignored.
        float ceilingY = 1e20f;
        const int ceilingSelection = mem->FindNamedSel("terrain_hole_ceiling");
        if (ceilingSelection >= 0)
        {
            const auto& selection = mem->NamedSel(ceilingSelection);
            if (selection.Size() != 1 || !std::isfinite(mem->Pos(selection[0]).Y()) ||
                mem->Pos(selection[0]).Y() <= floorY)
                return holes.polys;
            ceilingY = mem->Pos(selection[0]).Y();
        }
        for (int k = 0; k <= 17; k++)
        {
            // terrainHole8: k 0-8 = drawn holes, 9-17 = hidden ones (terrain_hole_hidden, terrain_hole_hidden1 .. 8)
            const bool hidden = k > 8;
            const int s = hidden ? k - 9 : k;
            const char* base = hidden ? "terrain_hole_hidden" : "terrain_hole";
            char name[32];
            if (s == 0)
            {
                snprintf(name, sizeof(name), "%s", base);
            }
            else
            {
                snprintf(name, sizeof(name), "%s%d", base, s);
            }
            int si = mem->FindNamedSel(name);
            if (si < 0)
            {
                continue;
            }
            const NamedSelection& sel = mem->NamedSel(si);
            std::vector<std::pair<float, float>> pts;
            for (int i = 0; i < sel.Size(); i++)
            {
                Vector3Val p = mem->Pos(sel[i]);
                pts.emplace_back(p.X(), p.Z());
            }
            ShapeHolePoly poly;
            poly.floorY = floorY;
            poly.ceilingY = ceilingY;
            poly.draw = !hidden;
            if (HullXZ(pts, poly))
            {
                holes.polys.push_back(poly);
            }
            else
            {
                LOG_WARN(Physics, "terrainHole: {} selection {} needs 3-16 points around a non-empty area", shapeName,
                         name);
            }
        }
    }
    if (!holes.polys.empty())
    {
        GHoleShapeCount.fetch_add(1, std::memory_order_relaxed);
        LOG_INFO(Physics, "terrainHole: {} cuts {} hole(s) in the terrain", shapeName, (int)holes.polys.size());
    }
    return holes.polys;
}
// world X/Z footprint (counter-clockwise), bounding box and floor height of one hole of an object.
// Returns false for a degenerate footprint, which the caller must drop: see the note on the area check below.
bool MakeHoleArea(const Object* obj, const ShapeHolePoly& poly, Landscape::TerrainHoleArea& area)
{
    area.n = poly.n;
    area.draw = poly.draw;
    float cx = 0, cz = 0;
    for (int v = 0; v < poly.n; v++)
    {
        Vector3 w = obj->PositionModelToWorld(Vector3(poly.x[v], 0, poly.z[v]));
        area.x[v] = w.X();
        area.z[v] = w.Z();
        cx += poly.x[v];
        cz += poly.z[v];
    }
    float signedArea = 0;
    for (int v = 0; v < poly.n; v++)
    {
        int u = (v + 1) % poly.n;
        // Subtract an anchor first: world-coordinate products lose centimetre
        // footprints to cancellation many kilometres from the origin.
        signedArea += (area.x[v]-area.x[0])*(area.z[u]-area.z[0]) -
                      (area.x[u]-area.x[0])*(area.z[v]-area.z[0]);
    }
    // Sinkhole W1 (owner report: "the island surface goes black and all objects appear floating" while a mission
    // loads, switches or exits): an object whose transform is momentarily collapsed -- zero scale, not yet placed,
    // being torn down -- maps every hole point to one spot. Each edge test of a zero-length edge reads 0 >= 0, so
    // the hole contained EVERY point: the whole terrain was discarded by the draw and given no ground by the
    // gameplay queries. A real hole is at least a few square decimetres; anything smaller (or not finite) is no hole.
    if (!std::isfinite(signedArea) || std::fabs(signedArea) * 0.5f < 0.01f)
    {
        area.n = 0;
        return false;
    }
    if (signedArea < 0)
    {
        // a mirrored transform turned it clockwise
        std::reverse(area.x, area.x + poly.n);
        std::reverse(area.z, area.z + poly.n);
    }
    area.minX = *std::min_element(area.x, area.x + poly.n);
    area.maxX = *std::max_element(area.x, area.x + poly.n);
    area.minZ = *std::min_element(area.z, area.z + poly.n);
    area.maxZ = *std::max_element(area.z, area.z + poly.n);
    area.floorY = obj->PositionModelToWorld(Vector3(cx / poly.n, poly.floorY, cz / poly.n)).Y();
    area.ceilingY = 1e20f;
    if (poly.ceilingY < 1e19f)
    {
        // Ceiling metadata is world-horizontal. Refuse pitched/sheared instances,
        // rather than cutting the wrong half of a mountain.
        if (std::abs(obj->DirectionAside().Y()) > 1e-4f || std::abs(obj->Direction().Y()) > 1e-4f)
            return false;
        area.ceilingY = obj->PositionModelToWorld(Vector3(cx/poly.n,poly.ceilingY,cz/poly.n)).Y();
        if (!std::isfinite(area.ceilingY) || area.ceilingY <= area.floorY) return false;
    }
    return true;
}
} // namespace

int Landscape::GatherTerrainHoles(Vector3Par pos, float radius, TerrainHoleArea* out, int maxOut) const
{
    int xMin, xMax, zMin, zMax;
    ObjRadiusRectangle(xMin, xMax, zMin, zMax, pos, pos, radius);
    int n = 0;
    std::lock_guard<std::mutex> lock(GHoleMutex);
    for (int z = zMin; z <= zMax; z++)
    {
        for (int x = xMin; x <= xMax; x++)
        {
            const ObjectList& list = _objects(x, z);
            for (int i = 0; i < list.Size(); i++)
            {
                const Object* obj = list[i];
                if (!obj)
                {
                    continue;
                }
                const LODShape* shape = obj->GetShape();
                if (!shape || shape->FindRoadwayLevel() < 0 || shape->FindMemoryLevel() < 0)
                {
                    continue; // a hole needs a floor
                }
                float r = shape->BoundingSphere() + radius;
                if ((obj->Position() - pos).SquareSizeXZ() > r * r)
                {
                    continue;
                }
                const std::vector<ShapeHolePoly>& polys = ShapeHolesOf(shape);
                if (!polys.empty())
                {
                    RegisterHoleObject(obj);
                }
                for (const ShapeHolePoly& poly : polys)
                {
                    if (n >= maxOut)
                    {
                        return n;
                    }
                    if (MakeHoleArea(obj, poly, out[n]))
                    {
                        n++;
                    }
                }
            }
        }
    }
    return n;
}

int Landscape::TerrainHoleIndex(const TerrainHoleArea* holes, int n, float x, float z, float y)
{
    for (int h = 0; h < n; h++)
    {
        const TerrainHoleArea& a = holes[h];
        if (a.n < 3 || a.n > 16 || !std::isfinite(x) || !std::isfinite(z) || !std::isfinite(y) ||
            !std::isfinite(a.ceilingY) || y > a.ceilingY || x < a.minX || x > a.maxX || z < a.minZ || z > a.maxZ)
        {
            continue;
        }
        bool inside = true;
        for (int v = 0; v < a.n && inside; v++)
        {
            int u = (v + 1) % a.n;
            if (Cross2(a.x[v], a.z[v], a.x[u], a.z[u], x, z) < 0)
            {
                inside = false;
            }
        }
        if (inside)
        {
            return h;
        }
    }
    return -1;
}

bool Landscape::InTerrainHoles(const TerrainHoleArea* holes, int n, float x, float z)
{
    return TerrainHoleIndex(holes, n, x, z) >= 0;
}

bool Landscape::InTerrainHole(float x, float z, float* floorY, float y) const
{
    if (GHoleShapeCount.load(std::memory_order_relaxed) == 0)
    {
        return false; // terrainHole2: no hole-cutting model seen yet
    }
    // terrainHole3: only the registered hole-cutting objects are checked (no object-grid scan, no size limit)
    std::lock_guard<std::mutex> lock(GHoleMutex);
    for (size_t i = 0; i < GHoleObjects.size();)
    {
        const Object* obj = GHoleObjects[i];
        if (!obj)
        {
            GHoleObjects.erase(GHoleObjects.begin() + i); // deleted
            continue;
        }
        i++;
        const LODShape* shape = obj->GetShape();
        if (!shape)
        {
            continue;
        }
        float r = shape->BoundingSphere();
        float dx = obj->Position().X() - x, dz = obj->Position().Z() - z;
        if (dx * dx + dz * dz > r * r)
        {
            continue;
        }
        for (const ShapeHolePoly& poly : ShapeHolesOf(shape))
        {
            TerrainHoleArea area;
            if (!MakeHoleArea(obj, poly, area)) continue;
            if (TerrainHoleIndex(&area, 1, x, z, y) == 0)
            {
                if (floorY)
                {
                    *floorY = area.floorY;
                }
                return true;
            }
        }
    }
    return false;
}

int Landscape::TerrainHoleDrawEdges(Vector3Par camPos, float radius, float* edges, int maxEdges) const
{
    // terrainHole2: the holes nearest the camera, as edge lines for the terrain pixel shaders
    // (terrainHole3: gathers up to 128 before keeping the nearest, was the first 32 found)
    static thread_local TerrainHoleArea holes[128];
    int n = GatherTerrainHoles(camPos, radius, holes, 128);
    if (n <= 0)
    {
        return 0;
    }
    int order[128];
    float dist2[128];
    for (int h = 0; h < n; h++)
    {
        order[h] = h;
        float dx = 0.5f * (holes[h].minX + holes[h].maxX) - camPos.X();
        float dz = 0.5f * (holes[h].minZ + holes[h].maxZ) - camPos.Z();
        dist2[h] = dx * dx + dz * dz;
    }
    std::sort(order, order + n, [&](int a, int b) { return dist2[a] < dist2[b]; });
    int nEdges = 0;
    for (int k = 0; k < n; k++)
    {
        const TerrainHoleArea& a = holes[order[k]];
        if (!a.draw)
        {
            continue; // terrainHole8: hidden hole, the terrain stays drawn
        }
        const bool bounded = a.ceilingY < 1e19f;
        if (nEdges + a.n + (bounded ? 1 : 0) > maxEdges)
        {
            continue; // a smaller, further hole may still fit
        }
        if (bounded)
        {
            float* header = edges + nEdges++ * 4;
            header[0] = header[1] = 0;
            header[2] = a.ceilingY;
            header[3] = 2; // metadata, not an edge; followed by ordinary 0/1 edges
        }
        for (int v = 0; v < a.n; v++)
        {
            int u = (v + 1) % a.n;
            float ex = a.x[u] - a.x[v], ez = a.z[u] - a.z[v];
            float len = std::sqrt(ex * ex + ez * ez);
            if (len < 1e-6f)
            {
                len = 1e-6f;
            }
            // inside (counter-clockwise) when cross(edge, p - a) >= 0: -ez * x + ex * z + (ez * ax - ex * az) >= 0
            float* e = edges + nEdges * 4;
            e[0] = -ez / len;
            e[1] = ex / len;
            e[2] = (ez * a.x[v] - ex * a.z[v]) / len;
            e[3] = v == a.n - 1 ? 1.0f : 0.0f;
            nEdges++;
        }
    }
    return nEdges;
}

// terrainHole4: the camera may go under the terrain inside a hole (a cellar, a stairwell)
float Landscape::CameraFloorY(Vector3& pos, const Vector3* focus, bool aboveWater) const
{
    auto groundAt = [&](float x, float z)
    { return aboveWater ? SurfaceYAboveWater(x, z) : SurfaceY(x, z, nullptr, nullptr, nullptr); };
    const float groundY = groundAt(pos.X(), pos.Z());
    if (pos.Y() >= groundY + 0.5f || GHoleShapeCount.load(std::memory_order_relaxed) == 0)
    {
        return groundY; // above the ground, or no hole anywhere
    }
    float floorY;
    if (InTerrainHole(pos.X(), pos.Z(), &floorY, pos.Y()))
    {
        return std::min(groundY, floorY);
    }
    // outside every hole: a camera following something in a hole is pulled back into the hole, toward its focus
    if (!focus || pos.Y() >= groundY || focus->Y() >= groundAt(focus->X(), focus->Z()) ||
        !InTerrainHole(focus->X(), focus->Z(), &floorY, focus->Y()))
    {
        return groundY;
    }
    const Vector3 f = *focus;
    const Vector3 dir = pos - f;
    float tIn = 0, tOut = 1;
    for (int i = 0; i < 12; i++)
    {
        const float t = 0.5f * (tIn + tOut);
        const Vector3 p = f + dir * t;
        if (InTerrainHole(p.X(), p.Z(), nullptr, p.Y()))
        {
            tIn = t;
        }
        else
        {
            tOut = t;
        }
    }
    pos = f + dir * tIn;
    float posFloorY = floorY;
    InTerrainHole(pos.X(), pos.Z(), &posFloorY, pos.Y()); // the hole the camera ended up in (the focus's, or one on the way)
    return std::min(groundAt(pos.X(), pos.Z()), posFloorY);
}

// terrainHole5: helpers for explosions, men and formations around cellars
bool Landscape::InTerrainHoleBelow(Vector3Par p, float depth) const
{
    if (GHoleShapeCount.load(std::memory_order_relaxed) == 0)
    {
        return false;
    }
    // the height test first: it is a terrain lookup, the hole test takes the registry lock (called per man per frame)
    return p.Y() < SurfaceY(p.X(), p.Z()) - depth && InTerrainHole(p.X(), p.Z(), nullptr, p.Y());
}

bool Landscape::LeavesTerrainHoleUnderground(Vector3Par from, Vector3Par to) const
{
    if (GHoleShapeCount.load(std::memory_order_relaxed) == 0)
    {
        return false;
    }
    if (std::fabs(from.X() - to.X()) + std::fabs(from.Z() - to.Z()) < 1e-5f)
    {
        return false; // no sideways movement
    }
    return InTerrainHoleBelow(from, 0.5f) && !InTerrainHole(to.X(), to.Z(), nullptr, to.Y()) && to.Y() < SurfaceY(to.X(), to.Z()) - 0.3f;
}

bool Landscape::CutsTerrainHole(const Object* obj)
{
    if (!obj || GHoleShapeCount.load(std::memory_order_relaxed) == 0)
    {
        return false;
    }
    const LODShape* shape = obj->GetShape();
    if (!shape || shape->FindRoadwayLevel() < 0 || shape->FindMemoryLevel() < 0)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(GHoleMutex);
    return !ShapeHolesOf(shape).empty();
}

// terrainHole6: ground decals in holes (footprints, tracks, craters, shadows used to be fitted to the terrain above)
float Landscape::DecalSurfaceY(float x, float z, float refY) const
{
    const float terrainY = SurfaceY(x, z);
    // 5 cm, not more: on the top steps of a stairwell (just under the terrain) a decal must sit on the step, not float
    // at the terrain height over it; a vertex on a roof at the terrain height finds that roof as its roadway anyway
    if (refY >= terrainY - 0.05f || GHoleShapeCount.load(std::memory_order_relaxed) == 0 || !InTerrainHole(x, z, nullptr, refY))
    {
        return terrainY; // the stock fit (on and above the ground, and everywhere outside holes)
    }
    return RoadSurfaceY(Vector3(x, refY + 0.5f, z));
}

float Landscape::TrackSurfaceY(Vector3Par p) const
{
    if (InTerrainHoleBelow(p, 0.3f))
    {
        return RoadSurfaceY(p + VUp * 0.5f);
    }
    return RoadSurfaceY(p.X(), p.Z());
}

// terrainHole7: the room of a hole (the cellar reverb); unlike "under the terrain" this also holds on a slope, where a
// cellar roof can stand above the ground on the downhill side (job ug8e: the listener 1.9 m over the floor was level
// with the terrain there)
bool Landscape::InTerrainHoleRoom(Vector3Par p) const
{
    if (GHoleShapeCount.load(std::memory_order_relaxed) == 0)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(GHoleMutex);
    for (size_t i = 0; i < GHoleObjects.size(); i++)
    {
        const Object* obj = GHoleObjects[i];
        if (!obj)
        {
            continue; // InTerrainHole prunes the dead links
        }
        const LODShape* shape = obj->GetShape();
        if (!shape)
        {
            continue;
        }
        float r = shape->BoundingSphere();
        float dx = obj->Position().X() - p.X(), dz = obj->Position().Z() - p.Z();
        if (dx * dx + dz * dz > r * r)
        {
            continue;
        }
        for (const ShapeHolePoly& poly : ShapeHolesOf(shape))
        {
            TerrainHoleArea area;
            if (!MakeHoleArea(obj, poly, area)) continue;
            if (TerrainHoleIndex(&area, 1, p.X(), p.Z(), p.Y()) != 0)
            {
                continue;
            }
            const float topY = obj->PositionModelToWorld(Vector3(0, shape->Max().Y(), 0)).Y();
            if (p.Y() >= area.floorY - 0.5f && p.Y() <= topY)
            {
                return true;
            }
        }
    }
    return false;
}

} // namespace Poseidon
