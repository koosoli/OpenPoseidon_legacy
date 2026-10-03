#include <Poseidon/Core/Global.hpp>

#include <Poseidon/World/Entities/Vehicles/Air/Helicopter.hpp>
#include <Poseidon/World/Entities/Vehicles/Air/FlightCeiling.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/Core/Config/UserConfig.hpp>
#include <Poseidon/AI/AI.hpp>
#include <Poseidon/World/Entities/Weapons/Shots.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Weather/SnowField.hpp>
#include <Poseidon/World/Weather/AirflowField.hpp>
#include <Poseidon/World/Weather/RotorGroundPolicy.hpp>
#include <Poseidon/World/Weather/RotorLandDrawReceipt.hpp>
#include <Poseidon/World/Weather/RainWaterField.hpp>
#include <Poseidon/World/Weather/MudField.hpp>
#include <Poseidon/World/Weather/SandField.hpp>
#include <Poseidon/World/Weather/RainVolume.hpp>
#include <Poseidon/World/Entities/Infantry/UniformWetness.hpp>
#include <Poseidon/Graphics/Rendering/Effects/Smokes.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/UI/Locale/StringtableExt.hpp>
#include <Poseidon/Graphics/Textures/TexturePreload.hpp>
#include <Poseidon/Dev/Diag/DiagModes.hpp>

#include <Poseidon/Graphics/Core/TLVertex.hpp>

#include <Poseidon/Game/UiActions.hpp>

#include <Random/randomGen.hpp>

#include <Poseidon/Network/Network.hpp>

#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/Rendering/Draw/SpecLods.hpp>

#include <Poseidon/Foundation/Enums/EnumNames.hpp>

#include <Poseidon/Foundation/Strings/Mbcs.hpp>
#include <stdint.h>
#include <stdio.h>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <unordered_map>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>
#include <Poseidon/Foundation/Math/MathOpt.hpp>

#if _ENABLE_CHEATS
#define ARROWS 1
#else
#define ARROWS 0
#endif

namespace Poseidon
{
Helicopter::Helicopter(VehicleType* name, Person* pilot)
    : base(name, pilot),

      _rotorSpeed(0), _rotorSpeedWanted(0), // turning motor on/off
      _rotorPosition(0), _mainRotor(0), _mainRotorWanted(0), _backRotor(0), _backRotorWanted(0),

      _cyclicAside(0), _cyclicAsideWanted(0), _cyclicForward(0), _cyclicForwardWanted(0), _rotorDive(0),
      _rotorDiveWanted(0),

      _lastAngVelocity(VZero), _turbulence(VZero), _lastTurbulenceTime(Glob.time),

