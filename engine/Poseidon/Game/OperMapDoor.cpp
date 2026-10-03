
#include <Poseidon/Core/Application.hpp>

#include <Poseidon/AI/AI.hpp>
#include <Poseidon/AI/Path/AIDefs.hpp>
#include <Poseidon/Game/OperMap.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Dev/Debug/DebugWin.hpp>
#include <limits.h>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Containers/BoolArray.hpp>
#include <Poseidon/Foundation/Containers/StaticArray.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/LLinks.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>

using namespace Poseidon;
namespace Poseidon
{
} // namespace Poseidon

using namespace Poseidon::Dev;
#include <Poseidon/World/Entities/Vehicles/House.hpp>

#include <Poseidon/Dev/Diag/DiagModes.hpp>

using Poseidon::Foundation::MemAllocD;
using Poseidon::Foundation::MemAllocSS;
using Poseidon::Foundation::MStorage;

#define USE_NEW_ASTAR 1
OperDoor* OperMap::FindDoor(int x, int z)
{
    for (int d = 0; d < _doors.Size(); d++)
    {
        OperDoor& door = _doors[d];
        if (door.x == x && door.z == z && door.house)
        {
            return &door;
        }
    }
    return nullptr;
}

OperDoor* OperMap::FindDoor(int x, int z, Object* house)
{
    for (int d = 0; d < _doors.Size(); d++)
    {
        OperDoor& door = _doors[d];
        if (door.x == x && door.z == z && door.house == house)
        {
            return &door;
        }
    }
    return nullptr;
}

bool OperMap::FindDoor(OperInfo& info, int xs, int zs, int xe, int ze)
{
    for (int d = 0; d < _doors.Size(); d++)
    {
        OperDoor& door1 = _doors[d];
        if (door1.x != xs || door1.z != zs || !door1.house)
        {
            continue;
        }
        OperDoor* door2 = FindDoor(xe, ze, door1.house);
        if (!door2)
        {
            continue;
        }

        // teleport found
        info.house = door1.house;
        info.from = door1.exit;
        info.to = door2->exit;
        return true;
    }
    return false;
}

Object* OperMap::FindDoor(int xs, int zs, int xe, int ze)
{
    for (int d = 0; d < _doors.Size(); d++)
    {
        OperDoor& door1 = _doors[d];
        if (door1.x != xs || door1.z != zs || !door1.house)
        {
            continue;
        }
        OperDoor* door2 = FindDoor(xe, ze, door1.house);
        if (!door2)
        {
            continue;
        }

        // teleport found
        return door1.house;
    }
    return nullptr;
}

#define DIAG 0
#define DIAG_PATH 0

