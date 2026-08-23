// test_resource_database.cpp - Enfusion's `resourceDatabase.rdb` GUID index.
//
// Built byte by byte from the format in ResourceDatabase.hpp, so nothing
// Reforger-owned is committed and the expected result is known by construction.
//
// The corpus check that matters lives outside CI, where the corpus is: both
// databases an Arma Reforger install ships walk to EOF exactly -- core
// 81,645/81,645 over 1,193 records, data 11,509,935/11,509,935 over 116,389 --
// yielding 110,860 GUID -> path entries against the 51,452 the text scan finds.

#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Asset/Formats/Enfusion/ResourceDatabase.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using Poseidon::Asset::Formats::Enfusion::ReadResourceDatabase;
using Poseidon::Asset::Formats::Enfusion::ResourceDatabase;

namespace
{

struct RdbBuilder
{
    std::vector<uint8_t> records;

    static void U16(std::vector<uint8_t>& out, uint16_t v)
    {
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    }
    static void U32(std::vector<uint8_t>& out, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }

    //! `type` decides the record's width: >= 6 carries a trailing 8-byte field.
    void Add(const std::string& path, uint16_t type, const std::string& guidHex)
    {
        U32(records, static_cast<uint32_t>(path.size() + 1));
        records.insert(records.end(), path.begin(), path.end());
        records.push_back(0);
        U16(records, type);
        U32(records, 0);
        // Stored little-endian; the textual form is these bytes reversed.
        for (int i = 7; i >= 0; --i)
        {
            const std::string byteHex = guidHex.substr(static_cast<size_t>(i) * 2, 2);
            records.push_back(static_cast<uint8_t>(std::stoul(byteHex, nullptr, 16)));
        }
        if (type >= 6)
            for (int i = 0; i < 8; ++i)
                records.push_back(0xAB);
    }

    //! FORM/RDBC wrapper plus a header blob the reader has to skip past.
    std::vector<uint8_t> Finish(size_t headerFillerBytes = 18) const
    {
        std::vector<uint8_t> body;
        U32(body, 7); // version
        U32(body, 0); // total size, patched below
        for (size_t i = 0; i < headerFillerBytes; ++i)
            body.push_back(0x5A); // undecoded header; must not parse as a record
        body.insert(body.end(), records.begin(), records.end());

        std::vector<uint8_t> out{'F', 'O', 'R', 'M'};
        const uint32_t formSize = static_cast<uint32_t>(4 + body.size());
        for (int i = 3; i >= 0; --i) // FORM size is BIG-endian
            out.push_back(static_cast<uint8_t>((formSize >> (8 * i)) & 0xFF));
        out.insert(out.end(), {'R', 'D', 'B', 'C'});
        out.insert(out.end(), body.begin(), body.end());
        const uint32_t total = static_cast<uint32_t>(out.size());
        std::memcpy(out.data() + 16, &total, 4);
        return out;
    }
};

//! Enough records that the reader's prefix probe accepts the table start.
void AddFiller(RdbBuilder& builder, int count)
{
    for (int i = 0; i < count; ++i)
        builder.Add("Prefabs/Filler/Item_" + std::to_string(i) + ".et", 6, "00000000000000FF");
}

} // namespace

TEST_CASE("resourceDatabase.rdb walks to EOF and maps GUIDs to paths", "[enfusion][rdb]")
{
    RdbBuilder builder;
    AddFiller(builder, 70);
    builder.Add("Prefabs/Structures/Cultural/Churches/Church_01/Church_01_blue_nocellar.et", 6, "2BF82F9EBAC834D6");
    const std::vector<uint8_t> bytes = builder.Finish();

    const ResourceDatabase db = ReadResourceDatabase(bytes.data(), bytes.size());
    INFO(db.error);
    REQUIRE(db.valid());
    // Byte accounting is the reader's own correctness criterion. Without it a
    // desynchronised walk returns plausible paths under the wrong GUIDs.
    REQUIRE(db.closes());
    REQUIRE(db.consumed == bytes.size());
    CHECK(db.version == 7);
    CHECK(db.records == 71);

    const auto found = db.byGuid.find("2BF82F9EBAC834D6");
    REQUIRE(found != db.byGuid.end());
    CHECK(found->second == "Prefabs/Structures/Cultural/Churches/Church_01/Church_01_blue_nocellar.et");
}

TEST_CASE("A directory record is 8 bytes shorter than a file record", "[enfusion][rdb]")
{
    // The trap, and the only thing about this format that is not guessable from a
    // single record: type >= 6 carries a trailing 8-byte field and types 4 and 5
    // do not. Assume one stride for all of them and the walk desynchronises a few
    // hundred records in, then keeps producing well-formed nonsense -- which is
    // why the whole file has to close, and why this test interleaves the two
    // widths rather than testing them apart.
    RdbBuilder builder;
    AddFiller(builder, 70);
    builder.Add("Prefabs/Structures/Walls/Concrete/ConcreteWall_01", 5, "FF799E50C4B35FF9");
    builder.Add("Prefabs/Structures/Walls/Concrete/ConcreteWall_01/Wall_V1.et", 6, "1122334455667788");
    builder.Add("Prefabs/Autotest", 4, "AABBCCDDEEFF0011");
    builder.Add("Prefabs/Systems", 7, "99887766554433FF");
    const std::vector<uint8_t> bytes = builder.Finish();

    const ResourceDatabase db = ReadResourceDatabase(bytes.data(), bytes.size());
    INFO(db.error);
    REQUIRE(db.valid());
    REQUIRE(db.closes());
    CHECK(db.records == 74);

    CHECK(db.byGuid.at("FF799E50C4B35FF9") == "Prefabs/Structures/Walls/Concrete/ConcreteWall_01");
    CHECK(db.byGuid.at("1122334455667788") == "Prefabs/Structures/Walls/Concrete/ConcreteWall_01/Wall_V1.et");
    CHECK(db.byGuid.at("AABBCCDDEEFF0011") == "Prefabs/Autotest");
    CHECK(db.byGuid.at("99887766554433FF") == "Prefabs/Systems");
}

TEST_CASE("A truncated database is refused rather than half-read", "[enfusion][rdb]")
{
    // A partial walk of this format yields real-looking paths under whichever
    // GUIDs it happens to land on. Half an index is worse than none, so the
    // reader must report that it did not close instead of returning what it got.
    RdbBuilder builder;
    AddFiller(builder, 70);
    std::vector<uint8_t> bytes = builder.Finish();
    bytes.resize(bytes.size() - 5);

    const ResourceDatabase db = ReadResourceDatabase(bytes.data(), bytes.size());
    CHECK_FALSE(db.closes());
}

TEST_CASE("A non-RDBC container is rejected", "[enfusion][rdb]")
{
    RdbBuilder builder;
    AddFiller(builder, 70);
    std::vector<uint8_t> bytes = builder.Finish();
    bytes[8] = 'T';
    bytes[9] = 'E';
    bytes[10] = 'R';
    bytes[11] = 'R';

    const ResourceDatabase db = ReadResourceDatabase(bytes.data(), bytes.size());
    CHECK_FALSE(db.valid());
    CHECK_FALSE(db.closes());
}
