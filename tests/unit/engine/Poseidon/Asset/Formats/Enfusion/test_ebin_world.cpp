// test_ebin_world.cpp - Enfusion's EBIN world container, and the one field that
// decides whether a placement's Y is an elevation or an offset.
//
// Every container here is built byte by byte from the format documented in
// EbinWorld.hpp, so nothing Reforger-owned is committed and the expected result is
// known by construction.
//
// The measurement these tests exist to protect is in EbinPlacement::hasFlags: over
// Everon's 1,228,065 in-bounds placements, the ones carrying a `Flags` property sit
// within 0.5 m of the terrain 96.3% of the time and the ones without do so 15.4% of
// the time with a median stored Y of exactly 0.00. Reading every Y as absolute is
// what buried Everon's houses a median 29 m underground while its vegetation, rocks
// and walls all looked correct.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Asset/Formats/Enfusion/EbinWorld.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using Poseidon::Asset::Formats::Enfusion::EbinPlacement;
using Poseidon::Asset::Formats::Enfusion::EbinWorld;
using Poseidon::Asset::Formats::Enfusion::ReadEbin;

namespace
{

//! Builds an EBIN by hand. Names are interned in the order they are first asked
//! for, which is what the format requires and what a real writer does.
struct EbinBuilder
{
    std::vector<std::string> names;
    std::vector<uint8_t> records;

    uint16_t Name(const std::string& text)
    {
        for (size_t i = 0; i < names.size(); ++i)
            if (names[i] == text)
                return static_cast<uint16_t>(i);
        names.push_back(text);
        return static_cast<uint16_t>(names.size() - 1);
    }

    static void U16(std::vector<uint8_t>& out, uint16_t value)
    {
        out.push_back(static_cast<uint8_t>(value & 0xFF));
        out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    }
    static void U32(std::vector<uint8_t>& out, uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
    }
    static void F32(std::vector<uint8_t>& out, float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, 4);
        U32(out, bits);
    }

    //! `0x0D nameIdx` -- a class name or a property name.
    void Ident(std::vector<uint8_t>& out, const std::string& text)
    {
        out.push_back(0x0D);
        U16(out, Name(text));
    }
    //! `coords <f32[3]>`
    void Coords(std::vector<uint8_t>& out, float x, float y, float z)
    {
        Ident(out, "coords");
        out.push_back(0x08);
        F32(out, x);
        F32(out, y);
        F32(out, z);
    }
    //! `angles <f32[3]>` -- index 1 is yaw, in degrees.
    void Angles(std::vector<uint8_t>& out, float x, float y, float z)
    {
        Ident(out, "angles");
        out.push_back(0x08);
        F32(out, x);
        F32(out, y);
        F32(out, z);
    }
    //! `<name> <two u32>` -- tag 0x02 is eight bytes, not four.
    void TwoWords(std::vector<uint8_t>& out, const std::string& property, uint32_t a, uint32_t b)
    {
        Ident(out, property);
        out.push_back(0x02);
        U32(out, a);
        U32(out, b);
    }
    void Flags(std::vector<uint8_t>& out, uint32_t a, uint32_t b) { TwoWords(out, "Flags", a, b); }

    //! Wraps a payload as a `0x0E` block whose declared size INCLUDES its own four
    //! size bytes, and appends it to `out`.
    static void Block(std::vector<uint8_t>& out, const std::vector<uint8_t>& payload)
    {
        out.push_back(0x0E);
        U32(out, static_cast<uint32_t>(payload.size() + 4));
        out.insert(out.end(), payload.begin(), payload.end());
    }

    //! An entity block: three zero flag bytes, then a record stream.
    static std::vector<uint8_t> Entity(const std::vector<uint8_t>& body)
    {
        std::vector<uint8_t> payload{0x00, 0x00, 0x00};
        payload.insert(payload.end(), body.begin(), body.end());
        return payload;
    }

    std::vector<uint8_t> Finish() const
    {
        std::vector<uint8_t> out{'E', 'B', 'I', 'N'};
        U32(out, 1);
        U16(out, static_cast<uint16_t>(names.size()));
        U16(out, 0); // no interned GUIDs
        for (const std::string& name : names)
        {
            U16(out, static_cast<uint16_t>(name.size()));
            out.insert(out.end(), name.begin(), name.end());
        }
        out.insert(out.end(), records.begin(), records.end());
        return out;
    }
};

} // namespace

