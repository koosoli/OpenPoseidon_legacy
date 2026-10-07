// test_ballistics.cpp - drag-correct ballistic prediction.
//
// The thing under test is NOT "the integrator returns plausible numbers". It is the claim that
// justified writing this solver at all: that the engine currently predicts a
// projectile's path with a model that is not the model the projectile flies under, and that this
// solver closes that gap. So the tests are built around three independent anchors:
//
//   1. an ANALYTIC ground truth (drag off -> the vacuum parabola has a closed form),
//   2. STEP INDEPENDENCE (the answer must not be a property of the chosen step size),
//   3. a ROUND TRIP (fire along the solved direction and check the round actually arrives).
//
// Only the last of those could be satisfied by a solver that merely agrees with itself, and the
// round trip is checked against a separately-written integration, not against the solver's own
// internals.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Poseidon/World/Entities/Weapons/Ballistics.hpp>
#include <Poseidon/UI/InGame/CombatAssistMath.hpp>
#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp> // G_CONST

#include <cmath>
#include <limits>

TEST_CASE("Combat bearing agrees with cardinal directions and wraps north", "[combat-assists]")
{
    using Poseidon::CombatAssists::BearingDegrees;
    CHECK(BearingDegrees(0, 1) == 0);
    CHECK(BearingDegrees(1, 0) == 90);
    CHECK(BearingDegrees(0, -1) == 180);
    CHECK(BearingDegrees(-1, 0) == 270);
    CHECK(BearingDegrees(-0.001f, 1) == 0);
    CHECK(BearingDegrees(1, 1) == 45);
}

TEST_CASE("Grenade ground estimate follows an analytic parabola and hillside", "[combat-assists]")
{
    Poseidon::Ballistics::ShellParams shell;
    shell.timeToLive = 10;
    float range = -1;
    const Vector3 origin(0, 1, 0), velocity(50, 25, 0);
    for (float slope : {0.0f, 0.1f, -0.1f})
    {
        INFO("Ground slope " << slope);
        int queries = 0;
        const auto ground = [&](float x, float) { ++queries; return slope * x; };
        REQUIRE(Poseidon::CombatAssists::PredictGroundImpact(shell, origin, velocity, VZero, ground, range));
        const float relativeVy = 25 - slope * 50;
        const float time = (relativeVy + std::sqrt(relativeVy * relativeVy + 2 * G_CONST)) / G_CONST;
        CHECK(std::abs(range - 50 * time) < 1.5f);
        CHECK(queries <= 501);
    }
    shell.timeToLive = 0.1f;
    CHECK_FALSE(Poseidon::CombatAssists::PredictGroundImpact(shell, origin, velocity, VZero,
        [](float, float) { return 0.0f; }, range));
    shell.timeToLive = 10;
    CHECK_FALSE(Poseidon::CombatAssists::PredictGroundImpact(shell, origin, velocity, VZero,
        [](float, float) { return std::numeric_limits<float>::quiet_NaN(); }, range));
}

using Catch::Approx;
// Vector3 / VZero are global (Math3D.hpp macro-aliases them onto the active precision), not
// members of namespace Poseidon.
namespace B = Poseidon::Ballistics;

TEST_CASE("Carrier velocity includes rotation at the muzzle", "[ballistics][cargo-weapons]")
{
    const Vector3 linear(8, 0, 3);
    const Vector3 angular(0, .5f, 0);
    const Vector3 offset(2, 0, 1);
    const Vector3 velocity = B::CarrierPointVelocity(linear, angular, offset);
    CHECK(velocity.X() == Approx(8.5f));
    CHECK(velocity.Y() == Approx(0));
    CHECK(velocity.Z() == Approx(2));
    CHECK((B::CarrierPointVelocity(linear, VZero, offset) - linear).Size() == Approx(0));
    CHECK((B::CarrierPointVelocity(linear, angular, VZero) - linear).Size() == Approx(0));
    const Matrix3 turn(MRotationZ, .7f);
    CHECK((B::CarrierPointVelocity(turn * linear, turn * angular, turn * offset) -
           turn * velocity).Size() < .00001f);
}

TEST_CASE("Cargo aiming compensates actual launch velocity without a walking delay", "[ballistics][cargo-weapons]")
{
    const Vector3 muzzle(100, 2, 200);
    const Vector3 linear(0, 0, 14);
    const Vector3 angular(0, .4f, 0);
    const Vector3 offset(1.3f, 1, .6f);
    const Vector3 inherited = B::CarrierPointVelocity(linear, angular, offset);
    const float flightTime = .1f;
    const Vector3 target(180, 2, 200);
    const Vector3 aimedDisplacement = target - muzzle - inherited * flightTime;
    // Independent rigid-body component calculation, then the actual flight.
    const Vector3 launchVelocity(angular.Y() * offset.Z(), 0,
                                 linear.Z() - angular.Y() * offset.X());
    const Vector3 hit = muzzle + aimedDisplacement + launchVelocity * flightTime;
    CHECK((hit - target).Size() < .0001f);
    CHECK((hit - inherited * .2f - target).Size() > 2.5f);
    CHECK((muzzle + (target - muzzle) + launchVelocity * flightTime - target).Size() > 1.3f);
}

