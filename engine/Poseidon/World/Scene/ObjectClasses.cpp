
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp>
#include <Poseidon/World/Scene/ObjectClasses.hpp>

#include <Poseidon/AI/AI.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Random/randomGen.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Dev/Diag/LightingDiag.hpp>
#include <string.h>
#include <cmath>
#include <cstdlib> // std::getenv - WGR_ROAD_LEGACY_SURFACE (RoadConformsOnGpu)
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/BoolArray.hpp>
#include <Poseidon/Foundation/Enums/EnumNames.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>
#include <Poseidon/Foundation/Math/MathOpt.hpp>

namespace Poseidon::Foundation
{
template class Ref<RoadType>;
} // namespace Poseidon::Foundation

// template VerySmallArray<int,MAX_LOD_LEVELS>; // bug is MSVC 5.0 - force instantiation

namespace Poseidon
{
template <>
const ::Poseidon::Foundation::EnumName* ::Poseidon::Foundation::GetEnumNames(StreetLamp::LightState dummy)
{
    static const ::Poseidon::Foundation::EnumName LightStateNames[] = {
        ::Poseidon::Foundation::EnumName(StreetLamp::LSOff, "OFF"),
        ::Poseidon::Foundation::EnumName(StreetLamp::LSOn, "ON"),
        ::Poseidon::Foundation::EnumName(StreetLamp::LSAuto, "AUTO"), ::Poseidon::Foundation::EnumName()};
    return LightStateNames;
}

EntityHitType::EntityHitType(const ParamEntry* param) : base(param)
{
    _scopeLevel = 0;
}
EntityHitType::~EntityHitType() = default;

void EntityHitType::Load(const ParamEntry& par)
{
    base::Load(par);
}
void EntityHitType::InitShape()
{
    base::InitShape();
}

void EntityHitType::DeinitShape()
{
    base::DeinitShape();
}

EntityHit::EntityHit(LODShapeWithShadow* shape, const EntityHitType* type, int id) : base(shape, type, id)
{
    PoseidonAssert(type == GetNonAIType());

    _hit.Realloc(type->_hitPoints.Size());
    _hit.Resize(type->_hitPoints.Size());
    for (int i = 0; i < type->_hitPoints.Size(); i++)
    {
        _hit[i] = 0;
    }

    PoseidonAssert(type);
    PoseidonAssert(!type->IsAbstract());
    PoseidonAssert(_shape == type->_shape);
}

EntityHit::~EntityHit() = default;

inline float CalcHitDammage(float distance2, float valRange2)
{
    if (distance2 <= valRange2)
    {
        return 1;
    }
    else
    {
        return valRange2 * valRange2 / (distance2 * distance2);
    }
    // else return valRange2*valRange2*valRange2/(distance2*distance2*distance2);
}
float EntityHit::GetHit(const HitPoint& hitpoint) const
{
    int index = hitpoint.GetIndex();
    if (index >= 0)
    {
        // discreete hit simulation
        if (_hit[index] >= 0.9)
        {
            return 1;
        }
    }
    return 0;
}

float EntityHit::GetHitCont(const HitPoint& hitpoint) const
{
    int index = hitpoint.GetIndex();
    if (index >= 0)
    {
        // continuous hit simulation
        return _hit[index];
    }
    return 0;
}

float EntityHit::LocalHit(Vector3Par pos, float val, float valRange)
{
    // scan all hitpoints and dammage them
    Shape* hitShape = _shape->HitpointsLevel();
    if (!hitShape)
    {
        return 1;
    }

    LOG_DEBUG(Graphics, "{} hit ({:.1f},{:.1f},{:.1f}) (val {:.2f},{:.2f})", (const char*)GetDebugName(), pos[0],
              pos[1], pos[2], val, valRange);

    Animate(_shape->FindHitpoints());

    if (valRange < 0)
    {
        // narrower hit area improves hit locality for soldiers; suits all vehicles
        valRange *= 0.25; // smaller area around direct hit
                          // val *= 2; // but stronger effect
    }

    float valRange2 = Square(valRange);
    const HitPointList& hitpoints = GetType()->GetHitPoints();
    for (int i = 0; i < hitpoints.Size(); i++)
    {
        const HitPoint& hit = *hitpoints[i];
        int index = hit.GetSelection();
        if (index >= 0)
        {
            const NamedSelection& sel = hitShape->NamedSel(index);
            for (int j = 0; j < sel.Size(); j++)
            {
                int pIndex = sel[j];
                Vector3Val hitPt = hitShape->Pos(pIndex);
                float distance2 = hitPt.Distance2(pos);
                float dammage = val * CalcHitDammage(distance2, valRange2);
                LOG_DEBUG(Graphics, "hitPt {:.1f},{:.1f},{:.1f}", hitPt[0], hitPt[1], hitPt[2]);
                LOG_DEBUG(Graphics, " dist {:.1f}: dammage {:.6f}", sqrt(distance2), dammage);
                if (dammage > 1e-4)
                {
                    float hitVal = dammage / hit.GetArmor();
                    float oldHit = _hit[i];
                    float newHit = oldHit + hitVal;
                    _hit[i] = newHit;
                    LOG_DEBUG(Graphics, " hit {}: {:.6f}", (const char*)sel.GetName(), newHit);
                    saturateMin(_hit[i], 1);
                }
            }
        }
    }
    Deanimate(_shape->FindHitpoints());
    return GetType()->GetStructuralDammageCoef();
}

void EntityHit::ResetStatus()
{
    for (int i = 0; i < _hit.Size(); i++)
    {
        _hit[i] = 0;
    }
    base::ResetStatus();
}

StreetLampType::StreetLampType(const ParamEntry* param) : base(param)
{
    _scopeLevel = 1;
}

StreetLampType::~StreetLampType() = default;

void StreetLampType::Load(const ParamEntry& par)
{
    base::Load(par);

    _colorDiffuse = GetColor(par >> "colorDiffuse");
    _colorAmbient = GetColor(par >> "colorAmbient");
    _brightness = par >> "brightness";

    // LAMP-003: the config numbers that decide the pool size. `brightness` is not an
    // intensity -- LightPoint::SetBrightness turns it into _startAtten = 50*brightness,
    // a flat full-strength core of that radius with an inverse-square tail cut at 10x.
    // Log it once per lamp type so a night capture can be read against real numbers.
    LOG_INFO(World, "LAMP-003 type {}: brightness={:.3f} -> startAtten={:.1f} m, reach={:.0f} m; "
                    "diffuse=({:.2f},{:.2f},{:.2f}) ambient=({:.2f},{:.2f},{:.2f})",
             (const char*)GetName(), _brightness, 50.0f * _brightness, 500.0f * _brightness, _colorDiffuse.R(),
             _colorDiffuse.G(), _colorDiffuse.B(), _colorAmbient.R(), _colorAmbient.G(), _colorAmbient.B());
}

void StreetLampType::InitShape()
{
    const ParamEntry& par = *_par;
    _scopeLevel = 2;
    base::InitShape();
    if (!_shape)
        return; // Abstract lamp types have no hit-point geometry.
    DEF_HIT(_shape, _bulbHit, "lampa", nullptr, par >> "armorBulb");
}
void StreetLampType::DeinitShape()
{
    base::DeinitShape();
}

StreetLamp::StreetLamp(LODShapeWithShadow* shape, StreetLampType* type, int id)
    : base(shape, type, id), _pilotLight(true),
      _lightPos(GetShape() ? GetShape()->MemoryPoint("light") : VZero)
{
    SetSimulationPrecision(12.1256);
    _destrType = DestructTree;
    _static = true;
    Object::_type = Primary;
    _lightState = LSAuto;
}

static const Color StreetLightColor(0.9, 0.8, 0.6);
static const Color StreetLightAmbient(0.1, 0.1, 0.1);

void StreetLamp::SwitchLight(LightState state)
{
    _lightState = state;
    SimulateSwitch();
    // check if coordinates make sense
    if (Position().SquareSize() > 100)
    {
        CreateLight(*this);
    }
    else
    {
        Fail("Switched lamp not in landscape");
    }
}

void StreetLamp::Init(Matrix4Par pos)
{
    base::Init(pos);
    SimulateSwitch();
    CreateLight(pos);
}

// LAMP-001 diagnostic. It has to distinguish three states that all look like "the village
// is dark": no StreetLamp objects were created at all (the models' `class` property never
// promoted them), lamps exist but are switched off, and lamps exist and are lit. A counter
// that can only say "0 lights" cannot tell the first from the second.
namespace
{
long long s_lampsSeen = 0;
long long s_lampsLit = 0;
long long s_lampsAdded = 0;
} // namespace

void StreetLampDiagCount(bool lit)
{
    ++s_lampsSeen;
    if (lit)
        ++s_lampsLit;
}

void StreetLampDiagReport()
{
    // Edge-triggered, not purely time-throttled. A 5-second throttle silently swallowed the
    // one report that mattered: --test-world-hour applies the clock ~17 ms after the lamps
    // first evaluate, so the re-decision landed inside the throttle window and the log kept
    // showing the stale noon verdict. Always print when the counters move.
    static float next = 0.0f;
    static long long lastSeen = -1, lastLit = -1, lastAdded = -1;
    const bool changed = (s_lampsLit != lastLit) || (s_lampsAdded != lastAdded);
    const float now = Glob.time.toFloat();
    if (!changed && now < next)
        return;
    next = now + 5.0f;
    lastSeen = s_lampsSeen;
    lastLit = s_lampsLit;
    lastAdded = s_lampsAdded;
    LOG_INFO(World, "LAMP-001: {} lamp simulations since start, {} of them switched on, {} lamp lights live in the scene",
             s_lampsSeen, s_lampsLit, s_lampsAdded);
}

void StreetLamp::CreateLight(Matrix4Par pos)
{
    StreetLampDiagCount(_pilotLight);
    // The report used to run HERE, i.e. before the ++s_lampsAdded below, so the first call
    // always printed "0 lights added" even when it was about to add one -- a diagnostic
    // that could not distinguish "did not add" from "has not added yet". Report last.
    const Poseidon::Dev::LampLightSettings& lampSet = Poseidon::Dev::GLampLightSettings();
    if (_light && _lightGeneration != lampSet.generation)
    {
        // The dev lever moved: drop the light so the branch below rebuilds it with the
        // new radius/brightness. Rebuilding rather than mutating keeps this to one code
        // path, and LightPointVisible has no setter for diffuse/ambient anyway.
        _light.Free();
        _spill.Free();
        --s_lampsAdded;
    }
    if (!_light)
    {
        if (_pilotLight && lampSet.enabled)
        {
            Ref<LODShapeWithShadow> shape = GLOB_SCENE->Preloaded(HalfLight);
            // LAMP-004: the lamp's colour. The authored (0.90, 0.80, 0.60) is a warm white;
            // a period street lamp is sodium orange, which is what the owner asked for.
            const Color authored = Type()->_colorDiffuse;
            const auto tint = lampSet.useColorTemperature
                                  ? Poseidon::Dev::LampTemperatureColor(lampSet.colorTemperature)
                                  : std::array<float, 3>{lampSet.color[0], lampSet.color[1], lampSet.color[2]};
            const Color diffuse = (lampSet.colorOverride
                                       ? Color(tint[0], tint[1], tint[2], authored.A())
                                       : authored) *
                                  lampSet.brightnessScale;
            const Color ambient = Type()->_colorAmbient * (lampSet.brightnessScale * lampSet.ambientScale);
            // SetBrightness is really "set radius" on both light kinds: _startAtten = 50 *
            // coef, and the shader is full-brightness INSIDE startAtten and cut at 10x it.
            const float radius = Type()->_brightness * lampSet.radiusScale;
            if (lampSet.cone)
            {
                // A street lamp points at the ground. As a point light it lit the sky, the
                // rooftops and the next village equally -- see LampLightSettings. The spot
                // path already exists end to end (LightReflector -> LTSpotLight ->
                // WgrLight::dir.w -> the `cone` term in lights_contrib); nothing but
                // headlights had ever used it.
                Ref<LightReflector> spot = new LightReflector(shape, diffuse, ambient, H_PI * 0.25f);
                spot->SetBrightness(radius);
                spot->SetPosition(pos.FastTransform(_lightPos));
                spot->SetOrient(Vector3(0, -1, 0), Vector3(0, 0, 1));
                _light = spot;
                if (lampSet.spill > 0.0f)
                {
                    _spill = new LightPointVisible(shape, diffuse * lampSet.spill, ambient * lampSet.spill);
                    _spill->SetBrightness(radius * lampSet.spillRadius);
                    _spill->SetPosition(pos.FastTransform(_lightPos));
                    GLOB_SCENE->AddLight(_spill);
                }
            }
            else
            {
                Ref<LightPointVisible> point = new LightPointVisible(shape, diffuse, ambient);
                point->SetBrightness(radius);
                // LGT-014. radiusScale shrinks the flat CORE, which is the whole point of
                // LAMP-004 -- but reach was 10x the core, so it shrank that too and the
                // lamps went out as the owner backed away ("wenn ich weiter weg bin ... ist
                // das licht nicht mehr zu sehen"). Scale the reach back up by the same
                // factor: the pool stays small, the lamp is still there from 500 m.
                point->SetEndAttenScale(10.0f / std::max(lampSet.radiusScale, 0.02f));
                point->SetPosition(pos.FastTransform(_lightPos));
                _light = point;
                if (lampSet.spill > 0.0f)
                {
                    // LGT-014. The spill used to exist only on the cone path, so with cones
                    // off a lamp was one tight pool and nothing else -- which is the "more
                    // falloff, spill should light the surroundings a bit more" the owner
                    // asked for, missing. A second, dim, WIDE point light at the same place
                    // is the falloff: the bright core stays small (radiusScale) and this one
                    // washes the street and the house fronts around it without lifting them
                    // to floodlight.
                    _spill = new LightPointVisible(shape, diffuse * lampSet.spill,
                                                   ambient * (lampSet.spill * lampSet.spillAmbient));
                    _spill->SetBrightness(radius * lampSet.spillRadius);
                    _spill->SetEndAttenScale(
                        10.0f / std::max(lampSet.radiusScale * lampSet.spillRadius, 0.02f));
                    _spill->SetPosition(pos.FastTransform(_lightPos));
                    GLOB_SCENE->AddLight(_spill);
                }
            }
            // LGT-024 gauge. Every reach argument this session rested on Type()->_brightness
            // and nobody had ever printed it. POSEIDON_LAMP_DIAG=1 prints, once per distinct
            // value, what a lamp's numbers actually come out as -- core radius, start
            // attenuation and the distance past which the shader drops the light entirely.
            {
                static const bool diag = std::getenv("POSEIDON_LAMP_DIAG") != nullptr;
                if (diag)
                {
                    static float lastBrightness = -1.0f;
                    if (std::fabs(Type()->_brightness - lastBrightness) > 1e-4f)
                    {
                        lastBrightness = Type()->_brightness;
                        const float startAtten = 50.0f * radius;
                        const float endScale = 10.0f / std::max(lampSet.radiusScale, 0.02f);
                        LOG_INFO(Graphics,
                                 "LGT-024 lamp: cfg brightness {:.4f} -> radius {:.4f}, startAtten {:.2f} m, cutoff "
                                 "{:.1f} m; spill startAtten {:.2f} m, cutoff {:.1f} m",
                                 Type()->_brightness, radius, startAtten, startAtten * endScale,
                                 startAtten * lampSet.spillRadius,
                                 startAtten * lampSet.spillRadius *
                                     (10.0f / std::max(lampSet.radiusScale * lampSet.spillRadius, 0.02f)));
                    }
                }
            }
            _lightGeneration = lampSet.generation;
            GLOB_SCENE->AddLight(_light);
            ++s_lampsAdded;
        }
    }
    else
    {
        if (!_pilotLight)
        {
            _light.Free();
            _spill.Free();
            --s_lampsAdded;
        }
    }
    StreetLampDiagReport();
}

void StreetLamp::SimulateSwitch()
{
    {
        // LAMP-001: which of the three gates is holding the lamp off. All three have to be
        // in one line: "the bulb is broken", "it is daytime" and "the lamp is destroyed"
        // are indistinguishable from the outside, and the first is the one a missing
        // "lampa" hit selection would produce on every lamp in the world.
        static float next = 0.0f;
        static float lastTod = -1.0f;
        const float now = Glob.time.toFloat();
        const float tod = Glob.clock.GetTimeOfDay();
        if (now >= next || fabs(tod - lastTod) > 0.005f)
        {
            next = now + 5.0f;
            lastTod = tod;
            LOG_INFO(World, "LAMP-001 gate: totalDammage={:.3f} bulbHit={:.3f} bulbPtr={} timeOfDay={:.4f} timeInYear={:.1f} sunY={:.3f} state={}",
                     GetTotalDammage(), GetHit(Type()->_bulbHit), (const void*)&Type()->_bulbHit, Glob.clock.GetTimeOfDay(), Glob.clock.GetTimeInYear(),
                     GLOB_SCENE->MainLight()->SunDirection().Y(),
                     static_cast<int>(_lightState));
        }
    }
    if (GetTotalDammage() < 0.3 && GetHit(Type()->_bulbHit) < 0.5f)
    {
        switch (_lightState)
        {
            case LSOff:
                _pilotLight = false;
                break;
            case LSOn:
                _pilotLight = true;
                break;
            default:
                Fail("Light state");
            case LSAuto:
            {
                float timeOfDay = Glob.clock.GetTimeOfDay();
                _pilotLight = (timeOfDay < 0.3 || timeOfDay > 0.7);
            }
            break;
        }
    }
    else
    {
        _pilotLight = false;
    }
}

void StreetLamp::ResetStatus()
{
    // force update as soon as possible
    _simulationSkipped = _simulationPrecision;
    base::ResetStatus();
}

void StreetLamp::HitBy(EntityAI* killer, float howMuch, RString ammo)
{
    SimulateSwitch();
    CreateLight(*this);
}

void StreetLamp::OnTimeSkipped()
{
    SimulateSwitch();
    CreateLight(*this);
}

void StreetLamp::Simulate(float deltaT, SimulationImportance prec)
{
    SimulateSwitch();
    CreateLight(*this);
    // if( !_object ) _delete=true;
}

LSError StreetLamp::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    PARAM_CHECK(ar.SerializeEnum("lightState", _lightState, 1, LSAuto))
    PARAM_CHECK(ar.Serialize("pilotLight", _pilotLight, 1, false))
    if (ar.IsLoading())
    {
        SimulateSwitch();
        CreateLight(*this);
    }
    return LSOK;
}

