#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <Poseidon/World/Terrain/LandscapeLod.hpp>
#include <Poseidon/World/Terrain/TerrainSubdivision.hpp>
#include <Poseidon/World/Terrain/TerrainCrater.hpp>
#include <Poseidon/World/Terrain/TerrainSatmap.hpp>
#include <Poseidon/Dev/Diag/TerrainBrush.hpp>
#include <unordered_set>
#include <vector>
#include <stdlib.h>
#include <cmath>
#include <utility>
#include <limits>

using namespace Poseidon;

TEST_CASE("Satellite top-view raster handles winding and rejects vertical faces", "[terrain-satmap]")
{
    std::array<float,3> w{};
    REQUIRE(SatmapTriangleWeights({0,0},{2,0},{0,2},0.5f,0.5f,w));
    CHECK(w[0] == Catch::Approx(0.5f));
    CHECK(w[1] == Catch::Approx(0.25f));
    CHECK(w[2] == Catch::Approx(0.25f));
    REQUIRE(SatmapTriangleWeights({0,0},{0,2},{2,0},0.5f,0.5f,w));
    CHECK_FALSE(SatmapTriangleWeights({0,0},{2,0},{0,2},1.5f,1.5f,w));
    CHECK_FALSE(SatmapTriangleWeights({0,0},{0,0},{0,2},0,0,w));
}

TEST_CASE("Ballistic craters restore normal depth and scale by ammo blast", "[terrain-crater]")
{
    const Dev::RocketCraterSettings defaults;
    CHECK(defaults.enabled);
    CHECK(defaults.radius == 12.0f);
    CHECK(defaults.depth == 1.0f);
    CHECK(BallisticCraterScale(150) == Catch::Approx(1));
    CHECK(BallisticCraterScale(1200) == Catch::Approx(2));
    CHECK(BallisticCraterScale(18.75f) == Catch::Approx(0.5));
    CHECK(BallisticCraterScale(300) > BallisticCraterScale(150));
    CHECK(BallisticCraterScale(1e30f) == 2);
    CHECK(BallisticCraterScale(0.001f) == 0.25f);
    CHECK(BallisticCraterScale(0) == 0);
    CHECK(BallisticCraterScale(-150) == 0);
    CHECK(BallisticCraterScale(std::numeric_limits<float>::infinity()) == 0);
    CHECK(BallisticCraterScale(std::numeric_limits<float>::quiet_NaN()) == 0);
}

TEST_CASE("Terrain brush wheel supports fractional steps and bounded fine radii", "[terrain-brush-radius]")
{
    CHECK(TerrainBrushWheelRadius(8, -1) < 8);
    CHECK(TerrainBrushWheelRadius(64, 0) == 64);
    CHECK(TerrainBrushWheelRadius(TerrainBrushWheelRadius(64, 0.5f), -0.5f) == Catch::Approx(64));
    CHECK(TerrainBrushWheelRadius(250, 10) == 250);
    CHECK(TerrainBrushWheelRadius(0.1f, -10) == 0.1f);
    CHECK(TerrainBrushWheelRadius(0.5f, -10) < 0.5f);
    CHECK(TerrainBrushWheelRadius(64, std::numeric_limits<float>::quiet_NaN()) == 64);
    CHECK(TerrainBrushWheelRadius(std::numeric_limits<float>::infinity(), 0) == 8);
    CHECK(TerrainBrushRadius(2, 1) == 2);
    CHECK(TerrainBrushRadius(0.5f, 6.25f) == 4.6875f);
}

TEST_CASE("Terrain brush preview radius matches the painted footprint", "[terrain-brush-radius]")
{
    CHECK(TerrainBrushRadius(8, 50) == 37.5f);
    CHECK(TerrainBrushRadius(64, 6.25f) == 64);
    CHECK(TerrainBrushRadius(1000, 6.25f) == 250);
    CHECK(TerrainBrushRadius(250, 200) == 250);
    for (float spacing : {6.25f, 25.0f, 50.0f, 200.0f})
    {
        const float radius = TerrainBrushRadius(8, spacing);
        CHECK(TerrainCraterDepth(radius, 0, radius, 4) == 0);
        CHECK(TerrainCraterDepth(radius+1, 0, radius, 4) == 0);
        CHECK(TerrainCraterDepth(radius*0.5f, 0, radius, 4) > 0);
        CHECK(TerrainBrushRadius(radius, spacing) == radius);
    }
}

TEST_CASE("Fine terrain brush reaches a vertex even at the cell centre", "[terrain-brush-radius]")
{
    for (float spacing : {1.0f, 6.25f, 25.0f, 50.0f, 200.0f})
    {
        const float radius = TerrainBrushRadius(0.5f, spacing);
        for (int z = 0; z <= 10; ++z)
            for (int x = 0; x <= 10; ++x)
            {
                const float dx = std::min(x, 10-x)*0.1f*spacing;
                const float dz = std::min(z, 10-z)*0.1f*spacing;
                CHECK(TerrainCraterDepth(dx, dz, radius, 1.0f) > 0.0f);
            }
    }
}

TEST_CASE("Large world refinement stays within the heightfield allocation budget", "[terrain-size]")
{
    CHECK(BoundedTerrainSubdivision(256, 4) == 4);
    CHECK(BoundedTerrainSubdivision(1024, 3) == 3);
    CHECK(BoundedTerrainSubdivision(1024, 4) == 3);
    CHECK(BoundedTerrainSubdivision(2048, 3) == 2);
    CHECK(BoundedTerrainSubdivision(4096, 3) == 1);
    CHECK(BoundedTerrainSubdivision(8192, 3) == 0);
    CHECK(BoundedTerrainSubdivision(16384, 3) == 0);
    CHECK(BoundedTerrainSubdivision(0, 3) == 0);
    CHECK(BoundedTerrainSubdivision(256, -1) == 0);
}

// LOD level computation

TEST_CASE("ComputeSegmentLod returns correct levels by distance", "[World][Terrain][LOD]")
{
    const float landGrid = 50.0f;
    const int maxN = 64;
    const int segX = 0, segZ = 0;
    // cacheRadius ~= 1805, nearDist ~= 596, midDist ~= 1192

    SECTION("camera on top of segment -> LOD 0 (full detail)")
    {
        REQUIRE(ComputeSegmentLod(segX, segZ, 200.0f, 200.0f, landGrid, maxN) == 0);
    }

    SECTION("camera at mid range -> LOD 1")
    {
        REQUIRE(ComputeSegmentLod(segX, segZ, 1000.0f, 200.0f, landGrid, maxN) == 1);
    }

    SECTION("camera far away -> LOD 2")
    {
        REQUIRE(ComputeSegmentLod(segX, segZ, 2200.0f, 200.0f, landGrid, maxN) == 2);
    }
}