namespace
{

/// A 7.62-class rifle round: the family this matters most for, because it is the one the AI
/// shoots at 300-800 m where a vacuum parabola visibly under-elevates.
B::ShellParams Rifle()
{
    B::ShellParams p;
    p.airFriction = -0.001f; // deceleration coefficient, negative as the shipped data has it
    p.coefGravity = 1.0f;
    p.initTime = 0.0f;
    p.timeToLive = 10.0f;
    return p;
}

/// Independent re-implementation of the flight rule in ShotShell::Simulate, written out here on
/// purpose. The round-trip tests below check the solver against THIS, not against the solver's own
/// SimulateAgainstTarget -- otherwise they would only prove the code agrees with itself. If someone
/// changes the engine's integration and updates only Ballistics.cpp, this copy stops agreeing and
/// these tests fail, which is exactly the alarm we want.
float IndependentMiss(const B::ShellParams& shell, Vector3 pos, Vector3 speed, Vector3 targetPos, Vector3 targetSpeed)
{
    constexpr float step = 1.0f / 1000.0f; // finer than the solver's, deliberately
    float t = 0.0f;
    float best = (pos - targetPos).Size();
    while (t < shell.timeToLive && t < 10.0f)
    {
        Vector3 accel = speed * (speed.Size() * shell.airFriction);
        accel[1] -= shell.coefGravity * G_CONST;
        pos += speed * step;
        speed += accel * step;
        t += step;

        const float d = (pos - (targetPos + targetSpeed * t)).Size();
        if (d < best)
        {
            best = d;
        }
        else if (d > best * 4.0f && best < 1e4f)
        {
            break; // past closest approach and receding
        }
    }
    return best;
}

} // namespace

TEST_CASE("With drag off the integrator reproduces the vacuum parabola", "[ballistics][physics]")
{
    B::ShellParams shell = Rifle();
    shell.airFriction = 0.0f;

    const float v0 = 700.0f;
    const Vector3 start(0, 0, 0);
    const Vector3 velocity(0, 0, v0); // straight down +Z, dead level

    const float distance = 400.0f;
    const B::Impact hit = B::SimulateAtDistance(shell, start, velocity, distance);

    REQUIRE(hit.reached);

    // Closed form: horizontal speed is unchanged without drag, so t = d/v0 and the drop is
    // g*t^2/2. The distance is measured on the SPHERE of radius `distance`, so the horizontal
    // run is very slightly less than `distance` once the round has fallen -- at 400 m the drop is
    // ~1.6 m and the difference is millimetres, comfortably inside the tolerance.
    const float expectedTime = distance / v0;
    const float expectedDrop = 0.5f * G_CONST * expectedTime * expectedTime;

    CHECK(hit.time == Approx(expectedTime).epsilon(0.01));
    CHECK(-hit.position[1] == Approx(expectedDrop).epsilon(0.02));
}

TEST_CASE("The prediction does not depend on the integration step", "[ballistics][physics]")
{
    const B::ShellParams shell = Rifle();
    const Vector3 start(0, 0, 0);
    const Vector3 velocity(0, 0, 750.0f);

    const B::Impact coarse = B::SimulateAtDistance(shell, start, velocity, 800.0f, B::DefaultStep);
    const B::Impact mid = B::SimulateAtDistance(shell, start, velocity, 800.0f, B::DefaultStep / 4.0f);
    const B::Impact fine = B::SimulateAtDistance(shell, start, velocity, 800.0f, B::DefaultStep / 16.0f);

    REQUIRE(coarse.reached);
    REQUIRE(mid.reached);
    REQUIRE(fine.reached);

    // The real property worth asserting is CONVERGENCE, not a magic tolerance: refining the step
    // must move the answer less each time. Forward Euler's error is linear in the step, so a 4x
    // refinement should cut the remaining error by roughly 4x.
    const float coarseErr = std::fabs(coarse.position[1] - fine.position[1]);
    const float midErr = std::fabs(mid.position[1] - fine.position[1]);
    CHECK(midErr < coarseErr);

    // And the absolute bar, stated as a physical criterion rather than a tuned number: 2 cm of
    // drop at 800 m. Weapon dispersion at that range is metres, so a step-induced difference two
    // orders of magnitude below it cannot change an aiming decision. This started at 1/200 s,
    // which missed this bar at 2.6 cm -- see DefaultStep for why the step moved, not the margin.
    CHECK(coarse.position[1] == Approx(fine.position[1]).margin(0.02));
    CHECK(coarse.time == Approx(fine.time).margin(0.002));
}

