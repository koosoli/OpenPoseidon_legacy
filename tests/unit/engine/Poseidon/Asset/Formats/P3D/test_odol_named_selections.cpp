// test_odol_named_selections.cpp -- AST-019.
//
// A named selection's `selectedFaces` array holds FACE INDICES. A section's
// `faceLower`/`faceUpper` hold FACE-STREAM BYTE OFFSETS. They sit in the same LOD
// and they are not the same unit, and that is exactly how the converter came to
// run the selections through the section's offset table and get nothing back.
//
// The fixture is built to make the two readings disagree at every entry rather
// than to be merely valid:
//
//   * the face list mixes triangles and a quad, so the byte offsets are 0, 8, 16,
//     26, 34, 42 -- NOT an arithmetic series. A stride-based reading cannot pass
//     by accident.
//   * one selected entry is 0, which is simultaneously a valid index and a valid
//     offset. That coincidence is the only reason the old code ever produced a
//     correct index, and it is measured: 461 of the 41,058 entries it emitted
//     across 150 structures_e models were right, and 40,597 were the wrong face.
//   * one entry is a valid offset but NOT a valid index, so a reader that still
//     believed the offset story would emit a face here and this fails.
//   * one entry is a valid index that is not on any byte boundary -- the case the
//     old code dropped silently.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <Poseidon/Asset/Formats/P3D/ODOLLoader.hpp>
#include <cstdint>
#include <string>
#include <vector>

using namespace Poseidon::Asset::Formats;
using namespace Poseidon::Asset::Formats::P3D;

// The format's own Vector3, not the engine's: `Vector3` is ambiguous here.
using BisVector3 = Poseidon::Asset::Formats::Vector3;

namespace
{

constexpr uint32_t kVertexCount = 12;

// Six faces: T T Q T T T. Byte offsets under the Arma 2 family's 2-byte header
// and 2-byte indices are 0, 8, 16, 26, 34, 42, and the stream is 50 bytes.
Odol73StaticModel BuildModel(uint32_t revision)
{
    Odol73StaticModel model;
    model.directory.model.boundingSphere = 1.0f;
    // One LOD, and the preamble is where its resolution lives.
    model.directory.model.resolutions.push_back(1.0f);

    Odol73StaticLod lod;
    lod.header.vertexCount = kVertexCount;
    lod.header.bboxMin = BisVector3{-1.0f, -1.0f, -1.0f};
    lod.header.bboxMax = BisVector3{1.0f, 1.0f, 1.0f};
    lod.header.textures.push_back("ca\\test\\stone_co.paa");

    lod.polygons.faceStreamHeaderBytes = revision == 73 ? 4 : 2;
    lod.polygons.faceStreamIndexBytes = revision == 73 ? 4 : 2;
    lod.polygons.faces = {
        {0, 1, 2}, {1, 2, 3}, {2, 3, 4, 5}, {4, 5, 6}, {6, 7, 8}, {8, 9, 10},
    };
    uint32_t stream = 0;
    for (const auto& face : lod.polygons.faces)
        stream +=
            lod.polygons.faceStreamHeaderBytes + lod.polygons.faceStreamIndexBytes * static_cast<uint32_t>(face.size());
    lod.polygons.faceDataSize = stream;

    // One section covering faces 2, 3 and 4 by BYTE OFFSET. On the A2 stride that
    // is [16, 42); the A3 stride doubles it. Expressed by rebuilding rather than
    // hard-coded, so the case says "the third face onwards" in both.
    uint32_t offsets[7] = {};
    uint32_t at = 0;
    for (size_t i = 0; i < lod.polygons.faces.size(); ++i)
    {
        offsets[i] = at;
        at += lod.polygons.faceStreamHeaderBytes +
              lod.polygons.faceStreamIndexBytes * static_cast<uint32_t>(lod.polygons.faces[i].size());
    }
    offsets[6] = at;

    Odol73StaticSection section;
    section.faceLower = static_cast<int32_t>(offsets[2]);
    section.faceUpper = static_cast<int32_t>(offsets[5]);
    section.textureIndex = 0;
    section.materialIndex = -1;
    lod.sections.push_back(section);

    // An ordinary selection. Entry 0 is both a valid index and a valid offset;
    // 3 and 5 are indices that are not offsets; `offsets[1]` is an offset that is
    // NOT a valid index and must be rejected.
    Odol73NamedSelection component;
    component.name = "component01";
    component.selectedFaces = {0, 3, static_cast<int32_t>(offsets[1]), 5};
    component.selectedVertices = {0, 1, 2, 3};
    lod.namedSelections.push_back(component);

    // The proxy-marker shape, measured 4,690 times across 150 structures_e
    // models: three vertices, one face, no section list.
    Odol73NamedSelection proxy;
    proxy.name = "proxy:\\ca\\structures_e\\misc\\misc_interier\\chair_ep1.001";
    proxy.selectedFaces = {4};
    proxy.selectedVertices = {6, 7, 8};
    lod.namedSelections.push_back(proxy);

    lod.rest.positions.assign(kVertexCount, BisVector3{0.0f, 0.0f, 0.0f});
    lod.rest.normals.assign(kVertexCount, BisVector3{0.0f, 1.0f, 0.0f});
    lod.rest.clip.assign(kVertexCount, 0);
    lod.rest.uv0.uv.assign(kVertexCount, {0.0f, 0.0f});
    lod.rest.uvSetCount = 1;

    model.lods.push_back(std::move(lod));
    return model;
}

const Poseidon::Model::NamedSelection& SelectionNamed(const Poseidon::Model::Mesh& mesh, const std::string& name)
{
    for (const auto& selection : mesh.selections)
        if (selection.name == name)
            return selection;
    throw std::runtime_error("selection not found: " + name);
}

} // namespace

