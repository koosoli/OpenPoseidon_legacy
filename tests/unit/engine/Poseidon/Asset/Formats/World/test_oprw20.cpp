#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <Poseidon/Asset/Formats/World/Oprw20.hpp>
#include <Poseidon/Asset/Formats/World/Oprw24.hpp>
#include <Poseidon/Asset/Formats/World/Oprw25.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include "../BISFramework/test_helpers.hpp"
#include "test_fixtures.hpp"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace Poseidon;
using namespace Poseidon::Asset::Formats;
using namespace Poseidon::Asset::Formats::World;

namespace
{

// A minimum viable Armed Assault world (OPRW 18/20), assembled field by field.
//
// Synthetic rather than a slice of Sahrani: the local Arma 1 packages are licensed
// data that must not enter the repository, and CI has none of them.
//
// The grids are 32x32 rather than the revision-25 fixture's 4x4, and that is the
// point. Every bulk payload then exceeds the 1024-byte compression threshold, so
// this fixture exercises the LZSS path that is the whole reason revision 20 needs
// its own handling -- an uncompressed fixture would pass with the codec gate
// inverted. The real-corpus case at the bottom covers back-references; the encoder
// here emits literals only, which is enough to drive the decoder, the flag-byte
// cadence and the trailing checksum.
class Oprw20Builder
{
  public:
    void U8(uint8_t v) { bytes_.push_back(v); }
    void I16(int16_t v) { Raw(&v, sizeof(v)); }
    void I32(int32_t v) { Raw(&v, sizeof(v)); }
    void F32(float v) { Raw(&v, sizeof(v)); }
    void Asciiz(const char* v) { Raw(v, std::strlen(v) + 1); }
    void Raw(const void* data, size_t size)
    {
        const auto* p = static_cast<const uint8_t*>(data);
        bytes_.insert(bytes_.end(), p, p + size);
    }

    // A bulk array, written the way the container writes one: raw below the
    // threshold, LZSS above it, with no flag to say which.
    void Payload(const std::vector<uint8_t>& payload)
    {
        if (payload.size() < COMPRESSION_THRESHOLD)
        {
            Raw(payload.data(), payload.size());
            return;
        }
        // SSCompress flag bytes carry eight bits, LSB first; a set bit means the
        // next byte is a literal. All-literals is a legal encoding, just not one a
        // compressor would choose.
        int32_t checksum = 0;
        for (size_t i = 0; i < payload.size(); i += 8)
        {
            const size_t run = payload.size() - i < 8 ? payload.size() - i : 8;
            U8(static_cast<uint8_t>((1u << run) - 1u));
            for (size_t k = 0; k < run; ++k)
            {
                U8(payload[i + k]);
                checksum += payload[i + k];
            }
        }
        I32(checksum);
    }

    void PayloadBytes(size_t count, uint8_t fill = 0) { Payload(std::vector<uint8_t>(count, fill)); }

    // A quadtree whose root is a leaf: every cell carries the same four bytes.
    void UniformQuadTree(uint32_t leafValue)
    {
        U8(0);
        Raw(&leafValue, sizeof(leafValue));
    }

    const std::vector<uint8_t>& Bytes() const { return bytes_; }

