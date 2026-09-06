#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Audio/DynSound.hpp>
#include <Poseidon/World/World.hpp>
#include <Random/randomGen.hpp>
#include <Poseidon/World/Terrain/Roads.hpp>
#include <Poseidon/World/MapTypes.hpp>
#include <Poseidon/Core/Global.hpp>
#include <stdlib.h>
#include <string.h>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Containers/Array2D.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/GlobalAlive.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>
#include <Poseidon/Foundation/Math/MathOpt.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/LLinks.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>

namespace Poseidon
{

} // namespace Poseidon
#include <Poseidon/AI/AI.hpp>
#include <Poseidon/World/Scene/ObjectClasses.hpp>

#include <Poseidon/Foundation/Algorithms/Qsort.hpp>
#include <Poseidon/Foundation/Common/Filenames.hpp>
namespace Poseidon
{

static int CalculateGradient(float gradient)
{
    gradient = fabs(gradient);
    if (gradient >= 0.95)
    {
        return 7; // nonpenetrable even for man
    }
    else if (gradient >= 0.60)
    {
        return 6; // man can go here
    }
    else if (gradient >= 0.40)
    {
        return 5; // most vehicles can go here, but very slow
    }
    else if (gradient >= 0.25)
    {
        return 4; // most vehicles can go here, but slow
    }
    else if (gradient >= 0.15)
    {
        return 3; // most vehicles can go here at 0.33 speed
    }
    else if (gradient >= 0.10)
    {
        return 2; // most vehicles can go here at 0.5 speed
    }
    else if (gradient >= 0.05)
    {
        return 1; // most vehicles can go here at 0.8 speed
    }
    return 0; // leveled surfaces - top speed for all
}

// calculate max. gradient of plane gradX,gradZ
static float MaxGradient(float gradX, float gradZ)
{
    // note: this is not exact, max. gradient may lay in arbitrary direction
    float gradXZ = ((gradX + gradZ) * 0.5) * H_SQRT2;
    return floatMax(gradXZ, floatMax(gradX, gradZ));
}

inline ForestPlain* NewForest(const char* name, int id)
{
    Ref<LODShapeWithShadow> shape = Shapes.New(name, false, true);
    Object* obj = NewObject(shape, id);
    return dyn_cast<ForestPlain>(obj);
}

#define ONLY_FOREST 0

void Landscape::ResetGeography()
{
    _mountains.Clear();
}

// Lazy initialization to avoid SIOF
static const RStringB& GetRoadString()
{
    static const RStringB roadString = "road";
    return roadString;
}

// If geography information is already present, detect it and return as fast as possible.

void Landscape::EnableGeographyAccumulation()
{
    const size_t cells = static_cast<size_t>(_landRange) * _landRange;
    _geoAccumTotalArea.assign(cells, 0.0f);
    _geoAccumHardArea.assign(cells, 0.0f);
    _geoAccumTotalCount.assign(cells, 0);
    _geoAccumHardCount.assign(cells, 0);
    _geoAccumEnabled = true;
}

void Landscape::AccumulateGeographyForObject(Object* obj)
{
    if (!_geoAccumEnabled || !obj || obj->GetType() != Primary)
        return;
    LODShape* shape = obj->GetShape();
    static int accumCalls = 0, accumSkipped = 0;
    ++accumCalls;
    // InitGeography's rule wants a mass and a Geometry LOD with components. Converted
    // native (.xob) models carry neither -- the church reports mass 0 and no geometry
    // level -- so for a shape without those the visual bounds decide, and anything
    // narrower than a metre (a bush, a sign) still stays out.
    const bool hasPhysicalRule = shape && shape->Mass() >= 10.0 && shape->FindGeometryLevel() >= 0 &&
                                 shape->GetGeomComponents().Size() > 0;
    const bool hasVisualBounds = shape && shape->NLevels() > 0 && floatMax(shape->Max()[0], shape->Max()[2]) >= 1.0f;
    if (!hasPhysicalRule && !hasVisualBounds)
    {
        ++accumSkipped;
        if (accumSkipped <= 5 && shape)
            LOG_INFO(World, "RFG-101 skip '{}': mass={:.1f} geomLevel={} components={} max=[{:.1f} {:.1f}]", shape->Name(),
                     shape->Mass(), shape->FindGeometryLevel(), shape->GetGeomComponents().Size(), shape->Max()[0],
                     shape->Max()[2]);
        return;
    }
    if (accumCalls % 1000 == 1)
        LOG_INFO(World, "RFG-101 geography accumulation: {} objects seen, {} skipped, revision {}", accumCalls,
                 accumSkipped, _geographyRevision);
    // Same classification InitGeography applies (see below): trees/tents are soft,
    // indestructible things count double, forests are not objects.
    int hardWeight = 0;
    if (dyn_cast<ForestPlain>(obj) == nullptr)
    {
        if (obj->GetDestructType() == DestructNo)
            hardWeight = 2;
        else if (obj->GetDestructType() != DestructTree && obj->GetDestructType() != DestructTent)
            hardWeight = 1;
    }
    // A converted native model has no destruct class the rule can read (the church
    // came through as "tree"-class soft); without the physical rule its bounds are the
    // only evidence, and a thing wider than a metre is a hard obstacle for clutter.
    if (!hasPhysicalRule && dyn_cast<ForestPlain>(obj) == nullptr && hardWeight == 0)
        hardWeight = 1;
    // A converted native model is autodetected as DestructTree whatever it is (the church
    // measured destruct=3), which the rule reads as soft. Only real vegetation is soft.
    if (hardWeight == 0 && dyn_cast<ForestPlain>(obj) == nullptr && obj->GetDestructType() == DestructTree &&
        shape->Name() && std::strstr(shape->Name(), ".xob") && !std::strstr(shape->Name(), "vegetation/"))
        hardWeight = 1;
    const float objRad = floatMax(shape->Max()[0], shape->Max()[2]);
    const Vector3 objCenter = obj->PositionModelToWorld(shape->GeometryCenter());
    const float xMaxObj = objCenter.X() + objRad, xMinObj = objCenter.X() - objRad;
    const float zMaxObj = objCenter.Z() + objRad, zMinObj = objCenter.Z() - objRad;
    const int cx0 = std::clamp(toIntFloor(xMinObj * _invLandGrid), 0, _landRange - 1);
    const int cx1 = std::clamp(toIntFloor(xMaxObj * _invLandGrid), 0, _landRange - 1);
    const int cz0 = std::clamp(toIntFloor(zMinObj * _invLandGrid), 0, _landRange - 1);
    const int cz1 = std::clamp(toIntFloor(zMaxObj * _invLandGrid), 0, _landRange - 1);
    const float cellArea = _landGrid * _landGrid;
    for (int z = cz0; z <= cz1; z++)
    {
        for (int x = cx0; x <= cx1; x++)
        {
            const float xMin = x * _landGrid, xMax = xMin + _landGrid;
            const float zMin = z * _landGrid, zMax = zMin + _landGrid;
            const float ax0 = floatMax(xMinObj, xMin), ax1 = floatMin(xMaxObj, xMax);
            const float az0 = floatMax(zMinObj, zMin), az1 = floatMin(zMaxObj, zMax);
            if (ax1 <= ax0 || az1 <= az0)
                continue;
            const float area = (ax1 - ax0) * (az1 - az0);
            const size_t i = static_cast<size_t>(z) * _landRange + x;
            _geoAccumTotalArea[i] += area;
            _geoAccumTotalCount[i] = static_cast<uint16_t>(std::min(65535, _geoAccumTotalCount[i] + 1));
            if (hardWeight == 2)
            {
                _geoAccumHardArea[i] += area * 4;
                _geoAccumHardCount[i] = static_cast<uint16_t>(std::min(65535, _geoAccumHardCount[i] + 2));
            }
            else if (hardWeight == 1)
            {
                _geoAccumHardArea[i] += area;
                _geoAccumHardCount[i] = static_cast<uint16_t>(std::min(65535, _geoAccumHardCount[i] + 1));
            }
            const float totalArea = _geoAccumTotalArea[i], hardArea = _geoAccumHardArea[i];
            const int totalCount = _geoAccumTotalCount[i], hardCount = _geoAccumHardCount[i];
            int howMany = 0, hard = 0, full = 0;
            if (totalArea >= cellArea * 9 / 10)
                full = 1;
            else if (totalArea >= cellArea / 2 || totalCount >= 20)
                howMany = 3;
            else if (totalArea >= cellArea / 4 || totalCount >= 10)
                howMany = 2;
            else if (totalCount > 0)
                howMany = 1;
            if (hardArea >= cellArea / 3 || hardCount >= 8)
                hard = 3;
            else if (hardArea >= cellArea / 6 || hardCount >= 4)
                hard = 2;
            else if (hardCount > 0)
                hard = 1;
            GeographyInfo& geog = _geography(x, z);
            const uint32_t before = geog.packed;
            if (full)
                geog.u.full = 1;
            if (static_cast<int>(geog.u.howManyObjects) < howMany)
                geog.u.howManyObjects = howMany;
            if (static_cast<int>(geog.u.howManyHardObjects) < hard)
                geog.u.howManyHardObjects = hard;
            if (geog.packed != before)
                ++_geographyRevision;
        }
    }
    {
        static int churchDiag = 0;
        if (shape->Name() && std::strstr(shape->Name(), "hurch_01") && churchDiag++ < 4)
            LOG_INFO(World, "RFG-101 church accumulated: '{}' mass={:.0f} rad={:.1f} hardWeight={} destruct={} forest={} cells x{}..{} z{}..{} hard@centre={}",
                     shape->Name(), shape->Mass(), objRad, hardWeight, static_cast<int>(obj->GetDestructType()),
                     dyn_cast<ForestPlain>(obj) != nullptr ? 1 : 0, cx0, cx1, cz0, cz1,
                     static_cast<int>(_geography((cx0 + cx1) / 2, (cz0 + cz1) / 2).u.howManyHardObjects));
    }
}

void Landscape::InitGeography()
{
    GRoadNet = new RoadNet;
    GRoadNet->Build(this);

    // check if randomization etc. is valid
    if (_mountains.Size() > 0)
    {
        // all data loaded from file
        // no additional processing required
        return;
    }

    RString name = Glob.header.worldname;
    if (name.GetLength() <= 0)
    {
        char shortName[80];
        GetFilename(shortName, _name);
        name = shortName;
    }
    // if we are running buldozer, world may be not registered
    if (ENGINE_CONFIG.landEditor)
    {
        if (!(Pars >> "CfgWorlds").FindEntry(name))
        {
            return;
        }
    }

    // The precondition is not that the two grids are the same size -- they are not
    // on any Arma 1 world (Sahrani: 512 land, 2048 terrain), and they are not on an
    // OFP world once SubdivideTerrain has run. It is that terrain is a whole
    // power-of-two REFINEMENT of land, so every land cell maps onto an exact
    // terrain footprint and the land-space loops below can address elevation at
    // all. Dim() already rejects non-power-of-two ranges, so this only has to
    // exclude a terrain grid coarser than the land grid.
    const int terrainLog = TerrainPerLandLog();
    const int terrainPerLand = TerrainPerLand();
    DoAssert(terrainLog >= 0 && _terrainRange == (_landRange << terrainLog));
    if (terrainLog < 0)
    {
        // Unreachable with any container this build reads. Bail rather than shift
        // a land index by a negative amount, which is undefined behaviour and not
        // the kind of bug an assert-and-continue build would ever pin down.
        return;
    }

    int x, z;
    for (z = 0; z < _landRange; z++)
    {
        I_AM_ALIVE();
        for (x = 0; x < _landRange; x++)
        {
            GeographyInfo& geog = _geography(x, z);
            geog.packed = 0;
            if (!this_InRange(x + 1, z + 1))
            {
                geog.u.waterDepth = 3;
            }
            else
            {
                // check if the current square is in water
                // int tIndex=GetTex(x,z);
                // AbstractTexture *texture=_texture[tIndex];
                // check (x,z) gradient
                // gradient is in interval 0..+7 (0 == level)
                // The land cell's four corners, in terrain coordinates. The loop
                // skipped the last row and column above, so x+1 and z+1 are at most
                // _landRangeMask and (x+1)<<terrainLog stays inside _terrainRange.
                const int tx = x << terrainLog;
                const int tz = z << terrainLog;
                float y = GetData(tx, tz);
                float yNextX = GetData(tx + terrainPerLand, tz);
                float yNextZ = GetData(tx, tz + terrainPerLand);
                float yNextXZ = GetData(tx + terrainPerLand, tz + terrainPerLand);
                // Water is decided over the cell's whole terrain footprint rather
                // than its corners. "Check if the current square is in water" is a
                // question about the ground inside the square, and once a land cell
                // spans 4x4 terrain cells a channel or a spit can sit entirely
                // between the corners and go unseen. At ratio 1 the footprint IS
                // the four corners, so this is bit-identical on OFP/CWA worlds.
                float yMin = y, yMax = y;
                for (int fz = 0; fz <= terrainPerLand; fz++)
                {
                    for (int fx = 0; fx <= terrainPerLand; fx++)
                    {
                        const float yFoot = GetData(tx + fx, tz + fz);
                        saturateMin(yMin, yFoot), saturateMax(yMax, yFoot);
                    }
                }
                if (yMin < 0.1)
                {
                    if (yMin < -0.8)
                    {
                        if (yMax < -2.5)
                        {
                            // guranteed deep water
                            geog.u.waterDepth = 3;
                        }
                        else
                        {
                            // mixed shallow/deep water
                            geog.u.waterDepth = 2;
                        }
                    }
                    else
                    {
                        // guaranteed shallow water
                        geog.u.waterDepth = 1;
                    }
                }
                float gradX0 = fabs((yNextX - y) * _invLandGrid);
                float gradZ0 = fabs((yNextZ - y) * _invLandGrid);
                float gradX1 = fabs((yNextXZ - yNextZ) * _invLandGrid);
                float gradZ1 = fabs((yNextXZ - yNextX) * _invLandGrid);
                float maxGrad0 = MaxGradient(gradX0, gradZ0);
                float maxGrad1 = MaxGradient(gradX1, gradZ1);
                geog.u.gradient = CalculateGradient(floatMax(maxGrad0, maxGrad1));
                // check how many objects are in the square
                // if there is some forest object, mark square as forest
                float totalArea = 0; // sum area of all objects in the sqaure
                int totalCount = 0;
                float hardArea = 0; // sum area of all objects in the sqaure
                int hardCount = 0;
                int howMany = 0;
                float xMin = x * _landGrid, zMin = z * _landGrid;
                float xMax = xMin + _landGrid, zMax = zMin + _landGrid;
                for (int xx = x - 1; xx <= x + 1; xx++)
                {
                    for (int zz = z - 1; zz <= z + 1; zz++)
                    {
                        if (!this_InRange(xx, zz))
                        {
                            continue;
                        }
                        ObjectList& list = _objects(xx, zz);
                        for (int oi = 0; oi < list.Size(); oi++)
                        {
                            Object* obj = list[oi];
                            LODShape* shape = obj->GetShape();
                            if (!shape)
                            {
                                continue; // ignore empty objects
                            }
                            const RStringB& className = shape->GetPropertyClass();
                            if (xx == x && zz == z)
                            {
                                if (className == GetRoadString())
                                {
                                    // it is road - use it as road
                                    geog.u.road = true;
                                    continue;
                                }
                                switch (shape->GetMapType())
                                {
                                    case MapForestTriangle:
                                    case MapForestSquare:
                                        geog.u.forestOuter = true;
                                        break;
                                    case MapForestBorder:
                                        continue;
                                }
                            }
                            if (obj->GetType() != Primary)
                            {
                                continue; // consider only primary objects
                            }
                            if (shape->Mass() < 10.0)
                            {
                                continue;
                            }
                            if (shape->FindGeometryLevel() < 0)
                            {
                                continue;
                            }
                            if (shape->GetGeomComponents().Size() <= 0)
                            {
                                continue;
                            }
                            // calculate exact intersection area
                            // of bounding rectangle with landscape rectangle
                            float objRad = floatMax(shape->Max()[0], shape->Max()[2]);
                            Vector3 objCenter = obj->PositionModelToWorld(shape->GeometryCenter());
                            float xMaxObj = objCenter.X() + objRad, xMinObj = objCenter.X() - objRad;
                            float zMaxObj = objCenter.Z() + objRad, zMinObj = objCenter.Z() - objRad;
                            if (xMaxObj <= xMin || xMinObj >= xMax)
                            {
                                continue;
                            }
                            if (zMaxObj <= zMin || zMinObj >= zMax)
                            {
                                continue;
                            }
                            saturateMin(xMaxObj, xMax), saturateMax(xMinObj, xMin);
                            saturateMin(zMaxObj, zMax), saturateMax(zMinObj, zMin);
                            float area = (xMaxObj - xMinObj) * (zMaxObj - zMinObj);
                            totalArea += area;
                            totalCount++;
                            if (dyn_cast<ForestPlain>(obj) == nullptr)
                            {
                                if (obj->GetDestructType() == DestructNo)
                                {
                                    // undestroyable obstacles calculate much more
                                    hardArea += area * 4;
                                    hardCount += 2;
                                }
                                else if (obj->GetDestructType() != DestructTree &&
                                         obj->GetDestructType() != DestructTent)
                                {
                                    // normal objects are hard to maneuvre
                                    hardArea += area;
                                    hardCount += 1;
                                }
                            }
                        }
                    }
                }
                // filled rectangles are assumed forest
                if (totalArea >= (_landGrid * _landGrid) * 9 / 10)
                {
                    geog.u.full = 1;
                }
                else if (totalArea >= (_landGrid * _landGrid) / 2 || totalCount >= 20)
                {
                    if (howMany < 3)
                    {
                        howMany = 3;
                    }
                }
                else if (totalArea >= (_landGrid * _landGrid) / 4 || totalCount >= 10)
                {
                    if (howMany < 2)
                    {
                        howMany = 2;
                    }
                }
                else if (totalCount > 0)
                {
                    if (howMany < 1)
                    {
                        howMany = 1;
                    }
                }
                // detect antitank obstacles
                int hard = 0;
                if (hardArea >= (_landGrid * _landGrid) / 3 || hardCount >= 8)
                {
                    if (hard < 3)
                    {
                        hard = 3;
                    }
                }
                else if (hardArea >= (_landGrid * _landGrid) / 6 || hardCount >= 4)
                {
                    if (hard < 2)
                    {
                        hard = 2;
                    }
                }
                else if (hardCount > 0)
                {
                    if (hard < 1)
                    {
                        hard = 1;
                    }
                }

                geog.u.howManyObjects = howMany;
                geog.u.howManyHardObjects = hard;

                if (geog.u.road)
                { // road is always well penetrable
                    geog.u.full = false;
                    // geog.waterDepth=0;
                    if (geog.u.gradient > 5)
                    {
                        geog.u.gradient = 5;
                    }
                    if (geog.u.howManyObjects > 2)
                    {
                        geog.u.howManyObjects = 2;
                    }
                }
            }
        }
    }

    ReplaceObjects(name);

    I_AM_ALIVE();
    InitSoundMap();
    I_AM_ALIVE();
    InitRandomization();
    I_AM_ALIVE();
    InitMountains();
}

struct ReplaceObjectInfo
{
    Ref<LODShapeWithShadow> replace;
    RefArray<LODShapeWithShadow> with;
    bool center;
};

void Landscape::ReplaceObjects(RString name)
{
    if (!_objectIds.empty())
    {
        RebuildIDCache();
    }
    else
    {
        ResetObjectIDs();
    }
    // decide which objects to replace
    const ParamEntry& world = Pars >> "CfgWorlds" >> name;

    const ParamEntry* entry = world.FindEntry("ReplaceObjects");
    if (!entry)
    {
        return;
    }
    int n = entry->GetEntryCount();
    if (n == 0)
    {
        return;
    }

    AutoArray<ReplaceObjectInfo> replace;
    replace.Realloc(n);
    replace.Resize(n);

    for (int i = 0; i < n; i++)
    {
        const ParamEntry& cls = entry->GetEntry(i);
        ReplaceObjectInfo& info = replace[i];

        RString value = cls >> "replace";
        info.replace = Shapes.New(value, false, true);
        int m = (cls >> "with").GetSize();
        info.with.Realloc(m);
        info.with.Resize(m);
        for (int j = 0; j < m; j++)
        {
            RString value = (cls >> "with")[j];
            info.with[j] = Shapes.New(value, false, true);
        }
        info.center = cls.FindEntry("center") ? cls >> "center" : false;
    }

    // replace objects
    for (int z = 0; z < _landRange; z++)
    {
        I_AM_ALIVE();
        for (int x = 0; x < _landRange; x++)
        {
            ObjectList& src = _objects(x, z);
            int on = src.Size();
            RefArray<Object> copy;
            copy.Realloc(on);
            copy.Resize(on);
            for (int oi = 0; oi < on; oi++)
            {
                copy[oi] = src[oi];
            }
            for (int oi = 0; oi < on; oi++)
            {
                Object* obj = copy[oi];
                LODShapeWithShadow* shape = obj->GetShape();
                if (!shape)
                {
                    continue; // ignore empty objects
                }

                // find in replace list
                for (int i = 0; i < replace.Size(); i++)
                {
                    if (replace[i].replace == shape)
                    {
                        Matrix4 transform = obj->Transform();
                        int id = obj->ID();
                        bool idUsed = false;

                        // delete old object
                        for (int j = 0; j < src.Size(); j++)
                        {
                            if (src[j] == obj)
                            {
                                src.Delete(j);
                                break;
                            }
                        }

                        // add new objects
                        for (int j = 0; j < replace[i].with.Size(); j++)
                        {
                            LODShapeWithShadow* newShape = replace[i].with[j];
                            if (newShape)
                            {
                                if (idUsed)
                                {
                                    id = NewObjectID();
                                }
                                else
                                {
                                    idUsed = true;
                                }

                                Object* newObject = NewObject(newShape, id);

                                // transform bcenter
                                if (replace[i].center)
                                {
                                    Vector3 pos;
                                    pos.Init();
                                    pos[0] = x * _landGrid + 0.5 * _landGrid;
                                    pos[2] = z * _landGrid + 0.5 * _landGrid;
                                    pos[1] = SurfaceY(pos[0], pos[2]);
                                    pos += newObject->GetShape()->BoundingCenter();
                                    transform.SetTranslation(pos);
                                }

                                newObject->SetTransform(transform);
                                newObject->InitSkew(this);
                                newObject->SetType(Primary);
                                AddObject(newObject);
                                _objectIds[id] = newObject;
                            }
                        }
                        break;
                    }
                }
            }
        }
    }
}

static int CmpMountains(const Vector3* v0, const Vector3* v1)
{
    return sign(v1->Y() - v0->Y());
}

void Landscape::InitMountains()
{
    _mountains.Clear();
    // found local maximas
    for (int z = 1; z < _landRangeMask; z++)
    {
        for (int x = 1; x < _landRangeMask; x++)
        {
            // A local maximum at LAND granularity, which is what a mountain is here
            // -- the position recorded below is x*_landGrid, so the stencil has to
            // step by a land cell too. GetLandData does the terrain-space lookup;
            // the loop bounds keep x-1 >= 0 and x+1 <= _landRange-1, so every
            // sample maps inside the terrain grid.
            float h11 = GetLandData(x, z);
            if (h11 <= 0)
            {
                continue;
            }
            float h00 = GetLandData(x - 1, z - 1);
            float h01 = GetLandData(x, z - 1);
            float h02 = GetLandData(x + 1, z - 1);
            float h10 = GetLandData(x - 1, z);
            float h12 = GetLandData(x + 1, z);
            float h20 = GetLandData(x - 1, z + 1);
            float h21 = GetLandData(x, z + 1);
            float h22 = GetLandData(x + 1, z + 1);

            if (h11 > h00 && h11 > h01 && h11 > h02 && h11 > h10 && h11 >= h12 && h11 >= h20 && h11 >= h21 &&
                h11 >= h22)
            {
                _mountains.Add(Vector3(x * _landGrid, h11, z * _landGrid));
            }
        }
    }

    QSort(_mountains.Data(), _mountains.Size(), CmpMountains);
}

GeographyInfo Landscape::GetGeography(int x, int z) const
{
    if (!this_InRange(z, x))
    {
        GeographyInfo geogr;
        geogr.packed = 0;
        geogr.u.waterDepth = 3;
        return geogr;
    }
    return _geography(x, z);
}

void Landscape::InitSoundMap()
{
    // some basic environmental sounds
    int x, z;
    // set default sound (birds & wind )
    for (z = 0; z < _landRange; z++)
    {
        for (x = 0; x < _landRange; x++)
        {
            SetSound(x, z, 2); // default sound - bees
        }
    }
    // spread birds sound
    for (z = 0; z < _landRange; z++)
    {
        for (x = 0; x < _landRange; x++)
        {
            // count trees
            int nTrees = 0;
            const ObjectList& list = _objects(x, z);
            for (int i = 0; i < list.Size(); i++)
            {
                Object* obj = list[i];
                LODShape* shape = obj->GetShape();
                if (!shape)
                {
                    continue;
                }
                if (!shape->Name())
                {
                    continue;
                }
                if (strstr(shape->Name(), "\\str "))
                {
                    nTrees++;
                }
                if (strstr(shape->Name(), "\\les "))
                {
                    nTrees += 20;
                }
            }
            if (nTrees >= 3)
            {
                for (int zz = -2; zz <= +2; zz++)
                {
                    for (int xx = -2; xx <= +2; xx++)
                    {
                        SetSound(x + xx, z + zz, 1);
                    }
                }
            }
        }
    }
    for (z = 0; z < _landRange; z++)
    {
        for (x = 0; x < _landRange; x++)
        {
            // some places are quiet
            // Land-space loop, terrain-space heightmap: sample the cell's corner.
            float height = GetLandData(x, z);
            if (height > 170)
            {
                SetSound(x, z, 3); // wind on highlands
            }
        }
    }
    // spread sea sound
    for (z = 0; z < _landRange; z++)
    {
        for (x = 0; x < _landRange; x++)
        {
            const GeographyInfo& geogr = _geography(x, z);
            if (geogr.u.waterDepth > 0)
            { // sea is very loud and can be heard very far
                for (int zz = -2; zz <= +2; zz++)
                {
                    for (int xx = -2; xx <= +2; xx++)
                    {
                        SetSound(x + xx, z + zz, 0);
                    }
                }
            }
        }
    }
}

} // namespace Poseidon
#include <Poseidon/Core/Global.hpp>
namespace Poseidon
{

void Landscape::InitDynSounds(const ParamEntry& entry)
{
    // init dynamic sounds
    // load config
    const ParamEntry& sounds = entry >> "sounds";
    for (int i = 0; i < sounds.GetSize(); i++)
    {
        const ParamEntry& source = entry >> RString(sounds[i]);
        RString sim = source >> "simulation";
        const ParamEntry& pos = source >> "position";
        float posX = pos[0];
        float posZ = pos[1];
        float posY = RoadSurfaceY(posX, posZ);
        Vector3 position(posX, posY, posZ);
        DynSoundSource* vehicle = new DynSoundSource(sim);
        vehicle->SetPosition(position);
        // GLOB_WORLD->AddVehicle(vehicle);
        GLOB_WORLD->AddBuilding(vehicle);
    }
}

inline int AvoidSteepChange(int val, int x, bool& someChange)
{
    // never increase abs of val
    if (abs(x) > abs(val))
    {
        return val;
    }
    const int maxDif = 2;
    int dif = val - x;
    if (dif > maxDif)
    {
        dif = maxDif;
    }
    else if (dif < -maxDif)
    {
        dif = -maxDif;
    }
    if (dif)
    {
        someChange = true;
    }
    return x + dif;
}

void Landscape::InitRandomization()
{
    I_AM_ALIVE();
    int i, x, z;
    _colorizePalette[0] = PackedColor(0x80, 0x80, 0x80, 0x80);
    for (i = 1; i < 256; i++)
    {
        const float colorRand = 0.1;
        const float colorMid = 1.0;
        const float brightRand = 0.5;
        const float brightMid = 1.0;
        float randR = GRandGen.RandomValue() * colorRand + (colorMid - colorRand * 0.5);
        float randG = GRandGen.RandomValue() * colorRand + (colorMid - colorRand * 0.5);
        float randB = GRandGen.RandomValue() * colorRand + (colorMid - colorRand * 0.5);
        float randBright = GRandGen.RandomValue() * brightRand + (brightMid - brightRand * 0.5);
        Color color(randR, randG, randB);
        _colorizePalette[i] = PackedColor(color * 0.5 * randBright);
    }
    for (z = 0; z < _landRange; z++)
    {
        for (x = 0; x < _landRange; x++)
        {
            RandomInfo& random = _random(x, z);
            int colorIndex = toInt(GRandGen.RandomValue() * 255);
            if (colorIndex < 0)
            {
                colorIndex = 0;
            }
            if (colorIndex > 255)
            {
                colorIndex = 255;
            }
            random.color = colorIndex;
            random.uOff = random.vOff = 0;
        }
    }
    I_AM_ALIVE();
    for (z = 1; z < _landRangeMask; z++)
    {
        for (x = 1; x < _landRangeMask; x++)
        {
            RandomInfo& random = _random(x, z);
            // check all four neighbourghs
            bool t00 = TextureIsSimple(GetTex(x, z));
            bool tM0 = TextureIsSimple(GetTex(x - 1, z));
            bool t0M = TextureIsSimple(GetTex(x, z - 1));
            bool tMM = TextureIsSimple(GetTex(x - 1, z - 1));
            if (t00 && tM0 && t0M && tMM)
            {
                // full offset possible in both direction
                // int uOff=toInt(GRandGen.RandomValue()*14-7);
                // int vOff=toInt(GRandGen.RandomValue()*14-7);
                int uOff = toInt(GRandGen.RandomValue() * 32 - 16);
                int vOff = toInt(GRandGen.RandomValue() * 32 - 16);
                if (uOff < -7)
                {
                    uOff = -7;
                }
                else if (uOff > +7)
                {
                    uOff = +7;
                }
                if (vOff < -7)
                {
                    vOff = -7;
                }
                else if (vOff > +7)
                {
                    vOff = +7;
                }
                random.uOff = uOff;
                random.vOff = vOff;
            }
        }
    }
    // avoid too steep changes in u-v offset between neighbourghs
    for (i = 0; i < 10; i++)
    {
        I_AM_ALIVE();
        bool someChange = false;
        for (z = 1; z < _landRangeMask; z++)
        {
            for (x = 1; x < _landRangeMask; x++)
            {
                bool t00 = TextureIsSimple(GetTex(x, z));
                bool tM0 = TextureIsSimple(GetTex(x - 1, z));
                bool t0M = TextureIsSimple(GetTex(x, z - 1));
                bool tMM = TextureIsSimple(GetTex(x - 1, z - 1));
                if (t00 && tM0 && t0M && tMM)
                {
                    RandomInfo& random00 = _random(x, z);
                    const RandomInfo& randomP0 = _random(x + 1, z);
                    const RandomInfo& randomN0 = _random(x - 1, z);
                    const RandomInfo& random0P = _random(x, z + 1);
                    const RandomInfo& random0N = _random(x, z - 1);
                    random00.uOff = AvoidSteepChange(random00.uOff, randomP0.uOff, someChange);
                    random00.uOff = AvoidSteepChange(random00.uOff, randomN0.uOff, someChange);
                    random00.uOff = AvoidSteepChange(random00.uOff, random0P.uOff, someChange);
                    random00.uOff = AvoidSteepChange(random00.uOff, random0N.uOff, someChange);
                    random00.vOff = AvoidSteepChange(random00.vOff, randomP0.vOff, someChange);
                    random00.vOff = AvoidSteepChange(random00.vOff, randomN0.vOff, someChange);
                    random00.vOff = AvoidSteepChange(random00.vOff, random0P.vOff, someChange);
                    random00.vOff = AvoidSteepChange(random00.vOff, random0N.vOff, someChange);
                }
            }
        }
        if (!someChange)
        {
            break;
        }
    }
    // Log("Landscape offset map blended: %d",i);
    I_AM_ALIVE();
}
} // namespace Poseidon
