// test_odol_revision.cpp - AST-012A: versioned ODOL dispatch.
//
// Architecture only. This routes on the exact revision and refuses everything it
// cannot read; it claims support for no later revision. The revisions named are
// those AST-007 found in the owner's corpora (7, 48, 49, 50, 52, 73), plus the
// controlled official Binarize fixture revision 75 -- the only
// population any claim here can honestly cover.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Asset/Formats/P3D/ODOLLoader.hpp>
#include <Poseidon/Asset/Formats/P3D/OdolRevision.hpp>
#include <Poseidon/Asset/Formats/P3D/Odol73.hpp>
#include <Poseidon/Asset/Formats/Common/FormatDetector.hpp>
#include <Poseidon/World/Model/ModelCache.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include "../../../../test_fixtures.hpp"
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace P3D = Poseidon::Asset::Formats::P3D;
using Poseidon::Asset::Formats::ODOLLoader;

// A header only: dispatch must decide before any body is read, so a body is never
// needed to prove the routing.
static std::vector<char> odolHeader(uint32_t version)
{
    std::vector<char> data(64, 0);
    std::memcpy(data.data(), "ODOL", 4);
    std::memcpy(data.data() + 4, &version, 4);
    return data;
}

TEST_CASE("ODOL revisions found in the corpora are described", "[p3d][odol][ast-012a]")
{
    REQUIRE(P3D::DescribeOdolRevision(7).support == P3D::OdolSupport::Parsed);
    // Revision 48 was Recognised-only for as long as no base Arma 2 asset was
    // available to test against. With the licensed base-A2 packages supplied it
    // routes through the same A2-family reader as 49/50/52 -- unchanged, because
    // that reader's version gates already covered it -- and all 64 revision-48
    // models Takistan references parse.
    REQUIRE(P3D::DescribeOdolRevision(48).support == P3D::OdolSupport::NarrowSubset);
    REQUIRE(std::string(P3D::DescribeOdolRevision(48).generation).find("Arma 2") != std::string::npos);
    REQUIRE(P3D::DescribeOdolRevision(49).support == P3D::OdolSupport::NarrowSubset);
    REQUIRE(std::string(P3D::DescribeOdolRevision(49).generation).find("Arma 2") != std::string::npos);
    REQUIRE(P3D::DescribeOdolRevision(50).support == P3D::OdolSupport::NarrowSubset);
    REQUIRE(std::string(P3D::DescribeOdolRevision(50).generation).find("Arrowhead") != std::string::npos);
    REQUIRE(P3D::DescribeOdolRevision(52).support == P3D::OdolSupport::NarrowSubset);
    REQUIRE(std::string(P3D::DescribeOdolRevision(52).generation).find("Arrowhead") != std::string::npos);
    // AST-012B reads one shape of revision 73 and refuses the rest, which is a
    // third state: claiming either Parsed or Recognised for it would be a lie in
    // one direction or the other.
    REQUIRE(P3D::DescribeOdolRevision(73).support == P3D::OdolSupport::NarrowSubset);
    REQUIRE(std::string(P3D::DescribeOdolRevision(73).generation) == "Arma 3");
    REQUIRE(P3D::DescribeOdolRevision(75).support == P3D::OdolSupport::Recognised);
    REQUIRE(std::string(P3D::DescribeOdolRevision(75).generation) == "Arma 3 Tools Binarize");
    // DZ-001. DayZ, and deliberately described as its own generation rather than as
    // an Arma one: 54 is an Arma 2 descendant structurally, but calling it "Arma 2"
    // here would send anyone reading a refusal to the wrong corpus.
    REQUIRE(P3D::DescribeOdolRevision(54).support == P3D::OdolSupport::NarrowSubset);
    REQUIRE(std::string(P3D::DescribeOdolRevision(54).generation) == "DayZ");
    REQUIRE(P3D::DescribeOdolRevision(9999).support == P3D::OdolSupport::Unknown);
}

// The revisions between the A2 family and DayZ were never observed in any corpus,
// so they must stay refused. This is the guard on the widening gate in Odol49.hpp:
// it keys on `revision >= 54`, and if 53 were ever admitted to the table it would
// silently take the DayZ field widths.
TEST_CASE("The gap between the Arma 2 family and DayZ stays unclaimed", "[p3d][odol][dz-001]")
{
    for (uint32_t revision : {51u, 53u, 55u, 56u})
    {
        INFO("revision " << revision);
        REQUIRE(P3D::DescribeOdolRevision(revision).support == P3D::OdolSupport::Unknown);
    }
}