      _rndFrequency(1 - GRandGen.RandomValue() * 0.05), // do not use same sound frequency
      _missileLRToggle(false), _rocketLRToggle(false)

{
    _head.SetPars("Air");
    _head.Init(Type()->_pilotPos - Vector3(0, 0.2, 0), Type()->_pilotPos, this);
    if (pilot && QIsManual(pilot->Brain()))
    {
        EngineOn();
    }
    SetSimulationPrecision(1.0 / 15);

    _mGunClouds.Load((*Type()->_par) >> "MGunClouds");

    _mGunFireFrames = 0;
    _mGunFireTime = UITIME_MIN;
    _mGunFirePhase = 0;
}

Helicopter::~Helicopter() = default;

HelicopterType::HelicopterType(const ParamEntry* param) : base(param)
{
    _scopeLevel = 1;

    _pilotPos = VZero;

    _hudPosition = VZero;
    _hudRight = VZero;
    _hudDown = VZero;
}

void HelicopterType::Load(const ParamEntry& par)
{
    base::Load(par);

    _turret.Load(par >> "Turret");

    _enableSweep = par >> "enableSweep";

    _mainRotorSpeed = par >> "mainRotorSpeed";
    _backRotorSpeed = par >> "backRotorSpeed";

    _minMainRotorDive = float(par >> "minMainRotorDive") * (H_PI / 180);
    _maxMainRotorDive = float(par >> "maxMainRotorDive") * (H_PI / 180);
    _neutralMainRotorDive = float(par >> "neutralMainRotorDive") * (H_PI / 180);

    _minBackRotorDive = float(par >> "minBackRotorDive") * (H_PI / 180);
    _maxBackRotorDive = float(par >> "maxBackRotorDive") * (H_PI / 180);
    _neutralBackRotorDive = float(par >> "neutralBackRotorDive") * (H_PI / 180);
}

void HelicopterType::InitShape()
{
    const ParamEntry& par = *_par;

    _scopeLevel = 2;
    base::InitShape();

    // The alt-name slot carries the Arma 3 spelling of each rotor selection.
    // A2 kept the Czech names (the Mi-35 resolves on the first name alone);
    // A3 renamed them wholesale, which is why a bridged MH-9's rotors sat
    // still -- not one of these six lookups found anything. Measured on
    // heli_light_01_f.p3d: "main rotor static", "main rotor blur", "tail
    // rotor", "tail rotor static", "tail rotor blur" all present. There is
    // deliberately NO alias for "velka vrtule" itself: A3 has no single
    // main-rotor selection, only five per-blade PROXIES (bladem01..05), so
    // its still blades cannot turn until proxy animation exists. The blur
    // disc is what shows at flight RPM, and that now swaps correctly.
    _rotorH.Init(_shape, "velka vrtule", nullptr, "velka osa");
    _rotorV.Init(_shape, "mala vrtule", "tail rotor", "mala osa");
    _hRotorStill.Init(_shape, "velka vrtule staticka", "main rotor static");
    _hRotorMove.Init(_shape, "velka vrtule blur", "main rotor blur");
    _vRotorStill.Init(_shape, "mala vrtule staticka", "tail rotor static");
    _vRotorMove.Init(_shape, "mala vrtule blur", "tail rotor blur");
    _missileLPos = _shape->MemoryPoint("L strela");
    _missileRPos = _shape->MemoryPoint("P strela");
    _rocketLPos = _shape->MemoryPoint("L raketa");
    _rocketRPos = _shape->MemoryPoint("P raketa");
    _mainRotorDiveAxis = _shape->MemoryPoint("predni osa naklonu");
    _backRotorDiveAxis = _shape->MemoryPoint("zadni osa naklonu");

    // The disc-tilt pivots, recovered when the model does not name them. Every
    // helicopter tilts its rotor disc by `neutralMainRotorDive` even standing
    // still, and Animate() rotates the whole rotor selection about this point
    // -- so a zero here is not "no tilt", it is "tilt about the MODEL ORIGIN",
    // which swings a rotor sitting metres forward and up right out of place.
    // Measured on the line-up: the A1 UH-60 names it (-0.13, 0.77, 1.78, i.e.
    // its hub); the A2 Mi-35 and the A3 MH-9 name nothing, because from Arma 2
    // on the tilt is a model.cfg bone and the memory point is gone. That is
    // the owner's "A2 heli rotor spinning wrong". The rotor's own rotation
    // centre is the same point the UH-60 spells out (hub Z 1.76 vs 1.78), so
    // use it -- and leave models that DO name the point exactly as they were.
    if (_mainRotorDiveAxis.SquareSize() < 1e-6f)
    {
        _mainRotorDiveAxis = _rotorH.Center();
    }
    if (_backRotorDiveAxis.SquareSize() < 1e-6f)
    {
        _backRotorDiveAxis = _rotorV.Center();
    }
    // One line per helicopter type saying which of the six rotor lookups
    // actually resolved, and where the main rotor ended up pivoting. Every
    // cross-generation rotor bug so far has been one of these six coming back
    // empty on a model whose blades are plainly there, and until this line
    // existed the only way to tell was to fly the thing.
    {
        auto levels = [this](const Animation& a) {
            int n = 0;
            for (int i = 0; i < _shape->NLevels(); i++)
                if (a.GetSelection(i) >= 0)
                    ++n;
            return n;
        };
        // Vertex count on the highest-detail LOD that resolved. A main rotor
        // reading back a couple of dozen vertices is a bare hub, not a rotor.
        auto verts = [this](const Animation& a) {
            for (int i = 0; i < _shape->NLevels(); i++)
                if (a.GetSelection(i) >= 0)
                    return _shape->Level(i)->NamedSel(a.GetSelection(i)).Size();
            return 0;
        };
        const Vector3 hc = _rotorH.Center(), hd = _rotorH.Direction();
        const Vector3 vc = _rotorV.Center(), vd = _rotorV.Direction();
        LOG_DEBUG(Graphics,
                  "Rotors on {}: main {}/{} lods, {} verts, about {:.2f},{:.2f},{:.2f} axis {:.2f},{:.2f},{:.2f} | "
                  "tail {}/{} lods, {} verts, about {:.2f},{:.2f},{:.2f} axis {:.2f},{:.2f},{:.2f} | "
                  "still/blur main {}/{} tail {}/{} | dive axes main {:.2f},{:.2f},{:.2f} tail {:.2f},{:.2f},{:.2f}",
                  (const char*)_shape->GetName(), levels(_rotorH), _shape->NLevels(), verts(_rotorH), hc.X(), hc.Y(),
                  hc.Z(), hd.X(), hd.Y(), hd.Z(), levels(_rotorV), _shape->NLevels(), verts(_rotorV), vc.X(), vc.Y(),
                  vc.Z(), vd.X(), vd.Y(), vd.Z(), levels(_hRotorStill), levels(_hRotorMove), levels(_vRotorStill),
                  levels(_vRotorMove), _mainRotorDiveAxis.X(), _mainRotorDiveAxis.Y(), _mainRotorDiveAxis.Z(),
                  _backRotorDiveAxis.X(), _backRotorDiveAxis.Y(), _backRotorDiveAxis.Z());
    }

    _altRadarIndicator.Init(_shape, par >> "IndicatorAltRadar");
    _altBaroIndicator.Init(_shape, par >> "IndicatorAltBaro");
    _speedIndicator.Init(_shape, par >> "IndicatorSpeed");
    _vertSpeedIndicator.Init(_shape, par >> "IndicatorVertSpeed");
    _rpmIndicator.Init(_shape, par >> "IndicatorRPM");
    _compass.Init(_shape, par >> "IndicatorCompass");
    _watch.Init(_shape, par >> "IndicatorWatch");

    _vario.Init(_shape, "horizont", nullptr, "osa_horizont", nullptr);
    for (int i = 0; i < _shape->NLevels(); i++)
    {
        int sel = _vario.GetSelection(i);
        int selCenter = _vario.GetCenterSelection(i);
        if (sel >= 0 && selCenter >= 0)
        {
            Shape* shape = _shape->Level(i);
            _varioDirection[i] = _vario.Center(i) - shape->CalculateCenter(shape->NamedSel(sel));
        }
        else
        {
            _varioDirection[i] = VForward;
        }
    }

    _altRadarIndicator2.Init(_shape, par >> "IndicatorAltRadar2");
    _altBaroIndicator2.Init(_shape, par >> "IndicatorAltBaro2");
    _speedIndicator2.Init(_shape, par >> "IndicatorSpeed2");
    _vertSpeedIndicator2.Init(_shape, par >> "IndicatorVertSpeed2");
    _rpmIndicator2.Init(_shape, par >> "IndicatorRPM2");
    _compass2.Init(_shape, par >> "IndicatorCompass2");
    _watch2.Init(_shape, par >> "IndicatorWatch2");

    _vario2.Init(_shape, "horizont2", nullptr, "osa_horizont2", nullptr);
    for (int i = 0; i < _shape->NLevels(); i++)
    {
        int sel = _vario2.GetSelection(i);
        int selCenter = _vario2.GetCenterSelection(i);
        if (sel >= 0 && selCenter >= 0)
        {
            Shape* shape = _shape->Level(i);
            _vario2Direction[i] = _vario2.Center(i) - shape->CalculateCenter(shape->NamedSel(sel));
        }
        else
        {
            _vario2Direction[i] = VForward;
        }
    }

    _hudPosition = _shape->MemoryPoint("HUD LH");
    _hudRight = _shape->MemoryPoint("HUD PH") - _hudPosition;
    _hudDown = _shape->MemoryPoint("HUD LD") - _hudPosition;

    // turret animations
    _turret.InitShape(par >> "Turret", _shape);

    _gunDir = _turret._dir;
    _gunPos = _turret._pos;

    int level = _shape->FindLevel(VIEW_GUNNER);
    if (level >= 0)
    {
        _shape->LevelOpaque(level)->MakeCockpit();
    }

    level = _shape->FindLevel(VIEW_PILOT);
    if (level >= 0)
    {
        _shape->LevelOpaque(level)->MakeCockpit();
        _pilotPos = _shape->LevelOpaque(level)->NamedPosition("pilot");
    }

    level = _shape->FindLevel(VIEW_CARGO);
    if (level < 0)
    {
        level = _shape->FindLevel(VIEW_PILOT);
    }
    if (level >= 0)
    {
        _shape->LevelOpaque(level)->MakeCockpit();
    }

    if (_pilotPos.SquareSize() < 0.1)
    {
        _pilotPos = _shape->MemoryPoint("pilot");
    }

    _animFire.Init(_shape, "zasleh", nullptr);

    DEF_HIT(_shape, _hullHit, "trup", nullptr, GetArmor() * (float)(par >> "armorHull"));
    DEF_HIT(_shape, _engineHit, "motor", nullptr, GetArmor() * (float)(par >> "armorEngine"));
    DEF_HIT(_shape, _avionicsHit, "elektronika", nullptr, GetArmor() * (float)(par >> "armorAvionics"));
    DEF_HIT(_shape, _rotorVHit, "mala vrtule", "tail rotor", GetArmor() * (float)(par >> "armorVRotor"));
    DEF_HIT(_shape, _rotorHHit, "velka vrtule", nullptr, GetArmor() * (float)(par >> "armorHRotor"));
    DEF_HIT(_shape, _missiles, "munice", nullptr, GetArmor() * (float)(par >> "armorMissiles"));

    DEF_HIT(_shape, _glassRHit, "sklo predni P", nullptr, GetArmor() * (float)(par >> "armorGlass"));
    DEF_HIT(_shape, _glassLHit, "sklo predni L", nullptr, GetArmor() * (float)(par >> "armorGlass"));
    // attach hitpoint to convex component corresponding to selection "sklo"
    FindArray<int> hitsL, hitsR;
    _shape->FindHitComponents(hitsR, "sklo predni P");
    _shape->FindHitComponents(hitsL, "sklo predni L");
    _glassRHit.SetIndexCC(hitsR);
    _glassLHit.SetIndexCC(hitsL);

    {
        WoundInfo dammageInfo;
        dammageInfo.LoadAndRegister(_shape, par >> "dammageHalf");
        _glassDammageHalf.Init(_shape, dammageInfo, nullptr, nullptr);
    }
    {
        WoundInfo dammageInfo;
        dammageInfo.LoadAndRegister(_shape, par >> "dammageFull");
        _glassDammageFull.Init(_shape, dammageInfo, nullptr, nullptr);
    }
}

static Vector3 BodyFriction(Vector3Val speed, float rotorEff, bool large)
{
    Vector3 friction;
    friction.Init();
    float stabXY = rotorEff * 0.9 + 0.1;
    friction[0] = speed[0] * fabs(speed[0]) * 25 + speed[0] * 1500 + fSign(speed[0]) * 40;
    if (!large)
    {
        friction[1] = speed[1] * fabs(speed[1]) * 3 + speed[1] * 500 + fSign(speed[1]) * 15;
        friction[2] = speed[2] * fabs(speed[2]) * 1 + speed[2] * 100 + fSign(speed[2]) * 6;
    }
    else
    {
        friction[1] = speed[1] * fabs(speed[1]) * 4 + speed[1] * 500 + fSign(speed[1]) * 15;
        friction[2] = (speed[2] * fabs(speed[2]) * 1 + speed[2] * 100 + fSign(speed[2]) * 6 +
                       speed[2] * speed[2] * speed[2] * 0.05f);
    }
    friction[0] *= stabXY;
    friction[1] *= stabXY;
    return friction;
}

static Vector3 RotorUpForce(Vector3Val speed, float coef, float alt)
{
    Vector3 force(VZero);
    float spd1 = speed[1] + 6 - 36 * coef;
    saturateMax(spd1, -5);
    force[1] += speed[0] * speed[0] * -4 + fabs(speed[0]) * 660;
    force[1] += spd1 * fabs(spd1) * -400 + spd1 * -14000;
    force[1] += speed[2] * speed[2] * -4 + fabs(speed[2]) * 660;
    saturateMax(force[1], -7000);

    /**/
    float fwdCoef = speed[2] * (1.0 / 20);
    saturate(fwdCoef, -1, 1);
    force[2] += force[1] * 0.1 * fwdCoef;
    /**/
    // AIR-010: limit rotor forces in high altitude. Was a straight line from
    // 1000 m to zero at 3000 m, and negative above that -- a rotor that pushed
    // DOWN, harder the faster it spun. Now an exponential falloff with a positive
    // floor; see FlightCeiling.hpp.
    force *= Air::HeliAirCoef(alt);
    return force;
}

#define FAST_COEF (1.0 / 25) // use fast/slow simulation mode

void Helicopter::SimulateOptimized(float deltaT, SimulationImportance prec)
{
    // PERF-034: the render history covers one world tick, not the four ticks
    // between 15 Hz body updates. Keep far helicopter motion/rotors on the
    // remainder path so that history has a new pose every tick. Do not raise
    // the rate of the hundreds of infantry sharing the visible-far list.
    // WorstImportance clamps helicopters to VisibleFar even beyond view range.
    static const bool smoothFar = []
    {
        const char* value = std::getenv("POSEIDON_HELI_FAR_FULL_RATE");
        return value == nullptr || value[0] != '0';
    }();
    if (prec == SimulateVisibleFar && smoothFar)
    {
        SimulateRest(deltaT, prec);
        return;
    }
    base::SimulateOptimized(deltaT, prec);
}

namespace
{
bool LandRotorBoundsClear(Vector3Par origin, Vector3Par aircraft, Object* ignore)
{
    int xMin,xMax,zMin,zMax;
    ObjRadiusRectangle(xMin,xMax,zMin,zMax,origin,aircraft,RotorLandProofRadius);
    unsigned visited=0;
    for (int z=zMin;z<=zMax;++z) for (int x=xMin;x<=xMax;++x)
    {
        const ObjectList& objects=GLandscape->GetObjects(z,x);
        for (int i=0;i<objects.Size();++i)
        {
            if (++visited>256) return false; // dense/unknown footprint fails closed
            Object* object=objects[i];
            if (object==ignore || object->GetType()==Temporary || object->GetType()==TypeTempVehicle) continue;
            const Vector3 position=object->Position();
            if (RotorLandBoundsBlock({origin.X(),origin.Y(),origin.Z()},
                {aircraft.X(),aircraft.Y(),aircraft.Z()},
                {position.X(),position.Y(),position.Z()},object->GetRadius())) return false;
        }
    }
    return true;
}
bool LandRotorFlag(const char* name, bool fallback);
bool LandRotorDrawTraceEnabled()
{
    static const bool enabled=LandRotorFlag("POSEIDON_ROTOR_LAND_DRAW_TRACE",false);
    return enabled;
}
RotorLandDrawReceipt landDrawReceipt;
void ReportRotorLandDraw(unsigned live)
{
    // Opt-in caller only. Wall cadence also observes genuinely frozen paused
    // views; no simulation time, particle age or renderer readiness is changed.
    static auto next=std::chrono::steady_clock::time_point{};
    const auto now=std::chrono::steady_clock::now();
    if(now<next)return;
    next=now+std::chrono::seconds(1);
    const auto& r=landDrawReceipt;
    const auto* camera=GScene?GScene->GetCamera():nullptr;
    const Vector3 position=camera?camera->Position():VZero;
    LOG_INFO(World,"ROTOR_LAND_DRAW clock={:.3f} live={} scene_cloudlets={} calls={} hidden={} shape={} near={} mip={} decal_calls={} invalid={} "
        "alpha=[{:.4f},{:.4f}] half_x_px=[{:.2f},{:.2f}] half_y_px=[{:.2f},{:.2f}] radius_m=[{:.3f},{:.3f}] distance_m=[{:.2f},{:.2f}] "
        "age_s=[{:.3f},{:.3f}] camera=[{:.3f},{:.3f},{:.3f}] scope=all-land-puffs-cpu-drawdecal-call-not-gpu-pixels",
        Glob.time.toFloat(),live,GScene&&GScene->GetCloudlets(),r.Calls(),r.counts[0],r.counts[1],r.counts[2],r.counts[3],r.counts[4],r.invalid,
        r.alphaMin,r.alphaMax,r.halfXMin,r.halfXMax,r.halfYMin,r.halfYMax,r.radiusMin,r.radiusMax,r.distanceMin,r.distanceMax,
        r.ageMin,r.ageMax,position.X(),position.Y(),position.Z());
    landDrawReceipt={};
}
// A short-lived land puff shares the established cloudlet rendering and room
// containment, but follows live airflow rather than baking wind at creation.
// The bound is across all helicopters and resets naturally at world teardown.
class RotorLandPuff final : public Cloudlet
{
    Vector3 _proofCentre;
    float _age = 0, _density, _shapeSphere, _radius = 0;
    bool _snow;
public:
    inline static unsigned Live = 0;
    static constexpr unsigned MaxLive = 128;
    RotorLandPuff(LODShapeWithShadow* shape, Vector3Par origin, float density, bool snow)
        : Cloudlet(shape, 2.0f, 2.0f), _proofCentre(origin), _density(density),
          _shapeSphere(shape->BoundingSphere()), _snow(snow)
    {
        ++Live;
        SetSimulationPrecision(1.0f / 25.0f);
    }
    ~RotorLandPuff() override { --Live; }
    void Simulate(float dt, SimulationImportance prec) override
    {
        if (!std::isfinite(dt) || dt <= 0) return; // no render/paused-clock ageing
        _age += dt;
        const auto appearance = RotorLandProfile(_age, _density, _snow);
        if (appearance.radius <= 0) { _invisible = true; SetDelete(); return; }
        {
            const Vector3 air = GAirflow.Sample(Position());
            if (!std::isfinite(air.X()) || !std::isfinite(air.Y()) || !std::isfinite(air.Z()))
            { _invisible = true; SetDelete(); return; }
            const float blend = 1.0f - std::exp(-1.6f * std::min(dt, 0.25f));
            _speed[0] += (air.X() - _speed.X()) * blend;
            _speed[2] += (air.Z() - _speed.Z()) * blend;
        }
        Cloudlet::Simulate(std::min(dt, .1f), prec);
        // The larger sprite never leaves the footprint proven at birth. Airflow
        // carries it outwards first; the remaining material then disperses there.
        Vector3 p = Position();
        if (!std::isfinite(p.X()) || !std::isfinite(p.Y()) || !std::isfinite(p.Z()))
        { _invisible = true; SetDelete(); return; }
        Vector3 radial = p - _proofCentre; radial[1] = 0;
        const float distance = radial.Size();
        if (distance > RotorLandMaxTravel)
        {
            radial *= RotorLandMaxTravel / distance;
            p[0] = _proofCentre.X() + radial.X(); p[2] = _proofCentre.Z() + radial.Z();
            SetPosition(p);
            const Vector3 normal = radial / RotorLandMaxTravel;
            const float outward = _speed * normal;
            if (outward > 0) _speed -= normal * outward;
        }
        if (GLandscape)
        {
            const float floor = GLandscape->SurfaceY(Position().X(), Position().Z()) + 0.08f;
            if (std::isfinite(floor) && Position().Y() < floor)
            {
                Vector3 p = Position(); p[1] = floor; SetPosition(p);
                _speed[1] = std::max(_speed.Y(), 0.0f);
            }
        }
        _radius = appearance.radius;
        // Override only this puff's generic smoke expansion/thinning. Keep its
        // weather-lit RGB, sheet animation and containment; no emissive flag.
        _constantColor = PackedColorRGB(_constantColor, int(appearance.opacity * 255));
        SetOrientScaleOnly(RotorLandCullScale(_radius, _shapeSphere));
    }
    void Draw(int level, ClipFlags, const FrameBase& frame) override
    {
        const bool trace=LandRotorDrawTraceEnabled();
        const auto refused=[trace](RotorLandDrawOutcome outcome) {if(trace) {landDrawReceipt.Record(outcome);ReportRotorLandDraw(Live);}};
        if (level == LOD_INVISIBLE || _radius <= 0 || Invisible()) {refused(RotorLandDrawOutcome::Hidden);return;}
        Shape* shape = GetShape()->LevelOpaque(level);
        if (!shape || shape->NFaces() != 1) {refused(RotorLandDrawOutcome::Shape);return;}
        const Camera& camera = *GScene->GetCamera();
        Vector3 projected = GScene->ScaledInvTransform() * frame.Position();
        const float nearest = camera.Near() * 10;
        if (projected.Z() < nearest) {refused(RotorLandDrawOutcome::Near);return;}
        Animate(level);
        const Matrix4& projection = camera.Projection();
        const float inverseZ = 1 / projected.Z();
        const float halfX = projection(0,0) * _radius * inverseZ * camera.InvLeft();
        const float halfY = -projection(1,1) * _radius * inverseZ * camera.InvTop();
        projected[0] = projection(0,2) + projection(0,0) * projected[0] * inverseZ;
        projected[1] = projection(1,2) + projection(1,1) * projected[1] * inverseZ;
        const float inverseNear = 1 / floatMax(projected.Z() - _radius, nearest);
        projected[2] = projection(2,2) + projection.Position()[2] * inverseNear;
        // Preserve the established decal's weather/eye lighting rather than
        // turning the larger sheet into an unlit bright overlay.
        TLMaterial material;
        material.ambient = HWhite;
        material.diffuse = HWhite * .25f;
        material.emmisive = HBlack;
        material.forcedDiffuse = HWhite * .25f;
        material.specFlags = 0;
        LightSun* sun = GScene->MainLight();
        sun->SetMaterial(material);
        Color light = sun->FullResult(.5f);
        if (sun->NightEffect() > .01f)
        {
            const LightList& lights = GScene->ActiveLights();
            const Matrix4 worldToModel(MTranslation,-frame.Position());
            for (int i=0;i<lights.Size();++i)
            {
                lights[i]->Prepare(worldToModel);
                lights[i]->SetMaterial(material);
                light += lights[i]->Apply(VZero,VForward);
            }
        }
        light = light * GEngine->GetAccomodateEye(); light.SetA(1);
        Color colour = light * (ColorVal)GetConstantColor();
        colour.SetA(colour.A() * (1 - GScene->Fog8(frame.Position().Distance2(camera.Position())) / 255.0f));
        const MipInfo mip = GEngine->TextBank()->UseMipmap(shape->FaceIndexed(0).GetTexture(),0,0);
        if (mip.IsOK())
        {
            const PackedColor packed(colour);
            GEngine->DrawDecal(projected,inverseNear,halfX,halfY,packed,mip,
                ClampU|ClampV|NoZWrite|IsAlpha|IsAlphaFog|NoShadow|IsColored|IsAlphaOrdered);
            if(trace)
            {
                landDrawReceipt.Record(RotorLandDrawOutcome::DecalCall,packed.A8()/255.0f,halfX,halfY,_radius,
                    std::sqrt(frame.Position().Distance2(camera.Position())),_age);
                ReportRotorLandDraw(Live);
            }
        }
        else refused(RotorLandDrawOutcome::Mip);
        Deanimate(level);
    }
    // Cloudlet's fixed-block pool reserves sizeof(Cloudlet). This derived
    // puff has additional state and must own matching allocation/deallocation.
    USE_FAST_ALLOCATOR;
};
DEFINE_FAST_ALLOCATOR(RotorLandPuff)
static_assert(sizeof(RotorLandPuff) > sizeof(Cloudlet));
static_assert(&RotorLandPuff::operator new != &Cloudlet::operator new,
    "Derived puff must not allocate from the smaller Cloudlet pool");
static_assert(&RotorLandPuff::operator delete != &Cloudlet::operator delete,
    "Derived puff must return storage to its own pool");

bool LandRotorFlag(const char* name, bool fallback)
{
#ifdef _WIN32
        char* value = nullptr; size_t size = 0;
        _dupenv_s(&value, &size, name);
        const bool result = value ? value[0] != '0' : fallback; std::free(value); return result;
#else
        const char* value = std::getenv(name);
        return value ? value[0] != '0' : fallback;
#endif
}
}

void Helicopter::EmitLandRotorRing(float dt, SimulationImportance prec)
{
    static const bool enabled = LandRotorFlag("POSEIDON_ROTOR_LAND", true);
    static const bool trace = LandRotorFlag("POSEIDON_ROTOR_LAND_TRACE", false);
    if(LandRotorDrawTraceEnabled())ReportRotorLandDraw(RotorLandPuff::Live);
    const float airflowScale = GAirflow.LocalScale();
    if (!GLandscape || !GWorld || !GScene || !GScene->GetCamera() ||
        !enabled || !GAirflow.LocalEnabled() || !std::isfinite(airflowScale) || airflowScale <= 0 ||
        !EnableVisualEffects(prec) || _isDead ||
        !std::isfinite(dt) || dt <= 0 || _rotorSpeed <= 0.2f || DirectionUp().Y() < 0.5f ||
        (Position() - GScene->GetCamera()->Position()).SquareSize() > 350.0f * 350.0f)
    {
        _landRotorTime = 0; return;
    }
    _landRotorTime += std::min(dt, 0.25f);
    if (_landRotorTime < 0.25f) return;
    _landRotorTime -= 0.25f;
    LODShapeWithShadow* shape = GScene->Preloaded(CloudletBasic);
    if (!shape || RotorLandCullScale(RotorLandMaxRadius,shape->BoundingSphere()) <= 0) return;
    static Ref<CloudletTTable> table = [] {
        auto* colours = new CloudletTTable; colours->Add(10000, HWhite); return colours;
    }();
    const Vector3 centre = Position();
    const float centreGround = GLandscape->SurfaceY(centre.X(), centre.Z());
    if (!std::isfinite(centreGround) || centre.Y() - centreGround < 0 ||
        centre.Y() - centreGround >= 30) return;
    RotorWash wash;
    wash.position = centre;
    wash.discRadius = std::max(GetRadius() * 0.8f, 3.0f);
    wash.strength = 14.0f * _rotorSpeed * _rotorSpeed * std::clamp(airflowScale, 0.0f, 3.0f);
    wash.groundY = centreGround;
    const float rain = UniformWettingDensity(GLandscape->GetRainDensity(), GRain.EffectiveDensity(),
                                            GRain.Params().snowflakes);
    static RotorGroundBudget probeBudget;
    unsigned emitted = 0, snowPuffs = 0, blocked = 0;
    for (unsigned spoke = 0; spoke < 8 && RotorLandPuff::Live < RotorLandPuff::MaxLive; ++spoke)
    {
        // Rotate the first sector so the five-probe budget does not privilege
        // the same six sectors on every clear-ground pulse.
        const auto offset = RotorGroundRing(_landRotorPhase, (spoke + _landRotorPhase) % 8, wash.discRadius);
        const float x = centre.X() + offset[0], z = centre.Z() + offset[1];
        const float extent = GLandscape->GetLandRange() * GLandscape->GetLandGrid();
        if (!(x >= 0 && z >= 0 && x < extent && z < extent)) continue;
        const float ground = GLandscape->SurfaceY(x, z);
        if (!std::isfinite(ground) || ground <= GLandscape->GetSeaLevel() + 0.03f) continue;
        const Vector3 origin(x, ground + 0.15f, z);
        RotorGroundEmission emission;
        bool proven = true, exhausted = false;
        for (unsigned probe = 0; probe < 5; ++probe)
        {
            if (!probeBudget.Take(Glob.time.toFloat())) { proven = false; exhausted = true; break; }
            const auto edge = RotorLandFootprint(probe);
            const float px = x + edge[0], pz = z + edge[1];
            if (!(px >= 0 && pz >= 0 && px < extent && pz < extent)) { proven = false; break; }
            const float height = GLandscape->SurfaceY(px,pz);
            if (!std::isfinite(height) || height <= GLandscape->GetSeaLevel()+.03f ||
                std::abs(height-ground) > 1.0f) { proven = false; break; }
            const Vector3 point(px,height+.15f,pz);
            Object* roadway = nullptr;
            const float support = GLandscape->RoadSurfaceY(point,nullptr,nullptr,nullptr,&roadway);
            const auto water = GRainWater().At(px,pz);
            const auto& surface = GLandscape->SurfaceAt(px,pz);
            const float remainingSnow = std::max(0.0f,GSnow().BaseDepthAt(px,pz)-
                GSnow().DeficitAt(int(std::floor(px/SnowField::CellSize)),int(std::floor(pz/SnowField::CellSize))));
            auto candidate = RotorGroundPolicy(_rotorSpeed,centre.Y()-height,DirectionUp().Y(),
                surface._dustness,remainingSnow,std::max(GMud().Wetness(),GSand().Wetness()),
                rain,water.depth,water.valid,true,false,roadway || !std::isfinite(support) || support > height+.1f);
            candidate.density = std::clamp(candidate.density*airflowScale,0.0f,1.0f);
            if (candidate.density < .025f || (probe && candidate.snow != emission.snow)) { proven = false; break; }
            if (!probe) emission = candidate;
            else emission.density = std::min(emission.density,candidate.density);
            // Thin ray radius is not a swept narrowphase in Object::Intersect.
            // A bounded conservative object-sphere refusal closes gaps between
            // the five physical rays, including a small roof between taps.
            if (!probe && !LandRotorBoundsClear(point,centre,this)) { ++blocked; proven=false; break; }
            CollisionBuffer hits;
            GLandscape->ObjectCollision(hits,nullptr,this,point,centre,.05f,ObjIntersectView);
            if (hits.Size()) { ++blocked; proven = false; break; }
        }
        if (exhausted) break;
        if (!proven) continue;
        auto* puff = new RotorLandPuff(shape,origin,emission.density,emission.snow);
        Vector3 velocity = EvaluateRotorWash(wash, origin);
        velocity[1] = emission.snow ? 0.7f : 0.35f;
        puff->SetPosition(origin); puff->SetSpeed(velocity);
        puff->SetSideSpeed(0, 0, 0, 0);
        puff->SetClimbRate(-0.2f, -0.3f, 2.0f);
        puff->SetFades(0.12f, 0.65f);
        puff->SetGrowUp(0.25f, 1.0f); // private profile supplies actual metre radius
        puff->SetAlpha(.2f);
        const auto tint=RotorLandTint(emission.snow);
        puff->SetColor(Color(tint[0],tint[1],tint[2]));
        puff->SetTemperature(0, 0, table); puff->SetShadeOrigin(Vector3(centre.X(),centreGround,centre.Z()), 1.9f);
        puff->SetSmokeShadow(false);
        GWorld->AddCloudlet(puff); ++emitted; snowPuffs += emission.snow ? 1u : 0u;
    }
    ++_landRotorPhase;
    if (trace && (_landRotorPhase % 8 == 0))
        LOG_INFO(World, "ROTOR_LAND object={} x={:.2f} y={:.2f} z={:.2f} rpm={:.3f} height={:.2f} emitted={} snow={} sheltered={} live={} rain={:.3f} profile=soft footprint={:.2f}",
            RenderId(), centre.X(), centre.Y(), centre.Z(), _rotorSpeed, centre.Y() - centreGround,
            emitted, snowPuffs, blocked, RotorLandPuff::Live, rain, RotorLandProofRadius);
}

void Helicopter::Simulate(float deltaT, SimulationImportance prec)
{
    _isDead = IsDammageDestroyed();
    EmitLandRotorRing(deltaT, prec);

    // Persistent ground edits belong to each helicopter, not the camera's four
    // nearest airflow emitters. Keep the fine snow-field work at 4 Hz.
    if (GSnow().CoverActive() && GSnow().BaseDepthAt(Position().X(), Position().Z()) > 0.0f && GAirflow.LocalEnabled() &&
        _rotorSpeed > 0.1f && std::isfinite(deltaT) && deltaT > 0 && DirectionUp().Y() > 0.5f)
    {
        _snowRotorTime += std::min(deltaT, 0.25f);
        if (_snowRotorTime >= 0.25f)
        {
            static const bool traceSnow = std::getenv("POSEIDON_SNOW_TRACE") != nullptr;
            const auto start = traceSnow ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            const Vector3 p = Position();
            const float ground = GLandscape->SurfaceY(p.X(), p.Z());
            if (ground > GLandscape->GetSeaLevel() && GLandscape->RoadSurfaceY(p.X(), p.Z()) <= ground + 0.3f)
                GSnow().ErodeRotor(p.X(), p.Z(), std::max(GetRadius() * 0.8f, 3.0f),
                                   _rotorSpeed, p.Y() - ground, 0.25f);
            _snowRotorTime -= 0.25f;
            if (traceSnow)
                LOG_INFO(World, "Snow rotor update: speed={:.3f} height={:.2f} us={:.1f}", _rotorSpeed,
                         p.Y()-ground, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now()-start).count());
        }
    }
    else _snowRotorTime = 0;

