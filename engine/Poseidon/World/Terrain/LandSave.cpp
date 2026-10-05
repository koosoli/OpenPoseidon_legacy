#include <Poseidon/UI/LocalMapWorlds.hpp>
#include <Poseidon/UI/LocalMapsCatalog.hpp>
#include <filesystem>
#include <Poseidon/Core/Application.hpp>

#include <chrono>
#include <atomic>
#include <optional>

#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Terrain/AuthoredObjectIds.hpp>
#include <Poseidon/World/Terrain/TerrainSubdivision.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/FileServer.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/IO/Serialization/ParamArchive.hpp>
#include <Poseidon/IO/Filesystem/Utf8Paths.hpp>
#include <Poseidon/Foundation/Threads/WatchDog.hpp>
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
#include <iterator> // std::prev, for the eviction cache's LRU list
#include <algorithm>
#include <memory>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp> // ShapeBank::Find / NewFromModel (async admission)
#include <Poseidon/World/Model/ShapeAdapter.hpp> // BuildAdapterBankTables (stage-3 async adapt)
#include <Poseidon/Graphics/Rendering/Shape/ObjectAdmitProfile.hpp> // the split of `create`
#include <Poseidon/Graphics/Textures/PreparedTextures.hpp> // worker-prepared texture store (clear + stats)
#include <Poseidon/World/Model/Model.hpp>              // the IR handed over by ObjectStreamPreparer
#include <Poseidon/World/Model/ModelMemory.hpp>        // conversion reservation estimate
#include <Poseidon/World/Terrain/ObjectStreamPrepare.hpp>
#include <Poseidon/World/Terrain/ObjectTextureUploadTrace.hpp>
#include <Poseidon/World/Terrain/ObjectStreamTreeFixture.hpp>
#include <Poseidon/World/Terrain/WarmModelCaptureWindow.hpp>
#include <Poseidon/Dev/Diag/StreamingDiag.hpp> // SnapshotObjectStreamDiag fills its counters
#include <Poseidon/Asset/Formats/Material/ShaderSchema.hpp>
#include <stdexcept>
#include <Poseidon/World/Terrain/WrpReader.hpp>
#include <Poseidon/IO/Serialization/SerializeBinExt.hpp>
#include <Poseidon/World/Terrain/TerrainProfile.hpp>

#include <time.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
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

    std::vector<std::pair<int, std::string>> rvmatCells; // Sinkhole W5
    for (i = 0; i < LAND_TEXTURES_MAX; i++)
    {
        char texName[33] = {}; // 32 wire bytes + guaranteed NUL for the strstr / SetTexture C-string use
        f.read(texName, 32);
        const size_t nameLength = strlen(texName);
        if (nameLength > 6 && _stricmp(texName + nameLength - 6, ".rvmat") == 0)
        {
            rvmatCells.emplace_back(i, texName); // a material, not an image: resolved below
        }
        else if (*texName)
        {
            PoseidonAssert(!strstr(texName, "$$")); // SetTexture(i,"landText\\pi.pac");
            SetTexture(i, texName);
        }
        ProgressSetRest(f.rest());
        ProgressRefresh();
    }
    // Sinkhole W5: cells may name an authored terrain material instead of a plain texture
    if (!rvmatCells.empty())
        LoadRvmatTerrainMaterials(rvmatCells);

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
            // Sinkhole W5: the converted Forest island's placements are its authored world transforms on this exact
            // heightfield (rocks half buried, pines pivoted 4 m under the trunk), so they keep their elevation. A
            // Visitor-made world relies on the legacy drop-to-ground correction and still gets it.
            ObjectCreate(so.id, so.name, ConvertToM(so.matrix), nullptr, nullptr, false, _rvmatCellMaterials);
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
    // The single longest blocking call in a mission start, and the one that sets the
    // watchdog's deadline. It is also the one place where "the game stopped responding
    // while loading" is genuinely ambiguous between slow and wedged; the report names the
    // world, so a stall here says which one.
    Poseidon::WatchDogItem watch = Poseidon::WatchScopeFor("world.load", name ? name : "");
    LOG_DEBUG(World, "Landscape::LoadData start");
    Log("Load landscape %s", name);
    _name = name; // remmember name
    LSError ret = LSUnknownError;

    if (Poseidon::IsLocalMapWorldIdentifier(name))
    {
        const auto* map = Poseidon::FindLocalMapWorld(name);
        if (!map || !Poseidon::IsLocalMapWorldAvailable(name) ||
            Poseidon::FindLocalMapWorld(AppConfig::Instance().GetLocalMapWorldId()) != map)
        {
            Poseidon::Foundation::ErrorMessage("Select this terrain in Local Content before opening its mission.");
            return LSUnknownError;
        }
        const bool loaded = map->enfusion
            ? LoadEnfusionNative(map->archiveRoot.c_str(), map->worldPath.c_str(), true)
            : LoadOprwModern(map->worldPath.c_str());
        return loaded ? LSOK : LSUnknownError;
    }

    // --test-world substitutes a landscape the mission never asked for. Arma 3
    // worlds are only reachable this way: they are named by CfgWorlds entries
    // this build has no configs for, so nothing would ever route to them.
    const std::string& override = AppConfig::Instance().GetTestWorldPath();
    if (!override.empty())
    {
        Log("--test-world: loading %s instead of %s", override.c_str(), name);
        // RFG-005: a DIRECTORY means an Enfusion install to load natively rather than
        // a world file to parse. Unambiguous -- `LoadOprwModern` would fail on a
        // directory anyway -- and it costs no new command-line option.
        std::error_code enfusionEc;
        if (std::filesystem::is_directory(override, enfusionEc))
        {
            if (LoadEnfusionNative(override.c_str()))
            {
                LOG_DEBUG(World, "Landscape::LoadData time {}", Poseidon::Foundation::GlobalTickCount() - start);
                return LSOK;
            }
            Poseidon::Foundation::ErrorMessage("Cannot load --test-world '%s' as an Enfusion install",
                                               override.c_str());
            return LSUnknownError;
        }
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

// The ground height the terrain-relative re-seat uses. SurfaceY answers YOutsideMap (-100 m) for any point in the
// LAST terrain row/column (its +1 neighbour is off the map), so an object standing in that edge strip was
// re-seated against -100 m on one side of a terrain change and the real ground on the other -- moved by up to
// height + 100 m. Clamp the lookup into the last full cell instead; both passes use it, so the object keeps
// exactly its height above the ground it actually stands on.
static float ReseatSurfaceY(const Landscape& land, float x, float z)
{
    const float maxCoord = (land.GetTerrainRange() - 1) * land.GetTerrainGrid() - 0.01f;
    return land.SurfaceY(std::clamp(x, 0.0f, maxCoord), std::clamp(z, 0.0f, maxCoord));
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
                float surfY = ReseatSurfaceY(*this, bcWorld.X(), bcWorld.Z());
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
                        // Y is not re-seated (a forest square follows the terrain through its conform
                        // plane / skew), but those were derived from the old heights: rebuild them, and
                        // let the retained GPU instance pick the new plane up.
                        obj->OnTerrainChanged(this);
                        if (GEngine)
                        {
                            GEngine->SceneObjectMoved(obj);
                        }
                        continue;
                    }
                    LODShape* shape = obj->GetShape();
                    Vector3 pos = obj->Position();
                    Vector3 bcWorld = (shape ? obj->PositionModelToWorld(-shape->BoundingCenter()) : pos);
                    float surfY = ReseatSurfaceY(*this, bcWorld.X(), bcWorld.Z());
                    pos[1] += surfY;
                    obj->SetPosition(pos);
                    obj->OnTerrainChanged(this);
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
    // UP-315733ab: a grid change must retire cached heightfields keyed off
    // HeightRevision, otherwise setTerrainGrid leaves visual and physical
    // terrain out of sync.
    ++_heightRevision;

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
    // UP-315733ab: see ResampleTerrain -- grid change retires HeightRevision key holders.
    ++_heightRevision;
}

