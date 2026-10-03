// Sinkhole W3: swimming.
//
// Ported from Malprave (swimming / swimBoatsAI / swimMoves patches) and made to ride the sea that is
// drawn. In water too deep to stand a man swims: the swim moves (CfgMovesMC states with the
// SwimActions / SwimForwardActions / SwimDeadActions action maps, the "swim" addon) take over, his
// travel comes from the stroke like walking, and the water carries him:
//  * heave: a spring-damper holds the animation's water line on the local surface -- the CPU wave
//    predictor (QueryWaterSurfaceScaled, the same sea the boats float on, sized from the Tidewater
//    look), or a lake's level -- averaged over the body so ripples shorter than a man do not shake him;
//  * drift: the waves' orbital motion carries him to and fro (and a little onward, as a float does);
//  * lean: the body tilts with the wave face under it, smoothed.
// Hands and feet throw a splash where they cut the surface (EmitSwimSplashes), for every swimmer
// near the camera; the Tidewater water draws them with the impact spray.
//
// A config without the swim moves keeps the old behaviour (the man walks the sea bed). Dead men
// float face down. POSEIDON_SWIM=0 turns swimming off, POSEIDON_SWIM_SPLASH=0 the splashes.

#include <Poseidon/World/Entities/Infantry/SoldierOldCommon.hpp>
#include <Poseidon/World/Entities/Infantry/SwimView.hpp>

