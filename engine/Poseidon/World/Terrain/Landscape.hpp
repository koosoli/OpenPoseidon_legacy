#pragma once

#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/AI/Path/AITypes.hpp>
#include <Poseidon/Foundation/Containers/BankArray.hpp>
#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp>
#include <Poseidon/World/Terrain/ObjectStreamPrepare.hpp>
#include <Poseidon/World/Terrain/ShapeCachePressurePolicy.hpp>
#include <Poseidon/World/Terrain/SimulationObjectCoverage.hpp>
#include <Poseidon/World/Terrain/SimulationResidency.hpp>
#include <Poseidon/World/Terrain/StaticPlainRouteProbe.hpp>

namespace Poseidon::Asset::Material
{
struct RvMaterialSource;
}
#include <array>
#include <chrono>
#include <list>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Poseidon
{
class TaskPool;
}
namespace Poseidon::Dev
{
struct ObjectStreamResidencyCounters;
}
namespace Poseidon
{
using Poseidon::TaskPool;

#define LandRange (GLandscape->GetLandRange())
#define LandRangeMask (GLandscape->GetLandRangeMask())
#define LandRangeLog (GLandscape->GetLandRangeLog())
#define InvLandRange (GLandscape->GetInvLandRange())

#define TerrainRange (GLandscape->GetTerrainRange())
#define TerrainRangeMask (GLandscape->GetTerrainRangeMask())
#define TerrainRangeLog (GLandscape->GetTerrainRangeLog())

#define LandGrid (GLandscape->GetLandGrid())
#define InvLandGrid (GLandscape->GetInvLandGrid())

#define TerrainGrid (GLandscape->GetTerrainGrid())
#define InvTerrainGrid (GLandscape->GetInvTerrainGrid())

#define LandSize (LandRange * LandGrid)
#define InvLandSize (InvLandRange * InvLandGrid)

// caution: many functions assume ObjRange==LandRange, ObjGrid==LandGrid

#define ObjRange (LandRange)
#define ObjGrid (LandGrid)
#define InvObjGrid (InvLandGrid)

// trick: instead of four comparisons use logical operations
// works if LandRange is power of 2
// following lines are equvalent
// #define InRange(z,x) ( z>=0 && x>=0 && z<LandRange && x<LandRange )
#define InRange(z, x) ((((z) | (x)) & ~LandRangeMask) == 0)

#define TerrainInRange(z, x) ((((z) | (x)) & ~TerrainRangeMask) == 0)

#define ObjInRange(z, x) ((((z) | (x)) & ~(ObjRange - 1)) == 0)

#define LANDDATA_SCALE (0.03f * 1.5f)

#define TACTICAL_VISIBILITY (ENGINE_CONFIG.tacticalZ)
#define RADAR_VISIBILITY (ENGINE_CONFIG.radarZ)

} // namespace Poseidon

// Canonical definition is at global scope (World/Simulation/Collisions.cpp).
void ObjRadiusRectangle(int& xMin, int& xMax, int& zMin, int& zMax, const Poseidon::Foundation::Vector3P& oPos,
                        const Poseidon::Foundation::Vector3P& nPos, float radius);

namespace Poseidon
{
void LandRadiusRectangle(int& xMin, int& xMax, int& zMin, int& zMax, Vector3Par oPos, Vector3Par nPos, float radius);

struct LandBegEnd
{
    int xBeg, zBeg, xEnd, zEnd;
    bool operator==(const LandBegEnd& src) const
    {
        if (xBeg == src.xBeg && zBeg == src.zBeg)
        { // all segment must be of same size
            PoseidonAssert(xEnd == src.xEnd && zEnd == src.zEnd);
        }
        return ((xBeg - src.xBeg) | (zBeg - src.zBeg)) == 0;
    }
};

#if !_MSC_VER
#ifndef __INTEL_COMPILER
#pragma warning 549 10
#endif
#endif

} // namespace Poseidon
#include <Poseidon/Foundation/Containers/SmallArray.hpp>
namespace Poseidon
{

class ObjectListFull : public SmallArray<Ref<Object>>
{
    typedef SmallArray<Ref<Object>> base;

    Vector3 _bCenter; // bounding sphere of static objects
    float _bRadius;
    int _nNonStatic;
    /// PERF-031: how many entries are `TypeVehicle`. `PredictCollision` skips a cell with
    /// none, which is the filter its own loop applies anyway -- and which `_nNonStatic`
    /// stopped approximating the day footprints started living in these cells: a `Mark` is
    /// non-static, so a cell that gained decals lost the skip and was then walked in full,
    /// static objects included.
    int _nTypeVehicle;
    // Non-owning mirror of TypeVehicle entries. Dense moving-unit scenes can
    // accumulate thousands of temporary footprint objects in the full cell
    // list; collision queries that only want vehicles must not scan them all.
    SmallArray<Object*> _typeVehicles;
    // Non-owning mirror of objects that can provide a roadway surface. Soldier
    // ground contact asks only for these after testing the terrain itself.
    SmallArray<Object*> _roadwayObjects;
    // Visibility rejects TypeTempVehicle unconditionally. Preserve the full
    // list order while avoiding footprint traversal on every sight query.
    SmallArray<Object*> _visibilityObjects;

  private:
    int CountNonStatic() const;
    int CountTypeVehicle() const;

  public:
    ObjectListFull(int x, int z);
    ~ObjectListFull();

    int GetNonStaticCount() const { return _nNonStatic; }
    int GetTypeVehicleCount() const { return _nTypeVehicle; }
    Object* GetTypeVehicle(int index) const { return _typeVehicles[index]; }
    int GetRoadwayObjectCount() const { return _roadwayObjects.Size(); }
    Object* GetRoadwayObject(int index) const { return _roadwayObjects[index]; }
    int GetVisibilityObjectCount() const { return _visibilityObjects.Size(); }
    Object* GetVisibilityObject(int index) const { return _visibilityObjects[index]; }
    void ChangeNonStaticCount(int val);
    void ChangeTypeVehicleCount(int val);
    void StaticChanged(); // recalculate what is neccessary
    void SetBSphere(Vector3Par center, float radius);
    void SetBSphere(int x, int z);

    Vector3 GetBSphereCenter() const { return _bCenter; }
    float GetBSphereRadius() const { return _bRadius; }

    // only add/remove methods
    int Add(Object* object, bool avoidRecalculation = false);
    void Delete(int index);
    void Clear();

    USE_FAST_ALLOCATOR
};

#if 1
// pointer to object list serving as object list
// used in landscape object index - many ObjectLists are be empty

class ObjectList
{
    SRef<ObjectListFull> _list;

  public:
    // check size, guranteed to be non-empty
    int SizeNotEmpty() const { return _list->Size(); }
    // check size, gurannteed to be non-empty
    int Size() const { return (_list ? _list->Size() : 0); }
    // get given object
    Object* operator[](int i) const
    {
        PoseidonAssert(_list);
        return _list->Get(i);
    }
    // add object
    int Add(const Ref<Object>& object, int x, int z, bool avoidRecalculation = false)
    {
        if (!_list)
            _list = new ObjectListFull(x, z);
        return _list->Add(object, avoidRecalculation);
    }
    // recalculate what is neccessary
    void Recalculate()
    {
        if (_list)
            _list->StaticChanged();
    }
    // delete given object
    void Delete(int index)
    {
        PoseidonAssert(_list);
        _list->Delete(index);
        if (_list->Size() == 0)
            _list.Free();
    }
    // compact memory usage
    void Compact()
    {
        if (_list)
        {
            _list->Compact();
            if (_list->Size() == 0)
                _list.Free();
        }
    }
    // release all memory
    void Clear() { _list.Free(); }

    // get underlying object list
    ObjectListFull* GetList() const { return _list; }
    // get underlying object list
    ObjectListFull* operator->() const { return _list; }
    // check if there is some list
    bool Null() const { return _list == nullptr; }
};

#else
typedef ObjectListFull ObjectList;
#endif

#define N_CLOUDS 4
#define MAX_SHAPES 256

#if _ENABLE_CHEATS
const int NClutters = 4;
#endif

} // namespace Poseidon
DEFINE_ENUM_BEG(GroundType)
GroundSolid, GroundWater DEFINE_ENUM_END(GroundType) namespace Poseidon
{
    // information about collision with ground
    struct UndergroundInfo
    {
        // texture (determines surface)
        Texture* texture;
        // object we are in contact with (may be null - contact with terrain)
        Object* obj;
        Point3 pos;      // world coordinate position of collision
        float under;     // how much are we under the ground level
        // WATER ONLY: the same submersion measured against the MEAN sea level, i.e. with the
        // wave taken out. `under` is the real depth and is what buoyancy must integrate; this is
        // for the terms that are really a function of how the hull SITS, not of which crest it
        // happens to be on. Ship's planing lift is one: it multiplies the up-force by up to 120x
        // with speed, and applied to a wave-varying depth that gain turns a swell into a
        // catapult. Equal to `under` on a flat sea and for every non-water contact.
        float underFlat;
        float dX, dZ;    // surface differential
        int vertex;      // which vertex of checked object
        int level;       // which level (landcontact or geometry)
        GroundType type; // type of collision (water, solid...)
    };

    // maintain cache of smaller rectangles (8x8)
    // this will improve texture consistency, vertex data locality
    // and caching efficiency

    class LandSegment : public RefCount, public CLRefLink
    {
        friend class Landscape;
        friend class LandCache;

      private:
        Shape _table;  // landscape geometry
        Shape _wTable; // water geometry

        LandBegEnd _rect;
        bool _valid;
        bool _someWater, _onlyWater;
        bool _needsGPU{false}; // true if CPU-generated but VBs not yet created
        int _lodLevel{0};      // 0=full detail, 1=2x stride, 2=4x stride
        float _seaLevel;       // what sea level is generated
        Poseidon::Foundation::Time _lastUsed;
        Vector3 _offset; // on T&L engine we try to make coordinates low

      private:
        void CalcBSphere();
        void CalcWBSphere();

      public:
        LandSegment();
        ~LandSegment() override;
        void Clear();
        bool ValidFor(const LandBegEnd& rect) const;
        bool VerifyStructure() const;

        Vector3Val Offset() const { return _offset; }

        // Read-only view of the generated geometry.  Everything above is reachable
        // only through `friend class Landscape` / `friend class LandCache`, which is
        // right for mutation but leaves a segment unverifiable from outside the
        // terrain tree.  Roadmap 8.6 needs to hash the COMPLETE output of
        // `Landscape::GenerateSegmentInto` at several TaskPool worker counts and
        // compare it bit for bit (tests/unit/engine/Poseidon/Core/
        // test_worker_determinism.cpp) — a spot check would not be evidence.  These
        // are const and add no state; they do not widen the mutation surface.
        const Shape& GetTable() const { return _table; }
        const Shape& GetWaterTable() const { return _wTable; }
        bool SomeWater() const { return _someWater; }
        bool OnlyWater() const { return _onlyWater; }
        int GetLodLevel() const { return _lodLevel; }

        USE_FAST_ALLOCATOR
    };

} // namespace Poseidon
#include <Poseidon/Foundation/Memory/MemFreeReq.hpp>

