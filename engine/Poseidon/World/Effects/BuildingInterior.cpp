#include <Poseidon/World/Effects/BuildingInterior.hpp>
#include <Poseidon/World/Effects/InteriorShellRaster.hpp>
#include <Poseidon/World/Effects/InteriorFaceIndex.hpp>

#include <Poseidon/World/Entities/Vehicles/House.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

namespace Poseidon
{
namespace
{

constexpr int MaxRooms = 64;
constexpr int MaxGridDim = 192;
// Quarter-metre cells, not half: a 0.3 m wall sampled at its centre-line on a
// 0.5 m grid can be MISSED ENTIRELY, and one missed wall cell lets the room
// flood leak outdoors -- after which every point outside reads as "in the
// room" and containment silently stops mattering. That was the observed leak.
constexpr float DefaultCell = 0.25f;
constexpr unsigned char LabelSolid = 0xFE;
constexpr unsigned char LabelFree = 0xFF;

/// Fully-saturated hue to RGB, classic six-sector form. Room ids only; distinctness
/// matters, perceptual uniformity does not.
Color HueToRgb(float h)
{
    h = h - std::floor(h);
    const float r = std::fabs(h * 6.0f - 3.0f) - 1.0f;
    const float g = 2.0f - std::fabs(h * 6.0f - 2.0f);
    const float b = 2.0f - std::fabs(h * 6.0f - 4.0f);
    return Color(std::clamp(r, 0.0f, 1.0f), std::clamp(g, 0.0f, 1.0f), std::clamp(b, 0.0f, 1.0f), 1.0f);
}

struct InteriorCache
{
    std::vector<BuildingType const*> keys;
    std::vector<std::unique_ptr<BuildingInterior>> values;
};

InteriorCache& Cache()
{
    static InteriorCache cache;
    return cache;
}

// One boundary face between a room cell and open air. `axis`/`side` identify the
// plane, `c` the cell the face belongs to.
struct BoundaryFace
{
    int axis;
    int side;
    int c[3];
    Vector3 centre{VZero};
};

} // namespace

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

void BuildingInterior::BuildRoofColumns()
{
    _roofTop.assign(static_cast<size_t>(_dim[0]) * _dim[2], -1);
    for (int x = 0; x < _dim[0]; ++x)
        for (int z = 0; z < _dim[2]; ++z)
            for (int y = _dim[1] - 1; y >= 0; --y)
                if (_labels[CellIndex(x, y, z)] == LabelSolid)
                {
                    _roofTop[x * _dim[2] + z] = y;
                    break;
                }
}

bool BuildingInterior::CoveredFromAboveModel(Vector3Par pos) const
{
    if (!_valid || _roofTop.empty()) return false;
    const int x = static_cast<int>(std::floor((pos.X() - _gridMin.X()) / _cell));
    const int z = static_cast<int>(std::floor((pos.Z() - _gridMin.Z()) / _cell));
    if (x < 0 || z < 0 || x >= _dim[0] || z >= _dim[2]) return false;
    const int top = _roofTop[x * _dim[2] + z];
    return top >= 0 && pos.Y() < _gridMin.Y() + (top + 1) * _cell;
}

void BuildingInterior::Build(BuildingType const* type)
{
    _valid = false;
    _roomCount = 0;
    _portals.Clear();
    _labels.clear();
    _roofTop.clear();
    _dim[0] = _dim[1] = _dim[2] = 0;
    _cell = DefaultCell;

    if (type == nullptr)
    {
        return;
    }
    const auto buildStart = std::chrono::steady_clock::now();

    LODShapeWithShadow* shape = type->GetShape();
    if (shape == nullptr)
    {
        return;
    }

    // -- shell ---------------------------------------------------------------
    // Fire geometry is the solid shell; view geometry closes holes fire leaves
    // open (glass). The labelled union of both is what rooms must stay inside.
    const int fireLevel = shape->FindFireGeometryLevel();
    const int viewLevel = shape->FindViewGeometryLevel();
    ConvexComponents* fireCC = fireLevel >= 0 ? shape->GetConvexComponents(fireLevel) : nullptr;
    ConvexComponents* viewCC = viewLevel >= 0 ? shape->GetConvexComponents(viewLevel) : nullptr;
    if (fireCC == nullptr || fireCC->Size() <= 0)
    {
        return; // no usable shell -- callers fall back to reactive sweeps
    }

    Vector3 mins = fireCC->Get(0)->Min();
    Vector3 maxs = fireCC->Get(0)->Max();
    for (int i = 1; i < fireCC->Size(); i++)
    {
        mins = Vector3(std::min(mins.X(), fireCC->Get(i)->Min().X()), std::min(mins.Y(), fireCC->Get(i)->Min().Y()),
                       std::min(mins.Z(), fireCC->Get(i)->Min().Z()));
        maxs = Vector3(std::max(maxs.X(), fireCC->Get(i)->Max().X()), std::max(maxs.Y(), fireCC->Get(i)->Max().Y()),
                       std::max(maxs.Z(), fireCC->Get(i)->Max().Z()));
    }
    if (viewCC != nullptr)
    {
        for (int i = 0; i < viewCC->Size(); i++)
        {
            mins = Vector3(std::min(mins.X(), viewCC->Get(i)->Min().X()), std::min(mins.Y(), viewCC->Get(i)->Min().Y()),
                           std::min(mins.Z(), viewCC->Get(i)->Min().Z()));
            maxs = Vector3(std::max(maxs.X(), viewCC->Get(i)->Max().X()), std::max(maxs.Y(), viewCC->Get(i)->Max().Y()),
                           std::max(maxs.Z(), viewCC->Get(i)->Max().Z()));
        }
    }
    mins = mins - Vector3(1, 1, 1);
    maxs = maxs + Vector3(1, 1, 1);

    // Coarsen instead of clipping when the bbox does not fit the grid budget.
    float cell = DefaultCell;
    for (int a = 0; a < 3; a++)
    {
        const float size = maxs[a] - mins[a];
        if (size / cell > MaxGridDim)
        {
            cell = std::max(cell, size / MaxGridDim);
        }
    }
    int dim[3];
    for (int a = 0; a < 3; a++)
    {
        const float size = maxs[a] - mins[a];
        dim[a] = std::max(1, static_cast<int>(std::ceil(size / cell)));
    }

    _gridMin = mins;
    _cell = cell;
    _dim[0] = dim[0];
    _dim[1] = dim[1];
    _dim[2] = dim[2];

    // -- voxelize ------------------------------------------------------------
    const long long totalCells = static_cast<long long>(_dim[0]) * _dim[1] * _dim[2];
    _labels.assign(static_cast<size_t>(totalCells), LabelFree);
    const auto rasterStart = std::chrono::steady_clock::now();

    static const bool componentRaster = []
    {
        const char* value = std::getenv("POSEIDON_INTERIOR_COMPONENT_RASTER");
        return !(value && value[0] == '0');
    }();
    if (componentRaster)
    {
        for (ConvexComponents* cc : {fireCC, viewCC})
        {
            if (!cc)
                continue;
            for (int i = 0; i < cc->Size(); ++i)
            {
                const ConvexComponent* comp = cc->Get(i);
                RasterizeInteriorShellComponent(_labels, _dim, _gridMin, _cell, comp->Min(), comp->Max(),
                                                [comp](Vector3Par p) { return comp->IsInside(p); });
            }
        }
    }
    else
    {
        // A cell is solid if ANY of seven samples (centre + the six face centres)
        // lands inside shell geometry. Centre-only sampling is what let thin walls
        // slip between cells; face-centre sampling catches anything thicker than
        // about half a cell even when it straddles a boundary.
        const Vector3 half(_cell * 0.5f, _cell * 0.5f, _cell * 0.5f);
        auto pointSolid = [&](Vector3Par p) -> bool
        {
            for (int pass = 0; pass < 2; pass++)
            {
                ConvexComponents* cc = pass == 0 ? fireCC : viewCC;
                if (cc == nullptr)
                {
                    continue;
                }
                for (int i = 0; i < cc->Size(); i++)
                {
                    const ConvexComponent* comp = cc->Get(i);
                    // cheap bbox pre-check before the plane test
                    if (p.X() < comp->Min().X() || p.X() > comp->Max().X() || p.Y() < comp->Min().Y() ||
                        p.Y() > comp->Max().Y() || p.Z() < comp->Min().Z() || p.Z() > comp->Max().Z())
                    {
                        continue;
                    }
                    if (comp->IsInside(p))
                    {
                        return true;
                    }
                }
            }
            return false;
        };
        for (int x = 0; x < _dim[0]; x++)
        {
            for (int y = 0; y < _dim[1]; y++)
            {
                for (int z = 0; z < _dim[2]; z++)
                {
                    const Vector3 p = _gridMin + Vector3((x + 0.5f) * _cell, (y + 0.5f) * _cell, (z + 0.5f) * _cell);
                    const Vector3 samples[7] = {p,
                                                p + Vector3(half.X(), 0, 0),
                                                p - Vector3(half.X(), 0, 0),
                                                p + Vector3(0, half.Y(), 0),
                                                p - Vector3(0, half.Y(), 0),
                                                p + Vector3(0, 0, half.Z()),
                                                p - Vector3(0, 0, half.Z())};
                    bool solid = false;
                    for (const Vector3& s : samples)
                    {
                        if (pointSolid(s))
                        {
                            solid = true;
                            break;
                        }
                    }
                    if (solid)
                    {
                        _labels[CellIndex(x, y, z)] = LabelSolid;
                    }
                }
            }
        }
    }

    // -- flood clamp ---------------------------------------------------------
    if (const char* trace = std::getenv("POSEIDON_INTERIOR_RASTER_TRACE"); trace && trace[0] == '1')
    {
        const float ms =
            std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - rasterStart).count();
        uint64_t hash = 14695981039346656037ull;
        for (unsigned char label : _labels)
        {
            hash ^= label;
            hash *= 1099511628211ull;
        }
        LOG_INFO(World, "Interior raster '{}': component={} ms={:.3f} shellHash={}", (const char*)type->GetName(),
                 componentRaster, ms, hash);
    }
    // Room labels may never escape the FIRE geometry's own bounding box: outside
    // it is definitionally outdoors, and a residual sampling gap in an exterior
    // wall must not turn the street into part of the living room.
    Vector3 fireMin = fireCC->Get(0)->Min();
    Vector3 fireMax = fireCC->Get(0)->Max();
    for (int i = 1; i < fireCC->Size(); i++)
    {
        fireMin = Vector3(std::min(fireMin.X(), fireCC->Get(i)->Min().X()),
                          std::min(fireMin.Y(), fireCC->Get(i)->Min().Y()),
                          std::min(fireMin.Z(), fireCC->Get(i)->Min().Z()));
        fireMax = Vector3(std::max(fireMax.X(), fireCC->Get(i)->Max().X()),
                          std::max(fireMax.Y(), fireCC->Get(i)->Max().Y()),
                          std::max(fireMax.Z(), fireCC->Get(i)->Max().Z()));
    }
    const Vector3 clampMin = fireMin - Vector3(_cell, _cell, _cell);
    const Vector3 clampMax = fireMax + Vector3(_cell, _cell, _cell);