  private:
    std::vector<uint8_t> bytes_;
};

constexpr int32_t kLand = 32;
constexpr int32_t kTerrain = 32;
constexpr size_t kLandCells = static_cast<size_t>(kLand) * kLand;
constexpr size_t kTerrainCells = static_cast<size_t>(kTerrain) * kTerrain;

const char* kRoadSegment = "ca\\roads\\ces_d10 100.p3d";

// Builds a complete, self-consistent world at the given revision. Tests that need a
// malformed one patch the returned bytes, so every case starts from a file that is
// known to parse.
std::vector<uint8_t> BuildWorld(int32_t revision = 20)
{
    Oprw20Builder b;
    b.Raw("OPRW", 4);
    b.I32(revision);
    // No app id: that field arrives in revision 25.
    b.I32(kLand);
    b.I32(kLand);
    b.I32(kTerrain);
    b.I32(kTerrain);
    b.F32(40.0f); // Sahrani's land cell

    b.UniformQuadTree(0x00070007u); // geography
    b.UniformQuadTree(0);           // sound map
    b.I32(1);                       // one mountain
    b.F32(120.0f);
    b.F32(31.0f);
    b.F32(240.0f);
    b.UniformQuadTree(0x00020002u); // material index: every cell uses material 2

    // Randomization: present on 18/20, absent from 24. Two bytes per land cell,
    // packed colour:8 / uOff:4 / vOff:4. Distinct per cell so a wrong element
    // width or a wrong cell count cannot pass.
    {
        std::vector<uint8_t> random(kLandCells * 2);
        for (size_t i = 0; i < kLandCells; ++i)
        {
            const uint16_t value = static_cast<uint16_t>((i & 0xFF) | (((i % 11) & 0x0F) << 8));
            random[i * 2 + 0] = static_cast<uint8_t>(value & 0xFF);
            random[i * 2 + 1] = static_cast<uint8_t>(value >> 8);
        }
        b.Payload(random);
    }

    b.PayloadBytes(kTerrainCells, 0); // grass approximation
    // No second per-terrain-cell byte array: revision 24 adds that one.

    {
        std::vector<uint8_t> elevation(kTerrainCells * sizeof(float));
        for (size_t i = 0; i < kTerrainCells; ++i)
        {
            const float height = static_cast<float>(i) - 4.0f;
            std::memcpy(elevation.data() + i * sizeof(float), &height, sizeof(height));
        }
        b.Payload(elevation);
    }

    b.I32(3); // terrain materials
    b.Asciiz("");
    b.U8(0);
    b.Asciiz("ca\\sara\\data\\layers\\p_000-000_l02.rvmat");
    b.U8(0);
    b.Asciiz("ca\\sara\\data\\layers\\p_001-000_l02.rvmat");
    b.U8(0);

    b.I32(2); // models
    b.Asciiz("ca\\plants\\les_singlestrom_b.p3d");
    b.Asciiz("ca\\rocks\\skala_1.p3d");

    b.I32(1); // static entities
    b.Asciiz("Land_kulna");
    b.Asciiz("ca\\buildings\\kulna.p3d");
    b.F32(2604.0f);
    b.F32(15.0f);
    b.F32(2852.0f);
    b.I32(7);

    b.UniformQuadTree(0); // object offsets
    b.I32(static_cast<int32_t>(kOprw25ObjectRecordSize));
    b.UniformQuadTree(0); // map-info offsets
    b.I32(0);             // map-info size

    b.PayloadBytes(kLandCells, 0);    // persistent
    b.PayloadBytes(kTerrainCells, 0); // subdivision hints

    b.I32(0); // max object id

    // Road network: one two-point link in cell 0, and nothing anywhere else. The
    // link is the discriminator for the connection-type gate -- read with the
    // revision-24 rule, the two type bytes this file does not have are taken from
    // the front of the segment path and it comes back as "\roads\..." instead.
    Oprw20Builder road;
    road.I32(1); // cell 0 holds one link
    road.I16(2); // two connections
    road.F32(100.0f);
    road.F32(1.0f);
    road.F32(200.0f);
    road.F32(110.0f);
    road.F32(1.0f);
    road.F32(200.0f);
    road.I32(42); // object id
    road.Asciiz(kRoadSegment);
    for (int i = 0; i < 12; ++i)
        road.F32(i == 0 || i == 4 || i == 8 ? 1.0f : 0.0f);
    const std::vector<uint8_t>& roadBytes = road.Bytes();
    b.I32(static_cast<int32_t>(roadBytes.size() + (kLandCells - 1) * sizeof(int32_t)));
    b.Raw(roadBytes.data(), roadBytes.size());
    for (size_t cell = 1; cell < kLandCells; ++cell)
        b.I32(0);

    // One object.
    b.I32(0); // object id
    b.I32(1); // model index
    b.F32(1);
    b.F32(0);
    b.F32(0);
    b.F32(0);
    b.F32(1);
    b.F32(0);
    b.F32(0);
    b.F32(0);
    b.F32(1);
    b.F32(1000.0f);
    b.F32(5.0f);
    b.F32(2000.0f);
    b.I32(2); // shape param

    return b.Bytes();
}

Oprw20World ReadFromBytes(const std::vector<uint8_t>& bytes, int32_t revision = 20)
{
    TestQIStream stream(bytes);
    BinaryReader reader(stream);
    return ReadOprwModern(reader, revision);
}

} // namespace