#include <Poseidon/World/Terrain/LandscapeLod.hpp>
namespace Poseidon
{

struct LandCacheSlot
{
    int xBeg, zBeg;
    int lodLevel;
    float lastUsed;
    Ref<LandSegment> segment;
};

class LandCache : public Poseidon::Foundation::MemoryFreeOnDemandHelper
{
    friend class Landscape;
    SegmentCache<LandCacheSlot> _cache;

  public:
    LandCache();
    void Clear();

    void Init(float viewDistance, float invLandGrid);

    Ref<LandSegment> Segment(Landscape* land, const LandBegEnd& rect, float currentTime);
    void Fill(Landscape* land, const Frame& pos, float currentTime, float viewDistance, Poseidon::TaskPool* pool);
    bool VerifyStructure() const;

    // { memory free on demand implementation
    size_t FreeOneItem() override;
    float Priority() override;
    const char* DomainName() const override { return "Terrain.LandCache"; }
    // }
};

class Weather
{
    friend class Landscape;
    float _overcastSetSky;
    float _overcastSetClouds;
    float _fogSet;
    float _cloudsPos;

    Ref<Texture> _sky;
    float _cloudsAlpha;
    float _cloudsBrightness;
    float _cloudsSpeed;
    float _skyThrough;

    float _rainDensity, _rainDensityWanted, _rainDensitySpeed;

    Poseidon::Foundation::Time _thunderBoltTime;
    Vector3 _windSpeed;
    Poseidon::Foundation::Time _lastWindSpeedChange;

    Vector3 _gust;
    Poseidon::Foundation::Time _gustUntil;

  public:
    Weather();
    void Init();
    void SetSky(Landscape* land, RStringB name);
    void SetSky(Landscape* land, RStringB n1, RStringB n2, float factor);
    void SetClouds(float alpha, float brightness, float speed, float through);
    void MoveClouds(float deltaT);

    void SetOvercast(Landscape* land, float overcast);
    void SetRain(float density, float time);
    // Defined inline alongside GetFog. It was declared here and defined nowhere in the
    // tree, so calling Landscape::GetOvercast() — which forwards to this — was a link
    // error waiting for its first caller; the Zeus weather panel found it in 2026-08.
    // _overcastSetClouds is the right source: SetOvercast saturates its argument to
    // [0,1] and stores it there, so this returns what was last asked for.
    float GetOvercast() const { return _overcastSetClouds; }

    void SetFog(Landscape* land, float fog);
    float GetFog() const { return _fogSet; }

    Texture* SkyTexture() const { return _sky; }
};

inline float ShortToHeight(short val)
{
    return val * LANDDATA_SCALE;
}
inline short HeightToShort(float val)
{
    return toIntFloor(val * (1 / LANDDATA_SCALE));
}

class GroundCollisionBuffer : public StaticArray<UndergroundInfo>
{
  public:
    GroundCollisionBuffer();
    ~GroundCollisionBuffer();
};

struct VehicleCollision
{
    const EntityAI* who;
    Vector3 pos;
    float distance;
    float time;
};

class VehicleCollisionBuffer : public StaticArray<VehicleCollision>
{
  public:
    VehicleCollisionBuffer();
    ~VehicleCollisionBuffer();
};

#if 1
#define RawToHeight(x) (x)
#define HeightToRaw(x) (x)
typedef float RawType;
#else
#define RawToHeight(x) ShortToHeight(x)
#define HeightToRaw(x) HeightToShort(x)
typedef short RawType;
#endif

} // namespace Poseidon
#include <Poseidon/Foundation/Containers/Array2D.hpp>
struct VisCheckContext;
struct CheckObjectCollisionContext;
class EditCursor;
#include <Random/randomGen.hpp>
namespace Poseidon
{

struct GroundLayerInfo;

struct A3TerrainUv
{
    std::array<float, 4> u{{1.0f, 0.0f, 0.0f, 0.0f}};
    std::array<float, 4> v{{0.0f, 1.0f, 0.0f, 0.0f}};
    bool worldPos = false;
};

// Lossless authored terrain description used by the WGPU terrain path for a
// later-generation material. The legacy path continues to use Landscape::_texture.
//
// Both generations author terrain normals and neither had anywhere to put them:
// the surface stack is `_nopx`/`_co` pairs on the odd/even stages, so every
// colour here has a normal beside it in the source. Arma 3 additionally carries
// one normal for the whole tile at Stage14 (`n_<x>_<y>_no.paa`), which Arma 2
// does not. Preserved whether or not the renderer consumes them yet.
//
// The surface stack is addressed BY STAGE, never by encounter order. A tile's
// LCA mask selects a slot, and the slot is the stage's position in the stack --
// Stage4 is slot 0, Stage6 slot 1, and so on. Faces that do not need a slot ship
// an rvmat with that stage empty (`p_015-010_n_l09_l13_n_l10_l11` leaves slots 0
// and 3 empty), which is the common case: 33,107 of Takistan's stage tokens are
// holes. Packing the named colours down into a dense list renumbers every slot
// after a hole and the mask then selects the wrong surface.
struct A3TerrainMaterial
{
    // Arma 2 / Operation Arrowhead's TerrainX carries six surface slots on
    // Stage4..Stage14; Arma 3's TerrainSNX carries five on Stage4..Stage12 and
    // spends Stage14 on the whole-tile normal instead.
    static constexpr size_t MaxSurfaces = 6;

    RStringB satellite;
    RStringB mask;
    std::array<RStringB, MaxSurfaces> surfaces;
    // Parallel to `surfaces`: the `_nopx` map authored on the odd stage
    // immediately before each colour. Empty where the source names none.
    std::array<RStringB, MaxSurfaces> surfaceNormals;
    // The whole tile's normal, sharing the satellite's frame. Arma 3 Stage14.
    RStringB tileNormal;
    A3TerrainUv satelliteUv;
    A3TerrainUv maskUv;
    std::array<A3TerrainUv, MaxSurfaces> surfaceUvs;
    // One past the highest slot this material names, so holes below it survive.
    unsigned char surfaceCount = 0;
};

#define USE_SWIZZLED_ARRAYS 0

// terrain and scene database storage

class Landscape : public SerializeClass
{
    friend class ::EditCursor;

    // hide GLandscape
    int GLandscape;

  protected:
    int _landRange;
    int _landRangeMask;
    int _landRangeLog;
    float _invLandRange;

    float _landGrid;
    float _invLandGrid;

    int _terrainRange;
    int _terrainRangeMask;
    int _terrainRangeLog;

    float _terrainGrid;
    float _invTerrainGrid;

#define this_TerrainInRange(z, x) ((((z) | (x)) & ~_terrainRangeMask) == 0)
#define this_InRange(z, x) ((((z) | (x)) & ~_landRangeMask) == 0)
#define this_ObjInRange(z, x) ((((z) | (x)) & ~_landRangeMask) == 0)

    Engine* _engine;
    World* _world;

    Array2D<GeographyInfo> _geography;
    Array2D<byte> _soundMap;
    AutoArray<Vector3> _mountains;

    // cached operational maps
    SRef<IOperCache> _operCache;
    SRef<ILockCache> _lockCache;

    RString _name; // current terrain file loaded

    struct TextureInfo
    {
        Ref<Texture> texture;
        bool offsetUV; // enable offseting
        // Four quadrant surfaces (TL, TR, BL, BR) decoded from the pre-blended texture name.
        // All four quadrants are the same for base textures.
        const SurfaceInfo* quadrants[4] = {nullptr, nullptr, nullptr, nullptr};
        operator Texture*() const { return texture; }
        Texture* operator->() const { return texture; }
    };
    AutoArray<TextureInfo> _texture;