TEST_CASE("A round fired along the solved direction reaches a static target", "[ballistics][aim]")
{
    const B::ShellParams shell = Rifle();
    const float v0 = 750.0f;

    const Vector3 shooter(0, 1.5f, 0);
    const Vector3 target(0, 1.5f, 600.0f);

    const Vector3 dir = B::CompensateAim(shell, v0, VZero, shooter, target, VZero);
    REQUIRE(dir.Size() == Approx(1.0f).epsilon(1e-4));

    // Fly it with the independently-written integrator above and measure the miss at the target's
    // range. 25 cm at 600 m is well inside any weapon's dispersion; the point is that the error
    // is small and centred, not that it is zero.
    const float miss = IndependentMiss(shell, shooter, dir * v0, target, VZero);
    CHECK(miss < 0.25f);
}

TEST_CASE("The solver leads a crossing target", "[ballistics][aim]")
{
    const B::ShellParams shell = Rifle();
    const float v0 = 750.0f;

    const Vector3 shooter(0, 1.5f, 0);
    const Vector3 target(0, 1.5f, 500.0f);
    const Vector3 targetSpeed(8.0f, 0, 0); // 8 m/s across, a running man

    const Vector3 lead = B::CompensateAim(shell, v0, VZero, shooter, target, targetSpeed);
    const Vector3 noLead = B::CompensateAim(shell, v0, VZero, shooter, target, VZero);

    // The lead solution must actually aim off to the side, and by roughly speed * flight time.
    CHECK(lead[0] > noLead[0]);

    const float leadMiss = IndependentMiss(shell, shooter, lead * v0, target, targetSpeed);
    CHECK(leadMiss < 0.4f);

    // And the naive solution must MISS the moving target, or the test above proves nothing.
    const float naiveMiss = IndependentMiss(shell, shooter, noLead * v0, target, targetSpeed);
    CHECK(naiveMiss > 3.0f);
}

TEST_CASE("The engine's vacuum aim under-elevates at range, and the solver fixes it",
          "[ballistics][aim][regression]")
{
    // This is the test that states the defect. CompensateAimVacuum is the arithmetic every AI
    // aiming site uses today (SoldierOldAI.cpp:446); CompensateAim is the ported solver. Fire both
    // at the same 800 m target and measure where each round actually goes under the engine's own
    // drag model.
    const B::ShellParams shell = Rifle();
    const float v0 = 750.0f;

    const Vector3 shooter(0, 1.5f, 0);
    const Vector3 target(0, 1.5f, 800.0f);

    const Vector3 vacuumDir = B::CompensateAimVacuum(v0, shooter, target);
    const Vector3 solvedDir = B::CompensateAim(shell, v0, VZero, shooter, target, VZero);

    const float vacuumMiss = IndependentMiss(shell, shooter, vacuumDir * v0, target, VZero);
    const float solvedMiss = IndependentMiss(shell, shooter, solvedDir * v0, target, VZero);
    const B::Approach vacuumShot = B::SimulateAgainstTarget(shell, shooter, vacuumDir * v0, target, VZero);

    // The drag-free aim misses by metres: it assumes the round still carries v0 at 800 m, so it
    // budgets far too little flight time and therefore far too little drop.
    CHECK(vacuumMiss > 1.0f);
    // The solver is inside a handspan.
    CHECK(solvedMiss < 0.4f);
    // And it misses LOW, which is the direction that matters -- an AI that under-elevates shoots
    // into the dirt in front of its target rather than over its head.
    CHECK(vacuumShot.position[1] < target[1]);
}

TEST_CASE("A round that expires before the distance reports that it did not reach", "[ballistics]")
{
    B::ShellParams shell = Rifle();
    shell.timeToLive = 0.2f; // expires after ~150 m at 750 m/s

    const B::Impact hit = B::SimulateAtDistance(shell, Vector3(0, 0, 0), Vector3(0, 0, 750.0f), 2000.0f);
    CHECK_FALSE(hit.reached);
}