TEST_CASE("EBIN placements record whether their entity carried Flags", "[enfusion][ebin]")
{
    EbinBuilder builder;

    // One entity WITHOUT Flags, at a Y that reads as an offset above the terrain.
    std::vector<uint8_t> flagless;
    builder.Coords(flagless, 5079.05f, 0.272f, 4011.38f);

    // One entity WITH Flags, at an absolute elevation.
    std::vector<uint8_t> flagged;
    builder.Coords(flagged, 100.0f, 77.5f, 200.0f);
    builder.Flags(flagged, 0, 524288);

    builder.Ident(builder.records, "SCR_DestructibleBuildingEntity");
    EbinBuilder::Block(builder.records, EbinBuilder::Entity(flagless));
    builder.Ident(builder.records, "Tree");
    EbinBuilder::Block(builder.records, EbinBuilder::Entity(flagged));

    const std::vector<uint8_t> bytes = builder.Finish();
    const EbinWorld world = ReadEbin(bytes.data(), bytes.size());

    INFO(world.error);
    REQUIRE(world.valid());
    // Byte accounting is the reader's own correctness check and it must close here
    // too, or the field under test was read out of a stream that drifted.
    REQUIRE(world.consumed == bytes.size());
    REQUIRE(world.placements.size() == 2);

    CHECK(world.placements[0].className == "SCR_DestructibleBuildingEntity");
    CHECK_FALSE(world.placements[0].hasFlags);
    CHECK(world.placements[0].position[1] == 0.272f);

    CHECK(world.placements[1].className == "Tree");
    CHECK(world.placements[1].hasFlags);
    CHECK(world.placements[1].position[1] == 77.5f);
}

TEST_CASE("Flags on a child entity does not mark its parent", "[enfusion][ebin]")
{
    // The negative case, and the one that would fail silently. `hasFlags` is set on
    // whichever entity is innermost while the property is being read, so a parent
    // whose CHILD carries Flags must come out flagless -- otherwise a house with any
    // flagged sub-entity would be treated as absolute and stay buried, and nothing
    // about the output would say so.
    EbinBuilder builder;

    std::vector<uint8_t> child;
    builder.Coords(child, 10.0f, 90.0f, 20.0f);
    builder.Flags(child, 0, 524288);

    std::vector<uint8_t> parent;
    builder.Coords(parent, 5079.05f, 0.272f, 4011.38f);
    builder.Ident(parent, "Tree");
    EbinBuilder::Block(parent, EbinBuilder::Entity(child));

    builder.Ident(builder.records, "SCR_DestructibleBuildingEntity");
    EbinBuilder::Block(builder.records, EbinBuilder::Entity(parent));

    const std::vector<uint8_t> bytes = builder.Finish();
    const EbinWorld world = ReadEbin(bytes.data(), bytes.size());

    INFO(world.error);
    REQUIRE(world.valid());
    REQUIRE(world.consumed == bytes.size());
    REQUIRE(world.placements.size() == 2);

    const EbinPlacement& outer = world.placements[0];
    const EbinPlacement& inner = world.placements[1];
    REQUIRE(outer.className == "SCR_DestructibleBuildingEntity");
    REQUIRE(inner.className == "Tree");

    CHECK_FALSE(outer.hasFlags);
    CHECK(inner.hasFlags);
}

TEST_CASE("A different two-word property does not count as Flags", "[enfusion][ebin]")
{
    // The other negative case, and the one that decides whether the whole rule does
    // anything. Tag 0x02 is a generic two-u32 value, not a Flags marker: it occurs
    // 2,201,432 times across the 103-file Reforger corpus against 872,185 flagged
    // placements, so most of them are some other property. A reader that marked the
    // entity on the TAG rather than on the NAME would set hasFlags almost
    // everywhere, every placement would be treated as absolute, and the importer
    // would go back to burying Everon's houses with nothing in its output changed.
    EbinBuilder builder;

    std::vector<uint8_t> body;
    builder.Coords(body, 5079.05f, 0.272f, 4011.38f);
    builder.TwoWords(body, "Layers", 0, 524288);

    builder.Ident(builder.records, "SCR_DestructibleBuildingEntity");
    EbinBuilder::Block(builder.records, EbinBuilder::Entity(body));

    const std::vector<uint8_t> bytes = builder.Finish();
    const EbinWorld world = ReadEbin(bytes.data(), bytes.size());

    INFO(world.error);
    REQUIRE(world.valid());
    REQUIRE(world.consumed == bytes.size());
    REQUIRE(world.placements.size() == 1);
    CHECK_FALSE(world.placements[0].hasFlags);
}