    // OPRW25 terrain RVMATs are not a single texture: TerrainSNX carries a
    // satellite colour, an LCA mask, then one base and up to four overlay
    // surface colours. The legacy renderer still uses _texture's satellite
    // entry; the WGPU path consumes this lossless authored description.
    AutoArray<A3TerrainMaterial> _a3TerrainMaterials;
    // Sinkhole W5: set only by LoadRvmatTerrainMaterials (a raw 4WVR world naming RVMATs -- the converted Forest
    // island). Its land cells are 1.7 m, so the renderer's cell-count page window would reach ~110 m; such a world
    // is small enough to keep every material resident instead. False for every stock, Arma and Enfusion world.
    bool _rvmatCellMaterials = false;

    // RFG-062: the Enfusion `.emat` surface path per terrain palette entry, filled ONLY
    // by the native Reforger loader (LandLoadEnfusion.cpp). Every other world leaves it
    // empty, which is what keeps the native clutter binding inert by CONSTRUCTION rather
    // than by a name test that could one day match an OFP texture. Kept beside
    // _a3TerrainMaterials rather than inside it: an A3TerrainMaterial with a non-zero
    // surfaceCount flips the terrain shader onto the paged authored path, which a native
    // world has no satellite or LCA mask to feed.
    AutoArray<RStringB> _enfusionSurfaces;
    // RFG-027/056 + clutter: one byte per 12.5 m land cell (row-major, z * _landRange + x),
    // non-zero where an Enfusion road ribbon runs through the cell. Roads are not a
    // surface here -- they are draped triangle bands built from the RoadEntity splines
    // in LandLoadEnfusion.cpp -- so the cell under the tarmac keeps its grass surface
    // index, and without this mask the clutter bake grows grass ON the road. Filled by
    // the native loader only; empty (Size 0) on every other world, which is what keeps
    // the clutter-clearing inert there. Cleared in Init() like _enfusionSurfaces.
    std::vector<uint8_t> _enfusionRoadCells;
    // Dev switch (Grass > "Clutter cleared under Enfusion roads"): whether the bake
    // honours _enfusionRoadCells. A process-wide flag rather than a GrassSettings field
    // because the mask is a WORLD fact that lives here; the terrain re-bakes when it moves.
    static bool s_enfusionRoadClutterClear;

    Array2D<short> _tex;
    Array2D<ObjectList> _objects;

    PackedColor _colorizePalette[256]; // random color. palette

    struct RandomInfo
    {
        // random color. data
        unsigned int color : 8;
        // precalculated u-v offset (-7..+7)
        int uOff : 4;
        int vOff : 4;
    };

    RandomGenerator _randGen;

    Array2D<RandomInfo> _random;

    // access to data and textures
    Array2D<RawType> _data;

    float GetData(int x, int z) const { return RawToHeight(_data(x, z)); }
    void SetData(int x, int z, float data) { _data(x, z) = HeightToRaw(data); }

    // Elevation lives on the TERRAIN grid; _geography, _objects, _tex, _random and
    // this_InRange are all LAND space. The two coincided in OFP at load time and
    // nowhere else: SubdivideTerrain refines terrain under a fixed land grid, and
    // since Arma 1 the container itself ships them apart (Sahrani is 512 land
    // against 2048 terrain). Land-space code that wants "the height at the corner
    // of this land cell" must come through here -- passing a land index straight to
    // GetData silently samples the top-left 1/ratio^2 of the map and reports it as
    // the whole world.
    int TerrainPerLandLog() const { return _terrainRangeLog - _landRangeLog; }
    int TerrainPerLand() const { return 1 << TerrainPerLandLog(); }
    float GetLandData(int x, int z) const
    {
        const int log = TerrainPerLandLog();
        return GetData(x << log, z << log);
    }

    int GetTex(int x, int z) const { return _tex(x, z); }
    void SetTex(int x, int z, int data) { _tex(x, z) = data; }

#if _ENABLE_CHEATS
    Ref<LODShapeWithShadow> _clutter[NClutters];
#endif

    Ref<LODShapeWithShadow> _cloud[N_CLOUDS];
    Ref<Object> _cloudObj[N_CLOUDS];
    Ref<Object> _skyObject;
    Ref<Object> _horizontObject;
    Ref<Object> _sunObject;
    Ref<Object> _moonObject;
    Ref<Object> _starsObject;
    Weather _weather; // sky and clouds - textures and parameters

    float _seaLevel;     // sea level with tide
    float _seaLevelWave; // sea level with wave effects
    float _seaWaveSpeed;
    // OFP's sun+moon tide (maxTide 5 m). Cleared by LoadOprwModern: later-generation worlds
    // have no tide and are authored to a sea at 0. See Simulate().
    bool _tidal = true;

    mutable int _lastFindObjectX, _lastFindObjectZ; // usally query for ids on same square
    int _objectId;
    // id's are remmembered at least during load session.
    //
    // Keyed, not indexed. This was an AutoArray addressed by the object id, which
    // holds only while ids are dense from zero -- true of every world up to Arma 3,
    // and false of DayZ: Chernarus's ids start at 0x01000000 and run to 654,367,047
    // for 2,971,218 objects, so Access(id) tried to grow one element per id and took
    // the process down inside Realloc. A dense array would need 5.2 GB to hold 23 MB
    // of links. The map costs a hash per lookup and is bounded by the object count
    // rather than by the largest id any world happens to use.
    std::unordered_map<int, OLink<Object>> _objectIds;
    // Modern WRP placement database. Arma 3/DayZ worlds contain enough static
    // placements that instantiating every Object, retained mesh, and texture at
    // load time dominates both startup and VRAM. Keep the compact authored rows
    // for the whole map and instantiate a camera-centred cell budget instead.
    struct ModernObjectPlacement
    {
        int id = -1;
        uint32_t modelIndex = 0;
        std::array<float, 12> rows{};
        OLink<Object> resident;
    };
    // Explicit script/save/network identities and mutated state outlive camera residency.
    // Sparse sets avoid adding per-placement overhead to multi-million-object worlds.
    std::unique_ptr<Streaming::SimulationResidencyState> _modernSimulation;
    Streaming::SimulationResidencyStatus QueueModernSimulationRegionForChannel(
        Object& actor, Vector3Par from, Vector3Par to, double radius, bool automatic, uint8_t channel);
    Streaming::SimulationResidencyStatus ModernSimulationRegionStatusForChannel(const Object& actor, uint8_t channel) const;
    void ReleaseModernSimulationRegionForChannel(Object& actor, uint8_t channel);
    void ObserveModernSimulationPlacement(uint32_t placement, Object* object);
    void ResetModernSimulationResidency();
    void EnsureModernSimulationResidency();
    bool HasModernSimulationLease(uint32_t placement) const;
    void PreserveModernSimulationModelRequests(std::vector<uint32_t>& epochs, uint32_t epoch) const;
    std::unordered_set<int> _modernRequiredObjects;
    std::unordered_set<int> _modernLogicalOnlyObjects;
    void RegisterModernObjectVisual(Object* object);
    std::vector<ModernObjectPlacement> _modernObjectPlacements;
    int AuthoredObjectIDFloor() const;
    std::vector<std::vector<uint32_t>> _modernObjectCells;
    // One non-empty cell inside the residency window, with its squared cell distance from the
    // window centre. The list is a pure function of the (quantised) centre -- the window radius
    // is derived from it, and the per-cell placement lists never change after load -- so it is
    // rebuilt only when the centre moves. Draining the admission backlog re-walks it, but does
    // not re-derive or re-sort it. Budget changes also invalidate window growth.
    // See UpdateModernObjectResidency.
    struct ModernObjectCellCandidate
    {
        uint32_t index;
        uint32_t distance2;
    };
    std::vector<ModernObjectCellCandidate> _modernObjectCandidates;
    int _modernObjectCandidateCenterX = -0x3fffffff;
    int _modernObjectCandidateCenterZ = -0x3fffffff;
    int _modernObjectCandidateRadius = 0; // window radius the cached list was gathered at
    bool _modernObjectCandidatesValid = false;
    std::vector<RStringB> _modernObjectModels;
    // Native visual-only tier: shares the model preparer and bank with near residency.
    std::vector<uint32_t> _nativeFarCandidates;
    std::vector<uint8_t> _nativeFarWanted;
    std::vector<uint8_t> _nativeFarModelWanted;
    std::vector<uint8_t> _nativeFarModelRejected;
    std::unordered_set<uint32_t> _nativeFarActive;
    std::unordered_map<uint32_t, Ref<LODShapeWithShadow>> _nativeFarModels;
    size_t _nativeFarCursor = 0;
    int _nativeFarCenterX = -0x3fffffff, _nativeFarCenterZ = -0x3fffffff;
    std::vector<uint8_t> _modernObjectCellActive;
    std::vector<uint8_t> _modernObjectCellDesired;
    std::vector<uint32_t> _modernObjectCellCursor;
    bool _modernObjectStreaming = false;
    bool _modernObjectResidencyPending = false;
    int _modernObjectCenterX = -0x3fffffff;
    int _modernObjectCenterZ = -0x3fffffff;
    // Starting window size, not a ceiling: UpdateModernObjectResidency doubles it until the
    // object budget is the bound, so a world smaller than the budget loads whole.
    int _modernObjectRadius = 64;
    uint32_t _modernObjectBudget = 20000;
    // Phase 7: the item budget after the measured-GPU-cost clamp, and the last value
    // logged, so the clamp reports each change once rather than every recentre.
    uint32_t _modernObjectEffectiveBudget = 0;
    uint32_t _modernObjectBudgetLogged = 0;
    uint32_t _modernObjectDesiredCount = 0;
    uint32_t _modernObjectDesiredBudget = 0;
    uint32_t _modernObjectCandidateBudget = 0;
    uint32_t _modernObjectCreatesPerUpdate = 128;
    uint32_t _modernResidentObjectCount = 0;
    // Cadence counter for the residency progress row (see UpdateModernObjectResidency).
    uint32_t _modernObjectUpdateCount = 0;
    // WGR_OBJECT_STREAM_WINDOW_GROWTH=0 pins the window at _modernObjectRadius (diagnostic A/B).
    bool _modernObjectWindowGrowth = true;
    uint32_t _modernObjectTestUpdate = 0;
    int _modernObjectTestPhase = -1;

