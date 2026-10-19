// Corner peeking (leaning), Arma style: Q leans left, E right; hold to lean while held, tap to
// latch the lean until the next tap.
//
// The lean is procedural, not a set of RTM moves. It is one more rigid transform folded into
// _gunTrans, the matrix the engine already applies to the "aiming" blend selection (spine, arms,
// head, the weapon proxy) on top of whatever move is playing. Everything that reads the upper body
// therefore leans with it, with no second code path to keep in step:
//  * the drawn body and weapon, in every LOD, first and third person;
//  * the eye (pilot point) and the optics camera, which are animated memory points;
//  * the muzzle and barrel direction (AnimateProxyMatrix), so shots leave from the leaned weapon;
//  * the fire geometry, which Animate() poses the same way, so he is hit where he is drawn.
// RTM lean poses would have needed one per stance x weapon x move and would still have left the
// eye and muzzle to match by hand; the aiming blend weights already fade the transform out down
// the spine to the hips, which is exactly where a lean bends.
//
// The transform rolls the upper body about the body's forward axis, pivoting low in the spine,
// plus a small sideways hip shift. It is applied before the aim rotation (aim * lean), so the
// barrel keeps pointing where the player aims and the rifle cants like a leaning shooter's.
//
// Standing and crouched only: prone, swimming, on a ladder, in a vehicle or seat, or dead he does
// not lean (and the Q/E keys go back to their OFP meaning: stance-up, slow/fast forward, surface).
// A wall on the lean side stops the lean before the head goes into it (UpdateLeanReach).
// AI does not lean yet, and the lean is not sent over the network.

#include <Poseidon/World/Entities/Infantry/SoldierOldCommon.hpp>

#include <Poseidon/AI/AIUnit.hpp>
#include <Poseidon/Input/InputSubsystem.hpp>

#include <algorithm>
#include <cmath>