TEST_CASE("A recognised revision is refused, naming its generation", "[p3d][odol][ast-012a]")
{
    auto data = odolHeader(73);
    try
    {
        ODOLLoader::loadFromBuffer(data.data(), static_cast<int>(data.size()), "a3.p3d");
        FAIL("expected ODOL 73 to be refused");
    }
    catch (const P3D::UnsupportedOdolRevision& error)
    {
        REQUIRE(error.version() == 73);
        REQUIRE(error.recognised());
        // A diagnostic that does not say which generation sends the reader nowhere.
        REQUIRE(std::string(error.what()).find("Arma 3") != std::string::npos);
        REQUIRE(std::string(error.what()).find("AST-012B") != std::string::npos);
    }
}

TEST_CASE("The official Binarize revision is recognised but safely refused", "[p3d][odol][ast-012c]")
{
    auto data = odolHeader(75);
    try
    {
        ODOLLoader::loadFromBuffer(data.data(), static_cast<int>(data.size()), "official-sample-odol75.p3d");
        FAIL("expected ODOL 75 to be refused");
    }
    catch (const P3D::UnsupportedOdolRevision& error)
    {
        REQUIRE(error.version() == 75);
        REQUIRE(error.recognised());
        REQUIRE(std::string(error.what()).find("Arma 3 Tools Binarize") != std::string::npos);
    }
}

TEST_CASE("An unrecognised revision is refused without speculating", "[p3d][odol][ast-012a]")
{
    auto data = odolHeader(4242);
    try
    {
        ODOLLoader::loadFromBuffer(data.data(), static_cast<int>(data.size()), "future.p3d");
        FAIL("expected an unknown ODOL revision to be refused");
    }
    catch (const P3D::UnsupportedOdolRevision& error)
    {
        REQUIRE(error.version() == 4242);
        REQUIRE_FALSE(error.recognised());
        REQUIRE(std::string(error.what()).find("unrecognised") != std::string::npos);
    }
}

TEST_CASE("The refusal stays a runtime_error", "[p3d][odol][ast-012a]")
{
    // ModelCache and ShapeAdapter catch broadly; the new type must not escape them.
    auto data = odolHeader(48);
    REQUIRE_THROWS_AS(ODOLLoader::loadFromBuffer(data.data(), static_cast<int>(data.size()), "a2.p3d"),
                      std::runtime_error);
}

TEST_CASE("A non-ODOL signature is a malformed input, not an unsupported revision",
          "[p3d][odol][ast-012a]")
{
    // These are different failures and must stay distinguishable: one is a valid
    // model this build cannot read, the other is not a model at all.
    std::vector<char> data(64, 0);
    std::memcpy(data.data(), "XXXX", 4);
    QIStream                               stream(data.data(), static_cast<int>(data.size()));
    Poseidon::Asset::Formats::BinaryReader reader(stream);
    REQUIRE_THROWS_AS(P3D::PeekOdolRevision(reader), std::runtime_error);

    bool wasRevisionError = false;
    try { QIStream s2(data.data(), 64); Poseidon::Asset::Formats::BinaryReader r2(s2); P3D::PeekOdolRevision(r2); }
    catch (const P3D::UnsupportedOdolRevision&) { wasRevisionError = true; }
    catch (const std::runtime_error&) {}
    REQUIRE_FALSE(wasRevisionError);
}

TEST_CASE("Peeking the revision does not consume the header", "[p3d][odol][ast-012a]")
{
    auto                                   data = odolHeader(7);
    QIStream                               stream(data.data(), static_cast<int>(data.size()));
    Poseidon::Asset::Formats::BinaryReader reader(stream);

    const int before = reader.tell();
    REQUIRE(P3D::PeekOdolRevision(reader).version == 7);
    REQUIRE(reader.tell() == before);
    // Idempotent, and the v7 reader must still see its own signature afterwards.
    REQUIRE(P3D::PeekOdolRevision(reader).version == 7);
    REQUIRE(reader.tell() == before);
}

TEST_CASE("Existing OFP/CWA ODOL models still load", "[p3d][odol][ast-012a]")
{
    // The regression that matters: dispatch must not change what v7 content does.
    auto model = ODOLLoader::load(GET_FIXTURE("p3d/animated_morph_odol.p3d"));
    REQUIRE(model.sourceFormat == "ODOL");
    REQUIRE(model.sourceVersion == 7);
    REQUIRE_FALSE(model.lodLevels.empty());
}

