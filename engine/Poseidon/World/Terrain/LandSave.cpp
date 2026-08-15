#include <Poseidon/Core/Application.hpp>

#include <chrono>

#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/FileServer.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/IO/Serialization/ParamArchive.hpp>
#include <Poseidon/AI/AI.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp> // GEngine (GPU-driven retained-scene drop on cell unload)
#include <Poseidon/World/Scene/ObjectClasses.hpp>

#include <Poseidon/Graphics/Rendering/Primitives/Poly.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Foundation/Platform/GamePaths.hpp>
#include <Poseidon/Core/Progress.hpp>

#include <Poseidon/UI/Locale/StringtableExt.hpp>

#include <Poseidon/Foundation/Math/Math3DP.hpp>

#include <Poseidon/World/Terrain/LandFile.hpp>
#include <Poseidon/Asset/Formats/World/Oprw25.hpp>
#include <Poseidon/Asset/Formats/World/Oprw24.hpp>
#include <Poseidon/Asset/Formats/World/Oprw20.hpp>
#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/Asset/Formats/Material/RvMaterialSource.hpp>
// FAR INSTANCE TIER — proxy dimensions read from a model's ModelInfo alone (no LOD body).
#include <Poseidon/Asset/Formats/P3D/ModelProxyBounds.hpp>
#include <cctype>
#include <Poseidon/Asset/Formats/Material/ShaderSchema.hpp>
#include <stdexcept>
#include <Poseidon/World/Terrain/WrpReader.hpp>
#include <Poseidon/IO/Serialization/SerializeBinExt.hpp>
#include <Poseidon/World/Terrain/TerrainProfile.hpp>

#include <time.h>
#include <cmath>
#include <cstdlib>
#include <string>
#include <cstdio>
#include <stdint.h>
#include <string.h>
#include <utility>
#include <unordered_set>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Common/GamePaths.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Containers/Array2D.hpp>
#include <Poseidon/Foundation/Framework/AppFrame.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Memory/CheckMem.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/LLinks.hpp>
#include <Poseidon/Foundation/Types/Memtype.h>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <Poseidon/Foundation/Types/RemoveLinks.hpp>
#include <Poseidon/Foundation/platform.hpp>
#include <Random/randomGen.hpp>

// #define LAND_TEXTURES_MAX 256
#define LAND_TEXTURES_MAX 512

