// test_odol_revision.cpp - AST-012A: versioned ODOL dispatch.
//
// Architecture only. This routes on the exact revision and refuses everything it
// cannot read; it claims support for no later revision. The revisions named are
// those AST-007 found in the owner's corpora (7, 48, 49, 50, 52, 73) -- the only
// population any claim here can honestly cover.

#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Asset/Formats/P3D/ODOLLoader.hpp>
#include <Poseidon/Asset/Formats/P3D/OdolRevision.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include "../../../../test_fixtures.hpp"
#include <cstring>
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