TEST_CASE("ComputeSegmentLod is symmetric in all directions", "[World][Terrain][LOD]")
{
    const float landGrid = 50.0f;
    const int maxN = 64;
    const float center = (0 + LandSegmentSizeLod * 0.5f) * landGrid; // 200

    // Camera at same distance in different directions should give same LOD
    float dist = 1500.0f;
    int lodRight = ComputeSegmentLod(0, 0, center + dist, center, landGrid, maxN);
    int lodLeft = ComputeSegmentLod(0, 0, center - dist, center, landGrid, maxN);
    int lodUp = ComputeSegmentLod(0, 0, center, center + dist, landGrid, maxN);
    int lodDown = ComputeSegmentLod(0, 0, center, center - dist, landGrid, maxN);

    REQUIRE(lodRight == lodLeft);
    REQUIRE(lodRight == lodUp);
    REQUIRE(lodRight == lodDown);
}

TEST_CASE("ComputeSegmentLod monotonically increases with distance", "[World][Terrain][LOD]")
{
    const float landGrid = 50.0f;
    const int maxN = 64;
    const float segCX = (0 + LandSegmentSizeLod * 0.5f) * landGrid;

    int prevLod = 0;
    for (float d = 0; d < 5000.0f; d += 50.0f)
    {
        int lod = ComputeSegmentLod(0, 0, segCX + d, segCX, landGrid, maxN);
        REQUIRE(lod >= prevLod);
        REQUIRE(lod >= 0);
        REQUIRE(lod <= 2);
        prevLod = lod;
    }
}

TEST_CASE("ComputeSegmentLod at exact threshold boundaries", "[World][Terrain][LOD]")
{
    const float landGrid = 50.0f;
    const int maxN = 64;

    float nearDist, midDist;
    ComputeLodThresholds(landGrid, maxN, nearDist, midDist);

    const float segCX = (0 + LandSegmentSizeLod * 0.5f) * landGrid;
    const float segCZ = segCX;

    // Just inside near -> LOD 0
    REQUIRE(ComputeSegmentLod(0, 0, segCX + nearDist - 1.0f, segCZ, landGrid, maxN) == 0);
    // Just past near -> LOD 1
    REQUIRE(ComputeSegmentLod(0, 0, segCX + nearDist + 1.0f, segCZ, landGrid, maxN) == 1);
    // Just inside mid -> LOD 1
    REQUIRE(ComputeSegmentLod(0, 0, segCX + midDist - 1.0f, segCZ, landGrid, maxN) == 1);
    // Just past mid -> LOD 2
    REQUIRE(ComputeSegmentLod(0, 0, segCX + midDist + 1.0f, segCZ, landGrid, maxN) == 2);
}

TEST_CASE("ComputeSegmentLod handles different grid sizes", "[World][Terrain][LOD]")
{
    const int maxN = 64;

    // Smaller grid -> thresholds are proportionally smaller
    float nearSmall, midSmall, nearLarge, midLarge;
    ComputeLodThresholds(25.0f, maxN, nearSmall, midSmall);
    ComputeLodThresholds(100.0f, maxN, nearLarge, midLarge);

    REQUIRE(nearLarge > nearSmall);
    REQUIRE(midLarge > midSmall);
    // Ratio should be proportional to grid ratio
    REQUIRE(nearLarge / nearSmall == Catch::Approx(100.0f / 25.0f));
}

TEST_CASE("ComputeSegmentLod handles different cache sizes", "[World][Terrain][LOD]")
{
    const float landGrid = 50.0f;

    // Larger cache -> wider radius -> thresholds are further
    float nearSmall, midSmall, nearLarge, midLarge;
    ComputeLodThresholds(landGrid, 16, nearSmall, midSmall);
    ComputeLodThresholds(landGrid, 256, nearLarge, midLarge);

    REQUIRE(nearLarge > nearSmall);
    REQUIRE(midLarge > midSmall);
}

// LOD cache staleness regression tests
// Bug: LandCache::Segment() returned cached segments without checking if the
// cached _lodLevel still matches what ComputeSegmentLod() requires for the
// current camera position. Cache key is (xBeg, zBeg) only -- no LOD in key --
// so the cache MUST compare _lodLevel vs required LOD on every hit.
// Also: cache misses always generated at LOD 0 via GenerateSegment() instead
// of using the camera-distance-based LOD.
// Fixed in: Fill() (b2d036f0) and Segment() (62eebe7a).

TEST_CASE("Same segment needs different LOD when camera moves closer", "[World][Terrain][LOD][regression]")
{
    const float landGrid = 50.0f;
    const int maxN = 64;
    const int segX = 0, segZ = 0;

    // Segment cached at LOD 2 when camera is far
    int farLod = ComputeSegmentLod(segX, segZ, 5000.0f, 200.0f, landGrid, maxN);
    REQUIRE(farLod == 2);

    // Same segment position, camera moves close -- now needs LOD 0
    int closeLod = ComputeSegmentLod(segX, segZ, 200.0f, 200.0f, landGrid, maxN);
    REQUIRE(closeLod == 0);

    // The cache must detect this mismatch and regenerate.
    // Before the fix, Segment() would return the stale LOD 2 segment.
    REQUIRE(farLod != closeLod);
}

TEST_CASE("Same segment needs different LOD when camera moves farther", "[World][Terrain][LOD][regression]")
{
    const float landGrid = 50.0f;
    const int maxN = 64;
    const int segX = 0, segZ = 0;

    int closeLod = ComputeSegmentLod(segX, segZ, 200.0f, 200.0f, landGrid, maxN);
    REQUIRE(closeLod == 0);

    int farLod = ComputeSegmentLod(segX, segZ, 5000.0f, 200.0f, landGrid, maxN);
    REQUIRE(farLod == 2);

    REQUIRE(closeLod != farLod);
}

TEST_CASE("Cache key excludes LOD -- same position maps to same key regardless of LOD",
          "[World][Terrain][LOD][regression]")
{
    // LandSegKey only contains (x, z) -- no LOD component.
    // This means the cache MUST check _lodLevel on every hit, otherwise it
    // will return a segment with the wrong LOD for the current camera position.
    LandSegKeyLod keyA{0, 0};
    LandSegKeyLod keyB{0, 0};

    // Same keys even though the segment might need different LODs
    REQUIRE(keyA == keyB);

    LandSegKeyLodHash h;
    REQUIRE(h(keyA) == h(keyB));

    // Verify that different positions DO produce different keys
    LandSegKeyLod keyC{8, 0};
    REQUIRE_FALSE(keyA == keyC);
}