    // ----------------------------------------------------------------------------------------
    // (a) THE EVICTION CACHE. RND-040 measured created=15958 / released=77643 over 112 s at
    // 30 m/s against 307 / 10895 at 5 m/s: eviction is destructive, so backtracking -- the
    // commonest camera motion there is -- rebuilds from disk what it just threw away.
    //
    // The cache is keyed on the MODEL SHAPE, not on the Object, because that is where the cost
    // is. ShapeBank holds a *weak* Link (Shape.hpp:1098-1104), so when the last Object
    // referencing a model dies the LODShapeWithShadow dies with it and ShapeBank::New
    // (ShapeDraw.cpp:353-362) drops the dead slot and reloads: ODOL decode, ShapeAdapter LOD
    // conversion, OptimizeOneShape vertex-buffer creation. That is the 580 ms-per-128 the
    // admission comment already names. An object reusing a LOADED model costs ~0.03 ms, so
    // holding one strong Ref per recently-vacated model converts the expensive half of a
    // re-admit into the cheap half.
    //
    // Bounded by DISTINCT MODEL COUNT, not by placement count or by bytes: a shape has no cheap
    // byte size (ShapeBank's own probe reports items for exactly this reason, Shape.hpp:1111-
    // 1116) and a world names a few hundred to a few thousand distinct models against millions
    // of placements, so the count is the quantity that actually bounds the memory.
    //
    // The list is LRU-ordered, coldest at the front. The index maps shape -> its list slot so
    // that touch, adopt and trim are all O(1); the Ref in the list is the only ownership this
    // cache ever holds.
    std::list<Ref<LODShapeWithShadow>> _modernObjectShapeCache;
    std::unordered_map<LODShapeWithShadow*, std::list<Ref<LODShapeWithShadow>>::iterator>
        _modernObjectShapeCacheIndex;
    size_t _modernObjectShapeCacheLimit = 0; // 0 = cache off (WGR_OBJECT_STREAM_EVICT_CACHE=0)
    uint64_t _modernObjectShapeCacheHits = 0;
    uint64_t _modernObjectShapeCacheInserts = 0;
    uint64_t _modernObjectShapeCacheDrops = 0;
    bool _modernObjectShapeCacheRegistered = false;
    Streaming::ShapeCachePressurePolicy _modernObjectShapeCachePressure;
    // Declared LAST of the cache members so it is DESTROYED FIRST: the probe holds std::function
    // closures over `this`, and CLDLink's destructor is what unlinks it from the global
    // free-on-demand list. Destroying it before the containers it reads makes a pressure
    // callback against a half-destroyed Landscape impossible.
    Foundation::MemoryDomainProbe _modernObjectShapeCacheProbe;

    // ----------------------------------------------------------------------------------------
    // (b) PREFETCH STATE. The residency window is reactive: it early-outs until the camera has
    // ALREADY crossed a 4-cell boundary, so every admit is on the critical path. These four
    // fields carry a smoothed camera velocity between updates so the window centre can be biased
    // ahead of the camera. Derived here rather than passed in because
    // UpdateModernObjectResidency receives the camera position from World::Draw.
    bool _modernObjectVelocityValid = false;
    float _modernObjectLastCameraX = 0.0f;
    float _modernObjectLastCameraZ = 0.0f;
    Poseidon::Foundation::Time _modernObjectLastCameraTime; // simulation time of the last sample
    float _modernObjectVelocityX = 0.0f; // m/s, exponentially smoothed
    float _modernObjectVelocityZ = 0.0f;
    float _modernObjectLeadMetres = 0.0f; // last applied lead distance, for the progress row
    // The radius, in cells, at which the desired set actually ran out of budget -- NOT the
    // candidate window radius. The candidate window is grown until the budget binds, so on
    // Everon it spans 64 cells while the desired set stops around 18 (~900 m). The prefetch lead
    // must be capped against THIS number: capping against the candidate radius would push the
    // window so far ahead that objects still behind the camera fall out of it.
    uint32_t _modernObjectDesiredRadiusCells = 0;

    // Eviction-cache maintenance. Definitions live beside the residency system in LandSave.cpp.
    void RetainReleasedShape(LODShapeWithShadow* shape);
    bool AdoptCachedShape(LODShapeWithShadow* shape);
    size_t TrimModernObjectShapeCache(size_t keep);

    // ----------------------------------------------------------------------------------------
    // (d) ASYNCHRONOUS COLD-MODEL PREPARATION. The residual stall after (a), (b) and (c) is one
    // cold model admitted synchronously -- file read, P3D decode, IR compile, adapter, vertex
    // buffers, GPU registration, ~90+ ms on Reforger Everon -- and the admit ceiling cannot cut
    // below `ceiling + one object`. The preparer runs the thread-safe front half (read + decode
    // + compile, ModelCache::LoadLooseFile) on worker threads; the admit loop installs a ready
    // IR through ShapeBank::NewFromModel and SKIPS, rather than waits for, an object whose IR
    // is not ready. See ObjectStreamPrepare.hpp for the thread-safety boundary and the levers.
    // Created lazily on the first streamed world; destroyed (joined) with the Landscape.
    std::unique_ptr<ObjectStreamPreparer> _modernObjectPreparer;
    // Exact DayZ tenement experiment. A converted shape stays outside ShapeBank until its
    // initialized primary textures have been touched across owner updates. One slot bounds
    // retained conversion memory; the ordinary path owns every other model.
    struct PendingOwnerPrimaryTextures
    {
        uint32_t modelIndex = 0;
        uint32_t placementIndex = 0;
        uint32_t cellIndex = 0;
        std::unique_ptr<LODShapeWithShadow> shape;
        std::shared_ptr<Model::Model> model;
        std::vector<Texture*> textures; // borrowed from shape sections; shape outlives this list
        size_t next = 0;
        uint64_t reservedBytes = 0;
        uint32_t uploads = 0;
        uint32_t uploadFailures = 0;
        // Optional second phase. Only the parent shape's authored NormalMap stages are
        // touched here; registration and all proxy models remain complete at handoff.
        bool stageParentNormals = false;
        bool stageParentMaterials = false;
        bool parentNormalsDone = true;
        int normalLevel = 0;
        int normalSection = 0;
        std::vector<std::string> materialRoleNames; // one translated section at a time
        size_t materialRoleNext = 0;
        uint32_t normalSections = 0;
        uint32_t normalAttempts = 0;
        uint32_t normalUploads = 0;
        uint64_t normalReservedBytes = 0;
        std::vector<std::string> normalUploadedNames;
        double normalStepMs = 0.0;
        double normalMaxStepMs = 0.0;
        std::chrono::steady_clock::time_point normalStarted;
        std::string normalStopReason;
        bool failed = false;
        double stepMs = 0.0;
        double maxStepMs = 0.0;
        std::chrono::steady_clock::time_point started;
    };
    std::unique_ptr<PendingOwnerPrimaryTextures> _modernPendingOwnerPrimaryTextures;
    struct ModernSourceDiagnosticEntry
    {
        uint32_t index = 0;
        uint64_t inventoryGeneration = 0;
        uint64_t diagnosticParseToken = 0;
        std::array<char, 128> identity{};
        std::chrono::steady_clock::time_point expires{};
        ObjectStreamPreparer::SourceEnvelopeRequest requestResult = ObjectStreamPreparer::SourceEnvelopeRequest::Unavailable;
        bool occupied = false, pending = false;
    };
    // Explicit diagnostic requests only: fixed storage, no IR/shape/object ownership.
    std::array<ModernSourceDiagnosticEntry, 8> _modernSourceDiagnostics{};
    bool _modernSourceDiagnosticsActive = false;
    bool ModernSourceDiagnosticInventoryMatches(const ModernSourceDiagnosticEntry& entry,
        const ObjectStreamPreparer::SourceEnvelopeSnapshot& latest) const;
    // Per model index: the value of _modernObjectPrefetchEpoch when the desired-set walk last
    // considered it, so a recentre requests each distinct cold model once, not once per
    // placement. Sized with _modernObjectModels.
    std::vector<uint32_t> _modernObjectModelPrefetchEpoch;
    uint32_t _modernObjectPrefetchEpoch = 0;
    // Counters for the residency row. `Installed` = objects admitted on a worker-prepared IR;
    // `Waited` = admission attempts skipped because the IR was not ready yet (a placement can
    // count several times across frames); `SyncCold` = cold models admitted synchronously
    // (async off, not a loose file, worker parse failed, or the model was already in flight
    // for a warm-path sibling and the sync path won); the *Ms figures are the split timers.
    uint64_t _modernObjectAsyncInstalled = 0;
    uint64_t _modernObjectAsyncWaited = 0;
    uint64_t _modernObjectSyncCold = 0;
    uint64_t _modernObjectColdModels = 0;
    double _modernObjectColdParseMs = 0.0;    // main-thread parse (sync cold only)
    double _modernObjectColdAdaptMs = 0.0;    // ShapeAdapter, all cold
    double _modernObjectColdOptimizeMs = 0.0; // vertex buffers, all cold
    double _modernObjectColdCreateMs = 0.0;   // ObjectCreate beyond ShapeBank (NewObject/AddObject/GPU)
    // The split of `create` (ObjectAdmitProfile, cumulative over cold models): where inside
    // ObjectCreate the milliseconds went. `Register` = RegisterGpuModel whole; `Section` = its
    // per-section loop (materials + textures), of which the four texture figures; `MeshBuild` /
    // `MeshCreate` / `ModelFfi` its geometry halves and the register FFI; `Proxies` =
    // EmitGpuProxies whole. `TexPrepared`/`TexUploads` count uploads served from the worker
    // store vs all real uploads; `ParkHits` counts registrations skipped whole (WGR_GPU_MODEL_PARK).
    double _modernObjectColdSceneMs = 0.0;
    double _modernObjectColdRegisterMs = 0.0;
    double _modernObjectColdProxiesMs = 0.0;
    double _modernObjectColdMeshBuildMs = 0.0;
    double _modernObjectColdMeshCreateMs = 0.0;
    double _modernObjectColdSectionMs = 0.0;
    double _modernObjectColdModelFfiMs = 0.0;
    double _modernObjectColdTexHeaderMs = 0.0; // TextureBankWgpu::Load misses: header opens via GFileServer
    uint64_t _modernObjectColdTexHeaderLoads = 0;
    double _modernObjectColdTexReadMs = 0.0;
    double _modernObjectColdTexCreateMs = 0.0;
    double _modernObjectColdTexFallbackMs = 0.0;
    double _modernObjectColdAlphaMs = 0.0;
    uint64_t _modernObjectColdTexUploads = 0;
    uint64_t _modernObjectColdTexPrepared = 0;
    uint64_t _modernObjectColdParkHits = 0;
    uint64_t _modernObjectColdParkStale = 0;
    // WHICH BOUND STOPPED THE ADMIT LOOP (roadmap 1.3 "cap GPU uploads by bytes and/or
    // milliseconds per frame"). The loop is governed by FIVE bounds -- a hard batch cap, a
    // per-frame upload byte ceiling, a wall-clock ceiling, an ordinary time budget, and a
    // floor that suppresses the ordinary budget until it is met -- and until these counters
    // not one of them reported ever firing. Tuning any of them was therefore guesswork: the
    // A/B that left the upload ceiling default-OFF had to be run as a whole-capture
    // before/after because there was no way to ask the cheaper question, "did that ceiling
    // stop a single update?".
    //
    // Counted once per residency update, on the FIRST bound that returns true -- that is the
    // one that ends the update, and re-counting the same bound on every subsequent cell would
    // make the tally a measure of how many cells the world has.
    // `_modernObjectAdmitUpdates` is the denominator; updates that drained every candidate
    // without hitting anything are `updates - (sum of the five)`.
    uint64_t _modernObjectAdmitUpdates = 0;

