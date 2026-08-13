#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <Poseidon/Asset/Formats/World/Oprw25.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include "../BISFramework/test_helpers.hpp"
#include "test_fixtures.hpp"
#include <cstdint>
#include <filesystem>
#include <cstring>
#include <string>
#include <vector>

using namespace Poseidon;
using namespace Poseidon::Asset::Formats;
using namespace Poseidon::Asset::Formats::World;

namespace
{

// A minimum viable revision-25 world, assembled field by field.
//
// Synthetic rather than a committed slice of a retail world: the local Arma 3
// worlds are licensed data that must not enter the repository, and CI has none of
// them. The grids are deliberately 4x4 so every bulk payload lands under the
// 1024-byte compression threshold and the fixture needs no LZO *compressor* --
// the compressed path is covered by the local-corpus case at the bottom of this
// file, where real payloads exercise it.
class Oprw25Builder
{
  public:
    void U8(uint8_t v) { bytes_.push_back(v); }
    void I16(int16_t v) { Raw(&v, sizeof(v)); }
    void I32(int32_t v) { Raw(&v, sizeof(v)); }
    void F32(float v) { Raw(&v, sizeof(v)); }
    void Asciiz(const char* v) { Raw(v, std::strlen(v) + 1); }
    void Bytes(size_t count, uint8_t fill = 0)
    {
        for (size_t i = 0; i < count; ++i)
            bytes_.push_back(fill);
    }
    void Raw(const void* data, size_t size)
    {
        const auto* p = static_cast<const uint8_t*>(data);
        bytes_.insert(bytes_.end(), p, p + size);
    }

    // A quadtree whose root is a leaf: every cell carries the same four bytes.
    void UniformQuadTree(uint32_t leafValue)
    {
        U8(0);
        Raw(&leafValue, sizeof(leafValue));
    }

    // A quadtree whose root is a node with sixteen leaf children, so the walk down
    // the index bits is exercised rather than short-circuited.
    void NodeQuadTree(const uint32_t (&leaves)[16])
    {
        U8(1);
        I16(0); // no child is itself a node
        for (uint32_t leaf : leaves)
            Raw(&leaf, sizeof(leaf));
    }

    const std::vector<uint8_t>& Bytes() const { return bytes_; }

  private:
    std::vector<uint8_t> bytes_;
};

constexpr int32_t kLand = 4;
constexpr int32_t kTerrain = 4;

// Builds a complete, self-consistent world. Tests that need a malformed one patch
// the returned bytes, so every case starts from a file that is known to parse.
std::vector<uint8_t> BuildWorld(bool geographyAsNode = false, int extraObjects = 0)
{
    Oprw25Builder b;
    b.Raw("OPRW", 4);
    b.I32(25);
    b.I32(107410);
    b.I32(kLand);
    b.I32(kLand);
    b.I32(kTerrain);
    b.I32(kTerrain);
    b.F32(32.0f);

    if (geographyAsNode)
    {
        // Leaf n holds two 16-bit cells; give every leaf a distinct pair so the
        // expansion order can be asserted exactly.
        uint32_t leaves[16];
        for (uint32_t i = 0; i < 16; ++i)
            leaves[i] = (i * 2u) | ((i * 2u + 1u) << 16);
        b.NodeQuadTree(leaves);
    }
    else
    {
        b.UniformQuadTree(0x00070007u);
    }

    b.UniformQuadTree(0); // sound map
    b.I32(1);             // one mountain
    b.F32(10.0f);
    b.F32(42.0f);
    b.F32(20.0f);
    b.UniformQuadTree(0x00020002u); // material index: every cell uses material 2

    b.Bytes(kTerrain * kTerrain, 0); // grass approximation
    b.Bytes(kTerrain * kTerrain, 0); // primary texture index
    for (int i = 0; i < kTerrain * kTerrain; ++i)
        b.F32(static_cast<float>(i) - 4.0f); // elevation -4 .. 11

    b.I32(3); // terrain materials
    b.Asciiz("");
    b.U8(0);
    b.Asciiz("a3\\map_test\\data\\layers\\p_000-000_l00.rvmat");
    b.U8(0);
    b.Asciiz("a3\\map_test\\data\\layers\\p_000-001_l00.rvmat");
    b.U8(0);

    b.I32(2); // models
    b.Asciiz("a3\\rocks_f\\stonesharp_big.p3d");
    b.Asciiz("a3\\plants_f\\tree\\t_pinuss1s_f.p3d");

    b.I32(1); // static entities
    b.Asciiz("Land_naviglight");
    b.Asciiz("a3\\roads_f\\runway\\runwaylights\\naviglight.p3d");
    b.F32(100.0f);
    b.F32(1.0f);
    b.F32(200.0f);
    b.I32(7);

    const int32_t objectCount = 1 + extraObjects;
    b.UniformQuadTree(0); // object offsets
    b.I32(objectCount * static_cast<int32_t>(kOprw25ObjectRecordSize));
    b.UniformQuadTree(0); // map-info offsets
    b.I32(0);             // map-info size

    b.Bytes(kLand * kLand, 0);       // persistent
    b.Bytes(kTerrain * kTerrain, 0); // subdivision hints

    b.I32(objectCount - 1); // max object id
    b.I32(kLand * kLand * 4);
    for (int i = 0; i < kLand * kLand; ++i)
        b.I32(0); // no road links in any cell

    for (int32_t i = 0; i < objectCount; ++i)
    {
        b.I32(i);     // object id
        b.I32(i % 2); // model index
        b.F32(1);
        b.F32(0);
        b.F32(0); // aside
        b.F32(0);
        b.F32(1);
        b.F32(0); // up
        b.F32(0);
        b.F32(0);
        b.F32(1); // dir
        b.F32(1000.0f + i);
        b.F32(5.0f);
        b.F32(2000.0f + i); // position
        b.I32(2);           // shape param
    }

    return b.Bytes();
}

Oprw25World ReadFromBytes(const std::vector<uint8_t>& bytes)
{
    TestQIStream stream(bytes);
    BinaryReader reader(stream);
    return ReadOprw25(reader);
}

} // namespace