TEST_CASE("SolveLevelShot is what the two aiming call sites consume", "[ballistics][aim]")
{
    // Man::AdjustWeapon (player sight zeroing) wants drop/distance; Man::CalculateAimWeapon
    // (infantry AI) wants the absolute drop and the flight time. Both come from this one solve,
    // which is why they can no longer disagree with each other.
    const B::ShellParams shell = Rifle();
    const float v0 = 750.0f;

    SECTION("with drag off it is the vacuum parabola")
    {
        B::ShellParams noDrag = shell;
        noDrag.airFriction = 0.0f;

        const float d = 400.0f;
        const B::LevelShot shot = B::SolveLevelShot(noDrag, v0, d);
        REQUIRE(shot.valid);

        const float t = d / v0;
        CHECK(shot.time == Approx(t).epsilon(0.01));
        CHECK(shot.drop == Approx(0.5f * G_CONST * t * t).epsilon(0.02));
    }

    SECTION("with drag the round is slower, so it is in the air longer and falls further")
    {
        const float d = 600.0f;
        const B::LevelShot dragged = B::SolveLevelShot(shell, v0, d);
        REQUIRE(dragged.valid);

        const float vacuumTime = d / v0;
        const float vacuumDrop = 0.5f * G_CONST * vacuumTime * vacuumTime;

        // Both directions matter. Longer flight is WHY the drop is larger, and it is also why the
        // old code under-led moving targets: it scaled lead by the vacuum time.
        CHECK(dragged.time > vacuumTime);
        CHECK(dragged.drop > vacuumDrop);
    }

    SECTION("a range the round cannot reach reports invalid rather than a wrong number")
    {
        B::ShellParams shortLived = shell;
        shortLived.timeToLive = 0.1f;
        const B::LevelShot shot = B::SolveLevelShot(shortLived, v0, 3000.0f);
        CHECK_FALSE(shot.valid);
        // The call sites fall back to their original closed form when this happens, so an
        // unreachable range must never silently produce drop == 0.
    }
}

namespace
{
/// A LAW/RPG-class unguided rocket: slow off the tube, a short hard burn, strong side drag.
B::MissileParams Rocket()
{
    B::MissileParams p;
    p.initTime = 0.0f;
    p.thrustTime = 0.4f;
    p.thrust = 300.0f;
    p.sideAirFriction = 1.0f;
    p.timeToLive = 20.0f;
    return p;
}

Matrix3 Attitude(Vector3 dir)
{
    return Matrix3(MDirection, dir.Normalized(), VUp);
}
} // namespace

TEST_CASE("The rocket motor accelerates the round along its own axis", "[ballistics][missile]")
{
    const B::MissileParams rocket = Rocket();
    const Vector3 start(0, 0, 0);
    const Vector3 dir(0, 0, 1);
    const float launchSpeed = 45.0f;

    const B::Impact hit = B::SimulateUnguidedMissileAtDistance(rocket, start, Attitude(dir), dir * launchSpeed, 300.0f);
    REQUIRE(hit.reached);

    // If the round merely coasted at its launch speed it would need 300/45 = 6.7 s. The motor
    // must have made it appreciably quicker; this is the test that would fail if thrust were
    // dropped or applied in world space instead of along the body axis.
    CHECK(hit.time < 4.0f);
}

TEST_CASE("Killing the motor makes the rocket slower and lower", "[ballistics][missile]")
{
    const Vector3 start(0, 0, 0);
    const Vector3 dir(0, 0, 1);
    const float launchSpeed = 45.0f;

    B::MissileParams powered = Rocket();
    B::MissileParams coasting = Rocket();
    coasting.thrust = 0.0f;

    // 180 m, not 250: an unpowered rocket at 45 m/s does not reach 250 m at all -- it bleeds speed
    // and expires short. That is the model behaving, not a bug, and it is why the range here is
    // inside what a coasting round can actually do.
    const B::Impact a = B::SimulateUnguidedMissileAtDistance(powered, start, Attitude(dir), dir * launchSpeed, 180.0f);
    const B::Impact b = B::SimulateUnguidedMissileAtDistance(coasting, start, Attitude(dir), dir * launchSpeed, 180.0f);
    REQUIRE(a.reached);
    REQUIRE(b.reached);

    CHECK(b.time > a.time);           // longer in the air
    CHECK(b.position[1] < a.position[1]); // and therefore further below the line of departure
}

TEST_CASE("A rocket droops far less than a shell of the same speed", "[ballistics][missile]")
{
    // The point of having a separate rocket model at all. Its body drag resists vertical motion an
    // order of magnitude harder than forward motion (the 10x terms in the X/Y polynomial), so
    // gravity is fought in a way a shell never fights it. Solving a rocket with the shell solver
    // would over-elevate it badly, and this is the test that pins that difference.
    const Vector3 start(0, 0, 0);
    const Vector3 dir(0, 0, 1);
    const float launchSpeed = 45.0f;
    const float range = 200.0f;

    B::MissileParams coasting = Rocket();
    coasting.thrust = 0.0f;

    B::ShellParams asShell;
    asShell.airFriction = 0.0f; // give the shell the BEST case: no drag at all
    asShell.coefGravity = 1.0f;
    asShell.timeToLive = 20.0f;

    const B::Impact rocketHit =
        B::SimulateUnguidedMissileAtDistance(coasting, start, Attitude(dir), dir * launchSpeed, range);
    const B::Impact shellHit = B::SimulateAtDistance(asShell, start, dir * launchSpeed, range);

    REQUIRE(rocketHit.reached);
    REQUIRE(shellHit.reached);
    CHECK(-rocketHit.position[1] < -shellHit.position[1]);
}

