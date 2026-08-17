#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <Poseidon/Asset/Formats/P3D/MLODLoader.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODWriter.hpp>
#include <Poseidon/World/Model/Model.hpp>

#include "../../../test_fixtures.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using Poseidon::Asset::Formats::MLODLoader;
using Poseidon::Asset::Formats::MLODWriter;
namespace MLODW = Poseidon::Asset::Formats::MLOD;

namespace
{

// The reader reverses face winding on load (indices 0/1 on a triangle, 0/1 and
// 2/3 on a quad), so a test that compares what it wrote against what came back
// has to undo the same swap. Asserting on the raw loaded order instead would
// bake the reader's convention into the writer's contract.
uint32_t triangleCorner(const Poseidon::Model::Triangle& tri, int corner)
{
    static const int unswap[3] = {1, 0, 2};
    return tri.indices[unswap[corner]];
}

uint32_t quadCorner(const Poseidon::Model::Quad& quad, int corner)
{
    static const int unswap[4] = {1, 0, 3, 2};
    return quad.indices[unswap[corner]];
}

// A six-point mesh: one triangle and one quad sharing point 2. The share is the
// point of the fixture -- it exercises the reader's per-point vertex merge, which
// is the step that would silently reorder or collapse vertices if the writer got
// the point table wrong.
MLODW::WriteModel buildTestModel()
{
    const Poseidon::Asset::Formats::Vector3 positions[6] = {
        {0.0f, 0.0f, 0.0f}, {1.25f, 0.0f, 0.0f},  {1.25f, 2.5f, 0.0f},
        {0.0f, 2.5f, 0.0f}, {-1.5f, 2.5f, 0.75f}, {-1.5f, 0.0f, 0.75f},
    };
    // Deliberately not unit vectors and deliberately far apart: the reader merges
    // vertices whose normals agree within 0.05, so near-identical normals would
    // hide a writer that mixed the indices up.
    const Poseidon::Asset::Formats::Vector3 normals[6] = {
        {0.0f, 0.0f, 1.0f},  {0.0f, 1.0f, 0.0f},  {1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, -1.0f}, {0.0f, -1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f},
    };

    MLODW::WriteLod visual;
    visual.resolution = 1.0f;
    visual.secondUVChannel = true;
    for (int i = 0; i < 6; ++i)
    {
        visual.points.push_back({positions[i], 0});
        visual.normals.push_back(normals[i]);
    }

    // UVs keyed off the point index, so the shared point carries the same pair in
    // both faces and the reader is expected to merge it into one vertex.
    auto cornerFor = [](int32_t point)
    {
        MLODW::WriteFaceVertex vertex;
        vertex.point = point;
        vertex.normal = point;
        vertex.u = 0.125f * static_cast<float>(point);
        vertex.v = 0.25f * static_cast<float>(point) + 0.5f;
        vertex.u1 = 0.75f - 0.0625f * static_cast<float>(point);
        vertex.v1 = 0.5f * static_cast<float>(point);
        return vertex;
    };

    MLODW::WriteFace tri;
    tri.vertexCount = 3;
    tri.flags = 0x20;
    tri.texture = "ca\\test\\data\\hull_co.paa";
    tri.material = "ca\\test\\data\\hull.rvmat";
    for (int i = 0; i < 3; ++i)
        tri.vertices[i] = cornerFor(i);

    // No texture, only an RVMAT -- the common Arma case that a texture-keyed
    // material table would fold into the triangle's material.
    MLODW::WriteFace quad;
    quad.vertexCount = 4;
    quad.flags = 0;
    quad.material = "ca\\test\\data\\glass.rvmat";
    const int32_t quadPoints[4] = {2, 3, 4, 5};
    for (int i = 0; i < 4; ++i)
        quad.vertices[i] = cornerFor(quadPoints[i]);

    visual.faces.push_back(tri);
    visual.faces.push_back(quad);
    visual.properties.push_back({"lodnoshadow", "1"});

    // 1e13 is the Geometry sentinel (World/Model/LodPurpose.hpp); it is written
    // verbatim and classified by the reader, so a writer that rounded it would
    // turn a geometry LOD into a visual one at an absurd draw distance.
    MLODW::WriteLod geometry;
    geometry.resolution = 1.0e13f;
    for (int i = 0; i < 3; ++i)
    {
        geometry.points.push_back({positions[i], 0});
        geometry.normals.push_back(normals[i]);
    }
    MLODW::WriteFace geoFace;
    geoFace.vertexCount = 3;
    for (int i = 0; i < 3; ++i)
    {
        geoFace.vertices[i].point = i;
        geoFace.vertices[i].normal = i;
    }
    geometry.faces.push_back(geoFace);

    MLODW::WriteModel model;
    model.lods.push_back(visual);
    model.lods.push_back(geometry);
    return model;
}

} // namespace