    if (_rotorSpeed > 0)
    {
        ConsumeFuel(deltaT * 0.03);
    }

    ConsumeFuel(deltaT * GetHit(Type()->_hullHit) * 0.2);

    Vector3Val speed = ModelSpeed();
    Vector3 force(VZero), friction(VZero);
    Vector3 torque(VZero), torqueFriction(VZero);

    Vector3 wCenter(VFastTransform, ModelToWorld(), GetCenterOfMass());

    Vector3 pForce(VZero);  // partial force
    Vector3 pCenter(VZero); // partial force application point

    float delta;

    if (_isDead)
    {
        if (_landContact || _waterContact || _objectContact)
        {
            EngineOffAction();
        }
    }
    if (_fuel <= 0)
    {
        EngineOffAction();
    }

    {
        if (_rotorSpeedWanted > 0.2 || _rotorSpeed > 0.1)
        {
            IsMoved();
        }

        // handle impulse
        float impulse2 = _impulseForce.SquareSize();
        if (impulse2 > Square(GetMass() * 0.01f))
        {
            IsMoved();
        }
    }

    if (GetStopped())
    {
        // reset impulse - avoid cummulation
        _impulseForce = VZero;
        _impulseTorque = VZero;
    }

    _rotorPosition += _rotorSpeed * deltaT * 20;
    // keep number small, is equivalent mod 2 (see usage of _rotorPosition)
    _rotorPosition = fastFmod(_rotorPosition, 4 * 3 * 5 * H_PI);