TEST_CASE("All segments in view can have different LODs for same cache key pattern",
          "[World][Terrain][LOD][regression]")
{
    // Simulate a real scenario: camera at center, segments at increasing distances
    // all need different LODs but share the same key structure.
    const float landGrid = 50.0f;
    const int maxN = 64;
    const float camX = 400.0f, camZ = 400.0f;

    int lodNear = ComputeSegmentLod(0, 0, camX, camZ, landGrid, maxN);  // close
    int lodFar = ComputeSegmentLod(40, 40, camX, camZ, landGrid, maxN); // far

    // Near and far segments must get different LODs
    REQUIRE(lodNear < lodFar);

    // If camera moves from near segment to far segment position,
    // the near segment's cached LOD becomes stale
    int lodNearAfterMove = ComputeSegmentLod(0, 0, 2200.0f, 2200.0f, landGrid, maxN);
    REQUIRE(lodNearAfterMove > lodNear); // was 0, now should be 1 or 2
}

TEST_CASE("LOD boundaries: adjacent segments with different LODs", "[World][Terrain][LOD]")
{
    const float landGrid = 50.0f;
    const int maxN = 64;
    float camX = 0.0f, camZ = 0.0f;

    // Segment (0,0): center at (200,200), dist=283 -> LOD 0
    // Segment (8,0): center at (600,200), dist=632 -> LOD 1
    int lodA = ComputeSegmentLod(0, 0, camX, camZ, landGrid, maxN);
    int lodB = ComputeSegmentLod(8, 0, camX, camZ, landGrid, maxN);

    REQUIRE(lodA == 0);
    REQUIRE(lodB == 1);
    // Adjacent segments can differ by at most 1 LOD level
    REQUIRE(std::abs(lodA - lodB) <= 1);
}

// LOD stride and vertex count

TEST_CASE("ComputeLodStride basic values", "[World][Terrain][LOD]")
{
    // subdivisionLevel=2 (subdivisionCount=4)
    REQUIRE(ComputeLodStride(0, 2) == 1); // 2^0 = 1 (full detail)
    REQUIRE(ComputeLodStride(1, 2) == 2); // 2^1 = 2 (half)
    REQUIRE(ComputeLodStride(2, 2) == 4); // 2^2 = 4 (quarter)
}

TEST_CASE("ComputeLodStride clamps to subdivision level", "[World][Terrain][LOD]")
{
    // lodLevel > subdivisionLevel -> clamp
    REQUIRE(ComputeLodStride(3, 2) == 4); // clamped: 2^2 not 2^3
    REQUIRE(ComputeLodStride(4, 2) == 4);
    REQUIRE(ComputeLodStride(2, 1) == 2); // clamped: 2^1 not 2^2
}

TEST_CASE("ComputeEffectiveSubdivCount reduces with LOD", "[World][Terrain][LOD]")
{
    const int subdivLevel = 3; // subdivisionCount = 8

    // LOD 0 -> full subdivision
    REQUIRE(ComputeEffectiveSubdivCount(subdivLevel, 0) == 8);
    // LOD 1 -> half
    REQUIRE(ComputeEffectiveSubdivCount(subdivLevel, 1) == 4);
    // LOD 2 -> quarter
    REQUIRE(ComputeEffectiveSubdivCount(subdivLevel, 2) == 2);
}

TEST_CASE("ComputeVertexCount for standard segment", "[World][Terrain][LOD]")
{
    const int segSize = 8;
    const int subdivLevel = 2; // subdivisionCount=4

    // LOD 0: effSubdiv=4, count per axis = 8*4+1 = 33, total = 33*33 = 1089
    REQUIRE(ComputeVertexCount(segSize, subdivLevel, 0) == 33 * 33);

    // LOD 1: effSubdiv=2, count per axis = 8*2+1 = 17, total = 17*17 = 289
    REQUIRE(ComputeVertexCount(segSize, subdivLevel, 1) == 17 * 17);

    // LOD 2: effSubdiv=1, count per axis = 8*1+1 = 9, total = 9*9 = 81
    REQUIRE(ComputeVertexCount(segSize, subdivLevel, 2) == 9 * 9);
}

TEST_CASE("Vertex count ratio between LOD levels", "[World][Terrain][LOD]")
{
    const int segSize = 8;
    const int subdivLevel = 2;

    int v0 = ComputeVertexCount(segSize, subdivLevel, 0);
    int v1 = ComputeVertexCount(segSize, subdivLevel, 1);
    int v2 = ComputeVertexCount(segSize, subdivLevel, 2);

    // Each LOD level roughly quarters the vertex count
    // (ignoring +1 edge vertices)
    REQUIRE(v0 > v1);
    REQUIRE(v1 > v2);
    // LOD 0 has ~4x more vertices than LOD 1
    float ratio01 = static_cast<float>(v0) / v1;
    REQUIRE(ratio01 > 3.0f);
    REQUIRE(ratio01 < 5.0f);
}

TEST_CASE("Vertex count stays within max bounds", "[World][Terrain][LOD]")
{
    const int segSize = 8;
    const int maxSubdivLevel = 4;
    const int maxSubdivCount = 1 << maxSubdivLevel; // 16
    const int maxNVertices = (segSize * maxSubdivCount + 1) * (segSize * maxSubdivCount + 1);

    for (int subdivLevel = 0; subdivLevel <= maxSubdivLevel; subdivLevel++)
    {
        for (int lod = 0; lod <= 2; lod++)
        {
            int vCount = ComputeVertexCount(segSize, subdivLevel, lod);
            REQUIRE(vCount <= maxNVertices);
            REQUIRE(vCount >= 1);
        }
    }
}

// Segment alignment

TEST_CASE("AlignToSegmentBeg rounds down to segment boundary", "[World][Terrain]")
{
    REQUIRE(AlignToSegmentBeg(0) == 0);
    REQUIRE(AlignToSegmentBeg(1) == 0);
    REQUIRE(AlignToSegmentBeg(7) == 0);
    REQUIRE(AlignToSegmentBeg(8) == 8);
    REQUIRE(AlignToSegmentBeg(9) == 8);
    REQUIRE(AlignToSegmentBeg(15) == 8);
    REQUIRE(AlignToSegmentBeg(16) == 16);
    REQUIRE(AlignToSegmentBeg(255) == 248);
}

TEST_CASE("AlignToSegmentEnd rounds up to segment boundary", "[World][Terrain]")
{
    REQUIRE(AlignToSegmentEnd(0) == 0);
    REQUIRE(AlignToSegmentEnd(1) == 8);
    REQUIRE(AlignToSegmentEnd(7) == 8);
    REQUIRE(AlignToSegmentEnd(8) == 8);
    REQUIRE(AlignToSegmentEnd(9) == 16);
    REQUIRE(AlignToSegmentEnd(15) == 16);
    REQUIRE(AlignToSegmentEnd(16) == 16);
    REQUIRE(AlignToSegmentEnd(17) == 24);
}