    // WGR_OBJECT_STREAM_PRELOAD. True once the residency window has stopped growing, after
    // which the ordinary per-update caps apply again -- the mode lifts the bound for the
    // FILL, not for play. Latched rather than recomputed so a later camera move into new
    // terrain cannot silently re-enter it and produce a second stall.
    bool _modernObjectPreloadDone = false;
    uint64_t _modernObjectPreloadUpdates = 0;
    uint32_t _modernObjectPreloadIdleUpdates = 0;
    uint64_t _modernObjectAdmitStopCap = 0;      // attempted >= admitCap
    uint64_t _modernObjectAdmitStopUpload = 0;   // frame upload bytes >= WGR_OBJECT_STREAM_UPLOAD_MB
    uint64_t _modernObjectAdmitStopCeiling = 0;  // elapsed >= WGR_OBJECT_STREAM_MAX_MS
    uint64_t _modernObjectAdmitStopBudget = 0;   // elapsed >= the ordinary budget (only above the floor)
    uint64_t _modernObjectAdmitStopCold = 0;     // cold attempts >= WGR_OBJECT_STREAM_COLD_PER_UPDATE (counted mode)
    // Ensure the preparer knows this world's model table; no-op if already current.
    void EnsureModernObjectPreparer();
    // Walk the desired set once per recentre and enqueue every distinct cold model in
    // distance order, so the workers have lead time before the admit loop needs the IR.
    void PrefetchModernObjectModels(const std::vector<ModernObjectCellCandidate>& candidates);
    // Only the small subset of streamed objects that actually emits imported
    // marker lights needs ownership state. Scene keeps non-owning links, so
    // erasing an entry also removes its lights without a map-sized per-placement
    // container.
    std::unordered_map<int, RefArray<LightPoint>> _modernObjectLightOwners;
    // id to data conversion

    RefArray<Object> _arrows;
    // Scene keeps non-owning links to lights. Imported static A3 marker lights
    // therefore need landscape-owned references for their entire loaded-world
    // lifetime, just like vehicle marker lights own their LightPoint instances.
    RefArray<LightPoint> _staticMarkerLights;
    LandCache _segCache;

    // FAR-001 coarse ring state, refreshed once per Landscape::Draw. Radius 0
    // (the default, and what a ground-level frame leaves it at) means disarmed.
    float _coarseRingX{0.0f}, _coarseRingZ{0.0f};
    float _coarseRingRadius{0.0f};
    int _coarseRingLod{0};

    // AutoArray<WaterLevel> _waters; // reflection levels in current scene

    SurfaceInfo _waterSurface;

    bool _nets; // mark if _networks member is valid

  protected:
    void DoConstruct(Engine* engine, World* world);
    void Dim(int x, int z, int rx, int rz, float landGrid);

    // void AddWater( Vector3Par pos );

    // float GetActiveWater( WaterLevel &level );

  public:
    Landscape(Engine* engine, World* world, bool nets = false); // default data
    ~Landscape();

    int GetTerrainRange() const { return _terrainRange; }
    int GetTerrainRangeMask() const { return _terrainRangeMask; }
    int GetTerrainRangeLog() const { return _terrainRangeLog; }

    int GetLandRange() const { return _landRange; }
    int GetLandRangeMask() const { return _landRangeMask; }
    int GetLandRangeLog() const { return _landRangeLog; }
    float GetInvLandRange() const { return _invLandRange; }

    float GetLandGrid() const { return _landGrid; }
    float GetInvLandGrid() const { return _invLandGrid; }

    float GetTerrainGrid() const { return _terrainGrid; }
    float GetInvTerrainGrid() const { return _invTerrainGrid; }

    // The compact authored placement rows for the whole map, and the model names
    // they index. Read-only: a far-field tier draws from the same rows the
    // residency window streams from, and must not be able to disturb them.
    const std::vector<ModernObjectPlacement>& ModernObjectPlacements() const { return _modernObjectPlacements; }
    const std::vector<RStringB>& ModernObjectModels() const { return _modernObjectModels; }

    enum class ModernSourceDiagnosticStatus
    { NotRequested, Pending, LatestSnapshot, Refused, Expired, Interrupted, StaleInventory, Invalid, WrongThread };
    struct ModernSourceDiagnosticSnapshot
    {
        ModernSourceDiagnosticStatus status = ModernSourceDiagnosticStatus::NotRequested;
        ObjectStreamPreparer::SourceEnvelopeRequest requestResult = ObjectStreamPreparer::SourceEnvelopeRequest::Unavailable;
        bool pendingDemand = false;
        ObjectStreamPreparer::SourceEnvelopeSnapshot latestSnapshot;
        Streaming::StaticPlainRouteObservation classNow = Streaming::StaticPlainRouteObservation::Unknown;
    };
    static constexpr size_t ModernSourceDiagnosticLimit = 8;
    static constexpr int ModernSourceDiagnosticTtlSeconds = 30;
    // Harness-authorized optional parsing. Snapshot is the LATEST original source
    // snapshot, never per-request completion, final bounds, coverage or Ready.
    ObjectStreamPreparer::SourceEnvelopeRequest RequestModernSourceDiagnostic(uint32_t index);
    ModernSourceDiagnosticSnapshot SnapshotModernSourceDiagnostic(uint32_t index) const;
    void MaintainModernSourceDiagnostics();
    // Tracks unexpired explicit records, including completed polls; owner
    // maintenance clears this guard after TTL even when simulation is disabled.
    bool HasModernSourceDiagnosticDemand() const { return _modernSourceDiagnosticsActive; }
    void PreserveModernSourceDiagnosticRequests(std::vector<uint32_t>& epochs, uint32_t epoch) const;
    void ResetModernSourceDiagnostics();

    // Streaming observability (dev-panel "Streaming" tab, --capture-metrics): fill the
    // cumulative residency/admission counters that otherwise reach only the log rows.
    // Main thread; cheap (one preparer mutex hop). Defined in LandSave.cpp beside the
    // residency system whose members it reads.
    void SnapshotObjectStreamDiag(Poseidon::Dev::ObjectStreamResidencyCounters& out) const;

  protected:
    void SetLandGrid(float grid);

  public:
    // data management
    void SetTexture(int i, const char* name);

    int LoadData(const char* name, float landGrid);
    int SaveData(const char* name);

    LSError LoadData(QIStream& in, float landGrid);
    void SaveData(QOStream& in) const;

    // load/save current status (no terrain/object data save here)
    LSError Serialize(ParamArchive& ar) override;

    void SerializeBin(SerializeBinStream& f, float landGrid);
    // perform terrain (_data) subdivision
  protected:
    // make Y of all objects relative to terrain level
    void MakeObjectsTerrainRelative();
    // make Y of all objects absolute
    void MakeObjectsTerrainAbsolute();

    // perform one generation of subdivision
    void SubdivideTerrainOneStep();