TEST_CASE("MLODWriter: authored mesh survives a write/read round trip", "[mlod][writer]")
{
    const MLODW::WriteModel source = buildTestModel();
    const std::vector<char> bytes = MLODWriter::writeToBuffer(source);

    auto model = MLODLoader::loadFromBuffer(bytes.data(), static_cast<int>(bytes.size()), "memory");

    REQUIRE(model.sourceFormat == "MLOD");
    REQUIRE(model.sourceVersion == 11); // 1.1
    REQUIRE(model.lodLevels.size() == 2);

    const auto& lod = model.lodLevels[0];
    const auto& mesh = lod.mesh;

    REQUIRE(lod.sourceEncoding == "P3DM");
    REQUIRE(lod.resolution == 1.0f);
    REQUIRE(lod.purpose == Poseidon::Model::LodPurpose::Visual);

    // Six points, six vertices: the shared point 2 carries identical attributes in
    // both faces, so the reader must merge it rather than split it.
    REQUIRE(mesh.vertices.size() == 6);
    for (size_t i = 0; i < 6; ++i)
    {
        const auto& expected = source.lods[0].points[i].position;
        const auto& expectedNormal = source.lods[0].normals[i];
        const auto& vertex = mesh.vertices[i];
        INFO("vertex " << i);
        REQUIRE(vertex.position.x == expected.x);
        REQUIRE(vertex.position.y == expected.y);
        REQUIRE(vertex.position.z == expected.z);
        REQUIRE(vertex.normal.x == expectedNormal.x);
        REQUIRE(vertex.normal.y == expectedNormal.y);
        REQUIRE(vertex.normal.z == expectedNormal.z);
        REQUIRE(vertex.uv.u == 0.125f * static_cast<float>(i));
        REQUIRE(vertex.uv.v == 0.25f * static_cast<float>(i) + 0.5f);
        REQUIRE(vertex.uv1.u == 0.75f - 0.0625f * static_cast<float>(i));
        REQUIRE(vertex.uv1.v == 0.5f * static_cast<float>(i));
    }

    REQUIRE(mesh.triangles.size() == 1);
    REQUIRE(mesh.quads.size() == 1);
    REQUIRE(lod.uvSetCount == 2);

    for (int corner = 0; corner < 3; ++corner)
        REQUIRE(triangleCorner(mesh.triangles[0], corner) == static_cast<uint32_t>(corner));
    const uint32_t expectedQuad[4] = {2, 3, 4, 5};
    for (int corner = 0; corner < 4; ++corner)
        REQUIRE(quadCorner(mesh.quads[0], corner) == expectedQuad[corner]);

    REQUIRE(static_cast<uint32_t>(mesh.triangles[0].flags) == 0x20u);
    REQUIRE(static_cast<uint32_t>(mesh.quads[0].flags) == 0u);

    // Two materials, not one: the faces share nothing, and the quad's empty
    // texture must not collapse it onto the triangle's entry.
    REQUIRE(mesh.materials.size() == 2);
    const auto& triMaterial = mesh.materials[mesh.triangles[0].materialIndex];
    const auto& quadMaterial = mesh.materials[mesh.quads[0].materialIndex];
    REQUIRE(triMaterial.texturePath == "ca\\test\\data\\hull_co.paa");
    REQUIRE(triMaterial.materialPath == "ca\\test\\data\\hull.rvmat");
    REQUIRE(quadMaterial.texturePath.empty());
    REQUIRE(quadMaterial.materialPath == "ca\\test\\data\\glass.rvmat");

    REQUIRE(mesh.properties.size() == 1);
    REQUIRE(mesh.properties[0].name == "lodnoshadow");
    REQUIRE(mesh.properties[0].value == "1");

    // The special-LOD sentinel has to arrive unrounded or the reader classifies it
    // as an ordinary visual LOD.
    REQUIRE(model.lodLevels[1].resolution == 1.0e13f);
    REQUIRE(model.lodLevels[1].purpose == Poseidon::Model::LodPurpose::Geometry);
    REQUIRE(model.lodLevels[1].mesh.triangles.size() == 1);
    REQUIRE(model.lodLevels[1].uvSetCount == 0);
}