TEST_CASE("Segment alignment: beg <= end for any range", "[World][Terrain]")
{
    for (int coord = 0; coord < 256; coord++)
    {
        int beg = AlignToSegmentBeg(coord);
        int end = AlignToSegmentEnd(coord);
        REQUIRE(beg <= end);
        REQUIRE(beg % LandSegmentSizeLod == 0);
        REQUIRE(end % LandSegmentSizeLod == 0);
        REQUIRE(beg <= coord);
        REQUIRE(end >= coord);
    }
}

TEST_CASE("Aligned range covers original range", "[World][Terrain]")
{
    int origBeg = 5, origEnd = 19;
    int alignedBeg = AlignToSegmentBeg(origBeg);
    int alignedEnd = AlignToSegmentEnd(origEnd);

    REQUIRE(alignedBeg <= origBeg);
    REQUIRE(alignedEnd >= origEnd);
    REQUIRE(alignedBeg == 0);
    REQUIRE(alignedEnd == 24);
}

TEST_CASE("Segment alignment with negative coordinates", "[World][Terrain]")
{
    // Negative coordinates use bitwise AND with mask ~7 = ...11111000
    // For two's complement, -1 & ~7 = -8
    REQUIRE(AlignToSegmentBeg(-1) == -8);
    REQUIRE(AlignToSegmentBeg(-8) == -8);
    REQUIRE(AlignToSegmentBeg(-9) == -16);
}

// Segment key hash

TEST_CASE("LandSegKeyLod equality", "[World][Terrain]")
{
    LandSegKeyLod a{0, 0};
    LandSegKeyLod b{0, 0};
    LandSegKeyLod c{8, 0};
    LandSegKeyLod d{0, 8};

    REQUIRE(a == b);
    REQUIRE_FALSE(a == c);
    REQUIRE_FALSE(a == d);
    REQUIRE_FALSE(c == d);
}

TEST_CASE("LandSegKeyLod hash: equal keys produce equal hashes", "[World][Terrain]")
{
    LandSegKeyLodHash h;
    REQUIRE(h(LandSegKeyLod{0, 0}) == h(LandSegKeyLod{0, 0}));
    REQUIRE(h(LandSegKeyLod{8, 16}) == h(LandSegKeyLod{8, 16}));
    REQUIRE(h(LandSegKeyLod{-8, -16}) == h(LandSegKeyLod{-8, -16}));
}

TEST_CASE("LandSegKeyLod hash: different keys produce different hashes", "[World][Terrain]")
{
    LandSegKeyLodHash h;
    // Not strictly required but highly expected for a useful hash
    std::unordered_set<size_t> hashes;
    for (int x = -24; x <= 24; x += 8)
    {
        for (int z = -24; z <= 24; z += 8)
        {
            hashes.insert(h(LandSegKeyLod{x, z}));
        }
    }
    // 7 x-values * 7 z-values = 49 keys, all should have unique hashes
    REQUIRE(hashes.size() == 49);
}

TEST_CASE("LandSegKeyLod hash works with negative coordinates", "[World][Terrain]")
{
    LandSegKeyLodHash h;
    // (x,z) and (-x,z) should have different hashes
    REQUIRE(h(LandSegKeyLod{8, 0}) != h(LandSegKeyLod{-8, 0}));
    // (x,z) and (x,-z) should have different hashes
    REQUIRE(h(LandSegKeyLod{0, 8}) != h(LandSegKeyLod{0, -8}));
}

// Cache sizing

TEST_CASE("EstimateCacheSize minimum is 16", "[World][Terrain]")
{
    // Very small view distance -> formula gives < 16 -> clamped to 16
    REQUIRE(EstimateCacheSize(10.0f, 50.0f) == 16);
}

TEST_CASE("EstimateCacheSize grows with view distance", "[World][Terrain]")
{
    const float landGrid = 50.0f;
    int sizeNear = EstimateCacheSize(500.0f, landGrid);
    int sizeMid = EstimateCacheSize(1000.0f, landGrid);
    int sizeFar = EstimateCacheSize(2000.0f, landGrid);

    REQUIRE(sizeFar > sizeMid);
    REQUIRE(sizeMid > sizeNear);
}

TEST_CASE("EstimateCacheSize scales quadratically with view distance", "[World][Terrain]")
{
    const float landGrid = 50.0f;
    // Doubling view distance should roughly 4x the cache (area scaling)
    int size1 = EstimateCacheSize(1000.0f, landGrid);
    int size2 = EstimateCacheSize(2000.0f, landGrid);

    float ratio = static_cast<float>(size2) / size1;
    // Should be approximately 4x (area scaling), but +8 offset distorts small values
    REQUIRE(ratio > 3.0f);
    REQUIRE(ratio < 5.0f);
}

TEST_CASE("EstimateCacheSize is capped at maximum", "[World][Terrain]")
{
    // Huge view distance should not exceed max
    const int maxReasonable = 512 * 512 / (LandSegmentSizeLod * LandSegmentSizeLod);
    int size = EstimateCacheSize(100000.0f, 1.0f);
    REQUIRE(size <= maxReasonable);
}

TEST_CASE("EstimateCacheSize inversely related to grid size", "[World][Terrain]")
{
    const float viewDist = 2000.0f;
    int sizeSmallGrid = EstimateCacheSize(viewDist, 25.0f);
    int sizeLargeGrid = EstimateCacheSize(viewDist, 100.0f);

    // Smaller grid -> more segments needed to cover same distance
    REQUIRE(sizeSmallGrid > sizeLargeGrid);
}

// Normal computation

TEST_CASE("Terrain normal on flat ground points up", "[World][Terrain]")
{
    // Flat terrain: no height change in any direction
    Vec3 n = ComputeTerrainNormal(0.0f, 0.0f, 12.5f, 1);

    // Y component should be dominant (pointing up, but negative due to cross product convention)
    REQUIRE(std::abs(n.x) < 0.001f);
    REQUIRE(std::abs(n.z) < 0.001f);
    // Normal is unit length
    float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    REQUIRE(len == Catch::Approx(1.0f).margin(0.001f));
}

TEST_CASE("Terrain normal is unit length for arbitrary slopes", "[World][Terrain]")
{
    float testCases[][2] = {{1.0f, 0.0f}, {0.0f, 1.0f}, {5.0f, 3.0f}, {-2.0f, 4.0f}, {-1.0f, -1.0f}};

    for (auto& tc : testCases)
    {
        Vec3 n = ComputeTerrainNormal(tc[0], tc[1], 12.5f, 1);
        float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        REQUIRE(len == Catch::Approx(1.0f).margin(0.001f));
    }
}

TEST_CASE("Terrain normal tilts with slope", "[World][Terrain]")
{
    // Positive xDelta: terrain rises in +X -> normal tilts in +X
    Vec3 nFlat = ComputeTerrainNormal(0.0f, 0.0f, 12.5f, 1);
    Vec3 nSloped = ComputeTerrainNormal(5.0f, 0.0f, 12.5f, 1);

    // The X component should be non-zero when there's an X slope
    REQUIRE(std::abs(nSloped.x) > std::abs(nFlat.x));
}