    // -- rooms from the Paths graph ------------------------------------------
    // Nodes are Paths "point" indices; exits (In nodes) are removed and the
    // connected components of what remains are the rooms.
    const int nPoints = type->_connections.Size();
    std::vector<char> isExit(nPoints, 0);
    for (int i = 0; i < type->_exits.Size(); i++)
    {
        const int e = type->_exits[i];
        if (e >= 0 && e < nPoints)
        {
            isExit[e] = 1;
        }
    }
    std::vector<int> roomOfNode(nPoints, -1);
    std::vector<std::vector<int>> roomNodes;
    bool warnedTooManyRooms = false;
    for (int start = 0; start < nPoints; start++)
    {
        if (isExit[start] || roomOfNode[start] >= 0)
        {
            continue;
        }
        int roomId = static_cast<int>(roomNodes.size());
        if (roomId >= MaxRooms)
        {
            // gappy or degenerate graph: fold the excess into room 0 rather than fail
            roomId = 0;
            if (!warnedTooManyRooms)
            {
                LOG_WARN(World, "BuildingInterior: more than {} rooms, excess merged into room 0", MaxRooms);
                warnedTooManyRooms = true;
            }
        }
        if (roomId == static_cast<int>(roomNodes.size()))
        {
            roomNodes.emplace_back();
        }
        // flood this component
        std::vector<int> stack;
        stack.push_back(start);
        roomOfNode[start] = roomId;
        while (!stack.empty())
        {
            const int node = stack.back();
            stack.pop_back();
            roomNodes[roomId].push_back(node);
            const FindArray<int>& adj = type->_connections[node];
            for (int i = 0; i < adj.Size(); i++)
            {
                const int nb = adj[i];
                if (nb < 0 || nb >= nPoints || isExit[nb] || roomOfNode[nb] >= 0)
                {
                    continue;
                }
                roomOfNode[nb] = roomId;
                stack.push_back(nb);
            }
        }
    }
    _roomCount = static_cast<int>(roomNodes.size());