TEST_CASE("MLODWriter: write() produces a file the loader opens", "[mlod][writer]")
{
    const auto path = std::filesystem::temp_directory_path() / "poseidon_mlod_writer_roundtrip.p3d";
    MLODWriter::write(buildTestModel(), path.string());

    auto model = MLODLoader::load(path.string());
    REQUIRE(model.lodLevels.size() == 2);
    REQUIRE(model.lodLevels[0].mesh.vertices.size() == 6);
    REQUIRE(model.lodLevels[0].sourceEncoding == "P3DM");

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("MLODWriter: rejects what the reader would misread", "[mlod][writer]")
{
    SECTION("no LODs")
    {
        REQUIRE_THROWS_WITH(MLODWriter::writeToBuffer(MLODW::WriteModel{}), Catch::Matchers::ContainsSubstring("LODs"));
    }

    SECTION("a face that is neither a triangle nor a quad")
    {
        MLODW::WriteModel model = buildTestModel();
        model.lods[0].faces[0].vertexCount = 5;
        REQUIRE_THROWS_WITH(MLODWriter::writeToBuffer(model), Catch::Matchers::ContainsSubstring("vertices"));
    }

    SECTION("a point index past the point table")
    {
        // The reader bounds-checks this and silently leaves the vertex at the
        // origin, so it has to be caught on the way out or not at all.
        MLODW::WriteModel model = buildTestModel();
        model.lods[0].faces[0].vertices[0].point = 99;
        REQUIRE_THROWS_WITH(MLODWriter::writeToBuffer(model), Catch::Matchers::ContainsSubstring("references point"));
    }

    SECTION("a normal index past the normal table")
    {
        MLODW::WriteModel model = buildTestModel();
        model.lods[0].faces[0].vertices[0].normal = 42;
        REQUIRE_THROWS_WITH(MLODWriter::writeToBuffer(model), Catch::Matchers::ContainsSubstring("references normal"));
    }

    SECTION("a string past the reader's own 1024-byte cap")
    {
        MLODW::WriteModel model = buildTestModel();
        model.lods[0].faces[0].material = std::string(2000, 'a');
        REQUIRE_THROWS_WITH(MLODWriter::writeToBuffer(model), Catch::Matchers::ContainsSubstring("exceeds"));
    }
}

namespace
{

// Compare two loaded models field by field.
//
// Vertex flags are compared, but do not read that as proof they round-trip: every
// point in both MLOD fixtures in this repo carries POINT_* flags of 0, measured,
// so both sides land on ClipAll whatever the writer does. The reader's
// POINT_* -> ClipFlags conversion is many-to-one, so a source with real flag bits
// could not be reconstructed from the IR at all -- see MLODWriter::describe.
void requireModelsAgree(const Poseidon::Model::Model& a, const Poseidon::Model::Model& b)
{
    REQUIRE(a.lodLevels.size() == b.lodLevels.size());

    for (size_t lodIndex = 0; lodIndex < a.lodLevels.size(); ++lodIndex)
    {
        INFO("LOD " << lodIndex);
        const auto& lodA = a.lodLevels[lodIndex];
        const auto& lodB = b.lodLevels[lodIndex];
        REQUIRE(lodA.resolution == lodB.resolution);
        REQUIRE(lodB.sourceEncoding == "P3DM");

        const auto& meshA = lodA.mesh;
        const auto& meshB = lodB.mesh;

        REQUIRE(meshA.vertices.size() == meshB.vertices.size());
        for (size_t i = 0; i < meshA.vertices.size(); ++i)
        {
            INFO("vertex " << i);
            REQUIRE(meshA.vertices[i].position.x == meshB.vertices[i].position.x);
            REQUIRE(meshA.vertices[i].position.y == meshB.vertices[i].position.y);
            REQUIRE(meshA.vertices[i].position.z == meshB.vertices[i].position.z);
            REQUIRE(meshA.vertices[i].normal.x == meshB.vertices[i].normal.x);
            REQUIRE(meshA.vertices[i].normal.y == meshB.vertices[i].normal.y);
            REQUIRE(meshA.vertices[i].normal.z == meshB.vertices[i].normal.z);
            REQUIRE(meshA.vertices[i].uv.u == meshB.vertices[i].uv.u);
            REQUIRE(meshA.vertices[i].uv.v == meshB.vertices[i].uv.v);
            REQUIRE(meshA.vertices[i].uv1.u == meshB.vertices[i].uv1.u);
            REQUIRE(meshA.vertices[i].uv1.v == meshB.vertices[i].uv1.v);
            REQUIRE(meshA.vertices[i].flags == meshB.vertices[i].flags);
        }

        REQUIRE(meshA.triangles.size() == meshB.triangles.size());
        for (size_t i = 0; i < meshA.triangles.size(); ++i)
        {
            INFO("triangle " << i);
            for (int corner = 0; corner < 3; ++corner)
                REQUIRE(meshA.triangles[i].indices[corner] == meshB.triangles[i].indices[corner]);
            REQUIRE(meshA.triangles[i].flags == meshB.triangles[i].flags);
            REQUIRE(meshA.triangles[i].originalIndex == meshB.triangles[i].originalIndex);
            // The material index only means the same thing if the tables match,
            // which is asserted below.
            REQUIRE(meshA.triangles[i].materialIndex == meshB.triangles[i].materialIndex);
        }

        REQUIRE(meshA.quads.size() == meshB.quads.size());
        for (size_t i = 0; i < meshA.quads.size(); ++i)
        {
            INFO("quad " << i);
            for (int corner = 0; corner < 4; ++corner)
                REQUIRE(meshA.quads[i].indices[corner] == meshB.quads[i].indices[corner]);
            REQUIRE(meshA.quads[i].flags == meshB.quads[i].flags);
            REQUIRE(meshA.quads[i].originalIndex == meshB.quads[i].originalIndex);
            REQUIRE(meshA.quads[i].materialIndex == meshB.quads[i].materialIndex);
        }

        REQUIRE(meshA.materials.size() == meshB.materials.size());
        for (size_t i = 0; i < meshA.materials.size(); ++i)
        {
            INFO("material " << i);
            REQUIRE(meshA.materials[i].name == meshB.materials[i].name);
            REQUIRE(meshA.materials[i].texturePath == meshB.materials[i].texturePath);
            REQUIRE(meshA.materials[i].materialPath == meshB.materials[i].materialPath);
        }

        REQUIRE(meshA.properties.size() == meshB.properties.size());
        for (size_t i = 0; i < meshA.properties.size(); ++i)
        {
            REQUIRE(meshA.properties[i].name == meshB.properties[i].name);
            REQUIRE(meshA.properties[i].value == meshB.properties[i].value);
        }
    }
}

Poseidon::Model::Model reemit(const Poseidon::Model::Model& source)
{
    const std::vector<char> bytes = MLODWriter::writeToBuffer(MLODWriter::describe(source));
    return MLODLoader::loadFromBuffer(bytes.data(), static_cast<int>(bytes.size()), "reemit");
}

} // namespace

// The stronger check: authored P3DM bytes in, the same geometry out. This is what
// distinguishes a writer from one that merely agrees with its own assumptions --
// the input here was produced by the fixture generator, not by this code.
TEST_CASE("MLODWriter: re-emitting a loaded P3DM fixture reproduces it", "[mlod][writer]")
{
    auto original = MLODLoader::load(GET_FIXTURE("mlod/p3dm_two_lod.p3d"));
    REQUIRE(original.lodLevels.size() == 2);
    REQUIRE(original.lodLevels[0].sourceEncoding == "P3DM");
    // Guard the guard: if the fixture ever loses its second UV channel or its
    // shared-point split, the comparison below stops proving what it claims to.
    REQUIRE(original.lodLevels[0].uvSetCount == 2);
    REQUIRE(original.lodLevels[0].mesh.vertices.size() == 6);
    REQUIRE(original.lodLevels[0].mesh.triangles.size() == 1);
    REQUIRE(original.lodLevels[0].mesh.quads.size() == 1);
    REQUIRE(original.lodLevels[0].mesh.materials.size() == 2);

    requireModelsAgree(original, reemit(original));
}

// The same round trip across the encoding boundary: an SP3X source re-emitted as
// P3DM. Three LODs, one of them a Memory LOD with points and no faces at all --
// the reader takes a different path for that, and the writer has to survive it.
TEST_CASE("MLODWriter: an SP3X model re-emits as P3DM without losing geometry", "[mlod][writer]")
{
    auto original = MLODLoader::load(GET_FIXTURE("mlod/complex_vehicle_mlod.p3d"));
    REQUIRE(original.lodLevels.size() == 3);
    REQUIRE(original.lodLevels[0].sourceEncoding == "SP3X");

    requireModelsAgree(original, reemit(original));
}

// COL-001: the writer's ComponentXX selections and #Mass# tagg. What the engine
// does with them is asserted in test_xob_collision.cpp through ShapeAdapter; here
// the question is only whether the bytes come back through the reader as written.
TEST_CASE("MLODWriter: named selections and #Mass# round-trip through the reader", "[mlod][writer][COL-001]")
{
    MLODW::WriteModel model = buildTestModel();
    MLODW::WriteLod& geometry = model.lods[1];

    MLODW::WriteNamedSelection component;
    component.name = "Component01";
    component.points = {0, 1, 2};
    component.faces = {0};
    geometry.selections.push_back(component);

    // A second selection with per-point weights, to see the bytes survive.
    MLODW::WriteNamedSelection soft;
    soft.name = "soft";
    soft.points = {1, 2};
    soft.pointWeights = {200, 17};
    geometry.selections.push_back(soft);

    geometry.mass = {10.0f, 20.0f, 30.0f};

    const std::vector<char> bytes = MLODWriter::writeToBuffer(model);
    const auto loaded = MLODLoader::loadFromBuffer(bytes.data(), static_cast<int>(bytes.size()), "selections");
    REQUIRE(loaded.lodLevels.size() == 2);
    const auto& mesh = loaded.lodLevels[1].mesh;
    REQUIRE(mesh.vertices.size() == 3);

    REQUIRE(mesh.selections.size() == 2);
    REQUIRE(mesh.selections[0].name == "Component01");
    REQUIRE(mesh.selections[0].vertexIndices.size() == 3);
    REQUIRE(mesh.selections[0].triangleIndices == std::vector<uint32_t>{0});
    REQUIRE(mesh.selections[1].name == "soft");
    REQUIRE(mesh.selections[1].vertexIndices.size() == 2);
    REQUIRE(mesh.selections[1].triangleIndices.empty());
    // The raw source bytes, undecoded, as the reader keeps them (AST-018).
    REQUIRE(mesh.selections[1].sourceVertexWeights.size() == 2);
    REQUIRE(mesh.selections[1].sourceVertexWeights[0] == 200);
    REQUIRE(mesh.selections[1].sourceVertexWeights[1] == 17);

    REQUIRE(mesh.vertexMass.size() == 3);
    float total = 0.0f;
    for (float m : mesh.vertexMass)
        total += m;
    REQUIRE(total == 60.0f);
    // The visual LOD wrote no #Mass# and reads back none.
    REQUIRE(loaded.lodLevels[0].mesh.vertexMass.empty());
}

TEST_CASE("MLODWriter: describe() carries a loaded model's selections and mass back out", "[mlod][writer][COL-001]")
{
    // The authored fixture: a box geometry LOD, one Component01 of six quads, 8 x 150 kg.
    auto original = MLODLoader::load(GET_FIXTURE("mlod/p3dm_geometry_mass.p3d"));
    REQUIRE(original.lodLevels.size() == 2);
    REQUIRE(original.lodLevels[1].mesh.selections.size() == 1);
    REQUIRE(original.lodLevels[1].mesh.vertexMass.size() == original.lodLevels[1].mesh.vertices.size());

    const MLODW::WriteModel described = MLODWriter::describe(original);
    REQUIRE(described.lods.size() == 2);
    REQUIRE(described.lods[1].selections.size() == 1);
    REQUIRE(described.lods[1].selections[0].name == "Component01");
    REQUIRE(described.lods[1].selections[0].faces == std::vector<int32_t>{0, 1, 2, 3, 4, 5});
    REQUIRE(described.lods[1].mass.size() == described.lods[1].points.size());
    REQUIRE(described.lods[0].selections.empty());
    REQUIRE(described.lods[0].mass.empty());

    const auto again = reemit(original);
    requireModelsAgree(original, again);
    const auto& before = original.lodLevels[1].mesh;
    const auto& after = again.lodLevels[1].mesh;
    REQUIRE(after.selections.size() == 1);
    REQUIRE(after.selections[0].name == before.selections[0].name);
    REQUIRE(after.selections[0].vertexIndices.size() == before.selections[0].vertexIndices.size());
    REQUIRE(after.selections[0].triangleIndices == before.selections[0].triangleIndices);
    float massBefore = 0.0f, massAfter = 0.0f;
    for (float m : before.vertexMass)
        massBefore += m;
    for (float m : after.vertexMass)
        massAfter += m;
    REQUIRE(massAfter == massBefore);
    REQUIRE(massAfter == 1200.0f);
}

TEST_CASE("MLODWriter: rejects selections and mass the reader would misread", "[mlod][writer][COL-001]")
{
    SECTION("a selection point past the point table")
    {
        MLODW::WriteModel model = buildTestModel();
        MLODW::WriteNamedSelection selection;
        selection.name = "Component01";
        selection.points = {0, 7};
        model.lods[1].selections.push_back(selection);
        REQUIRE_THROWS_WITH(MLODWriter::writeToBuffer(model), Catch::Matchers::ContainsSubstring("references point"));
    }
    SECTION("a selection face past the face table")
    {
        MLODW::WriteModel model = buildTestModel();
        MLODW::WriteNamedSelection selection;
        selection.name = "Component01";
        selection.faces = {3};
        model.lods[1].selections.push_back(selection);
        REQUIRE_THROWS_WITH(MLODWriter::writeToBuffer(model), Catch::Matchers::ContainsSubstring("references face"));
    }
    SECTION("a name the reader would take for a tagg keyword")
    {
        MLODW::WriteModel model = buildTestModel();
        MLODW::WriteNamedSelection selection;
        selection.name = "#Mass#";
        model.lods[1].selections.push_back(selection);
        REQUIRE_THROWS_WITH(MLODWriter::writeToBuffer(model), Catch::Matchers::ContainsSubstring("selection"));
    }
    SECTION("a mass table the wrong length")
    {
        MLODW::WriteModel model = buildTestModel();
        model.lods[1].mass = {1.0f, 2.0f};
        REQUIRE_THROWS_WITH(MLODWriter::writeToBuffer(model), Catch::Matchers::ContainsSubstring("mass"));
    }
}
