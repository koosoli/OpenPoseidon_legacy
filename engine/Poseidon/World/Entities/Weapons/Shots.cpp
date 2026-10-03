#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Dev/Diag/OpDiag.hpp> // DIAG-001 hooks, empty unless POSEIDON_DIAG
#include <Poseidon/World/Scene/CargoSurface.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/Core/Config/UserConfig.hpp>
#include <Poseidon/World/Entities/Weapons/Shots.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Random/randomGen.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Weather/WindModel.hpp>
#include <Poseidon/World/Weather/SnowField.hpp>
#include <Poseidon/World/Weather/SnowBulletImpact.hpp>
#include <Poseidon/Dev/Diag/BallisticsRecorder.hpp>
#include <Poseidon/Dev/Diag/PhysicsProbe.hpp>
#include <Poseidon/Dev/Diag/PhysicsRayAudit.hpp>
#include <Poseidon/World/Entities/Weapons/GrenadeFuse.hpp>
#include <Poseidon/World/Entities/Weapons/Penetration.hpp>
#include <Poseidon/World/Physics/PhysicsWorld.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/AI/VehicleAI.hpp>
#include <Poseidon/AI/AI.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <Poseidon/World/Entities/Weapons/Weapons.hpp>
#include <Poseidon/Network/Network.hpp>
#include <Poseidon/Graphics/Rendering/WaterInteractionBridge.hpp>
#include <Poseidon/Graphics/Rendering/Effects/Smokes.hpp>
#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp>
#include <Poseidon/Graphics/Textures/TexturePreload.hpp>
#include <Poseidon/Graphics/Rendering/Draw/SpecLods.hpp>
#include <Poseidon/World/Scene/ObjLine.hpp>
#include <Poseidon/World/Entities/Weapons/Ballistics.hpp>

#include <Poseidon/Foundation/Enums/EnumNames.hpp>

#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/Game/Commands/GameStateExt.hpp>
#include <float.h>
#include <stdint.h>
#include <cmath>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/StreamArray.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>
#include <Poseidon/Foundation/Math/MathOpt.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>

using namespace Poseidon;
namespace Poseidon
{
RString GetMissionDirectory();

namespace
{
bool SnowBulletTraceEnabled()
{
    static const bool enabled = [] {
#if defined(_WIN32)
        char* value = nullptr; size_t size = 0;
        _dupenv_s(&value, &size, "POSEIDON_SNOW_BULLET_TRACE");
        const bool result = value && value[0] == '1'; std::free(value); return result;
#else
        const char* value = std::getenv("POSEIDON_SNOW_BULLET_TRACE");
        return value && value[0] == '1';
#endif
    }();
    return enabled;
}

// Developer telemetry only: short rifle flights can terminate before the
// per-frame ballistic recorder sees them. Observe the actual terminal site;
// a missing snow edit alone cannot distinguish a roof hit from covered ground.
void SnowBulletTerminalTrace(const char* kind, const Vector3& point,
    const Vector3& velocity, Object* hit, bool local, bool explosive)
{
    if (!SnowBulletTraceEnabled() || !local || explosive) return;
    const double now = Glob.time.toFloat();
    if (!std::isfinite(now)) return;
    static double previousBucket = -1;
    static unsigned rows = 0;
    const double bucket = std::floor(now * 4.0);
    if (bucket != previousBucket) { previousBucket = bucket; rows = 0; }
    if (rows >= 16) return;
    ++rows;
    const RString name = hit ? hit->GetDebugName() : RString("");
    LOG_INFO(World,"SNOW_BULLET_TERMINAL kind={} time={:.3f} x={:.3f} y={:.3f} z={:.3f} vx={:.3f} vy={:.3f} vz={:.3f} hit=\"{}\"",
        kind, now, point.X(),point.Y(),point.Z(),velocity.X(),velocity.Y(),velocity.Z(),static_cast<const char*>(name));
}

void SnowBulletTerrainImpact(Object* projectile, Object* owner, const Vector3& point,
    const Vector3& incoming, float speed, float damage, bool local, bool explosive, bool water)
{
    if (!GLandscape || !local || explosive || water || !GSnow().CoverActive()) return;
    const float x = point.X(), z = point.Z();
    const float extent = GLandscape->GetLandRange() * GLandscape->GetLandGrid();
    if (!std::isfinite(x) || !std::isfinite(point.Y()) || !std::isfinite(z) ||
        !std::isfinite(extent) || x < 0 || z < 0 || x >= extent || z >= extent) return;
    const float base = GSnow().BaseDepthAt(x,z);
    const int ix = int(std::floor(x / SnowField::CellSize)), iz = int(std::floor(z / SnowField::CellSize));
    const float before = GSnow().DeficitAt(ix,iz);
    const float remaining = std::max(0.0f,base-before);
    float dx = 0, dz = 0;
    const float ground = GLandscape->SurfaceY(x,z,&dx,&dz);
    const Vector3 normal = Vector3(-dx,1.0f,-dz).Normalized();
    auto cut = SnowBulletImpactPolicy(speed,damage,remaining,normal.Y(),incoming*normal,
                                     point.Y()-ground,local,explosive,water,false,false);
    const auto groove = SnowBulletGroovePolicy(remaining,normal.Y(),incoming*normal,incoming.X(),incoming.Z());
    bool roadway = false, sheltered = false;
    bool changed = false;
    static SnowBulletBudget budget;
    const bool land = std::isfinite(ground) && ground > GLandscape->GetSeaLevel()+0.03f;
    const bool budgetGranted = cut.depth > 0 && land && budget.Take(Glob.time.toFloat());
    if (budgetGranted)
    {
        // Probe the centre and four corners of the complete oriented capsule's
        // enclosing rectangle, including its upstream snow-entry endpoint.
        const float halfLength = groove.length * 0.5f;
        const Vector3 centre(-groove.directionX*halfLength,0,-groove.directionZ*halfLength);
        const Vector3 along = groove.length > 0 ? Vector3(groove.directionX,0,groove.directionZ) : Vector3(1,0,0);
        const Vector3 across(-along.Z(),0,along.X());
        const Vector3 a = along*(cut.radius+halfLength), b = across*cut.radius;
        const Vector3 offsets[] = {centre,centre+a+b,centre+a-b,centre-a+b,centre-a-b};
        for (const Vector3& offset : offsets)
        {
            const Vector3 sample = point+offset;
            if (sample.X() < 0 || sample.Z() < 0 || sample.X() >= extent || sample.Z() >= extent) {roadway = true; break;}
            const float y = GLandscape->SurfaceY(sample.X(),sample.Z());
            if (!std::isfinite(y) || y <= GLandscape->GetSeaLevel()+0.03f) {roadway = true; break;}
            const Vector3 origin(sample.X(),y+0.015f,sample.Z());
            Object* road = nullptr;
            const float support = GLandscape->RoadSurfaceY(origin,nullptr,nullptr,nullptr,&road);
            if (road || !std::isfinite(support) || support > y+0.03f) {roadway = true; break;}
            CollisionBuffer hits;
            // Physical sky proof, independent of artistic AO and camera maps.
            // Ignore only this projectile and shooter, retaining actual roofs.
            GLandscape->ObjectCollision(hits,projectile,owner,origin,origin+Vector3(0,30,0),0.01f,ObjIntersectView);
            if (hits.Size()) {sheltered = true; break;}
        }
        cut = SnowBulletImpactPolicy(speed,damage,remaining,normal.Y(),incoming*normal,
                                     point.Y()-ground,local,explosive,water,roadway,sheltered);
        if (cut.depth > 0) changed = GSnow().BulletImpact(x,z,cut.radius,cut.depth,
            groove.directionX,groove.directionZ,groove.length);
    }
    if (SnowBulletTraceEnabled())
        LOG_INFO(World, "SNOW_BULLET terrain=1 changed={} x={:.3f} y={:.3f} z={:.3f} speed={:.2f} base={:.4f} remaining={:.4f} radius={:.4f} depth={:.4f} before={:.4f} after={:.4f} roadway={} sheltered={} land={} budget={} up={:.4f} incidence={:.4f} supportError={:.4f} directionX={:.5f} directionZ={:.5f} grooveLength={:.5f}",
                 changed,x,point.Y(),z,speed,base,remaining,cut.radius,cut.depth,before,GSnow().DeficitAt(ix,iz),roadway,sheltered,land,budgetGranted,normal.Y(),incoming*normal,point.Y()-ground,groove.directionX,groove.directionZ,groove.length);
}
} // namespace
}

using Poseidon::Foundation::EnumName;

namespace
{
/// Airspeed = ground speed minus the air's own motion. Drag acts on THAT, which
/// is the only physically correct place for wind to enter a trajectory: a
/// crosswind does not push the round sideways directly, it makes the round's
/// relative airflow oblique and the drag vector tilts.
///
/// OFF BY DEFAULT (WindModel::BallisticsEnabled). Classic OFP missions were
/// authored against a windless solution and this engine's standing constraint
/// is that they still play as they did — at full overcast the unified wind
/// reaches ~9 m/s mean, which over a 600 m shot is a real deflection. With the
/// flag false this returns `speed` unchanged, so the caller's expression is
/// byte-identical to the pre-wind code and no classic mission moves an inch.
inline Vector3 ShotAirspeed(Vector3Par speed)
{
    if (!Poseidon::WindModel::BallisticsEnabled())
    {
        return speed;
    }
    const Poseidon::WindSample& wind = Poseidon::GWind.Sample();
    return speed - Vector3(wind.velocityX, 0.0f, wind.velocityZ);
}
} // namespace

#if _ENABLE_CHEATS
#define ARROWS 1
#else
#define ARROWS 0
#endif

DEFINE_FAST_ALLOCATOR(Shot)
DEFINE_CASTING(Shot)