namespace Poseidon
{
LSError Landscape::LoadData(QIStream& f, float landGrid)
{
    ProgressReset();
    ProgressStart(LocalizeString(IDS_LOAD_WORLD));

    Log("Begin landscape load (%d KB).", Foundation::MemoryUsed() / 1024);

    int i;
    int x, z;

    Init(); // clear the landscape

    _segCache.Clear();

    WorldHeader header;
    f.read((char*)&header, sizeof(header));

    if (f.fail())
    {
        return LSUnknownError;
    }
    int version = 0;
    if (header.magic == FILE_MAGIC_4)
    {
        version = 4;
    }
    else if (header.magic == FILE_MAGIC_3)
    {
        version = 3;
    }
    else if (header.magic == FILE_MAGIC)
    {
        version = 2;
    }
    else
    {
        return LSUnknownError;
    }

    Dim(header.xRange, header.zRange, header.xRange, header.zRange, landGrid);

    FlushCache();

    for (z = 0; z < _landRange; z++)
    {
        for (x = 0; x < _landRange; x++)
        {
            SetTex(x, z, 0);
            SetData(x, z, 0);
        }
    }
    for (z = 0; z < header.zRange; z++)
    {
        for (x = 0; x < header.xRange; x++)
        {
            short val;
            f.read((char*)&val, sizeof(val));
            float data = val * LANDDATA_SCALE;
            if ((x | z) < _landRange)
            {
                SetData(x, z, data);
            }
        }
    }
    for (z = 0; z < header.zRange; z++)
    {
        for (x = 0; x < header.xRange; x++)
        {
            short val;
            f.read((char*)&val, sizeof(val));
            if ((x | z) < _landRange)
            {
                if (val < 0 || val > LAND_TEXTURES_MAX)
                {
                    Fail("Bad texture on landscape");
                    val = 0;
                }
                SetTex(x, z, val);
            }
        }
    }

    ProgressAdd(f.rest());
    ProgressFrame();

    for (i = 0; i < LAND_TEXTURES_MAX; i++)
    {
        char texName[33] = {}; // 32 wire bytes + guaranteed NUL for the strstr / SetTexture C-string use
        f.read(texName, 32);
        if (*texName)
        {
            PoseidonAssert(!strstr(texName, "$$")); // SetTexture(i,"landText\\pi.pac");
            SetTexture(i, texName);
        }
        ProgressSetRest(f.rest());
        ProgressRefresh();
    }

    if (f.fail())
    {
        return LSUnknownError;
    }

    const int maxID = 65536;
    bool isID[maxID];
    memset(isID, 0, sizeof(isID));

    // load all objects
    // bool someForest=false;
    int countObjects = 0;
    int maxObjID = -1;
    for (;;)
    {
        countObjects++;
        if (version < 3)
        {
            SingleObject so;
            f.read(&so, sizeof(so));
            if (f.fail() || f.eof())
            {
                break;
            }
            if (!*so.name)
            {
                break; // object list terminated
            }
            float x = so.x * _landGrid;
            float z = so.z * _landGrid;
            float y = SurfaceY(x, z) + so.y;

            Vector3 oPos(x, y, z);
            AddObject(x, y, z, so.heading, so.name);
        }
        else if (version == 3)
        {
            SingleObject3 so;
            f.read(&so, sizeof(so));
            if (f.fail() || f.eof())
            {
                break;
            }
            if (!*so.name)
            {
                break; // object list terminated
            }
            int id = NewObjectID();
            ObjectCreate(id, so.name, ConvertToM(so.matrix));
        }
        else
        {
            SingleObject4 so;
            f.read(&so, sizeof(so));
            if (f.fail() || f.eof())
            {
                break;
            }
            if (!*so.name)
            {
                break; // object list terminated
            }
            saturateMax(maxObjID, so.id);
            if (so.id < maxID && isID[so.id])
            {
                LOG_DEBUG(World, "Conflict id {}", so.id);
                Object* dup = FindObjectNC(so.id);
                if (dup)
                {
                    Vector3P pos = so.matrix.Position();
                    Fail("Duplicite object id.");
                    RptF("Duplicate1 %s (%.1f,%.1f,%.1f)", so.name, pos.X(), pos.Y(), pos.Z());
                    RptF("Duplicate2 %s (%.1f,%.1f,%.1f)", (const char*)dup->GetShape()->Name(), dup->Position().X(),
                         dup->Position().Y(), dup->Position().Z());
                }
            }
            if (so.id < maxID)
            {
                isID[so.id] = true;
            }
            PoseidonAssert(so.id >= 0);
            ObjectCreate(so.id, so.name, ConvertToM(so.matrix));
        }

        ProgressSetRest(f.rest());
        ProgressRefresh();
    }
    LOG_DEBUG(World, "Total {} objects, max. id {}", countObjects, maxObjID);

    ProgressFrame();

    Log("Landscape loaded. (%d KB).", Foundation::MemoryUsed() / 1024);

    InitGeography();

    ProgressFinish();

    return LSOK;
}

int Landscape::LoadData(const char* name, float landGrid)
{
    DWORD start = Poseidon::Foundation::GlobalTickCount();
    LOG_DEBUG(World, "Landscape::LoadData start");
    Log("Load landscape %s", name);
    _name = name; // remmember name
    LSError ret = LSUnknownError;

    // --test-world substitutes a landscape the mission never asked for. Arma 3
    // worlds are only reachable this way: they are named by CfgWorlds entries
    // this build has no configs for, so nothing would ever route to them.
    const std::string& override = AppConfig::Instance().GetTestWorldPath();
    if (!override.empty())
    {
        Log("--test-world: loading %s instead of %s", override.c_str(), name);
        if (LoadOprwModern(override.c_str()))
        {
            LOG_DEBUG(World, "Landscape::LoadData time {}", Poseidon::Foundation::GlobalTickCount() - start);
            return LSOK;
        }
        Poseidon::Foundation::ErrorMessage("Cannot load --test-world '%s'", override.c_str());
        return LSUnknownError;
    }

    // FIX - allow worlds in banks
    QIFStreamB f;
    f.AutoOpen(name);
    int seekStart = f.tellg();
    if (f.fail())
    {
    }
    else if (LoadOptimized(f, landGrid))
    {
        ret = LSOK;
    }
    else
    {
        f.seekg(seekStart, QIOS::beg);
        ret = LoadData(f, landGrid);
    }
    if (ret != LSOK)
    {
        Poseidon::Foundation::ErrorMessage("Cannot load world '%s'", name);
    }
    LOG_DEBUG(World, "Landscape::LoadData time {}", Poseidon::Foundation::GlobalTickCount() - start);
    return ret;
}

void Landscape::SaveData(QOStream& f) const
{
    int i;
    int x, z;

    // f.read((char *)&temp,sizeof(temp));
    WorldHeader header;

    header.magic = FILE_MAGIC_4;
    header.xRange = _landRange;
    header.zRange = _landRange;
    f.write((char*)&header, sizeof(header));

    for (z = 0; z < _landRange; z++)
    {
        for (x = 0; x < _landRange; x++)
        {
            // short val=toIntFloor(GetData(x,z)/LANDDATA_SCALE);
            short val = HeightToShort(GetData(x, z));
            f.write((char*)&val, sizeof(val));
        }
    }
    for (z = 0; z < _landRange; z++)
    {
        for (x = 0; x < _landRange; x++)
        {
            short val = GetTex(x, z);
            f.write((char*)&val, sizeof(val));
        }
    }

    // save all texture names
    for (i = 0; i < LAND_TEXTURES_MAX; i++)
    {
        char texName[32];
        if (_texture.Size() > i && _texture[i])
        {
            const char* name = _texture[i]->Name();
            if (i == 0)
            {
                name = "LandText\\mo.pac";
            }
            strncpy(texName, name, sizeof(texName));
        }
        else
        {
            *texName = 0;
        }
        f.write(texName, sizeof(texName));
    }

// save all objects
// check for ID duplicate
#define CHECK_ID 1
#if CHECK_ID
    const int maxID = 65536;
    bool isID[maxID];
    memset(isID, 0, sizeof(isID));
#endif
    for (z = 0; z < _landRange; z++)
    {
        for (x = 0; x < _landRange; x++)
        {
            const ObjectList& list = _objects(x, z);
            int i, n = list.Size();
            for (i = 0; i < n; i++)
            {
                Object* o = list[i];
                if (!o)
                {
                    continue;
                }
                if (o->GetType() != Primary && o->GetType() != Network)
                {
                    continue;
                }
                SingleObject4 so;
                so.matrix = ConvertToP(o->Transform());
                strncpy(so.name, o->GetShape()->Name(), sizeof(so.name));
                so.id = o->ID();
#if CHECK_ID
                if (so.id < maxID && isID[so.id])
                {
                    LOG_DEBUG(World, "Conflict id {}", so.id);
                    Object* dup = FindObjectNC(so.id);
                    if (dup)
                    {
                        Vector3P pos = so.matrix.Position();
                        Fail("Duplicite object id.");
                        RptF("Duplicate1 %s (%.1f,%.1f,%.1f)", so.name, pos.X(), pos.Y(), pos.Z());
                        RptF("Duplicate2 %s (%.1f,%.1f,%.1f)", (const char*)dup->GetShape()->Name(),
                             dup->Position().X(), dup->Position().Y(), dup->Position().Z());
                    }
                }
                if (so.id < maxID)
                {
                    isID[so.id] = true;
                }
#endif
                if (*so.name)
                {
                    f.write((char*)&so, sizeof(so));
                }
            }
        }
    }
    {
        SingleObject4 so;
        so.name[0] = 0;
        f.write((char*)&so, sizeof(so));
    }
}
int Landscape::SaveData(const char* name)
{
    QOFStream f(name);
    SaveData(f);
    f.close();
    if (f.fail())
    {
        Poseidon::Foundation::ErrorMessage("Cannot save world '%s'", name);
        return -1;
    }
    return 0;
}

#define PROGRESS       \
    if (f.IsLoading()) \
        ProgressSetRest(f.GetRest()), ProgressRefresh();

} // namespace Poseidon
#include <Poseidon/Foundation/Algorithms/SplineEqD.hpp>
namespace Poseidon
{

// bilinear interpolation, yxz, xf and zf = <0,1>

__forceinline float Bilint(float y00, float y01, float y10, float y11, float xf, float zf)
{
    float y0z = y00 * (1 - zf) + y01 * zf;
    float y1z = y10 * (1 - zf) + y11 * zf;
    return y0z * (1 - xf) + y1z * xf;
}

void Landscape::MakeObjectsTerrainRelative()
{
    // make Y of all objects relative to terrain level
    for (int z = 0; z < _landRange; z++)
    {
        for (int x = 0; x < _landRange; x++)
        {
            const ObjectList& list = _objects(x, z);
            for (int i = 0; i < list.Size(); i++)
            {
                Object* obj = list[i];
                if (!obj)
                {
                    continue;
                }
                if (dyn_cast<ForestPlain>(obj))
                {
                    continue;
                }
                LODShape* shape = obj->GetShape();
                Vector3 pos = obj->Position();
                Vector3 bcWorld = (shape ? obj->PositionModelToWorld(-shape->BoundingCenter()) : pos);
                float surfY = SurfaceY(bcWorld.X(), bcWorld.Z());
                pos[1] -= surfY;
                obj->SetPosition(pos);
                // note: list bounding box may change - we will take care of it later
            }
        }
    }
}
void Landscape::MakeObjectsTerrainAbsolute()
{
    // make Y of all objects back to terrain level
    for (int z = 0; z < _landRange; z++)
    {
        for (int x = 0; x < _landRange; x++)
        {
            const ObjectList& list = _objects(x, z);
            if (list.GetList())
            {
                for (int i = 0; i < list.Size(); i++)
                {
                    Object* obj = list[i];
                    if (!obj)
                    {
                        continue;
                    }
                    if (dyn_cast<ForestPlain>(obj))
                    {
                        continue;
                    }
                    LODShape* shape = obj->GetShape();
                    Vector3 pos = obj->Position();
                    Vector3 bcWorld = (shape ? obj->PositionModelToWorld(-shape->BoundingCenter()) : pos);
                    float surfY = SurfaceY(bcWorld.X(), bcWorld.Z());
                    pos[1] += surfY;
                    obj->SetPosition(pos);
                    // note: we will need to recalc. bbox - see StaticChanged()
                    // GPU-driven retained scene: this terrain-relative re-seat is the ONE mover
                    // of static objects that bypasses MoveObject (every heightfield change —
                    // subdivide/resample/subdiv-cache — funnels through here), so sync the
                    // retained instance now that the FINAL position is set. Relative's
                    // intermediate Y is deliberately not synced.
                    if (GEngine)
                    {
                        GEngine->SceneObjectMoved(obj);
                    }
                }
                list.GetList()->StaticChanged();
            }
        }
    }
}

void Landscape::ResampleTerrain(int sampleStepLog)
{
    int maxResample = _terrainRangeLog - _landRangeLog;
    if (maxResample <= 0)
    {
        return;
    }
    saturateMin(sampleStepLog, maxResample);
    MakeObjectsTerrainRelative();

    const int sampleStep = 1 << sampleStepLog;
    const int resultX = _terrainRange >> sampleStepLog;
    const int resultZ = _terrainRange >> sampleStepLog;
    Array2D<float> result;
    result.Dim(resultX, resultZ);

    for (int x = 0; x < resultX; x++)
    {
        for (int z = 0; z < resultZ; z++)
        {
            result(x, z) = _data(x * sampleStep, z * sampleStep);
        }
    }
    _data = result;
    _terrainRange >>= sampleStepLog;
    _terrainRangeMask = _terrainRange - 1;
    _terrainRangeLog -= sampleStepLog;
    _terrainGrid *= sampleStep;
    _invTerrainGrid = 1 / _terrainGrid;

    MakeObjectsTerrainAbsolute();

    _segCache.Clear();
    if (GScene)
    {
        GScene->GetShadowCache().Clear();
    }
}

template <class Type>
Type GetClipped(Array2D<Type>& array, int x, int y, const Type& defValue)
{
    if (x < 0 || y < 0 || x >= array.GetXRange() || y >= array.GetYRange())
    {
        return defValue;
    }
    return array.Get(x, y);
}

#define DIAG_PROBLEM 0

#if DIAG_PROBLEM
// there are some subdivision problems with road near 5052.75,6708.5

const float ProblemX = 5055.85;
const float ProblemZ = 6707.61;
#endif

inline bool ObjectInside(Object* obj, float xMin, float zMin, float xMax, float zMax, bool diag = false)
{
    LODShape* lShape = obj->GetShape();
    float bRadius = lShape->BoundingSphere();
    Vector3Val pos = obj->Position();
    float xMinO = pos.X() - bRadius, xMaxO = pos.X() + bRadius;
    float zMinO = pos.Z() - bRadius, zMaxO = pos.Z() + bRadius;

    bool ret = (xMaxO >= xMin && xMinO <= xMax && zMaxO >= zMin && zMinO <= zMax);
    if (diag)
    {
        LOG_DEBUG(World, "  {} ({:.1f},{:.1f} - {:.1f}) {}", (const char*)obj->GetDebugName(), pos.X(), pos.Z(),
                  bRadius, ret ? "inside" : "outside");
        LOG_DEBUG(World, "  -- {:.1f},{:.1f} .. {:.1f},{:.1f}   <><> {:.1f},{:.1f} .. {:.1f},{:.1f}", xMin, zMin, xMax,
                  zMax, xMinO, zMinO, xMaxO, zMaxO);
    }
    return ret;
}

static void CheckMinMax(float& min, float& max, float val)
{
    saturateMin(min, val), saturateMax(max, val);
}

void Landscape::SubdivideTerrainOneStep()
{
    // get config class name
    const ParamEntry& cls = Pars >> "CfgWorlds" >> Glob.header.worldname;

    struct Factors
    {
        float rougness;
        float maxRoad;
        float maxTrack;
        float maxSlopeFactor;
    };
    Factors whiteNoise;
    Factors fractal;

    const ParamEntry& subdiv = cls >> "Subdivision";
    const ParamEntry& fractalCls = subdiv >> "Fractal";
    const ParamEntry& whiteNoiseCls = subdiv >> "Fractal";

    whiteNoise.rougness = whiteNoiseCls >> "rougness";
    whiteNoise.maxRoad = whiteNoiseCls >> "maxRoad";
    whiteNoise.maxTrack = whiteNoiseCls >> "maxTrack";
    whiteNoise.maxSlopeFactor = whiteNoiseCls >> "maxSlopeFactor";

    fractal.rougness = fractalCls >> "rougness";
    fractal.maxRoad = fractalCls >> "maxRoad";
    fractal.maxTrack = fractalCls >> "maxTrack";
    fractal.maxSlopeFactor = fractalCls >> "maxSlopeFactor";

    const float minSubdivideY = subdiv >> "minY";
    const float minSubdivideSlope = subdiv >> "minSlope";

    // make relief data more dense
    const int resultX = _terrainRange * 2;
    const int resultZ = _terrainRange * 2;

    // we create result in separate array
    Array2D<float> result;
    // subdivision may be disabled on some rectangles
    Array2D<bool> enableSubdiv;
    // randomness may be disabled on some rectangles
    Array2D<float> enableRandom;

    result.Dim(resultX, resultZ);
    enableSubdiv.Dim(resultX, resultZ);
    enableRandom.Dim(resultX, resultZ);

    int terrainLog = _terrainRangeLog - _landRangeLog;
    int terrainLandStep = (1 << terrainLog);
    int terrainLandStepMask = terrainLandStep - 1;

    // check where is subdivision enabled and where not
    for (int x = 0; x < _terrainRange; x++)
    {
        for (int z = 0; z < _terrainRange; z++)
        {
            // source values
            // float y00 = ClippedData(z,  x);
            // float y01 = ClippedData(z+1,x);
            // float y10 = ClippedData(z,  x+1);
            // float y11 = ClippedData(z+1,x+1);

            int xl = x & ~terrainLandStepMask;
            int zl = z & ~terrainLandStepMask;

            // values in the corners of the landgrid
            float yl00 = ClippedData(zl, xl);
            float yl01 = ClippedData(zl + terrainLandStep, xl);
            float yl10 = ClippedData(zl, xl + terrainLandStep);
            float yl11 = ClippedData(zl + terrainLandStep, xl + terrainLandStep);

            float ylMin = floatMin(floatMin(yl00, yl01), floatMin(yl10, yl11));
            float ylMax = floatMax(floatMax(yl00, yl01), floatMax(yl10, yl11));

            int xg = xl >> terrainLog;
            int zg = zl >> terrainLog;
            GeographyInfo g = _geography(xg, zg);

            bool enable = true;
            float random = 1;

            if (ylMax <= ylMin + minSubdivideSlope * _terrainGrid || ylMin < minSubdivideY || g.u.forestInner ||
                g.u.forestOuter || g.u.waterDepth >= 1)
            {
                enable = false;
                random = 0;
            }
            else
            {
#if DIAG_PROBLEM
                // check if we are near given place
                float xx = x * _terrainGrid, zz = z * _terrainGrid;
                float xe = xx + _terrainGrid, ze = zz + _terrainGrid;

                float dist = sqrt(Square(xx - ProblemX) + Square(zz - ProblemZ));
                bool diag = false;
                if (xx <= ProblemX && xe >= ProblemX && zz <= ProblemZ && ze >= ProblemZ)
                {
                    LOG_DEBUG(World, "Grid {},{} ({:.2f},{:.2f}) *** {:.2f},{:.2f}", x, z, xx, zz, ProblemX, ProblemZ);
                }
                else if (dist < _terrainGrid * 1.5)
                {
                    LOG_DEBUG(World, "Grid {},{} ({:.2f},{:.2f})", x, z, xx, zz);
                    diag = true;
                }
#endif
                bool house = false;
                bool road = false;
                // check if there is some object in (x,z)..(x+1,z+1) terrain grid range
                // that would prevent smoothing or randomness
                float xMin = x * _terrainGrid, xMax = xMin + _terrainGrid;
                float zMin = z * _terrainGrid, zMax = zMin + _terrainGrid;
                for (int xxg = xg - 1; xxg <= xg + 1; xxg++)
                {
                    for (int zzg = zg - 1; zzg <= zg + 1; zzg++)
                    {
                        if (!this_InRange(xxg, zzg))
                        {
                            continue;
                        }
                        const ObjectList& ol = GetObjects(zzg, xxg);
                        if (ol.Size() > 0)
                        {
                            for (int i = 0; i < ol.SizeNotEmpty(); i++)
                            {
                                Object* obj = ol[i];
                                // check if object is in (x,z)..(x+1,z+1)
                                // check if object class makes some problems
                                if (obj->GetType() != Primary && obj->GetType() != Network)
                                {
                                    continue;
                                }
                                LODShape* lShape = obj->GetShape();
                                if (!lShape)
                                {
                                    continue;
                                }
                                // check object type
                                if (dyn_cast<Building>(obj))
                                {
                                    if (!ObjectInside(obj, xMin, zMin, xMax, zMax))
                                    {
                                        continue;
                                    }
                                    house = true;
                                }
                                else if (dyn_cast<Road>(obj))
                                {
#if DIAG_PROBLEM
                                    if (!ObjectInside(obj, xMin, zMin, xMax, zMax, diag))
#else
                                    if (!ObjectInside(obj, xMin, zMin, xMax, zMax))
#endif
                                    {
                                        continue;
                                    }
                                    // no random subdivision under roads
                                    road = true;
                                }
                            }
                        }
                    }
                }
                if (house)
                {
                    enable = false;
                }
                if (road)
                {
                    random = 0;
                    // check for wild terrain, if detected, disable subdivision completely
                    float maxDX = -1e10, maxDZ = -1e10;
                    float minDX = +1e10, minDZ = +1e10;
                    // scan for max and min differentials
                    CheckMinMax(minDZ, maxDZ, ClippedData(z, x) - ClippedData(z - 1, x));
                    CheckMinMax(minDZ, maxDZ, ClippedData(z + 1, x) - ClippedData(z + 1, x));
                    CheckMinMax(minDZ, maxDZ, ClippedData(z + 2, x) - ClippedData(z + 1, x));

                    CheckMinMax(minDZ, maxDZ, ClippedData(z, x + 1) - ClippedData(z - 1, x + 1));
                    CheckMinMax(minDZ, maxDZ, ClippedData(z + 1, x + 1) - ClippedData(z + 1, x + 1));
                    CheckMinMax(minDZ, maxDZ, ClippedData(z + 2, x + 1) - ClippedData(z + 1, x + 1));

                    CheckMinMax(minDX, maxDX, ClippedData(z, x) - ClippedData(z, x - 1));
                    CheckMinMax(minDX, maxDX, ClippedData(z, x + 1) - ClippedData(z, x));
                    CheckMinMax(minDX, maxDX, ClippedData(z, x + 2) - ClippedData(z, x + 1));

                    CheckMinMax(minDX, maxDX, ClippedData(z + 1, x) - ClippedData(z + 1, x - 1));
                    CheckMinMax(minDX, maxDX, ClippedData(z + 1, x + 1) - ClippedData(z + 1, x));
                    CheckMinMax(minDX, maxDX, ClippedData(z + 1, x + 2) - ClippedData(z + 1, x + 1));

                    maxDX *= _invTerrainGrid;
                    maxDZ *= _invTerrainGrid;
                    minDX *= _invTerrainGrid;
                    minDZ *= _invTerrainGrid;

                    if (maxDX - minDX > 1.0f || maxDZ - minDZ > 1.0f)
                    {
                        enable = false;
                    }
                }
            }
            enableSubdiv(x, z) = enable;
            enableRandom(x, z) = random;
        }
    }

    for (int x = 0; x < _terrainRange; x++)
    {
        for (int z = 0; z < _terrainRange; z++)
        {
            // source values
            float ymm = ClippedData(z - 1, x - 1);
            float ym0 = ClippedData(z, x - 1);
            float ym1 = ClippedData(z + 1, x - 1);
            float ym2 = ClippedData(z + 2, x - 1);

            float y0m = ClippedData(z - 1, x);
            float y00 = ClippedData(z, x);
            float y01 = ClippedData(z + 1, x);
            float y02 = ClippedData(z + 2, x);

            float y1m = ClippedData(z - 1, x + 1);
            float y10 = ClippedData(z, x + 1);
            float y11 = ClippedData(z + 1, x + 1);
            float y12 = ClippedData(z + 2, x + 1);

            float y2m = ClippedData(z - 1, x + 2);
            float y20 = ClippedData(z, x + 2);
            float y21 = ClippedData(z + 1, x + 2);
            float y22 = ClippedData(z + 2, x + 2);

            // destination indices
            int xd = x * 2;
            int zd = z * 2;
            // bilinear interpolation

            int xl = x & ~terrainLandStepMask;
            int zl = z & ~terrainLandStepMask;

            float yl00 = ClippedData(zl, xl);
            float yl01 = ClippedData(zl + terrainLandStep, xl);
            float yl10 = ClippedData(zl, xl + terrainLandStep);
            float yl11 = ClippedData(zl + terrainLandStep, xl + terrainLandStep);

            float ylMin = floatMin(floatMin(yl00, yl01), floatMin(yl10, yl11));
            float ylMax = floatMax(floatMax(yl00, yl01), floatMax(yl10, yl11));

            GeographyInfo g = _geography(xl >> terrainLog, zl >> terrainLog);

            const float omega = 1.0f;

            if (!enableSubdiv(x, z))
            {
                // note: bilinear interpolation is not what we want

                result(xd, zd) = y00;
                result(xd + 1, zd) = (y00 + y10) * 0.5f;
                result(xd, zd + 1) = (y00 + y01) * 0.5f;
                // result(xd+1,zd+1) = (y00+y01+y10+y11)*0.25f;
                result(xd + 1, zd + 1) = (y01 + y10) * 0.5f;
                continue;
            }

            // omega can be controlled depending on source geography info
            // omega == 0 is bilinear interpolation
            // omega == 1 is similiar to b-spline interpolation

            const float alpha = -omega * (1.0f / 16);
            const float beta = (8 + omega) * (1.0f / 16);
            const float sigma = alpha * alpha;
            const float mi = alpha * beta;
            const float ni = beta * beta;

            result(xd, zd) = y00;
            // randomize three new points
            // check surface roughness
            Texture* tex = GetTexture(GetTexture(zl >> terrainLog, xl >> terrainLog));
            float fractalRandomness = 1;
            float whiteNoiseRandomness = 1;

            float slope = ylMax - ylMin;
            saturateMax(slope, _terrainGrid * 0.04f);

            if (tex)
            {
                saturateMin(fractalRandomness, tex->Roughness() * fractal.rougness);
                saturateMin(whiteNoiseRandomness, tex->Roughness() * whiteNoise.rougness);
            }
            if (g.u.road)
            {
                saturateMin(fractalRandomness, fractal.maxRoad);
                saturateMin(whiteNoiseRandomness, whiteNoise.maxRoad);
            }
            if (g.u.track)
            {
                saturateMin(fractalRandomness, fractal.maxTrack);
                saturateMin(whiteNoiseRandomness, whiteNoise.maxTrack);
            }

            fractalRandomness *= _terrainGrid / 50;

            saturateMin(fractalRandomness, slope * fractal.maxSlopeFactor);
            saturateMin(whiteNoiseRandomness, slope * whiteNoise.maxSlopeFactor);

            float randomF = enableRandom(x, z);
            float randomness = (fractalRandomness + whiteNoiseRandomness) * randomF;

            // check if neighbourh square allows randomness
            float randomness10 = randomness;
            float randomness01 = randomness;

            saturateMin(randomness10, GetClipped(enableRandom, x, z - 1, 0.0f));
            saturateMin(randomness01, GetClipped(enableRandom, x - 1, z, 0.0f));

            float random10 = (_randGen.RandomValue(xd + 1, zd) - 0.5f) * (2 * randomness10);
            float random01 = (_randGen.RandomValue(xd, zd + 1) - 0.5f) * (2 * randomness01);
            float random11 = (_randGen.RandomValue(xd + 1, zd + 1) - 0.5f) * (2 * randomness);

            // check if each edge can be interpolated
            // this edge is between x,z and x,z-1
            bool neighbourgh0M = GetClipped(enableSubdiv, x, z - 1, true);
            if (neighbourgh0M)
            {
                result(xd + 1, zd) = (y00 + y10) * beta + (y20 + ym0) * alpha + random10;
            }
            else
            {
                result(xd + 1, zd) = (y00 + y10) * 0.5f;
            }
            // this edge is between x,z and x-1,z
            bool neighbourghM0 = GetClipped(enableSubdiv, x - 1, z, true);
            if (neighbourghM0)
            {
                result(xd, zd + 1) = (y00 + y01) * beta + (y02 + y0m) * alpha + random01;
            }
            else
            {
                result(xd, zd + 1) = (y00 + y01) * 0.5f;
            }
            result(xd + 1, zd + 1) =
                ((y00 + y01 + y10 + y11) * ni + (y20 + ym0 + y21 + ym1 + y02 + y0m + y12 + y1m) * mi +
                 (ymm + ym2 + y2m + y22) * sigma + random11);
        }
    }

    // change landscape attributes accordingly
    _data = result;
    _terrainRange <<= 1;
    _terrainRangeMask = _terrainRange - 1;
    _terrainRangeLog += 1;
    _terrainGrid /= 2;
    _invTerrainGrid = 1 / _terrainGrid;
}

void Landscape::SubdivideTerrain(int subdivStepLog)
{
    auto st0 = TerrainProfile::Now();

    MakeObjectsTerrainRelative();

    while (subdivStepLog > 0)
    {
        SubdivideTerrainOneStep();
        subdivStepLog--;
    }

    MakeObjectsTerrainAbsolute();

    _segCache.Clear();
    if (GScene)
    {
        GScene->GetShadowCache().Clear();
    }

    auto elapsed = TerrainProfile::Now() - st0;
    // ~3GHz: cycles/3e6 = ms
    RptF("SubdivideTerrain: %.1f ms, grid=%d range=%d", elapsed / 3e6, _terrainRange, _landRange);
    LOG_DEBUG(Core, "LOAD: SubdivideTerrain {}ms grid={} range={}", elapsed / 3e6, _terrainRange, _landRange);
}

// Subdivision cache file format: magic + version + params + raw heightmap data
static const uint32_t SUBDIV_CACHE_MAGIC = 0x53444356; // "SDCV"
static const uint32_t SUBDIV_CACHE_VERSION = 1;

static std::string GetSubdivCachePath(const char* wrpName, int targetSubdivLog)
{
    // Build cache path from WRP name
    std::string base(wrpName ? wrpName : "unknown");
    // Replace path separators and dots
    for (auto& c : base)
    {
        if (c == '\\' || c == '/' || c == ':')
            c = '_';
    }
    char buf[512];
    const auto& cacheDir = GamePaths::Instance().CacheDir();
    snprintf(buf, sizeof(buf), "%s/cwr_subdiv_%s_L%d.cache", cacheDir.c_str(), base.c_str(), targetSubdivLog);
    return std::string(buf);
}

bool Landscape::LoadSubdivCache(int targetSubdivLog)
{
    std::string path = GetSubdivCachePath(_name, targetSubdivLog);
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;

    uint32_t magic, version;
    int cachedLandRange, cachedLandRangeLog, cachedTerrainRange, cachedTerrainRangeLog;
    float cachedLandGrid, cachedTerrainGrid;

    bool ok = fread(&magic, 4, 1, f) == 1 && magic == SUBDIV_CACHE_MAGIC && fread(&version, 4, 1, f) == 1 &&
              version == SUBDIV_CACHE_VERSION && fread(&cachedLandRange, 4, 1, f) == 1 &&
              fread(&cachedLandRangeLog, 4, 1, f) == 1 && fread(&cachedLandGrid, 4, 1, f) == 1 &&
              fread(&cachedTerrainRange, 4, 1, f) == 1 && fread(&cachedTerrainRangeLog, 4, 1, f) == 1 &&
              fread(&cachedTerrainGrid, 4, 1, f) == 1;

    if (!ok || cachedLandRange != _landRange || cachedLandRangeLog != _landRangeLog || cachedLandGrid != _landGrid)
    {
        fclose(f);
        return false;
    }

    // Adjust objects to be relative before changing terrain
    MakeObjectsTerrainRelative();

    // Resize _data to match cached terrain dimensions
    _data.Dim(cachedTerrainRange, cachedTerrainRange);
    size_t dataBytes = cachedTerrainRange * cachedTerrainRange * sizeof(RawType);
    ok = fread(_data.RawData(), 1, dataBytes, f) == dataBytes;
    fclose(f);

    if (!ok)
    {
        MakeObjectsTerrainAbsolute();
        return false;
    }

    _terrainRange = cachedTerrainRange;
    _terrainRangeMask = _terrainRange - 1;
    _terrainRangeLog = cachedTerrainRangeLog;
    _terrainGrid = cachedTerrainGrid;
    _invTerrainGrid = 1.0f / _terrainGrid;

    // Adjust objects back to absolute with new terrain heights
    MakeObjectsTerrainAbsolute();

    _segCache.Clear();
    if (GScene)
        GScene->GetShadowCache().Clear();

    LOG_DEBUG(Core, "LOAD: SubdivCache HIT from {}", path);
    return true;
}

void Landscape::SaveSubdivCache(int targetSubdivLog)
{
    std::string path = GetSubdivCachePath(_name, targetSubdivLog);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f)
    {
        LOG_WARN(Core, "LOAD: SubdivCache save failed: {}", path);
        return;
    }