    // -- flood fill the grid -------------------------------------------------
    // Multi-source BFS carrying room ids, first-come-wins. Seeds are the room
    // nodes' positions; a seed landing inside a wall searches its 3x3x3
    // neighbourhood for the nearest free cell.
    std::vector<std::pair<int, int>> queue; // cell index, room id
    auto seedAt = [&](Vector3Par pos, int roomId)
    {
        int cx = static_cast<int>(std::floor((pos.X() - _gridMin.X()) / _cell));
        int cy = static_cast<int>(std::floor((pos.Y() - _gridMin.Y()) / _cell));
        int cz = static_cast<int>(std::floor((pos.Z() - _gridMin.Z()) / _cell));
        if (!CellSolid(cx, cy, cz))
        {
            queue.emplace_back(CellIndex(cx, cy, cz), roomId);
            return;
        }
        int best = -1;
        float bestDist2 = FLT_MAX;
        for (int dx = -1; dx <= 1; dx++)
        {
            for (int dy = -1; dy <= 1; dy++)
            {
                for (int dz = -1; dz <= 1; dz++)
                {
                    const int nx = cx + dx, ny = cy + dy, nz = cz + dz;
                    if (CellSolid(nx, ny, nz))
                    {
                        continue;
                    }
                    const Vector3 centre =
                        _gridMin + Vector3((nx + 0.5f) * _cell, (ny + 0.5f) * _cell, (nz + 0.5f) * _cell);
                    const float d2 = centre.Distance2(pos);
                    if (d2 < bestDist2)
                    {
                        bestDist2 = d2;
                        best = CellIndex(nx, ny, nz);
                    }
                }
            }
        }
        if (best >= 0)
        {
            queue.emplace_back(best, roomId);
        }
    };
    for (int r = 0; r < static_cast<int>(roomNodes.size()); r++)
    {
        for (int node : roomNodes[r])
        {
            seedAt(type->GetPosition(node), r);
        }
    }
    // BFS through free air; solid cells block, claimed cells are skipped.
    for (size_t head = 0; head < queue.size(); head++)
    {
        const int cellIdx = queue[head].first;
        const int roomId = queue[head].second;
        if (_labels[cellIdx] != LabelFree)
        {
            continue;
        }
        _labels[cellIdx] = static_cast<unsigned char>(roomId);
        const int x = cellIdx / (_dim[1] * _dim[2]);
        const int y = (cellIdx / _dim[2]) % _dim[1];
        const int z = cellIdx % _dim[2];
        const int nb[6][3] = {{x - 1, y, z}, {x + 1, y, z}, {x, y - 1, z}, {x, y + 1, z}, {x, y, z - 1}, {x, y, z + 1}};
        for (const auto& n : nb)
        {
            if (n[0] < 0 || n[1] < 0 || n[2] < 0 || n[0] >= _dim[0] || n[1] >= _dim[1] || n[2] >= _dim[2])
            {
                continue;
            }
            // flood clamp: never claim cells outside the fire shell's bbox
            const Vector3 centre = _gridMin + Vector3((n[0] + 0.5f) * _cell, (n[1] + 0.5f) * _cell,
                                                      (n[2] + 0.5f) * _cell);
            if (centre.X() < clampMin.X() || centre.X() > clampMax.X() || centre.Y() < clampMin.Y() ||
                centre.Y() > clampMax.Y() || centre.Z() < clampMin.Z() || centre.Z() > clampMax.Z())
            {
                continue;
            }
            const int ni = CellIndex(n[0], n[1], n[2]);
            if (_labels[ni] == LabelFree)
            {
                queue.emplace_back(ni, roomId);
            }
        }
    }

