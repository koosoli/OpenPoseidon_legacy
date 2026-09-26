#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Terrain/StaticSourceEnvelope.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <memory>

using namespace Poseidon::Streaming;
namespace
{
Poseidon::Model::Model OriginalStaticOdol()
{
    Poseidon::Model::Model model;
    model.sourceFormat = "ODOL";
    model.sourceVersion = 54;
    // Original fixture declares all four source LODs and has no hidden pose data.
    model.sourceAudit.producerVersion = 1;
    model.sourceAudit.sourceRevision = 54;
    model.sourceAudit.geometryCoverage = Poseidon::Model::SourceGeometryCoverage::DeclaredLodsDecoded;
    model.sourceAudit.observations = Poseidon::Model::SourceAuditAllObservations;
    model.sourceAudit.declaredLods = model.sourceAudit.decodedLods = 4;
    model.boundingSphere.radius = 1;
    for (float resolution : {1.f, 1e13f, 7e15f, 6e15f})
    {
        Poseidon::Model::LODLevel lod(resolution);
        for (const auto& position : {Poseidon::Model::Vector3{-1, -1, -1},
                Poseidon::Model::Vector3{1, -1, 1}, Poseidon::Model::Vector3{1, 1, -1}})
        {
            Poseidon::Model::Vertex vertex;
            vertex.position = position;
            lod.mesh.vertices.push_back(vertex);
        }
        lod.mesh.triangles.emplace_back(0, 1, 2);
        model.lodLevels.push_back(std::move(lod));
    }
    return model;
}
StaticSourceEnvelope Evidence(const Poseidon::Model::Model& model)
{ return BuildStaticSourceEnvelope(model, 17, StaticSourceCoverage::FullCompiledIR); }
}

TEST_CASE("Static ODOL evidence encloses distant query geometry despite undersized visual metadata", "[streaming-static-source-envelope]")
{
    auto model = OriginalStaticOdol();
    model.lodLevels[1].mesh.vertices[0].position = {200, -30, -80};
    model.lodLevels[2].mesh.vertices[1].position = {-40, 70, 12};
    model.lodLevels[3].mesh.vertices[2].position = {15, -20, 90};
    // This unused vertex still belongs to the engine's all-position radius.
    model.lodLevels[3].mesh.vertices.push_back({{0, 0, 300}, {}, {}});
    const auto evidence = Evidence(model);
    REQUIRE(evidence.State() == StaticSourceEnvelopeState::SourceEvidence);
    const auto radius = evidence.RadiusForGeneration(17);
    REQUIRE(radius);
    REQUIRE(*radius > 300);
    for (const auto& lod : model.lodLevels)
        for (const auto& vertex : lod.mesh.vertices)
        {
            const auto& p = vertex.position;
            REQUIRE(double(*radius) >= std::hypot(double(p.x), double(p.y), double(p.z)));
            // Adapter reversal changes axes/signs, not this containment claim.
            REQUIRE(double(*radius) >= std::hypot(-double(p.z), double(p.y), double(p.x)));
        }
    model.boundingSphere.radius = 500;
    REQUIRE(*Evidence(model).RadiusForGeneration(17) >= 500);
}

TEST_CASE("Source evidence is generation-qualified and cannot certify partial or other-format input", "[streaming-static-source-envelope]")
{
    auto model = OriginalStaticOdol();
    const auto evidence = Evidence(model);
    REQUIRE(evidence.SourceGeneration() == 17);
    REQUIRE_FALSE(evidence.RadiusForGeneration(18));
    REQUIRE_FALSE(evidence.RadiusForGeneration(0));
    auto zero = BuildStaticSourceEnvelope(model, 0, StaticSourceCoverage::FullCompiledIR);
    REQUIRE(zero.HasReason(StaticSourceEnvelopeReason::Generation));
    REQUIRE_FALSE(zero.RadiusForGeneration(0));
    auto partial = BuildStaticSourceEnvelope(model, 17, StaticSourceCoverage::Partial);
    REQUIRE(partial.HasReason(StaticSourceEnvelopeReason::IncompleteSource));
    REQUIRE_FALSE(partial.RadiusForGeneration(17));
    model.sourceFormat = "MLOD";
    REQUIRE(Evidence(model).HasReason(StaticSourceEnvelopeReason::Format));
    REQUIRE_FALSE(Evidence(model).RadiusForGeneration(17));
}