Shot::Shot(EntityAI* parent, const AmmoType* type) : base(type->GetShape(), type, -1), _parent(parent)
{
    if (type->GetShape() == nullptr)
    {
        Fail("No shape");
        LOG_DEBUG(Physics, "Type {}", (const char*)type->GetName());
    }
    _timeToLive = 10.0;
    SetSimulationPrecision(type->simulationStep);
}

// Malprave aiSuppression: suppression - rounds passing close to infantry pin them down
namespace SuppressionTuning
{
constexpr float Radius = 4.0f;          // metres from the round's path that still counts as a near miss
constexpr float MinShooterDist = 10.0f; // ignore units right next to the shooter (muzzle blast)
constexpr float PerRound = 0.35f;       // suppression from one rifle round passing at 0 m
constexpr float HitRef = 10.0f;         // ammo 'hit' of a normal rifle round (scales bigger calibres up)
constexpr float MaxPower = 4.0f;        // cap for very heavy rounds
} // namespace SuppressionTuning

static void ApplyNearMissSuppression(EntityAI* shooter, const AmmoType* type, Vector3Par from, Vector3Par to)
{
    using namespace SuppressionTuning;
    const Vector3 seg = to - from;
    const float segLen2 = seg.SquareSize();
    if (segLen2 < 1e-4f)
    {
        return;
    }
    // quick reject: bounding sphere of the step
    const Vector3 mid = (from + to) * 0.5f;
    const float reach2 = Square(sqrt(segLen2) * 0.5f + Radius);

    AIUnit* shooterUnit = shooter ? shooter->CommanderUnit() : nullptr;
    AIGroup* shooterGroup = shooterUnit ? shooterUnit->GetGroup() : nullptr;
    float power = type->hit / HitRef;
    saturate(power, 0.5f, MaxPower);

    for (int i = 0; i < GWorld->NVehicles(); i++)
    {
        EntityAI* ai = dyn_cast<EntityAI>(GWorld->GetVehicle(i));
        if (!ai || ai == shooter)
        {
            continue;
        }
        const Vector3 p = ai->AimingPosition();
        if (p.Distance2(mid) > reach2)
        {
            continue;
        }
        AIUnit* unit = ai->CommanderUnit();
        // infantry on foot only (crews inside vehicles are protected), local AI only
        if (!unit || static_cast<EntityAI*>(unit->GetPerson()) != ai || !unit->IsLocal() || unit->IsAnyPlayer())
        {
            continue;
        }
        if (shooterGroup && unit->GetGroup() == shooterGroup)
        {
            continue; // own squad's fire does not pin you down
        }
        if (shooter && shooter->Position().Distance2(p) < Square(MinShooterDist))
        {
            continue;
        }
        float t = ((p - from) * seg) / segLen2;
        saturate(t, 0, 1);
        const float d2 = (from + seg * t).Distance2(p);
        if (d2 > Square(Radius))
        {
            continue;
        }
        const float d = sqrt(d2);
        unit->AddSuppression(PerRound * power * (1 - d / Radius), shooter);
    }
}


void Shot::SetParent(EntityAI* parent)
{
    _parent = parent;
}

#ifdef NDEBUG
bool Shot::Invisible() const
{
    return _speed.Distance2(GLOB_SCENE->GetCamera()->Speed()) > Square(400);
}
#endif

void Shot::Sound(bool inside, float deltaT)
{
    const SoundPars& sound = Type()->_soundFly;
    if (!_sound && sound.name.GetLength() > 0)
    {
        _sound = GSoundScene->OpenAndPlay(sound.name, Position(), Speed());
    }
    if (_sound)
    {
        const Camera& camera = *GLOB_SCENE->GetCamera();
        Vector3 posToCamera = camera.Position() - Position();
        float speedCoef = 1;
        _sound->SetVolume(sound.vol * speedCoef, sound.freq);
        _sound->SetPosition(Position(), Speed());
    }
}

void Shot::UnloadSound()
{
    _sound.Free();
}

LSError Shot::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    PARAM_CHECK(ar.SerializeRef("Parent", _parent, 1))
    return LSOK;
}

NetworkMessageType Shot::GetNMType(NetworkMessageClass cls) const
{
    switch (cls)
    {
        case NMCCreate:
            return NMTCreateShot;
        case NMCUpdateGeneric:
        case NMCUpdateDammage:
        case NMCUpdatePosition:
            return NMTNone;
        default:
            return base::GetNMType(cls);
    }
}

class IndicesCreateShot : public IndicesCreateVehicle
{
    typedef IndicesCreateVehicle base;

  public:
    int parent;
    int timeToLive;
    int createPos;
    int createSpeed;
    int createOrient;

    IndicesCreateShot();
    NetworkMessageIndices* Clone() const override { return new IndicesCreateShot; }
    void Scan(NetworkMessageFormatBase* format) override;
};

IndicesCreateShot::IndicesCreateShot()
{
    parent = -1;
    timeToLive = -1;
    createPos = -1;
    createSpeed = -1;
    createOrient = -1;
}

void IndicesCreateShot::Scan(NetworkMessageFormatBase* format)
{
    base::Scan(format);

    SCAN(parent)
    SCAN(timeToLive)
    SCAN(createPos)
    SCAN(createSpeed)
    SCAN(createOrient)
}

NetworkMessageIndices* GetIndicesCreateShot()
{
    return new IndicesCreateShot();
}

class IndicesUpdateShot : public IndicesUpdateVehicle
{
    typedef IndicesUpdateVehicle base;

  public:
    IndicesUpdateShot();
    NetworkMessageIndices* Clone() const override { return new IndicesUpdateShot; }
    void Scan(NetworkMessageFormatBase* format) override;
};

IndicesUpdateShot::IndicesUpdateShot() = default;

void IndicesUpdateShot::Scan(NetworkMessageFormatBase* format)
{
    base::Scan(format);
}

NetworkMessageIndices* GetIndicesUpdateShot()
{
    return new IndicesUpdateShot();
}

NetworkMessageFormat& Shot::CreateFormat(NetworkMessageClass cls, NetworkMessageFormat& format)
{
    Vector3 temp = VZero;
    Matrix3 tempM = M3Identity;
    switch (cls)
    {
        case NMCCreate:
            base::CreateFormat(cls, format);
            format.Add("parent", NDTRef, NCTNone, DEFVALUENULL, DOC_MSG("Owner of shot"));
            format.Add("timeToLive", NDTFloat, NCTNone, DEFVALUE(float, 10), DOC_MSG("Time to live (in seconds)"));
            format.Add("createPos", NDTVector, NCTNone, DEFVALUE(Vector3, temp), DOC_MSG("Initial position"));
            format.Add("createSpeed", NDTVector, NCTNone, DEFVALUE(Vector3, temp), DOC_MSG("Initial speed"));
            format.Add("createOrient", NDTMatrix, NCTMatrixOrientation, DEFVALUE(Matrix3, tempM),
                       DOC_MSG("Initial orientation"));
            break;
        case NMCUpdateGeneric:
            base::CreateFormat(cls, format);
            break;
        default:
            base::CreateFormat(cls, format);
            break;
    }
    return format;
}

Shot* Shot::CreateObject(NetworkMessageContext& ctx)
{
    base* veh = base::CreateObject(ctx);
    Shot* shot = dyn_cast<Shot>(veh);
    if (!shot)
    {
        return nullptr;
    }

    if (shot->TransferMsg(ctx) != TMOK)
    {
        return nullptr;
    }
    return shot;
}

TMError Shot::TransferMsg(NetworkMessageContext& ctx)
{
    switch (ctx.GetClass())
    {
        case NMCCreate:
            if (ctx.IsSending())
            {
                TMCHECK(base::TransferMsg(ctx))
            }
            {
                PoseidonAssert(dynamic_cast<const IndicesCreateShot*>(ctx.GetIndices()))
                    const IndicesCreateShot* indices = static_cast<const IndicesCreateShot*>(ctx.GetIndices());

                ITRANSF_REF(parent)
                ITRANSF(timeToLive)
                TMCHECK(ctx.IdxTransfer(indices->createSpeed, _speed))
                if (ctx.IsSending())
                {
                    Vector3 pos = Position();
                    Matrix3 orient = Orientation();
                    TMCHECK(ctx.IdxTransfer(indices->createPos, pos))
                    TMCHECK(ctx.IdxTransfer(indices->createOrient, orient))
                }
                else
                {
                    Vector3 pos;
                    Matrix3 orient;
                    pos.Init();
                    TMCHECK(ctx.IdxTransfer(indices->createPos, pos))
                    TMCHECK(ctx.IdxTransfer(indices->createOrient, orient))
                    SetPosition(pos);
                    SetOrientation(orient);
                }
            }
            break;
        case NMCUpdateGeneric:
            TMCHECK(base::TransferMsg(ctx))
            break;
        default:
            TMCHECK(base::TransferMsg(ctx))
            break;
    }
    return TMOK;
}

float Shot::CalculateError(NetworkMessageContext& ctx)
{
    return base::CalculateError(ctx);
}

float Shot::GetMaxPredictionTime(NetworkMessageContext& ctx) const
{
    // no position updates are sent for shots, so long prediction is necessary
    return 100;
}

template <>
const EnumName* Poseidon::Foundation::GetEnumNames(Missile::EngineState dummy)
{
    static const EnumName EngineStateNames[] = {EnumName(Missile::Init, "INIT"), EnumName(Missile::Thrust, "THRUST"),
                                                EnumName(Missile::Fly, "FLY"), EnumName()};
    return EngineStateNames;
}

template <>
const EnumName* Poseidon::Foundation::GetEnumNames(Missile::LockState dummy)
{
    static const EnumName LockStateNames[] = {EnumName(Missile::Locked, "LOCKED"), EnumName(Missile::Lost, "LOST"),
                                              EnumName()};
    return LockStateNames;
}

