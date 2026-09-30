#include <Poseidon/World/Weather/MudField.hpp>
#include <Poseidon/World/Weather/MudGroundAdmission.hpp>
#include <cassert>
#include <limits>

using namespace Poseidon;

// Independently evaluates the documented packed GPU snapshot contract.
static float Sample(const std::vector<float>& data, float x, float z)
{
    const float tx = (x - data[0]) / data[2] - 0.5f;
    const float tz = (z - data[1]) / data[2] - 0.5f;
    assert(tx >= 0 && tz >= 0 && tx < 511 && tz < 511);
    const int ix = int(std::floor(tx)), iz = int(std::floor(tz));
    const float fx = tx - ix, fz = tz - iz;
    const size_t i = 4 + iz * 512 + ix;
    return data[i] * (1 - fx) * (1 - fz) + data[i + 1] * fx * (1 - fz) +
           data[i + 512] * (1 - fx) * fz + data[i + 513] * fx * fz;
}

int main()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    MudField mud;
    assert(!mud.ContactActive() && mud.Wetness() == 0 && mud.ContactDepth() == 0);
    mud.Advance(0, 1); mud.Advance(nan, 1); mud.Advance(inf, 1); mud.Advance(-1, 1);
    assert(mud.Wetness() == 0);
    mud.Advance(30, 1);
    assert(std::abs(mud.Wetness() - float(1 - std::exp(-1.0))) < 1e-7f);
    assert(mud.ContactActive() && mud.ContactDepth() > 0.10f && mud.ContactDepth() < MudField::DefaultLayerDepth);
    MudField split;
    for (int i = 0; i < 300; ++i) split.Advance(0.1f, 1);
    assert(std::abs(split.Wetness() - mud.Wetness()) < 0.00001f);

    // Real contacts cannot overdig; finite inputs and whole-world chunk keys.
    assert(!mud.Stamp(nan, 0, .125f, .04f));
    assert(!mud.Stamp(0, inf, .125f, .04f));
    assert(!mud.Stamp(0, 0, -1, .04f));
    assert(!mud.Stamp(0, 0, .125f, nan));
    assert(!mud.Stamp(1000001, 0, .125f, .04f));
    assert(mud.Stamp(-.0625f, -.0625f, .125f, 1));
    for (int i=0; i<40; ++i) mud.Stamp(-.0625f, -.0625f, MudField::MaxContactRadius, 1);
    assert(mud.HeightOffsetAt(-.0625f, -.0625f) == -MudField::MaxDepth);
    const auto revision = mud.Revision();
    assert(!mud.Stamp(-.0625f, -.0625f, MudField::MaxContactRadius, 1));
    assert(mud.Revision() == revision);
    assert(!mud.Stamp(-.0625f, -.0625f, .125f, .01f));
    assert(mud.HeightOffsetAt(100, 100) == 0);
    assert(mud.HeightOffsetAt(nan, 0) == 0);

    // Camera window reads do not mutate, and retain all world-space samples.
    const auto before = mud.Snapshot(0, 0);
    assert(before.size() == 4 + 512 * 512 && before[2] == .125f && before[3] == MudField::MaxDepth);
    const auto distant = mud.Snapshot(200, 200);
    for (size_t i = 4; i < distant.size(); ++i) assert(distant[i] == 0);
    assert(mud.Snapshot(0, 0) == before && mud.Revision() == revision);
    for (int z = -40; z <= 40; ++z)
        for (int x = -40; x <= 40; ++x)
        {
            const float east = x * .015625f, north = z * .015625f;
            const float offset = mud.HeightOffsetAt(east, north);
            assert(offset >= -MudField::MaxDepth && offset <= 0);
            assert(std::abs(Sample(before, east, north) - offset) < 1e-7f);
        }
    // Nonbinary world positions incur float subtraction roundoff when packed
    // window origin differs from zero. Keep its signed-height error below 2um.
    for (int z = -40; z <= 40; ++z)
        for (int x = -40; x <= 40; ++x)
            assert(std::abs(Sample(before, x * .013f, z * .011f) -
                mud.HeightOffsetAt(x * .013f, z * .011f)) < 0.000002f);
    const float x = -.037f, z = -.023f, h = 1e-4f;
    const auto gradient = mud.HeightGradientAt(x, z);
    assert(std::abs(gradient[0] - (mud.HeightOffsetAt(x+h, z) - mud.HeightOffsetAt(x-h, z))/(2*h)) < .001f);
    assert(std::abs(gradient[1] - (mud.HeightOffsetAt(x, z+h) - mud.HeightOffsetAt(x, z-h))/(2*h)) < .001f);

    // Drying and disabled rendering are not erasure; reset is explicit.
    mud.Advance(180, 0);
    assert(mud.Wetness() < .25f && mud.HeightOffsetAt(-.0625f, -.0625f) == -MudField::MaxDepth);
    mud.enabled = false;
    const float wet = mud.Wetness(); mud.Advance(180, 1);
    assert(mud.Wetness() == wet && mud.HeightOffsetAt(-.0625f, -.0625f) == 0);
    assert(mud.Snapshot(0, 0)[3] == 0 && !mud.Stamp(0, 0, .125f, .06f));
    mud.enabled = true;
    assert(mud.HeightOffsetAt(-.0625f, -.0625f) == -MudField::MaxDepth);
    mud.Reset();
    assert(mud.Wetness() == 0 && mud.Chunks() == 0 && mud.HeightOffsetAt(-.0625f, -.0625f) == 0);

    // Bound retained allocation: reject new chunks without camera-driven eviction.
    for (size_t i = 0; i < MudField::MaxChunks; ++i)
        assert(mud.Stamp(float(i) * 2 + .0625f, .0625f, .125f, .03f));
    assert(mud.Chunks() == MudField::MaxChunks);
    assert(!mud.Stamp(-100, -100, .125f, .06f) && mud.Rejected() > 0);
    assert(mud.HeightOffsetAt(.0625f, .0625f) == -.03f);
    assert(mud.Stamp(.0625f, .0625f, .125f, .06f)); // existing chunk still writable at cap


    // Real pressure contacts: rain creates capacity but untouched soil stays put.
    MudField deep;
    assert(deep.LayerDepth() == .24f && !deep.CompressBoot(.0625f, .0625f));
    assert(!deep.SetLayerDepth(nan) && !deep.SetLayerDepth(.351f) && !deep.SetLayerDepth(.01f));
    deep.Advance(70, 1);
    assert(deep.Wetness() > .9f && deep.ContactDepth() > .23f && deep.HeightOffsetAt(0,0) == 0);
    assert(deep.CompressBoot(.0625f, .0625f));
    const float first = -deep.HeightOffsetAt(.0625f, .0625f);
    assert(first >= .1f && first < .24f);
    for (int i=0; i<100; ++i) deep.CompressBoot(.0625f, .0625f);
    const float loaded = -deep.HeightOffsetAt(.0625f, .0625f);
    assert(loaded > .2f && loaded <= .24f && loaded > first);
    // Different overlapping steps cannot produce a sharp unbounded physical rim.
    for (int i=0; i<40; ++i) deep.CompressBoot(.0625f+i*.027f, .0625f-i*.011f);
    for (int z=-100; z<=100; ++z) for (int x=-100; x<=100; ++x) {
        const auto g=deep.HeightGradientAt(x*.017f,z*.013f);
        assert(std::isfinite(g[0]) && std::isfinite(g[1]));
        assert(std::abs(g[0]) <= MudField::MaxCellSlope+.00001f);
        assert(std::abs(g[1]) <= MudField::MaxCellSlope+.00001f);
        assert(deep.HeightOffsetAt(x*.017f,z*.013f) >= -.24f);
    }
    const auto retained = deep.Snapshot(0,0);
    assert(retained[3] == MudField::MaxDepth);
    assert(deep.SetLayerDepth(.05f));
    assert(deep.HeightOffsetAt(.0625f,.0625f) == -loaded);
    deep.Advance(2000,0);
    assert(!deep.ContactActive() && !deep.CompressBoot(.0625f,.0625f));
    assert(deep.HeightOffsetAt(.0625f,.0625f) == -loaded);
    assert(deep.Snapshot(0,0) == retained); // configuration/drying never clip old geometry
    MudField shallow; shallow.Advance(8,1);
    assert(shallow.ContactDepth() > 0 && shallow.ContactDepth() < .01f);
    assert(shallow.CompressBoot(.0625f,.0625f));
    assert(-shallow.HeightOffsetAt(.0625f,.0625f) < first);
    MudField configured;
    assert(configured.SetLayerDepth(.35f)); configured.Advance(1000,1);
    assert(!configured.CompressBoot(0,0,nan) && !configured.CompressBoot(0,0,0));
    for (int i=0; i<100; ++i) configured.CompressBoot(.0625f,.0625f);
    assert(-configured.HeightOffsetAt(.0625f,.0625f) > .34f);
    assert(-configured.HeightOffsetAt(.0625f,.0625f) <= MudField::MaxDepth);
    assert(configured.HeightOffsetAt(10,10) == 0); // rain alone never lowers the map

    assert(StockMudSoil("Field", "pol", "dirt", ""));
    assert(!StockMudSoil("Field", "pol", "dirt", "crops"));
    assert(!StockMudSoil("Field", "pol", "grass", ""));
    assert(!StockMudSoil("Field", "unknown", "dirt", ""));
    for (const auto source : {"Grass", "GrassHigh", "Village", "MudBuilding", "Asphalt", "", "Soil"})
        assert(!StockMudSoil(source, "pol", "dirt", ""));
    // Real Noe source evidence: exact retail cultivated texture +
    // unchanged Default metadata. Neither Default nor a "pole" name is enough.
    for (const auto texture : {"o/pole1.paa", "o\\pole2.paa", "\\O\\POLE1.PAA"})
        assert(StockNogovaMudSoil("\\Noe\\Noe.wrp", texture, "Default", "default", "normalExt", "", .5f, .5f));
    for (const auto texture : {"pole1.paa", "mod/o/pole1.paa", "o/pole3.paa", "o/POLE1.PAA.bak",
                               "o/b1.paa", "o/blita2.paa", "o/pt.paa", "o/l1.paa", "o/ces_hned.paa"})
        assert(!StockNogovaMudSoil("noe/noe.wrp", texture, "Default", "default", "normalExt", "", .5f, .5f));
    for (const auto world : {"", "worlds/noe.wrp", "noe/custom.wrp", "worlds/abel.wrp"})
        assert(StockNogovaMudSoil(world, "o/pole2.paa", "Default", "default", "normalExt", "", .5f, .5f));
    assert(StockCultivatedMudSoil("o/pole2.paa", "Default", "default", "normalExt", "", .01f, .99f));
    for (const auto source : {"GrassGrey", "Grass", "Village", "Lom", "MudBuilding", "", "Dirt"})
        assert(!StockNogovaMudSoil("noe/noe.wrp", "o/pole2.paa", source, "default", "normalExt", "", .5f, .5f));
    assert(!StockNogovaMudSoil("noe/noe.wrp", "o/pole2.paa", "Default", "pole2", "normalExt", "", .5f, .5f));
    for (const auto sound : {"grass", "road", "rock", "sand", "dirt", ""})
        assert(!StockNogovaMudSoil("noe/noe.wrp", "o/pole2.paa", "Default", "default", sound, "", .5f, .5f));
    assert(!StockNogovaMudSoil("noe/noe.wrp", "o/pole2.paa", "Default", "default", "normalExt", "Grass", .5f, .5f));
    for (const auto uv : {-.1f, 1.0f, nan, inf})
    {
        assert(!StockNogovaMudSoil("noe/noe.wrp", "o/pole2.paa", "Default", "default", "normalExt", "", uv, .5f));
        assert(!StockNogovaMudSoil("noe/noe.wrp", "o/pole2.paa", "Default", "default", "normalExt", "", .5f, uv));
    }
    // The production nine support probes include the entire boot kernel. A
    // centre exactly at the soil admission edge must fail the outer probes.
    auto allBootProbes = [](float u, float v) {
        for (int z = -1; z <= 1; ++z)
            for (int x = -1; x <= 1; ++x)
                if (!StockNogovaMudSoil("noe/noe.wrp", "o/pole2.paa", "Default", "default", "normalExt", "",
                                       u + x * (MudField::MaxContactRadius+.075f+MudField::CellSize) / 50.0f, v + z * (MudField::MaxContactRadius+.075f+MudField::CellSize) / 50.0f)) return false;
        return true;
    };
    assert(allBootProbes(.5f, .5f));
    assert(allBootProbes(.12f, .88f));
    assert(!allBootProbes(.0f, .5f) && !allBootProbes(.5f, 1.0f));

}