    uint32_t magic = SUBDIV_CACHE_MAGIC, version = SUBDIV_CACHE_VERSION;
    fwrite(&magic, 4, 1, f);
    fwrite(&version, 4, 1, f);
    fwrite(&_landRange, 4, 1, f);
    fwrite(&_landRangeLog, 4, 1, f);
    fwrite(&_landGrid, 4, 1, f);
    fwrite(&_terrainRange, 4, 1, f);
    fwrite(&_terrainRangeLog, 4, 1, f);
    fwrite(&_terrainGrid, 4, 1, f);

    size_t dataBytes = _terrainRange * _terrainRange * sizeof(RawType);
    size_t written = fwrite(_data.RawData(), 1, dataBytes, f);
    fclose(f);
    if (written != dataBytes)
    {
        LOG_WARN(Core, "LOAD: SubdivCache write failed ({}/{} bytes), removing {}", written, dataBytes, path);
        remove(path.c_str());
        return;
    }

    LOG_DEBUG(Core, "LOAD: SubdivCache saved ({} bytes) to {}", dataBytes + 32, path);
}

class ObjectRectIndex
{
    FindArray<RStringB> _objectNames;
    Array2D<int> _offset;
    int _endOffset;

  public:
    ObjectRectIndex();
    ~ObjectRectIndex();

    void Dim(int x, int z);
    void SetOffset(int x, int z, int offset) { _offset(x, z) = offset; }
    void SetEndOffset(int offset) { _endOffset = offset; }

    const RStringB& GetObjectName(int i) { return _objectNames[i]; }

    int GetBegOffset(int x, int z) const { return _offset(x, z); }
    int GetEndOffset(int x, int z) const;
    void Reset();
    void Monotonize();
    void TransferNames(SerializeBinStream& f);
};

int ObjectRectIndex::GetEndOffset(int x, int z) const
{
    z++;
    if (z < _offset.GetYRange())
    {
        return _offset(x, z);
    }
    else
    {
        z = 0;
        x++;
        if (x < _offset.GetXRange())
        {
            return _offset(x, z);
        }
        return _endOffset;
    }
}

ObjectRectIndex::ObjectRectIndex() = default;
ObjectRectIndex::~ObjectRectIndex() = default;

void ObjectRectIndex::Dim(int x, int z)
{
    _offset.Dim(x, z);
    // scan
}

void ObjectRectIndex::Reset()
{
    for (int z = 0; z < _offset.GetYRange(); z++)
    {
        for (int x = 0; x < _offset.GetXRange(); x++)
        {
            _offset(x, z) = 0;
        }
    }
}

void ObjectRectIndex::TransferNames(SerializeBinStream& f)
{
    f.TransferBasicArray(_objectNames);
    // check christmas
    time_t t;
    time(&t);
    struct tm* lt = localtime(&t);
    bool christmas = lt->tm_mon == 11 && (lt->tm_mday == 24 || lt->tm_mday == 25);
    if (christmas)
    {
        for (int j = 0; j < _objectNames.Size(); j++)
        {
            if (stricmp(_objectNames[j], "data3d\\str_smrcicicek.p3d") == 0)
            {
                _objectNames[j] = "data3d\\pa_sx.p3d";
            }
        }
    }
}

void ObjectRectIndex::Monotonize()
{
    int lastOffset = 0;
    for (int x = 0; x < _offset.GetXRange(); x++)
    {
        for (int z = 0; z < _offset.GetYRange(); z++)
        {
            int& off = _offset(x, z);
            if (off == 0)
            {
                off = lastOffset;
            }
            else if (off > lastOffset)
            {
                RptF("Object offset not monotone (%d,%d)", x, z);
            }
            else
            {
                lastOffset = off;
            }
        }
    }
    if (lastOffset >= _endOffset)
    {
        RptF("Object offset not monotone - end offset");
    }
}

#define ENABLE_OBJECT_INDEX 0

#if ENABLE_OBJECT_INDEX
ObjectRectIndex index;