LSError Missile::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    PARAM_CHECK(ar.SerializeRef("Target", _target, 1))
    PARAM_CHECK(ar.Serialize("thrust", _thrust, 1))
    PARAM_CHECK(ar.SerializeEnum("engine", _engine, 1))
    PARAM_CHECK(ar.SerializeEnum("lock", _lock, 1))
    return LSOK;
}

NetworkMessageType Missile::GetNMType(NetworkMessageClass cls) const
{
    switch (cls)
    {
        case NMCCreate:
            return NMTCreateShot;
        case NMCUpdateGeneric:
        case NMCUpdateDammage:
            return base::GetNMType(cls);
        case NMCUpdatePosition:
            return Entity::GetNMType(cls);
        default:
            return base::GetNMType(cls);
    }
}

float Missile::CalculateError(NetworkMessageContext& ctx)
{
    return base::CalculateError(ctx);
}

float Missile::GetMaxPredictionTime(NetworkMessageContext& ctx) const
{
    return 100;
}

const float MinExplosion = 0.25;
const float MaxExplosion = 1;

DEFINE_FAST_ALLOCATOR(PipeBomb)
DEFINE_CASTING(PipeBomb)

PipeBomb::PipeBomb(EntityAI* parent, const AmmoType* type) : Shot(parent, type)
{
    _explosion = false;
    _timeToLive = FLT_MAX;
}

void PipeBomb::Simulate(float deltaT, SimulationImportance prec)
{
    _timeToLive -= deltaT;
    if (IsLocal() && (_explosion || _timeToLive < 0))
    {
        if (Type()->explosive)
        {
            float size = Type()->hit * 0.003;
            saturate(size, MinExplosion, MaxExplosion);
            Explosion* explosion = new Explosion(nullptr, _parent, size);
            explosion->SetPosition(Position());
            GLOB_WORLD->AddAnimal(explosion);
            GetNetworkManager().CreateVehicle(explosion, VLTAnimal, "", -1);
        }
        GLandscape->ExplosionDammage(_parent, this, nullptr, Position(), VUp, Type());
        _delete = true;
    }
}

DEFINE_FAST_ALLOCATOR(TimeBomb)
DEFINE_CASTING(TimeBomb)

TimeBomb::TimeBomb(EntityAI* parent, const AmmoType* type) : Shot(parent, type)
{
    _timeToLive = 20;
}

void TimeBomb::Simulate(float deltaT, SimulationImportance prec)
{
    _timeToLive -= deltaT;
    if (IsLocal() && _timeToLive < 0)
    {
        if (Type()->explosive)
        {
            float size = Type()->hit * 0.003;
            saturate(size, MinExplosion, MaxExplosion);
            Explosion* explosion = new Explosion(nullptr, _parent, size);
            explosion->SetPosition(Position());
            GLOB_WORLD->AddAnimal(explosion);
            GetNetworkManager().CreateVehicle(explosion, VLTAnimal, "", -1);
        }
        GLandscape->ExplosionDammage(_parent, this, nullptr, Position(), VUp, Type());
        _delete = true;
    }
}

DEFINE_FAST_ALLOCATOR(Mine)
DEFINE_CASTING(Mine)

Mine::Mine(EntityAI* parent, const AmmoType* type) : Shot(parent, type)
{
    _timeToLive = GRandGen.PlusMinus(0.5, 0.1);
    {
        const ParamEntry& pars = _type->GetParamEntry();
        const ParamEntry* entry = pars.FindEntry("activationTime");
        if (entry)
        {
            const float activationTime = *entry;
            if (activationTime >= 0)
            {
                _timeToLive = GRandGen.PlusMinus(activationTime + 0.2, 0.1);
            }
        }
    }
    _active = true;
}

void Mine::Simulate(float deltaT, SimulationImportance prec)
{
    if (!_active)
    {
        // tilt aside to indicate mine is no longer active
        float bank = DirectionAside().Y();
        float bankWanted = 0.5;
        if (bank >= bankWanted)
        {
            return;
        }
        float delta = bankWanted - bank;
        saturate(delta, -deltaT * 0.5f, +deltaT * 0.5f);
        Matrix3 rotZ(MRotationZ, delta);

        Matrix3 newOrient = Orientation() * rotZ;
        SetOrientation(newOrient);
        return;
    }

    _timeToLive -= deltaT;
    if (IsLocal() && _timeToLive < 0)
    {
        float activationMass = 10000.0;
        {
            const ParamEntry& pars = _type->GetParamEntry();
            const ParamEntry* entry = pars.FindEntry("activationMass");
            if (entry)
            {
                activationMass = *entry;
            }
        }
        float activationDistance = Square(6.0);
        {
            // activationDistance config value is the square of the actual distance
            const ParamEntry& pars = _type->GetParamEntry();
            const ParamEntry* entry = pars.FindEntry("activationDistance");
            if (entry)
            {
                activationDistance = *entry;
            }
        }

        bool found = false;
        for (int i = 0; i < GWorld->NVehicles(); i++)
        {
            Vehicle* veh = GWorld->GetVehicle(i);
            if (!veh)
            {
                continue;
            }
            if (veh->GetMass() < activationMass)
            {
                continue;
            }
            if (Position().Distance2(veh->Position()) > activationDistance)
            {
                continue;
            }
            found = true;
            break;
        }
        if (found)
        {
            if (Type()->explosive)
            {
                float size = Type()->hit * 0.003;
                saturate(size, MinExplosion, MaxExplosion);
                Explosion* explosion = new Explosion(nullptr, _parent, size);
                explosion->SetPosition(Position());
                GLOB_WORLD->AddAnimal(explosion);
                GetNetworkManager().CreateVehicle(explosion, VLTAnimal, "", -1);
            }
            GLandscape->ExplosionDammage(_parent, this, nullptr, Position(), VUp, Type());
            _delete = true;
        }
        else
        {
            _timeToLive = GRandGen.PlusMinus(0.5, 0.1);
        }
    }
}

LSError Mine::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    PARAM_CHECK(ar.Serialize("active", _active, 1, true))
    return LSOK;
}

NetworkMessageType Mine::GetNMType(NetworkMessageClass cls) const
{
    switch (cls)
    {
        case NMCCreate:
            return NMTCreateShot;
        case NMCUpdateGeneric:
            return NMTUpdateMine;
        case NMCUpdateDammage:
        case NMCUpdatePosition:
            return NMTNone;
        default:
            return base::GetNMType(cls);
    }
}

class IndicesUpdateMine : public IndicesUpdateShot
{
    typedef IndicesUpdateShot base;

  public:
    int active;

    IndicesUpdateMine();
    NetworkMessageIndices* Clone() const override { return new IndicesUpdateMine; }
    void Scan(NetworkMessageFormatBase* format) override;
};

IndicesUpdateMine::IndicesUpdateMine()
{
    active = -1;
}

void IndicesUpdateMine::Scan(NetworkMessageFormatBase* format)
{
    base::Scan(format);
    SCAN(active)
}

NetworkMessageIndices* GetIndicesUpdateMine()
{
    return new IndicesUpdateMine();
}

NetworkMessageFormat& Mine::CreateFormat(NetworkMessageClass cls, NetworkMessageFormat& format)
{
    switch (cls)
    {
        case NMCUpdateGeneric:
            base::CreateFormat(cls, format);
            format.Add("active", NDTBool, NCTNone, DEFVALUE(bool, true), DOC_MSG("Mine is active (can explode)"),
                       ET_ABS_DIF, ERR_COEF_MODE);
            break;
        default:
            base::CreateFormat(cls, format);
            break;
    }
    return format;
}

TMError Mine::TransferMsg(NetworkMessageContext& ctx)
{
    switch (ctx.GetClass())
    {
        case NMCUpdateGeneric:
            TMCHECK(base::TransferMsg(ctx))
            {
                PoseidonAssert(dynamic_cast<const IndicesUpdateMine*>(ctx.GetIndices()))
                    const IndicesUpdateMine* indices = static_cast<const IndicesUpdateMine*>(ctx.GetIndices());
                ITRANSF(active)
            }
            break;
        default:
            TMCHECK(base::TransferMsg(ctx))
            break;
    }
    return TMOK;
}

float Mine::CalculateError(NetworkMessageContext& ctx)
{
    float error = 0;
    switch (ctx.GetClass())
    {
        case NMCUpdateGeneric:
            error += base::CalculateError(ctx);
            {
                PoseidonAssert(dynamic_cast<const IndicesUpdateMine*>(ctx.GetIndices()))
                    const IndicesUpdateMine* indices = static_cast<const IndicesUpdateMine*>(ctx.GetIndices());

                ICALCERR_NEQ(bool, active, ERR_COEF_MODE)
            }
            break;
        default:
            error += base::CalculateError(ctx);
            break;
    }
    return error;
}

DEFINE_FAST_ALLOCATOR(ShotShell)
DEFINE_CASTING(ShotShell)

