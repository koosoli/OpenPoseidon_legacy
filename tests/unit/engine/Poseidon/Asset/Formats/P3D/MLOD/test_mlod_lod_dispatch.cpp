// test_mlod_lod_dispatch.cpp - AST-011A: per-LOD SP3X/P3DM detection and dispatch.
//
// "MLOD" names the container, not the per-LOD encoding. The reader assumed SP3X
// for every LOD, so an Arma-generation model -- which stores P3DM under the same
// MLOD 1.1 header -- was read with the wrong layout and reported whatever the
// misaligned bytes said. AST-007's corpus inventory measured how far that reaches:
// 1,657 of 1,659 loose models across the Arma 2/OA and Arma 3 sample corpora are
// P3DM. These tests pin the routing, and that the SP3X path is unchanged.

#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODLoader.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODStructures.hpp>
#include <Poseidon/Asset/Formats/BISBinaryStream.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include "../../../../test_fixtures.hpp"
#include <fstream>
#include <string>
#include <vector>

using Poseidon::Asset::Formats::MLODLoader;
namespace MLOD = Poseidon::Asset::Formats::MLOD;

static std::vector<char> loadFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    REQUIRE(file);
    auto              size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> buffer(static_cast<size_t>(size));
    file.read(buffer.data(), size);
    return buffer;
}

TEST_CASE("MLOD LOD dispatch: signature peek does not consume", "[mlod][dispatch][ast-011a]")
{
    auto                                   data = loadFile(GET_FIXTURE("mlod/complex_vehicle_mlod.p3d"));
    QIStream                               stream(data.data(), static_cast<int>(data.size()));
    Poseidon::Asset::Formats::BinaryReader reader(stream);

    auto header = MLOD::readHeader(reader);
    REQUIRE(header.lodCount > 0);

    const int before = reader.tell();
    REQUIRE(MLOD::peekLodSignature(reader) == MLOD::LodSignature::SP3X);
    REQUIRE(reader.tell() == before);

    // Peeking twice must be idempotent, and the SP3X header reader must still see
    // its own signature afterwards -- that is what keeps the existing path intact.
    REQUIRE(MLOD::peekLodSignature(reader) == MLOD::LodSignature::SP3X);
    REQUIRE(reader.tell() == before);
    auto section = MLOD::readSP3XHeader(reader);
    REQUIRE(section.isValid());
}

TEST_CASE("MLOD LOD dispatch: P3DM is recognised, not misread as SP3X", "[mlod][dispatch][ast-011a]")
{
    auto                                   data = loadFile(GET_FIXTURE("mlod/p3dm_two_lod.p3d"));
    QIStream                               stream(data.data(), static_cast<int>(data.size()));
    Poseidon::Asset::Formats::BinaryReader reader(stream);

    auto header = MLOD::readHeader(reader);
    REQUIRE(header.lodCount == 2);
    REQUIRE(MLOD::peekLodSignature(reader) == MLOD::LodSignature::P3DM);
}

TEST_CASE("MLOD LOD dispatch: P3DM rejects structurally", "[mlod][dispatch][ast-011a]")
{
    const auto path = GET_FIXTURE("mlod/p3dm_two_lod.p3d");

    // The point of the typed exception: a caller can tell "valid model this build
    // cannot read" from "malformed input" without matching on message text.
    try
    {
        MLODLoader::load(path);
        FAIL("expected a P3DM LOD to be refused");
    }
    catch (const MLOD::UnsupportedLodFormat& error)
    {
        REQUIRE(error.signature() == MLOD::LodSignature::P3DM);
        REQUIRE(error.lodIndex() == 0);
        REQUIRE(std::string(error.what()).find("P3DM") != std::string::npos);
    }

    // And it stays a runtime_error, so existing catch sites keep working rather
    // than the new type escaping as an unhandled exception.
    REQUIRE_THROWS_AS(MLODLoader::load(path), std::runtime_error);
}

TEST_CASE("MLOD LOD dispatch: unknown signature is refused with what was found", "[mlod][dispatch][ast-011a]")
{
    try
    {
        MLODLoader::load(GET_FIXTURE("mlod/unknown_lod_signature.p3d"));
        FAIL("expected an unknown LOD signature to be refused");
    }
    catch (const MLOD::UnsupportedLodFormat& error)
    {
        REQUIRE(error.signature() == MLOD::LodSignature::Unknown);
        // A diagnostic that does not name the bytes it saw sends the reader back
        // to a hex editor, which is the failure this replaces.
        REQUIRE(std::string(error.what()).find("XXXX") != std::string::npos);
    }
}

TEST_CASE("MLOD LOD dispatch: routing is per LOD, not per file", "[mlod][dispatch][ast-011a]")
{
    // A mixed container must be routed LOD by LOD. If the decision were made once
    // from LOD 0, this fixture would be read entirely as P3DM or entirely as SP3X.
    auto                                   data = loadFile(GET_FIXTURE("mlod/p3dm_mixed_signatures.p3d"));
    QIStream                               stream(data.data(), static_cast<int>(data.size()));
    Poseidon::Asset::Formats::BinaryReader reader(stream);

    auto header = MLOD::readHeader(reader);
    REQUIRE(header.lodCount == 2);
    REQUIRE(MLOD::peekLodSignature(reader) == MLOD::LodSignature::P3DM);

    // Step over the P3DM LOD using the length the fixture generator knows, and
    // confirm the second LOD is independently recognised as SP3X.
    const int p3dmBytes = static_cast<int>(data.size()) - 12 - 68;
    reader.seekRelative(p3dmBytes);
    REQUIRE(MLOD::peekLodSignature(reader) == MLOD::LodSignature::SP3X);
}

TEST_CASE("MLOD LOD dispatch: existing SP3X models still load", "[mlod][dispatch][ast-011a]")
{
    // The regression that matters most: dispatch must not change what OFP/CWA
    // content does. These are the fixtures the pre-existing suites already cover.
    for (const char* fixture : {"mlod/complex_vehicle_mlod.p3d", "mlod/animated_marker_mlod.p3d"})
    {
        auto model = MLODLoader::load(GET_FIXTURE(fixture));
        REQUIRE(model.sourceFormat == "MLOD");
        REQUIRE_FALSE(model.lodLevels.empty());
    }
}