    // the grid is now a valid (if portal-less) room lookup; RoomOfModel is used
    // below while clustering portals, so validity starts here
    _valid = true;
    BuildRoofColumns();

    // -- portals from room/open-air boundaries --------------------------------
    std::vector<std::vector<BoundaryFace>> faces(_roomCount);
    for (int x = 0; x < _dim[0]; x++)
    {
        for (int y = 0; y < _dim[1]; y++)
        {
            for (int z = 0; z < _dim[2]; z++)
            {
                const int label = CellLabel(x, y, z);
                if (label < 0)
                {
                    continue;
                }
                const int nb[6][3] = {{x - 1, y, z},   {x + 1, y, z}, {x, y - 1, z},
                                      {x, y + 1, z},   {x, y, z - 1}, {x, y, z + 1}};
                for (int f = 0; f < 6; f++)
                {
                    const int nx = nb[f][0], ny = nb[f][1], nz = nb[f][2];
                    const bool outsideGrid = nx < 0 || ny < 0 || nz < 0 || nx >= _dim[0] || ny >= _dim[1] ||
                                             nz >= _dim[2];
                    const unsigned char neighbour = outsideGrid ? LabelFree : _labels[CellIndex(nx, ny, nz)];
                    if (neighbour != LabelFree)
                    {
                        // A solid neighbour is a wall/roof, not a portal. The
                        // previous CellLabel test collapsed both solid and open
                        // air to -1 and consequently promoted whole walls into
                        // enormous synthetic windows/breaches.
                        continue;
                    }
                    BoundaryFace face;
                    face.axis = f / 2;
                    face.side = (f % 2) == 0 ? -1 : 1;
                    face.c[0] = x;
                    face.c[1] = y;
                    face.c[2] = z;
                    float c[3] = {(x + 0.5f) * _cell, (y + 0.5f) * _cell, (z + 0.5f) * _cell};
                    c[face.axis] = (face.c[face.axis] + (face.side > 0 ? 1.0f : 0.0f)) * _cell;
                    face.centre = _gridMin + Vector3(c[0], c[1], c[2]);
                    faces[label].push_back(face);
                }
            }
        }
    }