TEST_CASE("Terrain normal: LOD stride affects scale", "[World][Terrain]")
{
    // Same height deltas but different stride -> different normals
    // because stride changes the horizontal distance in the cross product
    Vec3 n1 = ComputeTerrainNormal(2.0f, 0.0f, 12.5f, 1);
    Vec3 n2 = ComputeTerrainNormal(2.0f, 0.0f, 12.5f, 4);

    // Higher stride -> larger horizontal step -> flatter normal for same height diff
    // n2 should be more vertical (smaller x component) than n1
    REQUIRE(std::abs(n2.x) < std::abs(n1.x));
}

TEST_CASE("Terrain normal: zero terrain grid produces degenerate result", "[World][Terrain]")
{
    // terrainGrid=0 -> step=0 -> cross product is zero vector -> len=0 -> returns (0,0,0)
    Vec3 n = ComputeTerrainNormal(1.0f, 1.0f, 0.0f, 1);
    float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    REQUIRE(len < 0.001f);
}

// LOD threshold sanity

TEST_CASE("LOD thresholds: near < mid always", "[World][Terrain][LOD]")
{
    float gridSizes[] = {10.0f, 25.0f, 50.0f, 100.0f};
    int cacheSizes[] = {16, 32, 64, 128, 256};

    for (float grid : gridSizes)
    {
        for (int maxN : cacheSizes)
        {
            float near, mid;
            ComputeLodThresholds(grid, maxN, near, mid);
            REQUIRE(near > 0.0f);
            REQUIRE(mid > near);
        }
    }
}

TEST_CASE("LOD thresholds match ratio 1:2", "[World][Terrain][LOD]")
{
    float near, mid;
    ComputeLodThresholds(50.0f, 64, near, mid);
    REQUIRE(mid / near == Catch::Approx(2.0f));
}

// Segment grid layout

TEST_CASE("Segments tile without gaps or overlap", "[World][Terrain]")
{
    // Verify that segment flat_quads computed by Fill() tile correctly
    int camX = 128, camZ = 128; // center in grid units
    int segOriginX = AlignToSegmentBeg(camX);
    int segOriginZ = AlignToSegmentBeg(camZ);

    // Generate a grid of segment flat_quads around the camera
    std::vector<std::pair<int, int>> segments;
    for (int dx = -3; dx <= 3; dx++)
    {
        for (int dz = -3; dz <= 3; dz++)
        {
            int xBeg = segOriginX + dx * LandSegmentSizeLod;
            int zBeg = segOriginZ + dz * LandSegmentSizeLod;
            int xEnd = xBeg + LandSegmentSizeLod;
            int zEnd = zBeg + LandSegmentSizeLod;

            // Each segment covers exactly LandSegmentSizeLod cells
            REQUIRE(xEnd - xBeg == LandSegmentSizeLod);
            REQUIRE(zEnd - zBeg == LandSegmentSizeLod);

            segments.push_back({xBeg, zBeg});
        }
    }

    // All segment origins should be unique
    std::unordered_set<size_t> unique;
    LandSegKeyLodHash h;
    for (auto& [x, z] : segments)
    {
        unique.insert(h(LandSegKeyLod{x, z}));
    }
    REQUIRE(unique.size() == segments.size());
}

TEST_CASE("Segment origin is always aligned", "[World][Terrain]")
{
    // Any world position should produce an aligned segment origin
    for (int coord = -100; coord <= 100; coord++)
    {
        int aligned = AlignToSegmentBeg(coord);
        REQUIRE(aligned % LandSegmentSizeLod == 0);
        REQUIRE(aligned <= coord);
        REQUIRE(aligned + LandSegmentSizeLod > coord);
    }
}

// Height ↔ short conversion

TEST_CASE("ShortToHeightLod converts known values", "[World][Terrain]")
{
    REQUIRE(ShortToHeightLod(0) == 0.0f);
    REQUIRE(ShortToHeightLod(1) == Catch::Approx(LandDataScale));
    REQUIRE(ShortToHeightLod(-1) == Catch::Approx(-LandDataScale));
    REQUIRE(ShortToHeightLod(100) == Catch::Approx(100 * LandDataScale));
}

TEST_CASE("HeightToShortLod converts known values", "[World][Terrain]")
{
    REQUIRE(HeightToShortLod(0.0f) == 0);
    REQUIRE(HeightToShortLod(LandDataScale) == 1);
    REQUIRE(HeightToShortLod(-LandDataScale) == -1);
    REQUIRE(HeightToShortLod(100 * LandDataScale) == 100);
}

TEST_CASE("Height conversion round-trip", "[World][Terrain]")
{
    // Short -> Float -> Short should be within ±1 due to float precision
    for (short v = -1000; v < 1000; v++)
    {
        float h = ShortToHeightLod(v);
        short back = HeightToShortLod(h);
        int diff = static_cast<int>(back) - static_cast<int>(v);
        REQUIRE(std::abs(diff) <= 1);
    }
}

TEST_CASE("Height conversion handles extremes", "[World][Terrain]")
{
    // Max short value: 32767 -> ~1474m
    float maxHeight = ShortToHeightLod(32767);
    REQUIRE(maxHeight > 1000.0f);
    REQUIRE(maxHeight < 2000.0f);

    // Min short value: -32768 -> ~-1474m
    float minHeight = ShortToHeightLod(-32768);
    REQUIRE(minHeight < -1000.0f);
    REQUIRE(minHeight > -2000.0f);
}

TEST_CASE("HeightToShortLod floors rather than rounds", "[World][Terrain]")
{
    // Value between two short steps should floor down
    float halfStep = LandDataScale * 0.5f;
    REQUIRE(HeightToShortLod(halfStep) == 0);             // 0.0225 -> floor(0.5) = 0
    REQUIRE(HeightToShortLod(LandDataScale * 1.9f) == 1); // floor(1.9) = 1
}

// Height interpolation

TEST_CASE("InterpolateHeight at corners returns corner heights", "[World][Terrain]")
{
    float y00 = 10.0f, y01 = 20.0f, y10 = 30.0f, y11 = 40.0f;

    // Corner (0,0)
    REQUIRE(InterpolateHeight(0.0f, 0.0f, y00, y01, y10, y11) == Catch::Approx(y00));
    // Corner (1,0) -- in lower-left triangle since xIn(1) <= 1-zIn(1)
    REQUIRE(InterpolateHeight(1.0f, 0.0f, y00, y01, y10, y11) == Catch::Approx(y01));
    // Corner (0,1) -- on boundary: xIn(0) <= 1-zIn(0)=0
    REQUIRE(InterpolateHeight(0.0f, 1.0f, y00, y01, y10, y11) == Catch::Approx(y10));
    // Corner (1,1) -- upper-right triangle
    REQUIRE(InterpolateHeight(1.0f, 1.0f, y00, y01, y10, y11) == Catch::Approx(y11));
}