TEST_CASE("ODOL selections: selectedFaces are face indices, not face-stream offsets", "[Formats][ODOL][AST-019]")
{
    // Both generations. The unit is the same on each -- this was never an Arma 2
    // quirk, and reading it as offsets emptied Arma 3 selections just as thoroughly
    // (382 of 382 non-empty selections in an ODOL 73 hospital are index-valid and
    // 0 are offset-valid).
    for (const uint32_t revision : {49u, 73u})
    {
        INFO("ODOL revision " << revision);
        const auto model = ODOLLoader::convertStaticModel(BuildModel(revision), "test.p3d", revision);
        REQUIRE(model.lodLevels.size() == 1);
        const auto& mesh = model.lodLevels[0].mesh;

        const auto& component = SelectionNamed(mesh, "component01");
        // 0, 3 and 5 are the valid face indices. The fourth entry is a byte offset
        // that is out of range as an index and must not survive.
        REQUIRE(component.triangleIndices == std::vector<uint32_t>{0, 3, 5});
        REQUIRE(component.vertexIndices.size() == 4);
    }
}

TEST_CASE("ODOL selections: a proxy marker keeps the one face it selects", "[Formats][ODOL][AST-019]")
{
    // This is the whole proxy defect in one assertion. The marker selection names
    // exactly one face; with it, ShapeAdapter's face route can mark the section
    // IsHiddenProxy, and without it the marker triangle is baked as ordinary
    // untextured geometry -- 1,898 markers across 40 of Takistan's 513 models.
    const auto model = ODOLLoader::convertStaticModel(BuildModel(49), "test.p3d", 49);
    const auto& mesh = model.lodLevels[0].mesh;
    const auto& proxy = SelectionNamed(mesh, "proxy:\\ca\\structures_e\\misc\\misc_interier\\chair_ep1.001");
    REQUIRE(proxy.triangleIndices == std::vector<uint32_t>{4});
    REQUIRE(proxy.vertexIndices == std::vector<uint32_t>{6, 7, 8});
    // And the section list really is empty -- in the file, not just here. It is
    // empty in 1,171 of 1,171 selections of an ODOL 49 mosque and 414 of 414 of an
    // ODOL 73 hospital, so it can never serve as the fallback for the face list.
    REQUIRE(proxy.sectionIndices.empty());
}

TEST_CASE("ODOL selections: section bounds stay byte offsets", "[Formats][ODOL][AST-019]")
{
    // The guard against overcorrecting. Sections and selections disagree about
    // their unit, and this proves the section half was left alone: the section
    // declares [offsets[2], offsets[5]) and must cover faces 2, 3 and 4.
    for (const uint32_t revision : {49u, 73u})
    {
        INFO("ODOL revision " << revision);
        const auto model = ODOLLoader::convertStaticModel(BuildModel(revision), "test.p3d", revision);
        const auto& mesh = model.lodLevels[0].mesh;
        REQUIRE(mesh.sections.size() == 1);
        REQUIRE(mesh.sections[0].faceRangeKnown);
        REQUIRE(mesh.sections[0].firstFace == 2);
        REQUIRE(mesh.sections[0].faceCount == 3);
    }
}

TEST_CASE("ODOL selections: the two units disagree at every entry but one", "[Formats][ODOL][AST-019]")
{
    // Stated as a property of the fixture rather than trusted: if the offsets ever
    // became an arithmetic series matching the indices, the cases above would pass
    // under either reading and stop being evidence.
    const auto source = BuildModel(49);
    const auto& polygons = source.lods[0].polygons;
    const auto offsets = Odol73FaceStreamOffsets(polygons);
    REQUIRE(offsets.back() == polygons.faceDataSize);
    // The quad at index 2 is what breaks the stride.
    REQUIRE(offsets[3] - offsets[2] == 10);
    REQUIRE(offsets[1] - offsets[0] == 8);
    size_t coincidences = 0;
    for (size_t face = 0; face < polygons.faces.size(); ++face)
        if (offsets[face] == face)
            ++coincidences;
    REQUIRE(coincidences == 1); // face 0 only
}