    std::vector<char> exitCovered(type->_exits.Size(), 0);
    static const bool indexedFaces = []
    {
        const char* value = std::getenv("POSEIDON_INTERIOR_FACE_INDEX");
        return !(value && value[0] == '0');
    }();
    for (int r = 0; r < _roomCount; r++)
    {
        // cluster coplanar edge-adjacent faces (BFS over face adjacency)
        const int n = static_cast<int>(faces[r].size());
        std::unique_ptr<InteriorFaceIndex> faceIndex;
        if (indexedFaces)
            faceIndex = std::make_unique<InteriorFaceIndex>(faces[r], _dim);
        std::vector<char> visited(n, 0);
        for (int start = 0; start < n; start++)
        {
            if (visited[start])
            {
                continue;
            }
            std::vector<int> cluster;
            cluster.push_back(start);
            visited[start] = 1;
            for (size_t head = 0; head < cluster.size(); head++)
            {
                const BoundaryFace& a = faces[r][cluster[head]];
                if (indexedFaces)
                {
                    for (int i : faceIndex->Neighbours(a))
                        if (i >= 0 && !visited[i])
                        {
                            visited[i] = 1;
                            cluster.push_back(i);
                        }
                    continue;
                }
                for (int i = 0; i < n; i++)
                {
                    if (visited[i])
                    {
                        continue;
                    }
                    const BoundaryFace& b = faces[r][i];
                    if (b.axis != a.axis || b.side != a.side || b.c[b.axis] != a.c[a.axis])
                    {
                        continue;
                    }
                    const int t1 = (a.axis + 1) % 3, t2 = (a.axis + 2) % 3;
                    const int d1 = std::abs(b.c[t1] - a.c[t1]);
                    const int d2 = std::abs(b.c[t2] - a.c[t2]);
                    if (d1 + d2 == 1)
                    {
                        visited[i] = 1;
                        cluster.push_back(i);
                    }
                }
            }

            // cluster -> portal
            Vector3 centre{VZero};
            float tmin[2] = {FLT_MAX, FLT_MAX};
            float tmax[2] = {-FLT_MAX, -FLT_MAX};
            const int t1 = (faces[r][start].axis + 1) % 3, t2 = (faces[r][start].axis + 2) % 3;
            for (int ci : cluster)
            {
                centre += faces[r][ci].centre;
                const float ct1 = faces[r][ci].centre[t1];
                const float ct2 = faces[r][ci].centre[t2];
                tmin[0] = std::min(tmin[0], ct1);
                tmax[0] = std::max(tmax[0], ct1);
                tmin[1] = std::min(tmin[1], ct2);
                tmax[1] = std::max(tmax[1], ct2);
            }
            centre = centre * (1.0f / cluster.size());
            const float extent = std::max(tmax[0] - tmin[0], tmax[1] - tmin[1]) + _cell;
            const float radius = extent * 0.5f;

            Vector3 normal{VZero};
            normal[faces[r][start].axis] = static_cast<float>(faces[r][start].side);

            InteriorPortal portal;
            portal.centre = centre;
            portal.normal = normal;
            portal.radius = radius;
            portal.room = r;

            // sample one cell beyond the portal to find the room on the far side
            const Vector3 beyond = centre + normal * (_cell * 1.5f);
            const int beyondLabel = RoomOfModel(beyond);
            portal.otherRoom = beyondLabel;

            bool isDoor = false;
            for (int e = 0; e < type->_exits.Size(); e++)
            {
                const Vector3 exitPos = type->GetPosition(type->_exits[e]);
                if (exitPos.Distance(centre) <= std::max(1.5f, radius))
                {
                    isDoor = true;
                    exitCovered[e] = 1;
                }
            }
            portal.kind = isDoor ? PortalDoor : (extent > 4.0f ? PortalBreach : PortalWindow);
            _portals.Add(portal);
        }
    }

    // every In node must be served even if no clustered face matched it
    for (int e = 0; e < type->_exits.Size(); e++)
    {
        if (exitCovered[e])
        {
            continue;
        }
        const Vector3 exitPos = type->GetPosition(type->_exits[e]);
        float nearest = FLT_MAX;
        for (int p = 0; p < _portals.Size(); p++)
        {
            nearest = std::min(nearest, _portals[p].centre.Distance(exitPos));
        }
        if (nearest <= 2.0f)
        {
            continue;
        }
        // serve it from the room whose nodes sit closest to the exit
        int bestRoom = -1;
        float bestDist2 = FLT_MAX;
        for (int r = 0; r < static_cast<int>(roomNodes.size()); r++)
        {
            for (int node : roomNodes[r])
            {
                const float d2 = type->GetPosition(node).Distance2(exitPos);
                if (d2 < bestDist2)
                {
                    bestDist2 = d2;
                    bestRoom = r;
                }
            }
        }
        if (bestRoom < 0)
        {
            continue;
        }
        Vector3 centroid{VZero};
        for (int node : roomNodes[bestRoom])
        {
            centroid += type->GetPosition(node);
        }
        centroid = centroid * (1.0f / roomNodes[bestRoom].size());

        InteriorPortal portal;
        portal.centre = exitPos;
        portal.room = bestRoom;
        portal.radius = 1.0f;
        Vector3 dir = exitPos - centroid;
        dir[1] = 0.0f; // horizontal-only
        if (dir.SquareSize() > 1e-6f)
        {
            portal.normal = dir.Normalized();
        }
        else
        {
            portal.normal = Vector3(0, 0, 1);
        }
        const Vector3 beyond = portal.centre + portal.normal * (_cell * 1.5f);
        portal.otherRoom = RoomOfModel(beyond);
        portal.kind = PortalDoor;
        _portals.Add(portal);
    }

    // -- QA: count boundary faces to open air not covered by any portal -------
    long long uncoveredLeakFaces = 0;
    for (int x = 0; x < _dim[0]; x++)
    {
        for (int y = 0; y < _dim[1]; y++)
        {
            for (int z = 0; z < _dim[2]; z++)
            {
                const int label = CellLabel(x, y, z);
                if (label < 0)
                {
                    continue;
                }
                const int nb[6][3] = {{x - 1, y, z}, {x + 1, y, z}, {x, y - 1, z},
                                      {x, y + 1, z}, {x, y, z - 1}, {x, y, z + 1}};
                for (int f = 0; f < 6; f++)
                {
                    if (CellLabel(nb[f][0], nb[f][1], nb[f][2]) >= 0)
                    {
                        continue;
                    }
                    const int cc[3] = {x, y, z};
                    Vector3 fc =
                        _gridMin + Vector3((x + 0.5f) * _cell, (y + 0.5f) * _cell, (z + 0.5f) * _cell);
                    fc[f / 2] = (cc[f / 2] + ((f % 2) == 0 ? 0.0f : 1.0f)) * _cell;
                    bool covered = false;
                    for (int p = 0; p < _portals.Size() && !covered; p++)
                    {
                        if (_portals[p].centre.Distance(fc) <= _portals[p].radius + _cell)
                        {
                            covered = true;
                        }
                    }
                    if (!covered)
                    {
                        uncoveredLeakFaces++;
                    }
                }
            }
        }
    }