TEST_CASE("ODOL 73 preamble reads its distinct ModelInfo prefix", "[p3d][odol][ast-012b]")
{
    std::vector<char> data;
    auto append = [&data](auto value) { const char* p = reinterpret_cast<const char*>(&value); data.insert(data.end(), p, p + sizeof(value)); };
    data.insert(data.end(), {'O','D','O','L'}); append(uint32_t{73}); append(uint32_t{107410});
    data.push_back('\0'); append(uint32_t{2}); append(1.0f); append(10000.0f);
    append(int32_t{512}); append(2.0f); append(3.0f); append(int32_t{4}); append(int32_t{5}); append(int32_t{6});
    Vector3 aim{1.0f, 2.0f, 3.0f}; data.insert(data.end(), reinterpret_cast<char*>(&aim), reinterpret_cast<char*>(&aim) + sizeof(aim));
    append(uint32_t{0x11223344}); append(uint32_t{0x55667788}); append(0.5f);
    Vector3 min{-1.0f, -2.0f, -3.0f}, max{1.0f, 2.0f, 3.0f};
    data.insert(data.end(), reinterpret_cast<char*>(&min), reinterpret_cast<char*>(&min) + sizeof(min));
    data.insert(data.end(), reinterpret_cast<char*>(&max), reinterpret_cast<char*>(&max) + sizeof(max));
    QIStream stream(data.data(), static_cast<int>(data.size())); Poseidon::Asset::Formats::BinaryReader reader(stream);
    const auto preamble = P3D::ReadOdol73Preamble(reader);
    REQUIRE(preamble.appId == 107410); REQUIRE(preamble.resolutions == std::vector<float>{1.0f, 10000.0f});
    REQUIRE(preamble.special == 512); REQUIRE(preamble.aimingCenter.z == 3.0f); REQUIRE(preamble.bboxMin.x == -1.0f);
}

TEST_CASE("ODOL 73 uncompressed arrays preserve the next field", "[p3d][odol][ast-012b]")
{
    // A v73 compressed-array flag is per payload.  The uncompressed branch is
    // still required to consume exactly its bytes because another payload follows.
    const std::vector<char> data{0, 'a', 'b', 'c', 0x7f};
    QIStream stream(data.data(), static_cast<int>(data.size()));
    Poseidon::Asset::Formats::BinaryReader reader(stream);
    const auto bytes = P3D::ReadOdol73Compressed(reader, 3);
    REQUIRE(bytes == std::vector<uint8_t>{'a', 'b', 'c'});
    REQUIRE(reader.read<uint8_t>() == 0x7f);
}

TEST_CASE("ODOL 73 LZO arrays stop at their explicit end marker", "[p3d][odol][ast-012b]")
{
    // LZO: first literal run of three bytes, followed by the 17/0/0 end marker.
    // The sentinel models the compression flag of the following ODOL payload.
    const std::vector<char> data{1, 20, 'a', 'b', 'c', 17, 0, 0, 0x7f};
    QIStream stream(data.data(), static_cast<int>(data.size()));
    Poseidon::Asset::Formats::BinaryReader reader(stream);
    const auto bytes = P3D::ReadOdol73Compressed(reader, 3);
    REQUIRE(bytes == std::vector<uint8_t>{'a', 'b', 'c'});
    REQUIRE(reader.read<uint8_t>() == 0x7f);
}

