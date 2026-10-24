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
// Gear: a launcher, or a machine gun (CWA's own rule: it takes the secondary slot too), weighs a swimmer
// down -- slower strokes and lower in the water (SwimGearLoad).
//
// Falling (UpdateFalling, the swim pack's Fall / FallLand / SwimIn moves): a man dropping for a moment plays
// Fall -- upright, rifle held in both hands -- and is kept upright unless a blast or a hit throws him. On land a
// hard touchdown crouches (FallLand), then the normal moves; into deep water he plunges with his speed (the
// heave runs soft for a moment), comes up in SwimIn (the rifle from the hands to slung), and a fall from
// height hurts. A man put on the water (a mission's placement, getting out of a boat) just starts swimming.
//
// Diving (Sinkhole W4, SimDive; the swim pack's SwimDive / SwimUnder / SwimUnderIdle / SwimSurface moves): a swimmer
// the player steers dives on the move-down key where the water is deep enough. Under water there is no heave: he
// holds his depth, the move-up / move-down keys take him up and down, the body pitching a little with it, and
// the stroke (forward) or the hover (otherwise) play. Near the surface going up, or on move-up close under it, he
// surfaces into treading. He has DiveBreath seconds of air under water, then drowns slowly; at the surface it
// comes back. The camera may follow him under (World.cpp, SwimmerDiving). AI swimmers do not dive.
//
// A config without the swim moves keeps the old behaviour (the man walks the sea bed), and without the fall
// moves falling stays the engine's rigid tumble. Dead men float face down. POSEIDON_SWIM=0 turns swimming
// and the falling off, POSEIDON_SWIM_SPLASH=0 the splashes.

#include <Poseidon/World/Entities/Infantry/SoldierOldCommon.hpp>
#include <Poseidon/World/Entities/Infantry/SwimView.hpp>