TEST_CASE("OPRW25: synthetic world round-trips every declared field", "[Formats][WRP][OPRW25]")
{
    const auto world = ReadFromBytes(BuildWorld());

    REQUIRE(world.header.version == 25);
    REQUIRE(world.header.appId == 107410);
    REQUIRE(world.header.landRangeX == kLand);
    REQUIRE(world.header.terrainRangeX == kTerrain);
    REQUIRE(world.header.landCellSize == 32.0f);

    // Terrain cells are the land cell scaled by the grid ratio, not the stored
    // value. With equal grids here they coincide; the local-corpus case below is
    // where they differ (32 m land, 4 m terrain).
    REQUIRE(world.header.TerrainCellSize() == 32.0f);
    REQUIRE(world.header.WorldExtent() == 128.0f);

    REQUIRE(world.geography.size() == kLand * kLand);
    REQUIRE(world.materialIndex.size() == kLand * kLand);
    REQUIRE(world.materialIndex[0] == 2);
    REQUIRE(world.materialIndex[kLand * kLand - 1] == 2);

    REQUIRE(world.mountains.size() == 1);
    REQUIRE(world.mountains[0].y == 42.0f);

    REQUIRE(world.elevation.size() == kTerrain * kTerrain);
    REQUIRE(world.elevation.front() == -4.0f);
    REQUIRE(world.elevation.back() == 11.0f);
    REQUIRE(world.ElevationAt(1, 0) == -3.0f);
    REQUIRE(world.ElevationAt(0, 1) == 0.0f);

    REQUIRE(world.materials.size() == 3);
    REQUIRE(world.materials[0].empty());
    REQUIRE(world.materials[2] == "a3\\map_test\\data\\layers\\p_000-001_l00.rvmat");

    REQUIRE(world.models.size() == 2);
    REQUIRE(world.models[0] == "a3\\rocks_f\\stonesharp_big.p3d");

    REQUIRE(world.staticEntities.size() == 1);
    REQUIRE(world.staticEntities[0].className == "Land_naviglight");
    REQUIRE(world.staticEntities[0].objectId == 7);
    REQUIRE(world.staticEntities[0].position.x == 100.0f);

    REQUIRE(world.roadNet.size() == kLand * kLand);
    REQUIRE(world.objects.size() == 1);
    REQUIRE(world.objects[0].modelIndex == 0);
    REQUIRE(world.objects[0].shapeParam == 2);
    // Row 3 is the translation; the first three rows are the object basis.
    REQUIRE(world.objects[0].transform.rows[3].x == 1000.0f);
    REQUIRE(world.objects[0].transform.rows[3].z == 2000.0f);
    REQUIRE(world.objects[0].transform.rows[0].x == 1.0f);

    REQUIRE(world.mapInfo.empty());
}