TEST_CASE("OPRW20: synthetic Arma 1 world round-trips every declared field", "[Formats][WRP][OPRW20]")
{
    const auto world = ReadFromBytes(BuildWorld());

    REQUIRE(world.header.version == 20);
    // Revision 25's app id is not stored here, and reading one would eat the land
    // range: a zero here is the field's absence, not a world without a DLC.
    REQUIRE(world.header.appId == 0);
    REQUIRE(world.header.landRangeX == kLand);
    REQUIRE(world.header.terrainRangeX == kTerrain);
    REQUIRE(world.header.landCellSize == 40.0f);
    REQUIRE(world.header.WorldExtent() == 40.0f * kLand);

    REQUIRE(world.geography.size() == kLandCells);
    REQUIRE(world.materialIndex.size() == kLandCells);
    REQUIRE(world.materialIndex[kLandCells - 1] == 2);

    REQUIRE(world.mountains.size() == 1);
    REQUIRE(world.mountains[0].y == 31.0f);

    REQUIRE(world.elevation.size() == kTerrainCells);
    REQUIRE(world.elevation.front() == -4.0f);
    REQUIRE(world.elevation.back() == static_cast<float>(kTerrainCells) - 5.0f);

    REQUIRE(world.materials.size() == 3);
    REQUIRE(world.materials[2] == "ca\\sara\\data\\layers\\p_001-000_l02.rvmat");
    REQUIRE(world.models.size() == 2);
    REQUIRE(world.staticEntities.size() == 1);
    REQUIRE(world.staticEntities[0].className == "Land_kulna");

    REQUIRE(world.objects.size() == 1);
    REQUIRE(world.objects[0].modelIndex == 1);
    REQUIRE(world.objects[0].transform.rows[3].x == 1000.0f);
    REQUIRE(world.mapInfo.empty());
}

TEST_CASE("OPRW20: the randomization array is read at land resolution", "[Formats][WRP][OPRW20]")
{
    const auto world = ReadFromBytes(BuildWorld());

    // Present on 18/20 and nowhere else. Asserting every cell rather than a corner:
    // a payload read at the wrong element width still fills the vector.
    REQUIRE(world.randomization.size() == kLandCells);
    for (size_t i = 0; i < kLandCells; ++i)
    {
        INFO("cell " << i);
        REQUIRE(world.randomization[i] == static_cast<uint16_t>((i & 0xFF) | (((i % 11) & 0x0F) << 8)));
    }

    // And the array revision 24 adds is absent, rather than silently taking the
    // bytes of whatever follows.
    REQUIRE(world.primaryTextureIndex.empty());
    REQUIRE(world.grassApprox.size() == kTerrainCells);
    REQUIRE(world.subdivisionHints.size() == kTerrainCells);
    REQUIRE(world.persistent.size() == kLandCells);
}