DEFINE_FAST_ALLOCATOR(ForestPlain)

DEFINE_CASTING(ForestPlain)

static void InitSkewedShape(LODShape* lShape)
{
    if (lShape->GetOrHints() & ClipLandMask)
    {
        // adjust shape flags so that KeepHeight is not set
        for (int i = 0; i < lShape->NLevels(); i++)
        {
            Shape* shape = lShape->Level(i);
            for (int v = 0; v < shape->NVertex(); v++)
            {
                ClipFlags clip = shape->Clip(v);
                clip &= ~ClipLandMask;
                shape->SetClip(v, clip);
            }
            shape->CalculateHints();
        }
        lShape->AllowAnimation(false);
        lShape->CalculateHints();
        // LOG_DEBUG(Graphics, "InitSkewedShape {}",lShape->Name());
    }
}

ForestPlain::ForestPlain(LODShapeWithShadow* shape, int id) : base(shape, id)
{
    _singleMatrixT1 = false;
    _singleMatrixT2 = false;
    // check - some forest may use single matrix
    const char* name = shape->Name();
    if (strstr(name, "t1"))
    {
        _singleMatrixT1 = true;
        InitSkewedShape(shape);
    }
    else if (strstr(name, "t2"))
    {
        _singleMatrixT2 = true;
        InitSkewedShape(shape);
    }
}