    if (!_isStopped && !CheckPredictionFrozen())
    {
        saturateMin(_rotorSpeedWanted, 1 - GetHit(Type()->_engineHit));
        // main engine
        delta = _rotorSpeedWanted - _rotorSpeed;
        float brakeRotor = _mainRotor;
        saturate(brakeRotor, -0.2f, 1); // if we apply rotor up, rotor will stop
        // auto-rotate - when going down, rotor will move faster
        float goingDown = (-ModelSpeed().Y() - 2) * 0.25f;
        saturateMax(goingDown, 0);
        saturateMax(brakeRotor, -goingDown);

        float maxDown = -0.025f - brakeRotor * 0.37f;
        Limit(delta, maxDown * deltaT, +0.05f * deltaT);
        _rotorSpeed += delta;
        Limit(_rotorSpeed, 0, 1);

        float mrw = floatMin(_mainRotorWanted, _rotorSpeed);
        delta = mrw - _mainRotor;
        Limit(delta, -0.25 * deltaT, +0.25 * deltaT);
        _mainRotor += delta;
        Limit(_mainRotor, -0.2, _rotorSpeed);

        delta = _backRotorWanted - _backRotor;
        Limit(delta, -10 * deltaT, +10 * deltaT);
        _backRotor += delta;
        Limit(_backRotor, -1, +1);

        Limit(_cyclicAsideWanted, -2, +2);
        delta = _cyclicAsideWanted - _cyclicAside;
        Limit(delta, -10 * deltaT, 10 * deltaT);
        _cyclicAside += delta, Limit(_cyclicAside, -1, 1);

        delta = _cyclicForwardWanted - _cyclicForward;
        Limit(delta, -10 * deltaT, 10 * deltaT);
        _cyclicForward += delta, Limit(_cyclicForward, -1, 1);

        Limit(_rotorDiveWanted, -1, +1);
        delta = _rotorDiveWanted - _rotorDive;
        Limit(delta, -0.15 * deltaT, 0.15 * deltaT);
        _rotorDive += delta, Limit(_rotorDive, -1, 1);

        pCenter.Init();
        pCenter[0] = _cyclicAside * 0.8;
        pCenter[2] = _cyclicForward * -0.8;
        pCenter[1] = 0;
        float rotorSpeed = _rotorSpeed;
        if (_isDead)
        {
            rotorSpeed = 0;
        }
        if (rotorSpeed > 0.2)
        {
            float massCoef = GetMass() * (1.0 / 3000);
            saturate(massCoef, 1, 3);
            pForce =
                (RotorUpForce(speed, _mainRotor * rotorSpeed, Position().Y()) * (_rotorSpeed * _rotorSpeed * massCoef));
            if (fabs(_rotorDive) > 1e-3)
            {
                Matrix3 rotDive(MRotationX, _rotorDive);
                pForce = rotDive * pForce;
            }
        }
        else
        {
            pForce = VZero;
        }
#if ARROWS
        AddForce(DirectionModelToWorld(pCenter) + wCenter, DirectionModelToWorld(pForce) * InvMass());
#endif
        force += pForce;
        torque += pCenter.CrossProduct(pForce);

        // rotate helicopter when in free fall mode
        if (!_landContact && _rotorSpeed < 0.1)
        {
            torque += Vector3(0.5, 3.2, 1.3).CrossProduct(Vector3(0, GetMass(), 0));
        }

        float backRadius = _shape->BoundingSphere() * 0.95;
        // when moving fast, side bank causes torque
        float bank = DirectionAside().Y();
        float fastTurn = fabs(speed[2] - 3) * FAST_COEF;
        Limit(fastTurn, 0, 1); // back rotor has always some significance
        float sizeFactor = backRadius * (1.0 / 12);
        pForce = Vector3((bank * 10 * fastTurn + _backRotor * 4 * floatMax(1 - fastTurn, 0.1)) *
                             (_rotorSpeed * _rotorSpeed * GetMass() * sizeFactor),
                         0, 0);
        pCenter = Vector3(0, +backRadius * 0.08, -backRadius);
        torque += pCenter.CrossProduct(pForce);

#if ARROWS
        AddForce(DirectionModelToWorld(pCenter) + wCenter, DirectionModelToWorld(pForce) * InvMass());
#endif

        DirectionModelToWorld(torque, torque);
        DirectionModelToWorld(force, force);

        torqueFriction = _angMomentum * (0.2 + _rotorSpeed * _rotorSpeed * 2);

        Matrix4 movePos;
        ApplySpeed(movePos, deltaT);
        Frame moveTrans;
        moveTrans.SetTransform(movePos);

        if (Glob.time > _lastTurbulenceTime + 2)
        {
            _lastTurbulenceTime = Glob.time;
            const float maxXT = 1;
            const float maxYT = 1;
            const float maxZT = 1;
            float tx = (GRandGen.RandomValue() - 0.5) * (maxXT * 2);
            float ty = (GRandGen.RandomValue() - 0.5) * (maxYT * 2);
            float tz = (GRandGen.RandomValue() - 0.5) * (maxZT * 2);
            _turbulence = Vector3(tx, ty, tz);
        }

        Vector3 wind = GLandscape->GetWind() + _turbulence;
        Vector3 airSpeed = speed + DirectionWorldToModel(wind);
        friction = BodyFriction(airSpeed, _rotorSpeed * _rotorSpeed, Type()->_maxMainRotorDive > 0.01);

        DirectionModelToWorld(friction, friction);

#if ARROWS
        AddForce(wCenter, -friction * InvMass(), PackedColor(Color(0.5, 0, 0)));
#endif

        pForce = Vector3(0, -1, 0) * (GetMass() * G_CONST);
        force += pForce;
#if ARROWS
        AddForce(wCenter, pForce * InvMass());
#endif

        wCenter.SetFastTransform(moveTrans.ModelToWorld(), GetCenterOfMass());
        if (deltaT > 0)
        {
            bool wasLandContact = _landContact;
            _objectContact = false;
            _landContact = false;
            _waterContact = false;

            Vector3 totForce(VZero);

            float crash = 0;

            if (prec <= SimulateVisibleFar && IsLocal())
            {
                CollisionBuffer collision;
                GLOB_LAND->ObjectCollision(collision, this, moveTrans);
#define MAX_IN 0.4
#define MAX_IN_FORCE 0.1
#define MAX_IN_FRICTION 0.4

                for (int i = 0; i < collision.Size(); i++)
                {
                    CollisionInfo& info = collision[i];
                    if (info.object && !info.object->IsPassable())
                    {
                        _objectContact = true;
                        float cFactor = 1;
                        if (info.object->GetMass() < 50)
                        {
                            continue;
                        }
                        if (info.object->GetType() == Primary)
                        {
                            cFactor = 1;
                        }
                        else
                        {
                            cFactor = info.object->GetMass() * GetInvMass();
                            saturate(cFactor, 0, 5);
                        }
                        if (cFactor > 0.05)
                        {
                            Vector3Val pos = info.object->PositionModelToWorld(info.pos);
                            Vector3Val dirOut = info.object->DirectionModelToWorld(info.dirOut);
                            float forceIn = floatMin(info.under, MAX_IN_FORCE);
                            Vector3 pForce = dirOut * GetMass() * 40 * forceIn;
                            pCenter = pos - wCenter;
                            totForce += pForce;
                            torque += pCenter.CrossProduct(pForce);

                            Vector3Val objSpeed = info.object->ObjectSpeed();
                            Vector3 colSpeed = _speed - objSpeed;

                            // if info.under is bigger than MAX_IN, move out
                            if (info.under > MAX_IN)
                            {
                                Vector3 newPos = moveTrans.Position();
                                float moveOut = info.under - MAX_IN;
                                if (moveOut > 0.1)
                                {
                                    crash = 0.5;
                                }
                                newPos += dirOut * moveOut * 0.1;
                                moveTrans.SetPosition(newPos);
                            }
                            saturateMax(crash, (colSpeed.SquareSize() - 25) * (1.0 / 25));

                            const float maxRelSpeed = 5;
                            if (colSpeed.SquareSize() > Square(maxRelSpeed))
                            {
                                // adapt _speed to match criterion
                                colSpeed.Normalize();
                                colSpeed *= maxRelSpeed;
                                // only slow down
                                float oldSize = _speed.Size();
                                _speed = colSpeed + objSpeed;
                                if (_speed.SquareSize() > Square(oldSize))
                                {
                                    _speed = _speed.Normalized() * oldSize;
                                }
                            }

                            // second is "land friction" - causing no momentum

                            float frictionIn = floatMin(info.under, MAX_IN_FRICTION);
                            pForce[0] = fSign(speed[0]) * 20000;
                            pForce[1] = speed[1] * fabs(speed[1]) * 1000 + speed[1] * 8000 + fSign(speed[1]) * 10000;
                            pForce[2] = speed[2] * fabs(speed[2]) * 150 + speed[2] * 250 + fSign(speed[2]) * 2000;

                            pForce = DirectionModelToWorld(pForce) * GetMass() * (4.0 / 10000) * frictionIn;
#if ARROWS
                            AddForce(wCenter + pCenter, -pForce * InvMass());
#endif
                            friction += pForce;
                            torqueFriction += _angMomentum * 0.15;
                        }
                    }
                }
            } // if( object collisions enabled )

            GroundCollisionBuffer gCollision;
            bool enableLandcontact = true;
            if (wasLandContact && DirectionUp().Y() < 0.985f) // cos 10 deg
            {
                enableLandcontact = false;
            }
            GLOB_LAND->GroundCollision(gCollision, this, moveTrans, 0.05f, 0, enableLandcontact);

            if (gCollision.Size() > 0)
            {
                Vector3 gFriction(VZero);
                float maxUnder = 0;
                float maxUnderWater = 0;
#define MAX_UNDER 0.1f
#define MAX_UNDER_FORCE 0.1f

                Shape* landcontact = GetShape()->LandContactLevel();
                int nContactPoint = landcontact ? landcontact->NPos() : 3;
                saturateMax(nContactPoint, gCollision.Size());
                const float nPointCoef = 3.0f / nContactPoint;
                for (int i = 0; i < gCollision.Size(); i++)
                {
                    UndergroundInfo& info = gCollision[i];
                    if (info.under < 0)
                    {
                        continue;
                    }
                    float under;
                    if (info.type == GroundWater)
                    {
                        under = floatMin(info.under, 3) * 0.001f;
                        if (maxUnderWater < info.under)
                        {
                            maxUnderWater = info.under;
                        }
                        _waterContact = true;
                        if (_speed.SquareSize() > Square(8))
                        {
                            crash = 2;
                        }
                    }
                    else
                    {
                        _landContact = true;
                        if (maxUnder < info.under)
                        {
                            maxUnder = info.under;
                        }
                        under = floatMin(info.under, MAX_UNDER_FORCE);
                    }
                    Vector3 dirOut = Vector3(0, info.dZ, 1).CrossProduct(Vector3(1, info.dX, 0)).Normalized();
                    pForce = dirOut * GetMass() * 40.0f * nPointCoef * under;
                    pCenter = info.pos - wCenter;
                    torque += pCenter.CrossProduct(pForce);
                    // to do: analyze ground reaction force
                    totForce += pForce;

#if ARROWS
                    AddForce(wCenter + pCenter, pForce * under * InvMass());
#endif

                    pForce[0] = speed[0] * 5000 + fSign(speed[0]) * 10000;
                    pForce[1] = speed[1] * fabs(speed[1]) * 1000 + speed[1] * 8000 + fSign(speed[1]) * 10000;
                    pForce[2] = speed[2] * fabs(speed[2]) * 150 + speed[2] * 250 + fSign(speed[2]) * 5000;

                    pForce = DirectionModelToWorld(pForce) * GetMass() * nPointCoef * (1.0f / 10000);
#if ARROWS
                    AddForce(wCenter + pCenter, -pForce * InvMass());
#endif
                    friction += pForce;

                    if (fabs(speed[0]) < 1)
                    {
                        pForce[0] = 0;
                    }
                    if (fabs(speed[1]) < 1)
                    {
                        pForce[1] = 0;
                    }
                    if (fabs(speed[2]) < 1)
                    {
                        pForce[2] = 0;
                    }
                    torque -= pCenter.CrossProduct(pForce); // sub: it is friction

                    torqueFriction += _angMomentum * info.under * 3;
                }
                saturateMax(crash, (_speed.SquareSize() - 25) * (1.0f / 30));
                if (maxUnder > MAX_UNDER)
                {
                    Vector3 newPos = moveTrans.Position();
                    float moveUp = maxUnder - MAX_UNDER;
                    newPos[1] += moveUp;
                    moveTrans.SetPosition(newPos);

                    if (_speed.SquareSize() > Square(10))
                    {
                        _speed.Normalize();
                        _speed *= 10;
                    }
                    saturateMax(_speed[1], -0.1f);
                }
                const float maxFord = 1.1f;
                if (maxUnderWater > maxFord)
                {
                    float dammage = (maxUnderWater - maxFord) * 0.5f;
                    saturateMin(dammage, 0.2f);
                    LocalDammage(nullptr, this, VZero, dammage * deltaT, GetRadius());
                }
            }
            force += totForce;
            if (crash > 0)
            {
                if (Glob.time > _disableDammageUntil)
                {
                    _disableDammageUntil = Glob.time + 0.5f;
                    _doCrash = CrashLand;
                    if (_objectContact)
                    {
                        _doCrash = CrashObject;
                    }
                    if (_waterContact)
                    {
                        _doCrash = CrashWater;
                    }
                    _crashVolume = crash;
                    saturateMax(_crashVolume, crash);

                    CrashDammage(crash);
                    if (IsLocal() && crash > 5)
                    {
                        float maxTime = 5 - crash * 0.2f;
                        saturate(maxTime, 0.5f, 3);
                        if (_explosionTime > Glob.time + maxTime)
                        {
                            _explosionTime = Glob.time + GRandGen.Gauss(0, maxTime * 0.3f, maxTime);
                        }
                    }
                }
            }
        }

        bool stopCondition = false;
        if (_landContact && !_waterContact && !_objectContact)
        {
            // apply static friction
            float maxSpeed = Square(0.7f);
            if (!Driver())
            {
                maxSpeed = Square(1.2f);
            }
            if (_speed.SquareSize() < maxSpeed && _angVelocity.SquareSize() < maxSpeed * 0.3f)
            {
                stopCondition = true;
            }
        }
        if (stopCondition)
        {
            StopDetected();
        }
        else
        {
            IsMoved();
        }

        _lastAngVelocity = _angVelocity; // helper for prediction
        ApplyForces(deltaT, force, torque, friction, torqueFriction);

        _turret.Stabilize(this, Type()->_turret, Transform().Orientation(), moveTrans.Orientation());

        if (prec <= SimulateCamera)
        {
            _head.Move(deltaT, moveTrans, *this);
        }
        Move(moveTrans);
        DirectionWorldToModel(_modelSpeed, _speed);
    }