TEST_CASE("OPRW25: quadtree node children expand in the documented cell order", "[Formats][WRP][OPRW25]")
{
    const auto world = ReadFromBytes(BuildWorld(/*geographyAsNode=*/true));

    // Leaf index is (y << 2) + (x >> 1); within a leaf the 16-bit element is at
    // (x & 1). Leaf n was filled with the pair (2n, 2n+1), so cell (x, y) must
    // read 2*((y << 2) + (x >> 1)) + (x & 1). Asserting the whole grid rather
    // than a corner: an off-by-one in either the leaf shape or the descent would
    // still satisfy a single sample.
    for (int y = 0; y < kLand; ++y)
        for (int x = 0; x < kLand; ++x)
        {
            const int leaf = (y << 2) + (x >> 1);
            const int expected = leaf * 2 + (x & 1);
            INFO("cell " << x << "," << y);
            REQUIRE(world.geography[static_cast<size_t>(y) * kLand + x] == expected);
        }
}

TEST_CASE("OPRW25: earlier revisions are refused rather than misread", "[Formats][WRP][OPRW25]")
{
    // Revision 25 shares only its magic with the OPRW the legacy reader handles.
    // Accepting a v3 file here would parse an OFP world's dense grids as
    // quadtrees and produce plausible-looking nonsense.
    for (const int32_t revision : {3, 17, 24, 26})
    {
        auto bytes = BuildWorld();
        std::memcpy(bytes.data() + 4, &revision, sizeof(revision));
        INFO("revision " << revision);
        REQUIRE_THROWS_AS(ReadFromBytes(bytes), std::runtime_error);
    }
}

// DZ-001. Revision 29 is DayZ's, and it is accepted; the revisions around it are
// not. That distinction is the whole point of keeping kOprwModernRevisions an
// explicit list rather than a range: 29's header, static entities and road links
// each carry an extra field, and 26/27/28/30 have never been seen, so admitting
// them would apply DayZ's field widths to an unknown layout.
TEST_CASE("OPRW29: DayZ's revision is accepted and its neighbours are not",
          "[Formats][WRP][OPRW25][DZ-001]")
{
    REQUIRE(IsOprwModernRevision(29));
    for (const int32_t revision : {26, 27, 28, 30})
    {
        INFO("revision " << revision);
        REQUIRE_FALSE(IsOprwModernRevision(revision));
    }
}

TEST_CASE("OPRW25: a non-OPRW container is refused", "[Formats][WRP][OPRW25]")
{
    auto bytes = BuildWorld();
    std::memcpy(bytes.data(), "8WVR", 4);
    REQUIRE_THROWS_AS(ReadFromBytes(bytes), std::runtime_error);
}

TEST_CASE("OPRW25: an object table size that is not a whole record is refused", "[Formats][WRP][OPRW25]")
{
    auto bytes = BuildWorld();
    // Find the declared object table size and corrupt it. It is the only int32
    // equal to one record's width in the fixture, so this locates it without
    // hard-coding an offset that every later field edit would invalidate.
    const int32_t recordSize = static_cast<int32_t>(kOprw25ObjectRecordSize);
    bool patched = false;
    for (size_t i = 0; i + 4 <= bytes.size(); ++i)
    {
        int32_t value = 0;
        std::memcpy(&value, bytes.data() + i, sizeof(value));
        if (value == recordSize)
        {
            const int32_t bad = recordSize + 1;
            std::memcpy(bytes.data() + i, &bad, sizeof(bad));
            patched = true;
            break;
        }
    }
    REQUIRE(patched);
    REQUIRE_THROWS_AS(ReadFromBytes(bytes), std::runtime_error);
}

