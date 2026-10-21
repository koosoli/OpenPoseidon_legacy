// Jumping (owner, 2026-10-20: "i also want jumping build in"): the player's jump key (Space on foot) launches the man
// into the free fall the engine already simulates (Man::Simulate: gravity, air drag, ground and object contact), with
// his running speed kept and an upward speed added; out of a run he leaps, with a push forward along his heading. The landing is the falling code's (UpdateFalling,
// SoldierOldSwim.cpp): a soft touchdown goes straight back to the normal moves, a hard one crouches in FallLand.
//
// Sound: the push-off and the landing play the step sound of the ground (PlayJumpSound), the landing louder the faster
// he comes down. A soft landing bends the knees and dips the hips for a moment over whatever he does next (the landing
// layer, SoldierOldUpperBody.cpp: Quaternius' Jump_Land, the swim pack's JumpLand), so running he keeps running.
//
// The moves are the swim pack's (content/default-packs/swim): Jump, the take-off and rise, then JumpAir, held until
// he lands. Their legs and trunk are Quaternius' Jump_Start / Jump_Loop (Universal Animation Library, CC0), retargeted
// onto the MC rig with the rifle held as in Fall (apps/tools/BlenderAddon/io_import_p3d/retarget.py). Without those
// moves he still jumps, playing whatever he was playing.
//
// Standing or crouched, on the ground, not in water, not on a ladder or in a vehicle, not mid-reload-move, and not
// within JumpCooldown of the last landing. No climbing or vaulting: the jump only lifts him (~0.6 m) over what he runs
// at. AI does not jump.

#include <Poseidon/World/Entities/Infantry/SoldierOldCommon.hpp>

#include <Poseidon/AI/AIUnit.hpp>

#include <algorithm>
#include <cmath>

namespace Poseidon
{

namespace
{
constexpr float JumpUpSpeed = 3.4f;   // m/s up at the launch: a ~0.6 m rise, ~0.7 s in the air on the flat
constexpr float JumpMaxRun = 8.0f;    // m/s: the ground speed he carries into the air at most (with the leap)
constexpr float JumpLeapFrom = 2.5f;  // m/s: from this ground speed up a jump is a leap forward ...
constexpr float JumpLeapPush = 1.2f;  // m/s: ... with this much more speed along his heading (at twice it, all of it)
constexpr float JumpLiftTime = 0.12f; // s after the launch in which ground contact does not count yet
constexpr float JumpCooldown = 0.35f; // s after landing before the next jump
} // namespace

bool Man::CanJump() const
{
    if (_isDead || _jumping || _falling || _fallLanding || IsSwimming() || IsDown() || _ladderBuilding)
        return false;
    if (GetActiveCargoWeaponSeat() || GetHierachyParent())
        return false;
    const AIUnit* unit = Brain();
    if (unit && unit->GetVehicleIn())
        return false;
    if (!_landContact && !_objectContact)
        return false;
    if (_sinceJumpLanded < JumpCooldown)
        return false;
    // a move that plays an action (reloading, throwing, getting up) finishes first
    if (_primaryMove.context)
        return false;
    return true;
}

bool Man::TryJump()
{
    InitSwimMoves();
    if (!CanJump())
        return false;
    Vector3 run = Speed();
    run[1] = 0;
    // out of a run he leaps forward: a push along his heading, the more the faster he runs
    const float runSpeed = run.Size();
    if (runSpeed > JumpLeapFrom)
    {
        Vector3 ahead = Direction();
        ahead[1] = 0;
        if (ahead.SquareSize() > 1e-4f)
            run += ahead.Normalized() * (JumpLeapPush * std::min(1.0f, (runSpeed - JumpLeapFrom) / JumpLeapFrom));
    }
    if (run.SquareSize() > Square(JumpMaxRun))
        run = run * (JumpMaxRun / run.Size());
    _speed = run + Vector3(0, JumpUpSpeed, 0);
    _landContact = false;
    _objectContact = false;
    _freeFallUntil = Glob.time + JumpLiftTime;
    _lastMovementTime = Glob.time;
    _jumping = true;
    _jumpTime = 0;
    _airTime = 0;
    _fallSpeed = 0;
    if (_jumpMove != MoveIdNone)
        BlendToMove(_jumpMove);
    PlayJumpSound(0.8f);
    IsMoved();
    return true;
}

void Man::PlayJumpSound(float loudness)
{
    // the step sound of the ground he leaves or lands on (the soldier's SoundEnvironExt table), as a footstep is
    if (!GSoundScene || !GLandscape)
        return;
    const SoundPars& pars = Type()->GetEnvSoundExtRandom(RStringB(_surfaceSound));
    if (pars.name.GetLength() <= 0)
        return;
    Vector3 at = Position();
    at[1] = GLandscape->RoadSurfaceY(at + VUp * 0.5f);
    const float frequency = pars.freq * (0.85f + GRandGen.RandomValue() * 0.1f);
    GSoundScene->OpenAndPlayOnce(pars.name, at, Speed(), pars.vol * loudness, frequency);
}

bool Man::JumpLiftingOff() const
{
    return _jumping && _jumpTime < JumpLiftTime;
}

void Man::KeepJumpMoves()
{
    if (_jumpMove == MoveIdNone)
        return;
    const MoveId id = _primaryMove.id;
    if (id == _jumpMove)
    {
        if (_primaryTime >= 0.98f && _jumpAirMove != MoveIdNone)
            BlendToMove(_jumpAirMove);
    }
    else if (id != _jumpAirMove)
    {
        BlendToMove(_jumpAirMove != MoveIdNone ? _jumpAirMove : _jumpMove);
    }
}

} // namespace Poseidon