  public:
    void SubdivideTerrain(int subdivStepLog);
    void ResampleTerrain(int sampleStepLog);

    // Subdivision cache: saves/loads _data array to skip recomputation
    bool LoadSubdivCache(int targetSubdivLog);
    void SaveSubdivCache(int targetSubdivLog);

    bool LoadOptimized(QIStream& f, float landGrid); // wrapper around SerializeBin - true if OK
    // Loads modern OPRW worlds while preserving their authored coarse land grid
    // and fine elevation grid. See LandSave.cpp for the ownership split.
    bool LoadOprwModern(const char* path);
    // One parsed terrain RVMAT into _a3TerrainMaterials[index] (+ the legacy cell texture); see LandSave.cpp.
    bool ApplyTerrainMaterialSource(int index, const Poseidon::Asset::Material::RvMaterialSource& material);
    // Sinkhole W5: raw 4WVR worlds whose cell texture names end in ".rvmat" (stock worlds have none).
    void LoadRvmatTerrainMaterials(const std::vector<std::pair<int, std::string>>& cells);
    /// RFG-005: load an Enfusion (Arma Reforger) world straight out of the installed
    /// game's `.pak` archives -- no conversion, no intermediate file. `addonsDir` is
    /// the game's `addons` directory; `POSEIDON_REFORGER_WORLD` picks the world
    /// (default `worlds/eden`, which is Everon). Terrain only so far; see
    /// LandLoadEnfusion.cpp for what is deliberately absent.
    bool LoadEnfusionNative(const char* addonsDir, const char* worldDirectory = nullptr, bool localPreview = false);
    // No-op for legacy worlds. Called by the WGPU terrain path once the live
    // camera exists; reevaluates for center or budget changes, drains pending work.
    void UpdateModernObjectResidency(Vector3Par cameraPosition);
    // Experimental, default OFF. No load/pin/geometry work on a disabled call.
    bool ModernSimulationResidencyEnabled() const;
    Streaming::SimulationResidencyStatus QueueModernSimulationRegion(
        Object& actor, Vector3Par from, Vector3Par to, double radius, bool automatic = false);
    void ReleaseModernSimulationRegion(Object& actor);
    Streaming::SimulationResidencyStatus ModernSimulationRegionStatus(const Object& actor) const;
    // Exact query channel, independent of automatic neighborhoods. Ready is
    // geometry residency only; the origin-grid >25m collision limitation remains.
    Streaming::SimulationResidencyStatus QueueModernSimulationQuery(Object& actor,
        Streaming::SimulationQueryPurpose purpose, Vector3Par from, Vector3Par to, double radius);
    Streaming::SimulationResidencyStatus ModernSimulationQueryStatus(const Object& actor,
        Streaming::SimulationQueryPurpose purpose, Vector3Par from, Vector3Par to, double radius) const;
    void ReleaseModernSimulationQuery(Object& actor, Streaming::SimulationQueryPurpose purpose);
    // Experimental owner-only Fire-channel query. NonReady leaves retVal
    // unchanged. Ready appends legacy contacts plus complete leased static
    // terrain contacts independent of the legacy 25m origin-grid margin.
    // Radius certifies conservative residency padding; Object::Intersect keeps
    // its existing segment/contact semantics (this is NOT a thick-ray API).
    // A measured 2ms soft work guard can return Pending without any contacts;
    // one legacy scan/Intersect and final append are not preemptible. No default
    // gameplay consumer is migrated, nor is arbitrary dynamic coverage proven.
    // Degenerate (<=1mm) segments/unsafe float-to-grid conversions are Invalid;
    // legacy AABB scans beyond4096 cells refuse CapacityExceeded before work.
    Streaming::SimulationResidencyStatus TryModernSimulationFireCollision(
        CollisionBuffer& retVal, const Object& actor, Object* with, Object* ignore,
        Vector3Par from, Vector3Par to, float radius) const;
    Streaming::SimulationResidencyStats SnapshotModernSimulationResidency() const;
    // Only World can mint the owner boundary, after its real background-AI join.
    void PumpModernSimulationResidency(const Streaming::SimulationResidencyOwnerBoundary& boundary);

    // Harness-only budget injection; callers must opt in explicitly. No shipping UI policy.
    bool SetObjectStreamTestBudget(uint32_t budget);
    uint32_t ObjectStreamDesiredCount() const { return _modernObjectDesiredCount; }
    bool ObjectStreamPending() const { return _modernObjectResidencyPending; }
    int ObjectStreamCenterX() const { return _modernObjectCenterX; }
    int ObjectStreamCenterZ() const { return _modernObjectCenterZ; }
    void UpdateNativeFarResidency(Vector3Par cameraPosition);
    void ClearNativeFarResidency();
    void SaveOptimized(QOStream& f);
    void SaveOptimized(const char* name);

    // reset geography information - next call to InitGeography will create it
    void ResetGeography();

    // init geography information
    void InitGeography();
    // RFG-101: streamed worlds have no objects in their cells when InitGeography runs, so
    // every cell reads "no hard objects" and clutter grows through church floors. Objects the
    // stream creates afterwards are folded into the geography here with InitGeography's own
    // per-object rule and thresholds; the cell only ever gets denser (evictions do not undo
    // it -- a house that streamed out is still a house).
    void EnableGeographyAccumulation();
    void AccumulateGeographyForObject(Object* obj);
    bool _geoAccumEnabled = false;
    // Bumped whenever accumulation changed a cell; the terrain renderer re-uploads its
    // geography texture when it sees a new value (throttled there).
    uint32_t _geographyRevision = 0;
    uint32_t GeographyRevision() const { return _geographyRevision; }
    std::vector<float> _geoAccumTotalArea, _geoAccumHardArea;
    std::vector<uint16_t> _geoAccumTotalCount, _geoAccumHardCount;
    void InitMountains();
    void InitSoundMap();
    void InitDynSounds(const ParamEntry& entry);
    void InitRandomization();
    const char* GetName() const { return _name; }

    // use 0 to represent -1 (no sound)...0xff for 0xfe
    void SetSound(int x, int z, int index);
    int GetSound(int x, int z) const;

    void MakeShadows(Scene& scene);
    bool VerifyStructure() const;

    // buldozer interface
    AutoArray<int> GetObjectIDList() const;
    AutoArray<int> GetTextureIDList() const;

    void ShowArrow(Vector3Par pos);
    void ShowObject(Object* obj);

    void SetSelection(const AutoArray<int>& sel);
    void SetSelRectangle(Vector3Par min, Vector3Par max);

    bool Magnetize(bool points, bool planes, bool lockY, const AutoArray<int>& sel);

    // id to data conversion
    Object* FindObjectNC(int id) const; // do not use cache
    Object* FindObject(int id) const;   // cache may be available
    Object* GetObject(int id) const;
    // Owner-only, instance-only observation of EXISTING constructed objects.
    // No resolution, required pin, shape load or persistent group certification.
    Streaming::InstanceCoverageObservation ObserveConstructedObjectCoverage(
        int id, const Streaming::OwnerCoverageObservationScope& scope) const;
    Streaming::InstanceCoverageObservation ValidateConstructedObjectCoverage(
        const Streaming::InstanceCoverageObservation& observation,
        const Streaming::OwnerCoverageObservationScope& scope) const;
    // Owner-thread, potentially synchronous: resolve an authored placement and retain
    // its logical identity. FindObject/GetObject remain read-only residency probes.
    Object* ResolveObject(int id);

    // object ID management
    void ClearIDCache();
    void RebuildIDCache();
    void AddToIDCache(Object* object);
    int NewObjectID() { return ++_objectId; }
    void SetLastObjectID(int id) { _objectId = id; }
    int GetLastObjectID() const { return _objectId; }
    void ResetObjectIDs(); // reset object ids - use with caution
    void ResetState();     // repair all objects, check there are no non-primaries
    void OnTimeSkipped();  // time skipped, react accordingly

    Texture* GetTexture(int id) const;
    int GetNTextures() const { return _texture.Size(); }
    bool TextureIsSimple(int txt) const { return _texture[txt].offsetUV; }
    // The four CfgSurfaces entries (TL, TR, BL, BR) this terrain texture resolves
    // to, already decoded at load. A pre-blended OFP tile such as `tntntvtn` names
    // a different surface per quadrant; a plain texture repeats one across all
    // four. Entries may be null if the texture matched nothing.
    const SurfaceInfo* const* GetTextureQuadrants(int id) const
    {
        return id >= 0 && id < _texture.Size() ? _texture[id].quadrants : nullptr;
    }
    const A3TerrainMaterial* GetA3TerrainMaterial(int id) const
    {
        return id >= 0 && id < _a3TerrainMaterials.Size() ? &_a3TerrainMaterials[id] : nullptr;
    }
    bool HasRvmatCellMaterials() const { return _rvmatCellMaterials; }
    // The Enfusion surface `.emat` this palette entry was bound from, or "" -- and 0 on
    // every world that is not a natively loaded Reforger one. See _enfusionSurfaces.
    int GetEnfusionSurfaceCount() const { return _enfusionSurfaces.Size(); }
    const char* GetEnfusionSurface(int id) const
    {
        return id >= 0 && id < _enfusionSurfaces.Size() ? (const char*)_enfusionSurfaces[id] : "";
    }
    // True when an Enfusion road ribbon crosses land cell (z, x); always false on a
    // world the native Reforger loader did not build. See _enfusionRoadCells.
    bool HasEnfusionRoadCells() const { return !_enfusionRoadCells.empty(); }
    bool GetEnfusionRoadCell(int z, int x) const
    {
        if (z < 0 || x < 0 || z >= _landRange || x >= _landRange)
            return false;
        const size_t cell = static_cast<size_t>(z) * static_cast<size_t>(_landRange) + static_cast<size_t>(x);
        return cell < _enfusionRoadCells.size() && _enfusionRoadCells[cell] != 0;
    }
    static bool EnfusionRoadClutterClear() { return s_enfusionRoadClutterClear; }
    static void SetEnfusionRoadClutterClear(bool clear) { s_enfusionRoadClutterClear = clear; }

