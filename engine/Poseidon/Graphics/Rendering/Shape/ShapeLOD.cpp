
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <utility>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Containers/BoolArray.hpp>
#include <Poseidon/Foundation/Framework/AppFrame.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Memory/FastAlloc.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <Poseidon/Foundation/platform.hpp>

// Defined at global scope in World/Object code (unwrapped subsystems).
namespace Poseidon
{
class Object;
}
namespace Poseidon
{
Object* NewObject(Poseidon::Foundation::RString typeName, Poseidon::Foundation::RString shapeName);
}
Poseidon::Object* NewProxyObject(Poseidon::Foundation::RString shapeName);

namespace Poseidon
{

#if defined(_M_X64) || defined(_M_AMD64)
} // namespace Poseidon
#include <intrin.h>
namespace Poseidon
{
#elif defined(__x86_64__)
} // namespace Poseidon
#include <x86intrin.h>
namespace Poseidon
{
#endif
} // namespace Poseidon
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/World/Simulation/Animation/Animation.hpp>
#include <Poseidon/Graphics/Core/TLVertex.hpp>
#include <Poseidon/IO/FileServer.hpp>
#include <Poseidon/Foundation/Strings/StrFormat.hpp>

#include <Poseidon/IO/Streams/SerializeBin.hpp>

#include <Poseidon/Graphics/Rendering/Primitives/Edges.hpp>
#include <Poseidon/Graphics/Rendering/Draw/SpecLods.hpp>

#include <Poseidon/Foundation/Common/Filenames.hpp>

#include <Poseidon/Core/Data3D.h>

#include <Poseidon/World/MapTypes.hpp>