TEST_CASE("ODOL 73 selected corpus fixture matches its recorded preamble", "[p3d][odol][ast-012b][corpus]")
{
    const char* path = std::getenv("POSEIDON_ODOL73_FIXTURE");
    if (!path || !*path)
        SKIP("set POSEIDON_ODOL73_FIXTURE to the extracted AST-012B fixture");
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    REQUIRE(file.good());
    const auto size = file.tellg(); file.seekg(0);
    std::vector<char> bytes(static_cast<size_t>(size)); file.read(bytes.data(), size);
    QIStream stream(bytes.data(), static_cast<int>(bytes.size())); Poseidon::Asset::Formats::BinaryReader reader(stream);
    const auto directory = P3D::ReadOdol73StaticDirectory(reader, static_cast<int>(bytes.size()));
    REQUIRE(directory.model.appId == 107410);
    REQUIRE(directory.model.resolutions.size() == 2);
    REQUIRE(directory.model.resolutions[0] == 1.0f);
    REQUIRE(directory.starts.size() == 2);
    REQUIRE(directory.starts[0] < directory.ends[0]);
    CAPTURE(directory.starts, directory.ends, directory.permanent);
    const auto lod = P3D::ReadOdol73StaticLodHeader(reader, directory.starts[0], directory.ends[0]);
    REQUIRE(lod.vertexCount == 48);
    REQUIRE(lod.textures == std::vector<std::string>{"a3\\3den\\objects\\data\\area_ca.paa"});
    const auto materials = P3D::ReadOdol73EmbeddedMaterials(reader);
    REQUIRE(materials.size() == 1);
    REQUIRE(materials[0].name == "a3\\3den\\objects\\data\\area.rvmat");
    REQUIRE(materials[0].version == 11);
    REQUIRE(materials[0].texGens.size() == 8);
    const auto source = P3D::ToRvMaterialSource(materials[0]);
    REQUIRE(source.embedded);
    REQUIRE(source.stages.size() == materials[0].stageTextures.size());
    REQUIRE(source.texGens.size() == 8);
    // The stage transform is 3x4. With 4x4 the reader ran 128 bytes past the end
    // of the material, landed mid-face-record, and still produced eleven
    // plausible quads -- the face encoding resynchronises, so "it parsed" was not
    // evidence of anything. 0x626 is where the material actually ends.
    REQUIRE(reader.tell() == 0x626);
    REQUIRE(P3D::ReadOdol73VertexIndexArray(reader, "point-to-vertex map").empty());
    REQUIRE(P3D::ReadOdol73VertexIndexArray(reader, "vertex-to-point map").empty());
    const auto polygons = P3D::ReadOdol73Polygons(reader, lod.vertexCount, directory.ends[0]);
    REQUIRE(polygons.faces.size() == 16);
    REQUIRE(polygons.faceDataSize == 320);
    REQUIRE(polygons.unused == 0);
    REQUIRE(polygons.faces[0] == std::vector<uint32_t>{30, 28, 25, 27});
    REQUIRE(polygons.faces[5] == std::vector<uint32_t>{39, 35, 31, 43});
    REQUIRE(polygons.faces[15] == std::vector<uint32_t>{5, 17, 13, 9});
    REQUIRE(reader.tell() == 0x748);
    // The face stream measured in the section bounds' own units closes on the
    // declared total, which is what licenses reading those bounds as face ranges.
    REQUIRE(P3D::Odol73FaceStreamOffsets(polygons).back() == polygons.faceDataSize);
    const auto sections = P3D::ReadOdol73StaticSections(reader, directory.ends[0]);
    REQUIRE(sections.size() == 1);
    REQUIRE(sections[0].faceLower == 0);
    REQUIRE(sections[0].faceUpper == 320);
    REQUIRE(sections[0].materialIndex == 0);
    // The selected fixture has neither, but they are read rather than assumed
    // absent: the arrays are counted on the wire and skipping them would put
    // every later offset out by their length.
    REQUIRE(P3D::ReadOdol73NamedSelections(reader, directory.ends[0]).empty());
    REQUIRE(P3D::ReadOdol73NamedProperties(reader, directory.ends[0]).empty());
    const auto preRest = P3D::ReadOdol73StaticPreRestData(reader, directory.ends[0]);
    REQUIRE(preRest.colorTop == static_cast<int32_t>(0xff000000));
    REQUIRE(preRest.restDataSize == 613);
    const int restStart = reader.tell();
    const auto rest = P3D::ReadOdol73StaticRestData(reader, lod, preRest, directory.ends[0]);
    REQUIRE(reader.tell() == restStart + 613);
    REQUIRE(rest.uvSetCount == 1);
    REQUIRE(rest.positions.size() == 48);
    REQUIRE(rest.normals.size() == 48);
    REQUIRE(rest.clip.size() == 48);
    // A unit cube from 3den: every position is a corner or an edge midpoint.
    REQUIRE(rest.positions[0].x == -0.5f);
    REQUIRE(rest.positions[0].y == -0.5f);
    REQUIRE(rest.positions[0].z == -0.5f);
    REQUIRE(rest.positions[39].x == 0.5f);
    REQUIRE(rest.positions[39].z == 0.5f);
    // Decoded normals are axis-aligned units; the format's scale factor is
    // negative, so a sign error here would show up as -1 where +1 belongs.
    REQUIRE(rest.normals[0].z == 1.0f);
    REQUIRE(rest.normals[39].x == 1.0f);
    REQUIRE(rest.uv0.uv[0][0] == Catch::Approx(0.250250f).margin(1e-5));
    REQUIRE(rest.uv0.uv[47][0] == Catch::Approx(0.749720f).margin(1e-5));
    REQUIRE(P3D::ReadOdol73TrailingLodByte(reader, directory.ends[0]) == 1);
    REQUIRE(reader.tell() == static_cast<int>(directory.ends[0]));
}