TEST_CASE("InterpolateHeight on flat terrain returns constant", "[World][Terrain]")
{
    float h = 42.0f;
    // All corners at same height -> output is constant everywhere
    for (float x = 0.0f; x <= 1.0f; x += 0.1f)
    {
        for (float z = 0.0f; z <= 1.0f; z += 0.1f)
        {
            REQUIRE(InterpolateHeight(x, z, h, h, h, h) == Catch::Approx(h));
        }
    }
}

TEST_CASE("InterpolateHeight midpoint of uniform slope", "[World][Terrain]")
{
    // Linear ramp: height = 10 * x + 20 * z
    float y00 = 0.0f;  // x=0, z=0
    float y01 = 10.0f; // x=1, z=0
    float y10 = 20.0f; // x=0, z=1
    float y11 = 30.0f; // x=1, z=1

    // Center (0.5, 0.5) -- on the diagonal boundary
    float center = InterpolateHeight(0.5f, 0.5f, y00, y01, y10, y11);
    REQUIRE(center == Catch::Approx(15.0f));
}

TEST_CASE("InterpolateHeight is continuous at triangle boundary", "[World][Terrain]")
{
    // The diagonal xIn = 1 - zIn divides the cell into two triangles.
    // Height should be continuous across this boundary.
    float y00 = 0.0f, y01 = 10.0f, y10 = 5.0f, y11 = 20.0f;

    // Test along the diagonal (xIn = 1 - zIn)
    for (float t = 0.0f; t <= 1.0f; t += 0.05f)
    {
        float xIn = 1.0f - t;
        float zIn = t;

        // Approaching from lower-left triangle (xIn slightly less)
        float hLeft = InterpolateHeight(xIn - 0.001f, zIn, y00, y01, y10, y11);
        // Approaching from upper-right triangle (xIn slightly more)
        float hRight = InterpolateHeight(xIn + 0.001f, zIn, y00, y01, y10, y11);

        // Should be approximately equal (continuous)
        REQUIRE(hLeft == Catch::Approx(hRight).margin(0.1f));
    }
}

TEST_CASE("InterpolateHeight triangle selection", "[World][Terrain]")
{
    // Non-planar quad: different heights that make the triangle choice matter
    float y00 = 0.0f, y01 = 0.0f, y10 = 0.0f, y11 = 10.0f;

    // Lower-left triangle (0,0)-(1,0)-(0,1): all heights are 0
    REQUIRE(InterpolateHeight(0.1f, 0.1f, y00, y01, y10, y11) == Catch::Approx(0.0f));

    // Upper-right triangle near (1,1): influenced by y11=10
    REQUIRE(InterpolateHeight(0.9f, 0.9f, y00, y01, y10, y11) > 5.0f);
}

// Height interpolation -- derivatives

TEST_CASE("InterpolateHeightDerivatives on flat terrain are zero", "[World][Terrain]")
{
    float h = 50.0f;
    float dX, dZ;
    InterpolateHeightDerivatives(0.3f, 0.2f, h, h, h, h, 0.08f, dX, dZ);
    REQUIRE(dX == Catch::Approx(0.0f));
    REQUIRE(dZ == Catch::Approx(0.0f));
}

TEST_CASE("InterpolateHeightDerivatives detect X slope", "[World][Terrain]")
{
    // Slope only in X direction: y increases with x
    float y00 = 0.0f, y01 = 10.0f, y10 = 0.0f, y11 = 10.0f;
    float invGrid = 1.0f / 12.5f;
    float dX, dZ;

    // Lower-left triangle
    InterpolateHeightDerivatives(0.2f, 0.2f, y00, y01, y10, y11, invGrid, dX, dZ);
    REQUIRE(dX > 0.0f);                 // positive X slope
    REQUIRE(dZ == Catch::Approx(0.0f)); // no Z slope
}

TEST_CASE("InterpolateHeightDerivatives detect Z slope", "[World][Terrain]")
{
    // Slope only in Z direction: y increases with z
    float y00 = 0.0f, y01 = 0.0f, y10 = 10.0f, y11 = 10.0f;
    float invGrid = 1.0f / 12.5f;
    float dX, dZ;

    // Lower-left triangle
    InterpolateHeightDerivatives(0.2f, 0.2f, y00, y01, y10, y11, invGrid, dX, dZ);
    REQUIRE(dX == Catch::Approx(0.0f)); // no X slope
    REQUIRE(dZ > 0.0f);                 // positive Z slope
}

TEST_CASE("InterpolateHeightDerivatives change sign across triangle boundary", "[World][Terrain]")
{
    // Twisted surface: only y11 is elevated
    float y00 = 0.0f, y01 = 0.0f, y10 = 0.0f, y11 = 10.0f;
    float invGrid = 1.0f / 12.5f;
    float dX1, dZ1, dX2, dZ2;

    // Lower-left triangle: all corners 0 -> zero derivatives
    InterpolateHeightDerivatives(0.1f, 0.1f, y00, y01, y10, y11, invGrid, dX1, dZ1);
    REQUIRE(dX1 == Catch::Approx(0.0f));
    REQUIRE(dZ1 == Catch::Approx(0.0f));

    // Upper-right triangle: y11=10 creates slopes
    InterpolateHeightDerivatives(0.9f, 0.9f, y00, y01, y10, y11, invGrid, dX2, dZ2);
    // d1011 = y10-y11 = -10, d0111 = y01-y11 = -10
    // dX = d1011 * -invGrid = 10 * invGrid > 0
    // dZ = d0111 * -invGrid = 10 * invGrid > 0
    REQUIRE(dX2 != Catch::Approx(0.0f));
    REQUIRE(dZ2 != Catch::Approx(0.0f));
}

// Segment cache -- LRU behavior

TEST_CASE("SegmentCache: miss generates new entry", "[World][Terrain][SegmentCache]")
{
    SegmentCacheLod cache(16);
    auto r = cache.Lookup(0, 0, 0, 1.0f);

    REQUIRE_FALSE(r.wasHit);
    REQUIRE_FALSE(r.wasLodEvict);
    REQUIRE(r.entry != nullptr);
    REQUIRE(r.entry->xBeg == 0);
    REQUIRE(r.entry->zBeg == 0);
    REQUIRE(r.entry->lodLevel == 0);
    REQUIRE(cache.Size() == 1);
    REQUIRE(cache.generateCount == 1);
}