DEFINE_FAST_ALLOCATOR(Forest)

Forest::Forest(BuildingType* type, int id) : base(type->GetShape(), id)
{
    _type = type;
    _type->VehicleAddRef();

#if FOREST_PATHS
    int n = NPos();
    _locks.Resize(n);
    for (int i = 0; i < n; i++)
        _locks[i] = 0;
#endif
}

Forest::~Forest()
{
    _type->VehicleRelease();
}

#define ANIM_FOREST 0

// Compute the (static) bilinear terrain-conform plane for this forest square, matching
// the corner sampling in ForestPlain::Animate. Forests and terrain don't move, so this
// is done once and cached; the wgpu backend evaluates it per instance in the vertex
// shader (see ConformPlane / shader3d.wgsl) instead of the CPU rewriting the shared mesh.
void ForestPlain::ComputeConformPlane()
{
    _conformValid = true;
    _conformPlane = ConformPlane{};
    if (!GLandscape)
    {
        return;
    }
    float xRel = Position().X() * InvLandGrid;
    float zRel = Position().Z() * InvLandGrid;
    int x = toIntFloor(xRel);
    int z = toIntFloor(zRel);

    int subdivLog = TerrainRangeLog - LandRangeLog;
    int subdiv = 1 << subdivLog;
    int xs = x * subdiv;
    int zs = z * subdiv;

    Landscape* land = GLandscape;
    float y00 = land->GetHeight(zs, xs);
    float y01 = land->GetHeight(zs, xs + subdiv);
    float y10 = land->GetHeight(zs + subdiv, xs);
    float y11 = land->GetHeight(zs + subdiv, xs + subdiv);

    _conformPlane.invLandGrid = InvLandGrid;
    _conformPlane.xf = float(x);
    _conformPlane.zf = float(z);
    _conformPlane.y00 = y00;
    _conformPlane.y10 = y10;
    _conformPlane.d1000 = y10 - y00;
    _conformPlane.d0100 = y01 - y00;
    _conformPlane.d1011 = y10 - y11;
    _conformPlane.d0111 = y01 - y11;
    _conformPlane.bias = _shape->BoundingCenter().Y();
}