TEST_CASE("A nested entity's transform is local to the entity containing it", "[enfusion][ebin]")
{
    // Read as world coordinates, a child's `coords` strands it wherever its local
    // offset happens to point -- which for a child at the origin of its parent is
    // the MAP origin. On Everon that put 671 objects within 30 m of (0,0),
    // including 128 houses and a church at (0, -114.2, 0).
    //
    // Parent at (100, 10, 200) rotated 90 degrees about Y, child 5 m along local +X.
    // The .wrp writer builds its basis as aside=(c,0,-s), dir=(s,0,c), so a local
    // +X offset under a +90 degree yaw comes out along world +Z. Asserting the axis
    // and not just the distance is the point: a sign error here still puts the
    // child 5 m from its parent, just in the wrong place, and no count would move.
    EbinBuilder builder;

    std::vector<uint8_t> child;
    builder.Coords(child, 5.0f, 0.0f, 0.0f);

    std::vector<uint8_t> parent;
    builder.Coords(parent, 100.0f, 10.0f, 200.0f);
    builder.Angles(parent, 0.0f, 90.0f, 0.0f);
    builder.Ident(parent, "Tree");
    EbinBuilder::Block(parent, EbinBuilder::Entity(child));

    builder.Ident(builder.records, "SCR_DestructibleBuildingEntity");
    EbinBuilder::Block(builder.records, EbinBuilder::Entity(parent));

    const std::vector<uint8_t> bytes = builder.Finish();
    const EbinWorld world = ReadEbin(bytes.data(), bytes.size());

    INFO(world.error);
    REQUIRE(world.valid());
    REQUIRE(world.consumed == bytes.size());
    REQUIRE(world.placements.size() == 2);

    const EbinPlacement& outer = world.placements[0];
    const EbinPlacement& inner = world.placements[1];
    REQUIRE(outer.className == "SCR_DestructibleBuildingEntity");
    REQUIRE(inner.className == "Tree");

    // The parent is top-level and must NOT move.
    CHECK(outer.position[0] == Catch::Approx(100.0f));
    CHECK(outer.position[1] == Catch::Approx(10.0f));
    CHECK(outer.position[2] == Catch::Approx(200.0f));

    CHECK(inner.position[0] == Catch::Approx(100.0f).margin(1e-3));
    CHECK(inner.position[1] == Catch::Approx(10.0f).margin(1e-3));
    CHECK(inner.position[2] == Catch::Approx(195.0f).margin(1e-3));
    CHECK(inner.angles[1] == Catch::Approx(90.0f));
}

TEST_CASE("A top-level entity is not composed onto anything", "[enfusion][ebin]")
{
    // The negative case. Composition must apply ONLY to an entity nested inside
    // another entity: 1,226,183 of Everon's 1,230,897 placements are top-level and
    // already in world coordinates, so a rule that caught them too would move the
    // entire island rather than the 0.34% that needed it.
    EbinBuilder builder;

    std::vector<uint8_t> first;
    builder.Coords(first, 100.0f, 10.0f, 200.0f);
    std::vector<uint8_t> second;
    builder.Coords(second, 7.0f, 1.0f, 9.0f);

    builder.Ident(builder.records, "Tree");
    EbinBuilder::Block(builder.records, EbinBuilder::Entity(first));
    builder.Ident(builder.records, "Tree");
    EbinBuilder::Block(builder.records, EbinBuilder::Entity(second));

    const std::vector<uint8_t> bytes = builder.Finish();
    const EbinWorld world = ReadEbin(bytes.data(), bytes.size());

    INFO(world.error);
    REQUIRE(world.valid());
    REQUIRE(world.placements.size() == 2);
    CHECK(world.placements[1].position[0] == Catch::Approx(7.0f));
    CHECK(world.placements[1].position[1] == Catch::Approx(1.0f));
    CHECK(world.placements[1].position[2] == Catch::Approx(9.0f));
}