TEST_CASE("A rocket fired along the solved direction reaches its target", "[ballistics][missile][aim]")
{
    const B::MissileParams rocket = Rocket();
    const float launchSpeed = 45.0f;

    const Vector3 shooter(0, 1.5f, 0);
    const Vector3 target(0, 1.5f, 250.0f);

    const Vector3 dir =
        B::CompensateUnguidedMissile(rocket, launchSpeed, VUp, VZero, shooter, target, VZero);
    REQUIRE(dir.Size() == Approx(1.0f).epsilon(1e-4));

    const B::Approach check = B::SimulateUnguidedMissileAgainstTarget(rocket, shooter, Attitude(dir),
                                                                     dir * launchSpeed, target, VZero);
    CHECK(check.error < 1.0f);

    // And it must actually elevate rather than fire flat, or the solve did nothing.
    CHECK(dir[1] > 0.0f);
}

TEST_CASE("The derived per-calibre coefficients order the way physics requires",
          "[ballistics][bal-010]")
{
    // The values in docs/ballistics/cwa-airfriction.cpp are the whole point of making airFriction
    // config-driven: one shared constant made a 5.56 and a .50 decelerate identically.
    //
    // The property to pin is the COEFFICIENT ordering, and it must be isolated from muzzle
    // velocity to mean anything. The first version of this test compared drops at each round's own
    // v0 and failed -- correctly. A 7.62 at 838 m/s drops MORE at 300 m than a 5.56 at 940 m/s
    // (0.707 m against 0.610 m) despite being the slicker projectile, simply because it starts
    // slower and is therefore in the air longer. Absolute drop is not a statement about drag.
    auto shellWith = [](float k)
    {
        B::ShellParams p;
        p.airFriction = k;
        p.coefGravity = 1.0f;
        p.timeToLive = 10.0f;
        return p;
    };

    const float range = 300.0f;
    const float sameV0 = 900.0f; // one muzzle velocity, so only the coefficient varies

    const B::LevelShot m855 = B::SolveLevelShot(shellWith(-0.000983f), sameV0, range);  // 5.56
    const B::LevelShot m118 = B::SolveLevelShot(shellWith(-0.000600f), sameV0, range);  // 7.62
    const B::LevelShot m33 = B::SolveLevelShot(shellWith(-0.000314f), sameV0, range);   // 12.7
    const B::LevelShot sabot = B::SolveLevelShot(shellWith(-0.000141f), sameV0, range); // APFSDS

    REQUIRE(m855.valid);
    REQUIRE(m118.valid);
    REQUIRE(m33.valid);
    REQUIRE(sabot.valid);

    // Less drag => less time to the target => less drop. Strictly monotone across the set.
    CHECK(sabot.time < m33.time);
    CHECK(m33.time < m118.time);
    CHECK(m118.time < m855.time);

    CHECK(sabot.drop < m33.drop);
    CHECK(m33.drop < m118.drop);
    CHECK(m118.drop < m855.drop);

    // The old universal constant sat closest to the 7.62 -- which is why it was never obviously
    // wrong, and was wrong for everything else.
    const B::LevelShot legacy = B::SolveLevelShot(shellWith(B::DefaultAirFriction), sameV0, range);
    REQUIRE(legacy.valid);
    CHECK(std::fabs(legacy.drop - m118.drop) < std::fabs(legacy.drop - m855.drop));

    // And the separate, real fact the failed first version stumbled on: at their OWN muzzle
    // velocities the drop ordering between 5.56 and 7.62 inverts. Worth keeping, because anyone
    // sanity-checking these numbers against a range card will meet it.
    const B::LevelShot m855Real = B::SolveLevelShot(shellWith(-0.000983f), 940.0f, range);
    const B::LevelShot m118Real = B::SolveLevelShot(shellWith(-0.000600f), 838.0f, range);
    REQUIRE(m855Real.valid);
    REQUIRE(m118Real.valid);
    CHECK(m118Real.drop > m855Real.drop);
}

