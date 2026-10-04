#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Terrain/TerrainCdlod.hpp>
#include <algorithm>
#include <limits>
#include <vector>

using namespace Poseidon;

TEST_CASE("Terrain background partitions the same authored tree without changing near LOD", "[terrain][cdlod][terrain-background]")
{
    // Actual original-island native sample spacing/range and renderer tree policy.
    // Deterministic hill heights provide bounds; this is not a retail map/pixel proof.
    constexpr int range = 2048, leaf = 32;
    constexpr float grid = 6.25f;
    const int rootTexels = CdlodRootTexels(range * 3, leaf);
    const int origin = CdlodCenteredOrigin(rootTexels, range, leaf);
    std::vector<CdlodNode> tree;
    int root, levels; float leafSize;
    auto bounds = [&](int x, int z, int span, float& mn, float& mx) {
        CdlodHeightBounds(x, z, span, range, false,
            [](int sz, int sx) { return float((sx * 13 + sz * 7) % 300); }, mn, mx);
    };
    BuildCdlodTree(rootTexels, origin, origin, grid, leaf, bounds, tree, root, levels, leafSize);
    std::vector<float> ranges;
    ComputeCdlodRanges(leafSize * 4, 2, levels, ranges);
    const float treeMin = origin * grid, treeMax = (origin + rootTexels) * grid;
    const float oldRect[] = {4000, 4000, 8000, 8000};
    for (float height : {5.0f, 1200.0f, 5000.0f})
    {
        std::vector<CdlodSelection> old, background;
        const auto select = [&](const float* rect, auto& result) {
            SelectCdlod(tree, root, levels - 1, 6000, height, 6000, ranges, .5f,
                [&](const CdlodNode& node) { return CdlodIntersectsRect(node, rect[0], rect[1], rect[2], rect[3]); },
                [&](const CdlodSelection& node) { result.push_back(node); });
        };
        const float wholeTree[] = {treeMin, treeMin, treeMax, treeMax};
        select(oldRect, old); select(wholeTree, background);
        REQUIRE(!old.empty());
        REQUIRE(background.size() < 1024); // Distance ladder remains bounded.
        REQUIRE(background.size() > old.size());
        for (const auto& near : old)
        {
            const auto exact = std::find_if(background.begin(), background.end(), [&](const auto& far) {
                return near.originX == far.originX && near.originZ == far.originZ && near.size == far.size &&
                    near.level == far.level && near.morphStart == far.morphStart && near.morphEnd == far.morphEnd;
            });
            CHECK(exact != background.end());
        }
        double area = 0;
        for (size_t i = 0; i < background.size(); ++i)
        {
            const auto& a = background[i]; area += double(a.size) * a.size;
            CHECK(a.originX >= treeMin); CHECK(a.originZ >= treeMin);
            CHECK(a.originX + a.size <= treeMax); CHECK(a.originZ + a.size <= treeMax);
            CHECK(a.morphEnd == ranges[a.level]);
            for (size_t j = i + 1; j < background.size(); ++j)
            {
                const auto& b = background[j];
                CHECK_FALSE((a.originX < b.originX + b.size && a.originX + a.size > b.originX &&
                    a.originZ < b.originZ + b.size && a.originZ + a.size > b.originZ));
            }
        }
        CHECK(area == double(treeMax - treeMin) * (treeMax - treeMin));
        const auto covers = [&](float x, float z) {
            return std::count_if(background.begin(), background.end(), [&](const auto& p) {
                return x >= p.originX && x < p.originX + p.size && z >= p.originZ && z < p.originZ + p.size;
            });
        };
        CHECK(covers(12000, 12000) == 1); // Visible distant coast has opaque ownership.
        CHECK(covers(-1000, -1000) == 1); // Extended seabed continues across map edge.
    }
}

TEST_CASE("Terrain rectangle admission rejects invalid coverage and keeps edge ownership", "[terrain][cdlod][terrain-background]")
{
    CdlodNode node{}; node.originX = 10; node.originZ = 20; node.size = 200;
    CHECK(CdlodIntersectsRect(node, 10, 20, 210, 220));
    CHECK_FALSE(CdlodIntersectsRect(node, 210, 20, 410, 220));
    CHECK_FALSE(CdlodIntersectsRect(node, 10, 220, 210, 420));
    CHECK_FALSE(CdlodIntersectsRect(node, 10, 20, 10, 220));
    CHECK_FALSE(CdlodIntersectsRect(node, 20, 20, 10, 220));
    CHECK_FALSE(CdlodIntersectsRect(node, 10, 20, INFINITY, 220));
    CHECK_FALSE(CdlodIntersectsRect(node, 10, std::numeric_limits<float>::quiet_NaN(), 210, 220));
    node.size = -1; CHECK_FALSE(CdlodIntersectsRect(node, 10, 20, 210, 220));
    node.size = INFINITY; CHECK_FALSE(CdlodIntersectsRect(node, 10, 20, 210, 220));
}