TEST_CASE("OPRW20: road links carry no connection types and keep their whole path", "[Formats][WRP][OPRW20]")
{
    const auto world = ReadFromBytes(BuildWorld());

    REQUIRE(world.roadNet.size() == kLandCells);
    REQUIRE(world.roadNet[0].size() == 1);
    const auto& link = world.roadNet[0][0];
    REQUIRE(link.positions.size() == 2);
    REQUIRE(link.connectionTypes.empty());
    REQUIRE(link.objectId == 42);
    // The whole point of the gate. Under the revision-24 rule the two absent type
    // bytes are taken off the front of this string and it reads "\roads\...", which
    // the road network's own declared size does NOT catch: the asciiz resynchronises
    // on the same terminator either way. The path is the only witness.
    REQUIRE(link.p3dPath == kRoadSegment);
}

TEST_CASE("OPRW20: revision 18 reads through the same layout", "[Formats][WRP][OPRW20]")
{
    const auto world = ReadFromBytes(BuildWorld(18), 18);
    REQUIRE(world.header.version == 18);
    REQUIRE(world.randomization.size() == kLandCells);
    REQUIRE(world.primaryTextureIndex.empty());
    REQUIRE(world.roadNet[0][0].p3dPath == kRoadSegment);
}

TEST_CASE("OPRW20: each profile refuses the revisions it does not name", "[Formats][WRP][OPRW20]")
{
    // The profiles exist so that a caller who means one generation gets a refusal
    // rather than a plausible misread. An Arma 1 world fed to the revision-24 path
    // would take its randomization array as the terrain byte arrays and desync
    // from there.
    {
        const auto bytes = BuildWorld(20);
        TestQIStream stream(bytes);
        BinaryReader reader(stream);
        REQUIRE_THROWS_AS(ReadOprw24(reader), std::runtime_error);
    }
    {
        const auto bytes = BuildWorld(20);
        TestQIStream stream(bytes);
        BinaryReader reader(stream);
        REQUIRE_THROWS_AS(ReadOprw25(reader), std::runtime_error);
    }
    {
        const auto bytes = BuildWorld(18);
        TestQIStream stream(bytes);
        BinaryReader reader(stream);
        REQUIRE_THROWS_AS(ReadOprw20(reader), std::runtime_error);
    }
}

TEST_CASE("OPRW20: revisions between the two verified groups are refused", "[Formats][WRP][OPRW20]")
{
    // 21, 22 and 23 are not in the corpus. Revision 23 is where the codec is said
    // to change and the other three differences are only bracketed between 20 and
    // 24, so accepting one of these would be a guess dressed as a version check.
    for (const int32_t revision : {17, 19, 21, 22, 23, 26})
    {
        auto bytes = BuildWorld();
        std::memcpy(bytes.data() + 4, &revision, sizeof(revision));
        INFO("revision " << revision);
        TestQIStream stream(bytes);
        BinaryReader reader(stream);
        REQUIRE(PeekOprwModernRevision(reader) == 0);
        REQUIRE_THROWS_AS(ReadOprwModern(reader), std::runtime_error);
    }
}

TEST_CASE("OPRW20: the shared probe names the revision and leaves the stream alone", "[Formats][WRP][OPRW20]")
{
    for (const int32_t revision : {18, 20})
    {
        const auto bytes = BuildWorld(revision);
        TestQIStream stream(bytes);
        BinaryReader reader(stream);
        INFO("revision " << revision);
        REQUIRE(PeekOprwRevision(reader) == revision);
        REQUIRE(PeekOprwModernRevision(reader) == revision);
        REQUIRE(IsOprwArma1(reader));
        REQUIRE(reader.tell() == 0);
        // The probe must not have consumed anything: a full read still succeeds.
        REQUIRE_NOTHROW(ReadOprwModern(reader));
    }
}

TEST_CASE("OPRW20: an object referencing a missing model is refused", "[Formats][WRP][OPRW20]")
{
    auto bytes = BuildWorld();
    // The object record's model index is the second int32 of the last 60 bytes.
    const size_t recordStart = bytes.size() - kOprw25ObjectRecordSize;
    const int32_t bad = 99;
    std::memcpy(bytes.data() + recordStart + 4, &bad, sizeof(bad));
    REQUIRE_THROWS_AS(ReadFromBytes(bytes), std::runtime_error);
}