TEST_CASE("OPRW25: an object referencing a missing model is refused", "[Formats][WRP][OPRW25]")
{
    auto bytes = BuildWorld();
    // The object record's model index is the second int32 of the last 60 bytes.
    const size_t recordStart = bytes.size() - kOprw25ObjectRecordSize;
    const int32_t bad = 99;
    std::memcpy(bytes.data() + recordStart + 4, &bad, sizeof(bad));
    REQUIRE_THROWS_AS(ReadFromBytes(bytes), std::runtime_error);
}

TEST_CASE("OPRW25: probe accepts revision 25 and leaves the stream alone", "[Formats][WRP][OPRW25]")
{
    const auto bytes = BuildWorld();
    TestQIStream stream(bytes);
    BinaryReader reader(stream);
    REQUIRE(IsOprw25(reader));
    REQUIRE(reader.tell() == 0);
    // The probe must not have consumed anything: a full read still succeeds.
    REQUIRE_NOTHROW(ReadOprw25(reader));
}

// The compressed path, the two-resolution grid, and the file-scale self-checks
// (road network and map-info sizes) can only be exercised by a real world. These
// are licensed local data, so the case reports itself skipped everywhere else.
TEST_CASE("OPRW25: local Arma 3 world parses to its declared byte counts", "[Formats][WRP][OPRW25][GameData]")
{
    // Altis is here specifically because its grids differ from the other two
    // (1024/4096 at 30 m, versus 256/2048 at 32 m). Two worlds of identical
    // dimensions cannot tell a correct two-grid reader from one that happens to
    // work at 8:1.
    const char* candidates[] = {
        "packages/a3-compat/world/stratis/stratis.wrp",
        "packages/a3-compat/world/vr/vr.wrp",
        "packages/a3-compat/world/altis/altis.wrp",
    };

    int parsed = 0;
    for (const char* relative : candidates)
    {
        // packages/ is the gitignored home for local licensed data. Resolved by
        // walking up from the executable rather than from the working directory,
        // which CTest does not guarantee.
        std::filesystem::path found;
        for (std::filesystem::path at(TestFixtures::GetExecutableDirectory()); !at.empty(); at = at.parent_path())
        {
            std::error_code ec;
            const std::filesystem::path candidate = at / relative;
            if (std::filesystem::is_regular_file(candidate, ec))
            {
                found = candidate;
                break;
            }
            if (at.parent_path() == at)
                break;
        }
        if (found.empty())
            continue;

        const std::string path = found.string();
        QIFStream file;
        file.open(path.c_str());
        if (file.fail())
            continue;

        BinaryReader reader(file);
        REQUIRE(IsOprw25(reader));
        const Oprw25World world = ReadOprw25(reader);
        ++parsed;

        INFO("world " << path);
        REQUIRE(world.header.appId == 107410);
        // Asserted as relationships rather than literals, so the case covers any
        // revision-25 world rather than the two it was first written against.
        REQUIRE(world.header.landRangeX == world.header.landRangeY);
        REQUIRE(world.header.terrainRangeX == world.header.terrainRangeY);
        REQUIRE(world.header.terrainRangeX % world.header.landRangeX == 0);
        REQUIRE(world.header.landCellSize > 0.0f);
        REQUIRE(world.header.WorldExtent() ==
                world.header.TerrainCellSize() * static_cast<float>(world.header.terrainRangeX));
        REQUIRE(world.elevation.size() == static_cast<size_t>(world.header.terrainRangeX) * world.header.terrainRangeY);
        REQUIRE(world.geography.size() == static_cast<size_t>(world.header.landRangeX) * world.header.landRangeY);
        REQUIRE(world.materialIndex.size() == world.geography.size());
        REQUIRE(!world.materials.empty());
        // Counted rather than asserted per object: Altis has 1.78 million of them,
        // and a REQUIRE each would be 1.78 million Catch2 assertions.
        size_t outOfRange = 0;
        for (const auto& object : world.objects)
            if (static_cast<size_t>(object.modelIndex) >= world.models.size())
                ++outOfRange;
        REQUIRE(outOfRange == 0);
        // ReadOprw25 already required the road network and trailing map info to
        // match their declared sizes, so reaching here is the byte-exact result.
    }

    if (parsed == 0)
        SKIP("No local Arma 3 revision-25 world available");
}
