#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Poseidon/World/Terrain/TerrainCdlod.hpp>
#include <Poseidon/World/Terrain/TerrainSatmap.hpp>
#include <Poseidon/World/Weather/SnowField.hpp>

#include <cmath>
#include <limits>
#include <vector>

using namespace Poseidon;

TEST_CASE("ocean continuation reach includes the far frustum corners", "[cdlod][water]")
{
    CHECK(CdlodFrustumReach(50000,0,0) == 50000);
    CHECK(CdlodFrustumReach(50000,1,1) == Catch::Approx(50000*std::sqrt(3.0f)));
    const float ultraWide = CdlodFrustumReach(50000,2,0.75f);
    CHECK(ultraWide > 100000);
    CHECK(ultraWide*ultraWide == Catch::Approx(50000.0f*50000.0f + 100000.0f*100000.0f + 37500.0f*37500.0f));
}

TEST_CASE("automatic satellite blend keeps constant color and world axes", "[terrain][satmap]")
{
    const auto constant = [](int,int,float,float) { return std::array<float,3>{12,87,190}; };
    for (float x : {-0.1f,0.0f,0.4f,0.5f,0.9f,1.0f,25.13f})
    {
        const auto c = SampleTerrainSatmap(x,0.7f,constant);
        CHECK(std::abs(c[0]-12) < 0.001f);
        CHECK(std::abs(c[1]-87) < 0.001f);
        CHECK(std::abs(c[2]-190) < 0.001f);
    }
    const auto axes = [](int x,int z,float,float) { return std::array<float,3>{float(x),0,float(z)}; };
    const auto centre = SampleTerrainSatmap(3.5f,7.5f,axes);
    CHECK(centre[0] == 3);
    CHECK(centre[2] == 7);
    const auto edge = SampleTerrainSatmap(4.0f,8.0f,axes);
    CHECK(edge[0] == 3.5f);
    CHECK(edge[2] == 7.5f);
}

TEST_CASE("Experimental snow persists until new precipitation fills tracks", "[snow][terrain]")
{
    SnowField snow;
    CHECK_FALSE(snow.enabled);
    snow.enabled = true;
    snow.Deposit(0.3f);
    snow.Stamp(0.0625f, 0.0625f, 0.3f, 1.0f);
    REQUIRE(snow.DeficitAt(0, 0) == Catch::Approx(0.3f));
    snow.falling = false;
    for (int i = 0; i < 600; ++i) snow.Advance(1.0f);
    CHECK(snow.DeficitAt(0, 0) == Catch::Approx(0.3f));
    const auto before = snow.Snapshot(0, 0);
    const auto elsewhere = snow.Snapshot(90000, -90000);
    CHECK(snow.Snapshot(0, 0) == before);
    snow.Deposit(0.1f);
    CHECK(snow.DeficitAt(0, 0) == Catch::Approx(0.2f));
    snow.Deposit(0.3f);
    CHECK(snow.DeficitAt(0, 0) == 0.0f);
    CHECK(snow.Depth() == Catch::Approx(0.5f));
    snow.Stamp(-0.0625f, -0.0625f, 0.2f);
    CHECK(snow.DeficitAt(-1, -1) > 0.0f);
    snow.enabled = false;
    CHECK(snow.Snapshot(0, 0)[3] == 0.0f);
    snow.enabled = true;
    CHECK(snow.DeficitAt(-1, -1) > 0.0f);
    snow.Reset();
    CHECK(snow.Chunks() == 0);
    CHECK(snow.Depth() == 0.0f);
}

TEST_CASE("Snow tracks interpolate travel and ignore teleports", "[snow][terrain]")
{
    SnowField snow;
    snow.enabled = true;
    snow.Deposit(0.2f);
    snow.Trail(0, 0, 10, 0, 0.2f);
    for (int x = 0; x < 80; ++x) CHECK(snow.DeficitAt(x, 0) > 0.0f);
    const auto count = snow.Chunks();
    snow.Trail(0, 0, 10000, 0, 0.2f);
    CHECK(snow.Chunks() == count);
    snow.Stamp(2, 2, 3, 1.0f);
    CHECK(snow.DeficitAt(16, 16) == Catch::Approx(0.2f));
    snow.Deposit(0.1f);
    CHECK(snow.DeficitAt(16, 16) == Catch::Approx(0.1f));
}