bool ForestPlain::GpuConformPlane(ConformPlane& out)
{
    // Skewed squares (t1/t2) bake their conform into the object matrix (InitSkew ->
    // SetTransform), so the GPU-driven path draws them rigid with world = Transform().
    if (_singleMatrixT1 || _singleMatrixT2)
    {
        return false;
    }
    if (!_conformValid)
    {
        ComputeConformPlane();
    }
    out = _conformPlane;
    out.mode = 1;
    out.active = true;
    return true;
}

void ForestPlain::Draw(int forceLOD, ClipFlags clipFlags, const FrameBase& pos)
{
    // Skewed forest squares (t1/t2) bake their conform into the object matrix already,
    // so there is nothing to conform on the GPU. Non-skewed squares conform per-vertex:
    // publish the (static) conform plane so the wgpu backend uploads one shared
    // undeformed mesh and conforms per instance in the vertex shader, instead of the CPU
    // rewriting the shared buffer for every instance every frame. The CPU deform in
    // Animate still runs (bounding box, and the GL33 path, which ignores this plane).
    if (_singleMatrixT1 || _singleMatrixT2)
    {
        base::Draw(forceLOD, clipFlags, pos);
        return;
    }
    if (!_conformValid)
    {
        ComputeConformPlane();
    }
    ConformPlane saved = GCurrentConformPlane;
    GCurrentConformPlane = _conformPlane;
    GCurrentConformPlane.active = true;
    base::Draw(forceLOD, clipFlags, pos);
    GCurrentConformPlane = saved;
}

bool ForestPlain::IsAnimated(int level) const
{
    return !(_singleMatrixT1 || _singleMatrixT2);
}
bool ForestPlain::IsAnimatedShadow(int level) const
{
    return false;
}

Matrix4 ForestPlain::GetInvTransform() const
{
    if (!_singleMatrixT1 && !_singleMatrixT2)
    {
        return base::GetInvTransform();
    }
    // matrix contains skew
    return Frame::InverseGeneral();
}

// Forests are skipped by the terrain-relative re-seat (their Y never moves), so this is the only place a
// forest learns the ground changed. Without it a non-skewed square kept the conform plane captured from
// the heights it was first drawn on, and a skewed (t1/t2) square kept the skew baked into its matrix --
// the floating forests (owner report: "floating forests and buildings ... 100 metres or less in the air").
void ForestPlain::OnTerrainChanged(Landscape* land)
{
    base::OnTerrainChanged(land);
    _conformValid = false;
    if ((_singleMatrixT1 || _singleMatrixT2) && land && _skewApplied)
    {
        // InitSkew appends its skew to the CURRENT matrix, so take the old skew off first:
        // S = shear (y += skewX*x + skewZ*z) + offsetY, S^-1 = shear by the negatives - offsetY.
        Matrix4 unskew = MIdentity;
        unskew(1, 0) = -_skewX;
        unskew(1, 2) = -_skewZ;
        Vector3 p = unskew.Position();
        p[1] = -_offsetY;
        unskew.SetPosition(p);
        SetTransform(Transform() * unskew);
        InitSkew(land);
    }
}