// --- Mach-dependent drag (BAL-010) -----------------------------------------------------------

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
/// Read a drag table back out of docs/ballistics/drag-functions/. Located relative to THIS file,
/// because the suite has no repo-root helper. Returns empty if the file is not reachable, and the
/// test skips rather than failing -- a checkout without docs/ is unusual but not broken.
std::vector<std::pair<float, float>> ReadDragTable(const char* name)
{
    std::string here = __FILE__;
    for (char& c : here)
    {
        if (c == '\\')
        {
            c = '/';
        }
    }
    // .../tests/unit/engine/Poseidon/World/Weapons/test_ballistics.cpp -> repo root is 7 up.
    for (int i = 0; i < 7; i++)
    {
        const size_t slash = here.find_last_of('/');
        if (slash == std::string::npos)
        {
            return {};
        }
        here.resize(slash);
    }
    const std::string path = here + "/docs/ballistics/drag-functions/" + name + ".txt";

    std::vector<std::pair<float, float>> rows;
    if (FILE* f = std::fopen(path.c_str(), "rb"))
    {
        char line[256];
        while (std::fgets(line, sizeof(line), f))
        {
            if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            {
                continue;
            }
            float mach = 0.0f;
            float cd = 0.0f;
            if (std::sscanf(line, "%f %f", &mach, &cd) == 2)
            {
                rows.emplace_back(mach, cd);
            }
        }
        std::fclose(f);
    }
    return rows;
}
} // namespace

TEST_CASE("The embedded drag tables still match their source files", "[ballistics][bal-010]")
{
    // A physics table that has drifted from the source it cites is indistinguishable from one that
    // was invented. This is the guard against that, and it is the reason the .txt files are kept
    // in the repo rather than only quoted in a comment.
    for (const char* name : {"G1", "G7"})
    {
        const auto rows = ReadDragTable(name);
        if (rows.empty())
        {
            WARN(std::string("drag table not reachable from __FILE__, skipping: ") + name);
            continue;
        }
        const B::DragModel model = std::strcmp(name, "G1") == 0 ? B::DragModel::G1 : B::DragModel::G7;
        for (const auto& row : rows)
        {
            // Exact tabulated Mach values must return exactly the tabulated Cd.
            CHECK(B::StandardDragCoefficient(model, row.first) == Approx(row.second).epsilon(1e-5));
        }
    }
}

TEST_CASE("The drag functions have the shape exterior ballistics says they do", "[ballistics][bal-010]")
{
    // Not decoration: these are the properties that separate a real drag function from a plausible
    // curve, and they are what a transcription error would break.

    // Both rise sharply through transonic and peak just past Mach 1 -- that IS the transonic wall.
    CHECK(B::StandardDragCoefficient(B::DragModel::G7, 1.05f) > B::StandardDragCoefficient(B::DragModel::G7, 0.8f) * 3.0f);
    CHECK(B::StandardDragCoefficient(B::DragModel::G1, 1.40f) > B::StandardDragCoefficient(B::DragModel::G1, 0.6f) * 3.0f);

    // G7's standard shape is a boat-tail spitzer, so it is far slicker than G1 everywhere.
    for (float mach : {0.5f, 1.0f, 1.5f, 2.0f, 3.0f})
    {
        CHECK(B::StandardDragCoefficient(B::DragModel::G7, mach) < B::StandardDragCoefficient(B::DragModel::G1, mach));
    }

    // Clamped, not extrapolated, outside the tabulated range.
    CHECK(B::StandardDragCoefficient(B::DragModel::G7, -1.0f) == Approx(B::StandardDragCoefficient(B::DragModel::G7, 0.0f)));
    CHECK(B::StandardDragCoefficient(B::DragModel::G7, 99.0f) == Approx(B::StandardDragCoefficient(B::DragModel::G7, 5.0f)));
}

TEST_CASE("Ammo that declares no drag function flies exactly as before", "[ballistics][bal-010]")
{
    // The safety property of the whole change: classic content declares no dragModel, so it must
    // reach the identical constant law it always used.
    const float k = -0.0005f;
    CHECK(B::DragRetardation(B::DragModel::Constant, k, 0.0f, 800.0f) == Approx(k));
    // A drag function without a coefficient describes nothing and must fall back, not divide by zero.
    CHECK(B::DragRetardation(B::DragModel::G7, k, 0.0f, 800.0f) == Approx(k));
    CHECK(B::DragRetardation(B::DragModel::G1, k, -3.0f, 800.0f) == Approx(k));
}