#include <Poseidon/Graphics/Rendering/Shape/ShapeShared.hpp>
namespace Poseidon
{

// The names are the ones config and model.cfg use, so they stay exactly as they
// were; only the dispatch moved onto the IR's table (AST-018).
const char* LevelName(float resolution)
{
    static char buf[256];
    switch (Model::ClassifyLodResolution(resolution))
    {
        case Model::LodPurpose::Geometry:              return "geometry";
        case Model::LodPurpose::Memory:                return "memory";
        case Model::LodPurpose::LandContact:           return "landContact";
        case Model::LodPurpose::Roadway:               return "roadway";
        case Model::LodPurpose::Paths:                 return "paths";
        case Model::LodPurpose::HitPoints:             return "hitpoints";
        case Model::LodPurpose::ViewGeometry:          return "geometryView";
        case Model::LodPurpose::FireGeometry:          return "geometryFire";
        case Model::LodPurpose::ViewPilotGeometry:     return "geometryViewPilot";
        case Model::LodPurpose::ViewGunnerGeometry:    return "geometryViewGunner";
        case Model::LodPurpose::ViewCommanderGeometry: return "geometryViewCommander";
        case Model::LodPurpose::ViewCargoGeometry:     return "geometryViewCargo";
        default:
            // Visual LODs, the crewed view distances and any sentinel this build
            // cannot name all print their resolution, as before.
            break;
    }
    snprintf(buf, sizeof(buf), "%g", resolution);
    return buf;
}

static const char* LevelName(LODShape* shape, Shape* level)
{
    for (int i = 0; i < shape->NLevels(); i++)
    {
        if (shape->Level(i) == level)
        {
            return LevelName(shape->Resolution(i));
        }
    }
    return "Error";
}

// See Shape.hpp for why this is a free function rather than part of LODShape::Load.
//
// WLD/VEG-SWAY: it exists because the two loaders disagreed. The legacy MLOD / ODOL-7 path
// resolved the `map` string here; ShapeAdapter (every imported generation: ODOL 40/48/49/50/52/54
// and 73) instead copied `Model::mapType`, a field only the ODOL-7 prologue reader ever writes
// (P3DStructures.hpp readModel). convertOdolStatic never assigns it, so it stayed at its default
// 0 -- and MapType's zeroth enumerator is MapTree. Every Arma 1/2/3, DayZ and Reforger model
// therefore loaded as a TREE, which is the gate for wind sway (Object.cpp GCurrentFoliageKind ->
// WgrDraw3D::mat_sun_ambient.w -> shader3d.wgsl veg_sway_offset; EngineWgpu WGR_INSTANCE_CANOPY_TREE
// -> gpu_driven.wgsl) and for canopy leaf normals. A masonry wall bending in the wind is exactly
// what that produces: the sway amplitude scales with the vertex's model-space height, so a wall's
// base stays planted and its top swings.
static MapType ResolveMapTypeTable(const char* value, const char* modelName, bool* recognised)
{
    const auto accept = [&](MapType type)
    {
        if (recognised)
        {
            *recognised = true;
        }
        return type;
    };
    if (recognised)
    {
        *recognised = false;
    }
    if (!value)
    {
        value = "";
    }
    if (!*value)
    {
        // VEG-SWAY / ARF -- Enfusion (Arma Reforger) vegetation converted from .xob carries
        // NO named properties at all, so `map` is absent and every spruce, hazel and willow
        // lands on MapHide. MEASURED, not assumed: of the 94 vegetation .p3d deployed from
        // Reforger's Everon, 90 contain no `map` token anywhere in the file.
        //
        // MapHide is not merely a map-symbol choice. The renderer keys its whole vegetation
        // treatment off MapType -- canopy/spherical leaf normals (EngineWgpu.cpp
        // BuildGpuInstance, INST_CANOPY_*), the per-draw foliage kind (Object.cpp
        // GCurrentFoliageKind) and now wind sway. On MapHide an imported tree is shaded and
        // animated as though it were masonry.
        //
        // The fallback is deliberately narrow so it cannot reach OFP/Arma content, which
        // always authors `map` explicitly: it requires BOTH an Enfusion "vegetation"
        // directory segment in the path AND that generation's `t_` / `b_` basename prefix
        // (t_picea_abies_3s, b_corylus_avellana_1). A rock or a fence cannot match. MEASURED
        // across the four local corpora: of 10,908 Arma 1 / Arma 2 / Arma 3 / DayZ models, ZERO
        // carry a "vegetation" path segment, so this cannot fire on any of them.
        // Precedent for name-keyed classification is already in-tree: IsGrassSurfaceName
        // (TerrainWgpu.cpp) picks grass surfaces out of authored terrain material names.
        if (const char* path = modelName; path && *path)
        {
            // Local, because there is no shared case-insensitive substring helper reachable
            // from this translation unit (the two in-tree ContainsNoCase are private statics
            // in DisplayUI and Landscape).
            auto containsNoCase = [](const char* hay, const char* needle) -> bool
            {
                const size_t n = std::strlen(needle);
                if (n == 0)
                    return true;
                for (const char* p = hay; *p; ++p)
                {
                    size_t i = 0;
                    while (i < n && p[i] &&
                           std::tolower(static_cast<unsigned char>(p[i])) ==
                               std::tolower(static_cast<unsigned char>(needle[i])))
                    {
                        ++i;
                    }
                    if (i == n)
                        return true;
                }
                return false;
            };
            if (containsNoCase(path, "vegetation"))
            {
                char base[256];
                GetFilename(base, path);
                const bool isTreeName = (base[0] == 't' || base[0] == 'T') && base[1] == '_';
                const bool isBushName = (base[0] == 'b' || base[0] == 'B') && base[1] == '_';
                // A stump is not a canopy: it has no crown to bend and no leaves to flutter,
                // and swaying one would read as the ground moving.
                const bool isStump = containsNoCase(base, "stump") || containsNoCase(base, "fallen");
                if (isTreeName && !isStump)
                {
                    return accept(MapTree);
                }
                if (isBushName && !isStump)
                {
                    return accept(MapBush);
                }
            }
        }
        return MapHide;
    }
    if (stricmp(value, "TREE") == 0)
        return accept(MapTree);
    // Both spellings of small tree. MEASURED across the four local corpora: the spaceless
    // `smalltree` is authored by Arma 2 Operation Arrowhead (plants_e.pbo: t_AmygdalusC2s_EP1,
    // t_PopulusF2s_EP1, t_PrunusS2s_EP1) and by one Arma 3 model; Arma 2 base, Arma 1 and DayZ
    // author only "SMALL TREE". Without the second spelling those trees fell through to the
    // unknown case -- MapHide -- and were classified as masonry.
    if (stricmp(value, "SMALL TREE") == 0 || stricmp(value, "SMALLTREE") == 0)
        return accept(MapSmallTree);
    if (stricmp(value, "BUSH") == 0)
        return accept(MapBush);
    if (stricmp(value, "BUILDING") == 0)
        return accept(MapBuilding);
    if (stricmp(value, "HOUSE") == 0)
        return accept(MapHouse);
    if (stricmp(value, "FOREST BORDER") == 0)
        return accept(MapForestBorder);
    if (stricmp(value, "FOREST TRIANGLE") == 0)
        return accept(MapForestTriangle);
    if (stricmp(value, "FOREST SQUARE") == 0)
        return accept(MapForestSquare);
    if (stricmp(value, "CHURCH") == 0)
        return accept(MapChurch);
    if (stricmp(value, "CHAPEL") == 0)
        return accept(MapChapel);
    if (stricmp(value, "CROSS") == 0)
        return accept(MapCross);
    if (stricmp(value, "ROCK") == 0)
        return accept(MapRock);
    if (stricmp(value, "BUNKER") == 0)
        return accept(MapBunker);
    if (stricmp(value, "FORTRESS") == 0)
        return accept(MapFortress);
    if (stricmp(value, "FOUNTAIN") == 0)
        return accept(MapFountain);
    if (stricmp(value, "VIEW-TOWER") == 0 || stricmp(value, "VIEWTOWER") == 0)
        return accept(MapViewTower);
    if (stricmp(value, "LIGHTHOUSE") == 0)
        return accept(MapLighthouse);
    if (stricmp(value, "QUAY") == 0)
        return accept(MapQuay);
    if (stricmp(value, "FUELSTATION") == 0)
        return accept(MapFuelstation);
    if (stricmp(value, "HOSPITAL") == 0)
        return accept(MapHospital);
    if (stricmp(value, "FENCE") == 0)
        return accept(MapFence);
    if (stricmp(value, "WALL") == 0)
        return accept(MapWall);
    if (stricmp(value, "HIDE") == 0)
        return accept(MapHide);
    if (stricmp(value, "BUSSTOP") == 0)
        return accept(MapBusStop);
    // Everything else -- and the corpora carry plenty: `main road`, `track`, `railway`,
    // `power lines`, `transmitter`, `watertower`, `ruin`, `stack`, `tsign`, `powerwind`,
    // `shipwreck` -- is a map SYMBOL this engine does not draw. MapHide is the right
    // fallback and, importantly, is not vegetation.
    return MapHide;
}

// VEG-SWAY GATE TRACE (WGR_SWAY_GATE_DUMP). The wgpu wind sway is a pure function of the MapType
// resolved above and of NOTHING else, on both object paths:
//   direct   -- Object.cpp GCurrentFoliageKind = tree ? 2 : bush ? 1 : 0, written verbatim into
//               WgrDraw3D::mat_sun_ambient.w (EngineWgpu.cpp, an unconditional overwrite) and read
//               as shader3d.wgsl `foliage_kind`;
//   retained -- EngineWgpu.cpp WGR_INSTANCE_CANOPY_{BUSH,TREE,FOREST}, read as gpu_driven.wgsl
//               `canopy`.
// The gate cannot be seen in a screenshot -- it moves geometry, and a still frame cannot show
// motion -- but it CAN be seen here, once per model at load, before anything renders. `kind` is
// the number the direct-path shader compares against 0.5; `canopy` is the retained-path bit mask.
// Non-zero on a wall is the bug.
//   WGR_SWAY_GATE_DUMP=1  every model      =2  only models whose gate is non-zero
static void LogSwayGate(MapType mapType, const char* property, const char* modelName)
{
    static const int dump = []
    {
        const char* v = std::getenv("WGR_SWAY_GATE_DUMP");
        return v ? atoi(v) : 0;
    }();
    if (dump <= 0)
    {
        return;
    }
    const bool isTree = mapType == MapTree || mapType == MapSmallTree || mapType == MapForestBorder ||
                        mapType == MapForestTriangle || mapType == MapForestSquare;
    const bool isBush = mapType == MapBush;
    const float kind = isTree ? 2.0f : (isBush ? 1.0f : 0.0f);
    const unsigned canopy = isBush                                              ? 1u
                            : (mapType == MapTree || mapType == MapSmallTree)   ? 2u
                            : (mapType == MapForestBorder || mapType == MapForestTriangle ||
                               mapType == MapForestSquare)
                                                                                ? 4u
                                                                                : 0u;
    if (dump < 2 || kind > 0.0f)
    {
        LOG_INFO(Graphics, "SwayGate: kind={} canopy=0x{:x} mapType={} property='{}' model={}", kind, canopy,
                 int(mapType), property ? property : "", modelName ? modelName : "");
    }
}

MapType ResolveMapTypeProperty(const char* value, const char* modelName, bool* recognised)
{
    const MapType mapType = ResolveMapTypeTable(value, modelName, recognised);
    LogSwayGate(mapType, value, modelName);
    return mapType;
}

void LODShape::CalculateHints()
{
    InvalidateQueryPolicy();
    _andHints = ~0;
    _orHints = 0;
    for_each_alpha for (int level = 0; level < NLevels(); level++)
    {
        Shape* shape = Level(level);
        if (!shape)
        {
            continue;
        }
        _andHints &= shape->GetAndHints();
        _orHints |= shape->GetOrHints();
    }
#if ALPHA_SPLIT
    Shape* oShape = LevelOpaque(0);
    Shape* aShape = LevelAlpha(0);
    if (!aShape || oShape->NFaces() >= aShape->NFaces())
        aShape = oShape;
#else
    Shape* aShape = LevelOpaque(0);
#endif
    _color = aShape->_color, _colorTop = aShape->_colorTop;
    float alpha = _color.A8() * (1.0 / 255);
    float transparency = 1 - alpha * 1.5;

    if (transparency >= 0.99)
    {
        _viewDensity = 0;
    }
    if (transparency > 0.01)
    {
        _viewDensity = log(transparency) * 6;
    }
    else
    {
        _viewDensity = -10;
    }
}

void LODShape::DefineMinMax(int level)
{
    Shape* oShape = LevelOpaque(level);
    Shape* aShape = LevelOpaque(level);
    _minMax[0] = oShape->Min();
    _minMax[1] = oShape->Max();
    if (aShape)
    {
        SaturateMin(_minMax[0], aShape->Min());
        SaturateMin(_minMax[1], aShape->Max());
    }
    CalculateBoundingSphere();
}

void LODShape::CalculateBoundingSphereRadius()
{
    InvalidateQueryPolicy();
    float maxSphere2 = 0;
    for (int level = 0; level < _nLods; level++)
    {
        Shape* shape = Level(level);
        if (!shape)
        {
            continue;
        }
        for (int i = 0; i < shape->_pos.Size(); i++)
        {
            const V3& pos = shape->_pos[i];
            float sphere2 = pos.SquareSize();
            saturateMax(maxSphere2, sphere2);
        }
        for (int p = 0; p < shape->_phase.Size(); p++)
        {
            for (int i = 0; i < shape->_phase[p].Size(); i++)
            {
                const Vector3& pos = shape->_phase[p][i];
                float sphere2 = pos.SquareSize();
                saturateMax(maxSphere2, sphere2);
            }
        }
    }
    _boundingSphere = sqrt(maxSphere2);
}

void LODShape::CalculateBoundingSphere()
{
    InvalidateQueryPolicy();
    // -_boundingCenter center is original zero positioned in new coordinate system
    // when you add _boundingCenter to object coordinates, you will get original position

    // calculate bounding sphere from min-max information
    Vector3 oldBoundingCenter = _boundingCenter;
    Vector3 changeBoundingCenter = VZero;
    if (!_lockAutoCenter)
    {
        if (_autoCenter && NLevels() > 0)
        {
            // consider only clipflags of
            changeBoundingCenter = (_minMax[0] + _minMax[1]) * 0.5;
            // for OnSurface shapes do not change y component
            Shape* level0 = Level(0);
            if ((level0->GetAndHints() & ClipLandOn) && (level0->Special() & OnSurface))
            {
                changeBoundingCenter[1] = 0;
            }
        }
        else
        {
            // we might need to reset bounding center to zero
            changeBoundingCenter = -_boundingCenter;
        }

        if (changeBoundingCenter.SquareSize() < 1e-10 && _boundingSphere > 0)
        {
            // note: bounding sphere may be changed even when center is not
            CalculateBoundingSphereRadius();
            return; // no change
        }
    }
    _boundingCenter += changeBoundingCenter;
    _minMax[0] -= changeBoundingCenter;
    _minMax[1] -= changeBoundingCenter;
    _aimingCenter -= changeBoundingCenter;

    float maxSphere2 = 0;
    for_each_alpha for (int level = 0; level < _nLods; level++)
    {
        Shape* shape = Level(level);
        if (!shape)
        {
            continue;
        }
        for (int i = 0; i < shape->_pos.Size(); i++)
        {
            V3& pos = shape->_pos[i];
            pos -= changeBoundingCenter;
            float sphere2 = pos.SquareSize();
            saturateMax(maxSphere2, sphere2);
        }
        for (int p = 0; p < shape->_phase.Size(); p++)
        {
            for (int i = 0; i < shape->_phase[p].Size(); i++)
            {
                Vector3& pos = shape->_phase[p][i];
                pos -= changeBoundingCenter;
                float sphere2 = pos.SquareSize();
                saturateMax(maxSphere2, sphere2);
            }
        }
        // change min-max offsets of all lods
        shape->_minMax[0] -= changeBoundingCenter;
        shape->_minMax[1] -= changeBoundingCenter;
        shape->_bCenter -= changeBoundingCenter;
        shape->_minMaxOrig[0] -= changeBoundingCenter;
        shape->_minMaxOrig[1] -= changeBoundingCenter;
        shape->_bCenterOrig -= changeBoundingCenter;
        // change proxy positions
        for (int i = 0; i < shape->_proxy.Size(); i++)
        {
            ProxyObject* proxy = shape->_proxy[i];
            Object* pobj = proxy->obj;
            pobj->SetPosition(pobj->Position() - changeBoundingCenter);
            // recalc. inverse transform
            proxy->invTransform = pobj->InverseScaled();
        }
    }
    _boundingSphere = sqrt(maxSphere2);
    // d coefs of planes are changed
    if (changeBoundingCenter.SquareSize() > 0.01)
    {
        for_each_alpha for (int level = 0; level < _nLods; level++)
        {
            Shape* shape = Level(level);
            if (shape)
            {
                shape->RecalculateNormals(false);
            }
        }
    }
}

void LODShape::CalculateMinMax(bool recalcLevels)
{
    //  calculate only for first LOD-level
    //  we assume this will be valid for the other too
    //  scan through all vertices of all levels
    if (recalcLevels)
    {
        for_each_alpha for (int level = 0; level < NLevels(); level++)
        {
            Shape* shape = Level(level);
            if (!shape)
            {
                continue;
            }
            shape->CalculateMinMax();
            shape->StoreOriginalMinMax();
        }
    }

    // calculate min-max of all levels
    _minMax[0] = Vector3(1e10, 1e10, 1e10);    // min
    _minMax[1] = Vector3(-1e10, -1e10, -1e10); // max
    //_minMax[0]=Vector3(COORD_MAX,COORD_MAX,COORD_MAX); // min
    //_minMax[1]=Vector3(COORD_MIN,COORD_MIN,COORD_MIN); // max
    for_each_alpha for (int level = 0; level < NLevels(); level++)
    {
        Shape* shape = Level(level);
        if (!shape)
        {
            continue;
        }
        SaturateMin(_minMax[0], shape->Min());
        SaturateMax(_minMax[1], shape->Max());
    }

    CalculateBoundingSphere();
}

void LODShape::CheckForcedProperties()
{
    const ParamEntry& notes = Pars >> "CfgModels";
    char shortName[256];
    GetFilename(shortName, GetName());
    while (strpbrk(shortName, " -/()"))
    {
        *strpbrk(shortName, " -/()") = '_';
    }
    const ParamEntry* modelNotes = notes.FindEntry(shortName);
    if (modelNotes)
    {
        // check if some properties are defined
        const ParamEntry* props = modelNotes->FindEntry("properties");
        if (props && NLevels() > 0)
        {
            // scan properties and add them into geometry or topmost level
            Shape* geom = GeometryLevel();
            if (!geom)
            {
                geom = Level(0);
            }
            for (int i = 0; i < props->GetSize() - 1; i += 2)
            {
                RStringB propName = (*props)[i];
                RStringB propVal = (*props)[i + 1];
                geom->SetProperty(propName, propVal);
            }
            // some force properties may change meaning of some shapes
            ScanShapes();
        }
    }
}

void LODShape::ScanProperties()
{
    const char* armor = PropertyValue("armor");
    if (armor && *armor)
    {
        _armor = atof(armor);
    }
    else
    {
        _armor = 200;
    }
    if (_armor > 1e-10)
    {
        _invArmor = 1 / _armor;
        _logArmor = log(_armor);
    }
    else
    {
        _invArmor = 1e10;
        _logArmor = 25;
    }
}

void LODShape::CalculateMass()
{
    // mass should always be stored in geometry
    Shape* shape = GeometryLevel();
    if (!shape)
    {
        return;
    }
    if (_massArray.Size() == 0)
    {
        return;
    }
    double totalMass = 0;
    int i;
    // The mass table is per POINT of the geometry LOD and the native loaders keep
    // the two the same size. An imported ODOL carries the table exactly as the
    // binariser wrote it, and its point map may have been dropped as identity, so
    // the two can disagree; bound the walk rather than read past either.
    const int n = std::min(shape->_pointToVertex.Size(), _massArray.Size());
    // calculate position of center of mass
    Vector3 sum(VZero);
    for (i = 0; i < n; i++)
    {
        Vector3Val r = shape->Pos(shape->_pointToVertex[i]);
        float m = _massArray[i];
        totalMass += m;
        sum += r * m;
    }
    if (totalMass > 0.0)
    {
        _centerOfMass = sum * (1 / totalMass);
    }
    else
    {
        _centerOfMass = VZero;
    }

    Matrix3 totalInertia(M3Zero);
    for (i = 0; i < n; i++)
    {
        //{{FIX inertia
        Vector3Val r = shape->Pos(shape->_pointToVertex[i]) - _centerOfMass;
        //}}FIX inertia
        float m = _massArray[i];
        Matrix3Val rTilda = r.Tilda();
        totalInertia -= rTilda * rTilda * m;
    }
    _mass = totalMass;
    if (totalMass > 0)
    {
        _invInertia = totalInertia.InverseGeneral();
        _invMass = 1 / totalMass;
    }
    else
    {
        _invInertia = M3Identity;
        _invMass = 1;
    }
}

// LOD implementation

void LODShape::DoClear()
{
    // Forget everything but name; retain the destination revision lineage.
    InvalidateQueryPolicy();
    _canOcclude = false; // do not use for occlusions unless told otherwise
    _canBeOccluded = false;
    _autoCenter = true;
    _allowAnimation = false;
    _lockAutoCenter = false;
    _special = 0;
    _andHints = _orHints = 0; // default: no hints
    _nLods = 0;
    _remarks = 0;
    // Never left to fresh memory: MapType's zeroth enumerator is MapTree, which is
    // the gate for wind sway. Load / SerializeBin / ShapeAdapter all overwrite this,
    // but a path that forgets to (the MLOD adapter branch did) must not inherit a
    // tree by accident.
    _mapType = MapHide;
    _boundingCenter = VZero;
    _geometryCenter = VZero;
    _boundingSphere = 0;
    _minMax[0] = VZero;
    _minMax[1] = VZero;
    _geometry = _memory = _landContact = _roadway = _hitpoints = _paths = -1;
    _geometryFire = _geometryView = -1;

    _geometryViewPilot = -1;
    _geometryViewGunner = -1;
    _geometryViewCommander = -1;
    _geometryViewCargo = -1;

    _invInertia.SetIdentity();
    _centerOfMass = VZero;
    _mass = 0;
    _invMass = 1e10;
    _aimingCenter = VZero;
    _geomComponents = new ConvexComponents();
    _viewComponents = new ConvexComponents();
    _fireComponents = new ConvexComponents();
    _massArray.Clear();
    _viewDensity = -100;
    _color = PackedBlack;
    _colorTop = PackedBlack;
}

void LODShape::DoConstruct()
{
    _name = "";
    DoClear();
}

void LODShape::DoConstruct(const LODShape& src, bool copyAnimations)
{
    InvalidateQueryPolicy();
    // copy shapes, not only references
    for_each_alpha for (int i = 0; i < src._nLods; i++)
    {
        if (src._lods[i])
        {
            _lods[i] = new Shape(*src._lods[i], copyAnimations);
        }
        else
        {
            _lods[i] = nullptr;
        }
        _resolutions[i] = src._resolutions[i];
    }
    _nLods = src._nLods;

    _autoCenter = src._autoCenter;
    _lockAutoCenter = src._lockAutoCenter;
    _allowAnimation = src._allowAnimation;
    _minMax[0] = src._minMax[0], _minMax[1] = src._minMax[1];
    _boundingCenter = src._boundingCenter;
    _boundingSphere = src._boundingSphere;
    _special = src._special;
    _remarks = src._remarks;
    _andHints = src._andHints;
    _orHints = src._orHints;
    _geometry = src._geometry;
    _geometryFire = src._geometryFire;
    _geometryView = src._geometryView;

    _geometryViewPilot = src._geometryViewPilot;
    _geometryViewCommander = src._geometryViewCommander;
    _geometryViewGunner = src._geometryViewGunner;
    _geometryViewCargo = src._geometryViewCargo;

    _memory = src._memory;
    _landContact = src._landContact;
    _roadway = src._roadway;
    _paths = src._paths;
    _hitpoints = src._hitpoints;
    _name = ""; // copy name is empty
    _invInertia = src._invInertia;
    _mass = src._mass;
    _invMass = src._invMass;
    _centerOfMass = src._centerOfMass;
    _aimingCenter = src._aimingCenter;
    if (copyAnimations)
    {
        _geomComponents = new ConvexComponents(*src._geomComponents);
        _viewComponents = new ConvexComponents(*src._viewComponents);
        _fireComponents = new ConvexComponents(*src._fireComponents);
        if (src._massArray.Size() > 0 && GeometryLevel())
        {
            _massArray = src._massArray;
        }
        else
        {
            _massArray.Clear();
        }
    }
    else
    {
        _geomComponents = new ConvexComponents();
        _viewComponents = new ConvexComponents();
        _fireComponents = new ConvexComponents();
        _massArray.Clear();
    }
}

void LODShape::DoDestruct()
{
    InvalidateQueryPolicy();
#if VERBOSE
    if (_name[0])
    {
        LOG_DEBUG(Graphics, "Destruct shape {}", (const char*)_name);
    }
#endif
    int i;
    for (i = 0; i < _nLods; i++)
    {
        _lods[i].Free();
    }
    _nLods = 0;
}

void LODShape::OptimizeShapes()
{
    // Skip LOD optimization when no Application is available (tools/studio context)
    if (!GApp)
        return;

    InvalidateQueryPolicy();
    // delete LODs that are too complex
    // this reduces memory usage
    int i = 0;
    // scan normal LODs
    for (; i < _nLods; i++)
    {
        if (_resolutions[i] >= 900)
        {
            break;
        }
        float relRes = _resolutions[i] / _boundingSphere;
        // LOG_DEBUG(Graphics, "  {}: Relative resolution: {:.3f}",i,relRes);
        if (relRes > ENGINE_CONFIG.objectLODLimit)
        {
            break; // this LOD is neccessary
        }
        int needed = atoi(_lods[i]->PropertyValue("lodneeded"));
        if (needed >= 2)
        {
            break;
        }
    }
    int n = i - 1; // last LOD is always neccesary

    for (i = 0; i < n; i++)
    {
        // delete too complex LODs
        _lods[i] = nullptr;
        LOG_DEBUG(Graphics, "Dropped {}: {} ({})", (const char*)_name, i, _resolutions[i]);
    }

    // remove any nullptr LODs
    int d = 0;
    for (int s = 0; s < _nLods; s++)
    {
        if (_lods[s])
        {
            // delete any vdecal lods (used for some trees)
            if (s != 0 && (_lods[s]->GetAndHints() & ClipDecalMask) != ClipDecalNone && _lods[s]->NPos() > 0)
            {
                LOG_DEBUG(Graphics, "VDecal  {}: {} ({})", (const char*)_name, s, _resolutions[s]);
            }
            else if (ENGINE_CONFIG.enableHWTL && atoi(_lods[s]->PropertyValue("notl")) > 0)
            {
                LOG_DEBUG(Graphics, "TL dropped {}: {} ({})", (const char*)_name, s, _resolutions[s]);
            }
            else
            {
                _resolutions[d] = _resolutions[s];
                _lods[d] = _lods[s];
                _lods[d]->SetLevel(d);
                d++;
            }
        }
    }
    for (int s = d; s < _nLods; s++)
    {
        _lods[s] = nullptr;
    }
    _nLods = d;

    // scan shadow LODs
    for (i = 0; i < _nLods; i++)
    {
        if (_resolutions[i] >= 900)
        {
            break;
        }
        float relRes = _resolutions[i] / _boundingSphere;
        if (relRes > ENGINE_CONFIG.shadowLODLimit)
        {
            break; // this LOD is neccessary
        }
    }
    n = i - 1; // last LOD is always neccesary

    // optimize shadow LODs
    // if some lod is disabled for shadowing
    // all lods before it should be disabled to
    for (i = 0; i < n; i++)
    {
        // disable shadows of too complex LODs
        if (_lods[i]->FindProperty("lodnoshadow") < 0)
        {
            _lods[i]->_prop.Add(NamedProperty("lodnoshadow", "1"));
        }
    }
    ScanShapes();
}

} // namespace Poseidon
#include <Poseidon/World/Terrain/Landscape.hpp>
namespace Poseidon
{

DEFINE_FAST_ALLOCATOR(ProxyObject)

static RString ReplacesChars(RString shapeName, char c, char w)
{
    if (!strchr(shapeName, c))
    {
        return shapeName;
    }
    shapeName.MakeMutable();
    char* str = shapeName.MutableData();
    while (*str)
    {
        if (*str == c)
        {
            *str = w;
        }
        str++;
    }
    return shapeName;
}

bool GReplaceProxies = true;

void LODShape::Load(QIStream& f, bool reversed)
{
    DoClear();

#if VERBOSE
    LOG_DEBUG(Graphics, "Load {}, {}", (const char*)_name, reversed ? "Reversed" : "");
#endif

    // check optimized load
    int seekStart = f.tellg();
    if (LoadOptimized(f))
    {
        // reverse all vector data if necessary
        if (reversed)
        {
            Reverse();
            _remarks |= REM_REVERSED;
        }
        if (!CheckLegalCreator())
        {
            RptF("Bad file format (%s).", Name());
        }
        return;
    }
    f.seekg(seekStart, QIOS::beg);
    // check magic

    _special = 0;
    _remarks = 0;

    if (reversed)
    {
        _remarks |= REM_REVERSED;
    }

    // load all LODs and their setups respectivelly
    // check for LOD header
    char magic[4];
    int nLods = 1;
    bool tagged = false;
    int ver = 0;

    _mass = 0;
    _invMass = 1e10;
    _invInertia.SetZero();
    _centerOfMass = Vector3(VZero);

    f.read(magic, sizeof(magic));
    if (f.fail() || f.eof())
    {
        WarningMessage("Error loading %s (Magic)", (const char*)_name);
        goto Error;
    }
    if (!strncmp(magic, "NLOD", sizeof(magic)))
    {
        // LOD header - we will load multiple LODs
        Log("Warning: NLOD format in object %s", (const char*)_name);
        tagged = true;
        f.read((char*)&nLods, sizeof(nLods));
        if (f.fail() || f.eof())
        {
            WarningMessage("Error loading %s (nLods)", (const char*)_name);
            goto Error;
        }
    }
    else if (!strncmp(magic, "MLOD", sizeof(magic)))
    {
        // LOD header - we will load multiple LODs
        tagged = true;
        f.read((char*)&ver, sizeof(ver));
        f.read((char*)&nLods, sizeof(nLods));
        if (f.fail() || f.eof())
        {
            WarningMessage("Error loading %s (nLods)", (const char*)_name);
            goto Error;
        }
    }
    else
    {
        ErrorMessage("Warning: preNLOD format in object %s", (const char*)_name);
        f.seekg(-(int)sizeof(magic), QIOS::cur);
        Fail("Very old object loaded.");
    }
    int major, minor;
    major = ver >> 8;
    minor = ver & 0xff;
    if (major > 1) // only format versions 1.xx supported
    {
        WarningMessage("%s: Unsupported version %d.%02d", (const char*)_name, major, minor);
        goto Error;
    }

    for (int i = 0; i < nLods; i++)
    {
#if VERBOSE > 1
        LOG_DEBUG(Graphics, "  Load level {}", i);
#endif
        Ref<Shape> shape = new Shape();
        float resolution = 0;

        int startShapeInStream = f.tellg();

        bool wasMassArray = _massArray.Size() > 0;
        resolution = shape->LoadTagged(f, reversed, ver, false, _massArray, tagged);

        bool geometryOnly = ResolGeometryOnly(resolution);
        if (geometryOnly && shape->NFaces() > 0)
        {
            // to avoid unnecessary vertex sharing,
            // reload geometry with no normals
            int endShapeInStream = f.tellg();
            f.seekg(startShapeInStream, QIOS::beg);

            // revert state - massArray might be set during LoadTagged
            if (!wasMassArray)
            {
                _massArray.Clear();
            }

            shape = new Shape();
            shape->LoadTagged(f, reversed, ver, false, _massArray, tagged);
            // revert to new position
            f.seekg(endShapeInStream, QIOS::beg);
        }

        if (shape->_loadWarning)
        {
            LOG_DEBUG(Graphics, "Warnings in {}:{}", (const char*)_name, resolution);
        }
        AddShape(shape, resolution);

        // scan selections for proxy objects

        // scan for proxies
        for (int i = 0; i < shape->_sel.Size(); i++)
        {
            static const char proxyName[] = "proxy:";
            static int proxyNameLen = strlen(proxyName);

            const NamedSelection& sel = shape->_sel[i];
            const char* selName = sel.Name();

            if (strncmp(selName, proxyName, proxyNameLen))
            {
                continue;
            }
            selName += proxyNameLen;

            if (sel.Size() != 3)
            {
                RptF("%s:%s: Bad proxy object definition %s", (const char*)_name, LevelName(this, shape), sel.Name());
                continue;
            }

            // check if proxy selection is hidden
            if (sel.Faces().Size() != 1)
            {
                RptF("%s: Proxy object should be single face %s", (const char*)_name, sel.Name());
                if (sel.Faces().Size() < 1)
                {
                    continue;
                }
            }

            Poly& face = shape->FaceIndexed(sel.Faces()[0]);
            face.OrSpecial(IsHiddenProxy | NoTexMerger);
            face.SetTexture(nullptr);
            if ((face.Special() & (IsHidden | IsHiddenProxy)) == 0)
            {
                LOG_DEBUG(Graphics, "{}:{}: Proxy face should be hidden {}", (const char*)_name, LevelName(this, shape),
                          sel.Name());
            }

            char shapeName[256];
            snprintf(shapeName, sizeof(shapeName), "%s", (const char*)selName);
            char* ext = strchr(shapeName, '.');
            int id = -1;
            if (ext)
            {
                *(ext++) = 0;
                id = atoi(ext);
            }
            // it is proxy: define it

            Ref<ProxyObject> obj = new ProxyObject;
            // create object from shape name
            // shape name may be name of some vehicle class
            //  replace any spaces with '_';
            if (GReplaceProxies)
            {
                RString proxyType = RString("Proxy") + ReplacesChars(shapeName, ' ', '_');
                RString proxyShape = GetShapeName(shapeName);
                Ref<Object> pobj = NewObject(proxyType, proxyShape);
                if (!pobj)
                {
                    RptF("Cannot create proxy object %s", shapeName);
                    continue;
                }
                obj->obj = pobj;
            }
            else
            {
                obj->obj = NewProxyObject(shapeName);
            }
            obj->name = shapeName;
            obj->id = id;
            // get proxy object coordinates
            // scan selection
            int pi0 = sel[0], pi1 = sel[1], pi2 = sel[2];

            const V3* p0 = &shape->Pos(pi0);
            const V3* p1 = &shape->Pos(pi1);
            const V3* p2 = &shape->Pos(pi2);

            float dist01 = p0->Distance2(*p1);
            float dist02 = p0->Distance2(*p2);
            float dist12 = p1->Distance2(*p2);

            // p0,p1 should be the shortest distance
            if (dist01 > dist02)
            {
                swap(p1, p2), swap(dist01, dist02); // swap points 1,2
            }
            if (dist01 > dist12)
            {
                swap(p0, p2), swap(dist01, dist12); // swap points 0,2
            }
            // p0,p2 should be the second shortest distance
            if (dist02 > dist12)
            {
                swap(p0, p1), swap(dist02, dist12);
            }
            //  verify results

            Matrix4 trans;
            trans.SetPosition(*p0);
            trans.SetDirectionAndUp((*p1 - *p0), (*p2 - *p0));
            LODShapeWithShadow* pshape = obj->obj->GetShape();
            if (pshape)
            {
                trans.SetPosition(trans.FastTransform(pshape->BoundingCenter()));
            }
            obj->obj->SetTransform(trans);
            obj->invTransform = trans.InverseScaled();
            obj->selection = i;
            obj->obj->SetDestructType(DestructNo);
            shape->_proxy.Add(obj);
        }

        if (shape->_sel.Size() <= 0)
        {
            // no selections: we may safely optimize
            shape->Optimize();
        }

        // scan for sections
        if (Pars.FindEntry("CfgModels"))
        {
            const ParamEntry& notes = Pars >> "CfgModels";
            // note: shape name may contain spaces
            char shortName[256];
            GetFilename(shortName, GetName());
            while (strpbrk(shortName, " -/()"))
            {
                *strpbrk(shortName, " -/()") = '_';
            }
            if (notes.FindEntry(shortName))
            {
                shape->DefineSections(notes >> shortName);
            }
            else
            {
                shape->DefineSections(notes >> "default");
            }
            if (!geometryOnly)
            {
                shape->FindSections();
            }
        }
    }
    PoseidonAssert(_nLods >= 1);

    // map type
    {
        const char* mapType = PropertyValue("map");
        bool recognised = false;
        _mapType = ResolveMapTypeProperty(mapType, _name, &recognised);
        if (*mapType && !recognised)
        {
            RptF("%s: Unknown map type %s", (const char*)_name, mapType);
        }
    }
    // move (0,0,0) to the geometry center
    {
        const char* autoCenter = PropertyValue("autocenter");
        if (*autoCenter && atoi(autoCenter) == 0)
        {
            _autoCenter = false;
        }
        CalculateMinMax();
        CalculateHints();
        // calculate mass, center of mass and inertia
        if (_massArray.Size() > 0)
        {
            CalculateMass();
        }
    }
    // autodetect if animation should be allowed
    if (_orHints & (ClipLandMask | ClipDecalMask))
    {
        _allowAnimation = true;
    }
    if ((_orHints & ClipLightMask) == (_andHints & ClipLightMask))
    {
        ClipFlags light = _orHints & ClipLightMask;
        switch (light)
        {
            case ClipLightSky:
            case ClipLightCloud:
            case ClipLightStars:
            case ClipLightLine:
                _allowAnimation = true;
                break;
        }
    }

    {
        const char* animated = PropertyValue("animated");
        if (*animated)
        {
            _allowAnimation = atoi(animated) != 0;
        }
    }

    for (int i = 0; i < _nLods; i++)
    {
        if (_resolutions[i] < 900)
        {
            saturateMin(_resolutions[i], BoundingSphere() * 2);
        }
    }
    // remove too complex LODs
    OptimizeShapes();
    // scan only normal LODs

    for (int i = 0; i < _nLods; i++)
    {
        if (_resolutions[i] < 900 && _lods[i])
        {
            Shape* oShape = _lods[i];
            if (oShape)
            {
                _special |= oShape->Special();
            }
        }
    }
    CalculateMinMax();
    if (_massArray.Size() > 0)
    {
        CalculateMass();
    }

    CheckForcedProperties();
    InitConvexComponents();
    ScanProperties();
    CalculateHints();

    _propertyClass = PropertyValue("class");
    _propertyDammage = PropertyValue("dammage");

    {
        int complexity = LevelOpaque(0)->NFaces();
        float size = BoundingSphere();
        Shape* view = ViewGeometryLevel();
        int viewComplexity = view ? view->NFaces() : 0;
        // check size and complexity
        // large or simple objects may occlude
        if (viewComplexity <= 0)
        {
            _canOcclude = false; // view geometry empty
        }
        else if (size > 5)
        {
            _canOcclude = true;
        }
        else if (size > 2 && viewComplexity <= 6)
        {
            _canOcclude = true;
        }
        // large or complex objects may be occluded
        if (complexity >= 6)
        {
            _canBeOccluded = true;
        }
        if (size > 5)
        {
            _canBeOccluded = true;
        }
        // allow override with property
        const char* expCanOcclude = PropertyValue("canocclude");
        const char* expCanBeOccluded = PropertyValue("canbeoccluded");
        if (*expCanOcclude)
        {
            _canOcclude = atoi(expCanOcclude) != 0;
        }
        if (*expCanBeOccluded)
        {
            _canBeOccluded = atoi(expCanOcclude) != 0;
        }
    }

    if (GUseFileBanks && !CheckLegalCreator())
    {
        RptF("Bad file format (%s).", Name());
    }

    return;
Error:
    // create empty shape
    _lods[0] = new Shape();
    _lods[0]->SetLevel(0);
    _resolutions[0] = 0.0f;
    _nLods = 1;
}

void LODShape::PrepareProperties(const ParamEntry& cfg)
{
    // read names of selections that must not split across sections
}

void LODShape::Reload(QIStream& f, bool reversed)
{
    Log("Reload shape %s", (const char*)_name);

    Load(f, reversed);
}

void LODShape::Load(const char* name, bool reversed)
{
    // convert Data.. structures into Poly
    // load external file, convert ...
    // if applied outside constructor, Unload should be performed first
    // on error return empty object
    char nameTemp[1024];
    if (strlen(name) > sizeof(nameTemp))
    {
        WarningMessage("Name '%s' too long.");
    }
    strncpy(nameTemp, name, sizeof(nameTemp)); // save name
    nameTemp[sizeof(nameTemp) - 1] = 0;
    strlwr(nameTemp);
    _name = nameTemp;

    QIFStream f;
    if (GFileServer)
    {
        GFileServer->Open(f, _name);
    }
    else
    {
        f.open(_name);
    }
    if (f.fail())
    {
        WarningMessage("Cannot open object %s", (const char*)_name);
        return;
    }
    Load(f, reversed);
}

ConvexComponents* LODShape::GetConvexComponents(int level) const
{
    if (level == FindGeometryLevel())
    {
        return _geomComponents;
    }
    else if (level == FindFireGeometryLevel())
    {
        return _fireComponents;
    }
    else if (level == FindViewGeometryLevel())
    {
        return _viewComponents;
    }
    else if (level == FindViewPilotGeometryLevel())
    {
        return _viewPilotComponents;
    }
    else if (level == FindViewGunnerGeometryLevel())
    {
        return _viewGunnerComponents;
    }
    else if (level == FindViewCommanderGeometryLevel())
    {
        return _viewCommanderComponents;
    }
    else if (level == FindViewCargoGeometryLevel())
    {
        return _viewCargoComponents;
    }
    else
    {
        return nullptr;
    }
}

void LODShape::FindHitComponents(FindArray<int>& hits, const char* name) const
{
    // scan firegeometry components
    hits.Resize(0);
    Shape* geom = FireGeometryLevel();
    if (!geom)
    {
        return;
    }
    int sel = geom->FindNamedSel(name);
    if (sel < 0)
    {
        return;
    }
    // check if it is in some Component%02d selection
    const NamedSelection& namedSel = geom->NamedSel(sel);

    for (int i = 1; i < 1000; i++)
    {
        char name[64];
        snprintf(name, sizeof(name), "Component%02d", i);
        int selIndex = geom->FindNamedSel(name);
        if (selIndex < 0)
        {
            break;
        }
        const NamedSelection& sel = geom->NamedSel(selIndex);
        if (sel.IsSubset(namedSel) || namedSel.IsSubset(sel))
        {
            // selection in component or component in selection
            hits.AddUnique(i - 1);
        }
    }
}

void LODShape::InvalidateConvexComponents(int level)
{
    ConvexComponents* cc = GetConvexComponents(level);
    if (cc)
    {
        cc->Invalidate();
    }
}

void LODShape::RecalculateConvexComponentsAsNeeded(int level)
{
    ConvexComponents* cc = GetConvexComponents(level);
    if (cc)
    {
        cc->RecalculateAsNeeded(Level(level));
    }
}

void LODShape::InitConvexComponents(ConvexComponents& cc, Shape* geom)
{
    geom->RecalculateNormalsAsNeeded();
    for (int i = 1; i < 1000; i++)
    {
        char name[64];
        snprintf(name, sizeof(name), "Component%02d", i);
        int selIndex = geom->FindNamedSel(name);
        if (selIndex < 0)
        {
            break;
        }
        const NamedSelection& sel = geom->NamedSel(selIndex);
        if (sel.Faces().Size() < 4)
        {
            if (sel.Faces().Size() > 0)
            {
                RptF("Strange convex component %s in %s:%s", (const char*)_name, (const char*)name,
                     LevelName(this, geom));
            }
            continue;
        }
        ConvexComponent* component = new ConvexComponent;
        cc.Add(component);
        component->Init(geom, name);
    }
    cc.Validate();
}

void LODShape::InitCC(Ref<ConvexComponents>& cc, Shape* shape)
{
    if (shape)
    {
        cc = new ConvexComponents();
        InitConvexComponents(*cc, shape);
        if (cc->RecalculateEdges(shape))
        {
            RptF("Shape %s:%s - bad components", (const char*)Name(), LevelName(this, shape));
        }
    }
}

void LODShape::InitConvexComponents()
{
    Shape* geom = GeometryLevel();
    Shape* fire = FireGeometryLevel();
    Shape* view = ViewGeometryLevel();
    if (geom)
    {
        InitConvexComponents(*_geomComponents, geom);
    }
    if (fire)
    {
        if (fire == geom)
        {
            _fireComponents = _geomComponents;
        }
        else
        {
            InitConvexComponents(*_fireComponents, fire);
        }
    }
    if (view)
    {
        if (view == geom)
        {
            _viewComponents = _geomComponents;
        }
        else if (view == fire)
        {
            _viewComponents = _fireComponents;
        }
        else
        {
            InitConvexComponents(*_viewComponents, view);
        }

        const char* expCanOcclude = PropertyValue("canocclude");
        if (*expCanOcclude && atoi(expCanOcclude) == 0)
        {
            // occlusion disabled - no edges
        }
        else
        {
            if (_viewComponents->RecalculateEdges(view))
            {
                RptF("Shape %s:%s - bad components", (const char*)Name(), LevelName(this, view));
            }
        }
    }

    if (geom)
    {
        _geometryCenter = (geom->Max() + geom->Min()) * 0.5;
        _geometrySphere = geom->Max().Distance(geom->Min()) * 0.5;
        _aimingCenter = _geometryCenter;
    }
    else
    {
        _geometrySphere = _boundingSphere;
        _geometryCenter = _boundingCenter;
        _aimingCenter = _boundingCenter;
    }
    // override aiming point if necessary
    const char* aimName = "zamerny";
    if (MemoryPointExists(aimName))
    {
        _aimingCenter = MemoryPoint(aimName);
    }

    InitCC(_viewPilotComponents, ViewPilotGeometryLevel());
    InitCC(_viewGunnerComponents, ViewGunnerGeometryLevel());
    InitCC(_viewCommanderComponents, ViewCommanderGeometryLevel());
    InitCC(_viewCargoComponents, ViewCargoGeometryLevel());
}

const RStringB& LODShape::PropertyValue(const char* name) const
{
    Shape* level = GeometryLevel();
    if (level)
    {
        const RStringB& value = level->PropertyValue(name);
        if (value.GetLength() > 0)
        {
            return value;
        }
    }
    level = _lods[0];
    if (!level)
    {
        Fail("No shape");
        return Foundation::RStringBEmpty;
    }
    return level->PropertyValue(name);
}

int LODShape::FindLevel(float resolution, bool noDecal) const
{
    // find first suitable LOD level
    PoseidonAssert(_nLods >= 1);
    if (resolution < 900)
    {
        // find normal LOD
        int i = 1;
        for (; i < _nLods; i++)
        {
            float aRes = _resolutions[i];
            if (aRes > resolution)
            {
                break; // this one is too rough
            }
            if (aRes >= 900)
            {
                break; // memory or special LOD
            }
        }
        i--;
        while (i > 0 && !_lods[i])
        {
            i--;
        }
        if (noDecal)
        {
            while (i > 0 && _lods[i] && _lods[i - 1] && (_lods[i]->GetAndHints() & ClipDecalMask) != ClipDecalNone)
            {
                i--;
            }
        }
        return _lods[i] ? i : -1;
    }
    else
    {
        // find special LOD
        float minDiff = 1e20;
        int minI = -1;
        for (int i = 0; i < _nLods; i++)
        {
            if (!_lods[i])
            {
                continue;
            }
            float diff = fabs(_resolutions[i] - resolution);
            if (minDiff > diff)
            {
                minDiff = diff, minI = i;
            }
        }
        return minI;
    }
}

bool LODShape::IsSpecLevel(int level, float spec) const
{
    return (level >= 0 && _resolutions[level] > spec * 0.99 && _resolutions[level] < spec * 1.01);
}

int LODShape::FindSpecLevel(float spec) const
{
    int level = FindLevel(spec);
    if (level >= 0 && _resolutions[level] > spec * 0.99 && _resolutions[level] < spec * 1.01)
    {
        return level;
    }
    return -1;
}

int LODShape::FindSqrtLevel(float resolution2, bool noDecal) const
{
    // find first suitable LOD level
    PoseidonAssert(_nLods >= 1);
    // only normal LOD searched
    int i = 1;
    for (; i < _nLods; i++)
    {
        float aRes = _resolutions[i];
        if (aRes * aRes > resolution2)
        {
            break; // this one is too rough
        }
        // AST-018 hardening (NOT a fix for any observed defect -- measured blast
        // radius on Takistan is zero). This was the last consumer deciding
        // drawability by float comparison, the thing LodPurpose.hpp exists to be
        // the single copy of. `>= 900` is also order-dependent, and 342 of
        // Takistan's 513 referenced models store their shadow buffers (11000 /
        // 11010) AFTER the 1e15 sentinels rather than in ascending order.
        //
        // The test is `== Visual`, deliberately not IsDrawableLod: the crewed view
        // LODs (1000 / 1100 / 1200) are drawable but are reserved distances, not
        // detail steps, and a distance chooser must never walk into them. Every
        // resolution below 900 classifies Visual, so on measured content this
        // breaks in exactly the same place the old comparison did.
        if (Model::ClassifyLodResolution(aRes) != Model::LodPurpose::Visual)
        {
            break; // special LOD (memory / geometry / shadow / unknown sentinel)
        }
    }
    i--;
    if (noDecal)
    {
        while (i > 0 && _lods[i - 1] && (_lods[i]->GetAndHints() & ClipDecalMask) != ClipDecalNone)
        {
            i--;
        }
    }
    return i;
}

// WGR_SHADOW_LOD_PROP_CACHE=0 restores the uncached per-level Shape::FindProperty walk, so an
// A/B of the shadow-LOD selection costs an env var rather than a rebuild.
static bool ShadowLodPropCacheEnabled()
{
    static const bool on = []
    {
        const char* v = std::getenv("WGR_SHADOW_LOD_PROP_CACHE");
        return !(v && std::strcmp(v, "0") == 0);
    }();
    return on;
}

// WGR_IGNORE_LODNOSHADOW=1 makes the shadow-LOD search ignore the `lodnoshadow` property, so a
// model that declares every visual LOD non-casting still casts from the one it draws. An
// ABLATION, default off: it exists to A/B whether a world's missing object shadows are that
// property or something else, without a rebuild between the two captures.
//
// Why it is worth having: `lodnoshadow` is a 2001 authoring hint meaning "do not build a stencil
// shadow VOLUME from this LOD" -- the volume was extruded per-object on the CPU and a canopy of
// alpha cards would have produced garbage. It does not mean "this object occludes no light", and
// a depth-map cascade has no such problem. DayZ's trees carry it on their visual LODs (measured:
// b_fagussylvatica_1f 6 occurrences over 8 LODs, b_piceaabies_1f 4), which is why a Chernarus
// forest lights every leaf card equally and throws nothing on the ground.
static bool IgnoreLodNoShadow()
{
    static const bool on = []
    {
        const char* v = std::getenv("WGR_IGNORE_LODNOSHADOW");
        return v && std::strcmp(v, "0") != 0;
    }();
    return on;
}

int LODShape::FindNearestWithoutProperty(int i, const char* property) const
{
    if (i == LOD_INVISIBLE)
    {
        return i;
    }
    PoseidonAssert(i < _nLods);
    PoseidonAssert(i >= 0);
    // "lodnoshadow" is the only property asked here at draw rate (Scene::AdjustShadowComplexity,
    // once per drawn object per complexity iteration, up to seven iterations a frame). Shape
    // memoises that one answer, so recognise it ONCE per call instead of paying FindProperty's
    // snprintf/strlwr/strcmp assertion machinery once per LOD stepped over.
    if (IgnoreLodNoShadow() && std::strcmp(property, "lodnoshadow") == 0)
    {
        return i;
    }
    const bool useCache = ShadowLodPropCacheEnabled() && std::strcmp(property, "lodnoshadow") == 0;
    const auto hasProperty = [&](const Shape* lod)
    { return useCache ? lod->HasNoShadowProperty() : (lod->FindProperty(property) >= 0); };
    while (i > 0 && _lods[i] && hasProperty(_lods[i]))
    {
        i--;
    }
    while (i < _nLods && _lods[i] && _resolutions[i] < 900 && hasProperty(_lods[i]))
    {
        i++;
    }
    if (i >= _nLods)
    {
        return -1;
    }
    return i;
}

int Shape::PointIndex(const char* name) const
{
    int index = FindNamedSel(name);
    if (index < 0)
    {
        return -1;
    }
    const Selection& sel = NamedSel(index);
    // selection should be one point only
    if (sel.Size() < 1)
    {
        LOG_ERROR(Graphics, "No point in selection {}.", name);
        return -1;
    }
    return sel[0];
}

const V3& Shape::NamedPosition(const char* name, const char* altName) const
{
    int pIndex = PointIndex(name);
    if (pIndex >= 0)
    {
        return Pos(pIndex);
    }
    if (altName)
    {
        int pIndex = PointIndex(altName);
        if (pIndex >= 0)
        {
            return Pos(pIndex);
        }
    }
    return V3Zero;
}

void LODShape::ScanProxies(bool modifyFaces)
{
    InvalidateQueryPolicy();
    extern bool GReplaceProxies;
    for (int l = 0; l < _nLods; l++)
    {
        Shape* shape = _lods[l];
        if (!shape)
            continue;
        for (int i = 0; i < shape->_sel.Size(); i++)
        {
            static const char proxyName[] = "proxy:";
            static int proxyNameLen = strlen(proxyName);
            const NamedSelection& sel = shape->_sel[i];
            const char* selName = sel.Name();
            if (strncmp(selName, proxyName, proxyNameLen))
                continue;
            selName += proxyNameLen;
            if (sel.Size() != 3)
            {
                RptF("%s: Bad proxy object definition %s", (const char*)_name, sel.Name());
                continue;
            }
            if (sel.Faces().Size() < 1)
                continue;
            if (modifyFaces)
            {
                Poly& face = shape->FaceIndexed(sel.Faces()[0]);
                face.OrSpecial(IsHiddenProxy | NoTexMerger);
                face.SetTexture(nullptr);
            }
            char shapeName[256];
            snprintf(shapeName, sizeof(shapeName), "%s", (const char*)selName);
            char* ext = strchr(shapeName, '.');
            int id = -1;
            if (ext)
            {
                *(ext++) = 0;
                id = atoi(ext);
            }
            Ref<ProxyObject> obj = new ProxyObject;
            if (GReplaceProxies)
            {
                RString proxyType = RString("Proxy") + ReplacesChars(shapeName, ' ', '_');
                RString proxyShape = GetShapeName(shapeName);
                Ref<Object> pobj = NewObject(proxyType, proxyShape);
                if (!pobj)
                {
                    RptF("Cannot create proxy object %s", shapeName);
                    continue;
                }
                obj->obj = pobj;
            }
            else
            {
                obj->obj = NewProxyObject(shapeName);
            }
            obj->name = shapeName;
            obj->id = id;
            int pi0 = sel[0], pi1 = sel[1], pi2 = sel[2];
            const V3* p0 = &shape->Pos(pi0);
            const V3* p1 = &shape->Pos(pi1);
            const V3* p2 = &shape->Pos(pi2);
            float dist01 = p0->Distance2(*p1);
            float dist02 = p0->Distance2(*p2);
            float dist12 = p1->Distance2(*p2);
            if (dist01 > dist02)
            {
                swap(p1, p2);
                swap(dist01, dist02);
            }
            if (dist01 > dist12)
            {
                swap(p0, p2);
                swap(dist01, dist12);
            }
            if (dist02 > dist12)
            {
                swap(p0, p1);
                swap(dist02, dist12);
            }
            Matrix4 trans;
            trans.SetPosition(*p0);
            trans.SetDirectionAndUp((*p1 - *p0), (*p2 - *p0));
            LODShapeWithShadow* pshape = obj->obj->GetShape();
            if (pshape)
                trans.SetPosition(trans.FastTransform(pshape->BoundingCenter()));
            obj->obj->SetTransform(trans);
            obj->invTransform = trans.InverseScaled();
            obj->selection = i;
            obj->obj->SetDestructType(DestructNo);
            shape->_proxy.Add(obj);
        }
    }
}

DEFINE_FAST_ALLOCATOR(LODShapeWithShadow)
LODShapeWithShadow::LODShapeWithShadow() = default;

LODShapeWithShadow::LODShapeWithShadow(const char* name, bool reversed) : LODShape(name, reversed) {}

LODShapeWithShadow::LODShapeWithShadow(QIStream& f, bool reversed) : LODShape(f, reversed) {}

LODShape::LODShape()
{
    DoConstruct();
}
LODShape::LODShape(const LODShape& src, bool copyAnimations)
{
    DoConstruct(src, copyAnimations);
    ScanProperties();
}
void LODShape::operator=(const LODShape& src)
{
    DoDestruct();
    DoConstruct(src, true);
}
namespace
{
// See LODShape::SetDestroyListener. Plain statics: set once before any streamed shape can
// die, cleared in the renderer's destructor; the listener itself is what has to be cheap.
LODShape::DestroyListener GShapeDestroyListener = nullptr;
void* GShapeDestroyListenerData = nullptr;
} // namespace

void LODShape::SetDestroyListener(DestroyListener listener, void* userData)
{
    GShapeDestroyListener = listener;
    GShapeDestroyListenerData = userData;
}

LODShape::~LODShape()
{
    if (GShapeDestroyListener)
    {
        GShapeDestroyListener(this, GShapeDestroyListenerData);
    }
    DoDestruct();
}

LODShape::LODShape(const char* name, bool reversed)
{
    DoConstruct();
    Load(name, reversed);
}
LODShape::LODShape(QIStream& f, bool reversed)
{
    DoConstruct();
    Load(f, reversed);
}

const V3& LODShape::NamedPoint(int level, const char* name, const char* altName) const
{
    Shape* shape = Level(level);
    int pIndex = shape->PointIndex(name);
    if (pIndex >= 0)
    {
        return shape->Pos(pIndex);
    }
    if (altName)
    {
        int pIndex = shape->PointIndex(altName);
        if (pIndex >= 0)
        {
            return shape->Pos(pIndex);
        }
    }
    return V3Zero;
}

const V3& LODShape::MemoryPoint(const char* name, const char* altName) const
{
    Shape* memory = MemoryLevel();
    if (!memory)
    {
        return V3Zero;
    }
    return memory->NamedPosition(name, altName);
}

bool LODShape::MemoryPointExists(const char* name) const
{
    Shape* memory = MemoryLevel();
    if (!memory)
    {
        return false;
    }
    int pIndex = memory->PointIndex(name);
    return pIndex >= 0;
}

bool LODShape::IsInside(Vector3Par pos) const
{
    // make sure there is well defined geometry LOD
    if (_geomComponents->Size() <= 0)
    {
        return false;
    }

    GeometryLevel()->RecalculateNormalsAsNeeded();
    RecalculateGeomComponentsAsNeeded();
    // all calculation will be performed in model space
    for (int iThis = 0; iThis < _geomComponents->Size(); iThis++)
    {
        const ConvexComponent& cThis = *(*_geomComponents)[iThis];
        // check intersection will all convex components
        if (cThis.IsInside(pos))
        {
            return true;
        }
    }
    return false;
}

void LODShape::OrSpecial(int special)
{
    _special |= special;
    for_each_alpha for (int i = 0; i < _nLods; i++)
    {
        if (_lods[i])
        {
            _lods[i]->OrSpecial(special);
        }
    }
}
void LODShape::AndSpecial(int special)
{
    _special &= special;
    for_each_alpha for (int i = 0; i < _nLods; i++)
    {
        if (_lods[i])
        {
            _lods[i]->AndSpecial(special);
        }
    }
}
void LODShape::SetSpecial(int special)
{
    _special = special;
    for_each_alpha for (int i = 0; i < _nLods; i++)
    {
        if (_lods[i])
        {
            _lods[i]->SetSpecial(special);
        }
    }
}
void LODShape::RescanSpecial()
{
    _special = 0;
    for_each_alpha for (int i = 0; i < _nLods; i++)
    {
        if (_resolutions[i] > 900)
        {
            continue;
        }
        if (_lods[i])
        {
            _special |= _lods[i]->Special();
        }
    }
}

void LODShape::ScanShapes()
{
    InvalidateQueryPolicy();
    _geometry = -1;
    _geometryFire = -1;
    _geometryView = -1;

    _geometryViewPilot = -1;
    _geometryViewGunner = -1;
    _geometryViewCommander = -1;
    _geometryViewCargo = -1;

    _memory = -1;
    _landContact = -1;
    _roadway = -1;
    _hitpoints = -1;
    _paths = -1;
    for (int i = 0; i < _nLods; i++)
    {
        float resolution = _resolutions[i];
        if (resolution > 900)
        {
            // AST-018: the third copy of the sentinel table, now the IR's.
            switch (Model::ClassifyLodResolution(resolution))
            {
                case Model::LodPurpose::Geometry:              _geometry = i; break;
                case Model::LodPurpose::Memory:                _memory = i; break;
                case Model::LodPurpose::LandContact:           _landContact = i; break;
                case Model::LodPurpose::Roadway:               _roadway = i; break;
                case Model::LodPurpose::Paths:                 _paths = i; break;
                case Model::LodPurpose::HitPoints:             _hitpoints = i; break;
                case Model::LodPurpose::ViewGeometry:          _geometryView = i; break;
                case Model::LodPurpose::FireGeometry:          _geometryFire = i; break;
                case Model::LodPurpose::ViewPilotGeometry:     _geometryViewPilot = i; break;
                case Model::LodPurpose::ViewGunnerGeometry:    _geometryViewGunner = i; break;
                case Model::LodPurpose::ViewCommanderGeometry: _geometryViewCommander = i; break;
                case Model::LodPurpose::ViewCargoGeometry:     _geometryViewCargo = i; break;
                default:
                    // Cockpit and spec-cockpit LODs are drawn, so they take no
                    // index here; anything else above 10000 is a sentinel this
                    // build has no slot for and is worth saying so.
                    if (resolution >= 10000)
                        LOG_DEBUG(Graphics, "{}: Uknown spec lod ({})", (const char*)Name(), resolution);
                    break;
            }
        }
    }
    if (_geometry >= 0)
    {
        // geometry is made alpha transparent
        _lods[_geometry]->OrSpecial(IsAlpha | IsAlphaFog | IsColored);
    }
    if (_geometryView >= 0)
    {
        // geometry is made alpha transparent
        _lods[_geometryView]->OrSpecial(IsAlpha | IsAlphaFog | IsColored);
    }
    if (_geometryFire >= 0)
    {
        // geometry is made alpha transparent
        _lods[_geometryFire]->OrSpecial(IsAlpha | IsAlphaFog | IsColored);
    }
    if (_geometryView < 0)
    {
        _geometryView = _geometry;
    }
    if (_geometryFire < 0)
    {
        _geometryFire = _geometryView;
    }
    if (_geometry >= 0)
    {
        const char* fireGeom = _lods[_geometry]->PropertyValue("firegeometry");
        if (atoi(fireGeom) > 0)
        {
            _geometryFire = _geometry;
        }
        const char* viewGeom = _lods[_geometry]->PropertyValue("viewgeometry");
        if (atoi(viewGeom) > 0)
        {
            _geometryView = _geometry;
        }
    }
}

void LODShape::AddShape(Shape* shape, float resolution)
{
    // added to the end of the LOD list
    PoseidonAssert(_nLods < MAX_LOD_LEVELS);
    _lods[_nLods] = shape;
    _resolutions[_nLods] = resolution;
    shape->SetLevel(_nLods);
    _nLods++;
    ScanShapes();
}

void LODShape::ChangeShape(int level, Shape* shape)
{
    InvalidateQueryPolicy();
    PoseidonAssert(level < _nLods);
    _lods[level] = shape;
    if (shape)
    {
        shape->SetLevel(level);
    }
}

} // namespace Poseidon