ShotShell::ShotShell(EntityAI* parent, const AmmoType* type) : base(parent, type)
{
    // The four flight parameters now come from ONE place, shared with the aim predictor
    // (Ballistics::ShellParamsFor). They used to be assigned here and re-derived by whatever
    // wanted to predict a trajectory, which is how the AI ended up aiming with a drag-free model
    // while these rounds flew with drag. Anything that wants to know how a shell flies must ask
    // the same function this constructor does. `coefGravity` and `timeToLive` are still read from
    // the ammo config there, exactly as they were here.
    const Poseidon::Ballistics::ShellParams shell = Poseidon::Ballistics::ShellParamsFor(*type);
    _initDelay = shell.initTime;
    _timeToLive = shell.timeToLive;
    _airFriction = shell.airFriction;
    _coefGravity = shell.coefGravity;
    _dragModel = shell.dragModel;
    _ballisticCoefficient = shell.ballisticCoefficient;

    _waterImpactDone = false;

    // Zero for every stock round -- see AmmoType::explosionTime. A grenade only
    // gets a fuse if its config asks for one.
    _fuse = type->explosionTime;
    // The panel override wins when set, so a delay can be tried without
    // rebuilding -- but only for ammo the CONFIG already gave a fuse. It changes
    // how long, never which rounds, so flares and smoke shells (which also
    // descend from Grenade) cannot acquire one by moving a slider.
    const GrenadeFuse::Settings& tuning = GrenadeFuse::Get();
    if (_fuse > 0.0f && tuning.fuseOverrideSeconds > 0.0f)
    {
        _fuse = tuning.fuseOverrideSeconds;
    }
    _fuseArmed = tuning.enabled && _fuse > 0.0f;
}

/// Reflection off a surface, with the energy a real bounce loses.
///
/// The two coefficients are not measurements of anything: a hand grenade is an
/// irregular lump of steel and how it bounces depends on where it lands. They
/// are chosen so it kicks off a wall, loses most of its speed doing so, and
/// settles within a metre or two rather than skating across the map.
bool ShotShell::BounceOff(Vector3Par surfaceNormal, Vector3Par contactPoint)
{
    // Lifted off the surface by a few centimetres. Without this the next tick
    // starts its sweep exactly on the face it just left and finds the same
    // contact again, which reads as a grenade stuck buzzing against a wall.
    SetPosition(contactPoint + surfaceNormal * 0.03f);

    const GrenadeFuse::BounceResult bounce = GrenadeFuse::Bounce(_speed, surfaceNormal, GrenadeFuse::Get());
    _speed = bounce.speed;
    _atRest = bounce.atRest;
    return !bounce.atRest;
}

/// The detonation itself, with no opinion about what triggered it.
void ShotShell::Detonate(Vector3Par pos, Object* hitObject, Vector3Par hitNormal)
{
    if (!IsLocal())
    {
        return;
    }
    Vector3 exploPos = pos;
    if (Type()->explosive)
    {
        // The 0.003 of the ground path, not the 0.001 of the object path. The
        // latter sits behind a `hit > 50` gate that a hand grenade (hit = 20)
        // never passes, so using it would give a fused grenade no visible
        // explosion at all -- which is exactly the sort of thing that reads as a
        // broken build rather than a chosen constant.
        float size = Type()->hit * 0.003;
        saturate(size, MinExplosion, MaxExplosion);
        exploPos[1] += 0.5 * size;
        Explosion* explosion = new Explosion(nullptr, _parent, size);
        explosion->SetPosition(exploPos);
        GLOB_WORLD->AddAnimal(explosion);
        GetNetworkManager().CreateVehicle(explosion, VLTAnimal, "", -1);
    }
    GLandscape->ExplosionDammage(_parent, this, hitObject, pos, hitNormal, Type());
}

bool ShotShell::Invisible() const
{
    if (_initDelay > 0)
    {
        return true;
    }
    return base::Invisible();
}

void ShotShell::Sound(bool inside, float deltaT)
{
    if (_initDelay > 0)
    {
        return;
    }
    base::Sound(inside, deltaT);
}

/// Does this round come out the far side of what it just hit?
///
/// Returns true when it did, having moved the shot past the exit point and taken
/// the energy the material cost it. False means stopped, and the caller deletes
/// the round exactly as before.
///
/// Requires the physics collider set: the thickness comes from a ray, and without
/// PHY-010 there is nothing to cast against. With physics absent this returns
/// false, which is today's behaviour and the safe direction.
bool ShotShell::TryPenetrate(const CollisionInfo& info, float deltaT)
{
    Physics::PhysicsWorld* physics = Physics::GetPhysicsWorld();
    if (!physics || !physics->IsCreated() || !info.object)
    {
        return false;
    }

    const float speed = _speed.Size();
    if (speed <= 1.0f)
    {
        return false;
    }
    const Vector3 dir = _speed / speed;
    const Point3  entry = info.object->PositionModelToTop(info.pos);

    // 2 m of search. Beyond that it is a hill or a bunker, and the honest answer
    // for "no far face found" is "stopped" rather than a guessed thickness.
    constexpr float MaxDepth = 2.0f;
    float           thickness = 0.0f;
    if (!physics->MeasureThickness(entry, dir, MaxDepth, thickness))
    {
        return false;
    }

    const Ballistics::ShellParams shell = Ballistics::ShellParamsFor(*Type());
    const Penetration::Material&  material =
        Penetration::Lookup(info.texture && info.texture->Name() ? info.texture->Name() : nullptr);
    const Penetration::Result result =
        Penetration::Compute(material, speed, shell.mass, shell.calibre, thickness);

    if (!result.penetrated)
    {
        return false;
    }

    // Out the far side, a little past it so the next segment does not start inside
    // the surface it just left and hit it again.
    SetPosition(entry + dir * (thickness + 0.01f));
    _speed = dir * result.exitSpeed;
    return true;
}

// Keep legacy scenery in front of a probe authoritative. Demo blocks are absent
// from CollisionBuffer, so compare distances before choosing either hit path.
static bool ShowcaseContactBeforeScenery(Vector3Par from, Vector3Par to, const CollisionBuffer& collision,
                                        Vector3& hit, Vector3& normal)
{
    if (!Poseidon::Dev::PhysicsShowcaseProjectileContact(from, to, hit, normal)) return false;
    for (int i = 0; i < collision.Size(); ++i)
    {
        const auto& info = collision[i];
        if (info.object && info.object->PositionModelToTop(info.pos).Distance2(from) <= hit.Distance2(from))
            return false;
    }
    return true;
}