TEST_CASE("External, posed and terrain-dependent geometry refuses usable source envelopes", "[streaming-static-source-envelope]")
{
    auto model = OriginalStaticOdol();
    SECTION("resolved proxy is external geometry") { model.lodLevels[0].mesh.proxies.emplace_back("original-proxy"); }
    SECTION("proxy selection without resolved dependency") { model.lodLevels[1].mesh.selections.emplace_back("proxy:original.001"); }
    SECTION("animation enabled") { model.allowAnimation = 1; }
    SECTION("animation frames") { model.lodLevels[1].mesh.frames.emplace_back(); }
    SECTION("bone weights") { model.lodLevels[1].mesh.selections.emplace_back("bone"); model.lodLevels[1].mesh.selections.back().vertexWeights = {255}; }
    SECTION("source weights are not a pose proof") { model.lodLevels[1].mesh.selections.emplace_back("component"); model.lodLevels[1].mesh.selections.back().sourceVertexWeights = {1}; }
    SECTION("on terrain") { model.lodLevels[1].mesh.vertices[0].flags = Poseidon::Model::VertexFlags::LandOn; }
    SECTION("keep terrain height") { model.lodLevels[1].mesh.vertices[0].flags = Poseidon::Model::VertexFlags::LandKeep; }
    SECTION("below terrain") { model.lodLevels[1].mesh.vertices[0].flags = Poseidon::Model::VertexFlags::LandUnder; }
    SECTION("above terrain") { model.lodLevels[1].mesh.vertices[0].flags = Poseidon::Model::VertexFlags::LandAbove; }
    const auto evidence = Evidence(model);
    REQUIRE(evidence.State() == StaticSourceEnvelopeState::Unknown);
    REQUIRE_FALSE(evidence.RadiusForGeneration(17));
}

TEST_CASE("Invalid and incomplete ODOL geometry cannot publish an enclosing float radius", "[streaming-static-source-envelope]")
{
    auto model = OriginalStaticOdol();
    SECTION("missing fire geometry") { model.lodLevels.erase(model.lodLevels.begin() + 2); }
    SECTION("missing physical faces") { model.lodLevels[1].mesh.triangles.clear(); }
    SECTION("face outside vertex stream") { model.lodLevels[3].mesh.triangles[0].indices[2] = 100; }
    SECTION("NaN position") { model.lodLevels[1].mesh.vertices[0].position.x = std::numeric_limits<float>::quiet_NaN(); }
    SECTION("infinite stored radius") { model.boundingSphere.radius = std::numeric_limits<float>::infinity(); }
    SECTION("negative stored radius") { model.boundingSphere.radius = -1; }
    SECTION("finite vertices whose enclosing float radius overflows") { model.lodLevels[1].mesh.vertices[0].position = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), 0}; }
    SECTION("resolution disagrees with purpose") { model.lodLevels[1].purpose = Poseidon::Model::LodPurpose::Visual; }
    SECTION("no source geometry") { model.lodLevels.clear(); }
    const auto evidence = Evidence(model);
    REQUIRE(evidence.State() == StaticSourceEnvelopeState::Unknown);
    REQUIRE_FALSE(evidence.RadiusForGeneration(17));
}

TEST_CASE("Source envelope admission is bounded before any vertex validation walk", "[streaming-static-source-envelope]")
{
    auto model = OriginalStaticOdol();
    SECTION("spare owned vertex capacity is charged")
    {
        model.lodLevels[0].mesh.vertices.reserve(size_t(StaticSourceDetail::MaxPayloadBytes / sizeof(Poseidon::Model::Vertex)) + 1);
    }
    SECTION("nested nongeometry payload is charged")
    {
        model.lodLevels[0].mesh.materials.emplace_back("original");
        model.lodLevels[0].mesh.materials.back().texturePath.resize(StaticSourceDetail::MaxPayloadBytes);
    }
    SECTION("LOD count is limited before traversal") { model.lodLevels.resize(StaticSourceDetail::MaxLods + 1); }
    const auto evidence = Evidence(model);
    REQUIRE(evidence.HasReason(StaticSourceEnvelopeReason::Capacity));
    REQUIRE(evidence.PayloadBytes() == 0);
    REQUIRE_FALSE(evidence.RadiusForGeneration(17));
}

TEST_CASE("IR class evidence never becomes a final constructor certificate", "[streaming-static-source-envelope]")
{
    auto model = OriginalStaticOdol();
    model.lodLevels[1].mesh.properties.emplace_back("class", "house");
    // Geometry evidence is deliberately class-independent. A config-aware
    // owner must reject/substitute this before using it for world coverage.
    REQUIRE(Evidence(model).State() == StaticSourceEnvelopeState::SourceEvidence);
    REQUIRE(Evidence(model).RadiusForGeneration(17));
}

