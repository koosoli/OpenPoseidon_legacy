// test_ballistics.cpp - drag-correct ballistic prediction.
//
// The thing under test is NOT "the integrator returns plausible numbers". It is the claim that
// justified porting this from the FP-GL fork at all: that the engine currently predicts a
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
#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp> // G_CONST

#include <cmath>

using Catch::Approx;
// Vector3 / VZero are global (Math3D.hpp macro-aliases them onto the active precision), not
// members of namespace Poseidon.
namespace B = Poseidon::Ballistics;

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