    // data access
    GeographyInfo GetGeography(int x, int z) const;
    float GetHeight(int z, int x) const; // note: x,z order differs from ClippedData
    // Row-major [z][x] float heights; valid until terrain data is rebuilt.
    const float* HeightmapData() const { return static_cast<const RawType*>(_data.RawData()); }
    int GetTexture(int z, int x) const;
    const ObjectList& GetObjects(int z, int x) const { return _objects(x, z); }
    PackedColor GetRandomColor(int x, int z, float& u, float& v) const
    {
        if (!this_InRange(x, z))
        {
            u = v = 0;
            return PackedWhite;
        }
        const RandomInfo& info = _random(x, z);
        u = info.uOff * 0.1;
        v = info.vOff * 0.1;
        return _colorizePalette[info.color];
    }
    const AutoArray<Vector3>& GetMountains() const { return _mountains; }

    void ReleaseAllVBuffers();
    void CreateAllVBuffers();
    void CreateNearVBuffers(float cx, float cz, float radius);

    void FlushCache();
    void FillCache(const Frame& pos);

    void RegisterTexture(int id, const char* name); // load a new texture
    void RegisterObjectType(const char* name);      // add a shape into the bank

    void Init(); // empty landscape
    void Quit(); // before quit

    void HeightChange(int x, int z, float y);
    uint64_t HeightRevision() const { return _heightRevision; }

  private:
    uint64_t _heightRevision = 0;
    uint64_t _subdivSourceHash = 0; // hash of the heights a subdivision cache was / will be built from

  public:
    void TextureChange(int x, int z, int id);

    Object* ObjectCreate(int id, const char* shape, const Matrix4& transform, int* x = nullptr, int* z = nullptr,
                         bool avoidRecalculation = false, bool preserveAuthoredElevation = false,
                         bool registerVisual = true);
  private:
    Object* ObjectCreateFromShape(int id, LODShapeWithShadow* shape, const char* name,
                                 const Matrix4& transform, int* x, int* z, bool avoidRecalculation,
                                 bool preserveAuthoredElevation, bool registerVisual);
    // Joined owner only. Caller holds its checked fresh source-bound shape and
    // repeats the live config probe in the same operation. This seam does not
    // look up a name, certify coverage, or enable visual registration.
    Object* ObjectCreateModernPlainFromShape(int id, LODShapeWithShadow* shape, const Matrix4& transform);
  public:
    void ObjectDestroy(int id);
    void ObjectMove(int id, const Matrix4& transform);
    void ObjectTypeChange(int id, const char* shape);

  protected:
    void TerrainChanged(float x, float z, float maxRange);

  public:
    bool ClippedIsWater(int z, int x) const;
    float ClippedData(int z, int x) const;
    Texture* ClippedTexture(int z, int x) const;
    int ClippedTextureIndex(int z, int x) const;

    int ClampFlags(int txt) const { return (_texture[txt].offsetUV ? NoClamp : ClampU | ClampV); }

    IOperCache* OperationalCache() const { return _operCache; }
    ILockCache* LockingCache() const { return _lockCache; }

    static void CalculBoundingRect(LandBegEnd& res, const Camera& camera, float dist, float grid);

    void DrawMesh(Scene& scene, TLVertexTable& table, const Shape& vMesh, Vector3Par offset, const LandBegEnd& rect,
                  bool isWater);

    Ref<LandSegment> GenerateSegment(const LandBegEnd& rect, bool deferGPU = false, int lodLevel = 0);
    void GenerateSegmentInto(LandSegment* seg, const LandBegEnd& rect, bool deferGPU = false, int lodLevel = 0);

    /// FAR-001, the terrain half. Arm (or disarm) the COARSE RING: terrain
    /// further than `radius` from (camX, camZ) is generated at `lod` instead of
    /// LOD 0.
    ///
    /// Why a ring is needed at all. Opening the far plane widens the terrain
    /// rectangle that `Landscape::Draw` builds, and at a 15 km reach on Everon's
    /// 50 m land grid that rectangle is roughly 600 x 600 cells -- about 5,600
    /// segments where ordinary play draws a few dozen. At LOD 0 with Everon's
    /// subdivision that is 1,089 vertices per segment, so the segment cache alone
    /// would want a couple of hundred megabytes and the draw would be hopeless.
    /// At LOD 2 it is 81 vertices per segment, a factor of thirteen, and the far
    /// ring becomes affordable. Measured numbers are in the FAR-001 write-up.
    ///
    /// The LOD machinery for this already existed -- `GenerateSegmentInto` has
    /// implemented strides for LOD 1 and 2 all along -- but every call site
    /// passed 0, under the comment "All segments generated at LOD 0 to prevent
    /// T-junction gaps". That reason is sound where it was written and does not
    /// apply here: the seam this creates is a single ring at `radius`, which is
    /// the ordinary view distance, i.e. exactly the distance at which the haze
    /// has already gone opaque. Inside the ring nothing changes, so ground play
    /// cannot grow a crack it did not have.
    ///
    /// `radius <= 0` or `lod <= 0` disarms it and every segment is LOD 0 again,
    /// which is the pre-FAR-001 behaviour and what the dev-panel switch restores.
    void SetCoarseRing(float camX, float camZ, float radius, int lod);

    /// The LOD a segment whose near corner is at (xBeg, zBeg) in land-grid cells
    /// must be generated at. 0 unless the coarse ring is armed and the segment
    /// lies entirely outside it.
    int RequiredSegmentLod(int xBeg, int zBeg) const;
    void FinalizeSegmentGPU(LandSegment* seg);

    void DrawRect(Scene& scene, const LandBegEnd& rect);
    void DrawSky(Scene& scene);
    void DrawHorizont(Scene& scene);
    void DrawClouds(Scene& scene);
    void Draw(Scene& scene);

    void DrawWater(const LandBegEnd& bigRect, Scene& scene);
    void DrawGround(const LandBegEnd& bigRect, Scene& scene, const GroundLayerInfo& layer);

    void SurfacePlane(Plane& plane, float x, float z) const;

    // simple inteface
    float SurfaceY(float x, float z) const;
    float SurfaceYAboveWater(float x, float z) const;

    // complex interface
    float SurfaceY(float x, float z, float* rdX, float* rdZ, Texture** texture = nullptr) const;
    // find topmost surface on given x,z coordinates
    float RoadSurfaceY(float xC, float zC, float* dX = nullptr, float* dZ = nullptr, Texture** texture = nullptr) const;
    // find nearest surface under given point
    float RoadSurfaceY(Vector3Par pos, float* dX = nullptr, float* dZ = nullptr, Texture** texture = nullptr,
                       Object** obj = nullptr) const;
    float SurfaceYAboveWater(float x, float z, float* rdX, float* rdZ, Texture** texture = nullptr) const;
    float RoadSurfaceYAboveWater(float xC, float zC, float* dX = nullptr, float* dZ = nullptr,
                                 Texture** texture = nullptr) const;
    float RoadSurfaceYAboveWater(Vector3Par pos, float* dX = nullptr, float* dZ = nullptr,
                                 Texture** texture = nullptr) const;

    float WaterDepth(float x, float z) const;

    float CalculateBump(float xC, float zC, Texture* texture, float bumpy) const;
    float BumpySurfaceY(float x, float z, float& rdX, float& rdZ, Texture*& texture, float bumpy, float& bump) const;
    float UnderRoadSurface(const Object* obj, Vector3Par pos, float bumpy, float* dX = nullptr, float* dZ = nullptr,
                           Texture** texture = nullptr) const;

