#include <Poseidon/World/Weather/SandField.hpp>
#include <Poseidon/World/Weather/SandGroundAdmission.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <cstdio>

using namespace Poseidon;

static std::array<float, 3> PackedSample(const std::vector<float>& data, float x, float z)
{
    const float tx = (x - data[0]) / data[2] - 0.5f, tz = (z - data[1]) / data[2] - 0.5f;
    const int ix = int(std::floor(tx)), iz = int(std::floor(tz));
    assert(ix >= 0 && iz >= 0 && ix < 511 && iz < 511);
    const float fx = tx - ix, fz = tz - iz;
    const int i = 4 + iz * 512 + ix;
    const float a = data[i], b = data[i + 1], c = data[i + 512], d = data[i + 513];
    return {a * (1-fx) * (1-fz) + b * fx * (1-fz) + c * (1-fx) * fz + d * fx * fz,
            ((b-a) * (1-fz) + (d-c) * fz) / data[2],
            ((c-a) * (1-fx) + (d-b) * fx) / data[2]};
}

int main()
{
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    SandField sand;
    assert(sand.ContactActive() && sand.Wetness() == 0 && sand.ContactDepth() == 0.10f);
    sand.Advance(0, 1); sand.Advance(-1, 1); sand.Advance(nan, 1); sand.Advance(inf, 1);
    assert(sand.Wetness() == 0);
    sand.Advance(30, 1);
    assert(std::abs(sand.Wetness() - float(1 - std::exp(-1.0))) < 1e-7f);
    SandField split;
    for (int i = 0; i < 120; ++i) split.Advance(.25f, 1);
    assert(std::abs(split.Wetness() - sand.Wetness()) < 1e-6f);
    const float dampDepth = sand.ContactDepth();
    sand.Advance(10000, 2);
    assert(sand.Wetness() == 1 && sand.ContactDepth() == .055f && sand.ContactRim() == .008f);
    assert(dampDepth < .10f && dampDepth > .055f);
    sand.Advance(10000, nan);
    assert(sand.Wetness() < 1e-20f && sand.ContactDepth() == .10f);

    assert(!sand.StampBoot(nan, 0, 0, 1, .1f, .01f));
    assert(!sand.StampBoot(0, inf, 0, 1, .1f, .01f));
    assert(!sand.StampBoot(1000001, 0, 0, 1, .1f, .01f));
    assert(!sand.StampBoot(0, 0, nan, 1, .1f, .01f));
    assert(!sand.StampBoot(0, 0, inf, 1, .1f, .01f));
    assert(!sand.StampBoot(0, 0, 0, 0, .1f, .01f));
    assert(!sand.StampBoot(0, 0, 0, 1, nan, .01f));
    assert(!sand.StampBoot(0, 0, 0, 1, .1f, -1));
    assert(!sand.StampBoot(0, 0, 0, 1, .1f, inf));
    assert(sand.Chunks() == 0 && sand.Revision() == 0);
    assert(sand.StampBoot(.0625f, .0625f, 0, 1, sand.ContactDepth(), sand.ContactRim()));
    assert(sand.HeightOffsetAt(.0625f, .0625f) == -.10f);
    assert(sand.HeightOffsetAt(.3125f, .0625f) > 0);
    assert(sand.HeightOffsetAt(1, 1) == 0);
    // Real toe/heel orientation, not an isotropic disc decal.
    assert(sand.HeightOffsetAt(.0625f, .1875f) < sand.HeightOffsetAt(.1875f, .0625f));
    auto snapshot = sand.Snapshot(0, 0);
    assert(snapshot.size() == 4 + 512 * 512 && snapshot[2] == .125f && snapshot[3] == .15f);
    const auto revision = sand.Revision();
    assert(!sand.StampBoot(.0625f, .0625f, 0, 1, .10f, .014f));
    assert(sand.Revision() == revision);
    sand.Advance(300, 1);
    assert(!sand.StampBoot(.0625f, .0625f, 0, 1, sand.ContactDepth(), sand.ContactRim()));
    assert(sand.Snapshot(0, 0) == snapshot); // cohesive rain never erases old loose-sand tracks

    for (int z = -12; z <= 12; ++z)
        for (int x = -12; x <= 12; ++x)
        {
            const float east = x * .041f + .009f, north = z * .037f + .017f;
            const auto packed = PackedSample(snapshot, east, north);
            const auto gradient = sand.HeightGradientAt(east, north);
            assert(std::abs(packed[0] - sand.HeightOffsetAt(east, north)) < .000003f);
            assert(std::abs(packed[1] - gradient[0]) < .00003f);
            assert(std::abs(packed[2] - gradient[1]) < .00003f);
            assert(std::isfinite(gradient[0]) && std::isfinite(gradient[1]));
            assert(std::hypot(gradient[0], gradient[1]) <= std::sqrt(2.0f) * (.15f + .02f) / .125f);
            assert(packed[0] >= -.15f && packed[0] <= .02f);
        }
    const float ex = .013f, ez = .023f, eps = .0001f;
    const auto gradient = sand.HeightGradientAt(ex, ez);
    assert(std::abs(gradient[0] - (sand.HeightOffsetAt(ex+eps, ez)-sand.HeightOffsetAt(ex-eps, ez))/(2*eps)) < .0002f);
    assert(std::abs(gradient[1] - (sand.HeightOffsetAt(ex, ez+eps)-sand.HeightOffsetAt(ex, ez-eps))/(2*eps)) < .0002f);
    const auto nearChunks = sand.Chunks();
    (void)sand.Snapshot(10000, -10000);
    assert(sand.Snapshot(0, 0) == snapshot && sand.Chunks() == nearChunks && sand.Revision() == revision);
    sand.enabled = false;
    assert(!sand.ContactActive() && sand.ContactDepth() == 0 && sand.ContactRim() == 0);
    assert((sand.HeightOffsetAt(.0625f, .0625f) == 0 && sand.HeightGradientAt(0, 0) == std::array<float, 2>{}));
    assert(!sand.StampBoot(0, 0, 0, 1, .1f, .01f));
    const float wet = sand.Wetness(); sand.Advance(30, 1); assert(sand.Wetness() == wet);
    const auto disabled = sand.Snapshot(0, 0); assert(disabled[3] == 0);
    assert(std::all_of(disabled.begin()+4, disabled.end(), [](float v) { return v == 0; }));
    sand.enabled = true;
    assert(sand.Snapshot(0, 0) == snapshot && sand.HeightOffsetAt(nan, 0) == 0);
    assert(sand.Snapshot(inf, 0)[3] == 0);
    sand.Reset(); assert(sand.Chunks() == 0 && sand.Wetness() == 0 && sand.Revision() == revision + 1);

    SandField a, b;
    a.StampBoot(.0625f, .0625f, 0, 1, .1f, .014f); a.StampBoot(.3125f, .0625f, 1, 0, .08f, .012f);
    b.StampBoot(.3125f, .0625f, 1, 0, .08f, .012f); b.StampBoot(.0625f, .0625f, 0, 1, .1f, .014f);
    assert(a.Snapshot(0, 0) == b.Snapshot(0, 0));
    assert(a.HeightOffsetAt(.3125f, .0625f) < 0); // new depression wins an old rim
    SandField negative;
    assert(negative.StampBoot(-.0625f, -.0625f, 0, 1, .10f, .014f));
    assert(negative.HeightOffsetAt(-.0625f, -.0625f) == -.10f);
    const auto negativeSnapshot = negative.Snapshot(-.0625f, -.0625f);
    assert(std::abs(PackedSample(negativeSnapshot, -.0625f, -.0625f)[0] + .10f) < .000001f);
    assert(negative.HeightOffsetAt(-1, -1) == 0);
    SandField capped;
    for (size_t i = 0; i < SandField::MaxChunks; ++i)
        assert(capped.StampBoot(float(i)*4+.5625f, .5625f, 0, 1, 1, 1));
    assert(capped.Chunks() == SandField::MaxChunks);
    const float retained = capped.HeightOffsetAt(.5625f, .5625f);
    assert(retained == -.15f);
    assert(!capped.StampBoot(-10, -10, 0, 1, .1f, .01f));
    assert(capped.Rejected() > 0 && capped.Chunks() == SandField::MaxChunks && capped.HeightOffsetAt(.5625f, .5625f) == retained);
    capped.Reset(); assert(capped.Rejected() == 0 && capped.Chunks() == 0);

    assert(StockSandSurface("Sand", "ps??????", "sand", ""));
    assert(StockSandSurface("SandAbel", "pi??????", "sand", ""));
    assert(StockSandSurface("SandDark", "pt??????", "sand", ""));
    for (const auto cls : {"SandBuilding", "SandySoil", "Sandstone", "Default", "sand", "Grass", "", "SandDarkMod"})
        assert(!StockSandSurface(cls, "ps??????", "sand", ""));
    assert(!StockSandSurface("Sand", "pt??????", "sand", ""));
    assert(!StockSandSurface("Sand", "ps??????", "rock", ""));
    assert(!StockSandSurface("Sand", "ps??????", "sand", "clutter"));
    for (const auto uv : {nan, inf, -.1f, 1.0f, 1.1f})
        assert(!SandSourceInterior(uv, .5f) && !SandSourceInterior(.5f, uv));
    assert(SandSourceInterior(.0f, .999f) && SandSourceInterior(.119f, .881f) && SandSourceInterior(.5f, .5f));
    std::puts("Sand field dry/wet compaction, signed rims, gradient/snapshot parity, persistence/cap and exact stock source tests passed.");
}