    int doors = 0, windows = 0, breaches = 0;
    for (int p = 0; p < _portals.Size(); p++)
    {
        switch (_portals[p].kind)
        {
            case PortalDoor: doors++; break;
            case PortalWindow: windows++; break;
            case PortalBreach: breaches++; break;
        }
    }

    if (const char* trace = std::getenv("POSEIDON_INTERIOR_RASTER_TRACE"); trace && trace[0] == '1')
    {
        uint64_t hash = 14695981039346656037ull;
        const auto fold = [&](auto value)
        {
            const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
            for (size_t i = 0; i < sizeof(value); ++i)
            {
                hash ^= bytes[i];
                hash *= 1099511628211ull;
            }
        };
        for (unsigned char label : _labels)
            fold(label);
        fold(_roomCount);
        for (int i = 0; i < _portals.Size(); ++i)
        {
            const auto& portal = _portals[i];
            for (int a = 0; a < 3; ++a)
            {
                fold(portal.centre[a]);
                fold(portal.normal[a]);
            }
            fold(portal.radius);
            fold(portal.room);
            fold(portal.otherRoom);
            fold(static_cast<int>(portal.kind));
        }
        LOG_INFO(World, "Interior topology '{}': indexed={} hash={}", (const char*)type->GetName(), indexedFaces, hash);
    }
    const float buildMs =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - buildStart).count();
    LOG_INFO(World,
             "BuildingInterior '{}': rooms={} portals={} (doors={} windows={} breaches={}) grid={}x{}x{} "
             "cell={:.2f}m leakFaces={} build={:.1f}ms",
             (const char*)type->GetName(), _roomCount, _portals.Size(), doors, windows, breaches, _dim[0], _dim[1],
             _dim[2], _cell, uncoveredLeakFaces, buildMs);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

int BuildingInterior::RoomOfModel(Vector3Par pos) const
{
    if (!_valid)
    {
        return -1;
    }
    const int cx = static_cast<int>(std::floor((pos.X() - _gridMin.X()) / _cell));
    const int cy = static_cast<int>(std::floor((pos.Y() - _gridMin.Y()) / _cell));
    const int cz = static_cast<int>(std::floor((pos.Z() - _gridMin.Z()) / _cell));
    return CellLabel(cx, cy, cz);
}

int BuildingInterior::ClassifyModel(Vector3Par pos) const
{
    if (!_valid)
    {
        return -1;
    }
    const int cx = static_cast<int>(std::floor((pos.X() - _gridMin.X()) / _cell));
    const int cy = static_cast<int>(std::floor((pos.Y() - _gridMin.Y()) / _cell));
    const int cz = static_cast<int>(std::floor((pos.Z() - _gridMin.Z()) / _cell));
    if (cx < 0 || cy < 0 || cz < 0 || cx >= _dim[0] || cy >= _dim[1] || cz >= _dim[2])
    {
        return -1;
    }
    const unsigned char label = _labels[CellIndex(cx, cy, cz)];
    if (label == LabelSolid)
    {
        return -2;
    }
    return label == LabelFree ? -1 : static_cast<int>(label);
}

int BuildingInterior::ClassifySegmentModel(Vector3Par from, Vector3Par to) const
{
    if (!_valid)
    {
        return -1;
    }
    const Vector3 delta = to - from;
    const float distance = delta.Size();
    const int steps = std::clamp(static_cast<int>(std::ceil(distance / std::max(_cell * 0.5f, 0.01f))), 1, 256);
    for (int i = 0; i <= steps; ++i)
    {
        const int label = ClassifyModel(from + delta * (static_cast<float>(i) / steps));
        if (label != -1)
        {
            return label;
        }
    }
    return -1;
}

bool BuildingInterior::SegmentPassesPortalModel(Vector3Par from, Vector3Par to,
                                               const InteriorPortalFilter &filter) const
{
    if (!_valid)
    {
        return false;
    }
    // Portals are few and this is only asked when a segment actually crossed
    // the shell or a room boundary, so the linear scan is the right answer.
    //
    // WHICH openings forgive is the caller's call now (InteriorPortalFilter),
    // because it is a tolerance, not a fact. The default -- authored doors
    // only, at three quarters of the inferred radius -- is what rain wants:
    // windows come from a heuristic voxel scan whose radii are not yet
    // trustworthy (an owner smoke showed streaks raining down inside a room,
    // traced to generous window discs forgiving whole wall sections) and a
    // BREACH marks geometry that failed to close outright. Until the
    // Fire-vs-View window scan replaces the heuristic (handover phase 3), a
    // door is the only opening rain may legitimately fall through, and the
    // radius scale is what stands between "rain through the door" and "rain
    // through the wall next to the door".
    const float scale = std::max(filter.radiusScale, 0.0f);
    if (scale <= 0.0f)
    {
        return false;
    }
    const Vector3 delta = to - from;
    const float len2 = delta.SquareSize();
    for (int i = 0; i < _portals.Size(); ++i)
    {
        const InteriorPortal& portal = _portals[i];
        if (!filter.Accepts(portal.kind))
        {
            continue;
        }
        float t = 0.5f;
        if (len2 > 1e-8f)
        {
            t = std::clamp((portal.centre - from).DotProduct(delta) / len2, 0.0f, 1.0f);
        }
        const Vector3 closest = from + delta * t;
        if ((closest - portal.centre).SquareSize() <= Square(portal.radius * scale))
        {
            return true;
        }
    }
    return false;
}