void Landscape::SubdivideTerrain(int subdivStepLog)
{
    auto st0 = TerrainProfile::Now();

    MakeObjectsTerrainRelative();

    while (subdivStepLog > 0 && _terrainRange <= MaxRefinedTerrainRange / 2)
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
// v2: + a hash of the SOURCE heights the cache was subdivided from, and the level. v1 caches were
// accepted whenever the land range and grid matched -- which every OFP/CWA island shares -- so a cache
// built by an older terrain decoder or from another eden.wrp replaced the whole terrain silently, and
// everything not re-seated (forests, cached conform heights) floated or sank. Bumping the version
// retires every v1 cache once.
static const uint32_t SUBDIV_CACHE_VERSION = 2;

// FNV-1a 64 over the raw heights, plus the dimensions
template <class Data> static uint64_t HashTerrainHeights(const Data& data, int range)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* p, size_t n)
    {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i)
        {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    mix(&range, sizeof(range));
    mix(data.RawData(), static_cast<size_t>(range) * static_cast<size_t>(range) * sizeof(RawType));
    return h;
}

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
    // the heights this subdivision starts from: SaveSubdivCache stamps them, a load must match them
    _subdivSourceHash = HashTerrainHeights(_data, _terrainRange);
    if (_name.GetLength() == 0)
    {
        return false; // an unnamed world has no stable cache key (cwr_subdiv__L*.cache was shared)
    }
    std::string path = GetSubdivCachePath(_name, targetSubdivLog);
    FILE* f = OpenFileUtf8(path.c_str(), "rb");
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
    uint64_t cachedSourceHash = 0;
    int32_t cachedLevel = -1;
    ok = ok && fread(&cachedSourceHash, 8, 1, f) == 1 && fread(&cachedLevel, 4, 1, f) == 1;

    if (!ok || cachedLandRange != _landRange || cachedLandRangeLog != _landRangeLog || cachedLandGrid != _landGrid ||
        cachedSourceHash != _subdivSourceHash || cachedLevel != targetSubdivLog)
    {
        if (ok)
        {
            LOG_INFO(Core, "LOAD: SubdivCache {} is stale (source heights or level differ): rebuilding", path);
        }
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
    // UP-315733ab: see ResampleTerrain -- grid change retires HeightRevision key holders.
    ++_heightRevision;

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
    if (_name.GetLength() == 0)
    {
        return;
    }
    std::string path = GetSubdivCachePath(_name, targetSubdivLog);
    FILE* f = OpenFileUtf8(path.c_str(), "wb");
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
    const int32_t level = targetSubdivLog;
    fwrite(&_subdivSourceHash, 8, 1, f); // v2: set by the LoadSubdivCache miss that preceded this save
    fwrite(&level, 4, 1, f);

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

    // FLOAT CENSUS (the OPRW twin of RFG-052, which only ever covered the native
    // Enfusion loader). A classic .wrp stores a placement's Y as the BOUNDING CENTRE
    // of the model, not its base -- measurable in the data: every kostel3 on Everon
    // sits 17.2 m over the heightfield, every kostelik 5-7 m, every flat road 0.9 m,
    // i.e. exactly half the model's height. Landscape::ObjectCreate undoes that with
    // the BoundingCenter() correction. If a shape's bounding box is not there when
    // that correction runs, the correction is a no-op and the object is left standing
    // at its own half-height -- which is what "a house that flies" looks like.
    //
    // Nothing said when that happened. This counts it, and names the worst one, so a
    // report of a floating building can be answered from the boot log instead of from
    // a screenshot hunt across 12.8 km of island.
    int floatingObjects = 0;
    float worstFloat = 0.0f;
    RStringB worstFloatName;
    Vector3 worstFloatPos(0, 0, 0);

    for (int i = 0; i < reader.GetObjectCount(); i++)
    {
        const auto& obj = reader.GetObject(i);
        int x, z;
        Object* o = ObjectCreate(obj.id, obj.name, obj.transform, &x, &z);

        if (o != nullptr)
        {
            const LODShapeWithShadow* shape = o->GetShape();
            // Measure the DRAWN bottom, not the placement origin: an object stands on
            // the ground when the low corner of its bounding box does, and a model whose
            // box never arrived reports an empty one, which is itself the defect.
            if (shape != nullptr && shape->Max().Y() > shape->Min().Y())
            {
                const Vector3 pos = o->Position();
                const float bottom = pos.Y() + shape->Min().Y();
                const float above = bottom - SurfaceY(pos.X(), pos.Z());
                if (above > 5.0f)
                {
                    ++floatingObjects;
                    if (above > worstFloat)
                    {
                        worstFloat = above;
                        worstFloatName = obj.name;
                        worstFloatPos = pos;
                    }
                }
            }
        }

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

    if (floatingObjects == 0)
    {
        LOG_INFO(World, "OPRW load: 0 of {} objects stand more than 5 m above the terrain",
                 reader.GetObjectCount());
    }
    else
    {
        LOG_INFO(World,
                 "OPRW load: {} of {} objects stand more than 5 m above the terrain; worst '{}' at {}, {}, {} is {} m",
                 floatingObjects, reader.GetObjectCount(), worstFloatName.Data(), worstFloatPos.X(), worstFloatPos.Y(),
                 worstFloatPos.Z(), worstFloat);
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

// The object-streaming knobs are defined next to the residency system they govern, further down
// this file. LoadOprwModern only reads them, at world load, to size the eviction cache and to
// record the policy in the load log, so it needs the declarations and not the definitions.
static size_t ObjectStreamShapeCacheLimit();
static float ObjectStreamCeilingMs();
static float ObjectStreamPrefetchSeconds();
static float ObjectStreamPrefetchMinSpeed();
static int ObjectStreamPrefetchMaxCells();

// Modern OPRW worlds carry two grids: a coarse "land" grid for cell semantics,
// materials and object lists, and a fine "terrain" grid for elevation. Preserve
// that authored split. Landscape's height/surface queries operate in terrain
// space; geography, roads, objects, randomisation and material lookup operate in
// land space. Their world extents remain identical because landGrid is
// terrainGrid * ratio.
// One parsed terrain RVMAT -> its A3TerrainMaterial slot and the cell's legacy texture. Shared by the Arma world
// loader (LoadOprwModern) and, for texture names ending in ".rvmat", the raw 4WVR loader (Sinkhole W5: the converted
// Forest island authors its ground the same way). Returns true when a legacy texture was resolved.
bool Landscape::ApplyTerrainMaterialSource(int index, const Poseidon::Asset::Material::RvMaterialSource& material)
{
    const size_t i = static_cast<size_t>(index);
    int resolved = 0;
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
    return resolved != 0;
}

// Sinkhole W5: a raw 4WVR world (the converted Forest island) may name an RVMAT in its cell texture table instead of
// a texture. Each one is parsed (text or binary) and resolved exactly as an Arma world's terrain material is, so the
// authored satellite/mask/surface path draws it. Stock OFP/CWA worlds name only .paa/.pac textures: for them this
// finds nothing and leaves _a3TerrainMaterials empty, as before.
void Landscape::LoadRvmatTerrainMaterials(const std::vector<std::pair<int, std::string>>& cells)
{
    int count = _texture.Size();
    for (const auto& cell : cells)
        count = std::max(count, cell.first + 1);
    if (_texture.Size() < count)
        _texture.Resize(count);
    _a3TerrainMaterials.Resize(count);
    for (int m = 0; m < count; ++m)
        _a3TerrainMaterials[m] = A3TerrainMaterial{};
    int resolved = 0;
    for (const auto& [index, name] : cells)
    {
        try
        {
            const auto material = Poseidon::Asset::Material::ParseRvMaterialFile(name);
            resolved += ApplyTerrainMaterialSource(index, material) ? 1 : 0;
        }
        catch (const std::exception& e)
        {
            LOG_WARN(World, "4WVR terrain material '{}' not loaded: {}", name, e.what());
        }
    }
    _rvmatCellMaterials = resolved > 0;
    LOG_INFO(World, "4WVR terrain materials: {} RVMAT cell textures, {} resolved to a legacy texture", cells.size(),
             resolved);
}

bool Landscape::LoadOprwModern(const char* path)
{
    namespace Fmt = Poseidon::Asset::Formats;

    const DWORD oprwStart = Poseidon::Foundation::GlobalTickCount();
    // Local map discovery supplies the archive's virtual WRP name. AutoOpen
    // also accepts loose files, preserving existing converted-world workflows.
    QIFStreamB file;
    file.AutoOpen(path);
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

    // WHERE A BIG WORLD'S LOAD TIME GOES, with denominators. Chernarus+ spends over ten
    // seconds inside this function with nothing logged above LOG_DEBUG, so "loading is slow"
    // has never had a breakdown attached to it. Milliseconds alone would not help either:
    // 2.9 million placements and 12 thousand are the same total at very different per-item
    // costs, and only the ratio says which one to attack.
    const DWORD parseStart = Poseidon::Foundation::GlobalTickCount();
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

    const DWORD parseMs = Poseidon::Foundation::GlobalTickCount() - parseStart;
    LOG_INFO(World, "World load: OPRW rev {} parsed in {} ms -- {} objects, {} models, terrain {}x{}",
             revision, parseMs, world.objects.size(), world.models.size(), world.header.terrainRangeX,
             world.header.terrainRangeX);

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
    // No tide on a foreign world. OFP's sea rides `maxTide * tide` (LandscapeShared.hpp: 5 m
    // amplitude, driven by the sun AND moon elevation) and its islands were built around that.
    // Arma 1/2/3 and DayZ have no tide -- their coasts, harbours and piers are authored to a
    // sea at exactly 0 -- so here the tide put the ocean up to several metres over them, and it
    // MOVED when the moon went onto a real ephemeris (f88f0ea): "the sea level rose" on Stratis.
    _tidal = false;

    const float landGrid = terrainGrid * ratio;
    const DWORD tDim = Poseidon::Foundation::GlobalTickCount();
    Dim(land, land, terrain, terrain, landGrid);
    FlushCache();
    LOG_INFO(World, "World load: Dim+FlushCache {} ms -- land {}x{}, terrain {}x{}",
             Poseidon::Foundation::GlobalTickCount() - tDim, land, land, terrain, terrain);
    const DWORD tGrids = Poseidon::Foundation::GlobalTickCount();

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
    LOG_INFO(World, "World load: grid fill {} ms -- {} elevation samples, {} geography+material cells",
             Poseidon::Foundation::GlobalTickCount() - tGrids, (long long)terrain * terrain,
             (long long)land * land);
    ProgressRefresh();

    const DWORD tMaterials = Poseidon::Foundation::GlobalTickCount();
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

            resolved += ApplyTerrainMaterialSource(static_cast<int>(i), material) ? 1 : 0;
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
    LOG_INFO(World, "World load: mountains+terrain materials {} ms -- {} mountains, {} RVMATs",
             Poseidon::Foundation::GlobalTickCount() - tMaterials, world.mountains.size(),
             world.materials.size());
    ProgressRefresh();
    const DWORD tPlacements = Poseidon::Foundation::GlobalTickCount();
    // A reused landscape may load another modern world without a fresh Init(). Retire
    // any exact DayZ converted-shape staging slot before replacing model indices.
    _modernPendingOwnerPrimaryTextures.reset();

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
        ResetModernSimulationResidency();
        _modernRequiredObjects.clear();
        _modernLogicalOnlyObjects.clear();
        _modernObjectPlacements.clear();
        _modernObjectPlacements.reserve(placementLimit);
        _modernObjectCells.assign(static_cast<size_t>(_landRange) * _landRange, {});
        _modernObjectCellActive.assign(_modernObjectCells.size(), 0);
        _modernObjectCellDesired.assign(_modernObjectCells.size(), 0);
        _modernObjectCellCursor.assign(_modernObjectCells.size(), 0);
        _modernObjectCandidates.clear();
        _modernObjectCandidatesValid = false;
        _modernObjectCandidateCenterX = -0x3fffffff;
        _modernObjectCandidateCenterZ = -0x3fffffff;
        _modernObjectCandidateRadius = 0;
        _modernObjectCandidateBudget = 0;
        _modernObjectDesiredBudget = 0;
        _modernObjectDesiredCount = 0;
        _modernObjectEffectiveBudget = 0;
        _modernObjectBudgetLogged = 0;

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

        // (a) THE EVICTION CACHE, and (b) the prefetch estimator, are per-world state: a new
        // world's shapes have nothing to do with the previous world's, and a stale velocity
        // would bias the first window centre of a world the camera has not moved in yet.
        _modernObjectShapeCacheLimit = ObjectStreamShapeCacheLimit();
        TrimModernObjectShapeCache(0);
        _modernObjectShapeCacheHits = 0;
        _modernObjectShapeCacheInserts = 0;
        _modernObjectShapeCacheDrops = 0;
        _modernObjectShapeCachePressure.Reset();
        _modernObjectVelocityValid = false;
        _modernObjectVelocityX = 0.0f;
        _modernObjectVelocityZ = 0.0f;
        _modernObjectLeadMetres = 0.0f;
        _modernObjectDesiredRadiusCells = 0;
        // (d) Asynchronous cold-model preparation is per-world too: the model table it indexes
        // is this world's, and every counter on the residency row starts from zero with it.
        _modernObjectModelPrefetchEpoch.assign(_modernObjectModels.size(), 0);
        _modernObjectPrefetchEpoch = 0;
        _modernObjectAsyncInstalled = 0;
        _modernObjectAsyncWaited = 0;
        _modernObjectSyncCold = 0;
        _modernObjectAdmitUpdates = 0;
        _modernObjectAdmitStopCap = 0;
        _modernObjectAdmitStopUpload = 0;
        _modernObjectAdmitStopCeiling = 0;
        _modernObjectAdmitStopBudget = 0;
        _modernObjectAdmitStopCold = 0;
        _modernObjectColdModels = 0;
        _modernObjectColdParseMs = 0.0;
        _modernObjectColdAdaptMs = 0.0;
        _modernObjectColdOptimizeMs = 0.0;
        _modernObjectColdCreateMs = 0.0;
        EnsureModernObjectPreparer();
        // Registered here rather than in the constructor because this translation unit owns the
        // streaming system and the registry entry must exist exactly once per Landscape: the
        // free-on-demand list is an intrusive CLList, so a second Register() of the same probe
        // would corrupt it. The probe reports items, not bytes -- a shape has no cheap byte size
        // -- and its Free hook empties the cache outright, which is safe by construction because
        // the cache is a pure optimisation: losing it costs reload time and nothing else.
        //
        // Priority 0.25 puts it BELOW textures (0.5) and the shape bank (1.0) so the global
        // pressure path reclaims this first, which is correct: it is the only one of the three
        // that is redundant by design.
        if (!_modernObjectShapeCacheRegistered)
        {
            _modernObjectShapeCacheRegistered = true;
            _modernObjectShapeCacheProbe.Register(
                "Models.StreamCache", 0.25f, {}, {}, [this] { return _modernObjectShapeCache.size(); },
                [this](size_t) -> size_t
                {
                    TrimModernObjectShapeCache(0);
                    // Deliberately reports zero bytes reclaimed. The contract is "bytes actually
                    // freed" and a shape has no cheap byte size, so any number here would be
                    // invented -- and an invented number that is too large makes
                    // MemoryFreeOnDemandList::Free stop early (MemFreeReq.cpp:60) believing it
                    // has reclaimed memory it has not. Zero errs toward freeing more elsewhere,
                    // which is the safe direction under pressure.
                    return static_cast<size_t>(0);
                });
        }

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
        // Reserve all authored (including reassigned duplicate) IDs before mission entities
        // can allocate IDs, rather than waiting for a camera-populated cache rebuild.
        _objectId = std::max(_objectId, AuthoredObjectIDFloor());
        _modernObjectResidencyPending = false;
        _modernObjectCenterX = -0x3fffffff;
        _modernObjectCenterZ = -0x3fffffff;
        _modernResidentObjectCount = 0;
        LOG_INFO(World, "World load: placement retention {} ms -- {} placements binned into {} land cells, {} models",
                 Poseidon::Foundation::GlobalTickCount() - tPlacements, _modernObjectPlacements.size(),
                 _modernObjectCells.size(), _modernObjectModels.size());

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
                // body, so reading it is not a header read at all), containers that are
                // neither ODOL nor MLOD, and any input that decoded to nonsense. A proxy
                // sized from any of those is worse than no proxy, so the model simply gets
                // none.
                //
                // MLOD used to land here too, and that is what emptied this tier on every
                // imported world: the Reforger models are MLOD 1.1 / P3DM without exception,
                // so ReadModelProxyBounds refused all 1,132 Everon models, the instance array
                // came out empty and the renderer released its far buffers at world load.
                // MLOD now derives its bounds from the first LOD's vertex block. It carries
                // no average colour anywhere, so those proxies keep the white default --
                // bounds.colorKnown is what says so.
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

        // Report the streaming POLICY, not only its inputs. Three of these are new behaviour
        // gated on env knobs, and a run that does not say which arm it is cannot be compared
        // with one that does.
        LOG_INFO(World,
                 "Modern object streaming: retained {} compact placements in {} land cells; radius={} cells, "
                 "resident budget={}, create batch={}, duplicate ids reassigned={}; evict cache={} shapes, "
                 "admit ceiling={:.1f} ms, prefetch={:.1f} s above {:.1f} m/s (max {} cells); async prepare={} "
                 "(workers={} queue={} ready={})",
                 _modernObjectPlacements.size(), _modernObjectCells.size(), _modernObjectRadius,
                 _modernObjectBudget, _modernObjectCreatesPerUpdate, reassigned, _modernObjectShapeCacheLimit,
                 ObjectStreamCeilingMs(), ObjectStreamPrefetchSeconds(), ObjectStreamPrefetchMinSpeed(),
                 ObjectStreamPrefetchMaxCells(),
                 _modernObjectPreparer && _modernObjectPreparer->Running() ? "on" : "off",
                 ObjectStreamPreparer::WorkerCount(), ObjectStreamPreparer::QueueLimit(),
                 ObjectStreamPreparer::ReadyLimit());
        const DWORD tFinish = Poseidon::Foundation::GlobalTickCount();
        ProgressFinish();
        LOG_INFO(World, "World load: LoadOprwModern total {} ms (ProgressFinish {} ms)",
                 Poseidon::Foundation::GlobalTickCount() - oprwStart,
                 Poseidon::Foundation::GlobalTickCount() - tFinish);
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
static bool ObjectStreamWarmProfileEnabled()
{
    static const bool on = []
    {
        const char* value = std::getenv("WGR_OBJECT_STREAM_WARM_PROFILE");
        return value && std::strcmp(value, "1") == 0;
    }();
    return on;
}

// A child admission must not reset its parent's inclusive buckets. Cold scopes
// retain their existing ownership and logging; this owns only an opt-in warm scope.
class WarmObjectAdmitProfileScope
{
    render::ObjectAdmitProfile& _profile;
    bool _previousActive;
    bool _opened;
public:
    WarmObjectAdmitProfileScope(render::ObjectAdmitProfile& profile, bool enabled)
        : _profile(profile), _previousActive(profile.active), _opened(enabled && !profile.active)
    {
        if (_opened)
        {
            _profile.Reset();
            _profile.active = true;
        }
    }
    bool Opened() const { return _opened; }
    void Close()
    {
        if (_opened)
        {
            _profile.active = _previousActive;
            _opened = false;
        }
    }
    ~WarmObjectAdmitProfileScope() { Close(); }
};

struct WarmObjectUpdateProfile
{
    render::ObjectAdmitProfile sums;
    uint32_t count = 0;
    double totalMs = 0.0, geoOwnerMs = 0.0;
    void Add(const render::ObjectAdmitProfile& p, double total, double geography)
    {
        ++count;
        totalMs += total;
        geoOwnerMs += geography;
        sums.sceneMs += p.sceneMs;
        sums.registerMs += p.registerMs;
        sums.proxiesMs += p.proxiesMs;
        sums.meshBuildMs += p.meshBuildMs;
        sums.meshCreateMs += p.meshCreateMs;
        sums.sectionMs += p.sectionMs;
        sums.modelRegisterMs += p.modelRegisterMs;
        sums.textureReadMs += p.textureReadMs;
        sums.textureCreateMs += p.textureCreateMs;
        sums.textureFallbackMs += p.textureFallbackMs;
        sums.alphaScanMs += p.alphaScanMs;
        sums.textureWindowWaitMs += p.textureWindowWaitMs;
        sums.texHeaderLoadMs += p.texHeaderLoadMs;
        sums.texHeaderLoads += p.texHeaderLoads;
        sums.meshes += p.meshes;
        sums.sections += p.sections;
        sums.textureUploads += p.textureUploads;
        sums.texturePrepared += p.texturePrepared;
        sums.proxyModels += p.proxyModels;
        sums.parkHits += p.parkHits;
        sums.parkStale += p.parkStale;
    }
    void EmitSummary(int centerX, int centerZ) const
    {
        // Separate process-wide cap from individual slow rows. Many sub-2ms
        // admissions can form a slow update; every opened warm scope contributes.
        static uint32_t rows = 0;
        static bool truncated = false;
        if (rows < 128)
        {
            ++rows;
            LOG_INFO(World,
                     "Object stream warm update profile: row={} centre=({}, {}) warmCount={} totalMs={:.3f} "
                     "geoOwnerMs={:.3f} sceneInclusiveMs={:.3f} registerInclusiveMs={:.3f} "
                     "proxyInclusiveMs={:.3f} meshBuildMs={:.3f} meshCreateMs={:.3f} "
                     "sectionInclusiveMs={:.3f} modelFfiMs={:.3f} texReadMs={:.3f} texCreateMs={:.3f} "
                     "texFallbackMs={:.3f} alphaMs={:.3f} textureWindowWaitSubsetMs={:.3f} "
                     "texHeaderMs={:.3f} texHeaders={} meshes={} sections={} uploads={} "
                     "preparedUploads={} gpuChildInstances={} parkHits={} parkStale={}",
                     rows, centerX, centerZ, count, totalMs, geoOwnerMs, sums.sceneMs, sums.registerMs,
                     sums.proxiesMs, sums.meshBuildMs, sums.meshCreateMs, sums.sectionMs,
                     sums.modelRegisterMs, sums.textureReadMs, sums.textureCreateMs,
                     sums.textureFallbackMs, sums.alphaScanMs, sums.textureWindowWaitMs,
                     sums.texHeaderLoadMs, sums.texHeaderLoads, sums.meshes, sums.sections,
                     sums.textureUploads, sums.texturePrepared, sums.proxyModels, sums.parkHits, sums.parkStale);
        }
        else if (!truncated)
        {
            truncated = true;
            LOG_INFO(World, "Object stream warm update profile: process summary cap 128 reached; further slow updates omitted");
        }
    }
};

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

// WGR_OBJECT_STREAM_MIN_BATCH: how many objects an update must attempt before the time
// budget is allowed to stop it. See the long note at the admitBudgetSpent site — this floor,
// not the budget, is what pins the fill-in frame at 200 ms+.
//
// Defaults to `legacyBatch` (the per-update create count), which is exactly today's
// behaviour, so this is inert until somebody measures the trade.
static uint32_t ObjectStreamMinBatch(uint32_t legacyBatch)
{
    static const long parsed = []
    {
        const char* v = std::getenv("WGR_OBJECT_STREAM_MIN_BATCH");
        return v ? std::strtol(v, nullptr, 10) : -1;
    }();
    if (parsed < 0)
    {
        return legacyBatch;
    }
    // At least one, or an update with a spent budget would admit nothing at all and the
    // world would never finish streaming.
    return static_cast<uint32_t>(std::clamp<long>(parsed, 1, static_cast<long>(legacyBatch)));
}

// WGR_RESIDENCY_CANDIDATE_CACHE=0 rebuilds and re-sorts the residency candidate list on every
// update, as it used to. See the rebuild guard in UpdateModernObjectResidency.
static bool ResidencyCandidateCacheEnabled()
{
    static const bool on = []
    {
        const char* v = std::getenv("WGR_RESIDENCY_CANDIDATE_CACHE");
        return !(v && std::strcmp(v, "0") == 0);
    }();
    return on;
}

// ============================================================================================
// (c) THE HARD WALL-CLOCK CEILING ON ONE ADMISSION PASS.
//
// WGR_OBJECT_STREAM_MAX_MS. The admit FLOOR (WGR_OBJECT_STREAM_MIN_BATCH, default = the legacy
// 128) is mandatory: admitBudgetSpent() refuses to consult the clock until the floor has been
// attempted, so the advertised 4 ms budget is unreachable and a batch that happens to contain
// cold models costs whatever they cost. Measured on Reforger Everon: 580 ms for the mandatory
// 128, 145x over budget (see the note at the admitBudgetSpent site below).
//
// This is the ceiling that overrides the floor. It is NOT a second budget: below the floor,
// nothing stops admission except this ceiling, so a cheap all-warm batch still admits its full
// 128 at full speed and only an EXPENSIVE batch is cut short. That distinction is the whole
// reason for adding a ceiling instead of simply lowering the floor:
// WGR_OBJECT_STREAM_MIN_BATCH=1 was measured to halve the stutter but LENGTHEN the total fill
// by 39% (63 s -> 88 s), because it throttles the cheap batches too.
//
// 16 ms is chosen against the measured distribution, not from taste. RND-040 measured the
// median frame at 24-25 ms and the stall frames at 230-270 ms. A 16 ms ceiling turns a 250 ms
// frame into roughly 41 ms (24 fps instantaneous) while leaving four times the nominal 4 ms
// budget for admission, so it should cost far less fill time than MIN_BATCH=1 did. It cannot
// bound the frame below `ceiling + one object`, because one object must always be attempted or
// the fill would never terminate -- a single cold model is the residual tail and async
// admission is the only thing that removes it. That is (d): ObjectStreamPreparer moves the
// parse half of a cold model to worker threads and the admit loop skips, rather than waits
// for, a model whose IR is not ready (WGR_OBJECT_STREAM_ASYNC, ObjectStreamPrepare.hpp). The
// adapter and GPU half stays on this thread, so the residual after (d) is `ceiling + adapt +
// vertex buffers + registration` of one model, and the per-model split row says how big
// that is.
//
// 0 disables the ceiling and restores exactly today's behaviour, so both arms come from one
// binary.
static float ObjectStreamCeilingMs()
{
    static const float ms = []
    {
        const char* v = std::getenv("WGR_OBJECT_STREAM_MAX_MS");
        const float parsed = v ? static_cast<float>(std::atof(v)) : 16.0f;
        return parsed >= 0.0f ? parsed : 16.0f;
    }();
    return ms;
}

// (c3) COUNTED ADMISSION -- the default since 2026-09-02. WGR_OBJECT_STREAM_ADMIT=timed restores
// the two clocks above.
//
// Both the ordinary budget (WGR_OBJECT_STREAM_MS_PER_UPDATE) and the hard ceiling
// (WGR_OBJECT_STREAM_MAX_MS) are wall clocks, and a wall clock deciding how many objects the
// stream admits this tick makes the RESIDENT SET a function of CPU speed: two machines running
// the same mission under the same seed and the same fixed step contain different objects on the
// same tick, which rendering, collision and any future replay can all see. The AI stage lost its
// last wall clock in SIM-813; these were the last two that decide what the world contains.
//
// The counted mode keeps the two bounds that were already counts -- the attempt cap and the
// upload-byte ceiling -- and replaces both clocks with the thing they were approximating: how
// many COLD models one update may pay for. The cost of an admission is bimodal (a warm re-admit
// is ~0.03 ms, a cold model is tens of milliseconds of decode/convert/register), so "N cold
// models" is the unit the clocks were really rationing, and it means the same thing on every
// machine. Warm admissions are unlimited up to the cap, exactly as before.
//
// What this trades, stated plainly: on a slow machine the timed mode admitted FEWER objects per
// frame (pop-in, frames kept short); the counted mode admits the SAME objects per frame and lets
// the frame take what those cost. The default of two cold models per update is chosen against
// the timed defaults it replaces (16 ms ceiling against a 26 ms median cold create measured on
// Reforger Everon: the clock stopped after one expensive model, or several cheap ones), and the
// floor guarantee is preserved by construction -- the count is only consulted once a cold
// attempt has been made, so an update always makes progress and the fill always terminates.
//
// WGR_OBJECT_STREAM_COLD_PER_UPDATE sets the count. The preload mode ignores it, as it ignores
// every per-update bound.
static bool ObjectStreamAdmitCounted()
{
    static const bool counted = []
    {
        const char* v = std::getenv("WGR_OBJECT_STREAM_ADMIT");
        return !(v && std::strcmp(v, "timed") == 0);
    }();
    return counted;
}

static uint32_t ObjectStreamBankTablesPerUpdate()
{
    // Opt-in: three native pairs reduced maxima but moved work into the settled
    // phase and delayed initial coverage. Keep the baseline until individual
    // material preparation is cheaper, not merely spread across more frames.
    static const uint32_t limit = [] {
        const char* value = std::getenv("WGR_OBJECT_STREAM_TABLE_BATCH");
        return static_cast<uint32_t>(std::clamp<long>(value ? std::strtol(value, nullptr, 10) : 0, 0, 128));
    }();
    return limit;
}

static uint32_t ObjectStreamColdPerUpdate()
{
    static const uint32_t count = []
    {
        const char* v = std::getenv("WGR_OBJECT_STREAM_COLD_PER_UPDATE");
        const long parsed = v ? std::strtol(v, nullptr, 10) : 2;
        // At least one: an update that may pay for no cold model would admit nothing once only
        // cold placements remain, and an update that admits nothing makes no progress.
        return static_cast<uint32_t>(std::clamp<long>(parsed, 1, 4096));
    }();
    return count;
}

// (c2) THE PER-FRAME UPLOAD BYTE CEILING.
//
// WGR_OBJECT_STREAM_UPLOAD_MB. The wall-clock ceiling above bounds ADMISSIONS, but one
// admitted object can carry dozens of textures: measured on the 30 m/s Everon traverse
// (2026-08-30, capture asset-obs-vehicle30-r2), the worst single frame paid 36.2 MB /
// 73.3 ms of texture upload work with the 16 ms ceiling active. This ceiling reads the
// frame's cumulative upload bytes (Dev::TextureStreamCounters::frameUploadBytes, rolled by
// StreamingFrameRoll each frame -- deliberately including uploads that happened OUTSIDE
// admission, because the frame does not care who spent the bytes) and stops admitting once
// they are over budget. Same guarantees as the wall-clock ceiling: it may cut the floor
// short, at least one object is always attempted so the fill terminates, and the residual
// tail is `budget + one object's textures`.
//
// DEFAULT OFF, and that is a measurement, not caution: the A/B on the same traverse
// (asset-obs-vehicle30-budget16 vs -r2, 16 MB vs off) changed NOTHING -- peak frame
// 36.4 vs 36.2 MB, 43 uploads in both arms -- because the worst frame is ONE object
// (church_01.p3d: 43 textures, 66 ms tex, 285 ms total admission), and the residual
// tail of any admission ceiling is `budget + one object` by construction. The wall-clock
// ceiling already prevents the multi-object pileup this lever would bound, so on this
// workload it is a lever without a problem. It stays available for a workload where
// many small cold objects land in one frame; the enabled arm costs nothing when idle.
// The real target the A/B exposed is per-OBJECT admission cost (adapt + register on the
// main thread), which is the async-admission item, not a budget item.
static uint64_t ObjectStreamUploadBudgetBytes()
{
    static const uint64_t bytes = []
    {
        const char* v = std::getenv("WGR_OBJECT_STREAM_UPLOAD_MB");
        const double parsedMb = v ? std::atof(v) : 0.0;
        if (!(parsedMb > 0.0))
            return static_cast<uint64_t>(0); // 0 or nonsense = ceiling off (the default)
        return static_cast<uint64_t>(parsedMb * 1024.0 * 1024.0);
    }();
    return bytes;
}

// (a) WGR_OBJECT_STREAM_EVICT_CACHE: how many recently-vacated MODEL SHAPES to keep alive so a
// returning camera re-admits instead of rebuilding. See the long note on the members in
// Landscape.hpp. 0 disables the cache entirely and restores today's destructive eviction.
//
// WHAT THE DEFAULT OF 256 CLAIMS. It claims that on every world that uses this path -- Everon,
// Chernarus, Stratis, Sahrani, Takistan and CWA -- keeping the geometry of up to 256 distinct
// models resident beyond their last placement is affordable. The units matter: worlds name a
// few hundred to a few thousand distinct models (Reforger Everon: 1,132), so 256 is an
// overshoot of at most ~23% of Everon's ENTIRE model set, and much less on a world with more
// models. It is bounded by model count, so a world with millions of placements does not make it
// larger.
//
// The claim that has NOT been measured is the Chernarus one. Chernarus is texture-VRAM bound
// and currently needs a mip bias to render at all, so a cache that cost texture VRAM would be
// dangerous there specifically. Reading the code, this one should not: faces hold a raw
// `Texture *` into the bank (Poly.hpp:29), the bank owns those entries for its own lifetime and
// registers NO Free hook (TextureBankGL33_Core.cpp:69-74), so a texture's residency does not
// depend on a shape holding it and letting the shape die would not have freed it. What the
// cache retains is CPU-side geometry plus the shape's vertex buffers. That is an inference from
// code and not a measurement; Chernarus is where it must be checked, and
// WGR_OBJECT_STREAM_EVICT_CACHE=0 is the A/B.
static bool ObjectStreamPressureCacheTrimEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("WGR_OBJECT_STREAM_PRESSURE_CACHE_TRIM");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

static size_t ObjectStreamShapeCacheLimit()
{
    static const size_t limit = []
    {
        const char* v = std::getenv("WGR_OBJECT_STREAM_EVICT_CACHE");
        const long parsed = v ? std::strtol(v, nullptr, 10) : 256;
        return static_cast<size_t>(std::clamp<long>(parsed, 0, 100000));
    }();
    return limit;
}

// (b) WGR_OBJECT_STREAM_PREFETCH_SECONDS: how far AHEAD of the camera, in seconds of travel, to
// bias the residency window centre. 0 disables the lead and restores the reactive window.
//
// 2.0 s is the measured length of the stall train, not a guess: RND-040 binned frame cost by
// time since the last recentre and found 118 frames holding ~250 ms for a full two seconds
// after the log line. Leading by the same two seconds is what gives (c)'s per-frame ceiling
// somewhere to put the work: the ceiling SPREADS a recentre over more frames, and without a
// lead those extra frames are frames in which the objects are already needed and missing.
// (b) and (c) are a pair; (b) alone moves the same synchronous cost earlier without shrinking
// it, and (c) alone trades a stall for pop-in.
static float ObjectStreamPrefetchSeconds()
{
    static const float seconds = []
    {
        const char* v = std::getenv("WGR_OBJECT_STREAM_PREFETCH_SECONDS");
        const float parsed = v ? static_cast<float>(std::atof(v)) : 2.0f;
        return parsed >= 0.0f ? parsed : 2.0f;
    }();
    return seconds;
}

// (b) WGR_OBJECT_STREAM_PREFETCH_MIN_SPEED: the speed, in m/s, below which no lead is applied at
// all, with a linear ramp to full lead at twice that speed.
//
// The deadband is not timidity, it is the finding. RND-040 measured walking (5 m/s) at 46.6 fps
// -- FASTER than its own static phase -- and only vehicle speed (30 m/s) at 4 frames per median
// second. There is nothing to fix below ~8 m/s, and a lead that engaged there would buy nothing
// while adding recentres every time a walking camera started or stopped. The ramp (rather than
// a step) is what keeps a camera hovering around the threshold from oscillating the window
// centre.
static float ObjectStreamPrefetchMinSpeed()
{
    static const float speed = []
    {
        const char* v = std::getenv("WGR_OBJECT_STREAM_PREFETCH_MIN_SPEED");
        const float parsed = v ? static_cast<float>(std::atof(v)) : 8.0f;
        return parsed >= 0.0f ? parsed : 8.0f;
    }();
    return speed;
}

// (b) WGR_OBJECT_STREAM_PREFETCH_MAX_CELLS: absolute ceiling on the lead, in land cells. The
// lead is ALSO capped at a fraction of the effective desired radius (see
// PrefetchRadiusFraction), which is the cap that actually protects a small window; this one is
// the backstop for an implausible velocity estimate.
static int ObjectStreamPrefetchMaxCells()
{
    static const int cells = []
    {
        const char* v = std::getenv("WGR_OBJECT_STREAM_PREFETCH_MAX_CELLS");
        const long parsed = v ? std::strtol(v, nullptr, 10) : 8;
        return static_cast<int>(std::clamp<long>(parsed, 0, 4096));
    }();
    return cells;
}

// The lead may never exceed this fraction of the EFFECTIVE desired radius (the radius at which
// the desired set ran out of object budget, not the grown candidate window). Everything the
// window gives up at the back it gives up here, so this is the number that decides whether
// prefetch can cause pop-in BEHIND a moving camera.
//
// The desired radius is world-dependent and much smaller than the candidate window: ~900 m on
// Everon and ~550-711 m on Chernarus against a 3,200 m candidate window (see the window-growth
// note in UpdateModernObjectResidency). A fixed lead in metres would therefore be mild on Everon
// and severe on Chernarus, which is exactly the class of default this codebase keeps getting
// wrong. Expressed as a fraction, 0.35 leaves at least 65% of the trailing radius intact on
// every world -- ~585 m behind on Everon, ~360-460 m on Chernarus -- and at 30 m/s the
// two-second lead is only 60 m, so this cap does not even bind until roughly 100 m/s (aircraft).
static constexpr float PrefetchRadiusFraction = 0.35f;

// A camera step larger than this between two updates is not motion, it is a teleport, and the
// velocity estimator must not be poisoned by it. RND-040's own measuring driver did exactly
// this twice -- camCommit over one waypoint interval divides by ~0 on a slow frame -- and
// dragged residency to the map corner. Sized to be unreachable by honest motion: the worst
// frame in that run was 1.555 s, which at 30 m/s is 47 m and at an aircraft's 100 m/s is 156 m.
static constexpr float PrefetchTeleportMetres = 250.0f;
// Time constant of the velocity smoother, seconds. This is what makes reversal and hard turns
// degrade gracefully instead of snapping the window: the lead vector takes ~0.5 s to swing
// round, during which it passes through zero (the true camera position), so the camera is never
// left outside its own window.
static constexpr float PrefetchSmoothingTau = 0.5f;
// Longer than this between updates and there is no usable velocity to derive. Deliberately well
// above the 1.555 s worst frame RND-040 measured -- the estimator is most needed precisely when
// frames are slow, so a tight gap limit would disable prefetch during the stall it exists to
// prevent.
static constexpr float PrefetchMaxUpdateGapSeconds = 2.0f;

// (a) WGR_OBJECT_STREAM_EVICT_CACHE_LAST_ONLY=0 caches a model on ANY release rather than only
// on the release of its last holder. See the refcount test in RetainReleasedShape: that test is
// a heuristic about who else holds the shape, and if some subsystem this file does not know
// about keeps a transient Ref, the heuristic silently stops the cache filling at all. The log
// row reports `cache=held/limit`, so a run that reads 0/256 has an answer available in one
// restart instead of a code change.
static bool ObjectStreamCacheLastHolderOnly()
{
    static const bool on = []
    {
        const char* v = std::getenv("WGR_OBJECT_STREAM_EVICT_CACHE_LAST_ONLY");
        return !(v && std::strcmp(v, "0") == 0);
    }();
    return on;
}

// (a) Take ownership of a shape whose last streamed placement has just been released.
//
// SAFETY. The only thing this holds is a strong Ref to a LODShapeWithShadow, which is exactly
// what a resident Object holds. It hands nothing out, it never resurrects an Object, and it
// cannot double-free: ownership is the Ref in the list and nothing else, so dropping the entry
// is the whole release protocol. The shape pointer returned by a later ShapeBank::New is the
// same pointer any still-resident sibling placement would have got -- shape sharing between
// placements of one model is the normal case, not a new one.
//
// MUST be called while the dying Object still holds its own reference. The refcount test below
// depends on it, and so does the guarantee that `shape` is a live pointer at all.
void Landscape::RetainReleasedShape(LODShapeWithShadow* shape)
{
    if (_modernObjectShapeCacheLimit == 0 || shape == nullptr)
        return;
    if (ObjectStreamPressureCacheTrimEnabled() && _modernObjectShapeCachePressure.Active())
        return; // optional pressure state sheds only redundant last-holder cache refs
    const auto slot = _modernObjectShapeCacheIndex.find(shape);
    if (slot != _modernObjectShapeCacheIndex.end())
    {
        // Already held -- another placement of the same model was released earlier. Refresh
        // recency; do NOT insert a second Ref, or the cache would be bounded by placements
        // (millions) rather than by distinct models (hundreds).
        _modernObjectShapeCache.splice(_modernObjectShapeCache.end(), _modernObjectShapeCache, slot->second);
        return;
    }
    // CACHE ONLY THE MODEL THAT IS ACTUALLY ABOUT TO DIE.
    //
    // A recentre at 30 m/s releases roughly 3,700 objects (77,643 over 21 recentres, RND-040)
    // but only a few hundred distinct models, and most of those models still have placements
    // resident elsewhere in the window -- they are in no danger of being destroyed and caching
    // them would spend every slot on models that did not need one, evicting the rare models
    // that did. This is the whole difference between a cache that survives a recentre and one
    // that is churned by it.
    //
    // The caller guarantees the dying Object still holds its Ref, so a count of exactly 1 means
    // "this Object is the only holder and the shape dies with it" -- precisely the case worth
    // paying memory for. The test errs toward NOT caching: any other live reference, including
    // a transient one taken by a subsystem this file knows nothing about, reads as "someone else
    // is keeping it alive" and the model is skipped. That is a missed optimisation, never a
    // correctness problem, and WGR_OBJECT_STREAM_EVICT_CACHE_LAST_ONLY=0 is the A/B if the log
    // shows the cache never filling.
    if (ObjectStreamCacheLastHolderOnly() && shape->RefCounter() > 1)
        return;
    _modernObjectShapeCache.emplace_back(shape);
    _modernObjectShapeCacheIndex.emplace(shape, std::prev(_modernObjectShapeCache.end()));
    ++_modernObjectShapeCacheInserts;
    TrimModernObjectShapeCache(_modernObjectShapeCacheLimit);
}

// (a) Hand a shape back to the Object that has just been admitted on it.
//
// The Object now owns the shape, so the cache entry is redundant and is dropped. Keeping it
// would let the cache fill with shapes that are resident anyway, which costs nothing in memory
// but silently stops the cache protecting anything -- the slots would all be spent on models
// that were in no danger of being destroyed. Returns true if this admission was a hit, i.e. an
// ODOL decode, LOD conversion and vertex-buffer build that did not happen.
bool Landscape::AdoptCachedShape(LODShapeWithShadow* shape)
{
    if (shape == nullptr)
        return false;
    const auto slot = _modernObjectShapeCacheIndex.find(shape);
    if (slot == _modernObjectShapeCacheIndex.end())
        return false;
    _modernObjectShapeCache.erase(slot->second);
    _modernObjectShapeCacheIndex.erase(slot);
    ++_modernObjectShapeCacheHits;
    return true;
}

// (a) Drop the coldest entries until at most `keep` remain. Returns the number dropped.
//
// Dropping an entry is not a destruction: it releases one reference. If any Object is still
// resident on that model the shape lives on exactly as before, and if none is, the shape is
// destroyed at precisely the moment it would have been destroyed without this cache. That is
// the argument that this cannot free something still in use -- the cache never shortens a
// lifetime, it only ever extends one.
size_t Landscape::TrimModernObjectShapeCache(size_t keep)
{
    size_t dropped = 0;
    while (_modernObjectShapeCache.size() > keep)
    {
        LODShapeWithShadow* victim = _modernObjectShapeCache.front().GetRef();
        _modernObjectShapeCacheIndex.erase(victim);
        _modernObjectShapeCache.pop_front();
        ++_modernObjectShapeCacheDrops;
        ++dropped;
    }
    return dropped;
}

// (d) Hand the preparer this world's model table. Creating the preparer is what starts the
// worker threads, so under WGR_OBJECT_STREAM_ASYNC=0 it is never created at all and the admit
// loop below sees `asyncOn == false` -- the old loop, with the same engine calls in the same
// order (plus a ShapeBank cache lookup per placement that feeds the split timers).
void Landscape::EnsureModernObjectPreparer()
{
    if (!ObjectStreamPreparer::AsyncEnabled())
        return;
    if (!_modernObjectPreparer)
        _modernObjectPreparer = std::make_unique<ObjectStreamPreparer>();
    std::vector<std::string> paths;
    paths.reserve(_modernObjectModels.size());
    for (const RStringB& model : _modernObjectModels)
        paths.emplace_back(model.GetLength() > 0 ? (const char*)model : "");
    ResetModernSourceDiagnostics();
    _modernObjectPreparer->Reset(paths.data(), paths.size());
    // The prepared-texture store belongs to the same lifetime: chains for a previous world's
    // textures are never wanted by this one.
    render::PreparedTextureStore::Instance().Clear();
    LOG_INFO(World,
             "Object stream prepare: {} workers armed for {} models (queue<={} ready<={}) "
             "texture prepare={} (budget={} MB ttl={:.0f} s)",
             ObjectStreamPreparer::WorkerCount(), paths.size(), ObjectStreamPreparer::QueueLimit(),
             ObjectStreamPreparer::ReadyLimit(), render::PreparedTextureStore::Enabled() ? "on" : "off",
             render::PreparedTextureStore::ByteBudget() / (1024 * 1024), render::PreparedTextureStore::TtlSeconds());
}

// (d) Give the workers lead time. Called once per recentre, after the desired set is known:
// every distinct model that has a non-resident placement in a desired, not-yet-active cell and
// is not resident in ShapeBank is requested, in candidate (distance) order, so the queue is
// sorted the way the admit loop will ask. The epoch stamp makes this one request per model per
// recentre, not per placement; the admit loop's own on-demand Request covers anything this
// walk did not reach (a full queue, or a model that went cold since).
void Landscape::PrefetchModernObjectModels(const std::vector<ModernObjectCellCandidate>& candidates)
{
    if (!_modernObjectPreparer || !_modernObjectPreparer->Running())
        return;
    if (_modernObjectModelPrefetchEpoch.size() != _modernObjectModels.size())
        _modernObjectModelPrefetchEpoch.assign(_modernObjectModels.size(), 0);
    ++_modernObjectPrefetchEpoch;
    // Pass 1: the distinct cold models of the new desired set, nearest first. Stamping is what
    // makes this one entry per model, and the stamp is also what DropStale reads.
    std::vector<uint32_t> wanted;
    const bool warmTextures = ObjectStreamPreparer::WarmTextureJobsEnabled();
    using WarmModel = std::pair<uint32_t, const LODShapeWithShadow*>;
    std::optional<Streaming::WarmModelCaptureWindow<WarmModel>> warmWanted;
    ObjectStreamPreparer::WarmCaptureCursor warmCursor;
    if (warmTextures)
    {
        warmCursor = _modernObjectPreparer->QueryWarmTextureCaptureCursor();
        warmWanted.emplace(warmCursor.ordinal);
    }
    uint32_t alreadyWarm = 0;
    for (const ModernObjectCellCandidate& cell : candidates)
    {
        if (!_modernObjectCellDesired[cell.index] || _modernObjectCellActive[cell.index])
            continue;
        for (uint32_t index : _modernObjectCells[cell.index])
        {
            const ModernObjectPlacement& placement = _modernObjectPlacements[index];
            if (placement.resident || placement.modelIndex >= _modernObjectModels.size())
                continue;
            const uint32_t model = placement.modelIndex;
            if (_modernObjectModelPrefetchEpoch[model] == _modernObjectPrefetchEpoch)
                continue;
            _modernObjectModelPrefetchEpoch[model] = _modernObjectPrefetchEpoch;
            if (_modernPendingOwnerPrimaryTextures &&
                _modernPendingOwnerPrimaryTextures->modelIndex == model)
                continue; // Taken Converted belongs to the one owner staging slot.
            if (const auto* warm = Shapes.Find(_modernObjectModels[model], false, true))
            {
                ++alreadyWarm;
                if (warmWanted) warmWanted->Observe({model, warm}); // Borrowed only until this owner call returns.
                continue;
            }
            wanted.push_back(model);
        }
    }
    // Drop what the previous window asked for and this one does not: queued requests and
    // ready IRs for models with no non-resident placement in the new desired set. Without
    // this the ready set (bounded, and the workers park when it is full) could fill with
    // models nobody will Take, and the models this window needs would sit behind them.
    auto wantedEpochs = _modernObjectModelPrefetchEpoch;
    for (size_t model = 0; model < _nativeFarModelWanted.size() && model < wantedEpochs.size(); ++model)
        if (_nativeFarModelWanted[model])
            wantedEpochs[model] = _modernObjectPrefetchEpoch;
    PreserveModernSimulationModelRequests(wantedEpochs, _modernObjectPrefetchEpoch);
    PreserveModernSourceDiagnosticRequests(wantedEpochs, _modernObjectPrefetchEpoch);
    const size_t staleDropped = _modernObjectPreparer->DropStale(
        wantedEpochs.data(), wantedEpochs.size(), _modernObjectPrefetchEpoch);
    // Pass 2: enqueue in that order. A full queue is not an error -- the admit loop
    // re-requests on demand when it reaches a placement whose model is still Unknown.
    uint32_t requested = 0;
    for (uint32_t model : wanted)
    {
        if (_modernObjectPreparer->Query(model) != ObjectStreamPreparer::State::Unknown)
            continue;
        if (!_modernObjectPreparer->Request(model))
            break;
        ++requested;
    }
    if (warmTextures && GEngine)
    {
        size_t workRemaining = 256; // Shared LOD/section/binding/source visits for this entire recentre.
        size_t stageSourceAttemptsRemaining = 2; // Only explicit opt-in WGPU preflight consumes this.
        const auto capture = warmWanted->CaptureWhile([&] { return workRemaining != 0; },
            [&](const WarmModel& candidate)
            {
                const auto [model, shape] = candidate;
                std::vector<WarmTextureRead> reads;
                GEngine->CaptureWarmTextureReads(shape, reads, workRemaining, &stageSourceAttemptsRemaining);
                if (!reads.empty())
                    _modernObjectPreparer->SubmitWarmTextures(model,
                        _modernObjectPreparer->QueryRadius(model), std::move(reads));
            }); // Empty/refused/thrown attempts still advance; all retain ordinary fallback.
        _modernObjectPreparer->RecordWarmTextureCaptureCursor(warmCursor, capture.nextCursor);
    }
    if (requested > 0 || staleDropped > 0)
        LOG_INFO(World,
                 "Object stream prepare: recentre wants {} cold models ({} already warm); requested {} ahead, "
                 "dropped {} stale",
                 wanted.size(), alreadyWarm, requested, staleDropped);
}

// The item budget, clamped by what the GPU can actually hold (roadmap Phase 7:
// "use CPU and GPU memory budgets rather than arbitrary item-count limits").
//
// `_modernObjectBudget` is a COUNT -- 20,000 placements -- and until the renderer's
// budget became a measurement rather than a fixed 2 GB guess there was nothing
// trustworthy to clamp it against. Now there is: the renderer probes the device's
// device-local heap and takes a documented share of it. Measured on the 30 m/s
// Everon traverse, 2026-08-31: 119.9 KB per resident object, so 20,000 items
// promises ~2.3 GB -- comfortably inside the 5,611 MB probed on this machine, but
// NOT inside a 4 GB card's share, and not on a texture-heavier corpus.
//
// The clamp therefore does nothing on hardware with room (it must not: shrinking a
// resident set that fits is pop-in bought for nothing) and engages only when the
// measured cost per object says the count would not fit. HYSTERESIS is the whole
// point of the two water marks: releasing at the same threshold that admits would
// let the set oscillate across a recentre boundary, which is precisely the
// "repeatedly loaded and evicted" the roadmap names.
//
// Per-object cost is measured, not assumed: tracked bytes / resident objects, from
// the renderer's own totals. Before anything is resident there is no measurement,
// so the clamp stays out of the way until the sample is meaningful.
static uint32_t GpuClampedObjectBudget(uint32_t itemBudget, uint32_t residentNow, uint32_t& lastEffective)
{
    // OPT-IN (WGR_OBJECT_STREAM_GPU_BUDGET=1), and the reason is measured, not cautious.
    //
    // The clamp behaves correctly where it must: on this machine's probed 5,611 MB
    // budget it fires ZERO times over the traverse -- 20,000 objects at the measured
    // ~150 KB each project well inside it, so a set that fits is never shrunk. Good.
    //
    // Under real pressure (WGR_DYNAMIC_VRAM_MB=1200) it clamps 20,000 -> 13,608, and
    // then it OSCILLATES: 96 budget changes over a 200 s traverse, roughly one every
    // two seconds. The loop is self-driving -- shrinking the set lowers the resident
    // count, which changes the measured per-object cost, which moves the projection
    // back under the low-water mark, which restores the full count, which grows the
    // set again. The two water marks give hysteresis to the THRESHOLD but not to the
    // DECISION, and "repeatedly loaded and evicted" is precisely what Phase 7 says to
    // avoid. The resident count also did not fall the way the desired-set count did,
    // so the release path's response to a lowered budget is not yet understood.
    //
    // What it needs before it can ship on: a dwell time on the decision (N consecutive
    // updates past a mark before acting), a per-object cost averaged over a window
    // rather than the instantaneous ratio, and a measurement of what the release path
    // actually does when the desired set shrinks. All three are cheap; none of them
    // are things to guess at 4 a.m. and leave in a build the owner wakes up to.
    static const bool enabled = []
    {
        const char* v = std::getenv("WGR_OBJECT_STREAM_GPU_BUDGET");
        return v && v[0] == '1';
    }();
    if (!enabled || !GEngine)
        return itemBudget;
    Engine::GpuMemoryStatsOut mem;
    if (!GEngine->GetGpuMemoryStats(mem) || mem.budgetBytes == 0)
        return itemBudget; // backend reports nothing (GL33): the count is all we have
    // TWO corrections learned by measuring, both of which the first version got wrong
    // and which made the clamp fire (20,000 -> 15,136) on a machine with 5.6 GB of
    // room -- the exact pop-in-for-nothing this must never cause:
    //
    // 1. Divide by the OBJECT-ATTRIBUTABLE bytes, not `trackedBytes`. Tracked also
    //    carries terrain and everything else the renderer accounts for, so early in a
    //    fill that fixed overhead is spread over a handful of objects and the average
    //    comes out several times too high.
    // 2. Only sample a set that is actually FULL. At 10% filled the per-object figure
    //    describes the first, nearest, most expensive objects, not the mix a full
    //    budget holds. Half the item budget is the earliest point the ratio means
    //    anything.
    const uint64_t objectBytes = mem.objectTextureBytes + mem.geometryLiveBytes;
    if (residentNow < itemBudget / 2 || objectBytes == 0)
        return itemBudget;
    const uint64_t perObject = objectBytes / residentNow;
    if (perObject == 0)
        return itemBudget;
    // Two marks, and the gap between them IS the hysteresis: shrink only when the
    // projected cost of the FULL item budget passes the high mark, and only grow
    // back once the projection falls under the low one.
    const uint64_t high = mem.budgetBytes; // projected cost may not exceed the budget
    const uint64_t low = mem.budgetBytes * 85 / 100;
    const uint64_t projected = perObject * itemBudget;
    uint32_t effective = lastEffective != 0 ? lastEffective : itemBudget;
    if (projected > high)
    {
        // Fit the count to the budget, never below a floor that keeps the near
        // window populated -- an empty world is worse than an over-budget one.
        const uint64_t fits = mem.budgetBytes / perObject;
        const uint32_t target = static_cast<uint32_t>(std::clamp<uint64_t>(fits, std::min(2000u, itemBudget), itemBudget));
        effective = std::min(effective, target);
    }
    else if (projected < low)
    {
        effective = itemBudget; // pressure gone: restore the configured count
    }
    lastEffective = effective;
    return effective;
}

bool Landscape::SetObjectStreamTestBudget(uint32_t budget)
{
    const char* enabled = std::getenv("WGR_OBJECT_STREAM_TEST_BUDGET");
    if (!enabled || enabled[0] != '1' || !_modernObjectStreaming || budget < 2000 || budget > 20000)
        return false;
    _modernObjectBudget = budget;
    return true;
}

void Landscape::UpdateModernObjectResidency(Vector3Par cameraPosition)
{
    if (!_modernObjectStreaming || _modernObjectCells.empty())
    {
        _modernPendingOwnerPrimaryTextures.reset();
        // (d) Same teardown argument as the cache below: a preparer still holding a dead
        // world's IRs, or a queue of its models, must be flushed. Reset only bumps a
        // generation and drops results; it never joins, so this costs nothing per frame.
        ResetModernSourceDiagnostics();
        if (_modernObjectPreparer && _modernObjectPreparer->ModelCount() > 0)
            _modernObjectPreparer->Reset(nullptr, 0);
        // (a) STREAMING IS OFF, SO THE CACHE MUST NOT SURVIVE. This is reached both on a world
        // that does not stream at all (the CWA islands) and on the frame after a world is torn
        // down: Landscape's reset clears the placement database and sets _modernObjectStreaming
        // false (Landscape.cpp:267-285) but knows nothing about this cache, so without this the
        // shapes of a world that no longer exists would be held for the rest of the session --
        // and Shape.hpp:1098-1104 warns specifically that a shape outliving its display
        // transition is how face Texture* pointers come to dangle.
        //
        // The flush belongs one line above, in that reset, which is the file that owns the
        // teardown. It is here because this session's file ownership stops at the residency
        // system; this function runs every frame from TerrainWgpu::DrawTerrain, so the cache is
        // released on the first frame after teardown rather than during it, and the cache is
        // never drawn from in between.
        if (!_modernObjectShapeCache.empty())
            TrimModernObjectShapeCache(0);
        if (ObjectStreamPressureCacheTrimEnabled())
            _modernObjectShapeCachePressure.Reset();
        return;
    }
    // Optional cache-only pressure response. Published scalar facts do not open a renderer
    // producer window. A dropped Shape Ref is NOT a physical pool-reclaim acknowledgement.
    if (ObjectStreamPressureCacheTrimEnabled())
    {
        Engine::GpuMemoryStatsOut memory{};
        const bool available = GEngine && GEngine->GetGpuMemoryStats(memory);
        _modernObjectShapeCachePressure.Observe(available, memory.trackedBytes, memory.budgetBytes);
        if (_modernObjectShapeCachePressure.DropCount(_modernObjectShapeCache.size()))
        {
            const size_t before = _modernObjectShapeCache.size();
            TrimModernObjectShapeCache(before - 1); // at most one cache reference this update
            static unsigned rows = 0;
            if (rows < 16)
            {
                ++rows;
                LOG_INFO(World, "Object stream pressure cache: droppedRef=1 cacheBefore={} cacheAfter={} trackedBytes={} budgetBytes={} physicalReclaim=unknown row={} limit=16",
                         before, _modernObjectShapeCache.size(), memory.trackedBytes,
                         memory.budgetBytes, rows);
            }
            else if (rows == 16)
            {
                ++rows;
                LOG_INFO(World, "Object stream pressure cache: log truncated after 16 drops; cache refs are not GPU reclaim evidence");
            }
        }
    }
    Vector3 effectivePosition = cameraPosition;
    // Deterministic residency-turnover smoke hook. It visits a non-overlapping
    // cell window after 180 updates and returns after 360, while the real camera
    // stays fixed so the final screenshot remains directly comparable. Normal
    // gameplay never reads this path unless the explicit diagnostic env is set.
    static const char* const sweepCells = std::getenv("WGR_OBJECT_STREAM_TEST_SWEEP_CELLS");
    if (const char* sweep = sweepCells)
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
    // ==========================================================================================
    // (b) PREFETCH: LEAD THE CAMERA INSTEAD OF FOLLOWING IT.
    //
    // The window below is REACTIVE. It quantises the centre to 4 cells and early-outs until the
    // camera has ALREADY crossed a boundary, so the objects on the far side of that boundary are
    // admitted at the moment they are first needed and every admit is on the critical path.
    // RND-040 showed the shape of the damage: spike SIZE is constant (~230-270 ms) and only
    // FREQUENCY scales with speed -- 21 recentres in 112 s at 30 m/s against 2 in 119 s at
    // 5 m/s -- and each recentre starts a ~2 s train of ~250 ms frames. This is the only one of
    // the four causes that targets SPEED rather than cost.
    //
    // The velocity is derived here because the function's only input is a position
    // (TerrainWgpu.cpp:2485) and this file owns the residency system; nothing upstream has to
    // change to feed it.
    //
    // HOW IT DEGRADES, which is the whole design:
    //   stationary  -- speed falls under the deadband, the lead ramps to zero and the centre is
    //                  the camera's own cell, i.e. exactly today's behaviour. Coming to a stop
    //                  costs at most one extra recentre as the lead unwinds, and (a) makes that
    //                  recentre a cache hit rather than a rebuild.
    //   reversing   -- the smoother turns the lead vector over ~0.5 s and it passes THROUGH zero
    //                  on the way, so the window centre sweeps back across the camera rather
    //                  than jumping past it. The camera is never outside its own window.
    //   sharp turn  -- the estimate lags for ~0.5 s, so the lead points at the old heading and
    //                  the centre sits off to one side by at most the lead distance. Bounded by
    //                  the same cap as everything else; the cost is a few extra recentres during
    //                  the turn, which is the trade (a) exists to make cheap.
    //   teleport    -- rejected outright rather than smoothed, see PrefetchTeleportMetres.
    // SIMULATION time, not the wall clock. The velocity estimate decides which cells the
    // prefetch lead admits and evicts, so a wall-clock dt made the resident set a function of
    // frame rate: the same flight on a faster machine took more samples of a shorter interval
    // and smoothed differently, and the window centre moved differently with it. Glob.time
    // advances with the fixed step, so under lockstep two runs feed this estimator identical
    // intervals. Two rendered frames inside one simulation tick (or a paused game) produce
    // dt == 0: that is not a discontinuity, it is the absence of new information, so the
    // estimate AND the last sample are both kept -- overwriting the sample without advancing
    // the time would make the next interval measure a partial displacement over a full dt.
    const Poseidon::Foundation::Time residencyNow = Glob.time;
    const float rawCameraX = effectivePosition.X();
    const float rawCameraZ = effectivePosition.Z();
    bool sampleTaken = true;
    if (!_modernObjectVelocityValid)
    {
        _modernObjectVelocityValid = true;
        _modernObjectVelocityX = 0.0f;
        _modernObjectVelocityZ = 0.0f;
    }
    else
    {
        const float dt = residencyNow - _modernObjectLastCameraTime;
        if (dt <= 0.0f)
        {
            sampleTaken = false;
        }
        else
        {
            const float dx = rawCameraX - _modernObjectLastCameraX;
            const float dz = rawCameraZ - _modernObjectLastCameraZ;
            const float step = std::sqrt(dx * dx + dz * dz);
            if (dt > PrefetchMaxUpdateGapSeconds || step > PrefetchTeleportMetres)
            {
                // A discontinuity. Drop to zero velocity rather than to a wrong one: a wrong
                // lead is worse than no lead, because it evicts the band the camera is
                // actually in.
                _modernObjectVelocityX = 0.0f;
                _modernObjectVelocityZ = 0.0f;
            }
            else
            {
                // Exponential smoothing with a time constant rather than a fixed per-frame
                // weight, because this runs once per RENDERED frame and how much simulation
                // time passes between two of those is precisely what is unstable here. A fixed
                // weight would smooth over 0.1 s at 40 fps and over 2 s at 4 fps -- least
                // responsive exactly during the stall.
                const float alpha = 1.0f - std::exp(-dt / PrefetchSmoothingTau);
                _modernObjectVelocityX += (dx / dt - _modernObjectVelocityX) * alpha;
                _modernObjectVelocityZ += (dz / dt - _modernObjectVelocityZ) * alpha;
            }
        }
    }
    if (sampleTaken)
    {
        _modernObjectLastCameraX = rawCameraX;
        _modernObjectLastCameraZ = rawCameraZ;
        _modernObjectLastCameraTime = residencyNow;
    }

    _modernObjectLeadMetres = 0.0f;
    // The residency-turnover smoke hook above exists to make two screenshots directly
    // comparable, and it works by TELEPORTING the effective centre on a phase change. A velocity
    // estimator fed by teleports is meaningless, and a lead applied on top of the sweep offset
    // would move the window the test is trying to pin. Prefetch is therefore off whenever that
    // diagnostic is armed, so the existing smoke test measures exactly what it measured before.
    const float prefetchSeconds = sweepCells == nullptr ? ObjectStreamPrefetchSeconds() : 0.0f;
    if (prefetchSeconds > 0.0f)
    {
        const float speed = std::sqrt(_modernObjectVelocityX * _modernObjectVelocityX +
                                      _modernObjectVelocityZ * _modernObjectVelocityZ);
        const float minSpeed = ObjectStreamPrefetchMinSpeed();
        // Ramp from no lead at minSpeed to full lead at twice minSpeed. See
        // ObjectStreamPrefetchMinSpeed for why walking must be left alone.
        const float engage =
            minSpeed <= 0.0f ? 1.0f : std::clamp((speed - minSpeed) / minSpeed, 0.0f, 1.0f);
        if (speed > 0.001f && engage > 0.0f)
        {
            // Cap against the EFFECTIVE desired radius (last update's, which is stable for a
            // world) and not the grown candidate window. On the first update of a world the
            // desired radius is not known yet, so fall back to the configured start radius.
            const float radiusCells = _modernObjectDesiredRadiusCells > 0
                                          ? static_cast<float>(_modernObjectDesiredRadiusCells)
                                          : static_cast<float>(_modernObjectRadius);
            const float capCells =
                std::min(static_cast<float>(ObjectStreamPrefetchMaxCells()), radiusCells * PrefetchRadiusFraction);
            const float lead = std::min(speed * prefetchSeconds * engage, std::max(0.0f, capCells * _landGrid));
            effectivePosition[0] += _modernObjectVelocityX / speed * lead;
            effectivePosition[2] += _modernObjectVelocityZ / speed * lead;
            _modernObjectLeadMetres = lead;
        }
    }

    constexpr int QuantizeCells = 4;
    int centerX = std::clamp(static_cast<int>(std::floor(effectivePosition.X() * _invLandGrid)), 0, _landRange - 1);
    int centerZ = std::clamp(static_cast<int>(std::floor(effectivePosition.Z() * _invLandGrid)), 0, _landRange - 1);
    centerX = (centerX / QuantizeCells) * QuantizeCells;
    centerZ = (centerZ / QuantizeCells) * QuantizeCells;
    const bool centerChanged = centerX != _modernObjectCenterX || centerZ != _modernObjectCenterZ;

    // PRELOAD MODE. Off by default; WGR_OBJECT_STREAM_PRELOAD=1 turns it on.
    //
    // The per-update caps exist to keep a frame short, and they do: the world fills in
    // gradually behind the camera. The cost is that the fill-in TAKES that long. This mode
    // trades the shape of that cost rather than its size -- pay it in a few very long
    // frames at the start, when nobody is aiming at anything, instead of in hundreds of
    // merely bad ones while they are. Measured on Stratis: full residency in 8 update
    // rounds against 21.
    // Generous against the 7-8 updates a full window takes, and small enough that the mode
    // cannot survive into play on a world that never goes idle.
    constexpr uint64_t kPreloadMaxUpdates = 64;
    static const bool preloadRequested = []
    {
        const char* v = std::getenv("WGR_OBJECT_STREAM_PRELOAD");
        return v != nullptr && v[0] == '1';
    }();
    const bool preloading = preloadRequested && !_modernObjectPreloadDone;

    // Budget is part of demand, even after the admission backlog has converged.
    // The GPU estimator is still opt-in; with it disabled this is a count read,
    // with no renderer query or candidate scan on the settled fast path.
    // Phase 7: the count, clamped by measured GPU cost (no-op when it fits).
    const uint32_t effectiveBudget =
        GpuClampedObjectBudget(_modernObjectBudget, _modernResidentObjectCount,
                   _modernObjectEffectiveBudget);
    if (effectiveBudget != _modernObjectBudget && effectiveBudget != _modernObjectBudgetLogged)
    {
        _modernObjectBudgetLogged = effectiveBudget;
        LOG_INFO(World,
             "Modern object residency: GPU budget clamps the item budget {} -> {} "
             "(measured cost per resident object; WGR_OBJECT_STREAM_GPU_BUDGET=0 disables)",
             _modernObjectBudget, effectiveBudget);
    }
    const bool demandChanged = centerChanged || effectiveBudget != _modernObjectDesiredBudget;
    if (!demandChanged && !_modernObjectResidencyPending)
    {
        return;
    }
    if (centerChanged)
    {
        _modernObjectCenterX = centerX;
        _modernObjectCenterZ = centerZ;
    }

    using CellCandidate = ModernObjectCellCandidate;
    // Candidate-window growth depends on both center and effective budget.
    // Pending admission alone must not repeat the gather/sort; a changed budget
    // must invalidate it even if the camera is stationary.
    const bool candidateCacheOn = ResidencyCandidateCacheEnabled();
    const bool rebuildCandidates = !candidateCacheOn || !_modernObjectCandidatesValid || centerChanged ||
                                   _modernObjectCandidateCenterX != centerX || _modernObjectCandidateCenterZ != centerZ ||
                                   _modernObjectCandidateBudget != effectiveBudget;
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
    if (rebuildCandidates)
    {
        const auto enclosedPlacements = [&](int radius)
        {
            const int ex0 = std::max(0, centerX - radius);
            const int ez0 = std::max(0, centerZ - radius);
            const int ex1 = std::min(_landRange, centerX + radius + 1);
            const int ez1 = std::min(_landRange, centerZ + radius + 1);
            uint64_t total = 0;
            for (int z = ez0; z < ez1 && total < effectiveBudget; ++z)
                for (int x = ex0; x < ex1; ++x)
                    total += _modernObjectCells[static_cast<size_t>(z) * _landRange + x].size();
            return total;
        };
        int windowRadius = std::clamp(_modernObjectRadius, 1, _landRange);
        if (_modernObjectWindowGrowth)
            while (windowRadius < _landRange && enclosedPlacements(windowRadius) < effectiveBudget)
                windowRadius = std::min(_landRange, windowRadius * 2);

        const int x0 = std::max(0, centerX - windowRadius);
        const int z0 = std::max(0, centerZ - windowRadius);
        const int x1 = std::min(_landRange, centerX + windowRadius + 1);
        const int z1 = std::min(_landRange, centerZ + windowRadius + 1);
        _modernObjectCandidates.clear();
        _modernObjectCandidates.reserve(static_cast<size_t>(x1 - x0) * (z1 - z0));
        for (int z = z0; z < z1; ++z)
        {
            for (int x = x0; x < x1; ++x)
            {
                const uint32_t index = static_cast<uint32_t>(z * _landRange + x);
                if (_modernObjectCells[index].empty())
                    continue;
                const int dx = x - centerX;
                const int dz = z - centerZ;
                _modernObjectCandidates.push_back({index, static_cast<uint32_t>(dx * dx + dz * dz)});
            }
        }
        std::sort(_modernObjectCandidates.begin(), _modernObjectCandidates.end(),
                  [](const CellCandidate& a, const CellCandidate& b)
                  { return a.distance2 < b.distance2 || (a.distance2 == b.distance2 && a.index < b.index); });
        _modernObjectCandidateCenterX = centerX;
        _modernObjectCandidateCenterZ = centerZ;
        _modernObjectCandidateRadius = windowRadius;
        _modernObjectCandidateBudget = effectiveBudget;
        _modernObjectCandidatesValid = true;
    }
    const std::vector<CellCandidate>& candidates = _modernObjectCandidates;

    uint32_t requested = 0;
    if (demandChanged)
    {
        std::fill(_modernObjectCellDesired.begin(), _modernObjectCellDesired.end(), 0);
        // (b) Record where the desired set actually ran out of budget. The candidates are
        // distance-sorted, so the last cell admitted carries the largest squared cell distance
        // and that is the EFFECTIVE window radius -- the one the prefetch lead is capped
        // against. It is much smaller than the candidate window (~18 cells against 64 on
        // Everon), and nothing reported it before, which is how a lead expressed in metres would
        // have been mild on one world and pop-in on another.
        uint32_t desiredRadius2 = 0;
        for (const CellCandidate& cell : candidates)
        {
            const uint32_t count = static_cast<uint32_t>(_modernObjectCells[cell.index].size());
            if (requested != 0 && requested + count > effectiveBudget)
                break;
            _modernObjectCellDesired[cell.index] = 1;
            requested += count;
            desiredRadius2 = std::max(desiredRadius2, cell.distance2);
        }
        _modernObjectDesiredRadiusCells = static_cast<uint32_t>(std::sqrt(static_cast<float>(desiredRadius2)));
    }
    else
    {
        for (const CellCandidate& cell : candidates)
            if (_modernObjectCellDesired[cell.index])
                requested += static_cast<uint32_t>(_modernObjectCells[cell.index].size());
    }

    _modernObjectDesiredCount = requested;
    _modernObjectDesiredBudget = effectiveBudget;
    // The slot owns a converted shape that is deliberately absent from ShapeBank. A window
    // change can make its originating placement irrelevant before its next upload.
    if (_modernPendingOwnerPrimaryTextures)
    {
        const auto& pending = *_modernPendingOwnerPrimaryTextures;
        const bool wanted = pending.cellIndex < _modernObjectCellDesired.size() &&
            _modernObjectCellDesired[pending.cellIndex] &&
            pending.placementIndex < _modernObjectPlacements.size() &&
            !_modernObjectPlacements[pending.placementIndex].resident &&
            _modernObjectPlacements[pending.placementIndex].modelIndex == pending.modelIndex;
        if (!wanted || pending.modelIndex >= _modernObjectModels.size() ||
            Shapes.Find(_modernObjectModels[pending.modelIndex], false, true))
        {
            LOG_INFO(World, "DayZ owner primary stage: cancelled modelIndex={} uploaded={} remaining={} reservedBytes={}",
                     pending.modelIndex, pending.uploads, pending.textures.size() - pending.next,
                     pending.reservedBytes);
            _modernPendingOwnerPrimaryTextures.reset();
        }
    }
    uint32_t released = 0;
    if (demandChanged)
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
                if (!object || _modernLogicalOnlyObjects.contains(placement.id))
                    continue;
                // Camera eviction may retire the visual, never a required identity or
                // non-default state that save/load must still be able to discover.
                if (_modernRequiredObjects.contains(placement.id) || object->MustBeSaved() || HasModernSimulationLease(index))
                {
                    if (GEngine)
                        GEngine->SceneObjectRemoved(object);
                    object->SetVisualResident(false);
                    _modernLogicalOnlyObjects.insert(placement.id);
                    _modernObjectLightOwners.erase(placement.id);
                    if (index < _nativeFarWanted.size() && object->IsDestroyed())
                        _nativeFarWanted[index] |= 2;
                    ++released;
                    continue;
                }
                Ref<Object> keepAlive = object;
                // (a) Take a reference to the MODEL before the Object that owns it dies at the
                // end of this iteration. This is the whole eviction cache: `keepAlive` still
                // holds the Object here, so the shape is guaranteed alive, and the cache's Ref
                // is taken while it is. Nothing about the Object is retained -- it is destroyed
                // exactly as before, on the same line, in the same frame.
                LODShapeWithShadow* releasedShape = object->GetShape();
                if (index < _nativeFarWanted.size())
                {
                    // Bit 2 persists across camera windows: never resurrect an observed wreck.
                    if (object->IsDestroyed())
                        _nativeFarWanted[index] |= 2;
                    else if ((_nativeFarWanted[index] & 1) && releasedShape &&
                             releasedShape->BoundingSphere() >= 2.0f &&
                             GEngine->SceneFarObjectCreated(index, releasedShape, object->Transform()))
                        _nativeFarActive.insert(index);
                }
                placement.resident = nullptr;
                RemoveObject(keepAlive);
                RetainReleasedShape(releasedShape);
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
        // (d) AFTER the release, not before: a model whose last placement was just released
        // (and that the eviction cache did not keep) is cold now, and this is the walk that
        // must see it as such.
        PrefetchModernObjectModels(candidates);
    }

    // One initialized, distinct primary image per residency update. The prepared cold
    // payload is deliberately not carried across updates: its scope is defined for the
    // joined Converted -> ObjectCreate operation. Use the texture bank's ordinary source
    // reader here; final registration still validates every material and GPU handle.
    bool primaryStepThisUpdate = false;
    if (_modernPendingOwnerPrimaryTextures && !_modernPendingOwnerPrimaryTextures->failed &&
        _modernPendingOwnerPrimaryTextures->next < _modernPendingOwnerPrimaryTextures->textures.size())
    {
        primaryStepThisUpdate = true;
        auto& pending = *_modernPendingOwnerPrimaryTextures;
        Texture* texture = pending.textures[pending.next];
        const bool residentBefore = texture && texture->IsGpuResident();
        const auto began = std::chrono::steady_clock::now();
        bool uploaded = false;
        try
        {
            if (GEngine && GEngine->TextBank() && texture)
            {
                GEngine->TextBank()->UseMipmap(texture, 0, 0);
                uploaded = texture->IsGpuResident();
                if (uploaded && texture->IsAlpha())
                    texture->GetAlphaClass(); // consume the top-mip handoff in the same update
            }
        }
        catch (...)
        {
            uploaded = false;
        }
        const double elapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - began).count();
        pending.stepMs += elapsedMs;
        pending.maxStepMs = std::max(pending.maxStepMs, elapsedMs);
        if (uploaded && !residentBefore)
            ++pending.uploads;
        else if (!uploaded)
        {
            ++pending.uploadFailures;
            pending.failed = true; // finish through unchanged synchronous registration
        }
        ++pending.next;
        static uint32_t stepRows = 0;
        if (stepRows < 64)
        {
            ++stepRows;
            LOG_INFO(World, "DayZ owner primary stage: step={} total={} modelIndex={} texture={} uploaded={} stepMs={:.3f} maxStepMs={:.3f} row={} limit=64",
                     pending.next, pending.textures.size(), pending.modelIndex,
                     texture && texture->Name() ? texture->Name() : "(none)", uploaded, elapsedMs,
                     pending.maxStepMs, stepRows);
        }
    }

    // The second, independently opted-in phase touches one parent section per update.
    // A section can name one file-backed NormalMap, so this performs at most one
    // material upload, and never overlaps a primary upload in the same update.
    if (_modernPendingOwnerPrimaryTextures && !primaryStepThisUpdate)
    {
        auto& pending = *_modernPendingOwnerPrimaryTextures;
        if (pending.stageParentMaterials && !pending.failed && !pending.parentNormalsDone &&
            pending.next == pending.textures.size())
        {
            // The translated declaration is resolved once per parent section;
            // one file-backed role is touched on a later update. This is a
            // partial pre-touch, not a certificate of final/proxy bindings.
            // Extra material images are bounded separately from the existing
            // converted-shape/primary slot; this is not an all-model reservation.
            constexpr uint64_t kMaterialBudget = 64ull * 1024 * 1024;
            constexpr uint32_t kMaterialSectionLimit = 128;
            constexpr uint32_t kMaterialRoleLimit = 128;
            constexpr uint32_t kMaterialUploadLimit = 64;
            const auto began = std::chrono::steady_clock::now();
            if (pending.normalStarted == std::chrono::steady_clock::time_point{})
                pending.normalStarted = began;
            if (began - pending.normalStarted >= std::chrono::milliseconds(1500))
            {
                pending.failed = true;
                pending.normalStopReason = "deadline";
            }
            bool attempted = false, uploaded = false;
            uint64_t charged = 0;
            int visitedLevel = -1, visitedSection = -1;
            std::string roleName;
            if (!pending.failed && pending.materialRoleNext < pending.materialRoleNames.size())
            {
                visitedLevel = pending.normalLevel;
                visitedSection = pending.normalSection;
                roleName = pending.materialRoleNames[pending.materialRoleNext++];
                if (++pending.normalAttempts > kMaterialRoleLimit ||
                    pending.normalUploads >= kMaterialUploadLimit)
                {
                    pending.failed = true;
                    pending.normalStopReason = "role-or-upload-cap";
                }
                else try
                {
                    if (!GEngine->StageWorldShapeParentMaterialRole(roleName.c_str(),
                            kMaterialBudget - pending.normalReservedBytes,
                            attempted, uploaded, charged))
                    {
                        pending.failed = true;
                        pending.normalStopReason = "source-or-budget";
                    }
                }
                catch (...) { pending.failed = true; pending.normalStopReason = "exception"; }
                if (uploaded)
                {
                    ++pending.normalUploads;
                    pending.normalReservedBytes += charged;
                    if (std::find(pending.normalUploadedNames.begin(),
                            pending.normalUploadedNames.end(), roleName) == pending.normalUploadedNames.end())
                        pending.normalUploadedNames.push_back(roleName);
                }
                if (pending.materialRoleNext == pending.materialRoleNames.size())
                {
                    pending.materialRoleNames.clear();
                    pending.materialRoleNext = 0;
                    ++pending.normalSection;
                }
            }
            else if (!pending.failed)
            {
                // At most one material translation/section per update. The
                // next update starts role uploads after this capture.
                while (pending.normalLevel < pending.shape->NLevels())
                {
                    if (!pending.shape->IsNormalLevel(pending.normalLevel))
                    { ++pending.normalLevel; pending.normalSection = 0; continue; }
                    const Shape* lod = pending.shape->LevelOpaque(pending.normalLevel);
                    if (!lod || pending.normalSection >= lod->NSections())
                    { ++pending.normalLevel; pending.normalSection = 0; continue; }
                    visitedLevel = pending.normalLevel;
                    visitedSection = pending.normalSection;
                    if (++pending.normalSections > kMaterialSectionLimit)
                    { pending.failed = true; pending.normalStopReason = "section-cap"; break; }
                    try
                    {
                        if (!GEngine->ListWorldShapeParentMaterialRoles(pending.shape.get(),
                                visitedLevel, visitedSection, pending.materialRoleNames) ||
                            pending.materialRoleNames.size() > 16) // declaration helper's fixed MaxRows
                        { pending.failed = true; pending.normalStopReason = "declaration"; }
                    }
                    catch (...) { pending.failed = true; pending.normalStopReason = "exception"; }
                    if (pending.materialRoleNames.empty()) ++pending.normalSection;
                    break;
                }
                if (pending.normalLevel == pending.shape->NLevels() && !pending.failed)
                    pending.parentNormalsDone = true;
            }
            const double elapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - began).count();
            pending.normalStepMs += elapsedMs;
            pending.normalMaxStepMs = std::max(pending.normalMaxStepMs, elapsedMs);
            static uint32_t materialRows = 0;
            if (materialRows < 128)
            {
                ++materialRows;
                LOG_INFO(World, "DayZ owner material stage: modelIndex={} lod={} section={} role={} attempted={} uploaded={} failed={} done={} roles={} uploads={} bytes={} stepMs={:.3f} maxStepMs={:.3f} row={} limit=128 scope=parent-declarations-partial",
                         pending.modelIndex, visitedLevel, visitedSection, roleName, attempted,
                         uploaded, pending.failed, pending.parentNormalsDone,
                         pending.normalAttempts, pending.normalUploads, pending.normalReservedBytes,
                         elapsedMs, pending.normalMaxStepMs, materialRows);
            }
            else if (materialRows == 128)
            {
                ++materialRows;
                LOG_INFO(World, "DayZ owner material stage: step log truncated after 128 rows; no coverage inferred");
            }
        }
        else if (pending.stageParentNormals && !pending.failed && !pending.parentNormalsDone &&
            pending.next == pending.textures.size())
        {
            constexpr uint64_t kNormalBudget = 64ull * 1024 * 1024;
            constexpr uint32_t kNormalSectionLimit = 32;
            constexpr uint32_t kNormalUploadLimit = 16;
            const auto began = std::chrono::steady_clock::now();
            if (pending.normalStarted == std::chrono::steady_clock::time_point{})
                pending.normalStarted = began;
            if (began - pending.normalStarted >= std::chrono::milliseconds(750))
            {
                pending.failed = true;
                pending.normalStopReason = "deadline";
            }
            bool visited = false, attempted = false, uploaded = false;
            uint64_t charged = 0;
            std::string uploadedName;
            int visitedLevel = -1, visitedSection = -1;
            while (!pending.failed && pending.normalLevel < pending.shape->NLevels())
            {
                if (!pending.shape->IsNormalLevel(pending.normalLevel))
                {
                    ++pending.normalLevel;
                    pending.normalSection = 0;
                    continue;
                }
                const Shape* lod = pending.shape->LevelOpaque(pending.normalLevel);
                if (!lod || pending.normalSection >= lod->NSections())
                {
                    ++pending.normalLevel;
                    pending.normalSection = 0;
                    continue;
                }
                visited = true;
                visitedLevel = pending.normalLevel;
                visitedSection = pending.normalSection++;
                if (++pending.normalSections > kNormalSectionLimit || pending.normalUploads >= kNormalUploadLimit)
                {
                    pending.failed = true;
                    pending.normalStopReason = pending.normalSections > kNormalSectionLimit ?
                        "section-cap" : "upload-cap";
                    break;
                }
                try
                {
                    const uint64_t remaining = kNormalBudget - pending.normalReservedBytes;
                    if (!GEngine->StageWorldShapeParentNormalMap(pending.shape.get(), visitedLevel,
                            visitedSection, remaining, attempted, uploaded, charged, uploadedName))
                    {
                        pending.failed = true;
                        pending.normalStopReason = "source-or-budget";
                    }
                }
                catch (...) { pending.failed = true; pending.normalStopReason = "exception"; }
                if (attempted) ++pending.normalAttempts;
                if (uploaded)
                {
                    ++pending.normalUploads;
                    pending.normalReservedBytes += charged;
                    if (!uploadedName.empty() && std::find(pending.normalUploadedNames.begin(),
                            pending.normalUploadedNames.end(), uploadedName) == pending.normalUploadedNames.end())
                        pending.normalUploadedNames.push_back(std::move(uploadedName));
                }
                break;
            }
            if (!visited && !pending.failed)
                pending.parentNormalsDone = true;
            const double elapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - began).count();
            pending.normalStepMs += elapsedMs;
            pending.normalMaxStepMs = std::max(pending.normalMaxStepMs, elapsedMs);
            static uint32_t normalRows = 0;
            if (normalRows < 64)
            {
                ++normalRows;
                LOG_INFO(World, "DayZ owner normal stage: modelIndex={} lod={} section={} attempted={} uploaded={} "
                         "failed={} done={} stepMs={:.3f} maxStepMs={:.3f} uploads={} bytes={} row={} limit=64",
                         pending.modelIndex, visitedLevel, visitedSection, attempted, uploaded,
                         pending.failed, pending.parentNormalsDone, elapsedMs, pending.normalMaxStepMs,
                         pending.normalUploads, pending.normalReservedBytes, normalRows);
            }
        }
    }

    uint32_t created = 0;
    uint32_t refused = 0;
    uint32_t attempted = 0;
    // (d) per-update async accounting, for the row below.
    uint32_t installed = 0; // admitted on a worker-prepared IR
    uint32_t waited = 0;    // skipped this update because the IR was not ready
    uint32_t syncCold = 0;  // cold models loaded synchronously on this thread
    double warmMs = 0.0;    // time in ObjectCreate for objects whose model was already resident
    double coldMs = 0.0;    // time in ShapeBank + ObjectCreate for cold models, this update
    const bool asyncOn = _modernObjectPreparer != nullptr && _modernObjectPreparer->Running();
    const bool warmProfileEnabled = ObjectStreamWarmProfileEnabled();
    // No initialization of profile buckets, additions, clock calls or heap work
    // in the default arm; storage becomes a measured diagnostic only when ON.
    std::optional<WarmObjectUpdateProfile> warmUpdateProfile;
    if (warmProfileEnabled)
        warmUpdateProfile.emplace();

    // Admission is denominated in TIME, not frames.
    //
    // This runs once per rendered frame (TerrainWgpu::DrawTerrain), so a fixed
    // per-update batch makes the streaming rate proportional to the framerate --
    // exactly backwards. The sustainable travel speed works out at
    // `batch * fps / (2 * radius * density)`, so on a dense world at 5 fps the
    // streamer sustains ~40 m/s and any aircraft outruns it permanently. Worse, the
    // deficit accumulates: each recentre destroys the trailing objects outright, so
    // what the camera left behind must be rebuilt if it turns around.
    //
    // That last sentence used to end "(there is no cache)". There is one now -- see (a) and
    // RetainReleasedShape above -- but read it precisely: the OBJECT is still destroyed
    // outright and still has to be recreated. What the cache saves is the MODEL behind it, and
    // that is where the measured cost lives (a warm re-admit is ~0.03 ms against a cold model's
    // ODOL decode, LOD conversion and vertex-buffer build). The accumulating deficit this
    // paragraph describes is therefore reduced, not removed.
    //
    // The floor is the legacy batch, so nothing streams slower than it used to; the
    // time budget only lets a slow frame admit MORE. The hard cap bounds the worst
    // case, because each create is a synchronous model decode + texture transcode.
    const auto admitStart = std::chrono::steady_clock::now();
    // The budget is consulted as a millisecond ceiling, so "no ceiling" is a large number
    // rather than a flag: every comparison downstream keeps working unchanged.
    const float admitBudgetMs = preloading ? 1.0e9f : ObjectStreamBudgetMs();
    const uint32_t admitCap =
        preloading ? std::numeric_limits<uint32_t>::max()
                   : _modernObjectCreatesPerUpdate * ObjectStreamMaxBatchScale();
    // THE FLOOR, NOT THE BUDGET, IS WHAT MAKES THE FILL-IN STUTTER.
    //
    // The time budget cannot be consulted until `floor` objects have been attempted, so a
    // batch whose objects are cold costs whatever they cost. Measured on Reforger Everon:
    // 580 ms to admit the mandatory 128 - 145x over a 4 ms budget - which is why frames sit
    // at 200 ms+ for the whole 107 s fill-in and why the world is under 10 fps while it
    // loads. The streamer is floor-bound early and cap-bound late; it is time-bound
    // essentially never, so the budget it advertises does almost nothing.
    //
    // The cost is wildly uneven and that is the point: first touch of a distinct MODEL pays
    // the ODOL decode, LOD conversion and GPU registration, while an object reusing a loaded
    // model costs ~0.03 ms. A fixed floor charges a frame for however many cold models happen
    // to fall in its batch.
    //
    // WGR_OBJECT_STREAM_MIN_BATCH lowers the floor so the clock is reached sooner. It
    // DEFAULTS TO THE LEGACY BATCH, i.e. today's behaviour, because the trade is real and
    // unmeasured: a smaller floor bounds the frame but needs more updates to place the same
    // objects, and with ~200k objects to admit that can lengthen the total fill even as it
    // makes it smoother. Which matters more is a judgement about felt experience, so it
    // wants a measurement and an owner, not a silent default change.
    const uint32_t admitFloor = ObjectStreamMinBatch(_modernObjectCreatesPerUpdate);
    // (c) THE CEILING THAT MAKES THE FLOOR YIELD. See ObjectStreamCeilingMs for the reasoning
    // and for why this is not simply a lower floor.
    const float admitCeilingMs = ObjectStreamCeilingMs();
    // (c3) Counted mode: neither clock is consulted; cold attempts are the ration instead.
    const bool admitCounted = ObjectStreamAdmitCounted();
    static const bool registrationQuotaOptIn = []
    {
        const char* value = std::getenv("WGR_OBJECT_STREAM_REGISTRATION_QUOTA");
        return value && std::strcmp(value, "1") == 0;
    }();
    const bool registrationQuota = admitCounted && registrationQuotaOptIn;
    uint32_t registrationReusable = 0, registrationNeeded = 0, registrationUnknown = 0;
    uint32_t registrationCharged = 0, registrationPromotions = 0;
    auto registrationCost = [&](const LODShapeWithShadow* shape)
    {
        const auto cost = GEngine ? GEngine->QueryRegistrationCost(shape)
                                 : render::RegistrationCost::Reusable;
        switch (cost)
        {
        case render::RegistrationCost::Reusable: ++registrationReusable; break;
        case render::RegistrationCost::NeedsRegistration: ++registrationNeeded; break;
        case render::RegistrationCost::Unknown: ++registrationUnknown; break;
        }
        return cost;
    };
    const uint32_t admitColdBudget =
        preloading ? std::numeric_limits<uint32_t>::max() : ObjectStreamColdPerUpdate();
    uint32_t coldAttempted = 0; // cold models this update paid for, admitted or not
    // Native bank-table resolution loads texture headers on the main thread,
    // BEFORE coldAttempted is charged. Bound that stage without blocking warm
    // placement or installation of conversions already completed by workers.
    // Preload and non-native worlds retain their existing behaviour; 0 is the A/B.
    const uint32_t tableLimit = (!preloading && GetEnfusionSurfaceCount() > 0)
                                   ? ObjectStreamBankTablesPerUpdate() : 0;
    uint32_t tablesPrepared = 0, tableDeferred = 0;
    double tablePrepareMs = 0;
    // (c2) The upload byte ceiling, same standing as the wall-clock ceiling: it may cut the
    // floor short, and `attempted > 0` keeps the one-object-per-update termination guarantee.
    // Reads the FRAME's cumulative upload bytes, so uploads outside this loop (terrain, UI
    // first-binds) count against the same budget -- the frame does not care who spent them.
    const uint64_t admitUploadBudgetBytes = ObjectStreamUploadBudgetBytes();
    auto uploadBudgetSpent = [&]
    {
        return admitUploadBudgetBytes > 0 && attempted > 0 &&
               Poseidon::Dev::GTextureStreamCounters().frameUploadBytes.load(std::memory_order_relaxed) >=
                   admitUploadBudgetBytes;
    };
    // WHICH bound stopped this update. Recorded on the first true only: the loop keeps asking
    // once a bound is spent (the outer cell loop re-enters), so counting every true would
    // count cells, not stops. See the member declarations in Landscape.hpp.
    ++_modernObjectAdmitUpdates;
    if (preloading)
    {
        ++_modernObjectPreloadUpdates;
    }
    bool admitStopRecorded = false;
    auto admitBudgetSpent = [&]
    {
        if (attempted >= admitCap)
        {
            if (!admitStopRecorded)
            {
                admitStopRecorded = true;
                ++_modernObjectAdmitStopCap;
            }
            return true;
        }
        if (uploadBudgetSpent())
        {
            if (!admitStopRecorded)
            {
                admitStopRecorded = true;
                ++_modernObjectAdmitStopUpload;
            }
            return true;
        }
        if (admitCounted)
        {
            // No clock below this line in counted mode. `coldAttempted` is incremented at the
            // moment a cold model's cost is committed to, so the first cold model of an update
            // is always paid for (progress), and the N+1th is deferred to the next update.
            if (coldAttempted >= admitColdBudget)
            {
                if (!admitStopRecorded)
                {
                    admitStopRecorded = true;
                    ++_modernObjectAdmitStopCold;
                }
                return true;
            }
            return false;
        }
        const auto elapsed = std::chrono::steady_clock::now() - admitStart;
        const float elapsedMs = std::chrono::duration<float, std::milli>(elapsed).count();
        if (attempted < admitFloor)
        {
            // Below the floor the ordinary budget is still ignored -- that is what the floor is
            // for, and it is why a cheap all-warm batch still admits its full 128 without ever
            // looking at the ordinary budget. Only the hard ceiling may cut the floor short.
            //
            // `attempted > 0` is load-bearing, not defensive. Without it an update that entered
            // with the ceiling already exceeded (the previous frame's cost lands on this frame's
            // clock the moment admission is slow) would admit NOTHING, and an update that admits
            // nothing makes no progress: `_modernObjectResidencyPending` stays set, the same work
            // is attempted next frame, and the fill never terminates. One object per update is
            // the guarantee that it always terminates -- and it is also the residual tail this
            // ceiling cannot remove, because that one object may be a cold model costing tens of
            // milliseconds. Removing THAT needs asynchronous admission.
            const bool ceiling = admitCeilingMs > 0.0f && attempted > 0 && elapsedMs >= admitCeilingMs;
            if (ceiling && !admitStopRecorded)
            {
                admitStopRecorded = true;
                ++_modernObjectAdmitStopCeiling;
            }
            return ceiling;
        }
        if (admitCeilingMs > 0.0f && elapsedMs >= admitCeilingMs)
        {
            if (!admitStopRecorded)
            {
                admitStopRecorded = true;
                ++_modernObjectAdmitStopCeiling;
            }
            return true;
        }
        if (elapsedMs >= admitBudgetMs)
        {
            if (!admitStopRecorded)
            {
                admitStopRecorded = true;
                ++_modernObjectAdmitStopBudget;
            }
            return true;
        }
        return false;
    };

    // (d) THE ADMIT LOOP, WITH SKIP-NOT-WAIT.
    //
    // A cell's placement list is partitioned in place as it drains: [0, cursor) is done
    // (resident, or refused), [cursor, scan) has been visited THIS update and is waiting for a
    // worker, [scan, end) is not yet visited. Admitting a placement swaps it down to `cursor`;
    // a waiting one is left where it is and `scan` moves past it. So the ready placements
    // behind a cold one are admitted this frame instead of queueing behind it, and a cell
    // whose only remaining placements are waiting stays inactive with cursor < size and is
    // rescanned next update from `cursor`. Order within a cell is not meaningful to anything
    // (the release loop walks the whole list), which is what makes the swap safe. `scan`
    // strictly increases, so the loop terminates regardless of what the workers do.
    //
    // Under WGR_OBJECT_STREAM_ASYNC=0 nothing is ever "waiting": every placement is admitted
    // in order exactly as before, through the same ObjectCreate. The only extra work is the
    // ShapeBank::Find that classifies warm against cold for the split timers.
    for (const CellCandidate& cell : candidates)
    {
        if (!_modernObjectCellDesired[cell.index] || _modernObjectCellActive[cell.index])
            continue;
        std::vector<uint32_t>& placements = _modernObjectCells[cell.index];
        uint32_t& cursor = _modernObjectCellCursor[cell.index];
        uint32_t scan = cursor;
        while (scan < placements.size() && !admitBudgetSpent())
        {
            const uint32_t index = placements[scan];
            ModernObjectPlacement& placement = _modernObjectPlacements[index];
            if (placement.resident || placement.modelIndex >= _modernObjectModels.size())
            {
                if (placement.resident && _modernLogicalOnlyObjects.contains(placement.id))
                {
                    if (registrationQuota)
                    {
                        ++attempted;
                        ++registrationPromotions;
                        if (render::ChargeRegistrationCost(true, false,
                                registrationCost(placement.resident->GetShape())))
                        {
                            ++coldAttempted;
                            ++registrationCharged;
                        }
                    }
                    RegisterModernObjectVisual(placement.resident);
                    _modernLogicalOnlyObjects.erase(placement.id);
                    if (_nativeFarActive.erase(index) && GEngine)
                        GEngine->SceneFarObjectRemoved(index);
                    ++created;
                }
                std::swap(placements[scan], placements[cursor]);
                ++cursor;
                ++scan;
                continue;
            }
            const RStringB& modelName = _modernObjectModels[placement.modelIndex];
            // Warm or cold? Warm = ShapeBank still holds it (an Object or the eviction cache
            // keeps it alive), and ObjectCreate below is the ~0.03 ms path. Cold is the ~90 ms
            // path, and the only one the preparer has anything to say about.
            const auto* warmShape = Shapes.Find(modelName, false, true);
            const bool cold = warmShape == nullptr;
            std::shared_ptr<Model::Model> preparsed;
            LODShapeWithShadow* preadapted = nullptr;
            std::unique_ptr<render::ColdPaaOwned> coldTexture;
            bool ownerPrimaryHandoff = false;
            uint32_t ownerPrimaryUploads = 0, ownerPrimaryLost = 0;
            uint32_t ownerPrimaryOriginPlacement = 0;
            double ownerPrimaryStepMs = 0.0, ownerPrimaryMaxStepMs = 0.0, ownerPrimaryWaitMs = 0.0;
            bool ownerPrimaryFailed = false;
            bool ownerNormalsEnabled = false, ownerMaterialsEnabled = false, ownerNormalsDone = false;
            uint32_t ownerNormalSections = 0, ownerNormalAttempts = 0, ownerNormalUploads = 0;
            uint64_t ownerNormalBytes = 0;
            double ownerNormalStepMs = 0.0, ownerNormalMaxStepMs = 0.0;
            std::vector<std::string> ownerNormalNames;
            std::string ownerNormalStopReason;
            if (cold && _modernPendingOwnerPrimaryTextures &&
                _modernPendingOwnerPrimaryTextures->modelIndex == placement.modelIndex)
            {
                auto& pending = *_modernPendingOwnerPrimaryTextures;
                if (!pending.failed && (pending.next < pending.textures.size() || !pending.parentNormalsDone))
                {
                    ++waited;
                    ++scan;
                    continue;
                }
                ownerPrimaryHandoff = true;
                ownerPrimaryOriginPlacement = pending.placementIndex;
                ownerPrimaryUploads = pending.uploads;
                ownerPrimaryStepMs = pending.stepMs;
                ownerPrimaryMaxStepMs = pending.maxStepMs;
                // The optional NormalMap phase can stop at its byte/section/deadline
                // bound without any primary upload failing. Keep the two proofs separate.
                ownerPrimaryFailed = pending.uploadFailures != 0;
                ownerNormalsEnabled = pending.stageParentNormals;
                ownerMaterialsEnabled = pending.stageParentMaterials;
                ownerNormalsDone = pending.parentNormalsDone;
                ownerNormalSections = pending.normalSections;
                ownerNormalAttempts = pending.normalAttempts;
                ownerNormalUploads = pending.normalUploads;
                ownerNormalBytes = pending.normalReservedBytes;
                ownerNormalStepMs = pending.normalStepMs;
                ownerNormalMaxStepMs = pending.normalMaxStepMs;
                ownerNormalNames = std::move(pending.normalUploadedNames);
                ownerNormalStopReason = pending.normalStopReason;
                ownerPrimaryWaitMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - pending.started).count();
                for (Texture* texture : pending.textures)
                    if (texture && !texture->IsGpuResident()) ++ownerPrimaryLost;
                preadapted = pending.shape.release();
                preparsed = std::move(pending.model);
                _modernPendingOwnerPrimaryTextures.reset();
            }
            else if (cold && asyncOn)
            {
                const ObjectStreamPreparer::State state = _modernObjectPreparer->Query(placement.modelIndex);
                if (state == ObjectStreamPreparer::State::Ready)
                {
                    if (tableLimit > 0 && tablesPrepared >= tableLimit)
                    {
                        // Leave the IR owned by the existing ready queue. Taking
                        // then re-requesting would throw away completed parsing.
                        ++tableDeferred;
                        ++waited;
                        ++scan;
                        continue;
                    }
                    preparsed = _modernObjectPreparer->Take(placement.modelIndex);
                    if (!preparsed)
                    {
                        // Ready at Query, gone at Take: the ready-limit trim on a worker got
                        // there first. Re-request and skip rather than parse it here.
                        _modernObjectPreparer->Request(placement.modelIndex);
                        ++waited;
                        ++scan;
                        continue;
                    }
                    // Stage 3 (WGR_OBJECT_STREAM_ASYNC_ADAPT, default on): hand the IR
                    // straight back with the main-thread bank tables and let a worker run
                    // the conversion too -- measured 3+ s of the traverse's cold admission,
                    // versus <100 ms for the table resolution that stays here.
                    // DEFAULT OFF AGAIN SINCE 2026-08-31, and this is a safety decision about
                    // my own change, not a performance one.
                    //
                    // It was turned back on after the tail split (FinishOdolAdapterTail) on the
                    // strength of a traverse: 797 cold models all async, main-thread adapt
                    // 0.0 ms, coverage at parity. What that traverse could not see is that
                    // running convertToLODShape on TWO THREADS CORRUPTS THE PROCESS HEAP.
                    // SIM-807 built the harness that can: with two workers converting
                    // concurrently -- the shipping configuration -- the process dies inside
                    // mimalloc at a different site each run, roughly one run in three, and it
                    // first showed as bit-divergence rather than a crash. Reproduce with
                    // SIM807_CONCURRENT_ADAPT=1 on the [race] tag.
                    //
                    // THE WRITE HAS SINCE BEEN FOUND AND FIXED (same day): CompactBuffer's
                    // reference count was a plain non-atomic int, and CompactBuffer<char> is
                    // String -- the buffer behind every RString and therefore every interned
                    // RStringB. The bank's mutex guards the MAP; the string it hands back is
                    // refcounted outside that lock by every Ref<String> copy and destructor.
                    // Two workers interning the same selection name lost an increment and
                    // freed a buffer somebody still held. See CompactBuf.hpp. That is why the
                    // rate tracked the named-selection count, and why pinning one name made it
                    // worse rather than better -- one counter, hammered harder.
                    //
                    // Measured with the fix: 0 crashes in 12 runs of the SIM-807 reproducer,
                    // against 9 in 12 without it.
                    //
                    // BACK ON by default, and on better evidence than it had the first time.
                    //
                    // The first time it was turned on, the argument was a traverse: 797 cold
                    // models, main-thread adapt 0.0 ms, coverage at parity. That argument was
                    // worthless -- a traverse cannot see a heap being scribbled on, which is
                    // why it took a determinism harness to find the fault.
                    //
                    // This time the argument is the harness itself, run across the axis that
                    // matters. It converts a reference shape on one thread, then 64 jobs across
                    // N threads, and compares COMPONENT hashes -- so it answers "is the output
                    // bit-identical", not "did it crash". Measured 2026-08-31 with the
                    // CompactBuffer fix in place, three runs per cell:
                    //
                    //     threads   1   2   4   8  16      selections 60 and 200
                    //     result    all 30 runs bit-identical to the single-thread reference
                    //
                    // 16 threads is 8x the shipping worker count and 200 selections is 3.3x the
                    // arm that used to crash. That is what the switch was waiting on.
                    //
                    // Confirmed on the real corpus too, and this time the traverse is evidence
                    // for the right claim -- not "did it crash" but "what does it buy".
                    // Reforger Everon (ev_DALL.wrp), 200,000-object budget, freefly, screenshot
                    // at 35 s, one run per arm:
                    //
                    //     adapt on    741 models converted, 0 failed, 97,322 objects resident
                    //     adapt off   708 models converted, 0 failed, 56,065 objects resident
                    //
                    // 73% more of the world admitted by the same wall-clock moment, because the
                    // conversion no longer blocks the main thread. Zero errors in either log.
                    // Note this path is inactive on stock CWA worlds -- it runs only for the
                    // modern/substituted world loader -- so a stock mission exercises none of it.
                    //
                    // WGR_OBJECT_STREAM_ASYNC_ADAPT=0 restores the synchronous adapt.
                    static const bool asyncAdapt = []
                    {
                        const char* v = std::getenv("WGR_OBJECT_STREAM_ASYNC_ADAPT");
                        return !v || v[0] != '0';
                    }();
                    if (asyncAdapt && (preparsed->sourceFormat == "ODOL" || preparsed->sourceFormat == "MLOD"))
                    {
                        // The adapter names the shape from sourcePath, and the bank keys by
                        // the lowercased name -- same lowering as BuildShapeCacheKey.
                        std::string low(static_cast<const char*>(modelName));
                        for (char& c : low)
                            if (c >= 'A' && c <= 'Z')
                                c = static_cast<char>(c - 'A' + 'a');
                        preparsed->sourcePath = low;
                        auto tables = std::make_shared<Model::ShapeAdapter::AdapterBankTables>();
                        const auto tableStart = std::chrono::steady_clock::now();
                        TenementPhysicalPaaScope physicalPaaTables(
                            low == R"(dz\structures\residential\tenements\tenement_small.p3d)");
                        Model::ShapeAdapter::BuildAdapterBankTables(*preparsed, *tables);
                        if (TenementPhysicalPaaScope::Overflowed())
                            LOG_INFO(World, "DayZ physical prefetch: parent Init identity capture overflowed 256 attempts; uncaptured sources use ordinary reads");
                        tablePrepareMs += std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - tableStart).count();
                        ++tablesPrepared;
                        const auto radiusToken = _modernObjectPreparer->QueryRadius(placement.modelIndex);
                        if (_modernObjectPreparer->SubmitConvert(placement.modelIndex, std::move(preparsed),
                                                                std::move(tables), &radiusToken, nullptr, true,
                                                                [](void*, const char* key, WarmTextureRead& out) {
                                                                    return GEngine && GEngine->CaptureWarmRapStageRead(key, out);
                                                                }))
                        {
                            ++waited;
                            ++scan;
                            continue;
                        }
                        // Submit refused (workers gone): preparsed was consumed only on
                        // success, but std::move may have emptied it -- reload defensively.
                        if (!preparsed)
                        {
                            _modernObjectPreparer->Request(placement.modelIndex);
                            ++waited;
                            ++scan;
                            continue;
                        }
                    }
                }
                else if (state == ObjectStreamPreparer::State::RapCapture)
                {
                    // Complete the bounded raP worker/owner/worker handoff without
                    // waiting. Refused stage capture still resumes full conversion.
                    _modernObjectPreparer->ResumeRapCapture(placement.modelIndex,
                        [](void*, const char* key, WarmTextureRead& out) {
                            return GEngine && GEngine->CaptureWarmRapStageRead(key, out);
                        });
                    ++waited;
                    ++scan;
                    continue;
                }
                else if (state == ObjectStreamPreparer::State::Converted)
                {
                    ObjectStreamPreparer::ConvertedShape conv =
                        _modernObjectPreparer->TakeConverted(placement.modelIndex);
                    if (!conv.shape)
                    {
                        ++waited;
                        ++scan;
                        continue;
                    }
                    if (conv.rapStages && conv.rapStages->modelIndex == placement.modelIndex)
                    {
                        static std::atomic<unsigned> rapHandoffRows{0};
                        const unsigned row = rapHandoffRows.fetch_add(1, std::memory_order_relaxed);
                        if (row < 2)
                            for (const auto& material : conv.rapStages->materials)
                                LOG_INFO(World, "RVMAT raP owner candidate: generation={} modelIndex={} material={} stage={} (names only; no current binding or PAA preparation)",
                                     conv.rapStages->generation, placement.modelIndex, material.key, material.stageName);
                    }
                    if (conv.rapStages) try
                    {
                        // One exact DayZ tree NormalMap, under an additional default-off switch.
                        // The worker's raP name is historical. Recheck the physical material
                        // member before this owner captures the current initialized PAA binding.
                        static const bool rapStageWarm = [] {
                            const char* flag = std::getenv("WGR_OBJECT_STREAM_RAP_STAGE_WARM_CAPTURE");
                            return flag && std::strcmp(flag, "1") == 0 &&
                                ObjectStreamPreparer::WarmTextureJobsEnabled();
                        }();
                        if (rapStageWarm && GEngine && conv.model &&
                            conv.rapStages->modelIndex == placement.modelIndex &&
                            Streaming::SelectedTreeFixtureModel(conv.model->sourcePath))
                        {
                            const auto token = _modernObjectPreparer->QueryRadius(placement.modelIndex);
                            if (token.generation == conv.rapStages->generation &&
                                token.modelIdentity == conv.model->sourcePath)
                            {
                                for (const auto& material : conv.rapStages->materials)
                                {
                                    const bool exactMaterial =
                                        material.key == R"(dz\plants\tree\data\t_piceaabies_2d_trunk.rvmat)" ||
                                        material.key == R"(dz\plants\tree\data\d_piceaabies_stumpb_trunk_a.rvmat)";
                                    if (!exactMaterial || material.consumer !=
                                        RapStageCandidateHandoff::Consumer::NormalMap || material.stageName !=
                                        R"(dz\plants\tree\data\t_piceaabies_trunk_no.paa)") continue;
                                    bool sameMaterialMember = false;
                                    if (QFBank* bank = QIFStreamB::AutoBank(material.key.c_str()))
                                    {
                                        const RString prefix = bank->GetPrefix();
                                        if (prefix.GetLength() >= 0 &&
                                            static_cast<size_t>(prefix.GetLength()) < material.key.size() &&
                                            !CmpStartStr(material.key.c_str(), prefix))
                                        {
                                            const char* member = material.key.c_str() + prefix.GetLength();
                                            if (auto current = bank->CaptureReadRequest(member, true))
                                            {
                                                BankReadMemberIdentity currentMember;
                                                sameMaterialMember = current->CopyMemberIdentity(currentMember) &&
                                                    material.SameCurrentMember(currentMember);
                                            }
                                        }
                                    }
                                    if (!sameMaterialMember) continue; // Ordinary material path handles remounts.
                                    WarmTextureRead read;
                                    const bool captured = GEngine->CaptureWarmRapStageRead(
                                        material.stageName.c_str(), read);
                                    if (captured)
                                    {
                                        std::vector<WarmTextureRead> reads;
                                        reads.push_back(std::move(read));
                                        const auto submitted = _modernObjectPreparer->SubmitWarmTextures(
                                            placement.modelIndex, token, std::move(reads));
                                        static std::atomic<unsigned> rapWarmRows{0};
                                        if (rapWarmRows.fetch_add(1, std::memory_order_relaxed) < 2)
                                            LOG_INFO(World, "RVMAT raP stage warm admission: modelIndex={} source={} captured=true submit={} (current PAA source; no Take/upload proof)",
                                                placement.modelIndex, material.stageName,
                                                static_cast<unsigned>(submitted));
                                        break; // the two exact materials share this one stage
                                    }
                                    static std::atomic<unsigned> rapWarmRefusals{0};
                                    if (rapWarmRefusals.fetch_add(1, std::memory_order_relaxed) < 2)
                                        LOG_INFO(World, "RVMAT raP stage warm admission: modelIndex={} source={} captured=false (ordinary upload fallback)",
                                            placement.modelIndex, material.stageName);
                                }
                            }
                        }
                    }
                    catch (...) {} // Optional stage admission never prevents shape install.
                    // A deliberately narrow owner-side latency pilot. Its entire output is
                    // the already initialized section Texture objects in this converted shape;
                    // material-stage maps are still resolved by final registration. The shape
                    // stays outside ShapeBank until all staged uploads finish, so no object or
                    // partial retained model is published during these updates.
                    static const bool ownerPrimaryStage = [] {
                        const char* flag = std::getenv("WGR_OBJECT_STREAM_DAYZ_OWNER_PRIMARY_STAGE");
                        return flag && std::strcmp(flag, "1") == 0;
                    }();
                    static const bool ownerParentNormals = [] {
                        const char* flag = std::getenv("WGR_OBJECT_STREAM_DAYZ_OWNER_NORMAL_STAGE");
                        return flag && std::strcmp(flag, "1") == 0;
                    }();
                    static const bool ownerParentMaterials = [] {
                        const char* flag = std::getenv("WGR_OBJECT_STREAM_DAYZ_OWNER_MATERIAL_STAGE");
                        return flag && std::strcmp(flag, "1") == 0;
                    }();
                    if (ownerPrimaryStage && !_modernPendingOwnerPrimaryTextures && GEngine &&
                        GEngine->TextBank() && conv.model &&
                        std::strcmp((const char*)modelName,
                                    R"(dz\structures\residential\tenements\tenement_small.p3d)") == 0)
                    {
                        constexpr uint64_t kPendingLimit = 128ull * 1024 * 1024;
                        constexpr size_t kTextureLimit = 64;
                        constexpr size_t kSectionVisitLimit = 512;
                        const uint64_t irBytes = Model::ResidentPayloadBytes(*conv.model);
                        const uint64_t reservedBytes = irBytes <= kPendingLimit / 2 ? 2 * irBytes : kPendingLimit + 1;
                        std::vector<Texture*> primary;
                        bool admissible = reservedBytes <= kPendingLimit;
                        if (admissible)
                        {
                            std::unordered_set<Texture*> seen;
                            size_t visited = 0;
                            if (conv.shape->NLevels() > 64) admissible = false;
                            for (int level = 0; level < conv.shape->NLevels() && admissible; ++level)
                            {
                                if (!conv.shape->IsNormalLevel(level)) continue;
                                Shape* lod = conv.shape->LevelOpaque(level);
                                if (!lod) continue;
                                for (int section = 0; section < lod->NSections(); ++section)
                                {
                                    if (++visited > kSectionVisitLimit) { admissible = false; break; }
                                    const ShapeSection& source = lod->GetSection(section);
                                    if (source.properties.Special() & (IsHidden | IsHiddenProxy)) continue;
                                    Texture* texture = source.properties.GetTexture();
                                    const char* name = texture ? texture->Name() : nullptr;
                                    const char* extension = name ? std::strrchr(name, '.') : nullptr;
                                    if (!texture || texture->IsGpuResident() || texture->ANMipmaps() <= 0 ||
                                        texture->AWidth(0) <= 0 || texture->AHeight(0) <= 0 || !extension ||
                                        std::strcmp(extension, ".paa") != 0 || !seen.insert(texture).second)
                                        continue;
                                    if (primary.size() == kTextureLimit) { admissible = false; break; }
                                    primary.push_back(texture);
                                }
                            }
                        }
                        if (admissible && !primary.empty())
                        {
                            auto pending = std::make_unique<PendingOwnerPrimaryTextures>();
                            pending->modelIndex = placement.modelIndex;
                            pending->placementIndex = index;
                            pending->cellIndex = cell.index;
                            pending->reservedBytes = reservedBytes;
                            pending->textures = std::move(primary);
                            pending->stageParentNormals = ownerParentNormals && !ownerParentMaterials;
                            pending->stageParentMaterials = ownerParentMaterials;
                            pending->parentNormalsDone = !(ownerParentNormals || ownerParentMaterials);
                            pending->started = std::chrono::steady_clock::now();
                            pending->shape.reset(conv.shape);
                            conv.shape = nullptr;
                            pending->model = std::move(conv.model);
                            // ColdPaaOwned is operation-local to the original joined admission.
                            // Never keep or reopen it across owner frames.
                            conv.coldTexture.reset();
                            LOG_INFO(World, "DayZ owner primary stage: started modelIndex={} textures={} reservedBytes={} ordinarySource=true",
                                     pending->modelIndex, pending->textures.size(), pending->reservedBytes);
                            _modernPendingOwnerPrimaryTextures = std::move(pending);
                            ++waited;
                            ++scan;
                            continue;
                        }
                        static uint32_t refusalRows = 0;
                        if (refusalRows < 8)
                        {
                            ++refusalRows;
                            LOG_INFO(World, "DayZ owner primary stage: refused modelIndex={} candidateTextures={} reservedBytes={} reason={} row={} limit=8",
                                     placement.modelIndex, primary.size(), reservedBytes,
                                     !admissible ? "bound" : "no-unuploaded-primary", refusalRows);
                        }
                    }
                    preadapted = conv.shape;
                    preparsed = std::move(conv.model);
                    coldTexture = std::move(conv.coldTexture);
                }
                else if (state != ObjectStreamPreparer::State::NotLoose && state != ObjectStreamPreparer::State::Failed)
                {
                    // Unknown (first sight, or dropped from the ready set), Queued, Parsing
                    // or Converting: not ready. Ask (a no-op unless Unknown) and SKIP.
                    // Never wait.
                    if (state == ObjectStreamPreparer::State::Unknown)
                        _modernObjectPreparer->Request(placement.modelIndex);
                    ++waited;
                    ++scan;
                    continue;
                }
                // NotLoose / Failed: sticky, the worker cannot help; fall through to the
                // synchronous path, which is exactly the old behaviour for this model.
            }
            std::swap(placements[scan], placements[cursor]);
            ++cursor;
            ++scan;
            ++attempted;
            if (registrationQuota && !cold &&
                render::ChargeRegistrationCost(true, false, registrationCost(warmShape)))
            {
                ++coldAttempted;
                ++registrationCharged;
            }
            Matrix4 transform;
            transform.SetDirectionAside(Vector3(placement.rows[0], placement.rows[1], placement.rows[2]));
            transform.SetDirectionUp(Vector3(placement.rows[3], placement.rows[4], placement.rows[5]));
            transform.SetDirection(Vector3(placement.rows[6], placement.rows[7], placement.rows[8]));
            transform.SetPosition(Vector3(placement.rows[9], placement.rows[10], placement.rows[11]));
            Object* admitted = nullptr;
            ShapeBank::LoadTiming timing;
            double shapeMs = 0.0;
            // Open the per-admission split scope for a COLD model only: `create` was 26 ms
            // median / 208 ms worst against a warm admit's 0.03 ms, so the warm path must not
            // pay even the clock reads. Everything under ObjectCreate that self-reports
            // (SceneObjectCreated, RegisterGpuModel, TextureWgpu::EnsureUploaded) accumulates
            // into the buckets while this is active.
            render::ObjectAdmitProfile& admitProfile = render::GObjectAdmitProfile;
            WarmObjectAdmitProfileScope warmScope(admitProfile, !cold && warmProfileEnabled);
            const bool warmProfiled = warmScope.Opened();
            static const bool textureTraceEnabled = [] {
                const char* flag = std::getenv("WGR_OBJECT_STREAM_DAYZ_TEXTURE_TRACE");
                return flag && std::strcmp(flag, "1") == 0;
            }();
            std::unique_ptr<Streaming::ObjectTextureUploadTrace> textureTrace;
            if (cold && textureTraceEnabled &&
                (std::strcmp((const char*)modelName, R"(dz\structures\residential\tenements\tenement_small.p3d)") == 0 ||
                 std::strcmp((const char*)modelName, R"(dz\structures\residential\schools\city_school.p3d)") == 0 ||
                 std::strcmp((const char*)modelName, R"(dz\structures\residential\police\village_policestation.p3d)") == 0))
                textureTrace = std::make_unique<Streaming::ObjectTextureUploadTrace>();
            auto* previousTextureTrace = Streaming::GObjectTextureUploadTrace;
            Streaming::GObjectTextureUploadTrace = textureTrace.get();
            if (cold)
            {
                ++coldAttempted;
                admitProfile.Reset();
                admitProfile.active = true;
            }
            // Same taken ConvertedShape, joined nonyielding owner operation through first GPU registration.
            render::ColdPaaAdmissionScope coldTextureScope(coldTexture.get());
            TenementPhysicalPaaScope physicalPaaAdmission(cold &&
                std::strcmp((const char*)modelName,
                    R"(dz\structures\residential\tenements\tenement_small.p3d)") == 0);
            const auto createStart = std::chrono::steady_clock::now();
            try
            {
                if (cold)
                {
                    // Install the shape first, on the pre-parsed IR when there is one, so the
                    // ObjectCreate below finds it in ShapeBank and pays only NewObject /
                    // AddObject / GPU registration. Held by a Ref across the create so the
                    // freshly built shape (refcount 0 out of the bank) cannot die between the
                    // two calls; the Object takes its own reference inside.
                    ShapeBank::WorldModelScope worldModel(Shapes); // RFG-099
                    Ref<LODShapeWithShadow> installedShape =
                        Shapes.NewFromModel(modelName, false, true, std::move(preparsed), &timing, preadapted);
                    shapeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                        createStart)
                                  .count();
                    admitted = ObjectCreate(placement.id, modelName, transform, nullptr, nullptr, false, true);
                    (void)installedShape;
                }
                else
                {
                    admitted = ObjectCreate(placement.id, modelName, transform, nullptr, nullptr, false, true);
                }
            }
            catch (const std::exception&)
            {
                admitted = nullptr;
            }
            if (TenementPhysicalPaaScope::Overflowed())
                LOG_INFO(World, "DayZ physical prefetch: admission Init identity capture overflowed 256 attempts; uncaptured sources use ordinary reads");
            if (ownerPrimaryHandoff)
            {
                if (ownerMaterialsEnabled)
                {
                    LOG_INFO(World, "DayZ owner material stage: handoff modelIndex={} done={} fallback={} reason={} sections={} roleAttempts={} uploads={} stagedDistinct={} bytes={} stepMs={:.3f} maxStepMs={:.3f} admitted={} scope=parent-declarations-partial-final-registration-ordinary",
                             placement.modelIndex, ownerNormalsDone, !ownerNormalsDone ||
                                 !ownerNormalStopReason.empty(), ownerNormalStopReason,
                             ownerNormalSections, ownerNormalAttempts, ownerNormalUploads,
                             ownerNormalNames.size(), ownerNormalBytes, ownerNormalStepMs,
                             ownerNormalMaxStepMs, admitted != nullptr);
                }
                if (ownerNormalsEnabled)
                {
                    const uint32_t captured = admitted && GEngine ?
                        GEngine->CountWorldShapeCapturedTextures(admitted->GetShape(), ownerNormalNames) : 0;
                    LOG_INFO(World, "DayZ owner normal stage: handoff modelIndex={} done={} fallback={} "
                             "reason={} sections={} attempts={} uploads={} stagedDistinct={} captured={} "
                             "bytes={} stepMs={:.3f} maxStepMs={:.3f} admitted={} "
                             "(parent NormalMap only; complete registration)",
                             placement.modelIndex, ownerNormalsDone, ownerPrimaryFailed, ownerNormalStopReason,
                             ownerNormalSections, ownerNormalAttempts, ownerNormalUploads,
                             ownerNormalNames.size(), captured, ownerNormalBytes, ownerNormalStepMs,
                             ownerNormalMaxStepMs, admitted != nullptr);
                }
                static uint32_t handoffRows = 0;
                if (handoffRows < 8)
                {
                    ++handoffRows;
                    LOG_INFO(World, "DayZ owner primary stage: handoff modelIndex={} originPlacement={} consumingPlacement={} admitted={} uploadSteps={} lostBeforeRegister={} uploadFailure={} stepMs={:.3f} maxStepMs={:.3f} waitToCreateMs={:.3f} finalCreateMs={:.3f} row={} limit=8",
                             placement.modelIndex, ownerPrimaryOriginPlacement, index, admitted != nullptr, ownerPrimaryUploads,
                             ownerPrimaryLost, ownerPrimaryFailed, ownerPrimaryStepMs,
                             ownerPrimaryMaxStepMs, ownerPrimaryWaitMs,
                             std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - createStart).count(),
                             handoffRows);
                }
            }
            Streaming::GObjectTextureUploadTrace = previousTextureTrace;
            if (textureTrace)
            {
                std::sort(textureTrace->slowest.begin(),
                    textureTrace->slowest.begin() + textureTrace->count,
                    [](const auto& lhs, const auto& rhs) { return lhs.readMs > rhs.readMs; });
                LOG_INFO(World, "DayZ texture first-touch trace: model={} uploads={} prepared={} succeeded={} measuredReadMs={:.3f} slowestRows={} limit=16",
                    (const char*)modelName, textureTrace->uploads, textureTrace->preparedUploads,
                    textureTrace->successfulUploads, textureTrace->measuredReadMs,
                    textureTrace->count);
                if (ownerMaterialsEnabled)
                    LOG_INFO(World, "DayZ owner material stage: final-registration remainingUploads={} stagedParentUploads={} scope=includes-proxies-and-other-uncovered-roles-not-completeness",
                             textureTrace->uploads, ownerNormalUploads);
                for (size_t traceIndex = 0; traceIndex < textureTrace->count; ++traceIndex)
                {
                    const auto& row = textureTrace->slowest[traceIndex];
                    LOG_INFO(World, "DayZ texture first-touch row: model={} rank={} source={} readMs={:.3f} bytes={} prepared={} uploaded={} limit=16",
                        (const char*)modelName, traceIndex + 1, row.name, row.readMs,
                        row.bytes, row.prepared, row.uploaded);
                }
            }
            double geoOwnerMs = 0.0;
            if (admitted != nullptr && _geoAccumEnabled)
            {
                if (warmProfiled)
                {
                    const auto geoBegan = std::chrono::steady_clock::now();
                    AccumulateGeographyForObject(admitted); // RFG-101
                    geoOwnerMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - geoBegan).count();
                }
                else
                    AccumulateGeographyForObject(admitted); // RFG-101
            }
            const double totalMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - createStart).count();
            warmScope.Close();
            if (cold)
            {
                admitProfile.active = false;
                const double createMs = std::max(0.0, totalMs - shapeMs);
                coldMs += totalMs;
                if (timing.preparsed)
                    ++installed;
                else
                    ++syncCold;
                ++_modernObjectColdModels;
                _modernObjectColdParseMs += timing.parseMs;
                _modernObjectColdAdaptMs += timing.adaptMs;
                _modernObjectColdOptimizeMs += timing.optimizeMs;
                _modernObjectColdCreateMs += createMs;
                _modernObjectColdSceneMs += admitProfile.sceneMs;
                _modernObjectColdRegisterMs += admitProfile.registerMs;
                _modernObjectColdProxiesMs += admitProfile.proxiesMs;
                _modernObjectColdMeshBuildMs += admitProfile.meshBuildMs;
                _modernObjectColdMeshCreateMs += admitProfile.meshCreateMs;
                _modernObjectColdSectionMs += admitProfile.sectionMs;
                _modernObjectColdModelFfiMs += admitProfile.modelRegisterMs;
                _modernObjectColdTexHeaderMs += admitProfile.texHeaderLoadMs;
                _modernObjectColdTexHeaderLoads += admitProfile.texHeaderLoads;
                _modernObjectColdTexReadMs += admitProfile.textureReadMs;
                _modernObjectColdTexCreateMs += admitProfile.textureCreateMs;
                _modernObjectColdTexFallbackMs += admitProfile.textureFallbackMs;
                _modernObjectColdAlphaMs += admitProfile.alphaScanMs;
                _modernObjectColdTexUploads += admitProfile.textureUploads;
                _modernObjectColdTexPrepared += admitProfile.texturePrepared;
                _modernObjectColdParkHits += admitProfile.parkHits;
                _modernObjectColdParkStale += admitProfile.parkStale;
                // One row per cold model. This is the split timer the whole change is judged
                // by: if `parse` is small next to `adapt`+`opt`+`create`, moving the parse off
                // the thread cannot have removed the stall, and the row says so per model.
                //
                // The `[...]` clause is the further split of `create`, measured inside the
                // engine (ObjectAdmitProfile): reg = RegisterGpuModel whole; mesh = vertex/
                // index/section building + wgr_mesh_create; sect = the per-section loop MINUS
                // its texture time (material resolution, classification); tex = texture time
                // (read+LZO / wgr_texture_create / whole-file fallback / alpha scans), with
                // up/prep = real uploads / of which worker-prepared; ffi = wgr_model_register;
                // prox = EmitGpuProxies; other = ObjectCreate outside SceneObjectCreated
                // (NewObject, AddObject insert, marker lights, transform repair). park=hit
                // means the registration was reused whole (WGR_GPU_MODEL_PARK).
                const double texMs = admitProfile.textureReadMs + admitProfile.textureCreateMs +
                                     admitProfile.textureFallbackMs + admitProfile.alphaScanMs;
                const double meshMs = admitProfile.meshBuildMs + admitProfile.meshCreateMs;
                const double sectOnlyMs = std::max(0.0, admitProfile.sectionMs - texMs);
                const double otherMs = std::max(0.0, createMs - admitProfile.sceneMs);
                LOG_INFO(World,
                         "Object stream cold model: {} mode={} parse={:.1f} adapt={:.1f} opt={:.1f} create={:.1f} "
                         "[reg={:.1f} mesh={:.1f} sect={:.1f} tex={:.1f} (read={:.1f} create={:.1f} fb={:.1f} "
                         "alpha={:.1f} up={} prep={}) ffi={:.1f} prox={:.1f} other={:.1f} park={}] "
                         "texhdr={:.1f}/{} total={:.1f} ms{} texWindowWait={:.3f} texEncodeMs={:.3f} texEncodeWithoutWindow={:.3f}/{} workerBc3Claims={}/{} "
                         "alphaPartsMs(read/decode/hist/shape)={:.3f}/{:.3f}/{:.3f}/{:.3f} alphaPaths(handoff/block/full)={}/{}/{}",
                         (const char*)modelName, timing.preparsed ? "async" : (asyncOn ? "sync-fallback" : "sync"),
                         timing.parseMs, timing.adaptMs, timing.optimizeMs, createMs, admitProfile.registerMs,
                         meshMs, sectOnlyMs, texMs, admitProfile.textureReadMs, admitProfile.textureCreateMs,
                         admitProfile.textureFallbackMs, admitProfile.alphaScanMs, admitProfile.textureUploads,
                         admitProfile.texturePrepared, admitProfile.modelRegisterMs, admitProfile.proxiesMs,
                         otherMs,
                         admitProfile.parkHits ? "hit" : (admitProfile.parkStale ? "stale" : "miss"),
                         admitProfile.texHeaderLoadMs, admitProfile.texHeaderLoads, totalMs,
                         timing.canonical ? "" : " (legacy loader)", admitProfile.textureWindowWaitMs,
                         admitProfile.textureEncodeMs, admitProfile.textureEncodeWithoutWindowMs,
                         admitProfile.textureEncodesWithoutWindow,
                         admitProfile.textureBc3PreparedClaims, admitProfile.textureBc3PreparedBytes,
                         admitProfile.alphaSourceReadMs, admitProfile.alphaDecodeMs,
                         admitProfile.alphaHistogramMs, admitProfile.alphaShapeMs,
                         admitProfile.alphaHandoffScans, admitProfile.alphaBlockReadScans, admitProfile.alphaFullDecodeScans);
            }
            else
            {
                warmMs += totalMs;
                if (warmProfiled)
                    warmUpdateProfile->Add(admitProfile, totalMs, geoOwnerMs);
                if (warmProfiled && totalMs >= 2.0)
                {
                    // Process cap, not a world/sample cap. All measured totals are
                    // owner CPU time; inclusive scene/register/proxy buckets overlap.
                    static uint32_t rows = 0;
                    static bool truncated = false;
                    if (rows < 128)
                    {
                        ++rows;
                        LOG_INFO(World,
                                 "Object stream warm profile: row={} id={} model={} admitted={} totalMs={:.3f} "
                                 "geoOwnerMs={:.3f} sceneInclusiveMs={:.3f} "
                                 "registerInclusiveMs={:.3f} proxyInclusiveMs={:.3f} meshBuildMs={:.3f} "
                                 "meshCreateMs={:.3f} sectionInclusiveMs={:.3f} modelFfiMs={:.3f} "
                                 "texReadMs={:.3f} texCreateMs={:.3f} texFallbackMs={:.3f} alphaMs={:.3f} "
                                 "textureWindowWaitSubsetMs={:.3f} texHeaderMs={:.3f} texHeaders={} "
                                 "meshes={} sections={} uploads={} preparedUploads={} gpuChildInstances={} "
                                 "parkHits={} parkStale={} alphaPartsMs={:.3f}/{:.3f}/{:.3f}/{:.3f}",
                                 rows, placement.id, (const char*)modelName, admitted != nullptr, totalMs,
                                 geoOwnerMs, admitProfile.sceneMs, admitProfile.registerMs,
                                 admitProfile.proxiesMs, admitProfile.meshBuildMs, admitProfile.meshCreateMs,
                                 admitProfile.sectionMs, admitProfile.modelRegisterMs, admitProfile.textureReadMs,
                                 admitProfile.textureCreateMs, admitProfile.textureFallbackMs, admitProfile.alphaScanMs,
                                 admitProfile.textureWindowWaitMs, admitProfile.texHeaderLoadMs,
                                 admitProfile.texHeaderLoads, admitProfile.meshes, admitProfile.sections,
                                 admitProfile.textureUploads, admitProfile.texturePrepared, admitProfile.proxyModels,
                                 admitProfile.parkHits, admitProfile.parkStale, admitProfile.alphaSourceReadMs,
                                 admitProfile.alphaDecodeMs, admitProfile.alphaHistogramMs, admitProfile.alphaShapeMs);
                    }
                    else if (!truncated)
                    {
                        truncated = true;
                        LOG_INFO(World, "Object stream warm profile: process row cap 128 reached; further slow rows omitted");
                    }
                }
            }
            placement.resident = admitted;
            ObserveModernSimulationPlacement(index, admitted);
            if (admitted)
            {
                if (_nativeFarActive.erase(index))
                    GEngine->SceneFarObjectRemoved(index);
                ++created;
                // (a) If the eviction cache was holding this model, the ObjectCreate above found
                // it in ShapeBank instead of decoding it again, and the Object now owns it. Drop
                // the cache's reference: the entry has done its job, and leaving it would spend
                // a cache slot on a model that is resident and therefore in no danger. Counted,
                // because the hit rate is the only honest measure of whether the cache is worth
                // its memory on a given world.
                AdoptCachedShape(admitted->GetShape());
            }
            else
                ++refused;
        }
        if (cursor >= placements.size())
            _modernObjectCellActive[cell.index] = 1;
        if (admitBudgetSpent())
            break;
    }
    // A proxy or another admission can populate ShapeBank after its asynchronous
    // request was queued. Its prepared copy then has no consumer: warm admissions
    // bypass Take(). Under byte pressure even one such copy can park every parser.
    // Reclaim only completed, demonstrably redundant results on the owner thread;
    // never discard a cold result or change active conversion ownership.
    if (asyncOn && created == 0 && waited > 0)
    {
        const auto prep = _modernObjectPreparer->SnapshotStats();
        if (prep.readyByteParks && (prep.readyPayloadBytes || prep.conversionReservedBytes) &&
            !prep.parseReservedBytes)
        {
            uint32_t redundant = 0;
            for (uint32_t i = 0; i < _modernObjectModels.size(); ++i)
            {
                const auto state = _modernObjectPreparer->Query(i);
                if ((state == ObjectStreamPreparer::State::Ready ||
                     state == ObjectStreamPreparer::State::Converted) &&
                    Shapes.Find(_modernObjectModels[i], false, true))
                {
                    if (state == ObjectStreamPreparer::State::Ready)
                    {
                        if (_modernObjectPreparer->Take(i)) ++redundant;
                    }
                    else
                    {
                        auto converted = _modernObjectPreparer->TakeConverted(i);
                        delete converted.shape;
                        ++redundant;
                    }
                }
            }
            if (redundant)
                LOG_INFO(World, "Object stream prepare: released {} redundant warm results", redundant);
        }
    }
    _modernObjectAsyncInstalled += installed;
    _modernObjectAsyncWaited += waited;
    _modernObjectSyncCold += syncCold;
    _modernResidentObjectCount = released > _modernResidentObjectCount + created
                                     ? 0
                                     : _modernResidentObjectCount + created - released;
    if (warmUpdateProfile && warmUpdateProfile->totalMs >= 16.0)
        warmUpdateProfile->EmitSummary(centerX, centerZ);
    // CONVERGENCE. Counted idle updates, not `pending`, and that is the whole lesson of
    // this function: two earlier versions latched on `!_modernObjectResidencyPending` and
    // neither ever fired in a 1500-frame run, because a world whose requested set sits just
    // under the resident budget (19994 of 20000 on the world this was measured on) keeps at
    // least one desired-but-inactive cell for ever. `pending` is therefore not a statement
    // about the FILL being finished; it is a statement about the budget being tight.
    //
    // Three consecutive updates that created nothing is. It cannot be starved by a cell that
    // will never be admitted, and it is a counter, so it means the same thing on every
    // machine.
    // WHEN THE MODE ENDS, and it took three attempts to get this right. Both obvious
    // conditions are wrong on a real world:
    //
    //  * `!_modernObjectResidencyPending` never becomes true when the requested set sits
    //    just under the resident budget (19994 of 20000 on the world this was measured on):
    //    at least one desired cell can never be admitted, so `pending` is a statement about
    //    the budget being tight, not about the fill being finished.
    //  * "three updates that created nothing" never becomes true either, for the same
    //    reason -- at the budget ceiling the streamer evicts and re-admits continuously and
    //    `created` is never 0.
    //
    // So the bound is a COUNT OF UPDATES, which is the one thing that cannot be starved by
    // either. kPreloadMaxUpdates is generous next to the 7-8 rounds a full window actually
    // takes, and it is what makes the mode safe to ship: it lifts the per-frame caps for a
    // bounded prefix of the run and then hands them back, whatever the world does.
    if (preloading)
    {
        const bool idle = created == 0 && ++_modernObjectPreloadIdleUpdates >= 3;
        if (created != 0)
        {
            _modernObjectPreloadIdleUpdates = 0;
        }
        if (idle || _modernObjectPreloadUpdates >= kPreloadMaxUpdates)
        {
            _modernObjectPreloadDone = true;
            LOG_INFO(World, "Modern object preload: ended after {} updates ({}); per-update caps back on",
                     _modernObjectPreloadUpdates, idle ? "window idle" : "update bound reached");
        }
    }

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
    // (d) A row is also due on any update that admitted a cold model, else the interesting
    // frames -- the ones this change exists for -- would be exactly the silent ones. Bounded by
    // the number of distinct cold models a run touches (hundreds), not by frames.
    const bool asyncRow = (installed + syncCold + tablesPrepared) > 0;
    if (centerChanged || !_modernObjectResidencyPending || progressRow || asyncRow)
    {
        if (tablesPrepared > 0 || tableDeferred > 0)
            LOG_INFO(World, "Object stream bank tables: prepared={} deferredPlacements={} limit={} ms={:.3f}",
                     tablesPrepared, tableDeferred, tableLimit, tablePrepareMs);
        // The three new fields exist so the fix can be judged from the log alone. `desired` is
        // the effective window radius (the candidate `window` is the grown one and has always
        // been much larger); `lead` is the prefetch bias in metres, and is 0 whenever the camera
        // is below the deadband, so a walking run reads exactly as it did before; `cache` is
        // held/limit, `hits` are re-admissions that skipped an ODOL decode, and `ins`/`drops`
        // are what separates a cache that is working from one that is thrashing -- drops far
        // above hits means the bound is too small for this world's model turnover, not that the
        // idea is wrong. RND-040's headline number to beat is created=15958 / released=77643
        // over 112 s at 30 m/s: if hits do not grow into that, the cache is not earning its
        // memory on this world.
        //
        // (d) `async` fields, this update then cumulative: inst = objects admitted on a
        // worker-prepared IR, wait = admissions skipped because the IR was not ready, sync =
        // cold models loaded on this thread anyway; warm/cold ms = this update's time in
        // ObjectCreate for resident vs cold models; then the preparer's own counters (prepared
        // IRs, currently queued / ready, cumulative worker parse time and the slowest parse),
        // and the cumulative cold split parse/adapt/opt/create in ms over N cold models. If
        // `parse` is a small share of that split, the parse was not where the ~90 ms lived
        // and this change is a measured null result -- read it before believing `inst`.
        ObjectStreamPreparer::Stats prep;
        if (asyncOn)
            prep = _modernObjectPreparer->SnapshotStats();
        // Cumulative store view for the texture-prepare row below; cheap (one mutex hop) and
        // only taken when a row is due anyway.
        const render::PreparedTextureStore::Stats texStore =
            render::PreparedTextureStore::Instance().SnapshotStats();
        LOG_INFO(World,
                 "Modern object residency: centre=({}, {}) window={} cells desired={} cells lead={:.0f} m "
                 "requested={} resident={} created={} released={} refused={} budget={} pending={} cache={}/{} "
                 "hits={} ins={} drops={} async={} inst={} wait={} sync={} warm={:.1f}ms cold={:.1f}ms "
                 "prep={}/{} q={} ready={} notloose={} failed={} dropped={} worker={:.0f}ms max={:.0f}ms "
                 "coldsplit parse/adapt/opt/create={:.0f}/{:.0f}/{:.0f}/{:.0f}ms over {} models "
                 "(inst={} wait={} sync={} total)",
                 centerX, centerZ, _modernObjectCandidateRadius, _modernObjectDesiredRadiusCells,
                 _modernObjectLeadMetres, requested, _modernResidentObjectCount, created, released, refused,
                 _modernObjectBudget, _modernObjectResidencyPending ? 1 : 0, _modernObjectShapeCache.size(),
                 _modernObjectShapeCacheLimit, _modernObjectShapeCacheHits, _modernObjectShapeCacheInserts,
                 _modernObjectShapeCacheDrops, asyncOn ? "on" : "off", installed, waited, syncCold, warmMs, coldMs,
                 prep.prepared, prep.requested, prep.queued, prep.ready, prep.notLoose, prep.failed, prep.dropped,
                 prep.workerMs, prep.workerMaxMs, _modernObjectColdParseMs, _modernObjectColdAdaptMs,
                 _modernObjectColdOptimizeMs, _modernObjectColdCreateMs, _modernObjectColdModels,
                 _modernObjectAsyncInstalled, _modernObjectAsyncWaited, _modernObjectSyncCold);
        if (registrationQuota)
            LOG_INFO(World,
                     "Object stream registration quota: reusable={} needed={} unknown={} warmCharged={} "
                     "promotions={} expensiveAttempts={} limit={} ownerStateOnly=1",
                     registrationReusable, registrationNeeded, registrationUnknown, registrationCharged,
                     registrationPromotions, coldAttempted, admitColdBudget);
        // The `create` split and the texture-prepare pipeline, cumulative, as their own row so
        // the main row's format (which scripts already parse) is untouched. `createsplit` sums
        // to ~create above: reg (RegisterGpuModel) + other (NewObject/AddObject/lights); reg
        // further divides into mesh/sect/tex/ffi (tex = read/create/fallback/alpha). texprep is
        // the worker pipeline: prepared chains (count/MB/worker-ms), consumed = uploads served
        // from the store vs all real uploads during cold admission, skipped/unreadable are the
        // worker's refusals, store = entries/MB currently held (expired = TTL drops). park is
        // the whole-registration reuse (WGR_GPU_MODEL_PARK): hits skipped a rebuild outright,
        // stale found moved textures and rebuilt.
        LOG_INFO(World,
                 "Modern object create split: scene={:.0f}ms [reg={:.0f} mesh={:.0f}+{:.0f} sect={:.0f} "
                 "tex read/create/fb/alpha={:.0f}/{:.0f}/{:.0f}/{:.0f} ffi={:.0f} prox={:.0f}] "
                 "texprep worker={}chains/{:.1f}MB/{:.0f}ms consumed={}/{} skipped={} unreadable={} "
                 "store={}entries/{:.1f}MB expired={} park hit={} stale={}",
                 _modernObjectColdSceneMs, _modernObjectColdRegisterMs, _modernObjectColdMeshBuildMs,
                 _modernObjectColdMeshCreateMs,
                 std::max(0.0, _modernObjectColdSectionMs - _modernObjectColdTexReadMs -
                                   _modernObjectColdTexCreateMs - _modernObjectColdTexFallbackMs -
                                   _modernObjectColdAlphaMs),
                 _modernObjectColdTexReadMs, _modernObjectColdTexCreateMs, _modernObjectColdTexFallbackMs,
                 _modernObjectColdAlphaMs, _modernObjectColdModelFfiMs, _modernObjectColdProxiesMs,
                 prep.texPrepared, prep.texPreparedBytes / (1024.0 * 1024.0), prep.texMs,
                 _modernObjectColdTexPrepared, _modernObjectColdTexUploads, prep.texSkipped, prep.texUnreadable,
                 texStore.entries, texStore.bytes / (1024.0 * 1024.0), texStore.expired,
                 _modernObjectColdParkHits, _modernObjectColdParkStale);
    }
}