void Landscape::LoadObjects(int x, int z)
{
    QIFStreamB in;
    in.AutoOpen(_name);
    SerializeBinStream f(&in);
    int beg = index.GetBegOffset(x, z);
    int end = index.GetEndOffset(x, z);
    if (end == beg)
    {
        // list is empty - no change required
        return;
    }
    f.SeekG(beg);
    ObjectList& ol = _objects(x, z);
    // remove all static objects from the list
    for (int oi = 0; oi < ol.Size(); oi++)
    {
        Object* obj = ol[oi];
        if (obj->GetType() == Primary || obj->GetType() == Network)
        {
            // list is already loaded - no need to load it
            return;
        }
    }
    while (f.TellG() < end && !f.GetError())
    {
        // load object info
        int id = f.LoadInt();
        if (id < 0)
        {
            LOG_ERROR(World, "Terminator reached");
            break;
        }
        int nameIndex = f.LoadInt();
        const RStringB& name = index.GetObjectName(nameIndex);
        Matrix4 trans;
        f.Transfer(trans);
        int xr, zr;
        ObjectCreate(id, name, trans, &xr, &zr);
        DoAssert(x == xr);
        DoAssert(z == zr);
    }
}

void Landscape::ReleaseObjects(int x, int z)
{
    ObjectList& ol = _objects(x, z);
    // remove all static objects from the list
    for (int oi = 0; oi < ol.Size();)
    {
        Object* obj = ol[oi];
        if (obj->GetType() == Primary || obj->GetType() == Network)
        {
            // GPU-driven rendering (docs/gpu-culling-and-depth-plan.md Stage 3b): this cell
            // unload deletes the object directly (not via RemoveObject), so drop it from the
            // retained scene here too or its instance slot + dangling key would leak. No-op
            // unless WGR_GPU_DRIVEN is on.
            if (GEngine)
            {
                GEngine->SceneObjectRemoved(obj);
            }
            ol.Delete(oi);
        }
        else
        {
            oi++;
        }
    }
}

#endif

template <class T, T default_value = 0>
class auto_init
{
  public:
    typedef T value_type;

  private:
    value_type value;

  public:
    // This is the point of the exercise: provide a default constructor
    __forceinline auto_init() : value(default_value) {}

    // These make it close to interchangeable with T
    __forceinline auto_init(const auto_init& other) : value(other.value) {}
    __forceinline explicit auto_init(const value_type& initial_value) : value(initial_value) {}
    __forceinline auto_init& operator=(const auto_init& other)
    {
        value = other.value;
        return *this;
    }
    __forceinline auto_init& operator=(value_type new_value)
    {
        value = new_value;
        return *this;
    }
    __forceinline operator const value_type&() const { return value; }
    __forceinline operator value_type&() { return value; }

    // And these are useful sometimes
    __forceinline const value_type& get() const { return value; }
    __forceinline void set(const value_type& new_value) { value = new_value; }
};

void Landscape::SerializeBin(SerializeBinStream& f, float landGrid)
{
    if (f.IsLoading())
    {
        ProgressReset();
        ProgressStart(LocalizeString(IDS_LOAD_WORLD));
        ProgressAdd(f.GetRest());
        ProgressFrame();
        Init(); // clear the landscape
    }

    Log("Begin landscape load (%d KB).", Foundation::MemoryUsed() / 1024);
#ifdef _MSC_VER
    if (!f.Version('WRPO'))
#else
    if (!f.Version(StrToInt("OPRW")))
#endif
    {
        f.SetError(f.EBadFileType);
        return;
    }
    int version = 3;
    f.Transfer(version);
    if (version < 2 || version > 3)
    {
        Poseidon::Foundation::WarningMessage("Bad version %d in landscape", version);
        f.SetError(f.EBadVersion);
        return;
    }
    if (f.IsLoading())
    {
        if (version >= 3)
        {
            int lx = f.LoadInt();
            int ly = f.LoadInt();
            int rx = f.LoadInt(); // terrain x,y
            int ry = f.LoadInt();
            Dim(lx, ly, rx, ry, landGrid);
        }
        else
        {
            Dim(256, 256, 256, 256, landGrid);
        }
        // set default grid size
        SetLandGrid(50);
        FlushCache();
    }
    else
    {
        if (version >= 3)
        {
            int lx = GetLandRange();
            int ly = GetLandRange();
            int rx = GetLandRange(); // terrain x,y
            int ry = GetLandRange();
            f.SaveInt(lx);
            f.SaveInt(ly);
            f.SaveInt(rx);
            f.SaveInt(ry);
        }
    }
    // transfer all relevant data
    f.TransferBinaryCompressed(_geography.RawData(), _geography.RawSize());
    PROGRESS;
    f.TransferBinaryCompressed(_soundMap.RawData(), _soundMap.RawSize());
    PROGRESS;
    f.TransferBasicArray(_mountains);
    PROGRESS;
    f.TransferBinaryCompressed(_tex.RawData(), _tex.RawSize());
    PROGRESS;
    f.TransferBinaryCompressed(_random.RawData(), _random.RawSize());
    PROGRESS;
    f.TransferBinaryCompressed(_data.RawData(), _data.RawSize());
    PROGRESS;
    // transfer texture index
    if (f.IsLoading())
    {
#if ENABLE_OBJECT_INDEX
        index.Dim(GetLandRange(), GetLandRange());
        index.Reset();
#else
        ObjectRectIndex index;
#endif

        int texCount = f.LoadInt();
        // count off the wire; each entry reads at least a 1-byte (NUL) name below — reject a
        // count the stream cannot back before a huge Resize.
        if (texCount < 0 || texCount > f.GetRest())
        {
            f.SetError(f.EFileStructure);
            return;
        }
        _texture.Resize(texCount);
        for (int i = 0; i < _texture.Size(); i++)
        {
            RStringB name;
            bool enableUV = false;
            f.Transfer(name);
            f.Transfer(enableUV);
            // note: enableUV is not used
            SetTexture(i, name);
        }
        PROGRESS;
        // FindArray<RStringB> objectNames;
        index.TransferNames(f);
        // f.TransferBasicArray(objectNames);
        PROGRESS;

// LOG_DEBUG(World, "Start objects {}",f.GetRest());
//  request all object files to be loaded
#if _ENABLE_CHEATS
        // remmember file position
        int startObjects = f.TellG();

        for (;;)
        {
            Matrix4 trans;
            int id = f.LoadInt();
            if (id < 0)
                break;
            int nameIndex = f.LoadInt();
            const RStringB& name = index.GetObjectName(nameIndex);
            GFileServer->Request(name, 1.0f);
            f.Transfer(trans);
        }

        // rewind file, process requests
        f.SeekG(startObjects);
#endif

#if ENABLE_OBJECT_INDEX
        int lastX = -1, lastZ = -1;
        int terminatorOffset = -1;
#endif

        // AutoArray< auto_init<char> > idUsed;
        AutoArray<InitPtr<Object>> idCache;
        AutoArray<AutoArray<InitPtr<Object>>> conflicts;

        for (;;)
        {
            Matrix4 trans;
#if ENABLE_OBJECT_INDEX
            terminatorOffset = f.TellG();
#endif
            int id = f.LoadInt();
            // id indexes idCache via Access(id), which grows it to id+1 — a huge id off the
            // wire is a multi-GB allocation. Treat an out-of-range id as the terminator.
            if (id < 0 || id > (16 << 20))
            {
                break;
            }

            int nameIndex = f.LoadInt();
            const RStringB& name = index.GetObjectName(nameIndex);
            f.Transfer(trans);
            int x, z;
            Object* obj = ObjectCreate(id, name, trans, &x, &z);

            idCache.Access(id);
            if (idCache[id])
            {
                // conflict detected
                conflicts.Access(id);
                conflicts[id].Add(obj);
            }
            else
            {
                idCache[id] = obj;
            }

            PROGRESS;

#if ENABLE_OBJECT_INDEX
            int offset = f.TellG();

            if (lastX != x || lastZ != z)
            {
                if (lastX > x || lastX == x && lastZ > z)
                {
                    RptF("x,z going back from %d,%d to %d,%d", lastX, lastZ, x, z);
                }
                if (index.GetBegOffset(x, z) != 0)
                {
                    RptF("Bad object segmentation - %d:%s", id, (const char*)name);
                }
                else
                {
                    Log("Start slot %d,%d", x, z);
                    index.SetOffset(x, z, offset);
                }
                lastX = x;
                lastZ = z;
            }
#endif
        }

        int newID = idCache.Size();
        for (int i = 0; i < conflicts.Size(); i++)
        {
            if (conflicts[i].Size() == 0)
            {
                continue;
            }
            Object* o1 = idCache[i];
            for (int j = 0; j < conflicts[i].Size(); j++)
            {
                Object* o2 = conflicts[i][j];
                DoAssert(o2->ID() == o1->ID());
                // swap so that O1 contains valid id
                if (dyn_cast<ForestPlain>(o1))
                {
                    swap(o1, o2);
                }
                // check new ID
                o2->SetID(newID++);
            }
        }
#if ENABLE_OBJECT_INDEX
        index.SetEndOffset(terminatorOffset);
        index.Monotonize();
#endif
        // LOG_DEBUG(World, "End   objects {}",f.GetRest());
    }
    else
    {
        // save texture info
        f.SaveInt(_texture.Size());
        for (int i = 0; i < _texture.Size(); i++)
        {
            Texture* txt = _texture[i].texture;
            RStringB name = txt ? txt->GetName() : "";
            bool enableUV = _texture[i].offsetUV;
            f.Transfer(name);
            f.Transfer(enableUV);
        }
        // transfer object lists
        // consider two pass - object name list and index into this list
        FindArray<RStringB> objectNames;
        for (int x = 0; x < _landRange; x++)
        {
            for (int z = 0; z < _landRange; z++)
            {
                ObjectList& ol = _objects(x, z);
                for (int i = 0; i < ol.Size(); i++)
                {
                    Object* obj = ol[i];
                    if (obj->GetType() != Primary && obj->GetType() != Network)
                    {
                        continue;
                    }
                    RStringB name = obj->GetShape()->GetName();
                    objectNames.AddUnique(name);
                }
            }
        }
        f.TransferBasicArray(objectNames);

        for (int x = 0; x < _landRange; x++)
        {
            for (int z = 0; z < _landRange; z++)
            {
                ObjectList& ol = _objects(x, z);
                for (int i = 0; i < ol.Size(); i++)
                {
                    Object* obj = ol[i];
                    if (obj->GetType() != Primary && obj->GetType() != Network)
                    {
                        continue;
                    }
                    RStringB name = obj->GetShape()->GetName();
                    Matrix4 pos = obj->Transform();
                    f.SaveInt(obj->ID());
                    f.SaveInt(objectNames.Find(name));
                    f.Transfer(pos);
                }
            }
        }
        f.SaveInt(-1); // terminator
    }
    if (f.IsLoading())
    {
        ProgressFinish();
    }
}

bool Landscape::LoadOptimized(QIStream& f, float landGrid)
{
    WrpReader reader;
    if (!reader.Load(f))
    {
        RptF("WrpReader: %s", reader.GetError() ? reader.GetError() : "unknown error");
        return false;
    }

    if (reader.GetFormat() != WrpReader::OPRW_V2 && reader.GetFormat() != WrpReader::OPRW_V3)
    {
        RptF("LoadOptimized: unexpected format %s", reader.GetFormatName());
        return false;
    }

    ProgressReset();
    ProgressStart(LocalizeString(IDS_LOAD_WORLD));
    Init();

    int lx = reader.GetGridX();
    int ly = reader.GetGridZ();
    int rx = reader.GetTerrainX();
    int ry = reader.GetTerrainZ();
    Dim(lx, ly, rx, ry, landGrid);
    SetLandGrid(50);
    FlushCache();

    // Copy raw arrays into Array2D members
    memcpy(_geography.RawData(), reader.GetGeography().Data(), reader.GetGeography().Size());
    memcpy(_soundMap.RawData(), reader.GetSoundMap().Data(), reader.GetSoundMap().Size());
    memcpy(_tex.RawData(), reader.GetTexIndices().Data(), reader.GetTexIndices().Size());
    memcpy(_random.RawData(), reader.GetRandom().Data(), reader.GetRandom().Size());

    // Copy heightmap into _data
    const float* heights = reader.GetHeightmapData();
    for (int z = 0; z < ly; z++)
        for (int x = 0; x < lx; x++)
            SetData(x, z, heights[z * lx + x]);

    // Copy mountains
    _mountains = reader.GetMountains();

    ProgressRefresh();

    // Set up textures
    _texture.Resize(reader.GetTextureCount());
    for (int i = 0; i < reader.GetTextureCount(); i++)
        SetTexture(i, reader.GetTextureName(i));

    ProgressRefresh();

    // Create objects from reader data (names already resolved by WrpReader)
#if _ENABLE_CHEATS
    for (int i = 0; i < reader.GetObjectCount(); i++)
    {
        const auto& obj = reader.GetObject(i);
        GFileServer->Request(obj.name, 1.0f);
    }
#endif

    AutoArray<InitPtr<Object>> idCache;
    AutoArray<AutoArray<InitPtr<Object>>> conflicts;

    for (int i = 0; i < reader.GetObjectCount(); i++)
    {
        const auto& obj = reader.GetObject(i);
        int x, z;
        Object* o = ObjectCreate(obj.id, obj.name, obj.transform, &x, &z);

        idCache.Access(obj.id);
        if (idCache[obj.id])
        {
            conflicts.Access(obj.id);
            conflicts[obj.id].Add(o);
        }
        else
        {
            idCache[obj.id] = o;
        }

        ProgressRefresh();
    }

    // Resolve ID conflicts
    int newID = idCache.Size();
    for (int i = 0; i < conflicts.Size(); i++)
    {
        if (conflicts[i].Size() == 0)
            continue;
        Object* o1 = idCache[i];
        for (int j = 0; j < conflicts[i].Size(); j++)
        {
            Object* o2 = conflicts[i][j];
            DoAssert(o2->ID() == o1->ID());
            if (dyn_cast<ForestPlain>(o1))
                swap(o1, o2);
            o2->SetID(newID++);
        }
    }

    ProgressFinish();
    return true;
}