TEST_CASE("G1 and G7 with their published coefficients agree on the same bullet",
          "[ballistics][bal-010][regression]")
{
    // The cross-validation that established the BC<->Cd normalisation, kept as a test so it cannot
    // silently break. M855 from 940 m/s: G1 at BC 0.304 and G7 at BC 0.151 are two entirely
    // different tables and two different coefficients, and they must describe the same flight.
    // A systematic error in DragRetardation would move both; only a correct normalisation makes
    // independent pairs land together.
    auto shellWith = [](B::DragModel model, float bcLbIn2)
    {
        B::ShellParams p;
        p.airFriction = B::DefaultAirFriction;
        p.coefGravity = 1.0f;
        p.timeToLive = 10.0f;
        p.dragModel = model;
        p.ballisticCoefficient = bcLbIn2 * B::BallisticCoefficientToSI;
        return p;
    };

    const Vector3 start(0, 0, 0);
    const Vector3 velocity(0, 0, 940.0f);

    const B::Impact g1 = B::SimulateAtDistance(shellWith(B::DragModel::G1, 0.304f), start, velocity, 300.0f);
    const B::Impact g7 = B::SimulateAtDistance(shellWith(B::DragModel::G7, 0.151f), start, velocity, 300.0f);
    REQUIRE(g1.reached);
    REQUIRE(g7.reached);

    // Flight time to 300 m within 1 %.
    CHECK(g1.time == Approx(g7.time).epsilon(0.01));

    // And both must be markedly draggier than the engine's old universal constant, which is the
    // finding that made the hand-fitted airFriction values provisional.
    B::ShellParams legacy;
    legacy.airFriction = B::DefaultAirFriction;
    legacy.coefGravity = 1.0f;
    legacy.timeToLive = 10.0f;
    const B::Impact old = B::SimulateAtDistance(legacy, start, velocity, 300.0f);
    REQUIRE(old.reached);
    CHECK(g7.time > old.time);
}

// =================================================================================================
// EXTERNAL REFERENCE
//
// Everything above this line checks the solver against the engine, against a hand-written second
// integration, or against itself. That can only catch inconsistency -- it cannot catch a model
// that is coherently wrong, which is precisely the failure the first pass of the airFriction data
// had: self-consistent, fitted to a remembered anchor, and systematically too flat.
//
// So these cases pin our numbers to an OUTSIDE implementation: gehtsoft-usa/BallisticCalculator1,
// a JBM/McCoy-derived ballistic calculator, checked 2026-08-27. Two things were compared and are
// held here:
//
//   * its retardation constant, which it inherits from JBM in imperial units, against ours,
//     derived independently from the physics in SI; and
//   * trajectories it and we produce for the same cartridge with the same published coefficient.
//
// docs/ballistics/drag-functions/SOURCE.md has the full comparison and the unit conversion.
// =================================================================================================

namespace
{
/// The reference calculator's drag normalisation, converted to SI.
///
/// It folds its constants into `PIR = (pi/8) * (rho0/144) = 2.08551e-4` and computes
/// `retardation[fps/s] = PIR * Cd * v_fps^2 / BC` with BC in lb/in^2. Converting to metres:
/// `* 0.3048` for the acceleration and `/ 0.09290304` for v^2, giving 6.842224e-4. Ours is
/// `rho * pi / (8 * BC_SI)` with BC_SI = BC * 703.06958, i.e. 6.842230e-4 -- the same number, from
/// a different starting point and a different unit system.
constexpr float ReferencePirSI = 2.08551e-04f * 0.3048f / 0.09290304f;

B::ShellParams WithDragFunction(B::DragModel model, float bcLbIn2)
{
    B::ShellParams p;
    p.airFriction = B::DefaultAirFriction;
    p.coefGravity = 1.0f;
    p.initTime = 0.0f;
    p.timeToLive = 10.0f;
    p.dragModel = model;
    p.ballisticCoefficient = bcLbIn2 * B::BallisticCoefficientToSI;
    return p;
}

B::ShellParams WithConstant(float airFriction)
{
    B::ShellParams p;
    p.airFriction = airFriction;
    p.coefGravity = 1.0f;
    p.initTime = 0.0f;
    p.timeToLive = 10.0f;
    return p;
}
} // namespace

TEST_CASE("The retardation constant matches an independent implementation", "[ballistics][bal-010][reference]")
{
    // A ballistic coefficient is only meaningful against the constant it is divided by, so this is
    // the most load-bearing number in the file: get it wrong and every published BC in the world
    // describes a different bullet than the one that flies. Two derivations, six figures.
    const float bcSI = 1.0f * B::BallisticCoefficientToSI; // BC of 1 lb/in^2, so k == the constant

    for (const B::DragModel model : {B::DragModel::G1, B::DragModel::G7})
    {
        for (const float mach : {0.5f, 1.0f, 1.5f, 2.0f, 3.0f})
        {
            const float cd = B::StandardDragCoefficient(model, mach);
            const float ours = B::DragRetardation(model, B::DefaultAirFriction, bcSI, mach * B::SpeedOfSound);
            CHECK(ours == Approx(-ReferencePirSI * cd).epsilon(1e-4));
        }
    }

    // And the atmosphere those constants describe is sea-level ICAO, which is what the reference
    // normalises to as well: its rho0 is 0.076474 lb/ft^3 = 1.2250 kg/m^3.
    CHECK(B::AirDensity == Approx(1.225f));
    CHECK(B::SpeedOfSound == Approx(340.3f).epsilon(1e-3));
}

