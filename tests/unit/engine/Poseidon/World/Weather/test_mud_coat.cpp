#include <Poseidon/World/Weather/MudCoat.hpp>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace Poseidon;

static MudCoat Run(MudCoat c, float seconds, float soft, float speed, float spin, float water = 0, float rain = 0)
{
    for (float t = 0; t < seconds; t += 1.0f / 60.0f) AdvanceMudCoat(c, 1.0f / 60.0f, soft, speed, spin, water, rain);
    return c;
}

int main()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    // Firm ground, standing: stays clean.
    assert(Run({}, 30, 0, 5, 0).wheels == 0 && Run({}, 30, 1, 0, 0).body == 0);
    // Driving through soaked mud: tyres dirty in seconds, body over about a minute.
    {
        const MudCoat c5 = Run({}, 5, 1, 6, 0.1f), c15 = Run({}, 15, 1, 6, 0.1f), c60 = Run({}, 60, 1, 6, 0.1f);
        std::printf("drive: wheels %.2f/%.2f/%.2f body %.2f/%.2f/%.2f (5/15/60 s)\n",
                    c5.wheels, c15.wheels, c60.wheels, c5.body, c15.body, c60.body);
        assert(c5.wheels > 0.4f && c15.wheels > 0.8f);
        // Damp field (soft ~0.6): tyres show mud from the first seconds.
        const MudCoat first = Run({}, 5, 0.6f, 5, 0.05f), tyres = Run({}, 30, 0.6f, 5, 0.05f);
        const MudCoat half = Run({}, 90, 0.6f, 5, 0.05f), full = Run({}, 240, 0.6f, 5, 0.05f);
        std::printf("damp field: wheels %.2f/%.2f (5/30 s), body %.2f/%.2f (90/240 s)\n",
                    first.wheels, tyres.wheels, half.body, full.body);
        assert(first.wheels > 0.45f);                                // tyres clearly muddy first
        assert(Run({}, 15, 0.6f, 5, 0.05f).wheels > 0.85f && tyres.wheels > 0.95f);
        assert(first.body < 0.05f);                                  // while the body is still clean
        assert(Run({}, 30, 0.6f, 5, 0.05f).body < 0.3f);            // body clean-ish after half a minute
        assert(half.body > 0.3f && half.body < 0.65f && full.body > 0.7f);
        assert(c15.body < 0.5f && c60.body > 0.6f);
        assert(c60.body <= c60.wheels);
    }
    // Light cars cake more slowly than trucks (pickup < 1), trucks unchanged.
    {
        MudCoat jeep, truck, base;
        for (int i = 0; i < 600; ++i)
        {
            AdvanceMudCoat(jeep, 1.0f / 60.0f, 0.6f, 5, 0.05f, 0, 0, 0.55f);
            AdvanceMudCoat(truck, 1.0f / 60.0f, 0.6f, 5, 0.05f, 0, 0, 1.0f);
            AdvanceMudCoat(base, 1.0f / 60.0f, 0.6f, 5, 0.05f, 0, 0);
        }
        std::printf("10 s damp field: jeep wheels %.2f, truck wheels %.2f\n", jeep.wheels, truck.wheels);
        assert(truck.wheels == base.wheels && jeep.wheels < truck.wheels * 0.85f && jeep.wheels > 0.4f);
    }
    // Wheelspin plasters it quickly.
    {
        const MudCoat s = Run({}, 15, 1, 0, 1);
        assert(s.wheels > 0.95f && s.body > 0.6f);
    }
    // Damp ground dirties more slowly than soaked.
    assert(Run({}, 20, 0.4f, 6, 0).body < Run({}, 20, 1, 6, 0).body);
    // Washing follows depth and speed (in game a flat rule cleaned the truck in a second).
    {
        const MudCoat dirty{1, 1};
        const MudCoat dip = Run(dirty, 2, 0, 4, 0, 0.1f);            // tyre just touching a stream
        std::printf("dip 2 s: wheels %.2f body %.2f\n", dip.wheels, dip.body);
        assert(dip.wheels > 0.9f && dip.body > 0.98f);
        const MudCoat ford10 = Run(dirty, 10, 0, 3, 0, 0.6f), ford30 = Run(dirty, 30, 0, 3, 0, 0.6f);
        std::printf("wheel-deep ford: wheels %.2f/%.2f body %.2f/%.2f (10/30 s)\n", ford10.wheels, ford30.wheels, ford10.body, ford30.body);
        assert(ford10.wheels > 0.4f && ford30.wheels < 0.2f);    // gradual, mostly clean by ~30 s
        assert(ford30.body > 0.8f);                               // body hardly touched
        const MudCoat deep30 = Run(dirty, 30, 0, 3, 0, 1.4f), deep120 = Run(dirty, 120, 0, 3, 0, 1.4f);
        std::printf("deep ford: body %.2f/%.2f (30/120 s)\n", deep30.body, deep120.body);
        assert(deep30.body > 0.45f && deep120.body < 0.15f);
        // Driving through scrubs faster than standing in it.
        assert(Run(dirty, 10, 0, 4, 0, 0.6f).wheels < Run(dirty, 10, 0, 0, 0, 0.6f).wheels);
    }
    // Rain rinses, slowly.
    {
        const MudCoat r60 = Run({1, 1}, 60, 0, 0, 0, 0, 1), r600 = Run({1, 1}, 600, 0, 0, 0, 0, 1);
        assert(r60.body > 0.85f && r600.body < 0.45f);
    }
    // Bad input never breaks it.
    {
        MudCoat c{nan, nan};
        AdvanceMudCoat(c, 1, nan, nan, nan, nan, nan);
        assert(c.wheels == 0 && c.body == 0);
        AdvanceMudCoat(c, nan, 1, 5, 1, 0, 0);
        assert(c.wheels == 0);
    }
    // Render encoding round-trips and never collides with ordinary (>= 0) values.
    {
        for (float w = 0; w <= 1.0f; w += 0.05f)
            for (float b = 0; b <= 1.0f; b += 0.05f)
            {
                const float e = EncodeMudCoat({w, b});
                assert(e <= -1.0f && e > -130.0f);
                const MudCoat d = DecodeMudCoat(e);
                assert(std::abs(d.wheels - w) <= 0.5f / 127.0f + 1e-5f);
                assert(std::abs(d.body - std::fmin(b, 0.999f)) < 1e-3f);
            }
        assert(DecodeMudCoat(1.0f).wheels == 0 && DecodeMudCoat(0.0f).body == 0 && DecodeMudCoat(nan).wheels == 0);
        assert(EncodeMudCoat({nan, nan}) == -1.0f);
    }
    // Up axis packing: upright, pitched, rolled; the plane height then follows the vehicle.
    {
        for (float pitch : {0.0f, 0.1f, 0.3f, -0.4f})
            for (float roll : {0.0f, 0.2f, -0.25f})
            {
                const float x = std::sin(roll), z = std::sin(pitch);
                const float y = std::sqrt(std::max(0.0f, 1 - x * x - z * z));
                const MudUp u = UnpackMudUp(PackMudUp(x, y, z));
                assert(std::abs(u.x - x) < 1e-3f && std::abs(u.z - z) < 1e-3f && std::abs(u.y - y) < 2e-3f);
            }
        const MudUp level = UnpackMudUp(PackMudUp(0, 1, 0));
        assert(std::abs(level.x) < 1e-3f && std::abs(level.y - 1) < 1e-6f);
        const MudUp bad = UnpackMudUp(nan);
        assert(bad.y == 1 && bad.x == 0);
        assert(PackMudUp(0, 0, 0) == PackMudUp(0, 1, 0));
        // A truck pitched 15 degrees nose-down on a slope: its bonnet, 1.8 m above the
        // lowest point along the truck's up axis, still measures 1.8 m.
        const float p = 0.26f;
        const float upx = 0, upy = std::cos(p), upz = std::sin(p);
        const MudUp u = UnpackMudUp(PackMudUp(upx, upy, upz));
        const float bx = 1000, by = 50, bz = 2000;                       // lowest point, world
        const float d = bx * u.x + by * u.y + bz * u.z;
        const float hx = bx + 3.0f * 0 + 1.8f * upx, hy = by - 3.0f * std::sin(p) + 1.8f * upy, hz = bz + 3.0f * std::cos(p) + 1.8f * upz;
        const float h = hx * u.x + hy * u.y + hz * u.z - d;              // bonnet 3 m forward along the truck
        std::printf("pitched bonnet height %.3f m (world drop %.2f m)\n", h, 3.0f * std::sin(p));
        assert(std::abs(h - 1.8f) < 0.02f);
    }
    std::printf("Mud coat pick-up, washing, rinsing and render encoding tests passed.\n");
    return 0;
}