namespace
{
// FAR INSTANCE TIER — which proxy shape a model gets, decided from its path alone.
//
// The path is all that is available here: the far tier is built from the placement rows
// before any shape is loaded, and loading a few hundred models to ask each one what it is
// would cost more than the entire tier saves. Every shipped world sorts its assets into
// directories by kind, so the path is a reliable classifier and a wrong answer is a
// cosmetic error at kilometres of distance, not a correctness one.
//
// VEGETATION IS TESTED FIRST, deliberately. A misclassified card costs a slightly wrong
// silhouette; a misclassified prism costs ~4x the draw (12 triangles against 2), and the
// classes overlap only in directory names like "buildings\...\bush", never the reverse.
// Anything unrecognised is a card for the same reason.
bool FarProxyIsPrism(const char* path)
{
    if (path == nullptr)
        return false;
    std::string lower(path);
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    static const char* const kVegetation[] = {"vegetation", "plants", "tree", "bush", "shrub", "grass"};
    for (const char* needle : kVegetation)
        if (lower.find(needle) != std::string::npos)
            return false;

    static const char* const kStructures[] = {"structures", "buildings", "building", "industrial",
                                              "military",   "houses",    "house",    "misc"};
    for (const char* needle : kStructures)
        if (lower.find(needle) != std::string::npos)
            return true;

    return false;
}
} // namespace

