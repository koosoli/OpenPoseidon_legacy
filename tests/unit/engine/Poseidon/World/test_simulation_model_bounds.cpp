#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <Poseidon/World/Terrain/SimulationModelBounds.hpp>

using namespace Poseidon::Streaming;

namespace
{
void AddBoundsLod(Poseidon::Model::Model& model, float resolution,
                  Poseidon::Model::Vector3 low, Poseidon::Model::Vector3 high)
{
    Poseidon::Model::LODLevel lod(resolution);
    Poseidon::Model::Vertex first, last;
    first.position = low;
    last.position = high;
    lod.mesh.vertices = {first, last};
    model.lodLevels.push_back(std::move(lod));
}

Poseidon::Model::Model StaticModel()
{
    Poseidon::Model::Model model;
    AddBoundsLod(model, 1, {-1, -1, -1}, {1, 1, 1});
    AddBoundsLod(model, 1e13f, {-2, 0, -10}, {4, 3, -8});
    return model;
}
}

TEST_CASE("Complete model bounds include special geometry beyond the visual LOD", "[streaming-bounds]")
{
    auto model = StaticModel();
    AddBoundsLod(model, 7e15f, {-50, 0, 0}, {50, 2, 2});
    AddBoundsLod(model, 3e15f, {0, -7, 0}, {1, -5, 1});
    auto bounds = BuildSimulationModelBounds(model, ModelSourceCoverage::FullIR);
    REQUIRE(bounds.state == ModelBoundsState::CompleteSource);
    REQUIRE(bounds.authoredSimulationLods == 3);
    REQUIRE(bounds.min == std::array<double, 3>{-50, -7, -10});
    REQUIRE(bounds.max == std::array<double, 3>{50, 3, 2});
    model.boundingBox = {{-1, -1, -1}, {1, 1, 1}}; // Stale visual-only cached box is not trusted.
    REQUIRE(BuildSimulationModelBounds(model, ModelSourceCoverage::FullIR).min == bounds.min);
    auto partial = BuildSimulationModelBounds(model, ModelSourceCoverage::Partial);
    REQUIRE(partial.state == ModelBoundsState::Unknown);
    REQUIRE(partial.HasReason(ModelBoundsReason::IncompleteSource));
    REQUIRE(partial.min == std::array<double, 3>{});
}

TEST_CASE("Visual-only models retain source bounds without asserting collision emptiness", "[streaming-bounds]")
{
    Poseidon::Model::Model model;
    AddBoundsLod(model, 1, {-1, -1, -1}, {1, 1, 1});
    const auto bounds = BuildSimulationModelBounds(model, ModelSourceCoverage::FullIR);
    REQUIRE(bounds.state == ModelBoundsState::CompleteSource);
    REQUIRE(bounds.authoredSimulationLods == 0);
    REQUIRE(bounds.max == std::array<double, 3>{1, 1, 1});
    REQUIRE(BuildSimulationModelBounds({}, ModelSourceCoverage::FullIR).HasReason(ModelBoundsReason::NoVertices));
}

TEST_CASE("Unresolved proxies and animation never publish usable simulation bounds", "[streaming-bounds]")
{
    auto model = StaticModel();
    model.lodLevels[1].mesh.proxies.emplace_back("distant-wall");
    auto proxy = BuildSimulationModelBounds(model, ModelSourceCoverage::FullIR);
    REQUIRE(proxy.state == ModelBoundsState::Unknown);
    REQUIRE(proxy.HasReason(ModelBoundsReason::UnresolvedProxy));
    REQUIRE(proxy.max == std::array<double, 3>{});
    model = StaticModel();
    model.lodLevels[0].mesh.selections.emplace_back("proxy:furniture.001");
    REQUIRE(BuildSimulationModelBounds(model, ModelSourceCoverage::FullIR).HasReason(ModelBoundsReason::UnresolvedProxy));
    model = StaticModel();
    model.allowAnimation = 1;
    REQUIRE(BuildSimulationModelBounds(model, ModelSourceCoverage::FullIR).HasReason(ModelBoundsReason::Animation));
    model.allowAnimation = 0;
    model.lodLevels[1].mesh.frames.emplace_back();
    REQUIRE(BuildSimulationModelBounds(model, ModelSourceCoverage::FullIR).HasReason(ModelBoundsReason::Animation));
    // ODOL bone membership can survive only as weighted named selections in the canonical IR.
    // Neither the allowAnimation flag nor a baked vertex-frame table must be required to refuse.
    model = StaticModel();
    model.lodLevels[1].mesh.selections.emplace_back("door_bone");
    auto& bone = model.lodLevels[1].mesh.selections.back();
    bone.vertexIndices = {0, 1};
    bone.vertexWeights = {255, 255};
    auto weighted = BuildSimulationModelBounds(model, ModelSourceCoverage::FullIR);
    REQUIRE(weighted.state == ModelBoundsState::Unknown);
    REQUIRE(weighted.HasReason(ModelBoundsReason::Animation));
    REQUIRE(weighted.max == std::array<double, 3>{});
    bone.vertexWeights.clear();
    bone.sourceVertexWeights = {1, 1};
    REQUIRE(BuildSimulationModelBounds(model, ModelSourceCoverage::FullIR).HasReason(ModelBoundsReason::Animation));
}