TEST_CASE("SegmentCache: hit returns same entry without regenerating", "[World][Terrain][SegmentCache]")
{
    SegmentCacheLod cache(16);
    auto r1 = cache.Lookup(0, 0, 0, 1.0f);
    int gen1 = r1.entry->generationId;

    auto r2 = cache.Lookup(0, 0, 0, 2.0f);

    REQUIRE(r2.wasHit);
    REQUIRE_FALSE(r2.wasLodEvict);
    REQUIRE(r2.entry->generationId == gen1); // same segment, not regenerated
    REQUIRE(cache.generateCount == 1);       // only the initial generate
    REQUIRE(cache.Size() == 1);
}

TEST_CASE("SegmentCache: hit updates lastUsed time", "[World][Terrain][SegmentCache]")
{
    SegmentCacheLod cache(16);
    cache.Lookup(0, 0, 0, 1.0f);
    auto r = cache.Lookup(0, 0, 0, 50.0f);

    REQUIRE(r.entry->lastUsed == 50.0f);
}

TEST_CASE("SegmentCache: different positions are independent entries", "[World][Terrain][SegmentCache]")
{
    SegmentCacheLod cache(16);
    auto r1 = cache.Lookup(0, 0, 0, 1.0f);
    auto r2 = cache.Lookup(8, 0, 1, 1.0f);
    auto r3 = cache.Lookup(0, 8, 2, 1.0f);

    REQUIRE(cache.Size() == 3);
    REQUIRE(cache.generateCount == 3);
    REQUIRE(r1.entry->generationId != r2.entry->generationId);
    REQUIRE(r2.entry->generationId != r3.entry->generationId);
}

// Segment cache -- LOD invalidation (regression tests for the bug)

TEST_CASE("SegmentCache: LOD mismatch evicts and regenerates", "[World][Terrain][SegmentCache][regression]")
{
    SegmentCacheLod cache(16);

    // Cache a segment at LOD 2
    auto r1 = cache.Lookup(0, 0, 2, 1.0f);
    REQUIRE(r1.entry->lodLevel == 2);
    int oldGen = r1.entry->generationId;

    // Same position, now requires LOD 0 (camera moved closer)
    auto r2 = cache.Lookup(0, 0, 0, 2.0f);

    REQUIRE_FALSE(r2.wasHit);
    REQUIRE(r2.wasLodEvict);
    REQUIRE(r2.entry->lodLevel == 0);          // regenerated at correct LOD
    REQUIRE(r2.entry->generationId != oldGen); // new segment was generated
    REQUIRE(cache.generateCount == 2);
    REQUIRE(cache.Size() == 1); // old entry removed, new inserted
}

TEST_CASE("SegmentCache: LOD mismatch in both directions", "[World][Terrain][SegmentCache][regression]")
{
    SegmentCacheLod cache(16);

    // LOD 0 -> LOD 2 (camera moves away)
    cache.Lookup(0, 0, 0, 1.0f);
    auto r = cache.Lookup(0, 0, 2, 2.0f);
    REQUIRE(r.wasLodEvict);
    REQUIRE(r.entry->lodLevel == 2);

    // LOD 2 -> LOD 1 (camera moves partially closer)
    r = cache.Lookup(0, 0, 1, 3.0f);
    REQUIRE(r.wasLodEvict);
    REQUIRE(r.entry->lodLevel == 1);

    // LOD 1 -> LOD 1 (camera stays same distance -- hit)
    r = cache.Lookup(0, 0, 1, 4.0f);
    REQUIRE(r.wasHit);
    REQUIRE(cache.generateCount == 3); // initial + 2 LOD evictions
}

TEST_CASE("SegmentCache: LOD eviction only affects matching position", "[World][Terrain][SegmentCache][regression]")
{
    SegmentCacheLod cache(16);

    // Two segments at different positions
    cache.Lookup(0, 0, 0, 1.0f);
    cache.Lookup(8, 0, 2, 1.0f);
    REQUIRE(cache.Size() == 2);

    // LOD change for (0,0) should not affect (8,0)
    cache.Lookup(0, 0, 2, 2.0f);
    REQUIRE(cache.Size() == 2);

    // (8,0) should still be a hit at LOD 2
    auto r = cache.Lookup(8, 0, 2, 2.0f);
    REQUIRE(r.wasHit);
    REQUIRE(cache.generateCount == 3); // 2 initial + 1 LOD eviction on (0,0)
}

TEST_CASE("SegmentCache: miss generates at requested LOD, not always LOD 0",
          "[World][Terrain][SegmentCache][regression]")
{
    SegmentCacheLod cache(16);

    // First lookup with LOD 2 -- must generate at LOD 2, not default LOD 0
    auto r = cache.Lookup(0, 0, 2, 1.0f);
    REQUIRE(r.entry->lodLevel == 2);

    auto r2 = cache.Lookup(8, 8, 1, 1.0f);
    REQUIRE(r2.entry->lodLevel == 1);
}

// Segment cache -- LRU eviction

TEST_CASE("SegmentCache: LRU eviction when at capacity", "[World][Terrain][SegmentCache]")
{
    SegmentCacheLod cache(3);

    cache.Lookup(0, 0, 0, 1.0f);
    cache.Lookup(8, 0, 0, 2.0f);
    cache.Lookup(16, 0, 0, 3.0f);
    REQUIRE(cache.Size() == 3);

    // Adding a 4th entry should evict the LRU (0,0)
    cache.Lookup(24, 0, 0, 4.0f);
    REQUIRE(cache.Size() == 3);

    // (0,0) was evicted -- lookup is a miss
    auto r = cache.Lookup(0, 0, 0, 5.0f);
    REQUIRE_FALSE(r.wasHit);
}

TEST_CASE("SegmentCache: MRU promotion prevents eviction", "[World][Terrain][SegmentCache]")
{
    SegmentCacheLod cache(3);

    cache.Lookup(0, 0, 0, 1.0f); // oldest
    cache.Lookup(8, 0, 0, 2.0f);
    cache.Lookup(16, 0, 0, 3.0f);

    // Touch (0,0) again -- promotes to MRU
    cache.Lookup(0, 0, 0, 4.0f);

    // Now (8,0) is LRU. Adding new entry should evict (8,0), not (0,0)
    cache.Lookup(24, 0, 0, 5.0f);

    // (0,0) should still be a hit
    auto r = cache.Lookup(0, 0, 0, 6.0f);
    REQUIRE(r.wasHit);

    // (8,0) was evicted
    auto r2 = cache.Lookup(8, 0, 0, 7.0f);
    REQUIRE_FALSE(r2.wasHit);
}