// Modern OPRW worlds carry two grids: a coarse "land" grid for cell semantics,
// materials and object lists, and a fine "terrain" grid for elevation. Preserve
// that authored split. Landscape's height/surface queries operate in terrain
// space; geography, roads, objects, randomisation and material lookup operate in
// land space. Their world extents remain identical because landGrid is
// terrainGrid * ratio.
bool Landscape::LoadOprwModern(const char* path)
{
    namespace Fmt = Poseidon::Asset::Formats;

    QIFStream file;
    file.open(path);
    if (file.fail())
    {
        RptF("LoadOprwModern: cannot open %s", path);
        return false;
    }

    Fmt::BinaryReader reader(file);
    // Revisions 18 and 20 (Armed Assault), 24 (Arma 2 / OA) and 25 (Arma 3). One
    // probe rather than one per revision: the reader gates the differences on the
    // revision it read, so the caller only has to know that this is a container of
    // that family.
    const int32_t revision = Fmt::World::PeekOprwModernRevision(reader);
    if (revision == 0)
    {
        RptF("LoadOprwModern: %s is not an OPRW revision 18, 20, 24 or 25 world", path);
        return false;
    }

    Fmt::World::Oprw25World world;
    try
    {
        world = Fmt::World::ReadOprwModern(reader, revision);
    }
    catch (const std::exception& e)
    {
        RptF("LoadOprwModern: %s failed to parse: %s", path, e.what());
        return false;
    }

    const int terrain = world.header.terrainRangeX;
    const int land = world.header.landRangeX;
    const float terrainGrid = world.header.TerrainCellSize();
    // Terrain cells per land cell on each axis. Both local ratios are powers of
    // two (8 for Stratis and VR, 4 for Altis); anything else would mean the
    // replication below does not tile the grid exactly.
    const int ratio = terrain / land;
    if (ratio < 1 || land * ratio != terrain)
    {
        RptF("LoadOprwModern: terrain %d is not a whole multiple of land %d", terrain, land);
        return false;
    }

    LOG_DEBUG(World, "LoadOprwModern {} (OPRW {}): land {} terrain {} ratio {} cell {}m extent {}m", path,
              world.header.version, land, terrain, ratio, terrainGrid, world.header.WorldExtent());

    ProgressReset();
    ProgressStart(LocalizeString(IDS_LOAD_WORLD));
    Init();

    const float landGrid = terrainGrid * ratio;
    Dim(land, land, terrain, terrain, landGrid);
    FlushCache();

    // Elevation is already per terrain cell, so it transfers one to one.
    for (int z = 0; z < terrain; ++z)
        for (int x = 0; x < terrain; ++x)
            SetData(x, z, world.elevation[static_cast<size_t>(z) * terrain + x]);
    ProgressRefresh();

    // The per-land-cell grids stay at their authored resolution. Before the
    // dual-grid migration these were replicated ratio^2 times (64x on Stratis),
    // inflating every object/geography/material cell structure to heightmap size.
    //
    // Geography is translated field by field, not copied. The two layouts are not
    // the same 16 bits: Arma 3 carries a min and a max water depth where the
    // engine has one, and the engine has forestInner/forestOuter/track/slow where
    // Arma 3 has a single forest bit and no equivalent for the rest. Blitting the
    // word across would silently land a max-water-depth in the engine's forest and
    // road bits. The fields with no counterpart are left zero rather than guessed;
    // geography drives AI pathing, so a wrong bit here is a wrong path, and none
    // of it affects whether the world draws.
    for (int z = 0; z < land; ++z)
    {
        for (int x = 0; x < land; ++x)
        {
            const size_t landIndex = static_cast<size_t>(z) * land + x;
            const Fmt::World::Oprw25Geography source{world.geography[landIndex]};

            GeographyInfo info;
            info.packed = 0;
            info.u.waterDepth = source.MaxWaterDepth();
            info.u.full = source.Full() ? 1 : 0;
            info.u.road = source.Road() ? 1 : 0;
            info.u.howManyObjects = source.ObjectCount();
            info.u.howManyHardObjects = source.HardObjectCount();
            info.u.gradient = source.Gradient();
            _geography(x, z) = info;

            _tex(x, z) = static_cast<short>(world.materialIndex[landIndex]);
        }
    }
    LOG_INFO(World,
             "Modern world dual grid: land {}x{} at {}m, terrain {}x{} at {}m; semantic cells reduced {}x "
             "versus the compatibility bridge",
             land, land, landGrid, terrain, terrain, terrainGrid, ratio * ratio);
    ProgressRefresh();

    _mountains.Realloc(static_cast<int>(world.mountains.size()));
    _mountains.Resize(static_cast<int>(world.mountains.size()));
    for (size_t i = 0; i < world.mountains.size(); ++i)
        _mountains[static_cast<int>(i)] = Vector3(world.mountains[i].x, world.mountains[i].y, world.mountains[i].z);

    // Terrain surface materials are RVMATs, not the plain textures the OFP path
    // wants: `_texture` holds one texture per cell, while an Arma 3 terrain tile
    // is a satellite layer plus a mask plus detail layers. Registering the RVMAT
    // path itself resolves to nothing and the ground draws white.
    //
    // So each RVMAT is parsed and its satellite colour layer taken as the cell's
    // texture. That is a reduction, not a translation -- the mask and detail
    // layers are dropped, so the ground gets its large-scale colour and none of
    // its close-up detail. It is the honest half of the material that the current
    // single-texture terrain path can actually carry.
    {
        int resolved = 0;
        _texture.Resize(static_cast<int>(world.materials.size()));
        _a3TerrainMaterials.Resize(static_cast<int>(world.materials.size()));
        for (size_t i = 0; i < world.materials.size(); ++i)
        {
            _a3TerrainMaterials[static_cast<int>(i)] = A3TerrainMaterial{};
            const std::string& materialPath = world.materials[i];
            if (materialPath.empty())
                continue;

            QIFStreamB materialFile;
            materialFile.AutoOpen(materialPath.c_str());
            if (materialFile.fail() || materialFile.rest() <= 0)
            {
                // One-shot diagnostic. "0 of 479 resolved" says nothing about
                // whether the path is wrong or the archive is absent, and those
                // want opposite fixes.
                static bool reported = false;
                if (!reported)
                {
                    reported = true;
                    // "0 of N resolved" says nothing about whether the path is
                    // wrong, the archive is absent, or the archive is mounted
                    // somewhere nobody looks -- and those want opposite fixes.
                    LOG_WARN(World, "LoadOprw25: cannot open terrain material '{}' (FileExist={}, bank={}, {} banks)",
                             materialPath, QIFStreamB::FileExist(materialPath.c_str()),
                             QIFStreamB::AutoBank(materialPath.c_str()) != nullptr, GFileBanks.Size());
                }
                continue;
            }

            std::vector<uint8_t> bytes(static_cast<size_t>(materialFile.rest()));
            materialFile.read(bytes.data(), static_cast<int>(bytes.size()));

            Poseidon::Asset::Material::RvMaterialSource material;
            try
            {
                material = Poseidon::Asset::Material::ParseArmaRapMaterial(bytes, materialPath);
            }
            catch (const std::exception&)
            {
                continue;
            }

            // The terrain families are authored compositions, not generic object
            // materials: a satellite image, a selector mask, then a stack of
            // ground surfaces. All three generations are described by one family
            // record (Asset::Material::FindTerrainFamily) rather than by stage
            // arithmetic inlined here, because Arma 1 differs from the later two
            // in four ways at once -- three stages per group instead of two, a
            // first surface at Stage2 instead of Stage4, a mask that shares the
            // satellite's `_lco` suffix, and a slot assignment that comes from
            // the shader id rather than from the stage position.
            //
            //   Arma 1       Terrain1..15  Stage2..Stage13 = four surfaces
            //   Arma 2 / OA  TerrainX      Stage4..Stage14 = six surfaces
            //   Arma 3       TerrainSNX    Stage4..Stage12 = five surfaces,
            //                              Stage14 = the whole-tile normal map.
            //
            // Preserve the exact references here; TerrainWgpu uploads and
            // composites them instead of inventing a cell-boundary fade.
            Poseidon::Asset::Material::TerrainFamily family;
            const bool isTerrainFamily =
                Poseidon::Asset::Material::FindTerrainFamily(material.pixelShaderId, family);
            A3TerrainMaterial& authored = _a3TerrainMaterials[static_cast<int>(i)];
            const auto uvForStage = [&](const Poseidon::Asset::Material::RvStage& stage)
            {
                const auto* transform = &stage.uvTransform;
                const std::string* uvSource = &stage.uvSource;
                if (!transform->present && stage.texGen >= 0)
                {
                    for (const auto& texGen : material.texGens)
                    {
                        if (texGen.index == stage.texGen)
                        {
                            transform = &texGen.uvTransform;
                            uvSource = &texGen.uvSource;
                            break;
                        }
                    }
                }
                // `uvTransform` is a basis, not a pair of row vectors: aside/up/dir
                // are where the source's X/Y/Z axes GO, so the u channel is the
                // first component of each of the three, not all three components
                // of `aside`. Transposing it here keeps the shader a plain dot
                // product.
                //
                // Reading it row-wise happens to work for the `tex` stages, whose
                // matrices are diagonal, and silently destroys every `worldPos`
                // stage: Stratis's satellite TexGen is aside=(k,0,0) up=(0,0,k)
                // dir=(0,-k,0), so the row reading takes v from the source's third
                // component -- the terrain HEIGHT -- instead of from world Z. The
                // satellite then smears along the slope and the LCA mask, which
                // shares that TexGen, selects one surface for the whole tile.
                A3TerrainUv uv;
                uv.u = {transform->aside[0], transform->up[0], transform->dir[0], transform->pos[0]};
                uv.v = {transform->aside[1], transform->up[1], transform->dir[1], transform->pos[1]};
                uv.worldPos = *uvSource == "worldPos";
                return uv;
            };
            // A stage's texture, or an empty string when it is absent, empty or
            // procedural. Reading by index rather than iterating is what lets the
            // group layout be data (the family record) instead of control flow.
            const auto textureOfStage = [&](int index) -> std::string
            {
                for (const auto& candidate : material.stages)
                {
                    if (candidate.index != index)
                        continue;
                    const std::string& text = candidate.texture.raw;
                    if (text.empty() || Poseidon::Asset::Material::RvTextureRef::LooksProcedural(text))
                        return std::string();
                    return text;
                }
                return std::string();
            };
            const auto stageAt = [&](int index) -> const Poseidon::Asset::Material::RvStage*
            {
                for (const auto& candidate : material.stages)
                    if (candidate.index == index)
                        return &candidate;
                return nullptr;
            };

            if (isTerrainFamily)
            {
                // The satellite is Stage0 by position in all three generations,
                // and all three name it `s_<x>_<y>_lco.paa`. Position decides,
                // never the suffix: Arma 1 gives the MASK the same `_lco` suffix,
                // so a suffix test binds the mask as a second satellite.
                const std::string satellite = textureOfStage(family.satelliteStage);
                if (!satellite.empty())
                {
                    authored.satellite = RStringB(satellite.c_str());
                    if (const auto* stage = stageAt(family.satelliteStage))
                        authored.satelliteUv = uvForStage(*stage);
                }
                const std::string mask = textureOfStage(family.maskStage);
                if (!mask.empty())
                {
                    authored.mask = RStringB(mask.c_str());
                    if (const auto* stage = stageAt(family.maskStage))
                        authored.maskUv = uvForStage(*stage);
                }
                // Arma 3's whole-tile normal. Arma 2 spends the same stage on its
                // sixth surface and Arma 1 has no such stage at all, so this is
                // gated on the family rather than on the stage index alone.
                if (family.tileNormalStage > 0)
                {
                    const std::string tileNormal = textureOfStage(family.tileNormalStage);
                    if (!tileNormal.empty())
                        authored.tileNormal = RStringB(tileNormal.c_str());
                }

                // The surface stack. `SlotOfGroup` is the identity for the later
                // generations -- their groups sit at their own slot's stage and a
                // hole is an empty stage that keeps its place -- and the set-bit
                // lookup for Arma 1, whose groups are packed and whose shader id
                // names the slots they occupy.
                const int groups = family.GroupCount();
                for (int group = 0; group < groups; ++group)
                {
                    const int slot = family.SlotOfGroup(group);
                    if (slot < 0 || static_cast<size_t>(slot) >= authored.surfaces.size())
                        continue;
                    const int base = family.firstSurfaceStage + group * family.stageStride;
                    const std::string colour = textureOfStage(base + family.colourOffset);
                    if (colour.empty())
                        continue;
                    authored.surfaces[static_cast<size_t>(slot)] = RStringB(colour.c_str());
                    const std::string normal = textureOfStage(base + family.normalOffset);
                    if (!normal.empty())
                        authored.surfaceNormals[static_cast<size_t>(slot)] = RStringB(normal.c_str());
                    if (const auto* stage = stageAt(base + family.colourOffset))
                        authored.surfaceUvs[static_cast<size_t>(slot)] = uvForStage(*stage);
                    authored.surfaceCount =
                        std::max(authored.surfaceCount, static_cast<unsigned char>(slot + 1));
                }
            }

            // NOT the Stage0 satellite, even though it is now captured in the IR.
            // The satellite is one low-frequency image per 1024 m tile with its
            // own worldPos TexGen; the legacy slot is sampled through the land
            // cell's frame, so binding it here tiles a whole tile's image into
            // every 50 m cell -- measured on Takistan as a corduroy pattern over
            // the entire map. It belongs to the authored path, which carries the
            // TexGen that makes it mean anything.
            //
            // The mask is `_lca` and the detail layers are `_nopx`/`_co` pairs.
            // Taking the first `_co` that is not a detail layer picks a close-range
            // surface, which is what the legacy cell frame can actually sample.
            //
            // Arma 1 needs the family record for this too, and for a reason that
            // cost a whole world: EVERY Sahrani surface colour is named
            // `<surface>_detail_co.paa`, so the "not a detail layer" filter -- which
            // exists to skip Arma 2/3's `_detail_*` helpers -- rejected all four of
            // them, on all 4,859 tiles. No material resolved a legacy texture and
            // the terrain drew white. Take slot 0's colour directly from the family
            // instead of pattern-matching a filename.
            if (isTerrainFamily && family.slotsFromShaderId)
            {
                const int slot0Group = 0;
                const int base = family.firstSurfaceStage + slot0Group * family.stageStride;
                const std::string colour = textureOfStage(base + family.colourOffset);
                if (!colour.empty())
                {
                    SetTexture(static_cast<int>(i), RStringB(colour.c_str()));
                    ++resolved;
                }
            }
            // The authored pass above already worked out which stage is slot 0's
            // colour, from the family record. If it found one, that IS the answer --
            // no filename needs interrogating. Preferring it costs nothing and closes
            // the failure the `_co.paa` filter below keeps producing on each new
            // generation: DayZ names every terrain surface colour `_ca.paa`
            // (`en_grass1_ca.paa`, `en_soil_ca.paa`, ...), so the filter matched
            // nothing on any of Enoch's 9,432 materials and the legacy slot stayed
            // null -- white ground on GL33, and on WGPU a silent single point of
            // failure the moment the authored path is off.
            //
            // That is the third generation to break the same way: Sahrani's
            // `_detail_co` (see above) and now DayZ's `_ca`. The lesson the comment
            // above draws -- take the colour from the family, not from the name -- is
            // simply applied to both branches here.
            else if (authored.surfaceCount > 0 && authored.surfaces[0].GetLength() != 0)
            {
                SetTexture(static_cast<int>(i), authored.surfaces[0]);
                ++resolved;
            }
            else
            {
                for (const auto& stage : material.stages)
                {
                    const std::string& texture = stage.texture.raw;
                    if (texture.empty() || Poseidon::Asset::Material::RvTextureRef::LooksProcedural(texture))
                        continue;
                    std::string lower = texture;
                    for (char& c : lower)
                        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                    if (lower.find("_co.paa") == std::string::npos || lower.find("detail") != std::string::npos)
                        continue;
                    SetTexture(static_cast<int>(i), RStringB(texture.c_str()));
                    ++resolved;
                    break;
                }
            }
        }
        LOG_DEBUG(World, "LoadOprw25 {}: {} of {} terrain materials resolved to a satellite texture", path, resolved,
                  world.materials.size());
        // Authored coverage, per field, so a generation that stops supplying one is
        // visible as a number rather than as a look nobody can attribute.
        int withSatellite = 0, withMask = 0, withSurfaceNormals = 0, withTileNormal = 0;
        int surfaces = 0, holes = 0, sixSlot = 0;
        for (int m = 0; m < _a3TerrainMaterials.Size(); ++m)
        {
            const A3TerrainMaterial& authored = _a3TerrainMaterials[m];
            withSatellite += authored.satellite.GetLength() != 0;
            withMask += authored.mask.GetLength() != 0;
            withTileNormal += authored.tileNormal.GetLength() != 0;
            sixSlot += authored.surfaceCount > 5;
            for (unsigned char s = 0; s < authored.surfaceCount; ++s)
            {
                // A hole is a slot the source left empty below a slot it filled.
                // It must survive to the GPU: the mask selects by slot, so
                // closing the gap would renumber every surface after it.
                if (authored.surfaces[s].GetLength() == 0)
                {
                    ++holes;
                    continue;
                }
                ++surfaces;
                withSurfaceNormals += authored.surfaceNormals[s].GetLength() != 0;
            }
        }
        LOG_INFO(World,
                 "Terrain materials {}: {} satellite, {} mask, {} surfaces ({} holes preserved, {} using the sixth "
                 "slot), {} surface normals, {} tile normal, of {}",
                 path, withSatellite, withMask, surfaces, holes, sixSlot, withSurfaceNormals, withTileNormal,
                 _a3TerrainMaterials.Size());
    }
    ProgressRefresh();

    // Retain modern placements as compact authored rows and create only a
    // camera-centred working set. The opt-out is diagnostic: it preserves the
    // former eager behaviour for A/B captures without making it the default.
    const char* streamEnv = std::getenv("WGR_OBJECT_STREAMING");
    const bool streamObjects = streamEnv == nullptr || std::strcmp(streamEnv, "0") != 0;
    if (streamObjects)
    {
        const int maxObjects = AppConfig::Instance().GetTestWorldMaxObjects();
        const size_t placementLimit = maxObjects > 0 ? std::min(world.objects.size(), static_cast<size_t>(maxObjects))
                                                     : world.objects.size();
        _modernObjectModels.clear();
        _modernObjectModels.reserve(world.models.size());
        for (const std::string& model : world.models)
            _modernObjectModels.emplace_back(model.c_str());
        _modernObjectPlacements.clear();
        _modernObjectPlacements.reserve(placementLimit);
        _modernObjectCells.assign(static_cast<size_t>(_landRange) * _landRange, {});
        _modernObjectCellActive.assign(_modernObjectCells.size(), 0);
        _modernObjectCellDesired.assign(_modernObjectCells.size(), 0);
        _modernObjectCellCursor.assign(_modernObjectCells.size(), 0);

        if (const char* radius = std::getenv("WGR_OBJECT_STREAM_RADIUS_CELLS"))
            _modernObjectRadius = std::clamp(static_cast<int>(std::strtol(radius, nullptr, 10)), 8, _landRange);
        if (const char* budget = std::getenv("WGR_OBJECT_STREAM_MAX_OBJECTS"))
            _modernObjectBudget = static_cast<uint32_t>(
                std::clamp<long>(std::strtol(budget, nullptr, 10), 1000, 1000000));
        else
            _modernObjectBudget = 20000;
        // Diagnostic opt-out, in the same spirit as WGR_OBJECT_STREAMING=0: pin the window at
        // the configured radius so the pre-growth coverage and memory can be measured from the
        // same binary. Not a gameplay setting -- with it set, a world smaller than the budget
        // loads only the part of itself that happens to fall inside the radius.
        if (const char* growth = std::getenv("WGR_OBJECT_STREAM_WINDOW_GROWTH"))
            _modernObjectWindowGrowth = std::strcmp(growth, "0") != 0;
        if (const char* batch = std::getenv("WGR_OBJECT_STREAM_CREATES_PER_UPDATE"))
            _modernObjectCreatesPerUpdate = static_cast<uint32_t>(
                std::clamp<long>(std::strtol(batch, nullptr, 10), 1, 100000));
        else
            _modernObjectCreatesPerUpdate = 128;

        int highestId = -1;
        for (size_t i = 0; i < placementLimit; ++i)
            highestId = std::max(highestId, world.objects[i].objectId);
        int replacementId = highestId + 1;
        std::unordered_set<int> usedIds;
        usedIds.reserve(placementLimit);
        size_t reassigned = 0;
        for (size_t i = 0; i < placementLimit; ++i)
        {
            const auto& source = world.objects[i];
            ModernObjectPlacement placement;
            placement.id = source.objectId;
            if (!usedIds.insert(placement.id).second)
            {
                while (!usedIds.insert(replacementId).second)
                    ++replacementId;
                placement.id = replacementId++;
                ++reassigned;
            }
            placement.modelIndex = static_cast<uint32_t>(source.modelIndex);
            for (size_t row = 0; row < 4; ++row)
            {
                placement.rows[row * 3 + 0] = source.transform.rows[row].x;
                placement.rows[row * 3 + 1] = source.transform.rows[row].y;
                placement.rows[row * 3 + 2] = source.transform.rows[row].z;
            }
            const int x = std::clamp(static_cast<int>(std::floor(placement.rows[9] * _invLandGrid)), 0,
                                     _landRange - 1);
            const int z = std::clamp(static_cast<int>(std::floor(placement.rows[11] * _invLandGrid)), 0,
                                     _landRange - 1);
            const uint32_t index = static_cast<uint32_t>(_modernObjectPlacements.size());
            _modernObjectPlacements.push_back(std::move(placement));
            _modernObjectCells[static_cast<size_t>(z) * _landRange + x].push_back(index);
            if ((i & 0x3fff) == 0)
                ProgressRefresh();
        }
        _modernObjectStreaming = true;
        _modernObjectResidencyPending = false;
        _modernObjectCenterX = -0x3fffffff;
        _modernObjectCenterZ = -0x3fffffff;
        _modernResidentObjectCount = 0;

        // FAR INSTANCE TIER — one cheap proxy per placement, built ONCE, here, because this
        // is the only point at which the complete authored placement list exists.
        //
        // The residency window above is a working set, not a draw distance: it holds ~20,000
        // objects inside ~900 m on Everon and ~550-711 m on Chernarus, and everything outside
        // it is not a coarse object but NO object. That is why distant worlds render as bare
        // ground and why assets appear out of nothing as you fly. This tier answers only the
        // rendering half of that: it never creates an Object, never simulates, and never
        // enters the residency accounting.
        //
        // Cost control is the per-MODEL cache. A world names a few hundred distinct models and
        // places them a few million times, so the proxy dimensions are read once per model
        // from its P3D header alone (ReadModelProxyBounds touches ModelInfo and no LOD body)
        // and then indexed per placement.
        if (GEngine != nullptr)
        {
            const auto farStart = std::chrono::steady_clock::now();
            struct FarModelProxy
            {
                float height = 0.0f;
                float baseY = 0.0f; // bbox min Y: the offset from the model origin to its foot
                float radius = 0.0f;
                uint32_t colour = 0xFFFFFFFFu;
                uint32_t flags = 0;
                bool valid = false;
            };
            std::vector<FarModelProxy> perModel(_modernObjectModels.size());
            size_t modelsRead = 0, modelsSkipped = 0;
            for (size_t m = 0; m < _modernObjectModels.size(); ++m)
            {
                const char* name = _modernObjectModels[m].GetLength() > 0 ? (const char*)_modernObjectModels[m]
                                                                          : nullptr;
                if (name == nullptr || !QIFStreamB::FileExist(name))
                {
                    ++modelsSkipped;
                    continue;
                }
                QIFStreamB stream;
                stream.AutoOpen(name);
                if (stream.fail())
                {
                    ++modelsSkipped;
                    continue;
                }
                Fmt::BinaryReader reader(stream);
                const Fmt::P3D::ModelProxyBounds bounds = Fmt::P3D::ReadModelProxyBounds(reader);
                // valid == false covers OFP-era ODOL 7 (whose ModelInfo sits behind every LOD
                // body, so reading it is not a header read at all), non-ODOL containers and
                // any header that decoded to nonsense. A proxy sized from any of those is
                // worse than no proxy, so the model simply gets none.
                if (!bounds.valid)
                {
                    ++modelsSkipped;
                    continue;
                }
                FarModelProxy& proxy = perModel[m];
                proxy.height = bounds.maxY - bounds.minY;
                proxy.baseY = bounds.minY;
                proxy.radius = bounds.horizontalExtent * 0.5f;
                proxy.colour = bounds.color;
                proxy.flags = FarProxyIsPrism(name) ? 1u : 0u;
                // A model with no vertical extent has no silhouette to stand in for.
                proxy.valid = proxy.height > 0.05f;
                if (proxy.valid)
                    ++modelsRead;
                else
                    ++modelsSkipped;
            }

            std::vector<Engine::FarProxyInstance> farInstances;
            farInstances.reserve(_modernObjectPlacements.size());
            size_t prisms = 0;
            for (const ModernObjectPlacement& placement : _modernObjectPlacements)
            {
                if (placement.modelIndex >= perModel.size())
                    continue;
                const FarModelProxy& proxy = perModel[placement.modelIndex];
                if (!proxy.valid)
                    continue;
                Engine::FarProxyInstance instance;
                instance.x = placement.rows[9];
                // The proxy stands ON the ground, so its origin is the model's bbox FOOT, not
                // the model origin. Those differ by minY, which is nonzero for a great many
                // assets — ignoring it sinks or floats every proxy by up to a few metres, and
                // a floating forest is the one artefact a distance tier cannot get away with.
                instance.y = placement.rows[10] + proxy.baseY;
                instance.z = placement.rows[11];
                instance.height = proxy.height;
                instance.radius = proxy.radius;
                instance.colour = proxy.colour;
                instance.flags = proxy.flags;
                prisms += (proxy.flags != 0);
                farInstances.push_back(instance);
            }
            GEngine->SetFarProxyInstances(farInstances.data(), farInstances.size());
            const auto farMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - farStart)
                                   .count();
            // Report the DENOMINATOR, not just the successes: proxies built against
            // placements retained is the number that says whether the far world is complete,
            // and models skipped names the reason when it is not.
            LOG_INFO(World,
                     "Far instance tier: {} proxies ({} prisms, {} cards) from {} placements; {} of {} models "
                     "read, {} skipped; {} ms",
                     farInstances.size(), prisms, farInstances.size() - prisms, _modernObjectPlacements.size(),
                     modelsRead, _modernObjectModels.size(), modelsSkipped, farMs);
        }

        LOG_INFO(World,
                 "Modern object streaming: retained {} compact placements in {} land cells; radius={} cells, "
                 "resident budget={}, create batch={}, duplicate ids reassigned={}",
                 _modernObjectPlacements.size(), _modernObjectCells.size(), _modernObjectRadius,
                 _modernObjectBudget, _modernObjectCreatesPerUpdate, reassigned);
        ProgressFinish();
        return true;
    }

    // Keyed by object id, not indexed by it. DayZ ids are sparse handles --
    // Chernarus runs 0x01000000..654,367,047 for 2,971,218 placements -- so an
    // array addressed by id would reserve 5.2 GB to hold 23 MB of pointers, and
    // died in Realloc long before that. Every world up to Arma 3 numbers its
    // objects densely from zero, which is why this held until now.
    std::unordered_map<int, Object*> idCache;
    std::unordered_map<int, std::vector<Object*>> conflicts;
    int highestId = -1;

    int created = 0;
    int refused = 0;
    // Triage lever (--test-world-max-objects). Chernarus places 2,971,218 objects,
    // 18x Stratis; capping is how you tell a scale limit from a compatibility one.
    const int maxObjects = AppConfig::Instance().GetTestWorldMaxObjects();
    for (const auto& placement : world.objects)
    {
        if (maxObjects > 0 && created >= maxObjects)
            break;
        const RStringB name(world.models[placement.modelIndex].c_str());

        // Rows are [aside, up, dir, pos] in both representations, which is why
        // this is a straight construction and not a basis change.
        const auto& rows = placement.transform.rows;
        Matrix4 transform;
        transform.SetDirectionAside(Vector3(rows[0].x, rows[0].y, rows[0].z));
        transform.SetDirectionUp(Vector3(rows[1].x, rows[1].y, rows[1].z));
        transform.SetDirection(Vector3(rows[2].x, rows[2].y, rows[2].z));
        transform.SetPosition(Vector3(rows[3].x, rows[3].y, rows[3].z));

        int x = 0, z = 0;
        Object* object = nullptr;
        try
        {
            // OPRW25 placements encode final world elevation. Do not apply the
            // legacy bounds-centre grounding adjustment used by OFP islands.
            object = ObjectCreate(placement.objectId, name, transform, &x, &z,
                                  /* avoidRecalculation = */ false,
                                  /* preserveAuthoredElevation = */ true);
        }
        catch (const std::exception&)
        {
            // One unreadable model must not cost the whole world. The census
            // reports which models parse; this is the runtime equivalent.
            ++refused;
            continue;
        }
        if (!object)
        {
            ++refused;
            continue;
        }
        ++created;

        const int placedId = placement.objectId;
        if (placedId > highestId)
            highestId = placedId;
        const auto slot = idCache.find(placedId);
        if (slot != idCache.end() && slot->second)
            conflicts[placedId].push_back(object);
        else
            idCache[placedId] = object;

        if ((created & 0x3ff) == 0)
            ProgressRefresh();
    }

    // Ids handed to duplicates must miss every authored id, so continue past the
    // highest one seen rather than past the container's size.
    int newID = highestId + 1;
    for (auto& entry : conflicts)
    {
        Object* o1 = idCache[entry.first];
        for (Object* o2 : entry.second)
        {
            if (dyn_cast<ForestPlain>(o1))
                swap(o1, o2);
            o2->SetID(newID++);
        }
    }

    LOG_DEBUG(World, "LoadOprw25 {}: {} objects created, {} refused, of {} placements", path, created, refused,
              world.objects.size());

    ProgressFinish();
    return true;
}