int OperMap::ResultPath(AIUnit* unit)
{
#if DIAG
    LOG_DEBUG(Script, "Path for {}, length = {}", (const char*)unit->GetDebugName(), _path.Size());
    for (int i = 0; i < _path.Size(); i++)
    {
        OperInfo& info = _path[i];
        LOG_DEBUG(Script, " {}: house {:x} {} .. {}", i, (uintptr_t)info.house, info.from, info.to);
    }
#endif
#if DIAG_PATH
    // Original path
    LOG_DEBUG(Script, "Raw path for {}, length = {}", (const char*)unit->GetDebugName(), _path.Size());
    for (int i = 0; i < _path.Size(); i++)
    {
        OperInfo& info = _path[i];
        if (info.house)
            LOG_DEBUG(Script, " {}: {}, {}, cost {:.0f} (house {:x} {}->{})", i, info._x, info._z, info._cost,
                      (uintptr_t)info.house, info.from, info.to);
        else
            LOG_DEBUG(Script, " {}: {}, {}, cost {:.0f}", i, info._x, info._z, info._cost);
    }
#endif

    Path& path = unit->GetPath();

    path.Clear();
    int n = _path.Size();
    PoseidonAssert(n >= 1);
    if (n < 1)
    {
        return 0;
    }

    // avoid more steps inside one building
    for (int i = 1; i < n; i++)
    {
        OperInfo& last = _path[i - 1];
        OperInfo& cur = _path[i];
        if (last.house && cur.house && last.house == cur.house)
        {
            // join
            cur.from = last.from;
            i--;
            n--;
            _path.Delete(i);
        }
    }
#if DIAG
    LOG_DEBUG(Script, "After optimization: length = {}", _path.Size());
    for (int i = 0; i < _path.Size(); i++)
    {
        OperInfo& info = _path[i];
        LOG_DEBUG(Script, " {}: house {:x} {} .. {}", i, (uintptr_t)info.house, info.from, info.to);
    }
#endif

    EntityAI* veh = unit->GetVehicle();
    float combatHeight = veh->GetCombatHeight();

    int lastIndex = 0;
    float lastCost = 0;
    Vector3 lastPos;
    lastPos.Init();
    lastPos[0] = _path[0]._x * OperItemGrid + 0.5 * OperItemGrid;
    lastPos[2] = _path[0]._z * OperItemGrid + 0.5 * OperItemGrid;
    lastPos[1] = GLandscape->RoadSurfaceYAboveWater(lastPos[0], lastPos[2]) + combatHeight;

    {
        // add start point
        int index = path.Add();
        OperInfoResult& info = path[index];
        info._pos = lastPos;
        info._cost = lastCost;
        if (n < 2)
        {
            RptF("Error %s: Invalid path - lenght 1", (const char*)unit->GetDebugName());
            return 1;
        }
    }

    int maxIndex = -1;
    float pathDist = 0;

    while (lastIndex < n - 1)
    {
        int i = lastIndex + 1;

        int dx = _path[i]._x - _path[i - 1]._x;
        int dz = _path[i]._z - _path[i - 1]._z;
        while (i < n - 1 && _path[i].house == nullptr && // way through building
               _path[i + 1].house == nullptr &&
               // check direction change
               _path[i + 1]._x - _path[i]._x == dx && _path[i + 1]._z - _path[i]._z == dz)
        {
            i++;
        }
#if DIAG
        LOG_DEBUG(Script, "    Index down to {}", i);
#endif
        lastIndex = i;
        float distance = 0;
        int index = i;
        if (_path[i].house != nullptr)
        {
            HOUSE_PATH_ARRAY(hPath, 64);
            bool found = _path[i].house->GetIPaths()->SearchPath(_path[i].from, _path[i].to, hPath);
            (void)found;
            PoseidonAssert(found);
            for (int p = 0; p < hPath.Size(); p++)
            {
                Vector3 actPos = hPath[p].pos;
                actPos[1] += combatHeight;
                float d = actPos.Distance(lastPos);
                if (d < 0.1)
                {
                    continue;
                }
                lastCost += _path[i].house->GetIPaths()->GetBType()->GetCoefInside() * d * veh->GetType()->GetMinCost();
                index = path.Add();
#if DIAG
                LOG_DEBUG(Script, "  Added point {} (house)", index);
#endif
                OperInfoResult& info = path[index];
                info._pos = actPos;
                info._cost = lastCost;
                info._house = _path[i].house;
                info._index = hPath[p].index;
                distance += d;
                lastPos = actPos;
            }
            float diffCost = lastCost - _path[i]._cost;
            for (int j = i; j < n; j++)
            {
                _path[j]._cost += diffCost;
            }
#if DIAG
            LOG_DEBUG(Script, " {}: Inserted house path length {}", i, path.Size());
#endif
        }
        else
        {
            index = path.Add();
#if DIAG
            LOG_DEBUG(Script, "  Added point {} (no house)", index);
#endif
            OperInfoResult& info = path[index];
            Vector3 actPos;
            actPos.Init();
            actPos[0] = _path[i]._x * OperItemGrid + 0.5 * OperItemGrid;
            actPos[2] = _path[i]._z * OperItemGrid + 0.5 * OperItemGrid;
            actPos[1] = GLandscape->RoadSurfaceYAboveWater(actPos[0], actPos[2]) + combatHeight;
            info._pos = actPos;
            info._cost = lastCost = _path[i]._cost;
            distance = actPos.Distance(lastPos);
            lastPos = actPos;
        }
        pathDist += distance;
        if (maxIndex < 0 && pathDist > DIST_MAX_OPER)
        {
            maxIndex = index + 1;
#if DIAG
            LOG_DEBUG(Script, "  Max index is {}", maxIndex);
#endif
        }
    }

#if DIAG
    LOG_DEBUG(Script, "");
#endif

    // final pass - limit cost to reasonable value
    if (path.Size() > 1)
    {
        float minCostPerM = veh->GetType()->GetMinCost();
        float maxCostPerM = veh->GetType()->GetMinCost() * 20;
        saturateMin(maxCostPerM, 3.6);
        // clamp to the faster of maxspeed/20 or 1/3.6 m/s (saturateMin, not Max)

        Vector3 lastPos = path[0]._pos;
        float lastSrcCost = path[0]._cost;
        float lastDstCost = 0;
        for (int i = 1; i < path.Size(); i++)
        {
            Vector3Val pos = path[i]._pos;
            float srcCost = path[i]._cost;
            float dist = pos.Distance(lastPos);

            float diffCost = srcCost - lastSrcCost;

            float minCost = minCostPerM * dist;
            float maxCost = maxCostPerM * dist;

            if (diffCost < minCost)
            {
                diffCost = minCost;
            }
            else if (diffCost > maxCost)
            {
                if (diffCost > 1e5)
                {
                    LOG_DEBUG(Script, "{}: Cost too high", (const char*)unit->GetDebugName());
                }
                diffCost = maxCost;
            }

            lastDstCost = lastDstCost + diffCost;
            path[i]._cost = lastDstCost;
            lastPos = pos;
            lastSrcCost = srcCost;
        }
    }
    if (maxIndex < 0)
    {
        maxIndex = path.Size();
    }

#if DIAG_PATH
    // Result path
    LOG_DEBUG(Script, "Result path for {}, length = {}", (const char*)unit->GetDebugName(), maxIndex);
    for (int i = 0; i < path.Size(); i++)
    {
        LOG_DEBUG(Script, " {}: {:.0f}, {:.0f}, cost {:.0f}", i, path[i]._pos.X() / OperItemGrid,
                  path[i]._pos.Z() / OperItemGrid, path[i]._cost);
    }
#endif

    return maxIndex;
}

