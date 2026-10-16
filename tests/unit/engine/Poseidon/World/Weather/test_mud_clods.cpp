#include <Poseidon/World/Weather/MudClodPhysics.hpp>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace Poseidon;

static MudClodState Launch(const MudClodLaunch& l, float y)
{
    MudClodState s;
    s.py = y; s.vx = l.vx; s.vy = l.vy; s.vz = l.vz;
    s.radius = l.radius; s.life = l.life; s.spinRate = l.spinRate;
    return s;
}

// Fly until landed (flat ground at y = 0); returns where it landed.
static MudClodState Fly(MudClodState s, float dt = 1.0f / 60.0f)
{
    for (int i = 0; i < 2000 && !s.Landed(); ++i) StepMudClod(s, dt, 0.0f);
    return s;
}

int main()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float mid[5] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f};

    // Ballistic: a clod thrown straight up comes back down and lands.
    {
        MudClodState s; s.py = 0.5f; s.vy = 5.0f; s.radius = 0.04f;
        const MudClodState end = Fly(s);
        assert(end.Landed() && end.age > 0.9f && end.age < 1.3f);
        assert(std::abs(end.py - 0.04f * 0.3f) < 1e-6f && end.vy == 0 && end.vx == 0);
        // Sticks: further steps never move it.
        MudClodState again = end; StepMudClod(again, 0.5f, 0.0f);
        assert(again.py == end.py && again.age > end.age);
    }
    // Drag: smaller clods lose more speed than big ones over the same flight.
    {
        MudClodState small; small.py = 2; small.vx = 8; small.radius = 0.01f;
        MudClodState big = small; big.radius = 0.06f;
        for (int i = 0; i < 30; ++i) { StepMudClod(small, 1 / 60.f, -100); StepMudClod(big, 1 / 60.f, -100); }
        assert(small.vx < big.vx && big.vx > 7.0f);
    }
    // Spinning wheel, truck standing still: mud flies back (opposite to drive) and up, metres away.
    {
        const MudClodLaunch l = PlanMudClodLaunch(0, 1, +1, 0, 0, 0, 0, 1, mid);
        assert(l.vz < -2 && l.vy > 2);
        const MudClodState end = Fly(Launch(l, 0.6f));
        std::printf("spin, standing: lands %.2f m behind\n", -end.pz);
        assert(end.pz < -1.5f && end.pz > -12.0f);
        // Reversing throws it forwards.
        const MudClodLaunch rev = PlanMudClodLaunch(0, 1, -1, 0, 0, 0, 0, 1, mid);
        assert(rev.vz > 2);
    }
    // Rolling without slip at 8 m/s: tread speed cancels the truck's, clods pop up and drop close behind.
    {
        const MudClodLaunch l = PlanMudClodLaunch(1, 0, +1, 8, 0, 0, 8, 0, mid);
        const MudClodState end = Fly(Launch(l, 0.6f));
        std::printf("rolling 8 m/s: lands %.2f m from release (truck moved %.2f m)\n", end.px, 8 * end.age);
        assert(std::abs(end.px) < 8 * end.age);       // left behind by the truck
        assert(l.vy > 1);                             // flicked up
    }
    // Spread: sizes, angles and lives vary but stay in range.
    {
        float rmin = 1, rmax = 0;
        for (int i = 0; i < 1000; ++i)
        {
            const float r[5] = {float(i % 7) / 7, float(i % 11) / 11, float(i % 13) / 13, float(i % 17) / 17, float(i % 19) / 19};
            const MudClodLaunch l = PlanMudClodLaunch(0.3f, 0.9f, 1, 1, 0, 2, 2, 0.5f, r);
            assert(std::isfinite(l.vx) && std::isfinite(l.vy) && std::isfinite(l.vz));
            assert(l.radius >= 0.016f && l.radius <= 0.087f && l.life >= 2.5f && l.life <= 5.5f);
            assert(l.vy > 0);
            rmin = std::fmin(rmin, l.radius); rmax = std::fmax(rmax, l.radius);
        }
        assert(rmax - rmin > 0.03f);
        const MudClodLaunch bad = PlanMudClodLaunch(nan, 0, 1, nan, 0, 0, nan, nan, mid);
        assert(std::isfinite(bad.vx) && std::isfinite(bad.vy) && std::isfinite(bad.vz));
    }
    // Sinking: full size until 60% of its life, then shrinks to nothing; expires at life.
    {
        MudClodState s; s.life = 5; s.landedAt = 0.5f; s.age = 2.0f;
        assert(MudClodVisibleScale(s) == 1);
        s.age = 4.0f; assert(MudClodVisibleScale(s) > 0.4f && MudClodVisibleScale(s) < 0.6f);
        s.age = 5.0f; assert(MudClodVisibleScale(s) == 0 && MudClodExpired(s));
        MudClodState flying; flying.age = 3; assert(MudClodVisibleScale(flying) == 1);
        MudClodState lost; lost.py = nan; assert(MudClodExpired(lost));
    }
    // Rate: none on firm ground, a few when rolling, lots when spinning.
    {
        assert(MudClodRate(1, 5, 0) == 0 && MudClodRate(nan, 5, 1) == 0);
        assert(MudClodRate(0, 0, 1) == 0);
        assert(MudClodRate(0, 8, 1) > 15 && MudClodRate(0, 8, 1) < 25);  // steady shower when rolling
        assert(MudClodRate(1, 0, 1) > 80);                                   // heavy spray when spinning
        assert(MudClodRate(0.1f, 6, 0.6f) * 6 > 80);                         // field drive, 6 wheels: ~100+ a second
        assert(MudClodRate(1, 0, 0.5f) < MudClodRate(1, 0, 1));
    }
    std::printf("Mud clod flight, landing, sinking and launch tests passed.\n");
    return 0;
}