#include <Poseidon/AI/AIUnit.hpp>
#include <Poseidon/Graphics/Rendering/WaterInteractionBridge.hpp>
#include <Poseidon/UI/InGame/InGameUI.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Simulation/Animation/RtAnimation.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Terrain/WaterBodies.hpp>
#include <Poseidon/World/Terrain/WaterSurfaceQuery.hpp>
#include <Poseidon/World/Entities/Vehicles/Misc/Ship.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace Poseidon
{
namespace
{
// A living swimmer's water line (SwimWaterLine, SwimView.hpp) follows his chin -- the head bone's neck joint,
// SwimHeadRest, smoothed -- whatever the move (treading holds the head up, the stroke leans it forward): the
// water stands SwimChinAbove under it, so a loaded soldier rides as low as he really does, mouth and eyes
// clear, shoulders under. (It followed the "zamerny" memory point, taken for the eyes: that point sits at the
// chest, 1.20 m over the feet against the eyes' ~1.55, and held the water at his waist.) A dead man (and a
// model without the head bone) floats at SwimWaterLine.
constexpr float SwimChinAbove = 0.02f;
constexpr float SwimChinTime = 0.6f;
// The player in first person rides this much higher (m), so the drawn crests stay under his eyes.
constexpr float SwimEyeLift = 0.15f;
// The head bone's neck joint in the men's rest (model) space -- the chin line (the CWA skeleton, as below).
const Vector3 SwimHeadRest(0.011f, 0.516f, 0.003f);
// Heave: spring (1/s^2) and damping (1/s) pulling the water line onto the surface. A 1.2 s natural
// period against swell of 6-10 s: the body follows the waves with little lag, but still plunges and
// bobs back up after a jump in.
constexpr float SwimHeaveSpring = 28.0f;
constexpr float SwimHeaveDamping = 8.5f;
// the fastest the water line he follows may rise or fall (m/s): a storm sea's surface moves at 2-3 m/s,
// but the surface he samples can STEP -- the drawn grid near the camera refreshes every 1-3 frames and
// stands up to 2 m off the CPU predictor in a storm, so a free camera passing him switched sources --
// and the step, read as the water's speed (125 m/s for 2 m in a frame), threw him 3 m out of the water
// (owner, 2026-10-13)
constexpr float SwimTargetMaxRate = 3.0f;
// how much faster than the surface he follows the body may rise (m/s)
constexpr float SwimRiseMaxExcess = 3.0f;
// Share of the waves' orbital velocity that carries the swimmer.
constexpr float SwimDrift = 0.85f;
// How far the body leans with the wave face (0 upright .. 1 square to the surface), and how fast (s).
constexpr float SwimLean = 0.7f;
constexpr float SwimLeanTime = 0.35f;
// Gear load (SwimGearLoad): the stroke's travel scales by it, and the chin rides lower by SwimGearSink per unit
// short of 1 (a launcher: 5 cm, the water at the mouth).
constexpr float SwimGearMG = 0.9f;
constexpr float SwimGearLauncher = 0.8f;
constexpr float SwimGearSink = 0.25f;
// Falling in: the heave runs this soft (share of the spring) for this long, so the body plunges with its speed
// and bobs up; water stops hurting below WaterImpactSafe (m/s, ~10 m drop) and kills around +WaterImpactSpan.
constexpr float SwimEntrySoftTime = 1.2f;
constexpr float SwimEntrySoftSpring = 0.3f;
constexpr float WaterImpactSafe = 14.0f;
constexpr float WaterImpactSpan = 12.0f;
// Falling: Fall starts after this long in the air dropping faster than this (s, m/s) -- not on a step or a
// bump; the body rights itself over FallRightTime (s); a touchdown faster than FallLandSpeed (m/s) crouches.
// A blast or hit (an impulse over 5 g) lets him tumble for FallTumbleTime (s).
constexpr float FallStartTime = 0.25f;
constexpr float FallStartSpeed = 3.0f;
constexpr float FallRightTime = 0.3f;
constexpr float FallLandSpeed = 4.0f;
constexpr float FallTumbleTime = 1.5f;
// A jump (SoldierOldJump.cpp) turns into Fall only after this long in the air (s): off a ledge, not a hop.
constexpr float JumpFallAfter = 0.9f;
// Put on the water: a man this close over a deep spot (m) starts swimming with no fall.
constexpr float SwimPlacedAbove = 1.0f;
// Splashes: only for swimmers this close to the camera (m).
constexpr float SwimSplashRange = 150.0f;
// The finger tips and toe tips in the men's rest (model) space -- the wrist and toe joints of the CWA
// skeleton (fitted from the stock animations) a hand's / a toe's length on.
const Vector3 SwimLimbRest[4] = {Vector3(-0.003f, -0.40f, 0.22f), Vector3(0.007f, -0.40f, -0.206f),
                                 Vector3(0.19f, -1.08f, 0.229f), Vector3(0.186f, -1.08f, -0.221f)};

// Diving. The underwater moves lie flat with the body DiveBodyMid over the model origin and the head
// DiveHeadMid; a dive needs this much water (mean level to bed, m).
constexpr float DiveBodyMid = 1.05f;
constexpr float DiveHeadMid = 1.0f;
constexpr float DiveMinDepth = 2.2f;
// up / down speed on the keys (m/s), how fast it is reached (s), and the sink while the dive move plays
constexpr float DiveVerticalSpeed = 0.9f;
constexpr float DiveVerticalTime = 0.5f;
constexpr float DiveEntrySink = 1.1f;
// he stays this far under (body middle to surface) unless surfacing, keeps this far off the bed, and surfaces
// when going up within DiveSurfaceAt
constexpr float DiveStayUnder = 0.45f;
constexpr float DiveBedClear = 0.25f;
constexpr float DiveSurfaceAt = 0.6f;
// the body pitches up to this much (rad) with the vertical speed, about its middle
constexpr float DivePitchMax = 0.35f;
// air: seconds under water (a loaded soldier, breath held), refilled this many times faster at the surface; out
// of air he drowns: his damage rises to death over DiveDrownTime whatever his armour (a hit-point rate did not kill
// him in two minutes -- owner, 2026-10-17: "at one point he needs to drown")
constexpr float DiveBreath = 40.0f;
constexpr float DiveBreathRefill = 4.0f;
constexpr float DiveDrownTime = 12.0f;

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
    level = GLandscape->SeaLevelAt(x, z);
    waveScale = 1.0f;
    if (const WaterBody* body = GetWaterBodies().Find(x, z))
    {
        level = body->SurfaceLevelAt(x, z);
        waveScale = body->waveScale;
    }
}

// `queried`: the drawn surface's height here from the renderer's point queries (SwimBodySurface), used in
// place of the predictor's; the drawn grid round the camera still has the last word.
WaterSurfaceSample SwimSurface(float x, float z, float level, float waveScale, const float* queried = nullptr)
{
    // WGR_WAVE_BUOYANCY=0 (Collisions.cpp): the flat sea, for A/B
    static const bool waves = EnvOn("WGR_WAVE_BUOYANCY");
    if (!waves)
    {
        return WaterSurfaceSample{level, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    }
    WaterSurfaceSample sample = QueryWaterSurfaceScaled(x, z, Glob.time.toFloat(), level, waveScale);
    if (queried)
    {
        sample.height = *queried;
    }
    // near the camera, the surface as the Tidewater water draws it: the predictor matches the drawn sea in
    // scale, not crest for crest, and the gap (half a metre at times) put the player's eyes under the drawn
    // crests. POSEIDON_SWIM_DRAWN=0: the predictor everywhere.
    static const bool drawn = EnvOn("POSEIDON_SWIM_DRAWN");
    float y, w = 1.0f;
    if (drawn && DrawnWaterHeightAt(x, z, y, &w))
    {
        // blended to the predictor over the grid's outer 2 m: a boat whose hull straddled the edge stood
        // on two seas metres apart in a storm and was thrown into the air (owner, 2026-10-13)
        sample.height += (y - sample.height) * w;
    }
    return sample;
}
// The body's samples on the sea as it is DRAWN, wherever the camera is: the swimmer asks the renderer for
// his own points every step (RequestWaterQueries, the boats' hull path, answered 1-3 frames late) and
// rides the answers. Outside the 16 m drawn grid round the camera he rode the CPU predictor, which
// matches the drawn sea crest for crest nowhere and, in a maxed storm, stood three times its height:
// watched from the free camera he flew metres over the drawn troughs and sank under the crests (owner,
// 2026-10-22: "the soldier flies around in the water when i look at him in freefly"). All samples or
// none, so the body never straddles two seas; a server, another water mode or a stale answer keeps
// the predictor. Only for swimmers near the camera (the queries are shared with the boats' hulls).
// POSEIDON_SWIM_QUERY=0: the predictor (and the grid) only.
constexpr int SwimBodySamples = 5;
constexpr float SwimQueryRange = 300.0f;

void SwimBodySurface(const void* key, const float (&xz)[SwimBodySamples][2], float level, float waveScale,
                     WaterSurfaceSample (&out)[SwimBodySamples])
{
    static const bool query = EnvOn("POSEIDON_SWIM_QUERY") && EnvOn("WGR_WAVE_BUOYANCY");
    bool fromQuery = query;
    if (query)
    {
        const Camera* camera = GScene ? GScene->GetCamera() : nullptr;
        const float dx = camera ? camera->Position().X() - xz[0][0] : 0.0f;
        const float dz = camera ? camera->Position().Z() - xz[0][1] : 0.0f;
        fromQuery = camera && dx * dx + dz * dz < SwimQueryRange * SwimQueryRange;
    }
    float heights[SwimBodySamples];
    if (fromQuery)
    {
        float slopeX, slopeZ;
        for (int i = 0; i < SwimBodySamples && fromQuery; i++)
        {
            fromQuery = DrawnWaterQuery(xz[i][0], xz[i][1], heights[i], slopeX, slopeZ);
        }
        RequestWaterQueries(reinterpret_cast<uintptr_t>(key), &xz[0][0], SwimBodySamples);
    }
    for (int i = 0; i < SwimBodySamples; i++)
    {
        out[i] = SwimSurface(xz[i][0], xz[i][1], level, waveScale, fromQuery ? &heights[i] : nullptr);
    }
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

bool SwimmerDiving(const Object* obj)
{
    const Man* man = obj ? dyn_cast<const Man>(obj) : nullptr;
    return man && man->IsDiving();
}

bool SwimmerCameraWater(const Object* obj, float x, float z, float& waterY)
{
    const Man* man = obj ? dyn_cast<const Man>(obj) : nullptr;
    if (!man || !man->IsSwimming() || man->IsDiving() || !GLandscape)
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
    _swimHeadBone = skeleton ? skeleton->FindBone("hlava") : -1;
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
    _fallMove = Type()->GetMoveId("Fall");
    _fallLandMove = Type()->GetMoveId("FallLand");
    _swimInMove = Type()->GetMoveId("SwimIn");
    _swimDiveMove = Type()->GetMoveId("SwimDive");
    _swimUnderIdleMove = Type()->GetMoveId("SwimUnderIdle");
    _swimSurfaceMove = Type()->GetMoveId("SwimSurface");
    _jumpMove = Type()->GetMoveId("Jump");
    _jumpAirMove = Type()->GetMoveId("JumpAir");
    _jumpLandMove = Type()->GetMoveId("JumpLand");
}

float Man::SwimGearLoad() const
{
    float load = 1.0f;
    for (int i = 0; i < NWeaponSystems(); i++)
    {
        const WeaponType* weapon = GetWeaponSystem(i);
        if (weapon && (weapon->_weaponType & MaskSlotSecondary))
        {
            load = std::min(load, (weapon->_weaponType & MaskSlotPrimary) ? SwimGearMG : SwimGearLauncher);
        }
    }
    return load;
}

void Man::UpdateFalling(float deltaT)
{
    InitSwimMoves();
    if (_fallMove == MoveIdNone || !SwimEnabled())
    {
        return; // no fall moves (or swimming off): the engine's rigid fall, as before
    }
    if (_fallLanding)
    {
        if (_primaryMove.id != _fallLandMove || _primaryTime >= 0.98f)
        {
            _fallLanding = false;
            if (_primaryMove.id == _fallLandMove)
            {
                BlendToMove(GetDefaultMove());
            }
        }
        return;
    }
    AIUnit* unit = Brain();
    const bool inVehicle = unit && unit->GetVehicleIn();
    const bool airborne = (!_landContact && !_objectContact) || JumpLiftingOff();
    if (_jumping)
        _jumpTime += deltaT;
    else
        _sinceJumpLanded += deltaT;
    if (IsDead() || _ladderBuilding || inVehicle || !airborne)
    {
        const bool jumped = _jumping;
        if (_jumping)
        {
            _jumping = false;
            _sinceJumpLanded = 0;
        }
        if ((_falling || jumped) && !IsDead() && !inVehicle)
        {
            // touchdown: a hard one crouches, a soft one goes back to the normal moves with the knees giving for a
            // moment (the landing layer); both are heard
            PlayJumpSound(std::clamp(0.9f + 0.12f * _fallSpeed, 0.9f, 1.8f));
            if (_fallSpeed > FallLandSpeed && _fallLandMove != MoveIdNone)
            {
                BlendToMove(_fallLandMove);
                _fallLanding = true;
            }
            else
            {
                BlendToMove(GetDefaultMove());
                StartLandingLayer(_fallSpeed);
            }
        }
        _falling = false;
        _airTime = 0;
        _fallSpeed = 0;
        _fallTumble = 0;
        return;
    }
    _airTime += deltaT;
    const float vy = Speed().Y();
    _fallSpeed = std::max(_fallSpeed, -vy);
    if (_impulseForce.SquareSize() > Square(GetMass() * 5))
    {
        _fallTumble = FallTumbleTime;
    }
    _fallTumble = std::max(0.0f, _fallTumble - deltaT);
    if (_jumping && !_falling && !(_airTime > JumpFallAfter && vy < -FallStartSpeed))
    {
        // a jump plays its own moves; it turns into Fall only if it lasts (off a ledge), not at every hop's end
        KeepJumpMoves();
    }
    else
    {
        if (!_falling)
        {
            if (_airTime < FallStartTime || vy > -FallStartSpeed)
            {
                return;
            }
            _falling = true;
        }
        if (_primaryMove.id != _fallMove)
        {
            BlendToMove(_fallMove);
        }
    }
    if (_fallTumble > 0)
    {
        return;
    }
    // upright: no spin, the body righted toward vertical, the heading kept
    _angMomentum = VZero;
    _angVelocity = VZero;
    Vector3 dir = Direction();
    dir[1] = 0;
    dir = dir.SquareSize() > 1e-4f ? dir.Normalized() : Vector3(VForward);
    Vector3 up = DirectionUp() + (VUp - DirectionUp()) * std::min(1.0f, deltaT / FallRightTime);
    up.Normalize();
    Matrix3 orient;
    orient.SetUpAndDirection(up, dir);
    Frame trans = *this;
    trans.SetOrientation(orient);
    Move(trans);
}

bool Man::CanSwim() const
{
    InitSwimMoves();
    return _swimMoves[0] != MoveIdNone;
}

bool Man::SwimSystemActive() const
{
    return SwimEnabled() && CanSwim();
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
    // under water (W4): the SwimUnderActions map
    return _swimUnderIdleMove != MoveIdNone && map && map == Type()->GetActionMap(_swimUnderIdleMove);
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
        // put on the water (a mission's placement, out of a boat): over a deep spot, close above the surface, not
        // standing on anything and not already dropping fast -- he swims at once, with no fall
        const float vy = Speed().Y();
        bool placed = !_objectContact && pos.Y() < level + SwimPlacedAbove && vy < 0.5f && vy > -FallStartSpeed;
        if (placed && Ship::DeckShips() > 0)
        {
            // ...but not a man just stepped out onto a boat's deck: the sole is a third of a metre over the sea, and
            // in his first frame there he is neither a rider yet (OnPlatform) nor in contact, so he was "placed on
            // the water" and swam off the deck (owner, 2026-10-22: "when exiting, i do not want to end up in the
            // water but on deck of the boat")
            Object* support = nullptr;
            const float deckY = GLandscape->RoadSurfaceY(pos + VUp * 0.5f, nullptr, nullptr, nullptr, &support);
            if (support && dyn_cast<Ship>(support) && std::fabs(pos.Y() - deckY) < 0.5f)
                placed = false;
        }
        // (a man standing on a boat's deck is neither: the deck is his ground, however low over the sea)
        if (IsDead() || _ladderBuilding || depth < SwimStartDepth || OnPlatform() ||
            (level - pos.Y() < SwimStartDepth - 0.1f && !placed) || GLandscape->InTerrainHole(pos.X(), pos.Z()))
        {
            return false;
        }
        _swimming = true;
        _diving = false;
        _diveRequest = false;
        _surfaceRequest = false;
        _diveVy = 0;
        _swimVy = vy;
        // fallen in: the body plunges with its speed (soft heave) and comes up in SwimIn; a fall from height hurts
        const bool fallenIn = vy < -FallStartSpeed;
        _swimEntrySoft = fallenIn ? SwimEntrySoftTime : 0.0f;
        if (fallenIn && -vy > WaterImpactSafe)
        {
            const float hurt = (-vy - WaterImpactSafe) / WaterImpactSpan * GetInvArmor() * 3;
            LocalDammage(nullptr, this, VZero, std::min(hurt, 5.0f), 1.0f);
        }
        _falling = false;
        _fallLanding = false;
        _airTime = 0;
        _fallSpeed = 0;
        _swimTargetValid = false;
        _swimChinValid = false;
        _swimLimbValid = false;
        _swimUp = VUp;
        _waterBuoyancyContact = false;
        BlendToMove(fallenIn && _swimInMove != MoveIdNone ? _swimInMove : swim);
        return true;
    }
    // lifted clear of the water as it stands here, not of its mean level: the body rides 1.4 m under the chin line,
    // so on a crest some 3 m over the mean (a rough Tidewater sea) a man still swimming read as lifted out, left
    // the swim with the crest's rise as his speed and was thrown metres into the air, to fall back in and be
    // thrown again (owner, 2026-10-22: "bouncing and flying high out of the water if the water amplitude is high")
    // (the drawn surface's own height too when the renderer has answered for him: either may be the higher)
    float waterHere = std::max(level, SwimSurface(pos.X(), pos.Z(), level, waveScale).height);
    float drawnY, slopeX, slopeZ;
    if (DrawnWaterQuery(pos.X(), pos.Z(), drawnY, slopeX, slopeZ))
    {
        waterHere = std::max(waterHere, drawnY);
    }
    const bool liftedClear = pos.Y() > waterHere + SwimPlacedAbove + 0.5f;
    if (liftedClear || (depth < SwimStopDepth && !IsDead()))
    {
        // Both a shallow-water exit and a scripted lift/relocation must leave the
        // swim action map. Clearing only _swimming leaves its looping move active
        // on dry land. Above deep water the normal falling path can take over.
        _swimming = false;
        _diving = false;
        _diveRequest = false;
        _surfaceRequest = false;
        _diveOffset = VZero;
        _swimEntrySoft = 0;
        if (IsDead())
        {
            return false;
        }
        BlendToMove(!liftedClear && _swimOut != MoveIdNone ? _swimOut : GetDefaultMove());
        Matrix3 upright;
        Vector3 dir = Direction();
        dir[1] = 0;
        upright.SetUpAndDirection(VUp, dir.SquareSize() > 1e-4f ? dir : Vector3(VForward));
        Frame trans = *this;
        trans.SetOrientation(upright);
        if (!liftedClear)
        {
            // Wading out: stand him on the bottom. The swimmer's body rides ~0.3 m under where his feet
            // stand at the 1.2 m exit depth, so the ground contact used to throw him up to 1.6 m out of
            // the surf at 3.5 m/s into a fall -- on a steep beach or in surf the owner "flew around and
            // died" (2026-10-17).
            Vector3 standing = trans.Position();
            standing[1] = ground;
            trans.SetPosition(standing);
            Move(trans);
            _speed = VZero;
            _landContact = true;
            _freeFallUntil = Glob.time;
            return false;
        }
        Move(trans);
        _landContact = false;
        return false;
    }
    if (!IsDead() && !IsSwimMove(_primaryMove.id))
    {
        // something set a land move while swimming (getting out of a boat, a script switchMove):
        // back to treading water (holding still, under water)
        BlendToMove(_diving && _swimUnderIdleMove != MoveIdNone ? _swimUnderIdleMove : swim);
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

    Vector3 pos = Position() - _diveOffset;
    // horizontal travel from the swim cycle, like walking -- slower under a heavy load
    const float gear = SwimGearLoad();
    pos += (aside * moveX + fwd * moveZ) * gear;

    // the water under the body: five samples over its length and width, so ripples shorter than a man
    // average out and the slope under him is the swell's
    float level, waveScale;
    SwimWaterLevel(pos.X(), pos.Z(), level, waveScale);
    const float halfLength = 0.8f;
    const float halfWidth = 0.35f;
    const float bodyXz[SwimBodySamples][2] = {{pos.X(), pos.Z()},
                                              {pos.X() + fwd.X() * halfLength, pos.Z() + fwd.Z() * halfLength},
                                              {pos.X() - fwd.X() * halfLength, pos.Z() - fwd.Z() * halfLength},
                                              {pos.X() - aside.X() * halfWidth, pos.Z() - aside.Z() * halfWidth},
                                              {pos.X() + aside.X() * halfWidth, pos.Z() + aside.Z() * halfWidth}};
    WaterSurfaceSample body[SwimBodySamples];
    SwimBodySurface(this, bodyXz, level, waveScale, body);
    const WaterSurfaceSample& c = body[0];
    const WaterSurfaceSample& f = body[1];
    const WaterSurfaceSample& b = body[2];
    const WaterSurfaceSample& l = body[3];
    const WaterSurfaceSample& r = body[4];
    const float surface = (2.0f * c.height + f.height + b.height + l.height + r.height) * (1.0f / 6.0f);
    const float waterVX = (2.0f * c.velocityX + f.velocityX + b.velocityX + l.velocityX + r.velocityX) * (1.0f / 6.0f);
    const float waterVZ = (2.0f * c.velocityZ + f.velocityZ + b.velocityZ + l.velocityZ + r.velocityZ) * (1.0f / 6.0f);

    // drift: the waves' orbital motion carries the swimmer to and fro (under water much less)
    const float drift = _diving ? SwimDrift * 0.3f : SwimDrift;
    pos[0] += waterVX * drift * deltaT;
    pos[2] += waterVZ * drift * deltaT;

    // diving (W4): the player's move-down key, where it is deep enough, once he is settled in the water
    InitSwimMoves();
    if (_diving && IsDead())
    {
        _diving = false; // a dead diver floats up (the heave below takes him)
        _swimVy = _diveVy;
        _swimTargetValid = false;
    }
    if (_diveRequest && (_diving || IsDead() || _swimDiveMove == MoveIdNone ||
                         level - GLandscape->SurfaceY(pos.X(), pos.Z()) < DiveMinDepth))
    {
        _diveRequest = false; // too shallow (or nothing to dive with): the press is dropped, not kept for later
    }
    if (_diveRequest && _swimEntrySoft <= 0 && _primaryMove.id != _swimInMove && _primaryMove.id != _swimSurfaceMove)
    {
        _diveRequest = false;
        _surfaceRequest = false;
        _diving = true;
        _diveVy = std::min(_swimVy, 0.0f);
        BlendToMove(_swimDiveMove);
    }
    if (_diving)
    {
        SimDive(deltaT, surface, pos);
    }
    else
    {
        // heave: the animation's water line onto the surface, a spring-damper that also follows the
        // surface's own rise and fall, so a crest lifts the man rather than washing over him
        float waterLine = SwimWaterLine;
        const ManType* type = Type();
        type->FindSwimLimbs();
        if (!IsDead() && type->_swimHeadBone >= 0)
        {
            AnimationRTWeight weight;
            weight.Add(AnimationRTPair(type->_swimHeadBone, 1.0f));
            Matrix4 headMat = MIdentity;
            AnimateMatrix(headMat, weight);
            const float chin = Vector3(VFastTransform, headMat, SwimHeadRest).Y();
            _swimChin = _swimChinValid ? _swimChin + (chin - _swimChin) * std::min(1.0f, deltaT / SwimChinTime) : chin;
            _swimChinValid = true;
            waterLine = _swimChin - (SwimChinAbove - SwimGearSink * (1.0f - gear)); // a heavy load rides lower
            // Seen through his own eyes he rides SwimEyeLift higher: at the chin line the eyes stood 0-0.25 m over
            // the surface, the crests a metre ahead washed over the view, and the hands showed through the water
            // as if transparent (owner, 2026-10-17). The camera cannot be lifted instead -- it leaves his eyes for
            // the inside of his head (W3). The heave spring eases the change when the view switches.
            if (GWorld && GWorld->CameraOn() == this &&
                (GWorld->GetCameraType() == CamInternal || GWorld->GetCameraType() == CamGunner))
            {
                waterLine -= SwimEyeLift;
            }
        }
        float target = surface - waterLine;
        if (_swimTargetValid && deltaT > 0)
        {
            const float maxStep = SwimTargetMaxRate * deltaT;
            target = std::clamp(target, _swimPrevTarget - maxStep, _swimPrevTarget + maxStep);
        }
        const float targetVy = (_swimTargetValid && deltaT > 0) ? (target - _swimPrevTarget) / deltaT : 0.0f;
        _swimPrevTarget = target;
        _swimTargetValid = true;
        const float y0 = pos.Y();
        float y = y0;
        // sub-step a long frame: the spring stays stable whatever the frame rate
        _swimEntrySoft = std::max(0.0f, _swimEntrySoft - deltaT);
        const int steps = std::max(1, std::min(8, int(std::ceil(deltaT / 0.02f))));
        const float h = deltaT / steps;
        for (int i = 0; i < steps; i++)
        {
            // just fallen in: a soft spring lets the body plunge with its speed and bob up
            const float soft = _swimEntrySoft > 0 ? SwimEntrySoftSpring : 1.0f;
            const float acc = SwimHeaveSpring * soft * (target - y) + SwimHeaveDamping * std::sqrt(soft) * (targetVy - _swimVy);
            _swimVy += acc * h;
            // the water lifts no faster than its own rise and a little: a step in the sampled surface or a
            // spring overshoot on a steep crest must not fling him upward (calm water never reaches this; a
            // 13 m fall in bobs up at 2.8 m/s)
            _swimVy = std::min(_swimVy, std::max(targetVy, 0.0f) + SwimRiseMaxExcess);
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
                LOG_INFO(World, "Swim: keys={:.2f} dive-req={} level={:.2f} surface={:.2f} c={:.2f} y={:.2f} target={:.2f} vy={:.2f} bed={:.2f} water=({:.2f},{:.2f}) move={} eye-origin={:.2f} eye-surface={:.2f} chin-surface={:.2f} limbs=({:.2f},{:.2f},{:.2f},{:.2f})",
                         _swimVertWanted, _diveRequest ? 1 : 0, level, surface, c.height, y, target, _swimVy, bed + 0.3f, waterVX, waterVZ,
                         (const char*)Type()->GetMoveName(_primaryMove.id), eyeY - y, eyeY - c.height,
                         _swimChinValid ? y + _swimChin - c.height : 0.0f, _swimLimbRel[0], _swimLimbRel[1], _swimLimbRel[2], _swimLimbRel[3]);
                if (GScene && GScene->GetCamera())
                {
                    const Vector3 cd = GScene->GetCamera()->Direction();
                    const Vector3 cp = GScene->GetCamera()->Position();
                    const float pred = QueryWaterSurfaceScaled(pos.X(), pos.Z(), Glob.time.toFloat(), level, waveScale).height;
                    float drawnY = 0;
                    const bool hasDrawn = DrawnWaterHeightAt(pos.X(), pos.Z(), drawnY);
                    float queryY = 0, qsx, qsz;
                    const bool hasQuery = DrawnWaterQuery(pos.X(), pos.Z(), queryY, qsx, qsz);
                    LOG_INFO(World, "Swim query: {} query-pred={:.2f} cam-dist={:.1f}", hasQuery ? 1 : 0, hasQuery ? queryY - pred : 0.0f,
                             (cp - pos).SizeXZ());
                    LOG_INFO(World, "Swim cam: drawn={} drawn-pred={:.2f} dir=({:.2f},{:.2f},{:.2f}) camY-surface={:.2f} up=({:.2f},{:.2f},{:.2f}) headX={:.2f}", hasDrawn ? 1 : 0, hasDrawn ? drawnY - pred : 0.0f, cd.X(), cd.Y(), cd.Z(),
                             cp.Y() - c.height, _swimUp.X(), _swimUp.Y(), _swimUp.Z(), _headXRot);
                }
            }
        }
    } // not diving

    // lean: the body tilts with the slope of the water under it; under water it pitches with his climb or sink
    Vector3 up;
    if (_diving)
    {
        const float pitch = std::clamp(_diveVy / DiveVerticalSpeed, -1.0f, 1.0f) * DivePitchMax;
        up = VUp * std::cos(pitch) - fwd * std::sin(pitch);
    }
    else
    {
        const float slopeF = (f.height - b.height) / (2.0f * halfLength);
        const float slopeA = (r.height - l.height) / (2.0f * halfWidth);
        Vector3 waterUp = VUp - fwd * slopeF - aside * slopeA;
        waterUp.Normalize();
        up = VUp * (1.0f - SwimLean) + waterUp * SwimLean;
    }
    up.Normalize();
    _swimUp += (up - _swimUp) * std::min(1.0f, deltaT / SwimLeanTime);
    _swimUp.Normalize();

    Frame moveTrans = *this;
    Matrix3 orient;
    const Matrix3 rotate(MRotationY, turn);
    orient.SetUpAndDirection(_swimUp, rotate * fwd);
    moveTrans.SetOrientation(orient);
    // under water the pitch turns the body about its middle (the flat moves lie DiveBodyMid over the origin); the
    // offset eases out after surfacing rather than jumping
    const Vector3 offset = _diving ? (VUp - _swimUp) * DiveBodyMid : VZero;
    _diveOffset += (offset - _diveOffset) * std::min(1.0f, deltaT / SwimLeanTime);
    moveTrans.SetPosition(pos + _diveOffset);

    // Hulls are solid to a swimmer: the vehicles' geometry pushes him out sideways, as it does a man on foot
    // (the swim step never asked: he swam through a boat, owner 2026-10-19). Only the horizontal part -- the
    // water holds his height.
    if (GLandscape)
    {
        CollisionBuffer collision;
        GLandscape->ObjectCollision(collision, this, moveTrans, true);
        Vector3 push(VZero);
        for (int i = 0; i < collision.Size(); i++)
        {
            const CollisionInfo& info = collision[i];
            if (!info.object || info.object == this || info.object->IsPassable() || dyn_cast<Man, Object>(info.object))
                continue;
            Vector3 out = info.object->DirectionModelToWorld(info.dirOut);
            out[1] = 0;
            const float len = out.Size();
            if (len < 1e-3f || info.under <= 0.02f)
                continue;
            out = out * (1.0f / len);
            const float along = push * out;
            if (info.under - 0.02f > along)
                push += out * (info.under - 0.02f - along);
        }
        if (push.SquareSize() > 1e-6f)
        {
            moveTrans.SetPosition(moveTrans.Position() + push);
            static const bool diag = std::getenv("POSEIDON_BOAT_DIAG") != nullptr;
            if (diag && Brain() && Brain()->IsPlayer())
                LOG_INFO(World, "Swim hull push: t={:.2f} push={:.2f} m at {:.1f},{:.1f}", Glob.time.toFloat(), push.Size(),
                         moveTrans.Position().X(), moveTrans.Position().Z());
        }
    }

    const Vector3 oldPos = Position();
    _speed = deltaT > 0 ? (moveTrans.Position() - oldPos) * (1.0f / deltaT) : VZero;
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
    UpdateBreath(deltaT, _diving ? c.height - (pos.Y() + DiveHeadMid) : -1.0f);
    EmitSwimSplashes(deltaT);
}

void Man::ClimbOntoDeck(Ship* ship, Vector3Par deck)
{
    // out of the swim moves, as the lift out of the water in UpdateSwimming does
    _swimming = false;
    _diving = false;
    _diveRequest = false;
    _surfaceRequest = false;
    _diveOffset = VZero;
    _swimEntrySoft = 0;
    _falling = false;
    BlendToMove(GetDefaultMove());
    // standing on the sole, facing inboard
    Vector3 dir = ship->Position() - deck;
    dir[1] = 0;
    Matrix3 upright;
    upright.SetUpAndDirection(VUp, dir.SquareSize() > 1e-4f ? dir.Normalized() : Vector3(VForward));
    Matrix4 trans;
    trans.SetOrientation(upright);
    trans.SetPosition(deck);
    Move(trans);
    _speed = VZero;
    _landContact = true;
    _freeFallUntil = Glob.time;
    _waterDepth = 0;
    if (Brain() && Brain()->IsPlayer())
        SetPlayerWaterDepth(0);
    // and on board: the deck carries him from now on
    if (Ship* old = _platform ? dyn_cast<Ship>(_platform.GetLink()) : nullptr)
        old->RemoveRider(this);
    ship->AddRider(this);
    _platform = ship;
    LOG_INFO(World, "{} climbed aboard {}", (const char*)GetDebugName(), (const char*)ship->GetDebugName());
}

void Man::SetSwimVerticalWanted(float v)
{
    // the presses are latched: the keys are read in KeyboardPilot and acted on in the swim step, which need not run
    // on the same frame, and a tap (as for going prone) can last one
    const bool down = v < -0.1f, up = v > 0.1f;
    if (_swimming && !_diving && down)
    {
        _diveRequest = true;
    }
    if (_swimming && _diving && up && _swimVertWanted <= 0.1f)
    {
        _surfaceRequest = true;
    }
    if (down)
    {
        _surfaceRequest = false;
    }
    _swimVertWanted = v;
}

void Man::SimDive(float deltaT, float surface, Vector3& pos)
{
    // the dive move sinks him; after it the keys take him up and down, and he holds his depth otherwise
    const bool entering = _primaryMove.id == _swimDiveMove;
    float want = _swimVertWanted * DiveVerticalSpeed;
    if (_surfaceRequest)
    {
        want = std::max(want, DiveVerticalSpeed); // a tap of move up: on up to the surface
    }
    if (entering)
    {
        want = std::min(want, -DiveEntrySink);
    }
    _diveVy += (want - _diveVy) * std::min(1.0f, deltaT / DiveVerticalTime);
    float y = pos.Y() + _diveVy * deltaT;
    const float mid = y + DiveBodyMid;
    if (!entering && _diveVy > 0 && mid > surface - DiveSurfaceAt)
    {
        // up at the surface: into treading, the heave taking him from here softly
        _diving = false;
        _surfaceRequest = false;
        _swimVy = _diveVy;
        _swimTargetValid = false;
        _swimChinValid = false;
        _swimEntrySoft = SwimEntrySoftTime * 0.5f;
        BlendToMove(_swimSurfaceMove != MoveIdNone ? _swimSurfaceMove : _swimMoves[0]);
        pos[1] = y;
        return;
    }
    if (!entering && mid > surface - DiveStayUnder)
    {
        y = surface - DiveStayUnder - DiveBodyMid;
        _diveVy = std::min(_diveVy, 0.0f);
    }
    // off the bed: the flat body's underside is ~0.3 m under its middle
    const float bed = GLandscape->SurfaceY(pos.X(), pos.Z()) + DiveBedClear + 0.3f - DiveBodyMid;
    if (y < bed)
    {
        y = bed;
        _diveVy = std::max(_diveVy, 0.0f);
    }
    pos[1] = y;
    static const bool swimLog = std::getenv("POSEIDON_SWIM_LOG") != nullptr;
    if (swimLog && Brain() && Brain()->IsPlayer())
    {
        static float next = 0;
        if (Glob.time.toFloat() >= next)
        {
            next = Glob.time.toFloat() + 0.25f;
            LOG_INFO(World, "Dive: surface={:.2f} mid={:.2f} depth={:.2f} vy={:.2f} want={:.2f} keys={:.2f} up-req={} bed={:.2f} breath={:.1f} move={}",
                     surface, y + DiveBodyMid, surface - (y + DiveBodyMid), _diveVy, want, _swimVertWanted, _surfaceRequest ? 1 : 0, bed, _breath,
                     (const char*)Type()->GetMoveName(_primaryMove.id));
        }
    }
}

void Man::UpdateBreath(float deltaT, float headDepth)
{
    if (_breath < 0)
    {
        _breath = DiveBreath;
    }
    if (IsDead())
    {
        return;
    }
    const bool player = Brain() && Brain()->IsPlayer() && GWorld && GWorld->UI();
    if (headDepth > 0.1f)
    {
        const float before = _breath;
        _breath -= deltaT;
        if (player && before > 10.0f && _breath <= 10.0f)
        {
            GWorld->UI()->ShowHint("Running out of air - surface (move up)");
        }
        if (_breath <= 0)
        {
            if (player && before > 0)
            {
                GWorld->UI()->ShowHint("Out of air!");
            }
            _breath = 0;
            SetDammage(std::min(1.0f, GetTotalDammage() + deltaT / DiveDrownTime));
        }
    }
    else
    {
        _breath = std::min(DiveBreath, _breath + deltaT * DiveBreathRefill);
    }
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
