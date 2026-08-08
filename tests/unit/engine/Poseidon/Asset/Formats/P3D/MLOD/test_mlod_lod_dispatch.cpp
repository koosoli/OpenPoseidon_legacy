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
#include <algorithm>
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

TEST_CASE("MLOD LOD dispatch: P3DM now loads through the same converter", "[mlod][dispatch][ast-011b]")
{
    // This case previously asserted P3DM was refused. AST-011B implements it, so
    // the assertion inverts rather than being deleted -- the routing it covers is
    // still the thing under test.
    auto model = MLODLoader::load(GET_FIXTURE("mlod/p3dm_two_lod.p3d"));
    REQUIRE(model.sourceFormat == "MLOD");
    REQUIRE(model.lodLevels.size() == 2);
    REQUIRE(model.lodLevels[0].sourceEncoding == "P3DM");
    REQUIRE(model.lodLevels[1].sourceEncoding == "P3DM");
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

// ── AST-011B: P3DM faces, texture/material references, winding metadata ──────

TEST_CASE("P3DM: faces, counts and LOD resolutions", "[mlod][p3dm][ast-011b]")
{
    auto model = MLODLoader::load(GET_FIXTURE("mlod/p3dm_two_lod.p3d"));
    REQUIRE(model.lodLevels.size() == 2);

    const auto& visual = model.lodLevels[0];
    REQUIRE(visual.resolution == 1.0f);
    // One triangle and one quad, kept as their source primitive rather than
    // silently triangulated.
    REQUIRE(visual.mesh.triangles.size() == 1);
    REQUIRE(visual.mesh.quads.size() == 1);

    // The Geometry LOD's resolution must survive as the exact sentinel, because
    // that value is how a special LOD is identified at all.
    REQUIRE(model.lodLevels[1].resolution == 1.0e13f);
}

TEST_CASE("P3DM: texture and RVMAT references are both preserved", "[mlod][p3dm][ast-011b]")
{
    auto        model  = MLODLoader::load(GET_FIXTURE("mlod/p3dm_two_lod.p3d"));
    const auto& mesh   = model.lodLevels[0].mesh;

    // Two faces: one with a texture and an RVMAT, one with only the RVMAT. Keying
    // materials on the texture alone would merge them and lose a material.
    REQUIRE(mesh.materials.size() == 2);

    bool sawTextured = false, sawMaterialOnly = false;
    for (const auto& material : mesh.materials)
    {
        // The full path must survive: it is 26 characters, so a 32-byte SP3X-style
        // fixed field would have held it and hidden the truncation bug entirely.
        REQUIRE(material.materialPath == "ca\\test\\data\\slab.rvmat");
        if (!material.texturePath.empty())
        {
            REQUIRE(material.texturePath == "ca\\test\\data\\slab_co.tga");
            sawTextured = true;
        }
        else
        {
            // With no texture the name falls back to the RVMAT rather than to a
            // shared "#default#" bucket.
            REQUIRE(material.name == "ca\\test\\data\\slab.rvmat");
            sawMaterialOnly = true;
        }
    }
    REQUIRE(sawTextured);
    REQUIRE(sawMaterialOnly);
}

TEST_CASE("P3DM: UV sets are counted, not collapsed", "[mlod][p3dm][ast-011b]")
{
    auto model = MLODLoader::load(GET_FIXTURE("mlod/p3dm_two_lod.p3d"));
    // AST-011C reads the additional channels; AST-011B must at least not lose the
    // fact that they exist, or that ticket has nothing to assert against.
    REQUIRE(model.lodLevels[0].uvSetCount == 2);
    REQUIRE(model.lodLevels[1].uvSetCount == 1);
}

TEST_CASE("P3DM: named properties survive the differing TAGG encoding", "[mlod][p3dm][ast-011b]")
{
    auto        model = MLODLoader::load(GET_FIXTURE("mlod/p3dm_two_lod.p3d"));
    const auto& props = model.lodLevels[0].mesh.properties;

    // P3DM tags are `active byte, asciiz name, size`; SP3X tags are a fixed
    // 64-byte name plus size. Reading one with the other's layout desynchronises,
    // so a correctly recovered property is evidence the right decoder ran.
    REQUIRE(props.size() == 1);
    REQUIRE(props[0].name == "lodnoshadow");
    REQUIRE(props[0].value == "1");
}

TEST_CASE("P3DM: source winding is recorded as an assumption", "[mlod][p3dm][ast-011b]")
{
    auto model = MLODLoader::load(GET_FIXTURE("mlod/p3dm_two_lod.p3d"));
    // The _ASSUMED suffix is load-bearing: nothing has rendered a P3DM model, so
    // the shared winding convention is unverified. If this ever silently becomes
    // "confirmed", that happened without evidence.
    REQUIRE(model.lodLevels[0].sourceWinding == "CW_REVERSED_ON_LOAD_ASSUMED");

    auto sp3x = MLODLoader::load(GET_FIXTURE("mlod/complex_vehicle_mlod.p3d"));
    REQUIRE(sp3x.lodLevels[0].sourceWinding == "CW_REVERSED_ON_LOAD");
    REQUIRE(sp3x.lodLevels[0].sourceEncoding == "SP3X");
}

TEST_CASE("P3DM: a wrong version is refused rather than desynchronised", "[mlod][p3dm][ast-011b]")
{
    // P3DM has no headSize to skip by, so an unexpected header size cannot be
    // tolerated -- everything after it would be read at the wrong offset.
    auto data = loadFile(GET_FIXTURE("mlod/p3dm_two_lod.p3d"));
    REQUIRE(data.size() > 20);
    data[16] = 0x1D; // major 28 -> 29

    REQUIRE_THROWS_AS(
        MLODLoader::loadFromBuffer(data.data(), static_cast<int>(data.size()), "corrupt"),
        std::runtime_error);
}

// -- AST-011C: #UVSet# decoding and no UV1 -> UV0 collapse --------------------

TEST_CASE("P3DM: every UV channel is decoded and kept in source order", "[mlod][p3dm][ast-011c]")
{
    auto        model = MLODLoader::load(GET_FIXTURE("mlod/p3dm_two_lod.p3d"));
    const auto& visual = model.lodLevels[0];

    REQUIRE(visual.uvSetCount == 2);
    REQUIRE(visual.uvChannels.size() == 2);
    REQUIRE(visual.uvChannels[0].id == 0);
    REQUIRE(visual.uvChannels[1].id == 1);

    // One entry per ACTUAL face vertex: a triangle plus a quad is 7, not 8. Reading
    // the four reserved slots instead would give 8 and shift every later value.
    REQUIRE(visual.uvChannels[0].faceVertexUVs.size() == 7);
    REQUIRE(visual.uvChannels[1].faceVertexUVs.size() == 7);

    // Channel 1 is offset from channel 0 by construction, so equal values here
    // would mean one channel had overwritten the other.
    REQUIRE(visual.uvChannels[0].faceVertexUVs[0].u == 0.0f);
    REQUIRE(visual.uvChannels[1].faceVertexUVs[0].u == 1.0f);
    REQUIRE(visual.uvChannels[1].faceVertexUVs[0].v == 0.5f);
}

TEST_CASE("P3DM: vertices differing only in UV1 are not merged", "[mlod][p3dm][ast-011c]")
{
    // Two coincident triangles: identical positions, normals and uv0, differing
    // only in channel 1. Merging on uv0 alone would fold them into three shared
    // vertices and discard uv1 -- the collapse this ticket forbids.
    auto        model = MLODLoader::load(GET_FIXTURE("mlod/p3dm_uv_collapse.p3d"));
    const auto& mesh  = model.lodLevels[0].mesh;

    REQUIRE(mesh.triangles.size() == 2);
    REQUIRE(mesh.vertices.size() == 6);

    const auto& first  = mesh.vertices[mesh.triangles[0].indices[0]];
    const auto& second = mesh.vertices[mesh.triangles[1].indices[0]];
    REQUIRE(first.uv.u == second.uv.u);      // same base channel
    REQUIRE(first.uv1.u != second.uv1.u);    // different second channel
}

TEST_CASE("P3DM: a mis-sized UV block is skipped, not guessed at", "[mlod][p3dm][ast-011c]")
{
    // The fixture's second #UVSet# is internally consistent -- its declared size
    // matches its own payload, so the stream stays in sync -- but it is one pair
    // short of the face-vertex total. Decoding it anyway would put
    // plausible-looking wrong UVs on the mesh, which is far harder to notice than
    // a channel that is simply absent.
    auto        model = MLODLoader::load(GET_FIXTURE("mlod/p3dm_uv_bad_size.p3d"));
    const auto& lod   = model.lodLevels[0];

    // The count still records that the source carried two, so the discrepancy is
    // visible rather than erased.
    REQUIRE(lod.uvSetCount == 2);
    REQUIRE(lod.uvChannels.size() == 1);
    REQUIRE(lod.uvChannels[0].id == 0);
    REQUIRE(lod.uvChannels[0].faceVertexUVs.size() == 3);
}

TEST_CASE("SP3X: no UV channels and a zero second channel", "[mlod][p3dm][ast-011c]")
{
    // uv1 joins vertex identity, so it must stay zero for SP3X or the merge would
    // start splitting vertices in models that have no second channel at all.
    auto        model = MLODLoader::load(GET_FIXTURE("mlod/complex_vehicle_mlod.p3d"));
    const auto& lod   = model.lodLevels[0];
    REQUIRE(lod.uvChannels.empty());
    REQUIRE(lod.uvSetCount == 0);
    for (const auto& vertex : lod.mesh.vertices)
    {
        REQUIRE(vertex.uv1.u == 0.0f);
        REQUIRE(vertex.uv1.v == 0.0f);
    }
}
