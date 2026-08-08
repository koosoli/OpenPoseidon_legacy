// test_odol_revision.cpp - AST-012A: versioned ODOL dispatch.
//
// Architecture only. This routes on the exact revision and refuses everything it
// cannot read; it claims support for no later revision. The revisions named are
// those AST-007 found in the owner's corpora (7, 48, 49, 50, 52, 73) -- the only
// population any claim here can honestly cover.

#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Asset/Formats/P3D/ODOLLoader.hpp>
#include <Poseidon/Asset/Formats/P3D/OdolRevision.hpp>
#include <Poseidon/Asset/Formats/P3D/Odol73.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include "../../../../test_fixtures.hpp"
#include <cstring>
#include <cstdlib>
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
    for (uint32_t version : {48u, 49u, 50u, 52u, 73u})
    {
        const auto info = P3D::DescribeOdolRevision(version);
        // Recognised means "present in the corpora", never "supported".
        REQUIRE(info.support == P3D::OdolSupport::Recognised);
        REQUIRE(std::string(info.generation) != "unknown");
    }
    REQUIRE(P3D::DescribeOdolRevision(9999).support == P3D::OdolSupport::Unknown);
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
    auto data = odolHeader(49);
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
    const auto polygons = P3D::ReadOdol73StaticPolygonPrefix(reader, directory.ends[0]);
    REQUIRE_FALSE(polygons.indexStreamMarker);
    REQUIRE(polygons.unparsedIndexValue == 26);
    REQUIRE(polygons.polygons.size() == 11);
    REQUIRE(polygons.polygons[0] == std::vector<uint32_t>{39, 35, 31, 43});
    REQUIRE(polygons.followingByteOffset == 0x748);
    const auto sections = P3D::ReadOdol73StaticSections(reader, directory.ends[0]);
    REQUIRE(sections.size() == 1);
    REQUIRE(sections[0].faceLower == 0);
    REQUIRE(sections[0].faceUpper == 320);
    REQUIRE(sections[0].materialIndex == 0);
    const auto preRest = P3D::ReadOdol73StaticPreRestData(reader, directory.ends[0]);
    REQUIRE(preRest.colorTop == static_cast<int32_t>(0xff000000));
    REQUIRE(preRest.restDataSize == 613);
}