TEST_CASE("Drag-function trajectories match the external reference model", "[ballistics][bal-010][reference]")
{
    // Drop and flight time at 300 m and 600 m for the four cartridges the shipped overlay
    // (docs/ballistics/cwa-airfriction.cpp) puts on a drag function, each with its published
    // coefficient. Expected values come from the reference model integrated finely; the tolerance
    // is 2 %, far wider than the difference between its integrator and ours (well under 1 %) and
    // far tighter than any error that would matter -- a broken normalisation moves these by tens of
    // per cent, and the first-pass constant fit missed the 600 m drop by 14 %.
    struct Case
    {
        const char* name;
        B::DragModel model;
        float bc;
        float muzzle;
        float drop300;
        float time300;
        float drop600;
        float time600;
    };
    const Case cases[] = {
        // 5.56x45 M855, and the same bullet described by the other table -- both must land here.
        {"M855 G7", B::DragModel::G7, 0.151f, 940.0f, 0.6435f, 0.3860f, 3.5896f, 0.9757f},
        {"M855 G1", B::DragModel::G1, 0.304f, 940.0f, 0.6432f, 0.3857f, 3.5634f, 0.9680f},
        {"7.62x51 M80", B::DragModel::G1, 0.397f, 810.0f, 0.8222f, 0.4310f, 4.2155f, 1.0266f},
        {"7.62x54R ball", B::DragModel::G1, 0.426f, 830.0f, 0.7695f, 0.4153f, 3.8535f, 0.9740f},
        {"12.7x99 M2", B::DragModel::G1, 0.670f, 890.0f, 0.6204f, 0.3665f, 2.8184f, 0.8029f},
    };

    const Vector3 start(0, 0, 0);
    for (const Case& c : cases)
    {
        INFO(c.name);
        const B::ShellParams shell = WithDragFunction(c.model, c.bc);
        const Vector3 velocity(0, 0, c.muzzle);

        const B::Impact at300 = B::SimulateAtDistance(shell, start, velocity, 300.0f);
        const B::Impact at600 = B::SimulateAtDistance(shell, start, velocity, 600.0f);
        REQUIRE(at300.reached);
        REQUIRE(at600.reached);

        CHECK(-at300.position[1] == Approx(c.drop300).epsilon(0.02));
        CHECK(at300.time == Approx(c.time300).epsilon(0.02));
        CHECK(-at600.position[1] == Approx(c.drop600).epsilon(0.02));
        CHECK(at600.time == Approx(c.time600).epsilon(0.02));
    }
}

TEST_CASE("The shipped constant fallbacks track their drag functions", "[ballistics][bal-010][reference]")
{
    // Every drag-function class in the overlay also carries an airFriction, for a build that
    // ignores dragModel. Those constants are least-squares fits to the drag-function trajectory
    // over 0-600 m, and this is what stops them being re-fitted to a guess again: each must stay
    // close to the function it stands in for, and each must be markedly draggier than the
    // first-pass value it replaced.
    struct Case
    {
        const char* name;
        B::DragModel model;
        float bc;
        float muzzle;
        float fallback;
        float firstPass;
    };
    const Case cases[] = {
        {"BulletSingle 5.56", B::DragModel::G7, 0.151f, 940.0f, -0.001328f, -0.000983f},
        {"BulletSniper 7.62x51", B::DragModel::G1, 0.397f, 810.0f, -0.001039f, -0.000600f},
        {"Bullet7_6 7.62x54R", B::DragModel::G1, 0.426f, 830.0f, -0.000954f, -0.000616f},
        {"Bullet12_7 12.7x99", B::DragModel::G1, 0.670f, 890.0f, -0.000570f, -0.000314f},
    };

    const Vector3 start(0, 0, 0);
    for (const Case& c : cases)
    {
        INFO(c.name);
        const Vector3 velocity(0, 0, c.muzzle);

        const B::Impact fn = B::SimulateAtDistance(WithDragFunction(c.model, c.bc), start, velocity, 300.0f);
        const B::Impact fallback = B::SimulateAtDistance(WithConstant(c.fallback), start, velocity, 300.0f);
        const B::Impact firstPass = B::SimulateAtDistance(WithConstant(c.firstPass), start, velocity, 300.0f);
        REQUIRE(fn.reached);
        REQUIRE(fallback.reached);
        REQUIRE(firstPass.reached);

        // Within 5 % of the real model at 300 m. A constant law cannot do better than that across
        // the whole band -- it has no transonic rise to give -- which is why it is the fallback.
        CHECK(-fallback.position[1] == Approx(-fn.position[1]).epsilon(0.05));

        // The first-pass value was too flat, by more than the fit error above. If a change ever
        // makes these two swap places, the anchor has drifted back towards a guess.
        CHECK(-firstPass.position[1] < -fn.position[1]);
        CHECK(-fallback.position[1] > -firstPass.position[1]);
        CHECK(c.fallback < c.firstPass); // more negative == more drag
    }
}