void ShotShell::Simulate(float deltaT, SimulationImportance prec)
{
    Point3 position = Position();
#if POSEIDON_DIAG
    Dev::OpDiag::OnShotStep(this); // DIAG-001: flight path
#endif

    // FUSE. Only ammo whose config declares `explosionTime` gets here; for
    // everything else `_fuseArmed` is false and every path below is the one it
    // always took. See AmmoType::explosionTime for why absent means zero.
    if (_fuseArmed)
    {
        _fuse -= deltaT;
        if (_fuse <= 0.0f)
        {
            // Wherever it happens to be: in the air, against a wall, or lying on
            // the ground. That is the point of a fuse.
            Detonate(Position(), nullptr, VUp);
#if POSEIDON_DIAG
            Dev::OpDiag::OnShotEnd(this, "fuse");
#endif
            _delete = true;
            return;
        }
        if (_atRest)
        {
            // Lying still, counting down. Integration is skipped ENTIRELY rather
            // than run with a zero speed: `accel` below still holds gravity, so
            // stepping it would push the grenade through the ground it rests on
            // a little further every tick.
            return;
        }
    }

    // Drag on the RELATIVE airflow. Identical to `_speed * (_speed.Size() *
    // _airFriction)` unless the ballistics wind opt-in is on — see ShotAirspeed.
    const Vector3 airspeed = ShotAirspeed(_speed);
    const float airspeedSize = airspeed.Size();
    // The retardation coefficient. Constant for classic content -- byte-identical to the old
    // `airspeedSize * _airFriction` -- and Mach-dependent for ammo that declares a drag function.
    // Shared with the aim predictor so the two cannot describe different projectiles.
    const float dragK = Poseidon::Ballistics::DragRetardation(_dragModel, _airFriction, _ballisticCoefficient,
                                                              airspeedSize);
    Vector3 accel = airspeed * (airspeedSize * dragK);
    accel[1] -= _coefGravity * G_CONST;

    _initDelay -= deltaT;
    if (_initDelay <= 0)
    {
        _timeToLive -= deltaT;
        if (_timeToLive < 0)
        {
            SnowBulletTerminalTrace("expired",position,_speed,nullptr,IsLocal(),Type()->explosive);
            // Ballistics diagnostic — inlined around a bool, compiles to a
            // not-taken branch when the dev panel switch is off.
            Poseidon::Dev::Ballistics::NotifyImpact(static_cast<Entity*>(this), position.X(), position.Y(), position.Z(),
                                                    Poseidon::Dev::BallisticTerminus::Expired, nullptr);
#if POSEIDON_DIAG
            Dev::OpDiag::OnShotEnd(this, "timeout");
#endif
            _delete = true;
            return;
        }
    }

    if (_initDelay <= 0)
    {
        const Vector3 segmentStart = position;
        position += _speed * deltaT;
        // Malprave aiSuppression: rounds passing close to infantry suppress them (checked before any
        // impact below, so rounds landing near someone count too)
        ApplyNearMissSuppression(_parent, Type(), segmentStart, position);
        // PHY-020: physics probes are deliberately absent from the 2001 collision
        // world, so ObjectCollision below cannot see them. Query the physics world
        // with THIS TICK'S bullet segment instead of faking the probe into the old
        // path. Legacy collision and damage are untouched -- this only pushes dev
        // probes, and it is the same second-ray shape PHY-030 will want.
        Poseidon::Dev::PhysicsProbeOnProjectileHit(segmentStart, position, _speed.Size());
    }

    if (deltaT > 0)
    {
        // test collision with objects
        CollisionBuffer collision;
        GLandscape->ObjectCollision(collision, this, _parent, Position(), position, 0);
        // A passenger's own carrier may have convex hulls spanning its cabin.
        // Resolve its actual shell without excluding that shell or other crew.
        if (dyn_cast<ShotBullet>(this) && _parent && _parent == GWorld->GetRealPlayer() &&
            GWorld->GetMode() != GModeNetware && _parent->GetHierachyParent())
        {
            auto* carrier = const_cast<Object*>(_parent->GetHierachyParent());
            CollisionBuffer surface;
            if (IntersectCargoSurface(*carrier, surface, Position(), position))
            {
                for (int i = 0; i < collision.Size(); ++i)
                    if (collision[i].object == carrier) collision[i].object = nullptr;
                for (int i = 0; i < surface.Size(); ++i) collision.Append() = surface[i];
            }
        }
        Vector3 probeHit, probeNormal;
        if (IsLocal() && ShowcaseContactBeforeScenery(Position(), position, collision, probeHit, probeNormal))
        {
            if (_fuseArmed) BounceOff(probeNormal, probeHit);
            else
            {
                Detonate(probeHit, nullptr, probeNormal);
                _delete = true;
            }
            return;
        }
        // PHY-030: ask the physics world the same question and count the
        // disagreements. Watches only -- the legacy answer below is still the one
        // the game acts on, and the audit is off unless switched on.
        Poseidon::Dev::AuditProjectileSegment(Position(), position, collision.Size() > 0);
        if (collision.Size() > 0)
        {
            float minT = 1e10;
            int minI = -1;

            Texture* glass = GPreloadedTextures.New(TextureBlack);
            bool detectGlass = dyn_cast<ShotBullet>(this) != nullptr;

            for (int i = 0; i < collision.Size(); i++)
            {
                CollisionInfo& info = collision[i];
                // Carrier recursion can return the seated shooter even though
                // the top-level query excludes it. Do not exclude other crew.
                if (_parent && _parent->GetHierachyParent() && info.object == _parent)
                    continue;
                if (detectGlass && info.texture == glass)
                {
                    continue;
                }
                if (info.object)
                {
                    if (minT > info.under)
                    {
                        minT = info.under, minI = i;
                    }
                }
            }

            // A fused round bounces off what it hits. This has to come BEFORE the
            // damage loop below: a grenade that clatters off a wall must not also
            // deal its hit value to the wall on every contact.
            if (_fuseArmed && minI >= 0)
            {
                CollisionInfo& info = collision[minI];
                const Point3 contact = info.object->PositionModelToTop(info.pos);

                // `surfaceNormal`, not `dirOut`. On this swept-point path dirOut is
                // the penetration chord and points ALONG travel, so reflecting off
                // it would send the grenade onwards rather than back. CollisionInfo
                // documents this at its declaration.
                Vector3 normal = info.object->DirectionModelToTop(info.surfaceNormal);
                const float length = normal.Size();
                if (length > 1e-4f)
                {
                    normal = normal / length;
                }
                else
                {
                    // No usable normal: come back the way we came. Crude, but it
                    // keeps the grenade out of the geometry, which is the part
                    // that matters.
                    const float speed = _speed.Size();
                    normal = speed > 1e-4f ? -_speed / speed : VUp;
                }
                // Component planes face into the solid; we want the outward side.
                if (normal.DotProduct(_speed) > 0.0f)
                {
                    normal = -normal;
                }
                BounceOff(normal, contact);
                return;
            }

            if (IsLocal())
            {
                for (int i = 0; i < collision.Size(); i++)
                {
                    CollisionInfo& info = collision[i];
                    if (_parent && _parent->GetHierachyParent() && info.object == _parent)
                        continue;
                    if (detectGlass && info.texture != glass)
                    {
                        continue;
                    }
                    // stop on first solid obstacle
                    if (info.under > minT)
                    {
                        continue;
                    }
                    if (!info.object)
                    {
                        continue;
                    }
                    // dammage corresponding component (if any)
                    info.object->DirectLocalHit(info.component, Type()->hit //*info.object->GetInvArmor()
                    );
                }
            }

            if (minI >= 0)
            {
                if (SnowBulletTraceEnabled())
                {
                    const auto& terminal = collision[minI];
                    if (terminal.object)
                        SnowBulletTerminalTrace("object",terminal.object->PositionModelToTop(terminal.pos),
                            _speed,terminal.object,IsLocal(),Type()->explosive);
                }
                if (Poseidon::Dev::Ballistics::Enabled())
                {
                    // Ballistics diagnostic — the exact impact point and what was
                    // struck. Runs on remote shots too, so a client's trail ends
                    // where the round actually stopped. Guarded rather than relying
                    // on NotifyImpact's own gate: GetDebugName() allocates an
                    // RString, and that must not happen on every impact in a build
                    // where nobody opened the tab.
                    CollisionInfo& diagInfo = collision[minI];
                    const Point3 diagPos = diagInfo.object->PositionModelToTop(diagInfo.pos);
                    const RString diagName = diagInfo.object->GetDebugName();
                    Poseidon::Dev::Ballistics::NotifyImpact(static_cast<Entity*>(this), diagPos.X(), diagPos.Y(), diagPos.Z(),
                                                            Poseidon::Dev::BallisticTerminus::HitObject,
                                                            static_cast<const char*>(diagName));
                }
                if (IsLocal())
                {
                    CollisionInfo& info = collision[minI];
                    Point3 pos = info.object->PositionModelToTop(info.pos);

                    if (Type()->hit > 50)
                    {
                        Vector3 ePos = pos;
                        float size = Type()->hit * 0.001;
                        saturate(size, MinExplosion, MaxExplosion);
                        float minY = GLandscape->RoadSurfaceY(ePos[0], ePos[2]) + 2 * size;
                        if (ePos[1] < minY)
                        {
                            ePos[1] = minY;
                        }
                        Explosion* explosion = new Explosion(nullptr, _parent, size);
                        explosion->SetPosition(ePos);
                        GLOB_WORLD->AddAnimal(explosion);
                        GetNetworkManager().CreateVehicle(explosion, VLTAnimal, "", -1);
                    }
                    GLandscape->ExplosionDammage(_parent, this, info.object, pos, info.dirOut, Type());
                }

                // PENETRATION. The round has already damaged what it hit -- a bullet
                // that goes through a wall still marks the wall -- so this only
                // decides whether it also survives to carry on.
                if (Penetration::Enabled() && TryPenetrate(collision[minI], deltaT))
                {
                    return;
                }

                _delete = true;
                return;
            }
        }

        Vector3Val lPos = Position();
        Vector3 lDir = position - lPos;
        if (lDir.SquareSize() > 0)
        {
            Vector3 lDirNorm = lDir.Normalized();
            float maxDist = lDir * lDirNorm;

            Vector3 isect;
            bool hitSea = false;
            float t = GLandscape->IntersectWithGroundOrSea(&isect, hitSea, lPos, lDirNorm, 0, maxDist * 1.1);

            if (!_waterImpactDone)
            {
                const float seaLevel = GLandscape->GetSeaLevel();

                // Check if bullet segment crossed the sea surface or hit sea geometry
                const bool segmentCrossedSea = (lPos.Y() > seaLevel && position.Y() <= seaLevel) ||
                                               (lPos.Y() <= seaLevel && position.Y() > seaLevel);
                if (hitSea || segmentCrossedSea || (t <= maxDist && isect.Y() <= seaLevel + 0.3f))
                {
                    _waterImpactDone = true;
                    const Vector3 waterPoint =
                        (hitSea || segmentCrossedSea)
                            ? Vector3(lPos.X() + lDirNorm.X() * t, seaLevel, lPos.Z() + lDirNorm.Z() * t)
                            : isect;
                    // TW-WATER W8a: an explosive round's water event is submitted by
                    // Landscape::ExplosionDammageEffects (with its blast, on every client); a
                    // bullet's here, its `hit` in the mass lane and indirectHitRange in the depth
                    // lane for the Tidewater spout.
                    if (!Type()->explosive)
                    {
                        HydroWaterInteractionEvent event{};
                        event.positionRadius[0] = waterPoint.X();
                        event.positionRadius[1] = waterPoint.Z();
                        event.positionRadius[2] = 1.8f; // Radius
                        event.positionRadius[3] = 3.8f; // Strength
                        event.velocityKind[0] = lDirNorm.X() * 15.0f;
                        event.velocityKind[1] = lDirNorm.Z() * 15.0f;
                        event.velocityKind[2] = -25.0f; // Downward entry velocity
                        event.velocityKind[3] = HydroWaterInteractionBullet;
                        event.timeLifeFoamMass[1] = 1.8f;
                        event.timeLifeFoamMass[2] = 1.0f; // Foam density
                        event.timeLifeFoamMass[3] = std::max(Type()->hit, 0.1f);
                        event.directionDepthFlags[0] = lDirNorm.X();
                        event.directionDepthFlags[1] = lDirNorm.Z();
                        event.directionDepthFlags[2] = std::max(Type()->indirectHitRange, 0.01f);
                        event.directionDepthFlags[3] = HydroWaterInteractionPendingImpulse;
                        SubmitWaterInteraction(event);
                    }

                    // Ordinary rifle impacts use the ripple field by default. The optional
                    // CPU droplet emitter is exposed in the dev Water tab for A/B inspection.
                    // (W8a) In the Tidewater mode an explosive round's plume replaces the droplets.
                    const bool explosiveImpact = Type()->explosive;
                    const bool twBurst =
                        explosiveImpact && TidewaterWaterBurst(waterPoint.X(), waterPoint.Y(), waterPoint.Z());
                    if (!twBurst && (explosiveImpact || RifleWaterImpactSprayEnabled()))
                    {
                        WaterSource waterSplash;
                        waterSplash.SetSize(explosiveImpact ? 0.35f : 0.055f, explosiveImpact ? 0.65f : 0.10f);
                        waterSplash.SetFades(0.08f, 0.03f, explosiveImpact ? 0.45f : 0.12f);
                        waterSplash.SetTimes(0.08f, explosiveImpact ? 0.8f : 0.20f);

                        const int numDroplets = explosiveImpact ? 24 : 2;
                        for (int i = 0; i < numDroplets; ++i)
                        {
                            float angle =
                                static_cast<float>(i) * (2.0f * 3.14159265f / static_cast<float>(numDroplets));
                            float spreadSpeed = explosiveImpact ? 1.5f + GRandGen.RandomValue() * 2.5f
                                                                : 0.15f + GRandGen.RandomValue() * 0.25f;
                            float upSpeed = explosiveImpact ? 4.5f + GRandGen.RandomValue() * 5.5f
                                                            : 0.55f + GRandGen.RandomValue() * 0.70f;
                            Vector3 vel(std::cos(angle) * spreadSpeed, upSpeed, std::sin(angle) * spreadSpeed);
                            Cloudlet* droplet = waterSplash.Drop(waterPoint, vel);
                            if (droplet)
                            {
                                GLOB_WORLD->AddCloudlet(droplet);
                            }
                        }
                    }
                }
            }

            if (t <= maxDist)
            {
                position = isect;

                // A fused round bounces off the ground too -- unless it went into
                // the sea, where there is nothing to bounce from and a grenade
                // that kept skipping across the surface would look absurd. Water
                // detonates on contact, as it does today.
                if (_fuseArmed && !hitSea && !_waterImpactDone)
                {
                    // SurfaceY's gradient form gives dY/dX and dY/dZ at the point,
                    // and the upward normal follows from them directly. There is no
                    // SurfaceNormal() on Landscape -- this IS the way to ask.
                    float dX = 0.0f;
                    float dZ = 0.0f;
                    GLandscape->SurfaceY(isect.X(), isect.Z(), &dX, &dZ);
                    Vector3 normal(-dX, 1.0f, -dZ);
                    const float length = normal.Size();
                    normal = length > 1e-4f ? normal / length : VUp;

                    BounceOff(normal, isect);
                    return;
                }

                // Ballistics diagnostic — terrain / sea terminus.
                SnowBulletTerminalTrace(hitSea || _waterImpactDone ? "water" : "ground",
                    isect,_speed,nullptr,IsLocal(),Type()->explosive);
                Poseidon::Dev::Ballistics::NotifyImpact(static_cast<Entity*>(this), position.X(), position.Y(), position.Z(),
                                                        (hitSea || _waterImpactDone)
                                                            ? Poseidon::Dev::BallisticTerminus::HitWater
                                                            : Poseidon::Dev::BallisticTerminus::HitGround,
                                                        nullptr);

                // Persistent snow edits come only from this actual terminal
                // terrain hit. Object/roof hits, fused bounces and water took
                // their original routes above; existing impact FX remain below.
                SnowBulletTerrainImpact(this,_parent,isect,lDirNorm,_speed.Size(),Type()->hit,
                                        IsLocal(),Type()->explosive,hitSea || _waterImpactDone);

                // A sea hit already submitted its water interaction above. Do not also
                // run the legacy ground-impact presentation for ordinary rifle rounds:
                // that path is the large visible "splash" the Water-tab switch controls.
                const bool showLegacyWaterImpact = Type()->explosive || RifleWaterImpactSprayEnabled();
                if (IsLocal() && (!_waterImpactDone || showLegacyWaterImpact))
                {
                    Vector3 exploPos = position;
                    // (W8a) no fireball on open water in the Tidewater mode: its plume is the burst
                    if (Type()->explosive && !TidewaterWaterBurst(position.X(), position.Y(), position.Z()))
                    {
                        float size = Type()->hit * 0.003;
                        saturate(size, MinExplosion, MaxExplosion);
                        exploPos[1] += 0.5 * size;
                        Explosion* explosion = new Explosion(nullptr, _parent, size);
                        explosion->SetPosition(exploPos);
                        GLOB_WORLD->AddAnimal(explosion);
                        GetNetworkManager().CreateVehicle(explosion, VLTAnimal, "", -1);
                    }
                    GLandscape->ExplosionDammage(_parent, this, nullptr, position, VUp, Type());
                }
                _delete = true;
                return;
            }
        }
    }

    if (_initDelay <= 0)
    {
        _speed += accel * deltaT;
    }

    Move(position);
}