void Landscape::ClearNativeFarResidency()
{
    if (GEngine && !_nativeFarActive.empty())
        GEngine->ClearFarObjects();
    _nativeFarActive.clear();
    _nativeFarModels.clear();
    _nativeFarCandidates.clear();
    _nativeFarWanted.clear();
    _nativeFarModelWanted.clear();
    _nativeFarModelRejected.clear();
    _nativeFarCursor = 0;
    _nativeFarCenterX = _nativeFarCenterZ = -0x3fffffff;
}

void Landscape::UpdateNativeFarResidency(Vector3Par cameraPosition)
{
    // Kept opt-in until the authored-mesh cost/coverage and lifetime tests pass.
    static const bool enabled = [] {
        const char* value = std::getenv("WGR_NATIVE_FAR_AUTHORED");
        return value && value[0] == '1';
    }();
    if (!enabled || !_modernObjectStreaming || GetEnfusionSurfaceCount() == 0 || !GEngine)
        return;
    if (!std::isfinite(cameraPosition.X()) || !std::isfinite(cameraPosition.Z()) || _landGrid <= 0)
        return;
    constexpr float range = 3000.0f;
    constexpr size_t candidateBudget = 200000;
    const int cx = static_cast<int>(std::clamp(cameraPosition.X() / _landGrid,
                                             0.0f, static_cast<float>(_landRange - 1))) / 16 * 16;
    const int cz = static_cast<int>(std::clamp(cameraPosition.Z() / _landGrid,
                                             0.0f, static_cast<float>(_landRange - 1))) / 16 * 16;
    if (cx != _nativeFarCenterX || cz != _nativeFarCenterZ)
    {
        _nativeFarCenterX = cx;
        _nativeFarCenterZ = cz;
        _nativeFarWanted.resize(_modernObjectPlacements.size(), 0);
        for (auto& state : _nativeFarWanted)
            state &= 2; // preserve observed destruction, rebuild only the wanted bit
        _nativeFarModelWanted.assign(_modernObjectModels.size(), 0);
        _nativeFarModelRejected.resize(_modernObjectModels.size(), 0);
        _nativeFarCandidates.clear();
        _nativeFarCursor = 0;
        const int radius = static_cast<int>(std::min(std::ceil(range / _landGrid),
                                                    static_cast<float>(_landRange)));
        std::vector<ModernObjectCellCandidate> cells;
        for (int z = std::max(0, cz - radius); z <= std::min(_landRange - 1, cz + radius); ++z)
            for (int x = std::max(0, cx - radius); x <= std::min(_landRange - 1, cx + radius); ++x)
            {
                const uint32_t d2 = (x - cx) * (x - cx) + (z - cz) * (z - cz);
                const uint32_t cell = z * _landRange + x;
                if (d2 <= static_cast<uint32_t>(radius * radius) &&
                    cell < _modernObjectCells.size() && !_modernObjectCells[cell].empty())
                    cells.push_back({cell, d2});
            }
        std::sort(cells.begin(), cells.end(), [](const auto& a, const auto& b) {
            return a.distance2 != b.distance2 ? a.distance2 < b.distance2 : a.index < b.index;
        });
        for (const auto& cell : cells)
        {
            for (uint32_t index : _modernObjectCells[cell.index])
            {
                const auto& placement = _modernObjectPlacements[index];
                if (placement.modelIndex >= _modernObjectModels.size() || (_nativeFarWanted[index] & 2))
                    continue;
                _nativeFarCandidates.push_back(index);
                _nativeFarWanted[index] = 1;
                _nativeFarModelWanted[placement.modelIndex] = 1;
                if (_nativeFarCandidates.size() >= candidateBudget)
                    break;
            }
            if (_nativeFarCandidates.size() >= candidateBudget)
                break;
        }
        for (auto it = _nativeFarActive.begin(); it != _nativeFarActive.end();)
        {
            if ((_nativeFarWanted[*it] & 1) == 0 || _modernObjectPlacements[*it].resident)
            {
                GEngine->SceneFarObjectRemoved(*it);
                it = _nativeFarActive.erase(it);
            }
            else
                ++it;
        }
        for (auto it = _nativeFarModels.begin(); it != _nativeFarModels.end();)
            if (!_nativeFarModelWanted[it->first])
                it = _nativeFarModels.erase(it);
            else
                ++it;
        // Preserve near requests and retire old far work through the SAME queue.
        if (_modernObjectPreparer && _modernObjectModelPrefetchEpoch.size() == _nativeFarModelWanted.size())
        {
            auto epochs = _modernObjectModelPrefetchEpoch;
            for (size_t i = 0; i < epochs.size(); ++i)
                if (_nativeFarModelWanted[i])
                    epochs[i] = _modernObjectPrefetchEpoch;
            PreserveModernSimulationModelRequests(epochs, _modernObjectPrefetchEpoch);
            PreserveModernSourceDiagnosticRequests(epochs, _modernObjectPrefetchEpoch);
            _modernObjectPreparer->DropStale(epochs.data(), epochs.size(), _modernObjectPrefetchEpoch);
        }
        LOG_INFO(World, "Native authored far window: candidates={} active={} models={} radius={}m cap={}",
                 _nativeFarCandidates.size(), _nativeFarActive.size(), _nativeFarModels.size(), range,
                 candidateBudget);
    }
    if (_nativeFarCandidates.empty())
        return;
    const auto start = std::chrono::steady_clock::now();
    uint32_t added = 0, modelWork = 0;
    bool completedSweep = false;
    for (size_t scan = 0; scan < std::min<size_t>(4096, _nativeFarCandidates.size()); ++scan)
    {
        if (scan > 0 && std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - start).count() >= 2.0)
            break;
        const uint32_t index = _nativeFarCandidates[_nativeFarCursor];
        _nativeFarCursor = (_nativeFarCursor + 1) % _nativeFarCandidates.size();
        completedSweep |= _nativeFarCursor == 0;
        const auto& placement = _modernObjectPlacements[index];
        const uint32_t modelIndex = placement.modelIndex;
        if (placement.resident || (_nativeFarWanted[index] & 2) || _nativeFarActive.count(index) ||
            _nativeFarModelRejected[modelIndex])
            continue;
        auto found = _nativeFarModels.find(modelIndex);
        if (found == _nativeFarModels.end())
        {
            const RStringB& name = _modernObjectModels[modelIndex];
            Ref<LODShapeWithShadow> shape = Shapes.Find(name, false, true);
            if (!shape)
            {
                if (_modernPendingOwnerPrimaryTextures &&
                    _modernPendingOwnerPrimaryTextures->modelIndex == modelIndex)
                    continue; // The near owner already holds this Converted shape.
                // Near admissions win. Never decode a cold far model synchronously.
                if (_modernObjectResidencyPending || modelWork || !_modernObjectPreparer ||
                    !_modernObjectPreparer->Running())
                    continue;
                const auto state = _modernObjectPreparer->Query(modelIndex);
                if (state == ObjectStreamPreparer::State::Unknown)
                    _modernObjectPreparer->Request(modelIndex);
                else if (state == ObjectStreamPreparer::State::Ready)
                {
                    auto model = _modernObjectPreparer->Take(modelIndex);
                    if (model)
                    {
                        ++modelWork;
                        std::string low(static_cast<const char*>(name));
                        for (char& c : low)
                            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                        model->sourcePath = low;
                        auto tables = std::make_shared<Model::ShapeAdapter::AdapterBankTables>();
                        Model::ShapeAdapter::BuildAdapterBankTables(*model, *tables);
                        const auto radiusToken = _modernObjectPreparer->QueryRadius(modelIndex);
                        _modernObjectPreparer->SubmitConvert(modelIndex, std::move(model), std::move(tables), &radiusToken);
                    }
                }
                else if (state == ObjectStreamPreparer::State::Converted)
                {
                    auto converted = _modernObjectPreparer->TakeConverted(modelIndex);
                    if (converted.shape)
                    {
                        ++modelWork;
                        ShapeBank::WorldModelScope worldModel(Shapes);
                        shape = Shapes.NewFromModel(name, false, true, std::move(converted.model),
                                                    nullptr, converted.shape);
                    }
                }
                else if (state == ObjectStreamPreparer::State::Failed || state == ObjectStreamPreparer::State::NotLoose)
                    _nativeFarModelRejected[modelIndex] = 1;
                if (!shape)
                    continue;
            }
            // Tiny props do not earn a far instance. No name-based asset substitutions.
            if (shape->BoundingSphere() < 2.0f)
            {
                _nativeFarModelRejected[modelIndex] = 1;
                continue;
            }
            found = _nativeFarModels.emplace(modelIndex, shape).first;
        }
        Matrix4 transform;
        transform.SetDirectionAside(Vector3(placement.rows[0], placement.rows[1], placement.rows[2]));
        transform.SetDirectionUp(Vector3(placement.rows[3], placement.rows[4], placement.rows[5]));
        transform.SetDirection(Vector3(placement.rows[6], placement.rows[7], placement.rows[8]));
        transform.SetPosition(Vector3(placement.rows[9], placement.rows[10], placement.rows[11]));
        const float scale = transform.Scale();
        transform.SetUpAndAside(transform.DirectionUp(), transform.DirectionAside());
        transform.SetScale(scale);
        if (GEngine->SceneFarObjectCreated(index, found->second, transform))
        {
            _nativeFarActive.insert(index);
            ++added;
        }
        else
            _nativeFarModelRejected[modelIndex] = 1;
    }
    if (completedSweep && _modernObjectPreparer &&
        _modernObjectModelPrefetchEpoch.size() == _nativeFarModelWanted.size())
    {
        // Near and far can install the same model while its earlier parse is still
        // in flight. Retire redundant results rather than pinning ready-queue slots.
        auto epochs = _modernObjectModelPrefetchEpoch;
        for (size_t i = 0; i < epochs.size(); ++i)
        {
            if (_nativeFarModelWanted[i] && !_nativeFarModelRejected[i])
                epochs[i] = _modernObjectPrefetchEpoch;
            const auto state = _modernObjectPreparer->Query(static_cast<uint32_t>(i));
            if ((state == ObjectStreamPreparer::State::Ready ||
                 state == ObjectStreamPreparer::State::Converted) &&
                Shapes.Find(_modernObjectModels[i], false, true))
                epochs[i] = _modernObjectPrefetchEpoch - 1;
        }
        PreserveModernSimulationModelRequests(epochs, _modernObjectPrefetchEpoch);
        PreserveModernSourceDiagnosticRequests(epochs, _modernObjectPrefetchEpoch);
        const size_t retired = _modernObjectPreparer->DropStale(
            epochs.data(), epochs.size(), _modernObjectPrefetchEpoch);
        if (retired)
            LOG_INFO(World, "Native authored far: retired {} unneeded prepared results", retired);
    }
    if (added && (_nativeFarActive.size() / 10000 != (_nativeFarActive.size() - added) / 10000))
        LOG_INFO(World, "Native authored far progress: active={} sharedModels={} near={}",
                 _nativeFarActive.size(), _nativeFarModels.size(), _modernResidentObjectCount);
}