// Wall-clock budget for one residency admission pass, in milliseconds. See the
// commentary at the admission loop for why this is time and not a count.
// WGR_OBJECT_STREAM_MS_PER_UPDATE=0 pins admission to the legacy per-frame batch,
// so both arms of an A/B come from one binary.
static float ObjectStreamBudgetMs()
{
    static const float ms = []
    {
        const char* v = std::getenv("WGR_OBJECT_STREAM_MS_PER_UPDATE");
        const float parsed = v ? static_cast<float>(std::atof(v)) : 4.0f;
        return parsed >= 0.0f ? parsed : 4.0f;
    }();
    return ms;
}

// Hard ceiling on the time budget, as a multiple of the legacy batch. Each create is
// a synchronous model decode plus texture transcode, so an unbounded budget on a
// stalled frame would trade pop-in for a much worse hitch.
static uint32_t ObjectStreamMaxBatchScale()
{
    static const uint32_t scale = []
    {
        const char* v = std::getenv("WGR_OBJECT_STREAM_MAX_BATCH_SCALE");
        const long parsed = v ? std::strtol(v, nullptr, 10) : 12;
        return static_cast<uint32_t>(std::clamp<long>(parsed, 1, 256));
    }();
    return scale;
}

void Landscape::UpdateModernObjectResidency(Vector3Par cameraPosition)
{
    if (!_modernObjectStreaming || _modernObjectCells.empty())
        return;
    Vector3 effectivePosition = cameraPosition;
    // Deterministic residency-turnover smoke hook. It visits a non-overlapping
    // cell window after 180 updates and returns after 360, while the real camera
    // stays fixed so the final screenshot remains directly comparable. Normal
    // gameplay never reads this path unless the explicit diagnostic env is set.
    if (const char* sweep = std::getenv("WGR_OBJECT_STREAM_TEST_SWEEP_CELLS"))
    {
        const int offsetCells = std::clamp(static_cast<int>(std::strtol(sweep, nullptr, 10)), 1, _landRange);
        const int phase = _modernObjectTestUpdate < 180 ? 0 : (_modernObjectTestUpdate < 360 ? 1 : 2);
        ++_modernObjectTestUpdate;
        if (phase == 1)
        {
            effectivePosition[0] += offsetCells * _landGrid;
            effectivePosition[2] += offsetCells * _landGrid;
        }
        if (phase != _modernObjectTestPhase)
        {
            _modernObjectTestPhase = phase;
            LOG_INFO(World, "Modern object residency test sweep: phase={} offsetCells={}", phase,
                     phase == 1 ? offsetCells : 0);
        }
    }
    constexpr int QuantizeCells = 4;
    int centerX = std::clamp(static_cast<int>(std::floor(effectivePosition.X() * _invLandGrid)), 0, _landRange - 1);
    int centerZ = std::clamp(static_cast<int>(std::floor(effectivePosition.Z() * _invLandGrid)), 0, _landRange - 1);
    centerX = (centerX / QuantizeCells) * QuantizeCells;
    centerZ = (centerZ / QuantizeCells) * QuantizeCells;
    const bool centerChanged = centerX != _modernObjectCenterX || centerZ != _modernObjectCenterZ;
    if (!centerChanged && !_modernObjectResidencyPending)
        return;
    if (centerChanged)
    {
        _modernObjectCenterX = centerX;
        _modernObjectCenterZ = centerZ;
    }

    struct CellCandidate
    {
        uint32_t index;
        uint32_t distance2;
    };
    // The live set must be bounded by the OBJECT BUDGET, not by geometry. The radius is a
    // second, independent bound, and on any world whose whole placement database already fits
    // the budget it is the one that binds -- silently, because nothing reports it. Reforger's
    // Everon carries 19,644 placements against a 20,000-object budget, but its land grid is
    // 256x256 at 50 m, so a fixed 64-cell window spans 6.4 km of a 12.8 km world and enclosed
    // only 9,176 of them. 10,468 objects were refused for want of nothing: half the island
    // stayed empty while 10,824 budget slots went unused. Grow the window geometrically until
    // the budget is the bound or the window covers the map, so the radius is a starting size
    // rather than a ceiling. Worlds where the budget already binds (Stratis requests 19,998 of
    // 163,953 inside the first 64 cells, ChernarusPlus far sooner) never enter the loop and
    // keep their measured behaviour and memory exactly.
    const auto enclosedPlacements = [&](int radius)
    {
        const int ex0 = std::max(0, centerX - radius);
        const int ez0 = std::max(0, centerZ - radius);
        const int ex1 = std::min(_landRange, centerX + radius + 1);
        const int ez1 = std::min(_landRange, centerZ + radius + 1);
        uint64_t total = 0;
        for (int z = ez0; z < ez1 && total < _modernObjectBudget; ++z)
            for (int x = ex0; x < ex1; ++x)
                total += _modernObjectCells[static_cast<size_t>(z) * _landRange + x].size();
        return total;
    };
    int windowRadius = std::clamp(_modernObjectRadius, 1, _landRange);
    if (_modernObjectWindowGrowth)
        while (windowRadius < _landRange && enclosedPlacements(windowRadius) < _modernObjectBudget)
            windowRadius = std::min(_landRange, windowRadius * 2);

    std::vector<CellCandidate> candidates;
    const int x0 = std::max(0, centerX - windowRadius);
    const int z0 = std::max(0, centerZ - windowRadius);
    const int x1 = std::min(_landRange, centerX + windowRadius + 1);
    const int z1 = std::min(_landRange, centerZ + windowRadius + 1);
    candidates.reserve(static_cast<size_t>(x1 - x0) * (z1 - z0));
    for (int z = z0; z < z1; ++z)
    {
        for (int x = x0; x < x1; ++x)
        {
            const uint32_t index = static_cast<uint32_t>(z * _landRange + x);
            if (_modernObjectCells[index].empty())
                continue;
            const int dx = x - centerX;
            const int dz = z - centerZ;
            candidates.push_back({index, static_cast<uint32_t>(dx * dx + dz * dz)});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const CellCandidate& a, const CellCandidate& b)
              { return a.distance2 < b.distance2 || (a.distance2 == b.distance2 && a.index < b.index); });

    uint32_t requested = 0;
    if (centerChanged)
    {
        std::fill(_modernObjectCellDesired.begin(), _modernObjectCellDesired.end(), 0);
        for (const CellCandidate& cell : candidates)
        {
            const uint32_t count = static_cast<uint32_t>(_modernObjectCells[cell.index].size());
            if (requested != 0 && requested + count > _modernObjectBudget)
                break;
            _modernObjectCellDesired[cell.index] = 1;
            requested += count;
        }
    }
    else
    {
        for (const CellCandidate& cell : candidates)
            if (_modernObjectCellDesired[cell.index])
                requested += static_cast<uint32_t>(_modernObjectCells[cell.index].size());
    }

    uint32_t released = 0;
    if (centerChanged)
    {
        for (size_t cell = 0; cell < _modernObjectCells.size(); ++cell)
        {
            if (_modernObjectCellDesired[cell] ||
                (!_modernObjectCellActive[cell] && _modernObjectCellCursor[cell] == 0))
                continue;
            for (uint32_t index : _modernObjectCells[cell])
            {
                ModernObjectPlacement& placement = _modernObjectPlacements[index];
                Object* object = placement.resident;
                if (!object)
                    continue;
                Ref<Object> keepAlive = object;
                placement.resident = nullptr;
                RemoveObject(keepAlive);
                _objectIds.erase(placement.id);
                // Scene's light list consists of non-owning links. Dropping this
                // object's owner entry invalidates those links and prevents marker
                // lights accumulating each time a streamed cell is revisited.
                _modernObjectLightOwners.erase(placement.id);
                ++released;
            }
            _modernObjectCellActive[cell] = 0;
            _modernObjectCellCursor[cell] = 0;
        }
    }

    uint32_t created = 0;
    uint32_t refused = 0;
    uint32_t attempted = 0;

    // Admission is denominated in TIME, not frames.
    //
    // This runs once per rendered frame (TerrainWgpu::DrawTerrain), so a fixed
    // per-update batch makes the streaming rate proportional to the framerate --
    // exactly backwards. The sustainable travel speed works out at
    // `batch * fps / (2 * radius * density)`, so on a dense world at 5 fps the
    // streamer sustains ~40 m/s and any aircraft outruns it permanently. Worse, the
    // deficit accumulates: each recentre destroys the trailing objects outright
    // (there is no cache), so what the camera left behind must be rebuilt if it
    // turns around.
    //
    // The floor is the legacy batch, so nothing streams slower than it used to; the
    // time budget only lets a slow frame admit MORE. The hard cap bounds the worst
    // case, because each create is a synchronous model decode + texture transcode.
    const auto admitStart = std::chrono::steady_clock::now();
    const float admitBudgetMs = ObjectStreamBudgetMs();
    const uint32_t admitCap = _modernObjectCreatesPerUpdate * ObjectStreamMaxBatchScale();
    auto admitBudgetSpent = [&]
    {
        if (attempted < _modernObjectCreatesPerUpdate)
            return false; // always honour the legacy batch before consulting the clock
        if (attempted >= admitCap)
            return true;
        const auto elapsed = std::chrono::steady_clock::now() - admitStart;
        return std::chrono::duration<float, std::milli>(elapsed).count() >= admitBudgetMs;
    };

    for (const CellCandidate& cell : candidates)
    {
        if (!_modernObjectCellDesired[cell.index] || _modernObjectCellActive[cell.index])
            continue;
        std::vector<uint32_t>& placements = _modernObjectCells[cell.index];
        uint32_t& cursor = _modernObjectCellCursor[cell.index];
        while (cursor < placements.size() && !admitBudgetSpent())
        {
            const uint32_t index = placements[cursor++];
            ModernObjectPlacement& placement = _modernObjectPlacements[index];
            if (placement.resident || placement.modelIndex >= _modernObjectModels.size())
                continue;
            ++attempted;
            Matrix4 transform;
            transform.SetDirectionAside(Vector3(placement.rows[0], placement.rows[1], placement.rows[2]));
            transform.SetDirectionUp(Vector3(placement.rows[3], placement.rows[4], placement.rows[5]));
            transform.SetDirection(Vector3(placement.rows[6], placement.rows[7], placement.rows[8]));
            transform.SetPosition(Vector3(placement.rows[9], placement.rows[10], placement.rows[11]));
            try
            {
                placement.resident = ObjectCreate(placement.id, _modernObjectModels[placement.modelIndex], transform,
                                                  nullptr, nullptr, false, true);
            }
            catch (const std::exception&)
            {
                placement.resident = nullptr;
            }
            if (placement.resident)
                ++created;
            else
                ++refused;
        }
        if (cursor >= placements.size())
            _modernObjectCellActive[cell.index] = 1;
        if (admitBudgetSpent())
            break;
    }
    _modernResidentObjectCount = released > _modernResidentObjectCount + created
                                     ? 0
                                     : _modernResidentObjectCount + created - released;
    _modernObjectResidencyPending = false;
    for (const CellCandidate& cell : candidates)
    {
        if (_modernObjectCellDesired[cell.index] && !_modernObjectCellActive[cell.index])
        {
            _modernObjectResidencyPending = true;
            break;
        }
    }
    // Report progress, not just the endpoints. The original condition logged only on a centre
    // change and on convergence, so a run that was still filling emitted exactly one line --
    // `resident=128 pending=1` -- and stayed silent for however long the fill took. Two
    // independent readers took that single line as proof of a stalled streamer on worlds that
    // were in fact converging (Everon in 1.4 s, Stratis in 2.5 s). Silence during the only
    // interesting interval is how a working set that grows gets reported as one that does not,
    // so emit a rate-limited progress row whenever the fill is still outstanding.
    ++_modernObjectUpdateCount;
    constexpr uint32_t ProgressLogEveryUpdates = 16;
    const bool progressRow =
        _modernObjectResidencyPending && (_modernObjectUpdateCount % ProgressLogEveryUpdates) == 0;
    if (centerChanged || !_modernObjectResidencyPending || progressRow)
        LOG_INFO(World,
                 "Modern object residency: centre=({}, {}) window={} cells requested={} resident={} created={} "
                 "released={} refused={} budget={} pending={}",
                 centerX, centerZ, windowRadius, requested, _modernResidentObjectCount, created, released, refused,
                 _modernObjectBudget, _modernObjectResidencyPending ? 1 : 0);
}