void ShotBullet::Simulate(float deltaT, SimulationImportance prec)
{
    base::Simulate(deltaT, prec);
    SetEnd(Position());
}

void ShotBullet::Sound(bool inside, float deltaT)
{
    base::Sound(inside, deltaT);
}

DEFINE_FAST_ALLOCATOR(Missile)
DEFINE_CASTING(Missile)

Missile::Missile(EntityAI* parent, const AmmoType* type, Object* target)
    : Shot(parent, type),

      _lock(Locked), _engine(Init), _initTime(type->initTime), _thrustTime(type->thrustTime),
      _controlDirectionSet(false), _lightColor(0.7, 0.8, 1.0), _target(target)
//_cloudlets(GLOB_SCENE->Preloaded(CloudletMissile),0.01)
{
    _thrust = type->thrust;
    if (type->manualControl)
    {
        _cloudlets.Load(Pars >> "CfgCloudlets" >> "CloudletsMissileManual");
    }
    else
    {
        _cloudlets.Load(Pars >> "CfgCloudlets" >> "CloudletsMissile");
    }
    if (type->maxControlRange < 1 || !target && !type->manualControl)
    {
        _lock = Lost;
    }
    if (_thrustTime <= 0)
    {
        // free fall bombs should have very high time to live
        _timeToLive = 120;
    }
}

void Missile::SetLight(ColorVal color)
{
    _lightColor = color;
}

void Missile::SetControlDirection(Vector3 dir)
{
    _controlDirection = dir;     // manual missile control
    _controlDirectionSet = true; // manual control activated
}

inline bool FaceIsShining(const Poly& f, const Shape* shape)
{
    for (int v = 0; v < f.N(); v++)
    {
        ClipFlags clip = shape->Clip(f.GetVertex(v));
        if ((clip & ClipUserMask) != static_cast<uint32_t>(MSShining) * ClipUserStep)
        {
            return false;
        }
    }
    return true;
}

void Missile::Animate(int level)
{
    Shape* shape = _shape->Level(level);
    if (_thrustTime <= 0)
    {
        for (Offset o = shape->BeginFaces(); o < shape->EndFaces(); shape->NextFace(o))
        {
            Poly& f = shape->Face(o);
            if (!FaceIsShining(f, shape))
            {
                continue;
            }
            f.OrSpecial(IsHidden);
        }
        for (int i = 0; i < shape->NSections(); i++)
        {
            ShapeSection& sec = shape->GetSection(i);
            if (sec.material == MSShining)
            {
                sec.properties.OrSpecial(IsHidden);
            }
        }
    }
}

void Missile::Deanimate(int level)
{
    Shape* shape = _shape->Level(level);
    if (_thrustTime <= 0)
    {
        for (Offset o = shape->BeginFaces(); o < shape->EndFaces(); shape->NextFace(o))
        {
            Poly& f = shape->Face(o);
            if (!FaceIsShining(f, shape))
            {
                continue;
            }
            f.AndSpecial(!IsHidden);
        }
        for (int i = 0; i < shape->NSections(); i++)
        {
            ShapeSection& sec = shape->GetSection(i);
            if (sec.material == MSShining)
            {
                sec.properties.AndSpecial(~IsHidden);
            }
        }
    }
}