TEST_CASE("ODOL 73 static fixture reaches the common Poseidon model IR", "[p3d][odol][ast-012b][corpus]")
{
    // The point of AST-012B: revision 73 has to arrive in the same IR every other
    // format lands in, through the same loader entry point -- not in a private
    // struct that only its own test can read.
    const char* path = std::getenv("POSEIDON_ODOL73_FIXTURE");
    if (!path || !*path)
        SKIP("set POSEIDON_ODOL73_FIXTURE to the extracted AST-012B fixture");

    const auto model = ODOLLoader::load(path);
    REQUIRE(model.sourceFormat == "ODOL");
    REQUIRE(model.sourceVersion == 73);
    REQUIRE(model.lodLevels.size() == 2);

    const auto& lod = model.lodLevels[0];
    REQUIRE(lod.resolution == 1.0f);
    REQUIRE(lod.sourceEncoding == "ODOL73");
    REQUIRE(lod.uvSetCount == 1);
    REQUIRE(lod.mesh.vertices.size() == 48);
    REQUIRE(lod.mesh.quads.size() == 16);
    REQUIRE(lod.mesh.triangles.empty());
    REQUIRE(lod.mesh.quads[0].indices[0] == 30);
    REQUIRE(lod.mesh.quads[0].indices[3] == 27);
    // The single section covers the whole face stream, so every face draws with
    // its texture; an unassigned face would come out as UINT32_MAX.
    for (const auto& quad : lod.mesh.quads)
        REQUIRE(quad.materialIndex == 0);
    REQUIRE(lod.mesh.materials.size() == 1);
    REQUIRE(lod.mesh.materials[0].texturePath == "a3\\3den\\objects\\data\\area_ca.paa");
    REQUIRE(lod.mesh.materials[0].materialPath == "a3\\3den\\objects\\data\\area.rvmat");
    REQUIRE(lod.mesh.materials[0].embeddedStages.size() == 8);
    REQUIRE(lod.mesh.materials[0].embeddedStages[1].sourceStage == 1);
    REQUIRE(lod.mesh.vertices[0].position.x == -0.5f);
    REQUIRE(lod.mesh.vertices[0].normal.z == 1.0f);
    REQUIRE(lod.mesh.vertices[0].uv.u == Catch::Approx(0.250250f).margin(1e-5));

    // The second LOD is the geometry LOD and is genuinely empty. It is converted
    // rather than dropped: silently losing a LOD would move every later index.
    REQUIRE(model.lodLevels[1].mesh.vertices.empty());
    REQUIRE(model.lodLevels[1].mesh.quads.empty());
}

TEST_CASE("An ODOL 73 model outside the implemented shape is still refused", "[p3d][odol][ast-012b]")
{
    // Truncated after the revision: the narrow reader must fail as an unsupported
    // revision, not as a successful read of whatever the zeroes said.
    auto data = odolHeader(73);
    try
    {
        ODOLLoader::loadFromBuffer(data.data(), static_cast<int>(data.size()), "a3.p3d");
        FAIL("expected a non-static ODOL 73 model to be refused");
    }
    catch (const P3D::UnsupportedOdolRevision& error)
    {
        REQUIRE(error.version() == 73);
        REQUIRE(error.recognised());
        // The message has to carry the constraint that broke, or a caller has no
        // way to tell "unimplemented shape" from "corrupt file".
        REQUIRE(std::string(error.what()).find("static LODs") != std::string::npos);
    }
}

// WLD-016: the animation block is a shared Real Virtuality structure, and the two
// fields later revisions added are the whole difference between the Arma 2 and the
// Arma 3 layout. Both directions are tested from bytes rather than from a licensed
// model, because the property that matters is the block's exact width: it sits
// immediately before the LOD byte-offset table, so an over- or under-read shifts
// every offset in the file.
namespace
{
void appendU32(std::vector<char>& data, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        data.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
}

void appendFloat(std::vector<char>& data, float value)
{
    char bytes[sizeof(float)];
    std::memcpy(bytes, &value, sizeof(bytes));
    data.insert(data.end(), bytes, bytes + sizeof(bytes));
}

void appendAsciiz(std::vector<char>& data, const char* text)
{
    data.insert(data.end(), text, text + std::strlen(text));
    data.push_back('\0');
}

// One rotation class and one hide class, then the two lookup tables for a single
// resolution whose one bone drives nothing, then a sentinel. `revision` selects
// only which optional fields are written.
std::vector<char> animationBlock(uint32_t revision)
{
    std::vector<char> data;
    appendU32(data, 2); // class count

    appendU32(data, 0); // type 0: rotation
    appendAsciiz(data, "door");
    appendAsciiz(data, "user");
    for (int i = 0; i < 4; ++i)
        appendFloat(data, 0.0f); // min/max phase, min/max value
    if (revision >= 56)
    {
        appendFloat(data, 0.0f); // animPeriod
        appendFloat(data, 0.0f); // initPhase
    }
    appendU32(data, 0);          // sourceAddress
    appendFloat(data, 0.0f);     // angle0
    appendFloat(data, 1.5f);     // angle1

    appendU32(data, 9); // type 9: hide
    appendAsciiz(data, "damage");
    appendAsciiz(data, "damage");
    for (int i = 0; i < 4; ++i)
        appendFloat(data, 0.0f);
    if (revision >= 56)
    {
        appendFloat(data, 0.0f);
        appendFloat(data, 0.0f);
    }
    appendU32(data, 0);      // sourceAddress
    appendFloat(data, 0.5f); // hideValue
    if (revision >= 55)
        appendFloat(data, 0.0f); // unHideValue

    appendU32(data, 1); // one resolution
    appendU32(data, 1); // one bone in it
    appendU32(data, 0); // that bone drives no animation

    // Reverse table: one entry per (resolution, class), neither bound to a bone,
    // so neither is followed by an axis pair.
    appendU32(data, 0xffffffff);
    appendU32(data, 0xffffffff);

    data.push_back(0x7f); // sentinel: the LOD offset table would start here
    return data;
}
} // namespace