void Landscape::SnapshotObjectStreamDiag(Poseidon::Dev::ObjectStreamResidencyCounters& out) const
{
    // The same members the two "Modern object residency" log rows print, as data. A world
    // without authored placements has no streaming system; `valid` false says "no streamed
    // world" rather than a screen of zeros.
    out.valid = !_modernObjectPlacements.empty();
    if (!out.valid)
        return;
    out.residentObjects = _modernResidentObjectCount;
    out.requiredObjects = _modernRequiredObjects.size();
    out.logicalOnlyObjects = _modernLogicalOnlyObjects.size();
    out.budget = _modernObjectBudget;
    out.shapeCacheSize = _modernObjectShapeCache.size();
    out.shapeCacheLimit = _modernObjectShapeCacheLimit;
    out.shapeCacheHits = _modernObjectShapeCacheHits;
    out.shapeCacheInserts = _modernObjectShapeCacheInserts;
    out.shapeCacheDrops = _modernObjectShapeCacheDrops;
    out.asyncInstalled = _modernObjectAsyncInstalled;
    out.asyncWaited = _modernObjectAsyncWaited;
    out.syncCold = _modernObjectSyncCold;
    out.coldModels = _modernObjectColdModels;
    out.coldParseMs = _modernObjectColdParseMs;
    out.coldAdaptMs = _modernObjectColdAdaptMs;
    out.coldOptimizeMs = _modernObjectColdOptimizeMs;
    out.coldCreateMs = _modernObjectColdCreateMs;
    out.coldTexHeaderMs = _modernObjectColdTexHeaderMs;
    out.coldTexHeaderLoads = _modernObjectColdTexHeaderLoads;
    out.coldTexReadMs = _modernObjectColdTexReadMs;
    out.coldTexCreateMs = _modernObjectColdTexCreateMs;
    out.coldTexFallbackMs = _modernObjectColdTexFallbackMs;
    out.coldAlphaMs = _modernObjectColdAlphaMs;
    out.coldTexUploads = _modernObjectColdTexUploads;
    out.coldTexPrepared = _modernObjectColdTexPrepared;
    out.parkHits = _modernObjectColdParkHits;
    out.parkStale = _modernObjectColdParkStale;
    out.admitUpdates = _modernObjectAdmitUpdates;
    out.admitStopCap = _modernObjectAdmitStopCap;
    out.admitStopUpload = _modernObjectAdmitStopUpload;
    out.admitStopCeiling = _modernObjectAdmitStopCeiling;
    out.admitStopBudget = _modernObjectAdmitStopBudget;
    out.admitStopCold = _modernObjectAdmitStopCold;
    out.admitCounted = ObjectStreamAdmitCounted();
    out.admitColdBudget = ObjectStreamColdPerUpdate();
    // The armed flags come from the same accessors the admit loop reads, so a consumer that
    // renders an unarmed bound as n/a cannot disagree with what the loop actually did.
    out.admitUploadCeilingArmed = ObjectStreamUploadBudgetBytes() > 0;
    // In counted mode the clocks are never consulted, so the wall ceiling is unarmed whatever
    // WGR_OBJECT_STREAM_MAX_MS says; the consumer renders it n/a rather than 0.
    out.admitWallCeilingArmed = !ObjectStreamAdmitCounted() && ObjectStreamCeilingMs() > 0.0f;
    out.prepValid = _modernObjectPreparer && _modernObjectPreparer->Running();
    if (out.prepValid)
        out.prep = _modernObjectPreparer->SnapshotStats();
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
    const int authoredFloor = AuthoredObjectIDFloor();
    if (_modernObjectStreaming && ar.IsSaving())
        _objectId = std::max(_objectId, authoredFloor);
    PARAM_CHECK(ar.Serialize("lastObjectID", _objectId, 1))
    // Old saves may contain a counter derived only from the camera's resident subset.
    // Preserve larger dynamic counters, but never allocate inside unseen authored IDs.
    if (_modernObjectStreaming && !ar.IsSaving())
        _objectId = std::max(_objectId, authoredFloor);
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
    int maxId = AuthoredObjectIDFloor();
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
    const DWORD idStart = Poseidon::Foundation::GlobalTickCount();
    long long walked = 0;
    long long cached = 0;
    int x, z;
    int maxId = AuthoredObjectIDFloor();
    for (x = 0; x < _landRange; x++)
    {
        for (z = 0; z < _landRange; z++)
        {
            const ObjectList& list = _objects(x, z);
            for (int i = 0; i < list.Size(); i++)
            {
                Object* obj = list[i];
                ++walked;
                int id = obj->ID();
                if (id >= 0)
                {
                    _objectIds[id] = obj;
                    ++cached;
                    if (id > maxId)
                    {
                        maxId = id;
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
    LOG_INFO(World, "RebuildIDCache: walked {} objects in {}x{} land cells, cached {} ids (max id {}) in {} ms",
             walked, _landRange, _landRange, cached, maxId, Poseidon::Foundation::GlobalTickCount() - idStart);
}

void Landscape::AddToIDCache(Object* object)
{
    int id = object->ID();
    _objectIds[id] = object;
}

int Landscape::AuthoredObjectIDFloor() const
{
    return Streaming::AuthoredObjectIdFloor(_modernObjectStreaming, _modernObjectPlacements);
}

Object* Landscape::ResolveObject(int id)
{
    if (!_modernObjectStreaming || id < 0)
        return GetObject(id);
    const auto cached = _objectIds.find(id);
    if (cached != _objectIds.end() && cached->second)
    {
        _modernRequiredObjects.insert(id);
        return cached->second;
    }
    // Explicit, rare gameplay lookup. Do not build a second multi-million-entry
    // hash table, nor load the whole world on a dedicated server. Repeat lookups
    // hit the existing ID map. Background workers never touch this owner-thread path.
    for (auto& placement : _modernObjectPlacements)
    {
        if (placement.id != id || placement.modelIndex >= _modernObjectModels.size())
            continue;
        Matrix4 transform;
        transform.SetDirectionAside(Vector3(placement.rows[0], placement.rows[1], placement.rows[2]));
        transform.SetDirectionUp(Vector3(placement.rows[3], placement.rows[4], placement.rows[5]));
        transform.SetDirection(Vector3(placement.rows[6], placement.rows[7], placement.rows[8]));
        transform.SetPosition(Vector3(placement.rows[9], placement.rows[10], placement.rows[11]));
        ShapeBank::WorldModelScope worldModel(Shapes);
        Object* object = ObjectCreate(id, _modernObjectModels[placement.modelIndex], transform,
                                      nullptr, nullptr, false, true, false);
        if (!object)
            return nullptr;
        placement.resident = object;
        ObserveModernSimulationPlacement(uint32_t(&placement - _modernObjectPlacements.data()), object);
        _modernRequiredObjects.insert(id);
        _modernLogicalOnlyObjects.insert(id);
        if (_geoAccumEnabled)
            AccumulateGeographyForObject(object);
        const int x = std::clamp(static_cast<int>(std::floor(placement.rows[9] * _invLandGrid)), 0, _landRange - 1);
        const int z = std::clamp(static_cast<int>(std::floor(placement.rows[11] * _invLandGrid)), 0, _landRange - 1);
        const size_t cell = static_cast<size_t>(z) * _landRange + x;
        _modernObjectCellActive[cell] = 0;
        _modernObjectCellCursor[cell] = 0;
        _modernObjectResidencyPending = true;
        LOG_INFO(World, "Object stream: resolved required identity {} without visual admission", id);
        return object;
    }
    return nullptr;
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