    if (_isDead && (_landContact || _objectContact))
    {
        NeverDestroy();
        SmokeSourceVehicle* smoke = dyn_cast<SmokeSourceVehicle>(GetSmoke());
        if (smoke)
        {
            smoke->SetExplosion(100, 100);
            smoke->Explode();
        }
        // note: high speed during contact causes explosion
    }

    if (EnableVisualEffects(prec))
    {
        if (_mGunClouds.Active() || _mGunFire.Active())
        {
            Matrix4Val toWorld = Transform() * GunTransform();
            Vector3Val dir = toWorld.Direction();
            Vector3 gunPos(VFastTransform, toWorld, Type()->_gunPos);
            _mGunClouds.Simulate(gunPos, Speed() * 0.7 + dir * 5.0, 0.35, deltaT);
            _mGunFire.Simulate(gunPos, deltaT);
        }
    }

    base::Simulate(deltaT, prec);
}

bool HelicopterAuto::EngineIsOn() const
{
    return _rotorSpeedWanted > 0.5f;
}

void HelicopterAuto::EngineOn()
{
    _rotorSpeedWanted = 1;
    base::EngineOn();
    // keep landed
}
void HelicopterAuto::EngineOff() {}
void HelicopterAuto::EngineOffAction()
{
    StopRotor();
}