void ForestPlain::InitSkew(Landscape* land)
{
    if (!_singleMatrixT1 && !_singleMatrixT2)
    {
        return;
    }

    float xC = Position().X();
    float zC = Position().Z();
    // fine rectangles are not used - use rough instead
    // calculate surface level on given coordinates
    float xRel = xC * InvLandGrid;
    float zRel = zC * InvLandGrid;

    int x = toIntFloor(xRel);
    int z = toIntFloor(zRel);

    int subdivLog = land->GetTerrainRangeLog() - land->GetLandRangeLog();
    int subdiv = 1 << subdivLog;

    int xs = x * subdiv;
    int zs = z * subdiv;

    float y00 = land->GetHeight(zs, xs);
    float y01 = land->GetHeight(zs, xs + subdiv);
    float y10 = land->GetHeight(zs + subdiv, xs);
    float y11 = land->GetHeight(zs + subdiv, xs + subdiv);

    float d1000 = y10 - y00;
    float d0100 = y01 - y00;

    float d1011 = y10 - y11;
    float d0111 = y01 - y11;

    if (_singleMatrixT2)
    {
        // create a skew matrix for T2
        // T1 dy y00+d1000*zIn+d0100*xIn :
        _skewX = d0100 * InvLandGrid;
        _skewZ = d1000 * InvLandGrid;
    }
    else
    {
        // T2 dy y10+d0111-d1011*xIn-zIn*d0111
        // create a skew matrix for T1
        _skewX = -d1011 * InvLandGrid;
        _skewZ = -d0111 * InvLandGrid;
    }

    // calculate (0,0,0) current height
    // model coordinates of original (0,0,0) point are -_shape->BoundingCenter()

    // apply skew settings
    Matrix4 skew = MIdentity;
    skew(1, 0) = _skewX;
    skew(1, 2) = _skewZ;

    // calculate where will be the bounding center transformed
    Vector3 bcT = skew.FastTransform(-_shape->BoundingCenter());
    // calculate world coord. position
    Vector3 bcW = PositionModelToWorld(bcT);

    // calculate surface position using current plane equation
    float xIn = bcW.X() * InvLandGrid - x; // relative 0..1 in square
    float zIn = bcW.Z() * InvLandGrid - z;
    float y = (_singleMatrixT2 ? y00 + d1000 * zIn + d0100 * xIn : y10 + d0111 - d1011 * xIn - zIn * d0111);

    _offsetY = y - bcW.Y();

    Vector3 pos = skew.Position();
    pos[1] += _offsetY;
    skew.SetPosition(pos);

    // append skew matrix to object matrix
    Matrix4 transform = Transform() * skew;
    SetTransform(transform);
    _skewApplied = true;
}

const float ForestViewDensity = -0.024079456087; // log(0.3)*0.02
float ForestPlain::ViewDensity() const
{
    return ForestViewDensity;
}
} // namespace Poseidon

#pragma optimize("t", on) // optimize for speed
namespace Poseidon
{
void ForestPlain::Animate(int level)
{
    Shape* shape = _shape->Level(level);
    if (!shape)
    {
        return;
    }

    PoseidonAssert(GLandscape);
    // save original position
    shape->SaveOriginalPos();

    if (_singleMatrixT1 || _singleMatrixT2)
    {
        // matrix included in object matrix
    }
    else
    {
        float xC = Position().X();
        float zC = Position().Z();
        // fine rectangles are not used - use rough instead
        // calculate surface level on given coordinates
        float xRel = xC * InvLandGrid;
        float zRel = zC * InvLandGrid;
        int x = toIntFloor(xRel);
        int z = toIntFloor(zRel);
        float xf = x;
        float zf = z;

        int subdivLog = TerrainRangeLog - LandRangeLog;
        int subdiv = 1 << subdivLog;

        int xs = x * subdiv;
        int zs = z * subdiv;

        Landscape* land = GLandscape;
        float y00 = land->GetHeight(zs, xs);
        float y01 = land->GetHeight(zs, xs + subdiv);
        float y10 = land->GetHeight(zs + subdiv, xs);
        float y11 = land->GetHeight(zs + subdiv, xs + subdiv);

        float d1000 = y10 - y00;
        float d0100 = y01 - y00;

        float d1011 = y10 - y11;
        float d0111 = y01 - y11;

        Matrix4Val toWorld = Transform();
        Matrix4Val fromWorld = GetInvTransform();

        // most forest objects are not rotated, only offseted - this can be easily optimized
        float yOffset = -_shape->BoundingCenter().Y();

        // In the wgpu color pass a conform plane is published (GCurrentConformPlane is
        // active) and the vertex shader conforms this shared, undeformed mesh per
        // instance — so running the per-vertex CPU deform here is pure per-frame waste
        // (the conform is static). Skip it; the bounding box below is still animated for
        // culling. GL33, and the shadow pass (which publishes no plane), keep deforming.
        if (!GCurrentConformPlane.active)
        {
            shape->InvalidateNormals();
            // convert plane equations to model space; change object shape to reflect surface
            bool rotated = Direction() * VForward < 0.99;
            if (!rotated)
            {
                for (int i = 0; i < shape->NPos(); i++)
                {
                    Vector3Val pos = shape->OrigPos(i);
                    // shape y is relative to surface; calculate world coordinates
                    float yPos = pos[1] - yOffset;
                    Vector3 tPos = pos + Position();

                    float xIn = tPos.X() * InvLandGrid - xf; // relative 0..1 in square
                    float zIn = tPos.Z() * InvLandGrid - zf;
                    float y =
                        (xIn <= 1 - zIn ? y00 + d1000 * zIn + d0100 * xIn : y10 + d0111 - d1011 * xIn - zIn * d0111);

                    V3& dPos = shape->SetPos(i);
                    dPos[1] = y + yPos - Position().Y();
                }
            }
            else
            {
                for (int i = 0; i < shape->NPos(); i++)
                {
                    Vector3Val pos = shape->OrigPos(i);
                    // shape y is relative to surface; calculate world coordinates
                    Vector3 tPos(VFastTransform, toWorld, pos);
                    float xIn = tPos.X() * InvLandGrid - xf; // relative 0..1 in square
                    float zIn = tPos.Z() * InvLandGrid - zf;

                    float yPos = pos[1] - yOffset;
                    float y =
                        (xIn <= 1 - zIn ? y00 + d1000 * zIn + d0100 * xIn : y10 + d0111 - d1011 * xIn - zIn * d0111);
                    tPos[1] = y + yPos;

                    V3& dPos = shape->SetPos(i);
                    dPos.SetFastTransform(fromWorld, tPos);
                }
            }
        }

        // animate bounding box
        // i.e. animate all 8 bbox corners

        Vector3 min(1e10, 1e10, 1e10), max(-1e10, -1e10, -1e10);
        for (int lr = 0; lr < 2; lr++)
        {
            for (int ud = 0; ud < 2; ud++)
            {
                for (int fb = 0; fb < 2; fb++)
                {
                    // assume generic (rotated) case here
                    Vector3 pos = shape->MinMaxOrigCorner(lr, ud, fb);
                    // corner is source model coordinates
                    // convert to animated coordinates
                    Vector3 tPos(VFastTransform, toWorld, pos);
                    float xIn = tPos.X() * InvLandGrid - xf; // relative 0..1 in square
                    float zIn = tPos.Z() * InvLandGrid - zf;
                    //
                    float yPos = pos[1] - yOffset;
                    float y =
                        (xIn <= 1 - zIn ? y00 + d1000 * zIn + d0100 * xIn : y10 + d0111 - d1011 * xIn - zIn * d0111);
                    tPos[1] = y + yPos;

                    Vector3 dPos(VFastTransform, fromWorld, tPos);
                    CheckMinMax(min, max, dPos);
                }
            }
        }
        // calculate bsphere and bcenter estimation
        Vector3 bCenter = (min + max) * 0.5;
        // calculate bradius
        float bRadius = min.Distance(bCenter);
        shape->SetMinMax(min, max, bCenter, bRadius);
    }
}

Vector3 ForestPlain::AnimatePoint(int level, int index) const
{
    if (_singleMatrixT1 || _singleMatrixT2)
    {
        Shape* shape = _shape->LevelOpaque(level);
        // return original position
        return PositionModelToWorld(shape->Pos(index));
    }
    else
    {
        return base::AnimatePoint(level, index);
    }
}
} // namespace Poseidon