TEST_CASE("Malformed geometry and contradictory LOD metadata remain unknown", "[streaming-bounds]")
{
    auto model = StaticModel();
    model.lodLevels[1].mesh.vertices[0].position.x = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(BuildSimulationModelBounds(model, ModelSourceCoverage::FullIR).HasReason(ModelBoundsReason::NonFinite));
    model = StaticModel();
    model.lodLevels[1].mesh.triangles.emplace_back(0, 1, 9);
    REQUIRE(BuildSimulationModelBounds(model, ModelSourceCoverage::FullIR).HasReason(ModelBoundsReason::InvalidFace));
    model = StaticModel();
    model.lodLevels[1].mesh.quads.emplace_back(0, 1, 0, 9);
    REQUIRE(BuildSimulationModelBounds(model, ModelSourceCoverage::FullIR).HasReason(ModelBoundsReason::InvalidFace));
    model = StaticModel();
    model.lodLevels[1].purpose = Poseidon::Model::LodPurpose::Visual;
    REQUIRE(BuildSimulationModelBounds(model, ModelSourceCoverage::FullIR).HasReason(ModelBoundsReason::InconsistentPurpose));
    model = StaticModel();
    AddBoundsLod(model, 9e15f, {0, 0, 0}, {1, 1, 1});
    REQUIRE(BuildSimulationModelBounds(model, ModelSourceCoverage::FullIR).HasReason(ModelBoundsReason::UnknownPurpose));
    model = StaticModel();
    model.geometryIdx = 0; // Points to visual geometry.
    REQUIRE(BuildSimulationModelBounds(model, ModelSourceCoverage::FullIR).HasReason(ModelBoundsReason::InconsistentIndex));
    model.geometryIdx = 1;
    model.geometryFireIdx = 1; // Valid fire-to-physical fallback.
    REQUIRE(BuildSimulationModelBounds(model, ModelSourceCoverage::FullIR).state == ModelBoundsState::CompleteSource);
    model.viewGeometryLODIndex = 99;
    REQUIRE(BuildSimulationModelBounds(model, ModelSourceCoverage::FullIR).HasReason(ModelBoundsReason::InconsistentIndex));
}

TEST_CASE("Transformed source bounds enclose all corners under shear and nonuniform scale", "[streaming-bounds]")
{
    Poseidon::Model::Model model;
    AddBoundsLod(model, 1e13f, {-2, 0, -10}, {4, 3, -8});
    const auto local = BuildSimulationModelBounds(model, ModelSourceCoverage::FullIR);
    Poseidon::Model::Matrix4x3 transform;
    transform.Set(0, -2, 1, 100, 3, 0, 2, 200, .5f, 1, 0, 300);
    const auto world = TransformSimulationModelBounds(local, transform);
    REQUIRE(world.state == ModelBoundsState::CompleteSource);
    const double expectedLow[] = {84, 174, 299}, expectedHigh[] = {92, 196, 305};
    for (int axis = 0; axis < 3; ++axis)
    {
        REQUIRE(world.min[axis] <= expectedLow[axis]);
        REQUIRE(world.max[axis] >= expectedHigh[axis]);
        REQUIRE(world.min[axis] == Catch::Approx(expectedLow[axis]).margin(.002));
        REQUIRE(world.max[axis] == Catch::Approx(expectedHigh[axis]).margin(.002));
    }
    for (int corner = 0; corner < 8; ++corner)
        for (int axis = 0; axis < 3; ++axis)
        {
            double transformed = transform.m[axis][3];
            for (int col = 0; col < 3; ++col)
                transformed += transform.m[axis][col] * ((corner & (1 << col)) ? local.max[col] : local.min[col]);
            REQUIRE(transformed >= world.min[axis]);
            REQUIRE(transformed <= world.max[axis]);
        }
}

TEST_CASE("Invalid transforms cannot turn unknown metadata into usable bounds", "[streaming-bounds]")
{
    Poseidon::Model::Matrix4x3 transform;
    const auto local = BuildSimulationModelBounds(StaticModel(), ModelSourceCoverage::FullIR);
    transform.m[1][3] = std::numeric_limits<float>::infinity();
    auto invalid = TransformSimulationModelBounds(local, transform);
    REQUIRE(invalid.state == ModelBoundsState::Unknown);
    REQUIRE(invalid.HasReason(ModelBoundsReason::InvalidTransform));
    REQUIRE(invalid.min == std::array<double, 3>{});
    const auto partial = BuildSimulationModelBounds(StaticModel(), ModelSourceCoverage::Partial);
    REQUIRE(TransformSimulationModelBounds(partial, {}).state == ModelBoundsState::Unknown);
    auto malformed = local;
    malformed.min[0] = malformed.max[0] + 1;
    REQUIRE(TransformSimulationModelBounds(malformed, {}).state == ModelBoundsState::Unknown);
    malformed = local;
    malformed.reasons = uint32_t(ModelBoundsReason::UnresolvedProxy);
    REQUIRE(TransformSimulationModelBounds(malformed, {}).state == ModelBoundsState::Unknown);
    transform = {};
    transform.m[0][0] = std::numeric_limits<float>::max();
    REQUIRE(TransformSimulationModelBounds(local, transform).state == ModelBoundsState::Unknown);
}