float HelicopterAuto::MakeAirborne()
{
    EngineOn();
    _rotorSpeed = _rotorSpeedWanted = 1.0;
    _mainRotor = _mainRotorWanted = 0.7;
    _pilotHeight = _defPilotHeight;
    _landContact = false;
    _objectContact = false;
    return _defPilotHeight;
}

void HelicopterAuto::SetFlyingHeight(float val)
{
    _defPilotHeight = val;
}

bool Helicopter::Airborne() const
{
    return !_landContact;
}

void HelicopterAuto::StopRotor()
{
    _rotorSpeedWanted = 0;
    base::EngineOff();
}

float Helicopter::GetEngineVol(float& freq) const
{
    freq = _rndFrequency * _rotorSpeed * (1 - _mainRotor * 0.1);
    return 1;
}

float Helicopter::GetEnvironVol(float& freq) const
{
    freq = 1;
    return _speed.Size() / Type()->GetMaxSpeedMs();
}

Texture* Helicopter::GetCursorTexture(Person* person)
{
    return base::GetCursorTexture(person);
}

Texture* Helicopter::GetCursorAimTexture(Person* person)
{
    // check if target designator is active
    if (_laserTargetOn)
    {
        return GPreloadedTextures.New(CursorLocked);
    }
    return base::GetCursorAimTexture(person);
}

void Helicopter::Sound(bool inside, float deltaT)
{
    _turret.Sound(Type()->_turret, inside, deltaT, *this, Speed());
    base::Sound(inside, deltaT);
}

void Helicopter::UnloadSound()
{
    base::UnloadSound();
    _turret.UnloadSound();
}

bool Helicopter::HasFlares(CameraType camType) const
{
    return camType != CamInternal && camType != CamGunner;
}

Matrix4 Helicopter::InsideCamera(CameraType camType) const
{
    return base::InsideCamera(camType);
}

int Helicopter::InsideLOD(CameraType camType) const
{
    return base::InsideLOD(camType);
}

void Helicopter::InitVirtual(CameraType camType, float& heading, float& dive, float& fov) const
{
    base::InitVirtual(camType, heading, dive, fov);
    /*
        switch( camType )
        {
            case CamGunner:
                fov=0.50;
            break;
        }
    */
}

void Helicopter::LimitVirtual(CameraType camType, float& heading, float& dive, float& fov) const
{
    base::LimitVirtual(camType, heading, dive, fov);
    /*
        switch( camType )
        {
            case CamGunner:
                saturate(fov,0.3,1.2);
                saturate(heading,-1.8,+1.8);
                saturate(dive,-0.7,+0.3);
            break;
        }
    */
}

Matrix4 Helicopter::GunTransform() const
{
    /*
        return
        (
            Matrix4(MTranslation,Type()->_gunAxis)*
            Matrix4(MRotationY,_gunYRot)*
            Matrix4(MRotationX,-_gunXRot)*
            Matrix4(MTranslation,-Type()->_gunAxis)
        );
    */
    // animate matrix connected with selection Type()->_mainTurret._gun
    int memory = GetShape()->FindMemoryLevel();
    int sel = Type()->_turret._gun.GetSelection(memory);
    if (sel >= 0)
    {
        Matrix4 mat = MIdentity;
        AnimateMatrix(mat, memory, sel);
        return mat;
    }
    return MIdentity;
}

bool Helicopter::IsAnimated(int level) const
{
    return true;
}
bool Helicopter::IsAnimatedShadow(int level) const
{
    // a stopped (unsimulated) vehicle cannot change pose
    return !ShadowPoseFrozen();
}

void Helicopter::AnimateMatrix(Matrix4& mat, int level, int selection) const
{
    // default proxy transform calculation
    // check which selection is the proxy in:

    _turret.AnimateMatrix(Type()->_turret, mat, this, level, selection);
}

Vector3 Helicopter::AnimatePoint(int level, int index) const
{
    // note: only turret/gun animation is done here
    // check which animations is this point in

    Shape* shape = _shape->Level(level);
    if (!shape)
    {
        return VZero;
    }
    shape->SaveOriginalPos();

    Vector3 pos = shape->OrigPos(index);

    _turret.AnimatePoint(Type()->_turret, pos, this, level, index);

    return pos;
}

inline float Helicopter::GetGlassBroken() const
{
    const HelicopterType* type = Type();
    float glassDammage = GetHitCont(type->_hullHit);
    saturateMax(glassDammage, GetTotalDammage());
    saturateMax(glassDammage, GetHitCont(type->_glassLHit));
    saturateMax(glassDammage, GetHitCont(type->_glassRHit));
    return glassDammage;
}

void Helicopter::DammageAnimation(int level)
{
    const HelicopterType* type = Type();
    // scan corresponding wound

    float glassDammage = GetGlassBroken();
    if (glassDammage >= 0.6)
    {
        type->_glassDammageFull.Apply(_shape, level);
    }
    else if (glassDammage >= 0.3)
    {
        type->_glassDammageHalf.Apply(_shape, level);
    }
}

void Helicopter::DammageDeanimation(int level)
{
    const HelicopterType* type = Type();
    // scan corresponding wound
    float glassDammage = GetGlassBroken();
    if (glassDammage >= 0.6)
    {
        type->_glassDammageFull.Restore(_shape, level);
    }
    else if (glassDammage >= 0.3)
    {
        type->_glassDammageHalf.Restore(_shape, level);
    }
}