float BuildingInterior::ClearanceModel(Vector3Par pos, int roomId, float maxDistance) const
{
    if (!_valid || roomId < 0 || roomId >= _roomCount || RoomOfModel(pos) != roomId || maxDistance <= 0.0f)
    {
        return 0.0f;
    }

    // 26 uniformly distributed lattice directions (axes, face diagonals and
    // body diagonals). This is substantially cheaper than searching a cube of
    // voxels per particle while still seeing ceilings, sloped roofs and room
    // corners. Half-cell stepping cannot skip a one-cell shell. The final 0.85
    // makes the finite direction set conservative between sampled directions.
    float nearest = maxDistance;
    const float stride = std::max(_cell * 0.5f, 0.01f);
    for (int dx = -1; dx <= 1; ++dx)
    {
        for (int dy = -1; dy <= 1; ++dy)
        {
            for (int dz = -1; dz <= 1; ++dz)
            {
                if (dx == 0 && dy == 0 && dz == 0)
                {
                    continue;
                }
                Vector3 direction(static_cast<float>(dx), static_cast<float>(dy), static_cast<float>(dz));
                direction = direction.Normalized();
                float previous = 0.0f;
                for (float distance = stride; distance <= nearest + stride; distance += stride)
                {
                    if (RoomOfModel(pos + direction * distance) == roomId)
                    {
                        previous = distance;
                        continue;
                    }

                    // Refine the first inside/outside bracket. Four iterations
                    // locate the voxel boundary to about 8 mm at 0.25 m cells.
                    float inside = previous;
                    float outside = distance;
                    for (int iteration = 0; iteration < 4; ++iteration)
                    {
                        const float middle = (inside + outside) * 0.5f;
                        if (RoomOfModel(pos + direction * middle) == roomId)
                        {
                            inside = middle;
                        }
                        else
                        {
                            outside = middle;
                        }
                    }
                    nearest = std::min(nearest, inside);
                    break;
                }
            }
        }
    }
    return std::max(nearest * 0.85f, 0.0f);
}

InteriorStepResult BuildingInterior::ContainedStepModel(Vector3Par from, Vector3Par to, int roomId) const
{
    InteriorStepResult result;
    result.blocked = false;
    result.position = to;
    result.room = RoomOfModel(to);
    if (!_valid || roomId < 0 || roomId >= _roomCount)
    {
        return result;
    }
    // stale room id: re-resolve instead of trapping the particle in a room it
    // is not actually in
    if (RoomOfModel(from) != roomId)
    {
        return result;
    }

    const Vector3 delta = to - from;
    const float dist = delta.Size();
    if (dist < 1e-6f)
    {
        result.room = roomId;
        return result;
    }
    int steps = std::clamp(static_cast<int>(std::ceil(dist / (_cell * 0.5f))), 1, 256);
    const Vector3 stepVec = delta * (1.0f / steps);

    Vector3 lastIn = from;
    for (int i = 1; i <= steps; i++)
    {
        const Vector3 p = from + stepVec * i;
        const int label = RoomOfModel(p);
        if (label == roomId)
        {
            lastIn = p;
            continue;
        }
        // first transition out of the room: may only happen through a portal
        const Vector3 crossPoint = (lastIn + p) * 0.5f;
        Vector3 dir = p - lastIn;
        dir = dir.SquareSize() > 1e-8f ? dir.Normalized() : stepVec.Normalized();
        bool passage = false;
        for (int pi = 0; pi < _portals.Size() && !passage; pi++)
        {
            const InteriorPortal& portal = _portals[pi];
            if (portal.room != roomId)
            {
                continue;
            }
            // Strictly the portal's own radius -- no slack factor. A crossing a
            // metre from the door frame is a WALL, and passing it is exactly the
            // "goes through walls" complaint; 1.4x used to allow that.
            if (portal.centre.Distance(crossPoint) <= portal.radius && dir.DotProduct(portal.normal) > 0.3f)
            {
                passage = true;
            }
        }
        if (passage)
        {
            // result room is whatever the first out-sample is in (or -1 outside)
            result.blocked = false;
            result.position = p;
            result.room = label;
            return result;
        }
        result.blocked = true;
        result.position = lastIn;
        result.room = roomId;
        // dominant-axis outward normal of the boundary that was about to be crossed
        const Vector3 outDir = p - lastIn;
        const float ax[3] = {std::fabs(outDir.X()), std::fabs(outDir.Y()), std::fabs(outDir.Z())};
        int axis = 0;
        if (ax[1] > ax[axis])
        {
            axis = 1;
        }
        if (ax[2] > ax[axis])
        {
            axis = 2;
        }
        result.normal = VZero;
        result.normal[axis] = outDir[axis] >= 0 ? 1.0f : -1.0f;
        return result;
    }
    result.blocked = false;
    result.position = to;
    result.room = roomId;
    return result;
}