void OperField::Rasterize(Object* obj, const Vector3* minMax, RastMode mode, OperItemType type, float xResize,
                          float zResize, bool soldier)
{
    Point3 rect[4];
    rect[0] = minMax[0];
    rect[0][0] -= xResize;
    rect[0][2] -= zResize;
    rect[2] = minMax[1];
    rect[2][0] += xResize;
    rect[2][2] += zResize;
    rect[1].Init();
    rect[1][0] = rect[0][0];
    rect[1][1] = rect[0][1];
    rect[1][2] = rect[2][2];
    rect[3].Init();
    rect[3][0] = rect[2][0];
    rect[3][1] = rect[0][1];
    rect[3][2] = rect[0][2];
    for (int j = 0; j < 4; j++)
    {
        rect[j] = obj->PositionModelToWorld(rect[j]);
        rect[j][0] *= InvOperItemGrid;
        rect[j][2] *= InvOperItemGrid;
    }
    float xMin = rect[0][0];
    float xMax = rect[0][0];
    int jzMin = 0;
    float zMin = rect[0][2];
    float zMax = rect[0][2];
    for (int j = 1; j < 4; j++)
    {
        if (rect[j][0] < xMin)
        {
            xMin = rect[j][0];
        }
        else if (rect[j][0] > xMax)
        {
            xMax = rect[j][0];
        }
        if (rect[j][2] < zMin)
        {
            zMin = rect[j][2];
            jzMin = j;
        }
        else if (rect[j][2] > zMax)
        {
            zMax = rect[j][2];
        }
    }
    if (xMin >= (_x + 1) * OperItemRange)
    {
        return;
    }
    if (xMax < _x * OperItemRange)
    {
        return;
    }
    if (zMin >= (_z + 1) * OperItemRange)
    {
        return;
    }
    if (zMax < _z * OperItemRange)
    {
        return;
    }

    int baseX = _x * OperItemRange;
    int baseZ = _z * OperItemRange;

    int xxMinLast = INT_MIN, xxMaxLast = INT_MAX;

    MapCoord zzMin = toIntFloor(zMin);
    MapCoord zzMax = toIntFloor(zMax);
    MapCoord zzL, zzR, xx, xxMin, xxMax;
    float xL, xR, dxL, dxR;
    xL = xR = rect[jzMin][0];
    int jLCur, jRCur, jLNext, jRNext;
    jLCur = jRCur = jzMin;
    jLNext = (jLCur - 1) & 3;
    jRNext = (jRCur + 1) & 3;
    bool bEnd = false;

    float dz = zzMin + 1 - zMin;

    float denomL = (rect[jLNext][2] - rect[jLCur][2]);
    float denomR = (rect[jRNext][2] - rect[jRCur][2]);
    float invDL = denomL != 0 ? 1 / denomL : 1e10;
    float invDR = denomR != 0 ? 1 / denomR : 1e10;

    dxL = (rect[jLNext][0] - rect[jLCur][0]) * invDL * dz;
    dxR = (rect[jRNext][0] - rect[jRCur][0]) * invDR * dz;

    for (int j = zzMin; j <= zzMax; j++)
    {
        zzL = toIntFloor(rect[jLNext][2]);
        if (zzL == j)
        {
            xL = rect[jLNext][0];
        }
        else
        {
            xL += dxL;
        }
        zzR = toIntFloor(rect[jRNext][2]);
        if (zzR == j)
        {
            xR = rect[jRNext][0];
        }
        else
        {
            xR += dxR;
        }
        if (xL < xR)
        {
            xxMin = toIntFloor(xL);
            xxMax = toIntFloor(xR);
        }
        else
        {
            xxMax = toIntFloor(xL);
            xxMin = toIntFloor(xR);
        }

        while (!bEnd && zzL == j)
        {
            xx = toIntFloor(rect[jLNext][0]);
            if (xx < xxMin)
            {
                xxMin = xx;
            }
            else if (xx > xxMax)
            {
                xxMax = xx;
            }
            jLCur--;
            jLCur &= 3;
            xL = rect[jLCur][0];
            jLNext--;
            jLNext &= 3;
            bEnd = jLNext == jRCur;
            zzL = toIntFloor(rect[jLNext][2]);
        }
        if (!bEnd)
        {
            float denomL = (rect[jLNext][2] - rect[jLCur][2]);
            float invDL = denomL != 0 ? 1 / denomL : 1e10;
            dxL = (rect[jLNext][0] - rect[jLCur][0]) * invDL;
        }

        while (!bEnd && zzR == j)
        {
            xx = toIntFloor(rect[jRNext][0]);
            if (xx < xxMin)
            {
                xxMin = xx;
            }
            else if (xx > xxMax)
            {
                xxMax = xx;
            }
            jRCur++;
            jRCur &= 3;
            xR = rect[jRCur][0];
            jRNext++;
            jRNext &= 3;
            bEnd = jRNext == jLCur;
            zzR = toIntFloor(rect[jRNext][2]);
        }
        if (!bEnd)
        {
            float denomR = (rect[jRNext][2] - rect[jRCur][2]);
            float invDR = denomR != 0 ? 1 / denomR : 1e10;
            dxR = (rect[jRNext][0] - rect[jRCur][0]) * invDR;
        }

        int z0 = j - baseZ;
        if (z0 < 0 || z0 >= OperItemRange)
        {
            continue;
        }

        // force rasterized section to be continuous
        if (xxMin > xxMaxLast)
        {
            xxMin = xxMaxLast;
        }
        if (xxMax < xxMinLast)
        {
            xxMax = xxMinLast;
        }

        xxMinLast = xxMin, xxMaxLast = xxMax;

        int x0 = xxMin - baseX;
        int x1 = xxMax - baseX;
        saturateMax(x0, 0);
        saturateMin(x1, OperItemRange - 1);

        OperItem* itemsZ0 = _items[z0];

        PoseidonAssert(mode == RMMax);

        if (soldier)
        {
            for (int k = x0; k <= x1; k++)
            {
                if (type > itemsZ0[k]._typeSoldier)
                {
                    itemsZ0[k]._typeSoldier = type;
                }
            }
        }
        else
        {
            for (int k = x0; k <= x1; k++)
            {
                if (type > itemsZ0[k]._type)
                {
                    itemsZ0[k]._type = type;
                }
            }
        }
    }
}