TEST_CASE("SegmentCache: MRU/LRU order is correct", "[World][Terrain][SegmentCache]")
{
    SegmentCacheLod cache(16);

    cache.Lookup(0, 0, 0, 1.0f);
    cache.Lookup(8, 0, 0, 2.0f);
    cache.Lookup(16, 0, 0, 3.0f);

    REQUIRE(cache.MostRecent()->xBeg == 16);
    REQUIRE(cache.LeastRecent()->xBeg == 0);

    // Touch (0,0) -- should become MRU
    cache.Lookup(0, 0, 0, 4.0f);
    REQUIRE(cache.MostRecent()->xBeg == 0);
    REQUIRE(cache.LeastRecent()->xBeg == 8);
}

// Segment cache -- time-based eviction

TEST_CASE("SegmentCache: old entries evicted after 60 seconds", "[World][Terrain][SegmentCache]")
{
    SegmentCacheLod cache(16);

    cache.Lookup(0, 0, 0, 1.0f);
    cache.Lookup(8, 0, 0, 1.0f);
    REQUIRE(cache.Size() == 2);

    // 62 seconds later, a cache miss triggers eviction of old entries
    cache.Lookup(16, 0, 0, 62.0f);
    REQUIRE(cache.Size() == 1); // only the new entry remains

    // Old entries are gone
    auto r = cache.Lookup(0, 0, 0, 62.0f);
    REQUIRE_FALSE(r.wasHit);
}

TEST_CASE("SegmentCache: entries within 60 seconds are kept", "[World][Terrain][SegmentCache]")
{
    SegmentCacheLod cache(16);

    cache.Lookup(0, 0, 0, 10.0f);
    cache.Lookup(8, 0, 0, 10.0f);

    // 50 seconds later -- entries are 50s old, below 60s threshold
    cache.Lookup(16, 0, 0, 60.0f);
    REQUIRE(cache.Size() == 3); // all kept
}

TEST_CASE("SegmentCache: recently touched entries survive time eviction", "[World][Terrain][SegmentCache]")
{
    SegmentCacheLod cache(16);

    cache.Lookup(0, 0, 0, 1.0f);
    cache.Lookup(8, 0, 0, 1.0f);

    // Touch (0,0) at t=50 -- refreshes its lastUsed
    cache.Lookup(0, 0, 0, 50.0f);

    // At t=62, (8,0) is 61s old -> evicted. (0,0) is 12s old -> kept.
    cache.Lookup(16, 0, 0, 62.0f);

    auto r1 = cache.Lookup(0, 0, 0, 62.0f);
    REQUIRE(r1.wasHit); // survived

    auto r2 = cache.Lookup(8, 0, 0, 62.0f);
    REQUIRE_FALSE(r2.wasHit); // was evicted
}

// Segment cache -- combined scenarios

TEST_CASE("SegmentCache: LOD eviction at capacity doesn't lose new entry", "[World][Terrain][SegmentCache]")
{
    SegmentCacheLod cache(2);

    cache.Lookup(0, 0, 0, 1.0f);
    cache.Lookup(8, 0, 0, 1.0f);
    REQUIRE(cache.Size() == 2);

    // LOD change on (0,0): evicts old (0,0), generates new at LOD 1.
    // Cache was at capacity, but we removed one first, so no capacity eviction needed.
    auto r = cache.Lookup(0, 0, 1, 2.0f);
    REQUIRE(r.wasLodEvict);
    REQUIRE(r.entry->lodLevel == 1);
    REQUIRE(cache.Size() == 2);

    // Both entries should be accessible
    auto r1 = cache.Lookup(0, 0, 1, 3.0f);
    REQUIRE(r1.wasHit);
    auto r2 = cache.Lookup(8, 0, 0, 3.0f);
    REQUIRE(r2.wasHit);
}

TEST_CASE("SegmentCache: rapid LOD oscillation handles correctly", "[World][Terrain][SegmentCache]")
{
    SegmentCacheLod cache(16);

    // Simulate camera bouncing between near and far
    for (int i = 0; i < 10; i++)
    {
        int lod = (i % 2 == 0) ? 0 : 2;
        float t = static_cast<float>(i);
        auto r = cache.Lookup(0, 0, lod, t);

        REQUIRE(r.entry->lodLevel == lod);
        if (i == 0)
            REQUIRE_FALSE(r.wasHit); // first is a miss
        else
            REQUIRE(r.wasLodEvict); // rest are LOD evictions
    }

    REQUIRE(cache.generateCount == 10); // every oscillation regenerates
    REQUIRE(cache.Size() == 1);         // only one position
}

TEST_CASE("SegmentCache: integrated LOD computation + cache lookup", "[World][Terrain][SegmentCache]")
{
    // End-to-end test: ComputeSegmentLod feeds into cache lookup
    const float landGrid = 50.0f;
    const int maxN = 64;
    SegmentCacheLod cache(maxN);

    // Camera close to segment (0,0) -> LOD 0
    int lod = ComputeSegmentLod(0, 0, 200.0f, 200.0f, landGrid, maxN);
    REQUIRE(lod == 0);
    auto r1 = cache.Lookup(0, 0, lod, 1.0f);
    REQUIRE(r1.entry->lodLevel == 0);

    // Camera moves far -> LOD 2
    lod = ComputeSegmentLod(0, 0, 5000.0f, 200.0f, landGrid, maxN);
    REQUIRE(lod == 2);
    auto r2 = cache.Lookup(0, 0, lod, 2.0f);
    REQUIRE(r2.wasLodEvict);
    REQUIRE(r2.entry->lodLevel == 2);

    // Camera moves back -> LOD 0 again
    lod = ComputeSegmentLod(0, 0, 200.0f, 200.0f, landGrid, maxN);
    REQUIRE(lod == 0);
    auto r3 = cache.Lookup(0, 0, lod, 3.0f);
    REQUIRE(r3.wasLodEvict);
    REQUIRE(r3.entry->lodLevel == 0);
}

TEST_CASE("SegmentCache: multiple segments at different LODs simultaneously", "[World][Terrain][SegmentCache]")
{
    const float landGrid = 50.0f;
    const int maxN = 64;
    SegmentCacheLod cache(maxN);

    float camX = 400.0f, camZ = 400.0f;

    // Generate a grid of segments around camera -- each at its correct LOD
    for (int x = -24; x <= 24; x += 8)
    {
        for (int z = -24; z <= 24; z += 8)
        {
            int lod = ComputeSegmentLod(x, z, camX, camZ, landGrid, maxN);
            auto r = cache.Lookup(x, z, lod, 1.0f);
            REQUIRE(r.entry->lodLevel == lod);
        }
    }

    // Re-lookup all -- should be cache hits
    int hits = 0;
    for (int x = -24; x <= 24; x += 8)
    {
        for (int z = -24; z <= 24; z += 8)
        {
            int lod = ComputeSegmentLod(x, z, camX, camZ, landGrid, maxN);
            auto r = cache.Lookup(x, z, lod, 2.0f);
            if (r.wasHit)
                hits++;
        }
    }
    REQUIRE(hits == 49); // 7x7 grid, all hits
}