namespace Poseidon
{

namespace
{
constexpr float kLeanAngle = 0.33f;       // upper-body roll at full lean (19 degrees)
constexpr float kLeanShift = 0.12f;       // sideways hip shift at full lean, metres
constexpr float kLeanPivotDrop = 0.50f;   // the roll pivot sits this far below the aiming axis
constexpr float kLeanRate = 4.0f;         // full lean in a quarter second
constexpr float kLeanTapTime = 0.25f;     // a shorter press latches the lean
constexpr float kLeanHeadRadius = 0.15f;  // clearance kept between the eye and a wall
constexpr float kLeanShoulderOut = 0.18f; // lean-side shoulder, from the eye
constexpr float kLeanShoulderDrop = 0.25f;
constexpr float kLeanShoulderRadius = 0.10f;
constexpr float kLeanMaxSpeed = 3.2f; // m/s: walking and crouch-walking lean, running does not
} // namespace

bool Man::CanLean() const
{
    if (_isDead || IsSwimming() || IsDown() || _ladderBuilding)
        return false;
    if (Type()->_aimingAxisPoint < 0 || GetActiveCargoWeaponSeat() || GetHierachyParent())
        return false;
    const AIUnit* unit = Brain();
    if (unit && unit->GetVehicleIn())
        return false;
    // Moves that hold the weapon still (limitGunMovement 0: no weapon, reloading prone) do not lean,
    // and neither does a running man: leaning is for standing still and walking pace.
    if (GetLimitGunMovement() < 0.3f)
        return false;
    Vector3 speed = Speed();
    speed[1] = 0;
    return speed.SquareSize() < Square(kLeanMaxSpeed);
}

Matrix4 Man::LeanTransform(Vector3Par gunAxis, float lean) const
{
    const Vector3 pivot = gunAxis - Vector3(0, kLeanPivotDrop, 0);
    // Matrix rotation about Z takes up (0,1,0) to (-sin a, cos a): a negative angle leans right (+X).
    return Matrix4(MTranslation, pivot + Vector3(lean * kLeanShift, 0, 0)) * Matrix4(MRotationZ, -lean * kLeanAngle) *
           Matrix4(MTranslation, -pivot);
}

void Man::UpdateLeanInput()
{
    auto& input = InputSubsystem::Instance();
    const bool can = CanLean();
    input.SetLeanClaimsSharedKeys(can);

    const UserAction keys[2] = {UALeanLeft, UALeanRight};
    for (int side = 0; side < 2; ++side)
    {
        const bool down = can && input.GetAction(keys[side]) > 0.5f;
        if (!can && input.GetAction(keys[side]) > 0.5f)
            _leanBlocked[side] = true; // e.g. Q held to stand up from prone: not a lean once he is up
        if (_leanBlocked[side])
        {
            if (input.GetAction(keys[side]) <= 0.5f)
                _leanBlocked[side] = false;
            _leanHeld[side] = false;
            continue;
        }
        const int dir = side == 0 ? -1 : +1;
        if (down && !_leanHeld[side])
        {
            // A press always cancels a latched lean; a tap of the latched side just turns it off.
            _leanTapClears[side] = _leanLatched == dir;
            _leanLatched = 0;
            _leanPressTime[side] = Glob.uiTime;
        }
        else if (!down && _leanHeld[side] && Glob.uiTime < _leanPressTime[side] + kLeanTapTime && !_leanTapClears[side])
            _leanLatched = dir;
        _leanHeld[side] = down;
    }
    if (!can)
        _leanLatched = 0;

    const int held = (_leanHeld[1] ? 1 : 0) - (_leanHeld[0] ? 1 : 0);
    _leanWanted = (_leanHeld[0] || _leanHeld[1]) ? float(held) : float(_leanLatched);
}

void Man::UpdateLeanReach()
{
    _leanReach[0] = _leanReach[1] = 1.0f;
    if (!GLandscape || fabs(_leanWanted) < 0.01f)
        return;
    const int side = _leanWanted < 0 ? 0 : 1;
    _leanReach[side] = ProbeLeanReach(side);
}

float Man::ProbeLeanReach(int side, int* hitCount) const
{
    if (hitCount)
        *hitCount = 0;
    if (!GLandscape)
        return 1.0f;
    const float lean = side == 0 ? -1.0f : 1.0f;

    // The upright pose: points without the current aim/lean on top (as RecalcGunTransform takes them).
    bool& gunTransIdentRef = const_cast<Man*>(this)->_gunTransIdent;
    const bool gunTransIdent = gunTransIdentRef;
    gunTransIdentRef = true;
    const Vector3 gunAxis = GetWeaponCenter(0);
    const Vector3 eye = GetPilotPosition(CamInternal);
    gunTransIdentRef = gunTransIdent;
    const Matrix4 leanTrans = LeanTransform(gunAxis, lean);
    const Matrix4 frame = WorldTransform();

    // Sweep the eye and the lean-side shoulder from upright to full lean (a chord of the arc),
    // extended by a margin: the engine's segment test is a line, the margin stands for the
    // head's and shoulder's size. The reach is how much of the chord stays clear.
    const auto reach = [&](Vector3Par rest, float margin)
    {
        const Vector3 beg = frame.FastTransform(rest);
        Vector3 end = frame.FastTransform(leanTrans.FastTransform(rest));
        const float len = beg.Distance(end);
        if (len < 1e-3f)
            return 1.0f;
        end = beg + (end - beg) * ((len + margin) / len);
        // Both the solid (walk-into) geometry and the fire geometry: either alone misses walls on
        // some models (glass has no fire geometry, a plinth no fire geometry above it).
        CollisionBuffer hits;
        Man* self = const_cast<Man*>(this);
        GLandscape->ObjectCollision(hits, nullptr, self, beg, end, 0, ObjIntersectGeom);
        GLandscape->ObjectCollision(hits, nullptr, self, beg, end, 0, ObjIntersectFire);
        float entry = 1.0f;
        for (int i = 0; i < hits.Size(); ++i)
            if (hits[i].object && hits[i].object != this && std::isfinite(hits[i].under))
            {
                entry = std::min(entry, std::clamp(hits[i].under, 0.0f, 1.0f));
                if (hitCount)
                    ++*hitCount;
            }
        return std::clamp((entry * (len + margin) - margin) / len, 0.0f, 1.0f);
    };
    const Vector3 shoulder = eye + Vector3(lean * kLeanShoulderOut, -kLeanShoulderDrop, 0);
    return std::min(reach(eye, kLeanHeadRadius), reach(shoulder, kLeanShoulderRadius));
}

bool Man::MoveLean(float deltaT)
{
    float wanted = CanLean() ? _leanWanted : 0.0f;
    if (QIsManual() && wanted != 0.0f)
    {
        UpdateLeanReach();
        wanted = wanted < 0 ? -std::min(-wanted, _leanReach[0]) : std::min(wanted, _leanReach[1]);
    }
    float delta = wanted - _lean;
    if (fabs(delta) < 1e-5f)
        return false;
    // A wall pushes back at once; easing in and out is for the player's own moves.
    const float rate = fabs(wanted) < fabs(_lean) && wanted * _lean > 0 && fabs(_leanWanted) > fabs(wanted)
                           ? kLeanRate * 3
                           : kLeanRate;
    delta = std::clamp(delta, -rate * deltaT, rate * deltaT);
    _lean += delta;
    return true;
}

} // namespace Poseidon