#define OBJ_SPACE 1.0
#define OBJ_AVOID 8.0
#define ROAD_SPACE 1.0
#define OBJ_HEIGHT 2.5

#define OBJ_SPACE_SOLDIER 0.2
#define OBJ_AVOID_SOLDIER 2.0
#define ROAD_SPACE_SOLDIER 0.0
#define OBJ_HEIGHT_SOLDIER 0.5

void OperField::CreateField(int mask)
{
    OperItem* item;
    int ix, iz, i;
    const float maxWater = -0.5;
    // Malprave terrainHole3: items inside a terrain hole are blocked (OITSpace, like a building's walls; terrainHole3b:
    // terrainHole3 used OITAvoid, which for men is the cheap "near cover" type, not a block); men get in and out
    // only along the building's house paths (its Paths LOD exits), as for any house
    Landscape::TerrainHoleArea holes[32];
    int nHoles = 0;
    if (mask & MASK_USE_BUFFER)
    {
        Vector3 fieldCentre(_x * LandGrid + LandGrid * 0.5f, 0, _z * LandGrid + LandGrid * 0.5f);
        nHoles = GLandscape->GatherTerrainHoles(fieldCentre, LandGrid * 0.75f, holes, 32);
    }
    for (iz = 0; iz < OperItemRange; iz++)
    {
        for (ix = 0; ix < OperItemRange; ix++)
        {
            item = &_items[iz][ix];
            item->_searchID = 0;
            item->_type = OITNormal;
            item->_typeSoldier = OITNormal;
            if (mask & MASK_USE_BUFFER)
            {
                // sample center of the grid squares
                float xt = _x * LandGrid + ix * OperItemGrid + OperItemGrid * 0.5f;
                float zt = _z * LandGrid + iz * OperItemGrid + OperItemGrid * 0.5f;
                // note: all samples are in the same grid
                // SurfaceY may be optimized for contant grid
                float yt = GLandscape->SurfaceY(xt, zt);
                if (yt < maxWater)
                {
                    item->_typeSoldier = OITWater;
                    item->_type = OITWater;
                }
                if (nHoles > 0 && Landscape::TerrainHoleIndex(holes, nHoles, xt, zt, GLandscape->SurfaceY(xt,zt)) >= 0)
                {
                    item->_typeSoldier = OITSpace; // Malprave terrainHole3b (was OITAvoid)
                    item->_type = OITSpace;
                }
            }
        }
    }

    Object* obj;
    // avoid
    for (ix = _x - 1; ix <= _x + 1; ix++)
    {
        for (iz = _z - 1; iz <= _z + 1; iz++)
        {
            if (InRange(ix, iz))
            {
                const ObjectList& list = GLOB_LAND->GetObjects(iz, ix);
                for (i = 0; i < list.Size(); i++)
                {
                    obj = list[i];
                    if (obj == nullptr)
                    {
                        continue;
                    }
                    LODShape* shape = obj->GetShape();
                    if (!shape)
                    {
                        continue;
                    }
                    if (obj->GetMass() < 5.0)
                    {
                        continue;
                    }
                    if (!obj->HasGeometry())
                    {
                        continue;
                    }
                    if (obj->Static())
                    {
                        if ((mask & MASK_AVOID_OBJECTS) == 0)
                        {
                            continue;
                        }
                    }
                    else if (obj->GetType() == TypeVehicle)
                    {
                        if ((mask & MASK_AVOID_VEHICLES) == 0)
                        {
                            continue;
                        }
                    }

                    OperItemType type = OITAvoid;
                    if (obj->IsPassable())
                    {
                        type = OITAvoidBush;
                    }
                    else if (obj->GetDestructType() == DestructTree)
                    {
                        type = OITAvoidTree;
                    }
                    // render all convex components
                    const ConvexComponents& ccs = shape->GetGeomComponents();
                    for (int c = 0; c < ccs.Size(); c++)
                    {
                        const ConvexComponent* cc = ccs.Get(c);
                        Vector3Val min = obj->PositionModelToWorld(cc->Min());
                        float minSurfY = GLOB_LAND->RoadSurfaceY(min.X(), min.Z());
                        float height = min.Y() - minSurfY;
                        // rasterize for both soldiers and vehicles
                        if (height <= OBJ_HEIGHT_SOLDIER)
                        {
                            Rasterize(obj, cc->MinMax(), RMMax, type, OBJ_AVOID_SOLDIER, OBJ_AVOID_SOLDIER, true);
                        }
                        if (height <= OBJ_HEIGHT)
                        {
                            Rasterize(obj, cc->MinMax(), RMMax, type, OBJ_AVOID, OBJ_AVOID, false);
                        }
                    }
                    // insert doors
                    const IPaths* house = obj->GetIPaths();
                    if (house)
                    {
                        int n = house->NExits();
                        for (int i = 0; i < n; i++)
                        {
                            int exit = house->GetExit(i);
                            Vector3Val pos = house->GetPosition(exit);
                            int x = toIntFloor(pos.X() * InvOperItemGrid);
                            int z = toIntFloor(pos.Z() * InvOperItemGrid);
                            int xx = x / OperItemRange;
                            int zz = z / OperItemRange;
                            if (xx == _x && zz == _z)
                            {
                                int index = _doors.Add();
                                OperDoor& door = _doors[index];
                                door.house = const_cast<Object*>(house->GetObject());
                                door.exit = exit;
                                door.x = x;
                                door.z = z;
                            }
                        }
                    }
                }
            }
        }
    }
    // space
    for (ix = _x - 1; ix <= _x + 1; ix++)
    {
        for (iz = _z - 1; iz <= _z + 1; iz++)
        {
            if (InRange(ix, iz))
            {
                const ObjectList& list = GLOB_LAND->GetObjects(iz, ix);
                for (i = 0; i < list.Size(); i++)
                {
                    obj = list[i];
                    if (obj == nullptr)
                    {
                        continue;
                    }
                    LODShape* shape = obj->GetShape();
                    if (!shape)
                    {
                        continue;
                    }
                    if (obj->Static())
                    {
                        if ((mask & MASK_AVOID_OBJECTS) == 0)
                        {
                            continue;
                        }
                    }
                    else if (obj->GetType() == TypeVehicle)
                    {
                        if ((mask & MASK_AVOID_VEHICLES) == 0)
                        {
                            continue;
                        }
                    }
                    OperItemType type = OITSpace;
                    OperItemType typeHigh = OITSpace;
                    float bottomOfHigh = 1e10; // where typeHigh should start
                    float forceXBorder = 0;
                    if (obj->GetType() == Network)
                    {
                        // note: roads do not have any components

                        if (!obj->HasGeometry() || shape->FindGeometryLevel() < 0)
                        {
                            Rasterize(obj, shape->MinMax(), RMMax, OITRoad, OBJ_SPACE_SOLDIER, OBJ_SPACE_SOLDIER, true);
                            Rasterize(obj, shape->MinMax(), RMMax, OITRoad, OBJ_SPACE, OBJ_SPACE, false);
                            continue;
                        }
                        // if there is geometry, there may be some components as well
                        // if it has components, it is probably a bridge
                        float width = shape->Max().X() - shape->Min().X();
                        // leave only minimal area around roadway center
                        float xBorder = (width - 1.0) * 0.5f;
                        float zBorder = OBJ_SPACE + 1.5;
                        saturateMin(xBorder, 6);

                        Rasterize(obj, shape->MinMax(), RMMax, OITRoadForced, -xBorder, zBorder, true);
                        Rasterize(obj, shape->MinMax(), RMMax, OITRoadForced, -xBorder, zBorder, false);
                        forceXBorder = 5.0;

                        type = OITSpaceRoad;
                        Shape* road = shape->RoadwayLevel();
                        if (road)
                        {
                            bottomOfHigh = road->Max().Y();
                        }
                    }
                    else
                    {
                        if (!obj->HasGeometry())
                        {
                            continue;
                        }
                        if (obj->GetMass() < 5.0)
                        {
                            continue;
                        }

                        if (obj->IsPassable())
                        {
                            type = OITSpaceBush;
                        }
                        else if (obj->GetDestructType() == DestructTree)
                        {
                            type = OITSpaceTree;
                        }
                    }

                    bool hasPaths = obj->GetIPaths() != nullptr;

                    // render all convex components
                    const ConvexComponents& ccs = shape->GetGeomComponents();
                    for (int c = 0; c < ccs.Size(); c++)
                    {
                        const ConvexComponent* cc = ccs.Get(c);
                        Vector3Val min = obj->PositionModelToWorld(cc->Min());
                        Vector3Val ave = obj->PositionModelToWorld(0.5 * (cc->Min() + cc->Max()));
                        float aveSurfY = GLOB_LAND->RoadSurfaceY(ave.X(), ave.Z());
                        float height = min.Y() - aveSurfY;
                        // rasterize for both soldiers and vehicles
                        OperItemType cType = type;
                        float fXBorder = forceXBorder;
                        float maxBorder = 100;
                        if (cc->Max().Y() > bottomOfHigh + OBJ_HEIGHT)
                        {
                            cType = typeHigh;
                            fXBorder = 0;
                            maxBorder = 0.2;
                        }
                        if (hasPaths || height <= OBJ_HEIGHT_SOLDIER)
                        {
                            Rasterize(obj, cc->MinMax(), RMMax, cType,
                                      floatMin(maxBorder, floatMax(forceXBorder, OBJ_SPACE_SOLDIER)),
                                      floatMin(maxBorder, OBJ_SPACE_SOLDIER), true);
                        }
                        if (type == OITSpaceRoad || height <= OBJ_HEIGHT)
                        {
                            Rasterize(obj, cc->MinMax(), RMMax, cType,
                                      floatMin(maxBorder, floatMax(forceXBorder, OBJ_SPACE)),
                                      floatMin(maxBorder, OBJ_SPACE), false);
                        }
                    }
                }
            }
        }
    }
    // Malprave terrainHole3b: a house exit whose item lies in a terrain hole (a stairwell's top step) stays reachable
    for (int d = 0; d < _doors.Size() && nHoles > 0; d++)
    {
        int dx = _doors[d].x - _x * OperItemRange;
        int dz = _doors[d].z - _z * OperItemRange;
        if (dx < 0 || dx >= OperItemRange || dz < 0 || dz >= OperItemRange)
        {
            continue;
        }
        float xt = _x * LandGrid + dx * OperItemGrid + OperItemGrid * 0.5f;
        float zt = _z * LandGrid + dz * OperItemGrid + OperItemGrid * 0.5f;
        if (Landscape::TerrainHoleIndex(holes, nHoles, xt, zt, GLandscape->SurfaceY(xt,zt)) >= 0)
        {
            _items[dz][dx]._type = OITNormal;
            _items[dz][dx]._typeSoldier = OITNormal;
        }
    }
}