TEST_CASE("Snow rejects invalid inputs and snowfall fills even at maximum depth", "[snow][terrain]")
{
    SnowField snow;
    snow.enabled = true;
    snow.Deposit(0.5f);
    snow.Stamp(0.0625f, 0.0625f, 0.3f, 1.0f);
    snow.metresPerMinute = 0.1f;
    for (int i = 0; i < 60; ++i) snow.Advance(1.0f);
    CHECK(snow.Depth() == Catch::Approx(0.5f));
    CHECK(snow.DeficitAt(0, 0) == Catch::Approx(0.4f));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const auto count = snow.Chunks();
    snow.Trail(0, 0, 1, 1, nan);
    snow.Stamp(nan, 0, 1);
    snow.Deposit(nan);
    CHECK(snow.Chunks() == count);
    CHECK(snow.Depth() == Catch::Approx(0.5f));
    CHECK(snow.Snapshot(nan, 0)[3] == 0.0f);
    CHECK(snow.Snapshot(1e30f, 0)[3] == 0.0f);
}

TEST_CASE("Snow storage cap never evicts existing depressions", "[snow][terrain]")
{
    SnowField snow;
    snow.enabled = true;
    snow.Deposit(0.2f);
    for (size_t i = 0; i < SnowField::MaxChunks; ++i)
        snow.Stamp(float(i) * 2.0f + 0.5625f, 0.5625f, 0.125f);
    REQUIRE(snow.Chunks() == SnowField::MaxChunks);
    const float first = snow.DeficitAt(4, 4);
    REQUIRE(first > 0.0f);
    snow.Stamp(-20.0f, -20.0f, 1.0f);
    CHECK(snow.Chunks() == SnowField::MaxChunks);
    CHECK(snow.Rejected() > 0);
    CHECK(snow.DeficitAt(4, 4) == first);
}

namespace
{
constexpr float LeafSize = 200.0f; // 32 samples * 6.25 m, Nogova-like

// A full quadtree over a flat (y = 0) square, mirroring TerrainWgpu::BuildQuadtree
// but engine-free: level 0 leaves of LeafSize, root at level numLevels-1.
int BuildFlatTree(std::vector<CdlodNode>& tree, float ox, float oz, float size, int level)
{
    CdlodNode n{};
    n.originX = ox;
    n.originZ = oz;
    n.size = size;
    n.minY = 0.0f;
    n.maxY = 0.0f;
    n.level = level;
    n.child[0] = n.child[1] = n.child[2] = n.child[3] = -1;

    if (level > 0)
    {
        const float h = size * 0.5f;
        n.child[0] = BuildFlatTree(tree, ox, oz, h, level - 1);
        n.child[1] = BuildFlatTree(tree, ox + h, oz, h, level - 1);
        n.child[2] = BuildFlatTree(tree, ox, oz + h, h, level - 1);
        n.child[3] = BuildFlatTree(tree, ox + h, oz + h, h, level - 1);
    }

    const int idx = static_cast<int>(tree.size());
    tree.push_back(n);
    return idx;
}

int GeomLevel(float size)
{
    return static_cast<int>(std::lround(std::log2(size / LeafSize)));
}

bool EdgeAdjacent(const CdlodSelection& a, const CdlodSelection& b)
{
    const float ax1 = a.originX + a.size, az1 = a.originZ + a.size;
    const float bx1 = b.originX + b.size, bz1 = b.originZ + b.size;
    const float zOverlap = std::min(az1, bz1) - std::max(a.originZ, b.originZ);
    const float xOverlap = std::min(ax1, bx1) - std::max(a.originX, b.originX);
    const bool touchX = (ax1 == b.originX || bx1 == a.originX) && zOverlap > 0.0f;
    const bool touchZ = (az1 == b.originZ || bz1 == a.originZ) && xOverlap > 0.0f;
    return touchX || touchZ;
}

// CPU mirror of terrain.wgsl's coarse-lattice snap.
float SnapCoarse(float g, int gridN)
{
    const float gidx = g * static_cast<float>(gridN);
    return (std::round(gidx * 0.5f) * 2.0f) / static_cast<float>(gridN);
}
} // namespace

TEST_CASE("CDLOD ranges form a geometric ladder", "[terrain][cdlod]")
{
    std::vector<float> ranges;
    ComputeCdlodRanges(600.0f, 2.0f, 5, ranges);
    REQUIRE(ranges.size() == 5);
    REQUIRE(ranges[0] == Catch::Approx(600.0f));
    REQUIRE(ranges[1] == Catch::Approx(1200.0f));
    REQUIRE(ranges[4] == Catch::Approx(9600.0f));
}

