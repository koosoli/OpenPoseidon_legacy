#include <catch2/catch_test_macros.hpp>
#include <Poseidon/AI/FriendlyFire.hpp>
#include <Poseidon/AI/InfantryCombat.hpp>
using namespace Poseidon;

namespace
{
bool Safe(Vector3 body, Vector3 velocity = Vector3(0, 0, 600), float spread = 0)
{
    Ballistics::ShellParams shell;
    shell.timeToLive = 2;
    return InfantryCombat::BulletPathSafe(Vector3(0, 10, 0), velocity, shell, spread, VZero,
        [&](Vector3Par from, Vector3Par to, float, float envelope) {
            return InfantryCombat::IntersectsBody(from, to, body, 0.5f + envelope)
                ? InfantryCombat::PathDecision::Block : InfantryCombat::PathDecision::Continue;
        });
}
}
TEST_CASE("friendly fire checks residual travel past AI engagement range", "[combat]")
{
    CHECK_FALSE(Safe(Vector3(0, 9, 300)));
    // At one second the real path drops almost five metres below the barrel ray.
    CHECK_FALSE(Safe(Vector3(0, 5.2f, 600)));
    CHECK(Safe(Vector3(10, 5.2f, 600)));
    CHECK(Safe(Vector3(0, 10, -10)));
}
TEST_CASE("friendly fire includes shooter velocity and dispersion", "[combat]")
{
    CHECK_FALSE(Safe(Vector3(6, 5.2f, 600), Vector3(6, 0, 600)));
    CHECK(Safe(Vector3(6, 5.2f, 600)));
    CHECK_FALSE(Safe(Vector3(2, 5.2f, 600), Vector3(0, 0, 600), 2));
}
TEST_CASE("friendly fire handles finite segments and unsupported lifetime", "[combat]")
{
    CHECK(InfantryCombat::IntersectsBody(VZero, VZero, Vector3(0, 0.5f, 0), 1));
    CHECK_FALSE(InfantryCombat::IntersectsBody(VZero, Vector3(0, 0, 1), Vector3(0, 0, 3), 1));
    Ballistics::ShellParams shell;
    shell.timeToLive = 100;
    CHECK_FALSE(InfantryCombat::BulletPathSafe(VZero, Vector3(0, 0, 600), shell, 0, VZero,
        [](Vector3Par, Vector3Par, float, float) { return InfantryCombat::PathDecision::Continue; }));
}

TEST_CASE("blocked flight envelopes agree with exhaustive segment tests", "[combat]")
{
    Ballistics::ShellParams shell;
    shell.timeToLive = 2;
    InfantryCombat::BulletEnvelope path;
    InfantryCombat::BulletPathSafe(Vector3(0, 10, 0), Vector3(0, 0, 600), shell, 1, VZero,
        [&](Vector3Par from, Vector3Par to, float t, float r) {
            path.Add(from, to, t, r);
            return InfantryCombat::PathDecision::Continue;
        });
    for (float x : {0.0f, 1.0f, 5.0f, 20.0f})
        for (float z : {-10.0f, 20.0f, 160.0f, 300.0f, 600.0f, 1200.0f})
            for (float speed : {0.0f, 5.0f})
            {
                const Vector3 body(x, 5, z);
                const bool safe = InfantryCombat::BulletPathSafe(Vector3(0, 10, 0), Vector3(0, 0, 600), shell, 1, VZero,
                    [&](Vector3Par from, Vector3Par to, float t, float r) {
                        return InfantryCombat::IntersectsBody(from, to, body, 1 + speed * t + r)
                            ? InfantryCombat::PathDecision::Block : InfantryCombat::PathDecision::Continue;
                    });
                CHECK(path.Intersects(body, 1, speed) == !safe);
            }
    path.Clear();
    CHECK_FALSE(path.Intersects(Vector3(0, 5, 600), 100, 100));
}

TEST_CASE("fire lane movement requires sustained rejection for the same target", "[combat]")
{
    InfantryCombat::FireLaneWait wait;
    int target = 0, another = 0;
    CHECK_FALSE(wait.ShouldYield(0, &target));
    wait.Blocked(1, &target);
    CHECK_FALSE(wait.ShouldYield(1.2f, &target));
    wait.Blocked(1.5f, &target);
    wait.Blocked(1.8f, &target);
    CHECK(wait.ShouldYield(1.8f, &target));
    CHECK_FALSE(wait.ShouldYield(1.8f, &another));
    CHECK_FALSE(wait.ShouldYield(3, &target));
    wait.Blocked(3, &target);
    CHECK_FALSE(wait.ShouldYield(3, &target));
    wait.Blocked(3.5f, &target);
    wait.Blocked(3.8f, &another);
    CHECK_FALSE(wait.ShouldYield(3.8f, &another));
    wait.Clear();
    CHECK_FALSE(wait.Recent(3.9f, &another));
    wait.Blocked(4, &target);
    wait.Blocked(2, &target); // rewound mission clock cannot retain a stale wait
    CHECK_FALSE(wait.ShouldYield(2, &target));
}