// ---------------------------------------------------------------------------
// World-space wrappers
// ---------------------------------------------------------------------------

int BuildingInterior::RoomOfWorld(Object const* obj, Vector3Par pos) const
{
    if (obj == nullptr)
    {
        return -1;
    }
    return RoomOfModel(obj->GetInvTransform().FastTransform(pos));
}

InteriorStepResult BuildingInterior::ContainedStepWorld(Object const* obj, Vector3Par from, Vector3Par to,
                                                        int roomId) const
{
    if (obj == nullptr)
    {
        InteriorStepResult result;
        result.position = to;
        result.room = -1;
        return result;
    }
    const Matrix4 inv = obj->GetInvTransform();
    InteriorStepResult result = ContainedStepModel(inv.FastTransform(from), inv.FastTransform(to), roomId);
    result.position = obj->PositionModelToWorld(result.position);
    return result;
}

// ---------------------------------------------------------------------------
// Debug draw
// ---------------------------------------------------------------------------

void BuildingInterior::DebugDraw(Object const* obj) const
{
    if (GScene == nullptr || obj == nullptr || !_valid)
    {
        return;
    }
    LODShapeWithShadow* shape = GScene->Preloaded(SphereModel);
    if (shape == nullptr)
    {
        return;
    }

    // room cells: stride so the whole grid is sampled at roughly 200 points per room
    const long long totalCells = static_cast<long long>(_dim[0]) * _dim[1] * _dim[2];
    const long long stride =
        std::max<long long>(1, totalCells / (200LL * std::max(1, _roomCount)));
    long long index = 0;
    for (int x = 0; x < _dim[0]; x++)
    {
        for (int y = 0; y < _dim[1]; y++)
        {
            for (int z = 0; z < _dim[2]; z++, index++)
            {
                if (index % stride != 0)
                {
                    continue;
                }
                const unsigned char label = _labels[CellIndex(x, y, z)];
                if (label >= LabelSolid)
                {
                    continue;
                }
                const Vector3 modelPos =
                    _gridMin + Vector3((x + 0.5f) * _cell, (y + 0.5f) * _cell, (z + 0.5f) * _cell);
                Ref<Object> marker = new ObjectColored(shape, -1);
                marker->SetPosition(obj->PositionModelToWorld(modelPos));
                marker->SetScale(0.12);
                marker->SetConstantColor(PackedColor(HueToRgb(label * 0.618034f)));
                GScene->ObjectForDrawing(marker);
            }
        }
    }

    for (int p = 0; p < _portals.Size(); p++)
    {
        const InteriorPortal& portal = _portals[p];
        const Color color = portal.kind == PortalDoor    ? Color(0, 1, 0, 1)
                            : portal.kind == PortalWindow ? Color(0, 1, 1, 1)
                                                          : Color(1, 0.2f, 0.2f, 1);
        Ref<Object> marker = new ObjectColored(shape, -1);
        marker->SetPosition(obj->PositionModelToWorld(portal.centre));
        marker->SetScale(std::clamp(portal.radius, 0.3f, 1.5f));
        marker->SetConstantColor(PackedColor(color));
        GScene->ObjectForDrawing(marker);
    }
}

// ---------------------------------------------------------------------------
// Cache
// ---------------------------------------------------------------------------

BuildingInterior const* BuildingInterior::GetFor(BuildingType const* type)
{
    if (type == nullptr)
    {
        return nullptr;
    }
    InteriorCache& cache = Cache();
    for (size_t i = 0; i < cache.keys.size(); i++)
    {
        if (cache.keys[i] == type)
        {
            return cache.values[i].get();
        }
    }
    auto interior = std::make_unique<BuildingInterior>();
    interior->Build(type); // logs its own build line
    BuildingInterior* raw = interior.get();
    cache.keys.push_back(type);
    cache.values.push_back(std::move(interior));
    return raw;
}

void BuildingInterior::ClearCache()
{
    InteriorCache& cache = Cache();
    cache.keys.clear();
    cache.values.clear();
}

// ---------------------------------------------------------------------------
// Nearby-building overlay entry point
// ---------------------------------------------------------------------------

void DebugDrawInteriorNear(Vector3Par pos)
{
    if (GLandscape == nullptr)
    {
        return;
    }
    StaticArrayAuto<OLink<Object>> objects;
    GLandscape->IsInside(objects, nullptr, pos, ObjIntersectFire);
    if (objects.Size() <= 0)
    {
        GLandscape->IsInside(objects, nullptr, pos, ObjIntersectView);
    }
    for (int i = 0; i < objects.Size(); i++)
    {
        Building* building = dyn_cast<Building>(objects[i].GetLink());
        if (building == nullptr)
        {
            continue;
        }
        BuildingInterior const* interior = BuildingInterior::GetFor(building->GetBType());
        if (interior != nullptr)
        {
            interior->DebugDraw(building);
        }
        break; // first containing building wins
    }
}

} // namespace Poseidon