// The real LZSS payloads (back-references, not just literals), the two-resolution
// grid at 4:1, and the file-scale self-checks can only be exercised by a real
// world. These are licensed local data, so the case reports itself skipped
// everywhere else.
TEST_CASE("OPRW20: local Arma 1 worlds parse to their declared byte counts", "[Formats][WRP][OPRW20][GameData]")
{
    // All three shipped Sahrani worlds: two revisions (20 and 18) and two grid
    // sizes (512/2048 and 256/1024). One world could not tell a correct reader
    // from one that happens to work at 512.
    const char* candidates[] = {
        "packages/a1-compat/world/sara/sara.wrp",
        "packages/a1-compat/world/sara_dbe1/sara_dbe1.wrp",
        "packages/a1-compat/world/saralite/saralite.wrp",
    };

    int parsed = 0;
    for (const char* relative : candidates)
    {
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
        const int32_t revision = PeekOprwModernRevision(reader);
        INFO("world " << path);
        REQUIRE((revision == 18 || revision == 20));
        const Oprw20World world = ReadOprwModern(reader, revision);
        ++parsed;

        REQUIRE(world.header.appId == 0);
        REQUIRE(world.header.landCellSize == 40.0f);
        REQUIRE(world.header.terrainRangeX == world.header.landRangeX * 4);
        REQUIRE(world.randomization.size() == static_cast<size_t>(world.header.landRangeX) * world.header.landRangeY);
        REQUIRE(world.primaryTextureIndex.empty());
        REQUIRE(world.elevation.size() == static_cast<size_t>(world.header.terrainRangeX) * world.header.terrainRangeY);

        // Sahrani's stored peaks and its elevation grid are two independent
        // encodings of the same maximum, and they agree.
        float highestSample = world.elevation.front();
        for (const float height : world.elevation)
            if (height > highestSample)
                highestSample = height;
        float highestPeak = 0.0f;
        for (const auto& peak : world.mountains)
            if (peak.y > highestPeak)
                highestPeak = peak.y;
        REQUIRE(highestPeak == highestSample);

        size_t outOfRange = 0;
        for (const auto& object : world.objects)
            if (static_cast<size_t>(object.modelIndex) >= world.models.size())
                ++outOfRange;
        REQUIRE(outOfRange == 0);

        // Every road segment keeps its bank prefix. Truncation is exactly what the
        // connection-type gate gets wrong, and the road network's declared size
        // cannot see it, because the asciiz resynchronises on the same terminator
        // either way. Read with the revision-24 rule, Sahrani's most common segment
        // comes back as "\roads\ces_d10 100.p3d" -- the leading separator is the
        // tell. Asserted as a shape rather than a literal prefix: Queen's Gambit
        // legitimately mixes "ca\" and "dbe1\" banks in one world (506 of
        // sara_dbe1's 12,152 links). The exact-string discrimination lives in the
        // synthetic case above, where the expected path is known.
        size_t links = 0;
        for (const auto& cell : world.roadNet)
            for (const auto& link : cell)
            {
                ++links;
                REQUIRE(link.connectionTypes.empty());
                REQUIRE(!link.p3dPath.empty());
                REQUIRE(link.p3dPath.front() != '\\');
                REQUIRE(link.p3dPath.size() > 4);
                REQUIRE(link.p3dPath.compare(link.p3dPath.size() - 4, 4, ".p3d") == 0);
            }
        REQUIRE(links > 0);

        // ReadOprwModern already required the road network and the trailing map
        // info to match their declared sizes, so reaching here is byte-exact.
    }

    if (parsed == 0)
        SKIP("No local Arma 1 world available");
}