TEST_CASE("The Arma 2 animation block omits the fields revisions 55 and 56 added",
          "[p3d][odol][wld-016]")
{
    for (const uint32_t revision : {49u, 50u, 52u})
    {
        const auto data = animationBlock(revision);
        QIStream stream(data.data(), static_cast<int>(data.size()));
        Poseidon::Asset::Formats::BinaryReader reader(stream);
        const auto animations = P3D::ReadOdol73Animations(reader, revision);

        REQUIRE(animations.classes.size() == 2);
        REQUIRE(animations.classes[0].name == "door");
        REQUIRE(animations.classes[0].angle1 == Catch::Approx(1.5f));
        REQUIRE(animations.classes[1].type == 9);
        REQUIRE(animations.classes[1].hideValue == Catch::Approx(0.5f));
        REQUIRE(animations.bonesToAnimations.size() == 1);
        REQUIRE(animations.animationsToBones[0][0] == -1);
        // The block ended exactly where the LOD directory begins.
        REQUIRE(reader.read<uint8_t>() == 0x7f);
    }
}

TEST_CASE("The Arma 3 animation block still reads its later fields", "[p3d][odol][wld-016]")
{
    const auto data = animationBlock(73);
    QIStream stream(data.data(), static_cast<int>(data.size()));
    Poseidon::Asset::Formats::BinaryReader reader(stream);
    const auto animations = P3D::ReadOdol73Animations(reader);

    REQUIRE(animations.classes.size() == 2);
    REQUIRE(animations.classes[0].angle1 == Catch::Approx(1.5f));
    REQUIRE(animations.classes[1].hideValue == Catch::Approx(0.5f));
    REQUIRE(reader.read<uint8_t>() == 0x7f);
}

TEST_CASE("Reading an Arma 2 animation block as Arma 3 desynchronises it", "[p3d][odol][wld-016]")
{
    // The guard on the whole change: if the version gates were ignored, the block
    // would be read at the wrong width and the sentinel would not be where the LOD
    // directory expects it. This documents that the difference is real, not
    // defensive coding against a difference that does not exist.
    const auto data = animationBlock(49);
    QIStream stream(data.data(), static_cast<int>(data.size()));
    Poseidon::Asset::Formats::BinaryReader reader(stream);
    bool desynchronised = false;
    try
    {
        (void)P3D::ReadOdol73Animations(reader, 73);
        desynchronised = reader.read<uint8_t>() != 0x7f;
    }
    catch (const std::exception&)
    {
        desynchronised = true;
    }
    REQUIRE(desynchronised);
}

TEST_CASE("ODOL 73 face winding is measured against the model's own normals",
          "[p3d][odol][ast-012b][ast-018][corpus]")
{
    // AST-018 asks for an explicit source-geometry basis. Winding is the part
    // that can be established here without a render, because the file supplies
    // both sides of the comparison: index order and per-vertex normals. If the
    // cross product of the first two edges agrees with the stored normal the
    // indices are counter-clockwise about it; if it opposes, clockwise.
    //
    // What makes this evidence rather than a sample is unanimity. The fixture is
    // a double-sided sheet -- half its faces point inward -- so a winding rule
    // that held only for outward faces would show up here as a split.
    const char* path = std::getenv("POSEIDON_ODOL73_FIXTURE");
    if (!path || !*path)
        SKIP("set POSEIDON_ODOL73_FIXTURE to the extracted AST-012B fixture");

    const auto model = ODOLLoader::load(path);
    const auto& mesh = model.lodLevels[0].mesh;
    REQUIRE(mesh.quads.size() == 16);

    int agreeing = 0, opposing = 0, degenerate = 0;
    for (const auto& quad : mesh.quads)
    {
        const auto& a = mesh.vertices[quad.indices[0]].position;
        const auto& b = mesh.vertices[quad.indices[1]].position;
        const auto& c = mesh.vertices[quad.indices[2]].position;
        const float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
        const float vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
        const float gx = uy * vz - uz * vy, gy = uz * vx - ux * vz, gz = ux * vy - uy * vx;
        if (gx * gx + gy * gy + gz * gz < 1e-12f) { ++degenerate; continue; }
        const auto& n = mesh.vertices[quad.indices[0]].normal;
        const float sign = gx * n.x + gy * n.y + gz * n.z;
        if (sign > 0.0f) ++agreeing;
        else if (sign < 0.0f) ++opposing;
    }
    CAPTURE(agreeing, opposing, degenerate);
    REQUIRE(degenerate == 0);
    REQUIRE(agreeing == 0);
    REQUIRE(opposing == 16);
    // The conclusion the converter records, so the two cannot drift apart.
    REQUIRE(model.lodLevels[0].sourceWinding == "CW_RELATIVE_TO_STORED_NORMALS_MEASURED");
}