void Missile::Simulate(float deltaT, SimulationImportance prec)
{
#if POSEIDON_DIAG
    Dev::OpDiag::OnShotStep(this); // DIAG-001: flight path
#endif
    Vector3 force(VZero), torque(VZero);
    Vector3 friction(VZero), torqueFriction(VZero);
    Vector3 pForce(VZero), pCenter(VZero);
    Vector3Val position = Position();
    Vector3Val speed = ModelSpeed();
    float mass = GetMass();
    pForce[0] = speed[0] * speed[0] * speed[0] * 5e-4f + speed[0] * fabs(speed[0]) * 10.0f + speed[0] * 10;
    pForce[1] = speed[1] * speed[1] * speed[1] * 5e-4f + speed[1] * fabs(speed[1]) * 10.0f + speed[1] * 10;
    pForce[2] = speed[2] * speed[2] * speed[2] * 1e-5f + speed[2] * fabs(speed[2]) * 0.01f + speed[2] * 2;
    pForce[0] *= Type()->sideAirFriction;
    pForce[1] *= Type()->sideAirFriction;
    pForce[2] *= Type()->sideAirFriction;
    pForce *= mass * (1.0f / 10);

    bool freeFall = Type()->thrustTime <= 0;

#if ARROWS
    Vector3 wCenter(VFastTransform, ModelToWorld(), GetCenterOfMass());
#endif

    if (freeFall)
    {
        // aerodynamic instability aligns direction with speed
        pForce[0] *= 0.1f;
        pForce[1] *= 0.1f;
        pCenter = Vector3(0, 0, +0.3f);
        torque += pCenter.CrossProduct(pForce);

#if ARROWS
        AddForce(wCenter + DirectionModelToWorld(pCenter), DirectionModelToWorld(-pForce * InvMass()), Color(1, 0, 0));
#endif
    }
    else
    {
#if ARROWS
        AddForce(wCenter, DirectionModelToWorld(-pForce * InvMass()), Color(1, 0, 0));
#endif
    }

    friction += pForce;

    if (freeFall)
    {
        // drag force; only applied to free-fall bombs (late addition — adding to all missiles would change behaviour)
        pForce[0] = speed[0] * fabs(speed[0]) * -0.00033f + speed[0] * -0.005f;
        pForce[1] = speed[1] * fabs(speed[1]) * -0.00033f + speed[1] * -0.005f;
        pForce[2] = 0;
        pForce *= mass;
        force += pForce;

#if ARROWS
        AddForce(Position(), pForce * InvMass(), Color(1, 1, 0));
#endif
    }

    switch (_engine)
    {
        case Init:
            _initTime -= deltaT;
            if (_initTime < 0)
            {
                if (_thrustTime > 0)
                {
                    _engine = Thrust;
                }
                else
                {
                    _engine = Fly;
                }
            }
            break;
        case Thrust:
        {
            Point3 backPos = PositionModelToWorld(Vector3(0, 0, -0.5));
            _thrustTime -= deltaT;
            if (_thrustTime < 0)
            {
                _engine = Fly;
            }
            Vector3 cSpeed = Speed() * 0.1;
            const float maxCSpeed = 30;
            if (cSpeed.SquareSize() > Square(maxCSpeed))
            {
                cSpeed = cSpeed.Normalized() * maxCSpeed;
            }
            _cloudlets.Simulate(backPos, cSpeed, deltaT);

            float fade = 1;
            const AmmoType* type = Type();
            if (4 * _thrustTime < type->thrustTime)
            {
                fade = 4 * _thrustTime / type->thrustTime;
            }

            pForce = Vector3(0, 0, _thrust * fade) * mass;
            force += pForce;

            if (ENGINE_CONFIG.lights & LIGHT_MISSILE)
            {
                if (!_light)
                {
                    _light = new LightPointOnVehicle(GLOB_SCENE->Preloaded(SphereLight), _lightColor, Color(HBlack),
                                                     this, Vector3(0, 0, -0.5));
                    GLOB_SCENE->AddLight(_light);
                }
                if (_light)
                {
                    _light->SetDiffuse(_lightColor * fade);
                }
            }
        }
        break;
        case Fly:
            _light.Free();
            break;
    }

    if (_lock == Locked && _target && !_target->LockPossible(Type()))
    {
        _lock = Lost;
    }
    bool forceExplosion = false;
    if (_lock == Locked)
    {
        Vector3 cmdDir;
        float estT = 0.3;
        if (_controlDirectionSet && !_target)
        {
            cmdDir = _controlDirection - Position();
        }
        else if (_target)
        {
            Vector3 pos = _target->AimingPosition();
            float dist = pos.Distance(position);
            float estSpeed = (Type()->maxSpeed + speed.Z()) * 0.5f;
            float time = dist / floatMax(speed.Z(), estSpeed);
            // lead the target
            pos += time * _target->ObjectSpeed();
            if (freeFall)
            {
                pos[1] += pos.DistanceXZ(position) * 0.2f;
            }
            cmdDir = pos - Position();
            estT = floatMin(0.3f, time);
        }

        {
            Matrix3Val orientation = Orientation();

            float dFactor = Type()->maneuvrability * 0.3f;
            Vector3 rDir;
            Matrix3 estOrientation = orientation;
            if (freeFall)
            {
                Matrix3Val derOrientation = _angVelocity.Tilda() * orientation;
                Matrix3 estOrientation = orientation + derOrientation * estT;
                estOrientation.Orthogonalize();
            }
            Matrix3Val invEstOrientation = estOrientation.InverseRotation();
            Vector3Val rSpeed = invEstOrientation * (Speed() + estT * Acceleration());
            Vector3Val rPos = invEstOrientation * cmdDir;
            if (!freeFall)
            {
                saturate(dFactor, 0.5f, 0.95f);
                rDir = rSpeed.Normalized() * dFactor + VForward * (1 - dFactor);
            }
            else
            {
                saturate(dFactor, 0.1f, 0.5f);
                rDir = rSpeed.Normalized() * dFactor + VForward * (1 - dFactor);
            }

            Vector3 rdn = rDir.Normalized();
            Vector3 rpn = rPos.Normalized();

            float up = (rpn.Y() - rdn.Y()) * 20;
            float left = (rpn.X() - rdn.X()) * 20;

            if (speed[2] < 30)
            {
                up = left = 0; // disable controls when flying slow
            }
            if (Type()->manualControl)
            {
                if (!_parent ||            // controlling vehicle is no longer able to control
                    !_parent->CanFire() || // controlling vehicle too far
                    _parent->Position().Distance2(Position()) >= Square(Type()->maxControlRange))
                {
                    // stop rotation (once only)
                    _angMomentum = VZero;
                    up = left = 0;
                    _lock = Lost;
                }
            }
            else
            {
                if (_target)
                {
                    Vector3 relPos = PositionWorldToModel(_target->Position());
                    if (relPos.Z() < 0 || fabs(relPos.X()) > relPos.Z() * 1.5f || fabs(relPos.Y()) > relPos.Z() * 1.5f)
                    {
                        // target is behind or outside lock cone
                        _angMomentum = VZero;
                        up = left = 0;
                        _lock = Lost;
                    }
                }
            }
            float turn = speed[2] * (1.0 / 50);
            saturate(turn, 0.1, 3);
            float invTurn = 3 / turn;
            up *= invTurn;
            left *= invTurn;

            float maxMan = Type()->maneuvrability * 0.25;
            saturate(up, -maxMan, +maxMan);
            saturate(left, -maxMan, +maxMan);

            Vector3 pCenter = Vector3(0, 0, Type()->maneuvrability * 0.04f);
            Vector3 pForce = mass * turn * Vector3(left, up, 0);
            torque += pCenter.CrossProduct(pForce);

            if (freeFall)
            {
                force += pForce;
            }

#if ARROWS
            AddForce(PositionModelToWorld(pCenter), DirectionModelToWorld(pForce * InvMass()), Color(0, 0, 1));
#endif
        }
    }
    DirectionModelToWorld(friction, friction);
    DirectionModelToWorld(force, force);
    DirectionModelToWorld(torque, torque);

    torqueFriction = _angMomentum * 5.0;

    pForce = Vector3(0, -G_CONST, 0) * mass;
    force += pForce;

    Matrix4 movePos;
    ApplySpeed(movePos, deltaT);
    Frame moveTrans;
    moveTrans.SetTransform(movePos);

    if (IsLocal() && deltaT > 0)
    {
        CollisionBuffer collision;
        GLandscape->ObjectCollision(collision, this, _parent, Position(), moveTrans.Position(), 0);
        Vector3 probeHit, probeNormal;
        if (ShowcaseContactBeforeScenery(Position(), moveTrans.Position(), collision, probeHit, probeNormal))
        {
            float size = Type()->hit * 0.003f;
            saturate(size, MinExplosion, MaxExplosion);
            Explosion* explosion = new Explosion(nullptr, _parent, size);
            explosion->SetPosition(probeHit);
            GLOB_WORLD->AddAnimal(explosion);
            GetNetworkManager().CreateVehicle(explosion, VLTAnimal, "", -1);
            GLandscape->ExplosionDammage(_parent, this, nullptr, probeHit, probeNormal, Type());
            _delete = true;
            return;
        }
        if (collision.Size() > 0)
        {
            float minT = 1e10;
            int minI = -1;
            for (int i = 0; i < collision.Size(); i++)
            {
                const CollisionInfo& info = collision[i];
                if (info.object)
                {
                    if (minT > info.under)
                    {
                        minT = info.under, minI = i;
                    }
                }
            }

            if (minI >= 0)
            {
                const CollisionInfo& info = collision[minI];
                Point3 pos = info.object->PositionModelToWorld(info.pos);
                float size = Type()->hit * 0.003;
                saturate(size, MinExplosion, MaxExplosion);
                Explosion* explosion = new Explosion(nullptr, _parent, size);
                pos -= Direction() * 2 * size;
                float minY = GLandscape->RoadSurfaceY(position[0], position[2]) + 3 * size;
                if (pos[1] < minY)
                {
                    pos[1] = minY;
                }
                explosion->SetPosition(pos);
                GLOB_WORLD->AddAnimal(explosion);
                GetNetworkManager().CreateVehicle(explosion, VLTAnimal, "", -1);
                GLandscape->ExplosionDammage(_parent, this, info.object, pos, info.dirOut, Type());
                _delete = true;
#if _ENABLE_CHEATS
                if (this == GWorld->CameraOn() && _parent)
                {
                    GWorld->SwitchCameraTo(_parent, CamInternal);
                }
#endif
                return;
            }
        }
    }

    _timeToLive -= deltaT;

    if (IsLocal())
    {
        if (_timeToLive < 0)
        {
            forceExplosion = true;
        }

        Vector3 lPos = position;
        Vector3 lDir = movePos.Position() - lPos;
        if (lDir.SquareSize() > 0)
        {
            Vector3 lDirNorm = lDir.Normalized();
            float maxDist = lDirNorm * lDir;
            Vector3 isect;
            float t = GLandscape->IntersectWithGroundOrSea(&isect, lPos, lDirNorm, 0, maxDist * 1.1);
            if (t < maxDist)
            {
                lPos = isect;
                forceExplosion = true;
            }
        }

        if (forceExplosion)
        {
            // (W8a) a rocket, missile or bomb into open water: in the Tidewater mode the water
            // plume (from ExplosionDammageEffects' water event) replaces the fireball
            if (!TidewaterWaterBurst(lPos.X(), lPos.Y(), lPos.Z()))
            {
                float size = Type()->hit * 0.003;
                saturate(size, MinExplosion, MaxExplosion);
                Explosion* explosion = new Explosion(nullptr, _parent, size);
                explosion->SetPosition(lPos);
                GLOB_WORLD->AddAnimal(explosion);
                GetNetworkManager().CreateVehicle(explosion, VLTAnimal, "", -1);
            }
            GLandscape->ExplosionDammage(_parent, this, nullptr, lPos, VUp, Type());
            _delete = true;
#if _ENABLE_CHEATS
            if (this == GWorld->CameraOn() && _parent)
            {
                GWorld->SwitchCameraTo(_parent, CamInternal);
            }
#endif
            return;
        }
    }

    Move(moveTrans);
    DirectionWorldToModel(_modelSpeed, _speed);

    ApplyForces(deltaT, force, torque, friction, torqueFriction);
}