void Helicopter::Animate(int level)
{
    static const bool rotorDiag = std::getenv("POSEIDON_ROTOR_DIAG") != nullptr;
    if (rotorDiag)
    {
        static std::unordered_map<uint32_t, float> nextReport;
        float& next = nextReport[RenderId()];
        if (Glob.time.toFloat() >= next)
        {
            next = Glob.time.toFloat() + 1.0f;
            LOG_INFO(Graphics, "Rotor pose: object={} pos={:.1f},{:.1f},{:.1f} speed={:.3f} phase={:.3f} lod={} mainBlur={} tailBlur={}",
                     RenderId(), Position().X(), Position().Y(), Position().Z(), _rotorSpeed, _rotorPosition, level,
                     Type()->_hRotorMove.GetSelection(level), Type()->_vRotorMove.GetSelection(level));
        }
    }
    if (_rotorSpeed < 0.6)
    {
        Type()->_hRotorMove.Hide(_shape, level);
        Type()->_vRotorMove.Hide(_shape, level);
    }
    else
    {
        // Some distant LODs have blades but no blur selection. Keep their
        // rotating blades rather than hiding the only rotor geometry.
        if (Type()->_hRotorMove.GetSelection(level) >= 0)
            Type()->_hRotorStill.Hide(_shape, level);
        if (Type()->_vRotorMove.GetSelection(level) >= 0)
            Type()->_vRotorStill.Hide(_shape, level);
    }
    if (Type()->_rotorH.GetSelection(level) >= 0)
    {
        Matrix4 rot;
        Type()->_rotorH.GetRotation(rot, _rotorPosition * Type()->_mainRotorSpeed, level);

        float diveAngle = _rotorDive + Type()->_neutralMainRotorDive;
        if (fabs(diveAngle) > 1e-3)
        {
            saturate(diveAngle, Type()->_minMainRotorDive, Type()->_maxMainRotorDive);
            Matrix4 dive(MRotationX, diveAngle);

            Matrix4 diveAxis(MTranslation, Type()->_mainRotorDiveAxis);
            Matrix4 diveAxisInv(MTranslation, -Type()->_mainRotorDiveAxis);

            Matrix4 comb = diveAxis * dive * diveAxisInv * rot;

            Type()->_rotorH.Transform(_shape, comb, level);
        }
        else
        {
            Type()->_rotorH.Transform(_shape, rot, level);
        }
    }

    if (Type()->_rotorV.GetSelection(level) >= 0)
    {
        Matrix4 rot;
        Type()->_rotorV.GetRotation(rot, _rotorPosition * Type()->_backRotorSpeed, level);

        float diveAngle = _rotorDive + Type()->_neutralBackRotorDive;

        if (fabs(diveAngle) > 1e-3)
        {
            saturate(diveAngle, Type()->_minBackRotorDive, Type()->_maxBackRotorDive);
            Matrix4 dive(MRotationX, diveAngle);

            Matrix4 diveAxis(MTranslation, Type()->_backRotorDiveAxis);
            Matrix4 diveAxisInv(MTranslation, -Type()->_backRotorDiveAxis);

            Matrix4 comb = diveAxis * dive * diveAxisInv * rot;

            Type()->_rotorV.Transform(_shape, comb, level);
        }
        else
        {
            Type()->_rotorV.Transform(_shape, rot, level);
        }
    }

    // indicators
    float value = fastFmod(Position().Y(), 304);
    Type()->_altRadarIndicator.SetValue(_shape, level, value);
    Type()->_altRadarIndicator2.SetValue(_shape, level, value);
    value = Position().Y() - GLandscape->SurfaceYAboveWater(Position().X(), Position().Z());
    Type()->_altBaroIndicator.SetValue(_shape, level, value);
    Type()->_altBaroIndicator2.SetValue(_shape, level, value);
    value = fabs(ModelSpeed()[2]);
    Type()->_speedIndicator.SetValue(_shape, level, value);
    Type()->_speedIndicator2.SetValue(_shape, level, value);
    value = _speed.Y();
    Type()->_vertSpeedIndicator.SetValue(_shape, level, value);
    Type()->_vertSpeedIndicator2.SetValue(_shape, level, value);
    float rpm = _rotorSpeed * (1 - _mainRotor * 0.1);
    value = 10.0 * rpm;
    Type()->_rpmIndicator.SetValue(_shape, level, value);
    Type()->_rpmIndicator2.SetValue(_shape, level, value);
    value = atan2(Direction().X(), Direction().Z());
    Type()->_compass.SetValue(_shape, level, value);
    Type()->_compass2.SetValue(_shape, level, value);
    Type()->_watch.SetTime(_shape, level, Glob.clock);
    Type()->_watch2.SetTime(_shape, level, Glob.clock);

    // vario
    {
        Vector3Val dir = Type()->_varioDirection[level];
        Matrix3 rot(MDirection, dir, VUp);
        Matrix3 invRot(MInverseRotation, rot);

        Vector3 up = InvTransform().DirectionUp();
        up[2] = -up[2];

        up = invRot * up;
        Matrix3 orient(MUpAndDirection, up, VForward);
        orient = rot * orient;

        Matrix4 trans;
        trans.SetPosition(VZero);
        trans.SetOrientation(orient);
        Type()->_vario.Apply(_shape, trans, level);
    }

    // vario2
    {
        Vector3Val dir = Type()->_vario2Direction[level];
        Matrix3 rot(MDirection, dir, VUp);
        Matrix3 invRot(MInverseRotation, rot);

        Vector3 up = InvTransform().DirectionUp();
        up[2] = -up[2];

        up = invRot * up;
        Matrix3 orient(MUpAndDirection, up, VForward);
        orient = rot * orient;

        Matrix4 trans;
        trans.SetPosition(VZero);
        trans.SetOrientation(orient);
        Type()->_vario2.Apply(_shape, trans, level);
    }

    if (_mGunFireFrames > 0 || Glob.uiTime < _mGunFireTime + 0.05)
    {
        Type()->_animFire.Unhide(_shape, level);
        Type()->_animFire.SetPhase(_shape, level, _mGunFirePhase);
        _mGunFireFrames--;
    }
    else
    {
        Type()->_animFire.Hide(_shape, level);
    }

    // set gun and turret to correct position
    // calculate animation transformation
    // turret transformation
    _turret.Animate(Type()->_turret, this, level);

    DammageAnimation(level);

    base::Animate(level);
    //	SelectTexture(level,_rotorSpeed);
    // set minmax box and sphere
    Shape* shape = GetShape()->Level(level);
    float factor = 1.1;
    Vector3 min = shape->MinOrig();
    Vector3 max = shape->MaxOrig();
    Vector3 bCenter = shape->BSphereCenterOrig();
    float bRadius = shape->BSphereRadiusOrig();
    // enlarge twice to be sure soldier will fit it
    min = bCenter + (min - bCenter) * factor;
    max = bCenter + (max - bCenter) * factor;
    bRadius *= factor;
    shape->SetMinMax(min, max, bCenter, bRadius);
}

void Helicopter::Deanimate(int level)
{
    DammageDeanimation(level);

    base::Deanimate(level);
    Type()->_rotorH.Restore(_shape, level);
    Type()->_rotorV.Restore(_shape, level);
    //	SelectTexture(level,0);
    Type()->_hRotorStill.Unhide(_shape, level);
    Type()->_hRotorMove.Unhide(_shape, level);
    Type()->_vRotorStill.Unhide(_shape, level);
    Type()->_vRotorMove.Unhide(_shape, level);

    _turret.Deanimate(Type()->_turret, _shape, level);
}

void Helicopter::DoTransform(TLVertexTable& dst, const Shape& src, const Matrix4& posView, int from, int to) const
{
    // perform default transform
    base::DoTransform(dst, src, posView, from, to);
}

bool Helicopter::IsPossibleToGetIn() const
{
    if (GetHit(Type()->_engineHit) >= 0.5)
    {
        return false;
    }
    return base::IsPossibleToGetIn();
}

bool Helicopter::IsAbleToMove() const
{
    if (GetHit(Type()->_engineHit) >= 0.5)
    {
        return false;
    }
    return base::IsAbleToMove();
}

void Helicopter::Draw(int level, ClipFlags clipFlags, const FrameBase& pos)
{
    base::Draw(level, clipFlags, pos);

    Vector3 origin = Type()->_hudPosition;
    Vector3 right = Type()->_hudRight;
    Vector3 down = Type()->_hudDown;

    Vector3 normal = down.CrossProduct(right).Normalized();
    origin = origin - 0.002 * normal;

    if (right.SquareSize() < Square(0.001) || down.SquareSize() < Square(0.001))
    {
        return;
    }

    pos.PositionModelToWorld(origin, origin);
    pos.DirectionModelToWorld(right, right);
    pos.DirectionModelToWorld(down, down);

    PackedColor color(Color(0, 1, 0, 0.03));
    /*
        GEngine->DrawLine3D(origin, origin + right, color, 0);
        GEngine->DrawLine3D(origin + right, origin + right + down, color, 0);
        GEngine->DrawLine3D(origin + right + down, origin + down, color, 0);
        GEngine->DrawLine3D(origin + down, origin, color, 0);
    */
    Vector3 tl = origin + 0.25 * right;
    Vector3 tr = origin + 0.75 * right;
    Vector3 br = origin + 0.75 * right + down;
    Vector3 bl = origin + 0.25 * right + down;
    GEngine->DrawLine3D(tl, tr, color, 0);
    GEngine->DrawLine3D(tr, br, color, 0);
    GEngine->DrawLine3D(br, bl, color, 0);
    GEngine->DrawLine3D(bl, tl, color, 0);

    tl = origin + 0.25 * down;
    tr = origin + 0.25 * down + right;
    br = origin + 0.75 * down + right;
    bl = origin + 0.75 * down;
    GEngine->DrawLine3D(tl, tr, color, 0);
    GEngine->DrawLine3D(tr, br, color, 0);
    GEngine->DrawLine3D(br, bl, color, 0);
    GEngine->DrawLine3D(bl, tl, color, 0);

    // texts
    static Ref<Font> font;
    if (!font)
    {
        font = GEngine->LoadFont(GetFontID("tahomaB48"));
    }

    char text[256];
    PackedColor textColor(Color(0, 1, 0, 0.5));

    Vector3 textUp = -0.4 * 0.25 * down;
    Vector3 textRight = 0.75 * textUp.Size() * right.Normalized();
    float invRSize = 1.0 / textRight.Size();

    // fuel
    Vector3 textPos = origin + 0.6 * 0.25 * down;
    float value = 100.0f * GetFuel() / Type()->GetFuelCapacity();
    snprintf(text, sizeof(text), "%.0f%%", value);
    /*
    Vector3 width = GEngine->GetText3DWidth
    (
        textRight, font, text
    );
    */
    float x2c = 0.25 * right.Size() * invRSize;
    GEngine->DrawText3D(textPos, textUp, textRight, ClipAll | (static_cast<uint32_t>(MSShining) * ClipUserStep), font,
                        textColor, 0, text, 0, 0, x2c, 1);

    Target* target = GetFireTarget();
    if (target)
    {
        // target distance
        textPos = origin + 0.75 * right + 0.6 * 0.25 * down;
        value = target->AimingPosition().Distance(Position());
        snprintf(text, sizeof(text), "%.0f", value);
        x2c = 0.25 * right.Size() * invRSize;
        GEngine->DrawText3D(textPos, textUp, textRight, ClipAll | (static_cast<uint32_t>(MSShining) * ClipUserStep),
                            font, textColor, 0, text, 0, 0, x2c, 1);
    }

    // speed
    textPos = origin + 0.75 * down;
    value = 3.6 * fabs(ModelSpeed()[2]);
    snprintf(text, sizeof(text), "%.0f", value);
    x2c = 0.25 * right.Size() * invRSize;
    GEngine->DrawText3D(textPos, textUp, textRight, ClipAll | (static_cast<uint32_t>(MSShining) * ClipUserStep), font,
                        textColor, 0, text, 0, 0, x2c, 1);

    // height
    textPos = origin + 0.75 * right + 0.75 * down;
    value = Position().Y() + GetShape()->Min().Y() - GLandscape->SurfaceYAboveWater(Position().X(), Position().Z());
    snprintf(text, sizeof(text), "%d", toInt(value));
    x2c = 0.25 * right.Size() * invRSize;
    GEngine->DrawText3D(textPos, textUp, textRight, ClipAll | (static_cast<uint32_t>(MSShining) * ClipUserStep), font,
                        textColor, 0, text, 0, 0, x2c, 1);
}