TEST_CASE("Original ODOL source envelope encloses actual completed adapter positions", "[streaming-static-source-envelope]")
{
    auto model = OriginalStaticOdol();
    model.sourcePath = "static_source_original_synthetic.p3d";
    model.mass = 10;
    model.invMass = .1f;
    // The physical and view positions extend far beyond the visual LOD and
    // authored model sphere. Stored ODOL bounds are intentionally undersized.
    model.lodLevels[1].mesh.vertices[0].position = {220, 45, -160};
    model.lodLevels[3].mesh.vertices[2].position = {-75, -60, 310};
    for (auto& lod : model.lodLevels)
    {
        lod.mesh.materials.emplace_back("original-untextured");
        for (auto& vertex : lod.mesh.vertices) vertex.normal = {0, 1, 0};
    }
    bool reversed = false;
    SECTION("ordinary adapter tail") {}
    SECTION("real adapter reversal") { reversed = true; }
    SECTION("source LOD order differs") { std::swap(model.lodLevels[2], model.lodLevels[3]); }
    SECTION("reordered source with real reversal") { std::swap(model.lodLevels[2], model.lodLevels[3]); reversed = true; }
    REQUIRE(model.compile());
    const auto evidence = Evidence(model);
    const auto radius = evidence.RadiusForGeneration(17);
    REQUIRE(radius);

    // Empty supplied tables take the real null-texture/material path without
    // BuildAdapterBankTables, external assets, or resource-bank ownership.
    Poseidon::Foundation::CaptureMainThread();
    Poseidon::ShapeBank::CpuOnlyLoadScope cpuOnly;
    Poseidon::Model::ShapeAdapter::AdapterBankTables tables;
    std::unique_ptr<Poseidon::LODShapeWithShadow> shape(
        Poseidon::Model::ShapeAdapter::convertToLODShape(model, reversed, &tables, true));
    REQUIRE(shape);
    REQUIRE(shape->FindGeometryLevel() >= 0);
    REQUIRE(shape->FindFireGeometryLevel() >= 0);
    REQUIRE(shape->FindViewGeometryLevel() >= 0);
    size_t positions = 0;
    bool positionOutsideSerializedSphere = false;
    for (int level = 0; level < shape->NLevels(); ++level)
    {
        const auto* lod = shape->Level(level);
        REQUIRE(lod);
        for (int index = 0; index < lod->NPos(); ++index)
        {
            const auto& p = lod->Pos(index);
            const double length = std::hypot(double(p.X()), double(p.Y()), double(p.Z()));
            REQUIRE(std::isfinite(length));
            REQUIRE(length <= double(*radius));
            positionOutsideSerializedSphere |= length > model.boundingSphere.radius;
            ++positions;
        }
    }
    REQUIRE(positions >= 9);
    REQUIRE(positionOutsideSerializedSphere);
    REQUIRE(double(*radius) > shape->BoundingSphere());
}

TEST_CASE("Empty ODOL IR pose lists cannot hide missing or discarded source evidence", "[streaming-static-source-envelope]")
{
    auto model = OriginalStaticOdol();
    SECTION("old derived cache has no audit") { model.sourceAudit = {}; }
    SECTION("unknown producer version") { model.sourceAudit.producerVersion = 2; }
    SECTION("unsupported revision") { model.sourceAudit.sourceRevision = 99; }
    SECTION("audit revision differs from the loaded source") { model.sourceVersion = 40; }
    SECTION("keyframe absence was never observed")
    { model.sourceAudit.observations &= ~uint32_t(Poseidon::Model::SourceAuditObservation::Keyframes); }
    SECTION("declared LOD was not decoded") { ++model.sourceAudit.declaredLods; }
    SECTION("source animation directory exists") { model.sourceAudit.directoryHasAnimations = true; }
    SECTION("source animation class exists") { model.sourceAudit.directoryAnimationClasses = 1; }
    SECTION("source skeleton declared but empty") { model.sourceAudit.skeletonDeclared = true; }
    SECTION("source skeleton bones exist") { model.sourceAudit.skeletonBones = 1; }
    SECTION("source vertex bone references exist") { model.sourceAudit.vertexBoneReferenceCount = 1; }
    SECTION("source neighbour bone references exist") { model.sourceAudit.neighbourBoneReferenceCount = 1; }
    SECTION("source keyframe positions were skipped")
    { model.sourceAudit.keyframeCount = 1; model.sourceAudit.keyframePayloadDiscarded = true; }
    for (const auto& lod : model.lodLevels) REQUIRE(lod.mesh.frames.empty());
    REQUIRE(model.allowAnimation == 0);
    const auto evidence = Evidence(model);
    REQUIRE(evidence.HasReason(StaticSourceEnvelopeReason::IncompleteSource));
    REQUIRE_FALSE(evidence.RadiusForGeneration(17));
}