#include <Poseidon/AI/AIUnit.hpp>
#include <Poseidon/Graphics/Rendering/WaterInteractionBridge.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Simulation/Animation/RtAnimation.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Terrain/WaterBodies.hpp>
#include <Poseidon/World/Terrain/WaterSurfaceQuery.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace Poseidon
{
namespace
{
// A living swimmer's water line (SwimWaterLine, SwimView.hpp) follows his eyes (the "zamerny" memory
// point, smoothed): they ride SwimEyeAbove over the water whatever the move -- treading holds the head
// up, the stroke lies flat -- so his face is clear and his body under. A dead man (and a model without
// the eye point) floats at SwimWaterLine.
constexpr float SwimEyeAbove = 0.12f;
constexpr float SwimEyeTime = 0.6f;
// Heave: spring (1/s^2) and damping (1/s) pulling the water line onto the surface. A 1.2 s natural
// period against swell of 6-10 s: the body follows the waves with little lag, but still plunges and
// bobs back up after a jump in.
constexpr float SwimHeaveSpring = 28.0f;
constexpr float SwimHeaveDamping = 8.5f;
// Share of the waves' orbital velocity that carries the swimmer.
constexpr float SwimDrift = 0.85f;
// How far the body leans with the wave face (0 upright .. 1 square to the surface), and how fast (s).
constexpr float SwimLean = 0.7f;
constexpr float SwimLeanTime = 0.35f;
// Splashes: only for swimmers this close to the camera (m).
constexpr float SwimSplashRange = 150.0f;
// The finger tips and toe tips in the men's rest (model) space -- the wrist and toe joints of the CWA
// skeleton (fitted from the stock animations) a hand's / a toe's length on.
const Vector3 SwimLimbRest[4] = {Vector3(-0.003f, -0.40f, 0.22f), Vector3(0.007f, -0.40f, -0.206f),
                                 Vector3(0.19f, -1.08f, 0.229f), Vector3(0.186f, -1.08f, -0.221f)};

bool EnvOn(const char* name)
{
    const char* v = std::getenv(name);
    return !(v && v[0] == '0');
}

bool SwimEnabled()
{
    static const bool on = EnvOn("POSEIDON_SWIM");
    return on;
}

bool SwimSplashEnabled()
{
    static const bool on = EnvOn("POSEIDON_SWIM_SPLASH");
    return on;
}

// The mean water level at x, z (a lake's or river's level inside a registered water body, else the
// sea) and the wave scale there -- as Landscape::GroundCollision picks them for the boats.
void SwimWaterLevel(float x, float z, float& level, float& waveScale)
{
    level = GLandscape->GetSeaLevel();
    waveScale = 1.0f;
    if (const WaterBody* body = GetWaterBodies().Find(x, z))
    {
        level = body->SurfaceLevelAt(x, z);
        waveScale = body->waveScale;
    }
}

WaterSurfaceSample SwimSurface(float x, float z, float level, float waveScale)
{
    // WGR_WAVE_BUOYANCY=0 (Collisions.cpp): the flat sea, for A/B
    static const bool waves = EnvOn("WGR_WAVE_BUOYANCY");
    if (!waves)
    {
        return WaterSurfaceSample{level, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    }
    WaterSurfaceSample sample = QueryWaterSurfaceScaled(x, z, Glob.time.toFloat(), level, waveScale);
    // near the camera, the surface as the Tidewater water draws it: the predictor matches the drawn sea in
    // scale, not crest for crest, and the gap (half a metre at times) put the player's eyes under the drawn
    // crests. POSEIDON_SWIM_DRAWN=0: the predictor everywhere.
    static const bool drawn = EnvOn("POSEIDON_SWIM_DRAWN");
    float y;
    if (drawn && DrawnWaterHeightAt(x, z, y))
    {
        sample.height = y;
    }
    return sample;
}
} // namespace

float SwimMeanWaterLevel(float x, float z)
{
    float level, waveScale;
    SwimWaterLevel(x, z, level, waveScale);
    return level;
}

float SwimWaterSurfaceY(float x, float z)
{
    float level, waveScale;
    SwimWaterLevel(x, z, level, waveScale);
    return SwimSurface(x, z, level, waveScale).height;
}

bool SwimmerCameraWater(const Object* obj, float x, float z, float& waterY)
{
    const Man* man = obj ? dyn_cast<const Man>(obj) : nullptr;
    if (!man || !man->IsSwimming() || !GLandscape)
    {
        return false;
    }
    float level, waveScale;
    SwimWaterLevel(x, z, level, waveScale);
    waterY = SwimSurface(x, z, level, waveScale).height;
    return true;
}

void ManType::FindSwimLimbs() const
{
    if (_swimLimbsInit)
    {
        return;
    }
    _swimLimbsInit = true;
    const Skeleton* skeleton = _moveType ? _moveType->GetSkeleton() : nullptr;
    static const char* const bones[4] = {"lruka", "pruka", "lprsty", "pprsty"};
    for (int b = 0; skeleton && b < 4; b++)
    {
        _swimLimbBone[b] = skeleton->FindBone(bones[b]);
    }
}

void Man::BlendToMove(MoveId id)
{
    if (id == MoveIdNone || _primaryMove.id == id)
    {
        return;
    }
    // cross-fade from whatever is playing; no motion-graph path is needed
    _queueMove.Clear();
    _secondaryMove = _primaryMove;
    _secondaryTime = _primaryTime;
    SetPrimaryMove(MotionPathItem(id));
    _primaryTime = 0;
    _primaryFactor = 0;
    _stillMoveQueueEnd = MoveIdNone;
}

void Man::InitSwimMoves() const
{
    if (_swimMovesInit)
    {
        return;
    }
    _swimMovesInit = true;
    static const char* const names[4] = {"Swim", "SwimF", "SwimFastF", "SwimDead"};
    for (int i = 0; i < 4; i++)
    {
        _swimMoves[i] = Type()->GetMoveId(names[i]);
    }
    _swimOut = Type()->GetMoveId("SwimOut");
}

bool Man::CanSwim() const
{
    InitSwimMoves();
    return _swimMoves[0] != MoveIdNone;
}

bool Man::IsSwimMove(MoveId id) const
{
    InitSwimMoves();
    if (id == MoveIdNone)
    {
        return false;
    }
    // a swim move is any state that uses one of the swim moves' action maps, so added states
    // (backwards, sideways, turning, transitions ...) count too
    const ActionMap* map = Type()->GetActionMap(id);
    for (int i = 0; i < 4; i++)
    {
        if (_swimMoves[i] == MoveIdNone)
        {
            continue;
        }
        if (id == _swimMoves[i] || (map && map == Type()->GetActionMap(_swimMoves[i])))
        {
            return true;
        }
    }
    return false;
}

bool Man::UpdateSwimming()
{
    const MoveId swim = SwimEnabled() && CanSwim() ? _swimMoves[0] : MoveIdNone;
    if (swim == MoveIdNone || !GLandscape)
    {
        _swimming = false; // config without the swim moves: the old behaviour (walk on the sea bed)
        return false;
    }
    AIUnit* unit = Brain();
    if (unit && unit->GetVehicleIn())
    {
        _swimming = false; // riding in a vehicle (boat): never swimming
        return false;
    }
    Vector3Val pos = Position();
    float level, waveScale;
    SwimWaterLevel(pos.X(), pos.Z(), level, waveScale);
    const float ground = GLandscape->SurfaceY(pos.X(), pos.Z());
    // the mean level, not the wave: a man wading in the surf does not start and stop with every crest
    const float depth = level - ground;
    if (!_swimming)
    {
        if (_swimOut != MoveIdNone && _primaryMove.id == _swimOut && _primaryTime >= 0.98f)
        {
            // standing up out of the water has finished: normal moves again
            BlendToMove(GetDefaultMove());
        }
        // start only when actually in the water (not on a pier or a boat deck above it, not in a
        // terrain hole -- a cellar below sea level is dry) and it is too deep to stand
        if (IsDead() || _ladderBuilding || depth < SwimStartDepth || level - pos.Y() < SwimStartDepth - 0.1f ||
            GLandscape->InTerrainHole(pos.X(), pos.Z()))
        {
            return false;
        }
        _swimming = true;
        _swimVy = Speed().Y();
        _swimTargetValid = false;
        _swimEyeValid = false;
        _swimLimbValid = false;
        _swimUp = VUp;
        _waterBuoyancyContact = false;
        BlendToMove(swim);
        return true;
    }
    if (depth < SwimStopDepth && !IsDead())
    {
        // shallow enough to stand: back to the normal moves, upright
        _swimming = false;
        BlendToMove(_swimOut != MoveIdNone ? _swimOut : GetDefaultMove());
        Matrix3 upright;
        Vector3 dir = Direction();
        dir[1] = 0;
        upright.SetUpAndDirection(VUp, dir.SquareSize() > 1e-4f ? dir : Vector3(VForward));
        Frame trans = *this;
        trans.SetOrientation(upright);
        Move(trans);
        _landContact = false;
        return false;
    }
    if (!IsDead() && !IsSwimMove(_primaryMove.id))
    {
        // something set a land move while swimming (getting out of a boat, a script switchMove):
        // back to treading water
        BlendToMove(swim);
    }
    return true;
}

void Man::SimSwimMovement(float deltaT, SimulationImportance prec)
{
    float turn, moveX, moveZ;
    const bool change = SimulateAnimations(turn, moveX, moveZ, deltaT, prec);

    BasicSimulationCore(deltaT, prec);

    if (CheckPredictionFrozen())
    {
        return;
    }

    // heading on the level: the body leans with the waves, the stroke drives along the heading
    Vector3 fwd = Direction();
    fwd[1] = 0;
    fwd = fwd.SquareSize() > 1e-4f ? fwd.Normalized() : Vector3(VForward);
    const Vector3 aside(fwd.Z(), 0, -fwd.X());

    Vector3 pos = Position();
    // horizontal travel from the swim cycle, like walking
    pos += aside * moveX + fwd * moveZ;

    // the water under the body: five samples over its length and width, so ripples shorter than a man
    // average out and the slope under him is the swell's
    float level, waveScale;
    SwimWaterLevel(pos.X(), pos.Z(), level, waveScale);
    const float halfLength = 0.8f;
    const float halfWidth = 0.35f;
    const WaterSurfaceSample c = SwimSurface(pos.X(), pos.Z(), level, waveScale);
    const WaterSurfaceSample f =
        SwimSurface(pos.X() + fwd.X() * halfLength, pos.Z() + fwd.Z() * halfLength, level, waveScale);
    const WaterSurfaceSample b =
        SwimSurface(pos.X() - fwd.X() * halfLength, pos.Z() - fwd.Z() * halfLength, level, waveScale);
    const WaterSurfaceSample l =
        SwimSurface(pos.X() - aside.X() * halfWidth, pos.Z() - aside.Z() * halfWidth, level, waveScale);
    const WaterSurfaceSample r =
        SwimSurface(pos.X() + aside.X() * halfWidth, pos.Z() + aside.Z() * halfWidth, level, waveScale);
    const float surface = (2.0f * c.height + f.height + b.height + l.height + r.height) * (1.0f / 6.0f);
    const float waterVX = (2.0f * c.velocityX + f.velocityX + b.velocityX + l.velocityX + r.velocityX) * (1.0f / 6.0f);
    const float waterVZ = (2.0f * c.velocityZ + f.velocityZ + b.velocityZ + l.velocityZ + r.velocityZ) * (1.0f / 6.0f);

    // drift: the waves' orbital motion carries the swimmer to and fro
    pos[0] += waterVX * SwimDrift * deltaT;
    pos[2] += waterVZ * SwimDrift * deltaT;

    // heave: the animation's water line onto the surface, a spring-damper that also follows the
    // surface's own rise and fall, so a crest lifts the man rather than washing over him
    float waterLine = SwimWaterLine;
    const int memory = _shape->FindMemoryLevel();
    if (!IsDead() && memory >= 0 && Type()->_cameraPoint >= 0)
    {
        const float eye = AnimatePoint(memory, Type()->_cameraPoint).Y();
        _swimEye = _swimEyeValid ? _swimEye + (eye - _swimEye) * std::min(1.0f, deltaT / SwimEyeTime) : eye;
        _swimEyeValid = true;
        waterLine = _swimEye - SwimEyeAbove;
    }
    const float target = surface - waterLine;
    const float targetVy = (_swimTargetValid && deltaT > 0) ? (target - _swimPrevTarget) / deltaT : 0.0f;
    _swimPrevTarget = target;
    _swimTargetValid = true;
    const float y0 = pos.Y();
    float y = y0;
    // sub-step a long frame: the spring stays stable whatever the frame rate
    const int steps = std::max(1, std::min(8, int(std::ceil(deltaT / 0.02f))));
    const float h = deltaT / steps;
    for (int i = 0; i < steps; i++)
    {
        const float acc = SwimHeaveSpring * (target - y) + SwimHeaveDamping * (targetVy - _swimVy);
        _swimVy += acc * h;
        y += _swimVy * h;
    }
    // never through the bed (a trough over shallow water)
    const float bed = GLandscape->SurfaceY(pos.X(), pos.Z()) - 0.3f;
    if (y < bed)
    {
        y = bed;
        _swimVy = std::max(_swimVy, 0.0f);
    }
    pos[1] = y;
    static const bool swimLog = std::getenv("POSEIDON_SWIM_LOG") != nullptr;
    if (swimLog && Brain() && Brain()->IsPlayer())
    {
        static float next = 0;
        if (Glob.time.toFloat() >= next)
        {
            next = Glob.time.toFloat() + 0.25f;
            const int mem = _shape->FindMemoryLevel();
            const float eyeY = Type()->_cameraPoint >= 0 && mem >= 0 ? PositionModelToWorld(AnimatePoint(mem, Type()->_cameraPoint)).Y() : 0.0f;
            LOG_INFO(World, "Swim: level={:.2f} surface={:.2f} c={:.2f} y={:.2f} target={:.2f} vy={:.2f} bed={:.2f} water=({:.2f},{:.2f}) move={} eye-origin={:.2f} eye-surface={:.2f} limbs=({:.2f},{:.2f},{:.2f},{:.2f})",
                     level, surface, c.height, y, target, _swimVy, bed + 0.3f, waterVX, waterVZ,
                     (const char*)Type()->GetMoveName(_primaryMove.id), eyeY - y, eyeY - c.height, _swimLimbRel[0], _swimLimbRel[1], _swimLimbRel[2], _swimLimbRel[3]);
            if (GScene && GScene->GetCamera())
            {
                const Vector3 cd = GScene->GetCamera()->Direction();
                const Vector3 cp = GScene->GetCamera()->Position();
                const float pred = QueryWaterSurfaceScaled(pos.X(), pos.Z(), Glob.time.toFloat(), level, waveScale).height;
                float drawnY = 0;
                const bool hasDrawn = DrawnWaterHeightAt(pos.X(), pos.Z(), drawnY);
                LOG_INFO(World, "Swim cam: drawn={} drawn-pred={:.2f} dir=({:.2f},{:.2f},{:.2f}) camY-surface={:.2f} up=({:.2f},{:.2f},{:.2f}) headX={:.2f}", hasDrawn ? 1 : 0, hasDrawn ? drawnY - pred : 0.0f, cd.X(), cd.Y(), cd.Z(),
                         cp.Y() - c.height, _swimUp.X(), _swimUp.Y(), _swimUp.Z(), _headXRot);
            }
        }
    }

    // lean: the body tilts with the slope of the water under it
    const float slopeF = (f.height - b.height) / (2.0f * halfLength);
    const float slopeA = (r.height - l.height) / (2.0f * halfWidth);
    Vector3 waterUp = VUp - fwd * slopeF - aside * slopeA;
    waterUp.Normalize();
    Vector3 up = VUp * (1.0f - SwimLean) + waterUp * SwimLean;
    up.Normalize();
    _swimUp += (up - _swimUp) * std::min(1.0f, deltaT / SwimLeanTime);
    _swimUp.Normalize();

    Frame moveTrans = *this;
    Matrix3 orient;
    const Matrix3 rotate(MRotationY, turn);
    orient.SetUpAndDirection(_swimUp, rotate * fwd);
    moveTrans.SetOrientation(orient);
    moveTrans.SetPosition(pos);

    const Vector3 oldPos = Position();
    _speed = deltaT > 0 ? (pos - oldPos) * (1.0f / deltaT) : VZero;
    _angMomentum = VZero;
    _angVelocity = VZero;
    _acceleration = VZero;
    _landContact = true; // held by the water: no free fall
    _objectContact = false;
    _waterContact = true;
    _waterDepth = std::max(0.0f, c.height - pos.Y()); // the body hangs this deep; nobody drowns at the surface
    _waterBuoyancyContact = false;
    _surfaceSound = GLandscape->GetWaterSurface()._soundEnv;

    Move(moveTrans);
    DirectionWorldToModel(_modelSpeed, _speed);
    RecalcPositions(moveTrans);
    (void)change;

    if (Brain() && Brain()->IsPlayer())
    {
        SetPlayerWaterDepth(_waterDepth);
        _hydroWaterDepth = _waterDepth;
    }
    EmitSwimSplashes(deltaT);
}

void Man::EmitSwimSplashes(float deltaT)
{
    if (!SwimSplashEnabled() || deltaT <= 0 || !GScene || !GScene->GetCamera())
    {
        _swimLimbValid = false;
        return;
    }
    const Vector3 toCam = GScene->GetCamera()->Position() - Position();
    if (toCam.SquareSize() > SwimSplashRange * SwimSplashRange)
    {
        _swimLimbValid = false;
        return;
    }
    const ManType* type = Type();
    type->FindSwimLimbs();
    float level, waveScale;
    SwimWaterLevel(Position().X(), Position().Z(), level, waveScale);
    for (int i = 0; i < 4; i++)
    {
        const int bone = type->_swimLimbBone[i];
        if (bone < 0)
        {
            continue;
        }
        AnimationRTWeight weight;
        weight.Add(AnimationRTPair(bone, 1.0f));
        Matrix4 boneMat = MIdentity;
        AnimateMatrix(boneMat, weight);
        const Vector3 p = PositionModelToWorld(Vector3(VFastTransform, boneMat, SwimLimbRest[i]));
        const WaterSurfaceSample s = SwimSurface(p.X(), p.Z(), level, waveScale);
        const float rel = p.Y() - s.height;
        if (_swimLimbValid)
        {
            const Vector3 vel = (p - _swimLimbPos[i]) * (1.0f / deltaT);
            const float relVy = (rel - _swimLimbRel[i]) / deltaT;
            const float slide = std::sqrt(Square(vel.X() - s.velocityX) + Square(vel.Z() - s.velocityZ));
            _swimLimbCool[i] -= deltaT;
            // a limb slapping in, pulled out, or sweeping fast along (or just under) the surface
            const bool enter = _swimLimbRel[i] > 0.0f && rel <= 0.0f && relVy < -0.3f;
            const bool leave = _swimLimbRel[i] <= 0.0f && rel > 0.0f && relVy > 0.3f;
            const bool sweep = rel < 0.10f && rel > -0.20f && slide > 1.2f;
            if ((enter || leave || sweep) && _swimLimbCool[i] <= 0)
            {
                const float strength = enter ? std::fabs(relVy) : leave ? 0.6f * relVy : 0.45f * slide;
                const float height = std::clamp(0.05f + 0.12f * strength, 0.08f, 0.55f);
                HydroWaterInteractionEvent event{};
                event.positionRadius[0] = p.X();
                event.positionRadius[1] = p.Z();
                event.positionRadius[2] = 0.08f + 0.1f * height;
                event.positionRadius[3] = s.height;
                event.velocityKind[0] = vel.X();
                event.velocityKind[1] = vel.Z();
                event.velocityKind[2] = relVy;
                event.velocityKind[3] = HydroWaterInteractionSwimSplash;
                event.timeLifeFoamMass[3] = height;
                SubmitWaterInteraction(event);
                _swimLimbCool[i] = sweep && !enter ? 0.2f : 0.12f;
            }
        }
        _swimLimbRel[i] = rel;
        _swimLimbPos[i] = p;
    }
    _swimLimbValid = true;
}
} // namespace Poseidon