void Missile::Sound(bool inside, float deltaT)
{
    const SoundPars& sound = Type()->_soundEngine;
    if (_engine == Thrust)
    {
        if (!_soundEngine && sound.name.GetLength() > 0)
        {
            _soundEngine = GSoundScene->OpenAndPlay(sound.name, Position(), Speed());
        }
        if (_soundEngine)
        {
            float coef = _thrust * (1.0 / 800);
            _soundEngine->SetVolume(sound.vol * coef, sound.freq);
            _soundEngine->SetPosition(Position(), Speed());
        }
    }
    else
    {
        _soundEngine.Free();
    }
    base::Sound(inside, deltaT);
}

void Missile::UnloadSound()
{
    _soundEngine.Free();
    base::UnloadSound();
}

Missile::~Missile() = default;

DEFINE_CASTING(IlluminatingShell)
DEFINE_FAST_ALLOCATOR(IlluminatingShell)

#define ILL_TTL 17.0F
#define ILL_EXPL 2.0F

IlluminatingShell::IlluminatingShell(EntityAI* parent, const AmmoType* type)
    : base(parent, type), _lightColor(1.0, 1.0, 1.0)
{
    _timeToLive = ILL_TTL;
    _airFriction = -0.0005;

    const ParamEntry& cls = *type->_par;
    _lightColor = GetColor(cls >> "lightColor");
}

void IlluminatingShell::Simulate(float deltaT, SimulationImportance prec)
{
    base::Simulate(deltaT, prec);
    if (!_light && _timeToLive <= ILL_TTL - ILL_EXPL)
    {
        _light = new LightPointOnVehicle(GLOB_SCENE->Preloaded(SphereLight), _lightColor, Color(HBlack), this,
                                         Vector3(0, 0, -0.5));
        _light->SetBrightness(2);
        GLOB_SCENE->AddLight(_light);

        _airFriction = -0.2;

        RString name = RString("onFlare.sqs");
        if (QIFStreamB::FileExist(Poseidon::GetMissionDirectory() + name))
        {
            GameArrayType color;
            color.Add(_lightColor.R());
            color.Add(_lightColor.G());
            color.Add(_lightColor.B());
            GameArrayType arguments;
            arguments.Add(color);
            arguments.Add(GameValueExt(_parent));

            Script* script = new Script(name, arguments);
            GWorld->AddScript(script);
            GWorld->SimulateScripts();
        }
    }
    float fade = GRandGen.PlusMinus(0.8, 0.2);
    if (_light)
    {
        _light->SetDiffuse(_lightColor * fade);
    }
}

void IlluminatingShell::SetLight(ColorVal color)
{
    _lightColor = color;
}

NetworkMessageType IlluminatingShell::GetNMType(NetworkMessageClass cls) const
{
    switch (cls)
    {
        case NMCCreate:
            return NMTCreateShot;
        case NMCUpdateGeneric:
        case NMCUpdateDammage:
            return base::GetNMType(cls);
        case NMCUpdatePosition:
            return Entity::GetNMType(cls);
        default:
            return base::GetNMType(cls);
    }
}

LSError IlluminatingShell::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    PARAM_CHECK(ar.Serialize("lightColor", _lightColor, 1))
    return LSOK;
}

DEFINE_CASTING(SmokeShell)
DEFINE_FAST_ALLOCATOR(SmokeShell)

#define SMOKE_TTL 60.0F

SmokeShell::SmokeShell(EntityAI* parent, const AmmoType* type) : base(parent, type)
{
    _timeToLive = SMOKE_TTL;
    _airFriction = -0.0005;

    const ParamEntry& cls = *type->_par;
    _smoke.Load(cls >> "Smoke");
    _smoke.SetColor(GetColor(cls >> "smokeColor"));
}

void SmokeShell::Simulate(float deltaT, SimulationImportance prec)
{
    Point3 position = Position();

    // Same relative-airflow drag as ShotShell. A smoke round's body has a high
    // drag coefficient once it is falling, so this is where the wind carries a
    // deployed smoke grenade downwind coherently with the smoke it emits (which
    // already reads the same authority through the cloudlet sources).
    const Vector3 airspeed = ShotAirspeed(_speed);
    Vector3 accel = airspeed * (airspeed.Size() * _airFriction);
    accel[1] -= G_CONST;

    float surfaceY = GLandscape->SurfaceY(position[0], position[2]);

    _initDelay -= deltaT;
    if (_initDelay <= 0)
    {
        _timeToLive -= deltaT;
        if (_timeToLive < 0)
        {
            _delete = true;
        }

        position += _speed * deltaT;
        if (position[1] > surfaceY + 1e-3)
        {
            _speed += accel * deltaT;
        }
    }

    if (position[1] < surfaceY)
    {
        position[1] = surfaceY;
        _speed = VZero;
    }
    if (deltaT > 0)
    {
        CollisionBuffer collision;
        GLandscape->ObjectCollision(collision, this, _parent, Position(), position, 0);

        int nCol = 0;
        for (int i = 0; i < collision.Size(); i++)
        {
            // info.pos is relative to object
            const CollisionInfo& info = collision[i];
            if (info.object)
            {
                nCol++;
            }
        }
        if (nCol > 0)
        {
            position[1] = surfaceY;
            _speed = VZero;
        }
    }

    Move(position);
    _smoke.Simulate(Position(), Speed(), deltaT, prec);
}

LSError SmokeShell::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar))
    return LSOK;
}

DEFINE_FAST_ALLOCATOR(ShotBullet)
DEFINE_CASTING(ShotBullet)

ShotBullet::ShotBullet(EntityAI* parent, const AmmoType* type) : ShotShell(parent, type)
{
    _timeToLive = 3;
    _shape = GScene->Preloaded(BulletLine);

    _beg = VZero;
    _end = VZero;

    float width = type->hit * (1.0 / 20);
    saturate(width, 0.1, 2);

    PackedColor color = type->_tracerColor;
    if (!USER_CONFIG.IsEnabled(DTTracers))
    {
        color = type->_tracerColorR;
    }
    float a8 = color.A8() * width;
    saturate(a8, 0, 255);
    color.SetA8(toInt(a8));
    SetConstantColor(color);
}

void ShotBullet::SetBeg(Vector3Val beg)
{
    _beg = beg;
}

void ShotBullet::SetEnd(Vector3Val end)
{
    if (_beg.SquareSize() <= 1e-6)
    {
        _beg = end;
    }
    _end = end;
}

void ShotBullet::StartFrame()
{
    if (_end.SquareSize() < 1e-6)
    {
        _end = Position();
    }
    SetBeg(_end);
}

inline void ShotBullet::DoDraw(int level, ClipFlags clipFlags, const FrameBase& pos)
{
    if (_beg.Distance2(_end) < 0.1)
    {
        return;
    }

    DrawLines(level, clipFlags, *this);
}

int ShotBullet::PassNum(int lod)
{
    return 2; // alpha pass
}

bool ShotBullet::IsAnimated(int level) const
{
    return true;
}

bool ShotBullet::IsAnimatedShadow(int level) const
{
    return false;
}

void ShotBullet::Animate(int level)
{
    ObjectLine::SetPos(_shape, PositionWorldToModel(_beg), PositionWorldToModel(_end));
}

void ShotBullet::Deanimate(int level)
{
    ObjectLine::SetPos(_shape, VZero, VForward);
}

void ShotBullet::AnimatedMinMax(int level, Vector3* minMax)
{
    Shape* shape = _shape->Level(level);
    PoseidonAssert(shape->NVertex() == 2);
    Vector3 v0 = shape->Pos(0);
    Vector3 v1 = shape->Pos(1);
    minMax[0] = v0, minMax[1] = v0;
    CheckMinMax(minMax[0], minMax[1], v1);
}
void ShotBullet::AnimatedBSphere(int level, Vector3& bCenter, float& bRadius, bool isAnimated)
{
    Shape* shape = _shape->Level(level);
    PoseidonAssert(shape->NVertex() == 2);
    Vector3 v0 = shape->Pos(0);
    Vector3 v1 = shape->Pos(1);
    bCenter = (v0 + v1) * 0.5f;
    bRadius = v0.Distance(v1) * 0.5f;
}

void ShotBullet::Draw(int level, ClipFlags clipFlags, const FrameBase& pos)
{
#if !ALPHA_SPLIT
    DoDraw(level, clipFlags, pos);
#endif
}

#if ALPHA_SPLIT
void ShotBullet::DrawAlpha(int level, ClipFlags clipFlags, const FrameBase& pos)
{
    DoDraw(level, clipFlags, pos);
}
#endif

namespace Poseidon
{
Shot* NewShot(EntityAI* parent, const AmmoType* type, Object* target)
{
    Shot* v = nullptr;
    type->VehicleAddRef();
    switch (type->_simulation)
    {
        case AmmoShotShell:
            v = new ShotShell(parent, type);
            break;
        case AmmoShotMissile:
            v = new Missile(parent, type, target);
            break;
        case AmmoShotRocket:
            v = new Missile(parent, type, target);
            break;
        case AmmoShotBullet:
            v = new ShotBullet(parent, type);
            break;
        case AmmoShotIlluminating:
            v = new IlluminatingShell(parent, type);
            break;
        case AmmoShotSmoke:
            v = new SmokeShell(parent, type);
            break;
        case AmmoShotTimeBomb:
            v = new TimeBomb(parent, type);
            break;
        case AmmoShotPipeBomb:
            v = new PipeBomb(parent, type);
            break;
        case AmmoShotMine:
            v = new Mine(parent, type);
            break;
        default:
            LOG_ERROR(Physics, "Unsupported ammo type (type name {})", (const char*)type->GetName());
            return nullptr;
    }
    type->VehicleRelease();
    return v;
}
} // namespace Poseidon

EntityAI* Shot::GetOwner() const
{
    return _parent;
}