TEST_CASE("Snow refinement partitions the parent and resolves foot depressions", "[terrain][cdlod][snow]")
{
    const CdlodSelection parent{-800, -800, 1600, 0, 100, 200};
    std::vector<CdlodSelection> patches;
    RefineCdlodSnow(parent, 0, 0, 8, [&](const auto& patch) { patches.push_back(patch); });
    double area = 0;
    for (const auto& patch : patches)
    {
        area += double(patch.size) * patch.size;
        REQUIRE(patch.originX >= parent.originX);
        REQUIRE(patch.originZ >= parent.originZ);
        REQUIRE(patch.originX + patch.size <= parent.originX + parent.size);
        REQUIRE(patch.originZ + patch.size <= parent.originZ + parent.size);
        if (patch.originX < 8 && patch.originZ < 8 && patch.originX + patch.size > -8 && patch.originZ + patch.size > -8)
            CHECK(patch.size / 32 <= 0.125f);
    }
    CHECK(area == Catch::Approx(1600.0 * 1600.0));
    CHECK(patches.size() < 512);
    for (size_t i = 0; i < patches.size(); ++i)
        for (size_t j = i + 1; j < patches.size(); ++j)
            CHECK_FALSE((patches[i].originX < patches[j].originX + patches[j].size &&
                        patches[j].originX < patches[i].originX + patches[i].size &&
                        patches[i].originZ < patches[j].originZ + patches[j].size &&
                        patches[j].originZ < patches[i].originZ + patches[i].size));
}

TEST_CASE("Procedural ocean selection matches a stored flat CDLOD tree", "[terrain][cdlod][ocean]")
{
    std::vector<CdlodNode> tree;
    const int root = BuildFlatTree(tree, -3200.0f, 6400.0f, LeafSize * 32, 5);
    std::vector<float> ranges;
    ComputeCdlodRanges(LeafSize * 4, 2.0f, 6, ranges);
    for (float x : {-10000.0f, -3200.0f, 0.0f, 3200.0f, 40000.0f})
    {
        for (float y : {0.0f, 20.0f, 8000.0f})
        {
            std::vector<CdlodSelection> stored, lazy;
            auto visible = [](const CdlodNode& n) { return n.originZ + n.size > 7000.0f; };
            const bool a = SelectCdlod(tree, root, 5, x, y, 7000.0f, ranges, 0.25f, visible,
                [&](const CdlodSelection& s) { stored.push_back(s); });
            const bool b = SelectFlatCdlod(tree[root], x, y, 7000.0f, ranges, 0.25f, visible,
                [&](const CdlodSelection& s) { lazy.push_back(s); });
            REQUIRE(a == b);
            REQUIRE(stored.size() == lazy.size());
            for (size_t i = 0; i < stored.size(); ++i)
            {
                CHECK(stored[i].originX == lazy[i].originX);
                CHECK(stored[i].originZ == lazy[i].originZ);
                CHECK(stored[i].size == lazy[i].size);
                CHECK(stored[i].level == lazy[i].level);
                CHECK(stored[i].morphStart == lazy[i].morphStart);
                CHECK(stored[i].morphEnd == lazy[i].morphEnd);
            }
        }
    }
}

TEST_CASE("CDLOD node distance is zero inside and Euclidean outside", "[terrain][cdlod]")
{
    CdlodNode n{};
    n.originX = 0.0f;
    n.originZ = 0.0f;
    n.size = 100.0f;
    n.minY = -10.0f;
    n.maxY = 10.0f;

    REQUIRE(CdlodNodeDistanceSq(n, 50.0f, 0.0f, 50.0f) == Catch::Approx(0.0f));
    // 3-4-5 offset off the +x/+z corner, within the y slab.
    REQUIRE(CdlodNodeDistanceSq(n, 103.0f, 0.0f, 104.0f) == Catch::Approx(25.0f));
    // Purely above the top face.
    REQUIRE(CdlodNodeDistanceSq(n, 50.0f, 15.0f, 50.0f) == Catch::Approx(25.0f));
}

TEST_CASE("CDLOD morph band ends at the level range", "[terrain][cdlod]")
{
    std::vector<float> ranges;
    ComputeCdlodRanges(600.0f, 2.0f, 5, ranges);

    float ms = 0.0f, me = 0.0f;
    CdlodMorphBand(ranges, 2, 0.3f, ms, me);
    REQUIRE(me == Catch::Approx(ranges[2]));
    REQUIRE(ms < me);
    REQUIRE(ms == Catch::Approx(ranges[2] - (ranges[2] - ranges[1]) * 0.3f));

    // Level 0 morphs against a zero inner range.
    CdlodMorphBand(ranges, 0, 0.3f, ms, me);
    REQUIRE(me == Catch::Approx(ranges[0]));
    REQUIRE(ms == Catch::Approx(ranges[0] * 0.7f));
}

TEST_CASE("CDLOD morph mixes fine at k=0 and coarse at k=1", "[terrain][cdlod]")
{
    constexpr int gridN = 32;
    // An odd grid line that the coarse lattice must snap to its even neighbour.
    const float g = 5.0f / gridN;
    REQUIRE(SnapCoarse(g, gridN) == Catch::Approx(6.0f / gridN));

    const float origin = 100.0f, size = 400.0f;
    const float fine = origin + g * size;
    const float coarse = origin + SnapCoarse(g, gridN) * size;
    // mix(fine, coarse, k)
    REQUIRE((fine + (coarse - fine) * 0.0f) == Catch::Approx(fine));
    REQUIRE((fine + (coarse - fine) * 1.0f) == Catch::Approx(coarse));
}