HelicopterAuto::HelicopterAuto(VehicleType* name, Person* pilot)
    : Helicopter(name, pilot), _pilotHeading(0), _pilotSpeed(VZero), _pilotHeight(2.5), _pilotHeadingSet(false),
      _pilotDiveSet(false), _hoveringAutopilot(false), _dirCompensate(0.5), _defPilotHeight(50), _forceDive(1),
      _pilotDive(0), _pilotHeightHelper(true), _pilotSpeedHelper(true), _pilotDirHelper(true), _avoidBankJitter(false),
      _state(AutopilotNear), _pressedForward(false), _pressedBack(false), _pressedUp(false), _pressedDown(false),
      _sweepDelay(Glob.time), _sweepState(SweepDisengage), _stopMode(SMNone), _stopPosition(VZero)
{
}

void HelicopterAuto::Simulate(float deltaT, SimulationImportance prec)
{
    if (!_driver && !_isDead)
    {
        StopRotor();
    }

    SimulateUnits(deltaT);

    float massCoef = GetMass() * (1.0 / 3000);
    saturate(massCoef, 1, 3);

    float dirEstT = massCoef;
    const Matrix3& orientation = Orientation();

    Vector3Val avgAngVelocity = _angVelocity;
    Matrix3Val derOrientation = avgAngVelocity.Tilda() * orientation;

    Matrix3Val estOrientation = orientation + derOrientation * dirEstT;
    Vector3Val estDirection = estOrientation.Direction().Normalized();

    float bank = estOrientation.DirectionAside().Normalized().Y();
    float dive = estDirection.Y();

    float avoidGroundMRW = _mainRotorWanted;
    bool driverAlive = (_driver && _driver->Brain() && _driver && _driver->Brain()->GetLifeState() == AIUnit::LSAlive);
    if (_pilotHeightHelper && driverAlive)
    {
        // float estY=Position.Y()+_pilotSpeed*EST_DELTA+_acceleration*EST_DELTA*EST_DELTA*0.5;
        // estimate position after
        const float estT = 2;
        const float accPredict = 0.2; // estimate acceleration only partially
        Vector3 estPos = Position() + _speed * estT + 0.5 * accPredict * estT * estT * _acceleration;
        float estSurfaceY = GLOB_LAND->SurfaceYAboveWater(estPos.X(), estPos.Z());
        float estHeight = estPos.Y() - estSurfaceY;
        float minEstHeight = estHeight;
        if (_speed.SquareSizeXZ() > Square(5))
        {
            for (int t = 1; t < 3; t++)
            {
                float estT = t * 0.8;
                Vector3 estPos = Position() + _speed * estT + 0.5 * estT * estT * _acceleration;
                float estSurfaceY = GLOB_LAND->SurfaceYAboveWater(estPos.X(), estPos.Z());
                float estHeight = estPos.Y() - estSurfaceY;
                saturateMin(minEstHeight, estHeight);
            }
        }

        float changeAY = (_pilotHeight - minEstHeight) * 0.1;
        _mainRotorWanted = changeAY + _mainRotor;
        saturate(_mainRotorWanted, -1, 1);

        const float minHeight = 25;
        if (_pilotHeight >= minHeight)
        {
            float avoidGroundCAY = (_pilotHeight - minEstHeight) * 0.1;
            avoidGroundMRW = _mainRotor + avoidGroundCAY;
        }
    }

    const Vector3 relSpeed = ModelSpeed();
    float zPilotSpeed = _pilotSpeed[2];

    float estAccT = 3.0;
    if (_avoidBankJitter)
    {
        estAccT = 1.0;
    }
    Vector3 absSpeedWanted(VMultiply, DirModelToWorld(), _pilotSpeed);
    Vector3 changeAccel = (absSpeedWanted - _speed) * (1 / estAccT) - _acceleration;
    float bankLimit = 0.5;

    if (_pilotSpeedHelper && driverAlive)
    {
        DirectionWorldToModel(changeAccel, changeAccel);

        float minDive = -1, maxDive = +1;
        if (avoidGroundMRW >= 1.0)
        {
            float avoidW = floatMin(avoidGroundMRW - 1, 1);
            float speedDL = fabs(ModelSpeed()[2]) * (1.0 / 20);
            if (ModelSpeed()[2] > 5)
            {
                minDive = floatMin(+speedDL * avoidW, +0.5);
            }
            else if (ModelSpeed()[2] < -5)
            {
                maxDive = floatMax(-speedDL * avoidW, -0.5);
            }
            else
            {
                minDive = -0.2;
                maxDive = +0.2;
            }
            bankLimit = 1.0 - 0.8 * avoidW;
        }

        if (fabs(_forceDive) < 0.9)
        {
            _diveWanted = _forceDive;
        }
        else
        {
            float fastDive = fabs(zPilotSpeed) * (1.0 / 70);
            Limit(fastDive, 0.2, 0.7);
            float normalDive = (dive + changeAccel[2]) * (-0.2) * (1 - fastDive);
            _diveWanted = normalDive + zPilotSpeed * (-0.3 / 82) * fastDive;
        }
        saturate(_diveWanted, minDive, maxDive);
    }

    if (_pilotDirHelper && driverAlive)
    {
        float dirC = 1.0f;
        Vector3 direction = Direction() * (1 - dirC) + estDirection * dirC;
        float curHeading = atan2(direction[0], direction[2]);
        float changeHeading = AngleDifference(_pilotHeading, curHeading) * 8;
        Limit(changeHeading, -1, 1);
        // when slow, use back rotor
        float fastTurn = floatMax(fabs(relSpeed[2]) - 6, 0) * FAST_COEF;
        Limit(fastTurn, 0, 1);
        _backRotorWanted = -changeHeading * 2;
        saturate(_backRotorWanted, -1, +1);

        if (fabs(_forceBank) < 0.9)
        {
            _bankWanted = _forceBank;
        }
        else
        {
            float bankInTurn = fabs(zPilotSpeed) * FAST_COEF;
            saturateMin(bankInTurn, 1);

            float turnByBank = (fabs(relSpeed[2]) + fabs(_pilotSpeed[2]) - 4) * 0.05;
            saturate(turnByBank, 0, 1);
            saturateMax(turnByBank, bankInTurn);

            // when moving fast, turn by banking
            // float bankWantedFast=-changeHeading*2*turnByBank;
            // float bankWantedFast=-changeHeading*0.5f*turnByBank;
            float bankWantedFast = -changeHeading * turnByBank;

            if (_avoidBankJitter)
            {
                float thold = 0.1;
                if (fabs(bankWantedFast) < thold)
                {
                    bankWantedFast = 0;
                }
            }

            _bankWanted = bankWantedFast;
            if (_pilotSpeedHelper)
            {
                float bankWantedSlow = bank + changeAccel[0] * (-1.0 / 20);
                if (fabs(changeAccel[0]) < 1 && fabs(_pilotSpeed[0]) < 1)
                {
                    saturate(bankWantedSlow, -0.1, +0.1);
                }
                bankWantedSlow *= 1 - turnByBank;

                _bankWanted += bankWantedSlow;
            }

            saturate(_bankWanted, -0.5, 0.5);
        }
    }

    if (_pilotSpeedHelper && driverAlive)
    {
        // bank is limited
        // if( fabs(_diveWanted)>0.3 )
        // if diving, limit bank
        saturate(_diveWanted, -0.7, 0.6);
        float diveLimitBank = 0.5 - floatMax(fabs(_diveWanted), fabs(dive));
        saturate(diveLimitBank, 0.1, 0.8);
        saturateMin(bankLimit, diveLimitBank);

        saturate(_bankWanted, -bankLimit, +bankLimit);
    }

    if (!_driver || _driver->IsDammageDestroyed())
    {
        if (!_landContact)
        {
            _cyclicAsideWanted = -0.1;
            _cyclicForwardWanted = 0.1;
            _backRotorWanted = -0.1;
            _mainRotorWanted = 0.1;
        }
        else
        {
            _cyclicAsideWanted = 0;
            _cyclicForwardWanted = 0;
            _backRotorWanted = 0;
            _mainRotorWanted = 0;
        }
    }
    else
    {
        if (_rotorSpeed >= 0.3)
        {
            if (!_landContact)
            {
                _cyclicAsideWanted = +(_bankWanted - bank) * 1.0f;

                float minRotorDive = floatMin(Type()->_minMainRotorDive, Type()->_minBackRotorDive);
                float maxRotorDive = floatMax(Type()->_maxMainRotorDive, Type()->_maxBackRotorDive);

                float diveByRotor = _diveWanted;
                saturate(diveByRotor, minRotorDive, maxRotorDive);
                _rotorDiveWanted = -diveByRotor;

                float adjustedDive = _diveWanted - diveByRotor;
                _cyclicForwardWanted = -(adjustedDive - dive) * 16.0f;
                Limit(_cyclicForwardWanted, -1, +1);
            }
            else
            {
                _cyclicAsideWanted = 0;
                _cyclicForwardWanted = 0;
                _backRotorWanted = 0;
                _rotorDiveWanted = 0;
            }
        }
        else
        {
            // no controls available - no engine power
            _cyclicAsideWanted = 0;
            _cyclicForwardWanted = 0;
            _backRotorWanted = 0;
            _mainRotorWanted = 0.3;
            _rotorDiveWanted = 0;
        }

        if (_angVelocity.SquareSize() >= 10 * 10)
        { // if we are rotating fast, leave all controls in neutral position
            _cyclicAsideWanted = 0;
            _cyclicForwardWanted = 0;
            _backRotorWanted = 0;
            _mainRotorWanted = 0.3;
            _rotorDiveWanted = 0;
        }
    }

    MoveWeapons(deltaT);
    base::Simulate(deltaT, prec);
}

} // namespace Poseidon