TEST_CASE("The ODOL 73 fixture really is double sided", "[p3d][odol][ast-018][corpus]")
{
    // Load-bearing for the winding case above: it is only unanimity across both
    // facings if both facings are present. Half the vertex normals point away
    // from the origin and half toward it.
    const char* path = std::getenv("POSEIDON_ODOL73_FIXTURE");
    if (!path || !*path)
        SKIP("set POSEIDON_ODOL73_FIXTURE to the extracted AST-012B fixture");

    const auto model = ODOLLoader::load(path);
    const auto& mesh = model.lodLevels[0].mesh;
    int outward = 0;
    for (const auto& vertex : mesh.vertices)
        if (vertex.position.x * vertex.normal.x + vertex.position.y * vertex.normal.y +
            vertex.position.z * vertex.normal.z > 0.0f)
            ++outward;
    CAPTURE(outward, mesh.vertices.size());
    REQUIRE(outward == 24);
    REQUIRE(mesh.vertices.size() == 48);
}

TEST_CASE("ODOL 73 sections carry a usable face range", "[p3d][odol][ast-018][corpus]")
{
    // Section::startTriangle/triangleCount are the source's own offsets in a
    // unit that differs per revision and is not a triangle index on any of them.
    // AST-018 asks for section boundaries a later consumer can act on, so the
    // same boundary is also published as a face range in source face order --
    // the numbering Triangle::originalIndex and Quad::originalIndex carry.
    const char* path = std::getenv("POSEIDON_ODOL73_FIXTURE");
    if (!path || !*path)
        SKIP("set POSEIDON_ODOL73_FIXTURE to the extracted AST-012B fixture");

    const auto model = ODOLLoader::load(path);
    const auto& mesh = model.lodLevels[0].mesh;
    REQUIRE(mesh.sections.size() == 1);
    const auto& section = mesh.sections[0];

    // The raw offsets stay exactly as the source gave them.
    REQUIRE(section.startTriangle == 0);
    REQUIRE(section.triangleCount == 320);
    // And the converted range covers the LOD's 16 quads.
    REQUIRE(section.faceRangeKnown);
    REQUIRE(section.firstFace == 0);
    REQUIRE(section.faceCount == 16);
    REQUIRE(section.faceCount == mesh.quads.size() + mesh.triangles.size());

    // Every face the range names must exist in source face order, or the range
    // is not usable for the thing it exists for.
    for (uint32_t face = section.firstFace; face < section.firstFace + section.faceCount; ++face)
    {
        bool found = false;
        for (const auto& quad : mesh.quads)
            if (quad.originalIndex == face) found = true;
        for (const auto& triangle : mesh.triangles)
            if (triangle.originalIndex == face) found = true;
        CAPTURE(face);
        REQUIRE(found);
    }
}

TEST_CASE("An unconverted section range says so rather than reading as empty",
          "[p3d][odol][ast-018]")
{
    // The v7 path has not had its byte offsets converted to face indices, so it
    // must report the range as unknown. A default of "known, zero faces" would
    // be indistinguishable from a section that genuinely covers nothing.
    // The default itself, which is what an unconverted path leaves in place.
    REQUIRE_FALSE(Poseidon::Model::Section().faceRangeKnown);
    REQUIRE(Poseidon::Model::Section().faceCount == 0);

    // And the v7 path in practice, for whichever fixtures carry sections. The
    // count is reported rather than required: the shipped v7 fixtures are small
    // and several have none, so demanding sections here would pin the fixture
    // set rather than the behaviour.
    const char* fixtures[] = {"p3d/animated_morph_odol.p3d", "p3d/complex_vehicle.p3d",
                              "p3d/multi_lod_vehicle.p3d", "p3d/simple_tree.p3d"};
    size_t sectionsSeen = 0, modelsRead = 0;
    for (const char* fixture : fixtures)
    {
        Poseidon::Model::Model model;
        try { model = ODOLLoader::load(GET_FIXTURE(fixture)); }
        catch (const std::exception&) { continue; }
        ++modelsRead;
        for (const auto& lod : model.lodLevels)
            for (const auto& section : lod.mesh.sections)
            {
                ++sectionsSeen;
                REQUIRE_FALSE(section.faceRangeKnown);
            }
    }
    CAPTURE(modelsRead, sectionsSeen);
    REQUIRE(modelsRead > 0);
}