void Landscape::SaveOptimized(QOStream& f)
{
    SerializeBinStream str(&f);
    SerializeBin(str, _landGrid);
}

void Landscape::SaveOptimized(const char* name)
{
    // optional - just to make sure all is converted well
    QOFStream f;
    f.open(name);
    SaveOptimized(f);
    f.close();
}

// load/save current status (no terrain/object data save here)
LSError Landscape::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(ar.Serialize("lastObjectID", _objectId, 1))
    if (ar.IsSaving())
    {
        RefArray<Object> objects;
        for (int x = 0; x < _landRange; x++)
        {
            for (int z = 0; z < _landRange; z++)
            {
                const ObjectList& list = _objects(x, z);
                for (int o = 0; o < list.Size(); o++)
                {
                    Object* obj = list[o];
                    // do not save temporaries
                    if (obj->GetType() != Primary && obj->GetType() != Network)
                    {
                        continue;
                    }
                    PoseidonAssert(obj->GetShape());
                    PoseidonAssert(obj->GetShape()->Name());
                    if (obj->ID() == 0)
                    {
                        Log("Object id %d %s", obj->ID(), (const char*)obj->GetShape()->Name());
                    }
                    if (!obj->MustBeSaved())
                    {
                        continue; // save only non-default states
                    }
                    objects.Add(obj);
                }
            }
        }
        PARAM_CHECK(ar.Serialize("Objects", objects, 1))
    }
    else if (ar.GetPass() == ParamArchive::PassSecond)
    {
        ParamArchive arCls;
        if (!ar.OpenSubclass("Objects", arCls))
        {
            return LSOK; // no objects saved
        }
        arCls.FirstPass();
        int n;
        PARAM_CHECK(arCls.Serialize("items", n, 1))
        for (int i = 0; i < n; i++)
        {
            char buffer[256];
            snprintf(buffer, sizeof(buffer), "Item%d", i);
            ParamArchive arRef;
            if (!arCls.OpenSubclass(buffer, arRef))
            {
                continue;
            }
            Object* obj = Object::CreateObject(arRef); // Find object in landscape
            if (!obj)
            {
                continue;
            }
            arRef.SecondPass();
            PARAM_CHECK(obj->Serialize(arRef))
            arCls.CloseSubclass(arRef);
        }
    }
    else
    {
        RefArray<Object> objects;
        PARAM_CHECK(ar.Serialize("Objects", objects, 1))
    }
    return LSOK;
}

void Landscape::ResetObjectIDs()
{
    Log("ResetObjectIDs");
    ClearIDCache();
    // reset id of all vehicles - use with caution
    int maxId = -1;
    int x, z;
    for (x = 0; x < _landRange; x++)
    {
        for (z = 0; z < _landRange; z++)
        {
            ObjectList& list = _objects(x, z);
            for (int o = 0; o < list.Size(); o++)
            {
                Object* obj = list[o];
                // never change primary object ID
                if (obj->GetType() != Primary && obj->GetType() != Network)
                {
                    obj->SetID(-1);
                }
                else
                {
                    // PoseidonAssert( obj->ID()>=0 );
                }
                if (maxId < obj->ID())
                {
                    maxId = obj->ID();
                }
            }
        }
    }
    SetLastObjectID(maxId);
    // vehicles will have id assigned by world
}

bool Landscape::CheckObjectStructure() const
{
#if DO_LINK_DIAGS
#endif
    return true;
}

void Landscape::ResetState()
{
    // repair all objects, remove any non-primaries
    for (int x = 0; x < _landRange; x++)
    {
        for (int z = 0; z < _landRange; z++)
        {
            ObjectList& list = _objects(x, z);
            int maxRetry = 10000;
        Retry:
            if (--maxRetry <= 0)
            {
                Fail("Too much Retry attempts");
            }
            else
            {
                for (int o = 0; o < list.Size();)
                {
                    int oldSize = list.Size();
                    Object* obj = list[o];
                    if (obj->GetType() != Primary && obj->GetType() != Network)
                    {
                        // delete non-primary
                        list.Delete(o);
                        if (oldSize - 1 != list.Size())
                        {
                            goto Retry;
                        }
                        continue;
                    }
                    Vector3 oldPos = obj->Position();
                    obj->ResetStatus(); // full repair
                    // note: obj may move during ResetStatus
                    // check if it still belongs to the same list

                    int xl, zl;
                    SelectObjectList(xl, zl, obj->Position().X(), obj->Position().Z());
                    if (xl != x || zl != z)
                    {
                        if (oldSize - 1 != list.Size())
                        {
                            if (oldSize == list.Size())
                            {
                                // LOG_DEBUG(World, "Object list unchanged");
                                o++;
                                continue;
                            }
                            goto Retry;
                        }

                        // object moved to another list and deleted from this one
                        continue;
                    }

                    if (oldSize != list.Size())
                    {
                        goto Retry;
                    }

                    o++;
                }
            }
            list.Compact();
        }
    }
    for (int x = 0; x < _landRange; x++)
    {
        for (int z = 0; z < _landRange; z++)
        {
            ObjectList& list = _objects(x, z);
            for (int o = 0; o < list.Size();)
            {
                Object* obj = list[o];
                if (obj->GetType() != Primary && obj->GetType() != Network)
                {
                    // delete non-primary
                    LOG_ERROR(World, "Non-primary object {:x}:{},{} present", (uintptr_t)obj,
                              (const char*)obj->GetDebugName(),
                              obj->GetShape() ? (const char*)obj->GetShape()->Name() : "<null>");
                }
                o++;
            }
        }
    }
    RebuildIDCache();
}

void Landscape::OnTimeSkipped()
{
    // let all objects react to time change
    for (int x = 0; x < _landRange; x++)
    {
        for (int z = 0; z < _landRange; z++)
        {
            ObjectList& list = _objects(x, z);
            for (int o = 0; o < list.Size(); o++)
            {
                Object* obj = list[o];
                obj->OnTimeSkipped();
            }
        }
    }
}

void Landscape::ClearIDCache()
{
    Log("Landscape::ClearIDCache");
    _objectIds.clear();
}

void Landscape::RebuildIDCache()
{
    Log("Landscape::RebuildIDCache");
    int x, z;
    int maxId = -1;
    for (x = 0; x < _landRange; x++)
    {
        for (z = 0; z < _landRange; z++)
        {
            const ObjectList& list = _objects(x, z);
            for (int i = 0; i < list.Size(); i++)
            {
                Object* obj = list[i];
                int id = obj->ID();
                if (id >= 0)
                {
                    _objectIds[id] = obj;
                    if (id > maxId)
                    {
                        id = maxId;
                    }
                }
                else
                {
                    PoseidonAssert(obj->GetType() != Primary);
                    PoseidonAssert(obj->GetType() != Network);
                }
            }
        }
    }
    SetLastObjectID(maxId);
}

void Landscape::AddToIDCache(Object* object)
{
    int id = object->ID();
    _objectIds[id] = object;
}

Object* Landscape::FindObject(int id) const
{
    if (id < 0)
    {
        return nullptr; // id<0 means nullptr
    }
    // this function is much slower than GetObject(id)
    // try id cache first
    const auto found = _objectIds.find(id);
    if (found != _objectIds.end())
    {
        Object* ret = found->second;
        if (ret)
        {
            PoseidonAssert(ret->ID() == id);
            return ret;
        }
    }
    else
    {
        Log("No ID cache.");
    }
    return FindObjectNC(id);
}

Object* Landscape::FindObjectNC(int id) const
{
    // check cached square
    int x, z, xx, zz;
    // check last successfull square
    if (this_InRange(_lastFindObjectX, _lastFindObjectZ))
    {
        // check neighhbourghs
        for (xx = -1; xx <= +1; xx++)
        {
            for (zz = -1; zz <= +1; zz++)
            {
                x = _lastFindObjectX + xx;
                z = _lastFindObjectX + zz;
                if (!this_InRange(x, z))
                {
                    continue;
                }
                const ObjectList& list = _objects(x, z);
                for (int o = 0; o < list.Size(); o++)
                {
                    if (list[o]->ID() == id)
                    {
                        _lastFindObjectX = x;
                        _lastFindObjectZ = z;
                        Log("ID Cache qsearch %d OK", id);
                        return list[o];
                    }
                }
            }
        }
    }
    // check all cells
    for (x = 0; x < _landRange; x++)
    {
        for (z = 0; z < _landRange; z++)
        {
            const ObjectList& list = _objects(x, z);
            for (int o = 0; o < list.Size(); o++)
            {
                if (list[o]->ID() == id)
                {
                    _lastFindObjectX = x;
                    _lastFindObjectZ = z;
                    Log("ID Non-Cached search %d OK", id);
                    return list[o];
                }
            }
        }
    }
    Log("ID Non-Cached search %d failed", id);
    return nullptr;
}
} // namespace Poseidon