	// Sinkhole W1, ported from Malprave terrainHole1 (TerrainHoles.cpp): an object whose memory LOD has a "terrain_hole" (or "terrain_hole1" ..
	// "terrain_hole8") selection cuts the ground away inside the convex XZ footprint of those points, if it also has a
	// roadway LOD. Inside a hole the terrain gives no ground and the object's roadway (floor, stairs) is used instead.
	struct TerrainHoleArea
	{
		int n = 0;
		float x[16], z[16]; // world XZ, convex, counter-clockwise
		float minX = 0, maxX = 0, minZ = 0, maxZ = 0;
		float floorY = 0;   // terrainHole3: world height of the lowest point of the object's roadway
		float ceilingY = 1e20f; // optional terrain_hole_ceiling: retain terrain above a horizontal cave
		bool draw = true;   // terrainHole8: false = hidden hole (terrain_hole_hidden*), the terrain is still drawn there
	};
	// holes of the objects near pos (within radius), at most maxOut; returns the count
	int GatherTerrainHoles(Vector3Par pos, float radius, TerrainHoleArea *out, int maxOut) const;
	static bool InTerrainHoles(const TerrainHoleArea *holes, int n, float x, float z);
	static int TerrainHoleIndex(const TerrainHoleArea *holes, int n, float x, float z, float y = -1e20f); // terrainHole3, -1 = none
	// terrainHole3: floorY (optional) = the hole's floor, the lowest point of the cutting object's roadway
	bool InTerrainHole(float x, float z, float *floorY = nullptr, float y = -1e20f) const;
	// Terrain draw records: edges {nx,nz,d,0/1}; optional {0,0,ceilingY,2} before a bounded polygon.
	// Returns total records, including metadata; legacy unbounded polygons are unchanged.
	int TerrainHoleDrawEdges(Vector3Par camPos, float radius, float *edges, int maxEdges) const;
	// terrainHole4: the lowest height a camera at pos may have: the terrain (aboveWater: or the sea), or inside a hole
	// the hole's floor. A camera under the terrain outside every hole whose focus (optional) is under the terrain inside a
	// hole (a 3rd person view swung into the earth around a cellar) is first moved back along the line to the focus.
	float CameraFloorY(Vector3 &pos, const Vector3 *focus, bool aboveWater) const;
	// terrainHole5: p is inside a terrain hole and more than depth below the terrain there (down in a cellar / stairwell)
	bool InTerrainHoleBelow(Vector3Par p, float depth) const;
	// terrainHole5: moving from -> to leaves a terrain hole sideways under the ground (into the earth)
	bool LeavesTerrainHoleUnderground(Vector3Par from, Vector3Par to) const;
	// terrainHole5: obj's model cuts a terrain hole
	static bool CutsTerrainHole(const Object *obj);
	// terrainHole6: the height a ground decal (footprint, track, crater, shadow) at x, z is fitted to. refY = the decal's
	// own height: below the terrain inside a hole it lies on the hole's roadway under refY, otherwise on the terrain
	float DecalSurfaceY(float x, float z, float refY) const;
	// terrainHole6: the ground under p for tracks: the topmost roadway as before, or down in a hole the roadway under p
	float TrackSurfaceY(Vector3Par p) const;
	// terrainHole7: p is inside the room a hole-cutting object makes: within a hole outline, between the hole's floor
	// (less 0.5 m) and the top of the object's bounding box (the cellar roof) - also where a roof stands above a slope
	bool InTerrainHoleRoom(Vector3Par p) const;

    void SetSkyTexture(Texture* texture);

    Point3 PointOnSurface(float x, float y, float z) const;

    float AboveSurface(Vector3Val pos) const;
    float AboveSurfaceOrWater(Vector3Val pos) const;

    void InitObjectVehicles();

    Object* AddObject(float x, float y, float z, float head, const char* name);
    Object* AddObject(Vector3Par pos, float head, LODShapeWithShadow* shape, void* user = nullptr);

  private:
    inline void SelectObjectList(int& xl, int& zl, float x, float z);

  public:
    void AddObject(Object* obj, int* xr = nullptr, int* zr = nullptr, bool avoidRecalculation = false,
                   bool registerVisual = true);
    void Recalculate();
    void RemoveObject(Object* obj);
    void MoveObject(Object* obj, const Matrix4& transform);
    void MoveObject(Object* obj, Vector3Par pos);

    // dynamic object list loading / unloading
    void LoadObjects(int x, int z);
    void ReleaseObjects(int x, int z);

    // control weather
    float GetRainDensity() const { return _weather._rainDensity; }
    float GetOvercast() const { return _weather.GetOvercast(); }
    // Forwarded so a caller holding a Landscape can read fog back the same way it
    // reads overcast. Weather::GetFog() already existed; only the forwarder was
    // missing, which is why DebugCheats claimed there was "no public getter".
    float GetFog() const { return _weather.GetFog(); }
    void SetOvercast(float overcast) { _weather.SetOvercast(this, overcast); }
    void SetFog(float fog) { _weather.SetFog(this, fog); }
    void SetRain(float density, float time) { _weather.SetRain(density, time); }
    void Simulate(float deltaT);
    float GetSeaLevel() const { return _seaLevelWave; }
    void SetSeaWaveSpeed(float seaWaveSpeed) { _seaWaveSpeed = seaWaveSpeed; }

    Vector3 GetWind() const;

    const SurfaceInfo& GetWaterSurface() const { return _waterSurface; }
    const SurfaceInfo& SurfaceAt(float x, float z) const;

    // query weather
    Texture* SkyTexture();
    float CloudsPosition() const { return _weather._cloudsPos; }
    float SkyThrough() const { return _weather._skyThrough; }
    float CloudsAlpha() const { return _weather._cloudsAlpha; }
    float CloudsBrightness() const { return _weather._cloudsBrightness; }

    Object* NearestObject(Vector3Par pos, float limit = 0, ObjectType type = Any,
                          Object* ignore = nullptr); // default - no limit

    bool CheckVisibility(int x, int z, VisCheckContext& context, ObjIntersect isect) const;

    float VisibleStrategic(int xs, int zs, int xe, int ze) const;
    float VisibleStrategic(Vector3Par from, Vector3Par to) const;
    float Visible(Vector3Par from, Vector3Par to, float toRadius, const Object* skip1, const Object* target,
                  ObjIntersect isect = ObjIntersectView) const; // point visibility - used for flares etc.
    float Visible(const Object* sensor, const Object* object, float reserve = 1,
                  ObjIntersect isect = ObjIntersectView) const;
    float Visible(Vector3Par sensorPos, const Object* sensor, const Object* object, float reserve = 1,
                  ObjIntersect isect = ObjIntersectView) const;

    // check if point is inside some object,
    void IsInside(StaticArrayAuto<OLink<Object>>& objects, Object* ignore, Vector3Par pos,
                  ObjIntersect isect = ObjIntersectGeom);
    float CheckUnderLand(Vector3Par beg, Vector3Par dir, float tMin, float tMax, int x, int z) const;

    bool CheckIntersection(Vector3Par beg, Vector3Par end, int x, int z, float& tRet) const;

    // old calling convention - do not use
    Vector3 IntersectWithGround(Vector3Par from, Vector3Par dir, float minDist = 0,
                                float maxDist = 1e5 // virtually no limit
    ) const;
    // old calling convention - do not use
    Vector3 IntersectWithGroundOrSea(Vector3Par from, Vector3Par dir, float minDist = 0,
                                     float maxDist = 1e5 // virtually no limit
    ) const;

    // return time from minDist to maxDist (when intersection is found)
    float IntersectWithGround(Vector3* ret, Vector3Par from, Vector3Par dir, float minDist = 0,
                              float maxDist = 1e5 // virtually no limit
    ) const;
    // return time from minDist to maxDist (when intersection is found)
    float IntersectWithGroundOrSea(Vector3* ret, Vector3Par from, Vector3Par dir, float minDist = 0,
                                   float maxDist = 1e5 // virtually no limit
    ) const;
    float IntersectWithGroundOrSea(Vector3* ret, bool& sea, Vector3Par from, Vector3Par dir, float minDist = 0,
                                   float maxDist = 1e5 // virtually no limit
    ) const;

    Object* PreviewFire(const Object* ignore, Vector3Par from, Vector3Par speed, Vector3 accel,
                        float timeToLive) const; // return what will be hit if we will fire this way

    void CheckObjectCollision(int x, int z, CollisionBuffer& retVal, CheckObjectCollisionContext& context) const;

    // collision check
    void ObjectCollision(CollisionBuffer& retVal, Object* with, Object* ignore, Vector3Par beg, Vector3Par end,
                         float radius, ObjIntersect type = ObjIntersectFire) const;
    void ObjectCollision(CollisionBuffer& retVal, Object* with, const Frame& withPos, bool onlyVehicles = false) const;
    void PredictCollision(VehicleCollisionBuffer& ret, const Vehicle* vehicle, float maxTime, float gap,
                          float maxDistance) const;
    void GroundCollision(GroundCollisionBuffer& retVal, Object* with, const Frame& withPos, float above, float bumpy,
                         bool enableLandcontact = true, bool soldier = false) const;
    void GroundCollisionPlane( // faster (less acuurate) version
        GroundCollisionBuffer& retVal, Object* with, const Frame& withPos, float above, float bumpy,
        bool enableLandcontact = true);

    // effects of explosion only - no actual dammage
    void ExplosionDammageEffects(EntityAI* owner, Shot* shot, Object* directHit, Vector3Par pos, Vector3Par dir,
                                 const AmmoType* type, bool enemyDammage);
    // explosion does dammage
    void ExplosionDammage(EntityAI* owner, Shot* shot, Object* directHit, Vector3Par pos, Vector3Par dir,
                          const AmmoType* type);
    void Disclose(EntityAI* owner, Vector3Par pos, float maxDist, bool discloseSide, bool disclosePosition);

    bool CheckObjectStructure() const;

  protected:
    void ReplaceObjects(RString name);
};

const float YOutsideMap = -100; // sea

inline float Landscape::ClippedData(int z, int x) const
{
    if (this_TerrainInRange(z, x))
        return GetData(x, z);
    else
        return YOutsideMap;
}

inline void Landscape::SelectObjectList(int& xl, int& zl, float x, float z)
{
    int xx = toIntFloor(x * _invLandGrid);
    int zz = toIntFloor(z * _invLandGrid);
    if (!this_ObjInRange(xx, zz))
    {
        // find nearest in-range square and use it
        if (xx < 0)
            xx = 0;
        else if (xx > _landRangeMask)
            xx = _landRangeMask;
        if (zz < 0)
            zz = 0;
        else if (zz > _landRangeMask)
            zz = _landRangeMask;
        PoseidonAssert(this_ObjInRange(xx, zz));
    }
    xl = xx;
    zl = zz;
}

} // namespace Poseidon
namespace Poseidon
{
extern Landscape* GLandscape; // global single storage (see Landscape.cpp)
#define GLOB_LAND (GLandscape)

void ClearShapes(); // flush all cached shapes, types ...

Object* NewObject(LODShapeWithShadow* shape, int id);
} // namespace Poseidon