// AST-010 asks for structured unsupported-feature reports, and AST-012B's claim
// rests on one fixture. Point POSEIDON_ODOL73_CORPUS at a directory of extracted
// revision-73 models and this reports, per file, whether the narrow reader takes
// it and which constraint refuses it when it does not.
//
// It deliberately does not require every model to load. The reader implements one
// shape; a refusal is the designed outcome for anything else, and the value here
// is that the refusals are specific and the successes are real.
TEST_CASE("ODOL 73 corpus survey reports what the narrow reader takes",
          "[p3d][odol][ast-012b][corpus][survey]")
{
    namespace fs = std::filesystem;
    const char* root = std::getenv("POSEIDON_ODOL73_CORPUS");
    if (!root || !*root)
        SKIP("set POSEIDON_ODOL73_CORPUS to a directory of extracted revision-73 models");
    if (!fs::exists(root))
        SKIP("POSEIDON_ODOL73_CORPUS does not exist");

    std::vector<std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (entry.is_regular_file() && entry.path().extension() == ".p3d")
            files.push_back(entry.path().string());
    std::sort(files.begin(), files.end());
    REQUIRE_FALSE(files.empty());

    size_t loaded = 0, refused = 0, notRevision73 = 0;
    std::string report;
    for (const auto& file : files)
    {
        const std::string name = fs::path(file).filename().string();
        try
        {
            const auto model = ODOLLoader::load(file);
            if (model.sourceVersion != 73) { ++notRevision73; continue; }
            ++loaded;
            // A load that produced nothing is not a load. Every accepted model
            // must carry at least one LOD and a vertex somewhere, or the survey
            // is counting empty successes.
            REQUIRE_FALSE(model.lodLevels.empty());
            size_t vertices = 0;
            for (const auto& lod : model.lodLevels) vertices += lod.mesh.vertices.size();
            CAPTURE(name, vertices);
            REQUIRE(vertices > 0);
            report += "  LOADED   " + name + "\n";
        }
        catch (const P3D::UnsupportedOdolRevision& error)
        {
            ++refused;
            report += "  REFUSED  " + name + ": " + error.what() + "\n";
        }
        catch (const std::exception& error)
        {
            ++refused;
            report += "  ERROR    " + name + ": " + error.what() + "\n";
        }
    }
    WARN("ODOL 73 corpus survey (" << files.size() << " files, " << loaded << " loaded, " << refused
                                   << " refused, " << notRevision73 << " other revisions):\n"
                                   << report);
    // The survey must have reached a verdict on every file it listed.
    REQUIRE(loaded + refused + notRevision73 == files.size());
    // And at least one revision-73 model must load, or the reader has regressed
    // to accepting nothing and every refusal below would look like good news.
    REQUIRE(loaded > 0);
}

// Reachability through the production loader, not just through ODOLLoader.
//
// ModelCache is what the game calls, and it asks P3DFormatDetector whether a
// file is supported *before* handing it to any reader. That detector hardcoded
// {7, 8}, so revision 73 was rejected before the AST-012B reader was ever
// consulted: the reader was reachable from its own tests and from nothing else.
// This pins the fix at the layer that actually gates the runtime.
TEST_CASE("ODOL 73 reaches the production model-loading path", "[p3d][odol][ast-012b][corpus][survey]")
{
    namespace fs = std::filesystem;
    const char* root = std::getenv("POSEIDON_ODOL73_CORPUS");
    if (!root || !*root)
        SKIP("set POSEIDON_ODOL73_CORPUS to a directory of extracted revision-73 models");

    std::string sample;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (entry.is_regular_file() && entry.path().extension() == ".p3d")
        {
            // Pick one the narrow reader accepts; refusals are a separate case.
            try { if (ODOLLoader::load(entry.path().string()).sourceVersion == 73) { sample = entry.path().string(); break; } }
            catch (const std::exception&) { continue; }
        }
    if (sample.empty())
        SKIP("no acceptable revision-73 model in POSEIDON_ODOL73_CORPUS");
    CAPTURE(sample);

    // The gate itself: the detector must report the revision supported.
    const auto info = Poseidon::Asset::Formats::P3DFormatDetector::DetectFormat(sample);
    REQUIRE(info.signature == "ODOL");
    REQUIRE(info.version == 73);
    REQUIRE(info.isSupported);

    // And the production loader must return real geometry, not merely not-null.
    Poseidon::ModelCache cache;
    auto model = cache.load(sample);
    REQUIRE(model != nullptr);
    REQUIRE(model->sourceVersion == 73);
    REQUIRE_FALSE(model->lodLevels.empty());
    size_t vertices = 0, faces = 0;
    for (const auto& lod : model->lodLevels)
    {
        vertices += lod.mesh.vertices.size();
        faces += lod.mesh.triangles.size() + lod.mesh.quads.size();
    }
    CAPTURE(vertices, faces);
    REQUIRE(vertices > 0);
    REQUIRE(faces > 0);
}