TEST_CASE("CDLOD selection stays visible and morphs at each patch's level", "[terrain][cdlod]")
{
    constexpr int numLevels = 6; // root size = 200 * 2^5 = 6400
    const float rootSize = LeafSize * static_cast<float>(1 << (numLevels - 1));

    std::vector<CdlodNode> tree;
    const int root = BuildFlatTree(tree, 0.0f, 0.0f, rootSize, numLevels - 1);

    std::vector<float> ranges;
    ComputeCdlodRanges(LeafSize * 3.0f, 2.0f, numLevels, ranges);

    // Stand-in frustum: a node is visible when any part is in front of the camera.
    const float camX = rootSize * 0.5f, camY = 5.0f, camZ = rootSize * 0.5f;
    auto visible = [&](const CdlodNode& n) -> bool { return (n.originX + n.size) >= camX; };

    std::vector<CdlodSelection> emitted;
    auto emit = [&](const CdlodSelection& s) { emitted.push_back(s); };

    SelectCdlod(tree, root, numLevels - 1, camX, camY, camZ, ranges, 0.3f, visible, emit);

    REQUIRE(!emitted.empty());

    for (const auto& s : emitted)
    {
        // Nothing entirely behind the camera should be emitted.
        REQUIRE((s.originX + s.size) >= camX);
        REQUIRE(s.morphEnd > s.morphStart);
        // A patch's morph band must match its own geometry level, or it draws
        // un-morphed against coarser neighbours and cracks.
        REQUIRE(GeomLevel(s.size) == s.level);
        float ms = 0.0f, me = 0.0f;
        CdlodMorphBand(ranges, s.level, 0.3f, ms, me);
        REQUIRE(s.morphStart == Catch::Approx(ms));
        REQUIRE(s.morphEnd == Catch::Approx(me));
    }

    for (size_t i = 0; i < emitted.size(); i++)
    {
        for (size_t j = i + 1; j < emitted.size(); j++)
        {
            if (EdgeAdjacent(emitted[i], emitted[j]))
            {
                REQUIRE(std::abs(GeomLevel(emitted[i].size) - GeomLevel(emitted[j].size)) <= 1);
            }
        }
    }
}

TEST_CASE("Ocean reach follows sea intersections rather than the land far plane", "[terrain][cdlod]")
{
    REQUIRE(CdlodSeaRayReach(40000, 0, -1, 0, 300000) == 40000);
    REQUIRE(CdlodSeaRayReach(40000, 0, -1, 1, 300000) > 56000);
    REQUIRE(CdlodSeaRayReach(40000, 0, -0.01f, 1, 300000) == 300000);
    REQUIRE(CdlodSeaRayReach(40000, 0, 1, 1, 300000) == 300000);
    REQUIRE(CdlodSeaRayReach(40000, 1, 0, 0, 300000) == 300000);
    REQUIRE(CdlodSeaRayReach(-10, 0, 1, 0, 300000) == 10);
}

TEST_CASE("CDLOD refines near the camera and coarsens with distance", "[terrain][cdlod]")
{
    constexpr int numLevels = 6;
    const float rootSize = LeafSize * static_cast<float>(1 << (numLevels - 1));

    std::vector<CdlodNode> tree;
    const int root = BuildFlatTree(tree, 0.0f, 0.0f, rootSize, numLevels - 1);

    std::vector<float> ranges;
    ComputeCdlodRanges(LeafSize * 3.0f, 2.0f, numLevels, ranges);

    const float camX = 0.0f, camY = 2.0f, camZ = 0.0f;
    auto visible = [&](const CdlodNode&) -> bool { return true; };

    std::vector<CdlodSelection> emitted;
    auto emit = [&](const CdlodSelection& s) { emitted.push_back(s); };
    SelectCdlod(tree, root, numLevels - 1, camX, camY, camZ, ranges, 0.3f, visible, emit);

    auto levelNear = [&](float px, float pz) -> int
    {
        int best = 1000;
        for (const auto& s : emitted)
        {
            if (px >= s.originX && px < s.originX + s.size && pz >= s.originZ && pz < s.originZ + s.size)
            {
                best = std::min(best, GeomLevel(s.size));
            }
        }
        return best;
    };

    // The tile under the camera is finer than a tile far across the map.
    REQUIRE(levelNear(1.0f, 1.0f) < levelNear(rootSize - 1.0f, rootSize - 1.0f));
}
