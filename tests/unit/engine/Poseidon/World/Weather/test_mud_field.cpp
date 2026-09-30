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
    assert(mud.ContactActive() && mud.ContactDepth() > 0.02f && mud.ContactDepth() < 0.06f);
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
    assert(mud.HeightOffsetAt(-.0625f, -.0625f) == -MudField::MaxDepth);
    const auto revision = mud.Revision();
    assert(!mud.Stamp(-.0625f, -.0625f, .125f, 1));
    assert(mud.Revision() == revision);
    assert(!mud.Stamp(-.0625f, -.0625f, .125f, .01f));
    assert(mud.HeightOffsetAt(100, 100) == 0);
    assert(mud.HeightOffsetAt(nan, 0) == 0);

    // Camera window reads do not mutate, and retain all world-space samples.
    const auto before = mud.Snapshot(0, 0);
    assert(before.size() == 4 + 512 * 512 && before[2] == .125f && before[3] == .06f);
    const auto distant = mud.Snapshot(200, 200);
    for (size_t i = 4; i < distant.size(); ++i) assert(distant[i] == 0);
    assert(mud.Snapshot(0, 0) == before && mud.Revision() == revision);
    for (int z = -40; z <= 40; ++z)
        for (int x = -40; x <= 40; ++x)
        {
            const float east = x * .015625f, north = z * .015625f;
            const float offset = mud.HeightOffsetAt(east, north);
            assert(offset >= -.06f && offset <= 0);
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
    assert(std::abs(gradient[0] - (mud.HeightOffsetAt(x+h, z) - mud.HeightOffsetAt(x-h, z))/(2*h)) < .0001f);
    assert(std::abs(gradient[1] - (mud.HeightOffsetAt(x, z+h) - mud.HeightOffsetAt(x, z-h))/(2*h)) < .0001f);

    // Drying and disabled rendering are not erasure; reset is explicit.
    mud.Advance(180, 0);
    assert(mud.Wetness() < .25f && mud.HeightOffsetAt(-.0625f, -.0625f) == -.06f);
    mud.enabled = false;
    const float wet = mud.Wetness(); mud.Advance(180, 1);
    assert(mud.Wetness() == wet && mud.HeightOffsetAt(-.0625f, -.0625f) == 0);
    assert(mud.Snapshot(0, 0)[3] == 0 && !mud.Stamp(0, 0, .125f, .06f));
    mud.enabled = true;
    assert(mud.HeightOffsetAt(-.0625f, -.0625f) == -.06f);
    mud.Reset();
    assert(mud.Wetness() == 0 && mud.Chunks() == 0 && mud.HeightOffsetAt(-.0625f, -.0625f) == 0);

    // Bound retained allocation: reject new chunks without camera-driven eviction.
    for (size_t i = 0; i < MudField::MaxChunks; ++i)
        assert(mud.Stamp(float(i) * 2 + .0625f, .0625f, .125f, .03f));
    assert(mud.Chunks() == MudField::MaxChunks);
    assert(!mud.Stamp(-100, -100, .125f, .06f) && mud.Rejected() > 0);
    assert(mud.HeightOffsetAt(.0625f, .0625f) == -.03f);
    assert(mud.Stamp(.0625f, .0625f, .125f, .06f)); // existing chunk still writable at cap

    assert(StockMudSoil("Field", "pol", "dirt", ""));
    assert(!StockMudSoil("Field", "pol", "dirt", "crops"));
    assert(!StockMudSoil("Field", "pol", "grass", ""));
    assert(!StockMudSoil("Field", "unknown", "dirt", ""));
    for (const auto source : {"Grass", "GrassHigh", "Village", "MudBuilding", "Asphalt", "", "Soil"})
        assert(!StockMudSoil(source, "pol", "dirt", ""));
    // Real Noe source evidence: exact retail world + cultivated texture +
    // unchanged Default metadata. Neither Default nor a "pole" name is enough.
    for (const auto texture : {"o/pole1.paa", "o\\pole2.paa", "\\O\\POLE1.PAA"})
        assert(StockNogovaMudSoil("\\Noe\\Noe.wrp", texture, "Default", "default", "normalExt", "", .5f, .5f));
    for (const auto texture : {"pole1.paa", "mod/o/pole1.paa", "o/pole3.paa", "o/POLE1.PAA.bak",
                               "o/b1.paa", "o/blita2.paa", "o/pt.paa", "o/l1.paa", "o/ces_hned.paa"})
        assert(!StockNogovaMudSoil("noe/noe.wrp", texture, "Default", "default", "normalExt", "", .5f, .5f));
    for (const auto world : {"", "worlds/noe.wrp", "noe/custom.wrp", "worlds/abel.wrp"})
        assert(!StockNogovaMudSoil(world, "o/pole2.paa", "Default", "default", "normalExt", "", .5f, .5f));
    for (const auto source : {"GrassGrey", "Grass", "Village", "Lom", "MudBuilding", "", "Dirt"})
        assert(!StockNogovaMudSoil("noe/noe.wrp", "o/pole2.paa", source, "default", "normalExt", "", .5f, .5f));
    assert(!StockNogovaMudSoil("noe/noe.wrp", "o/pole2.paa", "Default", "pole2", "normalExt", "", .5f, .5f));
    for (const auto sound : {"grass", "road", "rock", "sand", "dirt", ""})
        assert(!StockNogovaMudSoil("noe/noe.wrp", "o/pole2.paa", "Default", "default", sound, "", .5f, .5f));
    assert(!StockNogovaMudSoil("noe/noe.wrp", "o/pole2.paa", "Default", "default", "normalExt", "Grass", .5f, .5f));
    for (const auto uv : {-.1f, .0f, .119f, .881f, 1.0f, nan, inf})
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
                                       u + x * .4f / 50.0f, v + z * .4f / 50.0f)) return false;
        return true;
    };
    assert(allBootProbes(.5f, .5f));
    assert(!allBootProbes(.12f, .5f) && !allBootProbes(.5f, .88f));

}
