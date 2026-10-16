#include <Poseidon/World/Weather/MudTraction.hpp>
#include <Poseidon/World/Weather/MudVehiclePrototype.hpp>
#include <Poseidon/World/Weather/MudField.hpp>
#include <cstdio>
#include <cassert>
#include <cmath>
#include <limits>

using namespace Poseidon;

int main()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    // Softness follows the MudField contact curve: dry ground is firm.
    assert(MudSoftness(0.0f) == 0.0f && MudSoftness(0.15f) == 0.0f && MudSoftness(1.0f) == 1.0f);
    assert(MudSoftness(nan) == 0.0f);
    for (float w = 0.0f; w < 1.0f; w += 0.05f) assert(MudSoftness(w + 0.05f) >= MudSoftness(w));

    // Grip: firm ground, dry mud, no wheels or no profile -> untouched.
    assert(MudGrip(1, .2f, .24f, 0, 6, .65f) == 1.0f);
    assert(MudGrip(1, .2f, .24f, 6, 0, .65f) == 1.0f);
    assert(MudGrip(0.1f, .2f, .24f, 6, 6, .65f) == 1.0f);
    assert(MudGrip(1, .2f, .24f, 6, 6, 0) == 1.0f);
    assert(MudGrip(1, .2f, .24f, 6, 6, nan) == 1.0f);

    // Soaked fresh mud loses most of the profile's loss; a deep rut the rest.
    const float fresh = MudGrip(1, 0, .24f, 6, 6, .65f);
    const float rut = MudGrip(1, .24f, .24f, 6, 6, .65f);
    assert(std::abs(fresh - (1 - .65f * .75f)) < 1e-5f);
    assert(std::abs(rut - (1 - .65f)) < 1e-5f);
    assert(rut < fresh && fresh < 1);
    // More wheels in mud, wetter ground and deeper ruts never increase grip.
    for (int n = 1; n < 6; ++n) assert(MudGrip(1, .1f, .24f, n + 1, 6, .65f) <= MudGrip(1, .1f, .24f, n, 6, .65f));
    for (float d = 0; d < .3f; d += .02f) assert(MudGrip(1, d + .02f, .24f, 6, 6, .65f) <= MudGrip(1, d, .24f, 6, 6, .65f));
    // Bounded for any input, including absurd profiles.
    for (float loss : {.1f, .65f, 1.0f, 5.0f, inf})
        for (float d : {-1.0f, 0.0f, .1f, 1.0f, nan})
        {
            const float g = MudGrip(1, d, .24f, 6, 6, loss);
            assert(g >= MudMinGrip && g <= 1.0f);
        }
    // Truck loses more than the jeep in the same mud.
    const auto truck = SelectMudWheelProfile("Truck5t", true, true);
    const auto jeep = SelectMudWheelProfile("Jeep", false, true);
    assert(truck.gripLoss > jeep.gripLoss && truck.digMax > jeep.digMax);
    assert(MudGrip(1, .1f, .24f, 6, 6, truck.gripLoss) < MudGrip(1, .1f, .24f, 4, 4, jeep.gripLoss));
    // Profiles that don't opt in stay off.
    assert(SelectMudWheelProfile("Ural", true, true).gripLoss == 0.0f);

    // Side grip falls half as much as drive grip.
    assert(MudSideGrip(1) == 1 && std::abs(MudSideGrip(.4f) - .7f) < 1e-6f && MudSideGrip(nan) == 1);
    for (float g = 0; g <= 1; g += .1f) assert(MudSideGrip(g) >= g && MudSideGrip(g) >= .5f);

    // Wheel spin: needs missing grip and real throttle.
    assert(MudWheelSpinTarget(1, 1, 0, 20) == 0);
    assert(MudWheelSpinTarget(.4f, 0, 0, 20) == 0);
    assert(MudWheelSpinTarget(.4f, .15f, 0, 20) == 0);
    assert(MudWheelSpinTarget(nan, 1, 0, 20) == 0 && MudWheelSpinTarget(.4f, 1, nan, 20) == 0);
    const float stuck = MudWheelSpinTarget(.35f, 1, 0, 20);
    const float rolling = MudWheelSpinTarget(.35f, 1, 15, 20);
    assert(stuck > .7f && rolling > 0 && rolling < stuck);
    // Reverse throttle spins the same way as forward.
    assert(MudWheelSpinTarget(.35f, -1, 0, 20) == stuck);
    // Less throttle, less spin; better grip, less spin.
    assert(MudWheelSpinTarget(.35f, .5f, 0, 20) < stuck);
    assert(MudWheelSpinTarget(.8f, 1, 0, 20) < stuck);

    // Spin builds fast, decays slower, never overshoots, stays in [0,1].
    float s = 0;
    for (int i = 0; i < 20; ++i) s = MudSpinApproach(s, 1, .016f);
    assert(s > .9f && s <= 1);
    float up = MudSpinApproach(0, 1, .1f), down = MudSpinApproach(1, 0, .1f);
    assert(up > 1 - down);
    assert(MudSpinApproach(.5f, .6f, 10) == .6f && MudSpinApproach(.5f, .4f, 10) == .4f);
    assert(MudSpinApproach(nan, 1, .1f) >= 0 && MudSpinApproach(.5f, nan, .1f) == .5f);
    assert(MudSpinApproach(.5f, 2, 100) == 1 && MudSpinApproach(.5f, -2, 100) == 0);

    // Digging: never deeper than the vehicle limit; more spin digs deeper.
    assert(MudDigDepth(0, .24f, .28f) == 0 && MudDigDepth(1, 0, .28f) == 0 && MudDigDepth(1, .24f, 0) == 0);
    assert(MudDigDepth(1, .24f, .28f) == .28f);
    assert(MudDigDepth(.2f, .24f, .28f) < MudDigDepth(.8f, .24f, .28f));
    assert(MudDigDepth(1, 1, .28f) <= .28f && MudDigDepth(nan, .24f, .28f) == 0);
    assert(MudDigPressure(0, .016f) == 0 && MudDigPressure(1, 0) == 0);
    assert(MudDigPressure(1, .016f) > 0 && MudDigPressure(1, .016f) < .01f);
    assert(MudDigPressure(1, 100) == 1 && MudDigPressure(nan, .1f) == 0);

    // Rut resistance grows with depth up to 2.5x.
    assert(MudSinkResistanceScale(0, .24f) == 1 && std::abs(MudSinkResistanceScale(.24f, .24f) - 1.6f) < 1e-6f);
    assert(std::abs(MudSinkResistanceScale(5, .24f) - 1.6f) < 1e-6f && MudSinkResistanceScale(nan, .24f) == 1);

    // Spray: none on firm ground, mostly from spin, capped.
    assert(MudSprayDensity(1, 10, 0) == 0);
    assert(MudSprayDensity(1, 0, 1) > MudSprayDensity(0, 10, 1));
    assert(MudSprayDensity(1, 100, 1) <= 1 && MudSprayDensity(nan, 1, 1) == 0);
    // Rut cap: never below base, limited by the dig limit and the soft layer.
    assert(MudRutCap(.28f, .24f, .15f) == .28f);
    assert(std::abs(MudRutCap(.28f, .05f, .03f) - .10f) < 1e-6f);
    assert(MudRutCap(.28f, .01f, .03f) == .03f);
    assert(MudRutCap(0, .24f, .1f) == .1f && MudRutCap(.28f, .24f, 0) == 0 && MudRutCap(nan, .24f, .1f) == .1f);
    // Pass depth: fresh ground -> base; existing rut -> a bit deeper; never past cap; never shallower.
    assert(MudRutPassDepth(0, .05f, .2f) == .05f);
    assert(MudRutPassDepth(.05f, .05f, .2f) > .05f);
    assert(MudRutPassDepth(.03f, .05f, .2f) == .05f);           // shallow dent: fresh press
    assert(MudRutPassDepth(.045f, .05f, .2f) > .05f);           // near-full rut: deeper
    assert(MudRutPassDepth(.2f, .05f, .2f) == .2f && MudRutPassDepth(.5f, .05f, .2f) == .2f);
    assert(MudRutPassDepth(.1f, .05f, .02f) == .05f || MudRutPassDepth(.1f, .05f, .02f) <= .1f);
    assert(MudRutPassDepth(nan, .05f, .2f) == .05f && MudRutPassDepth(.1f, 0, .2f) == 0);

    // End to end on the real MudField: drive a wheel along the same line several
    // times (stamps every 0.25 m, like MudWheelTrail) and watch the rut deepen.
    {
        MudField field;
        field.Advance(600.0f, 1.0f);                         // soaked
        const float contact = field.ContactDepth();
        const float base = std::min(truck.maxDepth, contact * truck.depthScale);
        const float cap = MudRutCap(truck.digMax, contact, base);
        float last = 0;
        std::printf("soaked: base %.3f cap %.3f\n", base, cap);
        for (int pass = 1; pass <= 8; ++pass)
        {
            for (float x = 100.0f; x <= 110.0f; x += 0.25f)
            {
                const float stored = std::max(0.0f, -field.HeightOffsetAt(x, 50.0f));
                const float d = MudRutPassDepth(stored, MudFirstPassDepth(base, cap), cap);
                field.Stamp(x, 50.0f, MudField::ContactRadius(d), d, 0.8f);
            }
            const float depth = -field.HeightOffsetAt(105.0f, 50.0f);
            std::printf("pass %d: rut %.3f m\n", pass, depth);
            assert(depth >= last - 1e-5f && depth <= cap + 1e-5f);
            // First drive already reaches most of the deep-rut look.
            if (pass == 1) assert(depth > MudFirstPassDepth(base, cap) * 0.75f && depth > cap * 0.7f);
            last = depth;
        }
        assert(last > base * 1.1f && last > cap * 0.9f);    // repeated passes deeper still, up to the cap
    }
    // Uneven stamps: bounded variation, ridges outside the rut on both sides.
    {
        float minD = 1, maxD = 0;
        for (int i = 0; i < 200; ++i)
        {
            const float x = 10 + i * 0.25f;
            const MudRutStamp st = PlanMudRutStamp(x, 5, 1, 0, 0.5f, 0.1f);
            minD = std::min(minD, st.depth); maxD = std::max(maxD, st.depth);
            assert(st.depth >= 0.08f - 1e-6f && st.depth <= 0.12f + 1e-6f);
            assert(st.radius >= 0.45f - 1e-6f && st.radius <= 0.575f + 1e-6f);
            assert(std::abs(st.z - 5) <= 0.04f + 1e-6f);                 // wanders sideways only a little
            assert(st.rimZ[0] < st.z - st.radius && st.rimZ[1] > st.z + st.radius);   // outside the rut
            for (int k = 0; k < 2; ++k) assert(st.rimHeight[k] > 0 && st.rimHeight[k] <= MudField::MaxRim);
            const MudRutStamp again = PlanMudRutStamp(x, 5, 1, 0, 0.5f, 0.1f);
            assert(again.depth == st.depth && again.rimHeight[1] == st.rimHeight[1]);   // deterministic
        }
        assert(maxD - minD > 0.02f);                                      // actually uneven
        const MudRutStamp calm = PlanMudRutStamp(3, 3, 0, 1, .5f, .1f, 0), spin = PlanMudRutStamp(3, 3, 0, 1, .5f, .1f, 1);
        assert(spin.rimHeight[0] > calm.rimHeight[0] && spin.rimRadius[0] > calm.rimRadius[0]);
        assert(PlanMudRutStamp(nan, 0, 1, 0, .5f, .1f).depth == 0 && PlanMudRutStamp(0, 0, 0, 0, .5f, .1f).depth > 0);
    }

    // MudField::Raise: heaps soil (positive offset), never inside a rut, capped, and a wheel presses it down.
    {
        MudField f;
        f.Advance(600, 1);
        assert(f.Raise(20.0625f, 20.0625f, .4f, .05f) && f.HeightOffsetAt(20.0625f, 20.0625f) > .04f);
        assert(!f.Raise(20.0625f, 20.0625f, .4f, .03f));                  // lower heap never lowers it
        f.Raise(30.0625f, 30.0625f, .4f, 5.0f);
        assert(f.HeightOffsetAt(30.0625f, 30.0625f) <= MudField::MaxRim + 1e-6f);
        f.Stamp(40.0625f, 40.0625f, .4f, .1f);
        const float rut = f.HeightOffsetAt(40.0625f, 40.0625f);
        f.Raise(40.0625f, 40.0625f, .4f, .05f);
        assert(f.HeightOffsetAt(40.0625f, 40.0625f) == rut);             // no soil piles in the rut
        f.Stamp(20.0625f, 20.0625f, .4f, .08f, 1.0f);
        assert(f.HeightOffsetAt(20.0625f, 20.0625f) < 0);                // driven over: ridge flattened into rut
        const auto snap = f.Snapshot(20, 20);
        bool positive = false;
        for (size_t i = 4; i < snap.size(); ++i) positive = positive || snap[i] > 0.01f;
        assert(positive);                                                 // ridges survive the renderer upload
    }

    // Full truck track on the real field: two wheel lines, three axles, two drives.
    {
        MudField f;
        f.Advance(600, 1);
        const float contact = f.ContactDepth();
        const float base = std::min(truck.maxDepth, contact * truck.depthScale);
        const float cap = MudRutCap(truck.digMax, contact, base);
        for (int drive = 0; drive < 2; ++drive)
            for (int axle = 0; axle < 3; ++axle)
                for (float x = 100; x <= 112; x += 0.25f)
                    for (float lane : {48.9f, 51.1f})
                    {
                        const float stored = std::max(0.0f, -f.HeightOffsetAt(x, lane));
                        const float d = MudRutPassDepth(stored, MudFirstPassDepth(base, cap), cap);
                        const MudRutStamp st = PlanMudRutStamp(x, lane, 1, 0, MudWheelRutRadius(d), d);
                        f.Stamp(st.x, st.z, st.radius, st.depth, 0.8f);
                        for (int k = 0; k < 2; ++k) f.Raise(st.rimX[k], st.rimZ[k], st.rimRadius[k], st.rimHeight[k]);
                    }
        float rutMin = 0, ridgeMax = 0, rutMax = -1;
        for (float x = 102; x <= 110; x += 0.125f)
        {
            const float c = f.HeightOffsetAt(x, 48.9f);
            rutMin = std::min(rutMin, c); rutMax = std::max(rutMax, c);
            for (float dz = 0.3f; dz <= 0.7f; dz += 0.0625f)
                ridgeMax = std::max(ridgeMax, std::max(f.HeightOffsetAt(x, 48.9f - dz), f.HeightOffsetAt(x, 48.9f + dz)));
        }
        std::printf("track: rut %.3f..%.3f m, ridges up to +%.3f m\n", rutMin, rutMax, ridgeMax);
        assert(rutMin < -0.15f);                 // deep after two drives
        assert(rutMax - rutMin > 0.02f);         // uneven bottom
        assert(ridgeMax > 0.02f && ridgeMax <= MudField::MaxRim + 1e-6f);   // visible ridges
    }
    // First pass: most of the way to the cap, never past it, plain base without headroom.
    {
        assert(std::abs(MudFirstPassDepth(.1f, .2f) - .17f) < 1e-6f);
        assert(MudFirstPassDepth(.1f, .1f) == .1f && MudFirstPassDepth(.1f, .05f) == .1f);
        assert(MudFirstPassDepth(0, .2f) == 0 && MudFirstPassDepth(nan, .2f) == 0 && MudFirstPassDepth(.1f, nan) == .1f);
        assert(MudFirstPassDepth(.1f, .2f, 1) == .2f && MudFirstPassDepth(.1f, .2f, 0) == .1f);
        // Damp field as logged in game (contact ~13 cm): one drive now presses
        // ~19 cm, close to the 22 cm the user liked after 5-8 passes, not ~10 cm.
        const float contact = 0.131f;
        const float base = std::min(truck.maxDepth, contact * truck.depthScale);
        const float cap = MudRutCap(truck.digMax, contact, base);
        std::printf("damp: base %.3f first %.3f cap %.3f\n", base, MudFirstPassDepth(base, cap), cap);
        assert(MudFirstPassDepth(base, cap) > base * 1.3f);
    }
    // Rolling resistance never becomes a wall.
    {
        assert(MudLimitRollingDeceleration(3.0f) == MudMaxRollingDeceleration);
        assert(MudLimitRollingDeceleration(-3.0f) == -MudMaxRollingDeceleration);
        assert(MudLimitRollingDeceleration(0.5f) == 0.5f && MudLimitRollingDeceleration(nan) == 0);
        assert(MudSinkResistanceScale(1, 0.24f) <= 1.6f + 1e-6f);
        // Soaked, all wheels in a full-depth rut: the truck keeps over half its grip,
        // and its full-throttle push (~9 m/s^2 * grip) beats the capped resistance.
        const float g = MudGrip(1, .24f, .24f, 6, 6, truck.gripLoss);
        std::printf("soaked deep-rut grip %.3f\n", g);
        assert(g > 0.55f && 9.0f * 0.5f * g > MudMaxRollingDeceleration * 1.5f);
    }
    // The displayed mud window leads the camera.
    {
        const MudViewCentre c = MudWindowCentre(100, 200, 0, 2);
        assert(c.x == 100 && std::abs(c.z - (200 + MudViewLookAhead)) < 1e-4f);
        const MudViewCentre down = MudWindowCentre(100, 200, 0.01f, 0.0f);
        assert(down.x == 100 && down.z == 200);
        const MudViewCentre bad = MudWindowCentre(100, 200, nan, 1);
        assert(bad.x == 100 && bad.z == 200);
        // Ahead of the camera the window (32 m half-size) reaches at least 44 m.
        assert(MudViewLookAhead + 32.0f - 2.0f >= 44.0f);
        // The fine mesh centred on it still fits inside the window.
        assert(MudViewLookAhead < 32.0f);
    }
    // Overcast missions start soaked, and heavy cloud keeps the soil from drying out.
    {
        assert(MudField::WeatherDampness(0.3f) == 0 && MudField::WeatherDampness(0.5f) == 0);
        assert(MudField::WeatherDampness(nan) == 0 && std::abs(MudField::WeatherDampness(1) - 0.95f) < 1e-6f);
        assert(MudField::WeatherDampness(0.8f) > 0.55f && MudField::WeatherDampness(0.8f) < 0.7f);
        for (float o = 0; o < 1; o += 0.05f) assert(MudField::WeatherDampness(o + 0.05f) >= MudField::WeatherDampness(o));
        MudField f;
        f.SeedWeather(0.8f);
        assert(f.Wetness() > 0.55f && f.ContactActive());
        const float start = f.Wetness();
        for (int i = 0; i < 600; ++i) f.Advance(1, 0, 0.8f);     // ten dry minutes under the same cloud
        assert(std::abs(f.Wetness() - start) < 1e-4f);
        for (int i = 0; i < 600; ++i) f.Advance(1, 0, 0.2f);     // clears up: dries out
        assert(f.Wetness() < 0.05f);
        MudField clear;
        clear.SeedWeather(0.3f);
        assert(clear.Wetness() == 0 && !clear.ContactActive());   // clear sky: dry ground
        for (int i = 0; i < 600; ++i) clear.Advance(1, 0, 0.9f);  // clouds roll in: soaks slowly
        assert(clear.Wetness() > 0.5f && clear.Wetness() < MudField::WeatherDampness(0.9f));
        MudField rain;
        rain.SeedWeather(0.8f);
        for (int i = 0; i < 300; ++i) rain.Advance(1, 0.8f, 0.8f); // real rain still soaks it further
        assert(rain.Wetness() > start + 0.2f);
    }
    // Far view: 0.5 m cells over 256 m, deepest rut per cell, ridges dropped, window centred.
    {
        MudField f;
        f.Advance(600, 1);
        f.Stamp(100.0625f, 200.0625f, 0.3f, 0.15f, 1.0f);       // a rut 100 m from the far centre
        f.Raise(101.0625f, 200.0625f, 0.3f, 0.05f);              // and a ridge beside it
        const auto far = f.SnapshotFar(0, 200);
        assert(far.size() == 4 + 512 * 512 && far[2] == MudField::FarCellSize && far[3] == MudField::MaxDepth);
        assert(far[0] <= -127.0f && far[0] >= -129.0f);           // window starts ~128 m before the centre
        const auto at = [&](float x, float z) {
            const int i = int(std::floor((x - far[0]) / 0.5f)), j = int(std::floor((z - far[1]) / 0.5f));
            return (i < 0 || j < 0 || i >= 512 || j >= 512) ? 1.0f : far[4 + j * 512 + i];
        };
        std::printf("far: rut %.3f ridge %.3f empty %.3f\n", at(100.06f, 200.06f), at(101.06f, 200.06f), at(50, 150));
        assert(at(100.06f, 200.06f) < -0.1f);                    // the rut shows from 100 m away
        assert(at(101.06f, 200.06f) <= 0.0f);                    // ridges never positive in the far view
        assert(at(50, 150) == 0.0f);
        for (size_t i = 4; i < far.size(); ++i) assert(far[i] <= 0.0f && far[i] >= -MudField::MaxDepth);
        // Outside the far window: nothing.
        const auto away = f.SnapshotFar(1000, 1000);
        for (size_t i = 4; i < away.size(); ++i) assert(away[i] == 0.0f);
    }
    // Track ruts: a wide band with a flat floor and ridges outside both belt edges.
    {
        const MudTrackStamp t = PlanMudTrackStamp(10, 10, 0, 1, 0.3f, 0.08f);
        assert(t.valid && t.radius > 0.2f);
        for (int k = 0; k < 3; ++k) assert(t.depth[k] > 0.06f && t.depth[k] < 0.1f);
        assert(std::abs(t.x[0] - t.x[2]) > 0.3f && std::abs(t.z[0] - t.z[2]) < 1e-4f); // spread across the belt
        assert(std::abs(t.rimX[0] - 10) > 0.3f && std::abs(t.rimX[1] - 10) > 0.3f);
        assert((t.rimX[0] - 10) * (t.rimX[1] - 10) < 0);                  // one ridge each side
        assert(!PlanMudTrackStamp(10, 10, 0, 1, 0.3f, 0).valid);
        assert(!PlanMudTrackStamp(std::numeric_limits<float>::quiet_NaN(), 10, 0, 1, 0.3f, 0.08f).valid);
        const MudTrackStamp slip = PlanMudTrackStamp(10, 10, 0, 1, 0.3f, 0.08f, 1);
        assert(slip.rimHeight[0] > t.rimHeight[0] && slip.rimRadius[0] > t.rimRadius[0]);
        // Pressed into a field along a straight line: the floor across the belt is
        // flat-ish and wider than a tyre groove of the same depth.
        MudField f;
        f.SeedWeather(1.0f);
        for (float z = 5; z < 15; z += 0.25f)
        {
            const MudTrackStamp s = PlanMudTrackStamp(20, z, 0, 1, 0.3f, 0.08f);
            for (int k = 0; k < 3; ++k) f.Stamp(s.x[k], s.z[k], s.radius, s.depth[k], 0.8f);
        }
        const float centre = -f.HeightOffsetAt(20, 10), side = -f.HeightOffsetAt(20.2f, 10);
        float wide = 0;
        for (float x = 19; x <= 21; x += 0.0625f) if (-f.HeightOffsetAt(x, 10) > 0.5f * centre) wide += 0.0625f;
        std::printf("track rut: centre %.3f m, 20 cm off-centre %.3f m, width at half depth %.2f m\n", centre, side, wide);
        assert(centre > 0.03f && side > 0.85f * centre);   // flat floor across the belt
        assert(wide > 0.55f && wide < 1.2f);                // about a belt wide, not a tyre groove
    }
    // Water standing in ruts: none on damp ground, rising once soaked, full in a downpour.
    {
        assert(MudRutWaterLevel(0.3f) == 0.0f && MudRutWaterLevel(0.7f) == 0.0f);
        assert(MudRutWaterLevel(0.9f) > 0.3f && MudRutWaterLevel(0.9f) < 0.8f);
        assert(MudRutWaterLevel(1.1f) == 1.0f && MudRutWaterLevel(1.0f) > 0.8f);
        assert(MudRutWaterLevel(0.7f) < MudRutWaterLevel(0.9f));
        assert(MudRutWaterLevel(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
        // An overcast (soaked) mission start already has water in the deep ruts.
        MudField f;
        f.SeedWeather(1.0f);
        std::printf("rut water at overcast start: wetness %.2f level %.2f\n", f.Wetness(), MudRutWaterLevel(f.Wetness()));
        assert(MudRutWaterLevel(f.Wetness()) > 0.4f && MudRutWaterLevel(f.Wetness()) < 0.85f);
    }
    return 0;
}