#pragma optimize("", on) // default optimization
namespace Poseidon
{
void ForestPlain::Deanimate(int level)
{
    // base::Deanimate(level);
}

RoadType::RoadType() = default;

// WLD-026 — under a backend that conforms terrain on the GPU, a road must be flagged
// `IsOnSurface`, NOT `OnSurface`. The two names look interchangeable and are not:
// ModelFlags.hpp:183 states the contract exactly — "IsOnSurface = confirmed on surface
// (OnSurface = should be placed on surface)". `OnSurface` is a REQUEST for conforming that
// has not happened yet; `IsOnSurface` says the geometry already sits on the ground and only
// needs the decal routing (polygon offset, no z-write, drawn in the on-surface pass).
// `render::IsOnSurfaceRouting` (RenderFlags.hpp:220) accepts either, so every routing,
// blending and bias decision downstream is unchanged. What changes is one thing:
//
//   ShapeDraw.cpp:503 — `if (level->Special() & (IsLight | OnSurface)) optimizeHW = false;`
//
// An `OnSurface` shape is DENIED a vertex buffer at load time. That was right in 2001, when
// OnSurface geometry was re-split against the terrain every frame and a static buffer would
// have been worthless. It is wrong here, and the consequences cascade:
//
//   1. No `_buffer` fails Shape::Draw's T&L gate (`GetTL() && EnableHWTLState && tlAble &&
//      _buffer`, ShapeDraw.cpp:198), so no road ever reaches DrawSectionTL.
//   2. It lands in FaceArray::Draw instead, which re-splits it per terrain triangle EVERY
//      FRAME (ClipShape.cpp:235-242, still unconditional on `spec & OnSurface`) and hands
//      CPU-lit screen-space vertices to EngineWgpu::DrawSection -> AppendTriangles: the 2D
//      pipeline. gfx2d/shader.wgsl is `textureSample * in.color` mixed toward fog — no sun,
//      no sky irradiance, no sRGB decode, no HDR handling.
//   3. So none of the road work done in gfx3d/shader3d.wgsl is reachable by a road: not the
//      mode-2 per-vertex ClipLand conform (which is what `GGpuTerrainConform` promises and
//      what Object::Draw skips the CPU split for), not the ndc^2 decal bias, not the
//      emissive_night override. Each was written for roads and none of them runs on one.
//
// That accounts for all three reported symptoms from one cause: untextured/white by day
// (gamma-space CPU colour written straight into a linear HDR target, with slot-0 white for
// any texture that fails to resolve), self-lit at night (the same colour carries no sun
// term), and — the see-through-mountains one — a depth value produced by a DIFFERENT
// projection from the one the rest of the scene uses. EngineWgpu::PushSceneCamera overrides
// the depth row to an infinite-far reversed-Z form (`_33=1, _43=-cNear`) while the CPU T&L
// path keeps `Camera::Projection()`'s finite `q = ccFar/(ccFar-cNear)` form; the gap is then
// papered over with a constant software z-bias (`GetZCoefs`, 16x GL33's). A constant offset
// in reversed-Z depth is a distance-QUADRATIC error in world space: measured at cNear=0.079,
// fog 3000 m, the road is depth-tested as though it stood at 608 m when it is at 1000 m, and
// at 874 m when it is at 2000 m. Anything in between cannot occlude it.
//
// Setting `IsOnSurface` takes roads off that path entirely and onto the normal 3D one, where
// the GPU conform pins each ClipLandOn vertex to the terrain in the vertex shader.
//
// SCOPE. Returns false whenever `GGpuTerrainConform` is false, so GL33 — and wgpu with
// `WGR_GPU_CONFORM=0` — keep the established CPU-split behaviour bit for bit. The escape
// hatch is `WGR_ROAD_LEGACY_SURFACE=1`, which restores the old flags exactly; the fixed
// behaviour is the default, matching WGR_SHADOW_LEGACY_GATE's convention.
static bool RoadConformsOnGpu()
{
    if (!GGpuTerrainConform)
    {
        return false;
    }
    static const bool legacy = []
    {
        const char* v = std::getenv("WGR_ROAD_LEGACY_SURFACE");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return !legacy;
}

RoadType::RoadType(const char* name)
{
    _shape = Shapes.New(name, false, true);
    // scan shape for selections to hide
    Shape* geom = _shape->GeometryLevel();
    if (geom && geom->NFaces() > 0)
    {
        // some shapes (bridges) should not be land-following
        //
        // WLD-026: this bail-out gives the model NOTHING — no OnSurface/IsOnSurface, no
        // NoZWrite, no SetCanBeOccluded(false), and not the ClipLand normalisation below.
        // It was suspected of being the reason DayZ rail tracks band, on the theory that
        // collidable rail ballast would be classed a road and then silently skipped. The
        // census says otherwise and the theory is dead: of 90 DayZ rail models, ZERO carry
        // the `road` property class, so a rail never reaches RoadType at all (it would also
        // bail here, since 90/90 do have a non-empty Geometry LOD). Rails are ordinary
        // alpha-cutout objects; whatever bands them is not on this path.
        //
        // The bail-out is nonetheless a live set worth being able to see: 12 DayZ models,
        // 4 Arma 1 models and 3 Arma 2 models ARE `class=road` with a non-empty Geometry LOD
        // and land here, keeping only whatever flags their P3D set. (Operation Arrowhead's
        // roads_e.pbo has none — 198/198 of its road-classed models take the else branch.)
        // Logged at INFO with the geometry face count so that set can be enumerated from a
        // normal boot log instead of guessed at.
        LOG_INFO(Graphics, "Road with geometry: {} ({} geometry faces), no road flags applied", name, geom->NFaces());
    }
    else
    {
        if ((_shape->Special() & OnSurface) == 0)
        {
            LOG_DEBUG(Graphics, "Missing OnSurface {}", name);
        }
        const bool gpuConform = RoadConformsOnGpu();
        _shape->OrSpecial(NoShadow | NoZWrite | (gpuConform ? IsOnSurface : OnSurface));
        if (gpuConform)
        {
            // OR-ing IsOnSurface is not enough. Road P3Ds normally set OnSurface themselves —
            // that is precisely what the LOG_DEBUG above tests for, and it fires rarely — so
            // the model's own bit has to be cleared or the shape is still denied its vertex
            // buffer and nothing above changes.
            _shape->AndSpecial(~OnSurface);
        }
        // no occlusions on roads
        _shape->SetCanBeOccluded(false);
        for_each_alpha for (int level = 0; level < _shape->NLevels(); level++)
        {
            Shape* shape = _shape->Level(level);
            if (!shape)
            {
                continue;
            }
            for (int i = 0; i < shape->NVertex(); i++)
            {
                // REPLACE the land nibble; do not OR into it. Object::Draw forks roads on
                // `(andHints & ClipLandMask) == ClipLandOn` being EXACT: one vertex still
                // carrying ClipLandKeep/Under/Above turns that AND into 0x900 and sends the
                // whole model down the other branch, and the two branches read Shape::Norm()
                // with OPPOSITE sign conventions -- the GPU path negates it (MeshBuild.cpp:44),
                // the CPU per-vertex path does not (TransLight.cpp:548).
                //
                // CORRECTION, WLD-026, from a census of 916 road models across four
                // generations (A1 roads.pbo rev 40; A2 Roads2.pbo rev 48; OA roads_e.pbo rev
                // 49 -- the exact Takistan corpus; OA roads_pmc rev 50; DayZ structures_rail
                // and structures_roads rev 54):
                //
                //   * NO model in ANY corpus mixes land bits. Every non-empty LOD stores its
                //     clip array as a condensed fill -- ONE clip word repeated across every
                //     vertex -- so a mixed nibble is not representable in the shipped data.
                //     The only values that occur anywhere are 0x3f (ClipAll), 0x13f
                //     (ClipAll|ClipLandOn) and 0x83f (ClipAll|ClipLandKeep, one model).
                //   * For roads_e.pbo specifically, 196/210 models are ALREADY pure
                //     ClipLandOn before the engine touches them, and the AND-of-hints is
                //     already exactly ClipLandOn on 198/210.
                //
                // So the older comment here -- "Arma 2 road parts mix land bits, which is why
                // this is A2-specific" -- is FALSE, and this rewrite is a measured no-op on
                // every road model that ships. It is kept as a cheap invariant (it costs one
                // pass over ~10 vertices per model and guarantees the exact-match fork above
                // for hand-made or converted content), not because it fixes anything, and it
                // is NOT the cause of the alternating light/dark "piano key" pattern. That
                // pattern was traced elsewhere -- see WLD-021 and the RoadConformsOnGpu
                // comment above.
                shape->SetClip(i, (shape->Clip(i) & ~ClipLandMask) | ClipLandOn);
            }
            shape->CalculateHints();
            if (gpuConform)
            {
                // Snapshot the undeformed geometry AFTER the clip normalisation above.
                // `MeshBuild::BuildOrigVertices` derives the per-vertex conform selector from
                // `OrigClip`, not `Clip`, and it is the only builder that writes a non-zero
                // selector — so without this the road uploads with conform_sel = 0 on every
                // vertex, the vertex shader's mode-2 branch does nothing, and the road draws
                // rigid at its authored height instead of pinned to the ground. Nothing else
                // saves it for a road: Object::Animate returns immediately on an all-ClipLandOn
                // shape ("will be done during SurfaceSplit", Object.cpp:374) without reaching
                // the SaveOriginalPos in the branch below it. The retained path already does
                // exactly this at its own registration site (EngineWgpu.cpp:4297) for the same
                // reason. Idempotent.
                shape->SaveOriginalPos();
            }
        }
        if (gpuConform)
        {
            // The conformed vertex data has to reach the GPU, and for a STATIC buffer it never
            // would. VertexBufferWgpu::Update early-outs on `!bufferDirty && (!isDynamic ||
            // haveUploadHash)`, and a road never dirties its buffer (its CPU deform is the one
            // Object::Animate skips), so a VBStatic road would keep the conform_sel = 0 bytes
            // that EngineWgpu::CreateVertexBuffer writes at creation — when no conform plane is
            // active and BuildVertices is therefore the only builder it can use. Marking the
            // shape animatable makes ShapeDraw.cpp:533 choose VBDynamic, whose FIRST Update runs
            // (haveUploadHash is false) inside BeginMeshTL with the object's conform plane
            // published, takes the BuildOrigVertices arm, and uploads the selector. From the
            // second frame on the early-out applies again, so this costs exactly one upload of a
            // ~10-vertex mesh per road MODEL, not per placement.
            //
            // Contained: `GetAllowAnimation` is read in exactly two places, both in
            // ShapeDraw.cpp — the buffer type above, and `optimizeSSE`, which additionally
            // requires >= 16 vertices and only selects a CPU quad path this road no longer
            // takes. Object::IsAnimated does not consult it.
            _shape->AllowAnimation(true);
            // Rebuild the buffers, because whatever exists was built under the wrong flags.
            //
            // During world load ShapeBank has not run its bulk OptimizeAll yet
            // (`_bulkOptimizeDone` false), so ShapeBank::New skipped per-shape optimization and
            // no level has a buffer: the release below is a no-op, this builds them once, and
            // the bulk pass — which runs after every RoadType is constructed — releases and
            // rebuilds them once more. Bounded and cheap: road segment models are tiny (a
            // typical Takistan piece is 10 vertices / 8 faces) over ~122 models on that world.
            //
            // A road loaded AFTER the bulk pass is the trap WLD-021 recorded: ShapeBank::New
            // calls OptimizeOneShape immediately, i.e. BEFORE this constructor runs, so its
            // buffer was decided by the P3D's own flags — absent if the model set OnSurface
            // itself, and VBStatic (no conform selector, ever) if it did not. Releasing first
            // makes both cases converge, and satisfies Shape::ConvertToVBuffer's
            // `DoAssert(!_buffer)`.
            for (int level = 0; level < _shape->NLevels(); level++)
            {
                if (Shape* shape = _shape->Level(level))
                {
                    shape->ReleaseVBuffer();
                }
            }
            Shapes.OptimizeOneShape(_shape);
        }
    }
}

RoadType::~RoadType() = default;

DEFINE_FAST_ALLOCATOR(Road)
DEFINE_CASTING(Road)

Road::Road(LODShapeWithShadow* shape, int id) : Object(shape, id)
{
    _roadType = RoadTypes.New(shape->Name());
    SetType(Network);
    if (shape->GeometryLevel())
    {
        SetDestructType(DestructBuilding);
    }
    else
    {
        SetDestructType(DestructNo);
    }
}

void Road::DrawDiags()
{
    // draw star on position of
    // GScene->DrawCollisionStar(Position(),3);
}

bool Road::IsAnimated(int level) const
{
    if (GetDestructType() == DestructNo)
    {
        return false;
    }
    return base::IsAnimated(level);
}
bool Road::IsAnimatedShadow(int level) const
{
    if (GetDestructType() == DestructNo)
    {
        return false;
    }
    return base::IsAnimatedShadow(level);
}

const float roadArmor = 1200;
const float roadInvArmor = 1 / roadArmor;
const float roadLogArmor = log(roadArmor);

float Road::GetArmor() const
{
    return roadArmor;
}
float Road::GetInvArmor() const
{
    return roadInvArmor;
}
float Road::GetLogArmor() const
{
    return roadLogArmor;
}

DEF_RSB(forest);
DEF_RSB(road);
DEF_RSB(streetlamp);
DEF_RSB(house);
DEF_RSB(vehicle);
DEF_RSB(church);

Object* NewObject(LODShapeWithShadow* shape, int id)
{
    const RStringB& className = shape->GetPropertyClass();
    if (className.GetLength() > 0)
    {
        if (className == RSB(forest))
        {
            BuildingType* type = dynamic_cast<BuildingType*>(VehicleTypes.FindShape(shape->Name()));
            if (type)
            {
                type->VehicleAddRef();
                Object* obj = new Forest(type, id);
                type->VehicleRelease();
                return obj;
            }
            // RptF("%s: no forest in config",(const char *)shape->Name());
            return new ForestPlain(shape, id);
        }
        else if (className == RSB(road))
        {
            return new Road(shape, id);
        }
        else if (className == RSB(streetlamp))
        {
            // EntityType *vehType = VehicleTypes.New("StreetLamp");
            EntityType* vehType = VehicleTypes.FindShapeAndSimulation(shape->Name(), className);
            if (!vehType)
            {
                vehType = VehicleTypes.FindShape(shape->Name());
            }
            if (!vehType)
            {
                LOG_DEBUG(Graphics, "{}: {}, config class missing", shape->Name(), (const char*)className);
                return new ObjectPlain(shape, id);
            }
            StreetLampType* type = dynamic_cast<StreetLampType*>(vehType);
            if (!type)
            {
                LOG_DEBUG(Graphics, "{}: {}, config class not StreetLamp", shape->Name(), (const char*)className);
                return new ObjectPlain(shape, id);
            }
            return new StreetLamp(shape, type, id);
        }
        else if (className == RSB(house) || className == RSB(vehicle) || className == RSB(church))
        {
            // search vehicle type bank for given shape
            // prefer type with the same simulation
            EntityType* vType = VehicleTypes.FindShapeAndSimulation(shape->Name(), className);
            if (!vType)
            {
                // if not found, resort to any type with this shape
                vType = VehicleTypes.FindShape(shape->Name());
            }
            if (!vType)
            {
                // it is not in config: ignore it
                LOG_DEBUG(Graphics, "{}: {}, config class missing", shape->Name(), (const char*)className);
                return new ObjectPlain(shape, id);
            }
            EntityAIType* type = dynamic_cast<EntityAIType*>(vType);
            if (!type)
            {
                Fail("Non-ai EntityAIType");
                return new ObjectPlain(shape, id);
            }
            RString sim = type->_simName;
            if (sim.GetLength() <= 0)
            {
                LOG_ERROR(Graphics, "No simulation: {}", (const char*)type->GetName());
                return new ObjectPlain(shape, id);
            }
            type->VehicleAddRef();
            Building* building = nullptr;
            // else if( !strcmpi(sim,"flag") ) building=new Flag(type,id);
            if (!strcmpi(sim, "house"))
            {
                building = new Building(type, id, shape);
            }
            else if (!strcmpi(sim, "church"))
            {
                building = new Church(type, id, shape);
            }
            else if (!strcmpi(sim, "fountain"))
            {
                building = new Fountain(type, id, shape);
            }
            else
            {
                LOG_ERROR(Graphics, "Unknown sim class {}", (const char*)sim);
                building = new Building(type, id, shape);
            }
            type->VehicleRelease();
            if (building)
            {
                return building;
            }
        }
        else
        {
            Log("Unknown object class '%s'", (const char*)className);
        }
    }
    return new ObjectPlain(shape, id);
}

} // namespace Poseidon
Object* NewProxyObject(RString shapeName)
{
    using namespace Poseidon;
    Ref<LODShapeWithShadow> shape = Shapes.New(GetShapeName(shapeName), false, true);
    return NewObject(shape, -1);
}
namespace Poseidon
{

RoadTypeBank RoadTypes;
} // namespace Poseidon
