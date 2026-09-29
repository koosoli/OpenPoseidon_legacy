#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/Shape/PreparedGpuSectionClassification.hpp>

namespace
{
using Facts = Poseidon::render::PreparedGpuSectionFacts;
using Route = Poseidon::render::PreparedGpuSectionRoute;
using Poseidon::render::ClassifyPreparedGpuSection;

Facts Base()
{
    Facts f;
    f.complete = true;
    f.capturedGroups = Facts::All;
    f.gpuOwned = true;
    f.indexCount = 6;
    f.texturePresent = true;
    f.alphaClass = Poseidon::AlphaStats::Opaque;
    f.materialFullyOpaque = true;
    f.foldedEmissive = {2.0f, 4.0f, 6.0f, 0.75f};
    f.emitterScale = 0.25f;
    return f;
}
}

TEST_CASE("Prepared classifier refuses absent owner facts, CPU complement and empty geometry",
          "[prepared-gpu-section]")
{
    auto f = Base();
    f.complete = false;
    REQUIRE(ClassifyPreparedGpuSection(f, 1).route == Route::Incomplete);
    f.complete = true;
    f.capturedGroups &= ~Facts::Surface;
    REQUIRE(ClassifyPreparedGpuSection(f, 1).route == Route::Incomplete);
    f.capturedGroups = Facts::All;
    f.gpuOwned = false;
    REQUIRE(ClassifyPreparedGpuSection(f, 1).route == Route::CpuOwnership);
    f.gpuOwned = true;
    f.indexCount = 0;
    REQUIRE(ClassifyPreparedGpuSection(f, 1).route == Route::EmptyGeometry);
}

TEST_CASE("Prepared classifier preserves IsAlpha opaque override and late handle condition",
          "[prepared-gpu-section]")
{
    auto f = Base();
    f.descriptor.alpha = Poseidon::render::AlphaMode::TestAndBlend;
    f.descriptor.blend = Poseidon::render::BlendMode::AlphaBlend;
    f.descriptor.alphaRef = 128;
    const auto uploaded = ClassifyPreparedGpuSection(f, 91);
    REQUIRE(uploaded.Accepted());
    REQUIRE(uploaded.route == Route::Opaque);
    REQUIRE(uploaded.variant == 0);
    REQUIRE(uploaded.alphaRef == 0.0f);
    REQUIRE(uploaded.scaledEmissive[0] == 0.5f);
    REQUIRE(uploaded.scaledEmissive[1] == 1.0f);
    REQUIRE(uploaded.scaledEmissive[2] == 1.5f);
    REQUIRE(uploaded.scaledEmissive[3] == 0.75f);
    const auto failedUpload = ClassifyPreparedGpuSection(f, 0);
    REQUIRE(failedUpload.route == Route::Cutout);
    REQUIRE(failedUpload.variant == 1);
    REQUIRE(failedUpload.alphaRef == 128.0f / 255.0f);
}

TEST_CASE("Prepared classifier preserves cutout, bark and leaf-card blend routes",
          "[prepared-gpu-section]")
{
    auto f = Base();
    f.alphaClass = Poseidon::AlphaStats::Cutout;
    auto decision = ClassifyPreparedGpuSection(f, 9);
    REQUIRE(decision.route == Route::Cutout);
    REQUIRE(decision.alphaRef == 0.5f);

    f.alphaClass = Poseidon::AlphaStats::Blend;
    f.leafCards = true;
    f.alphaHoles = true;
    decision = ClassifyPreparedGpuSection(f, 9);
    REQUIRE(decision.route == Route::Cutout); // actual leaf holes
    f.alphaHoles = false;
    decision = ClassifyPreparedGpuSection(f, 9);
    REQUIRE(decision.route == Route::Opaque); // hole-less bark under leaf material
    f.barkOrTrunkName = true;
    f.ematBlends = true;
    decision = ClassifyPreparedGpuSection(f, 9);
    REQUIRE(decision.route == Route::Opaque); // explicit bark rule wins first
}

TEST_CASE("Prepared classifier refuses real blend and retains solid BCR only with live upload",
          "[prepared-gpu-section]")
{
    auto f = Base();
    f.alphaClass = Poseidon::AlphaStats::Blend;
    REQUIRE(ClassifyPreparedGpuSection(f, 15).route == Route::CpuBlend);
    f.ematOpaqueSolid = true;
    REQUIRE(ClassifyPreparedGpuSection(f, 15).route == Route::Opaque);
    REQUIRE(ClassifyPreparedGpuSection(f, 0).route == Route::CpuBlend);
    f.alphaHoles = true;
    REQUIRE(ClassifyPreparedGpuSection(f, 15).route == Route::CpuBlend);
    f.alphaHoles = false;
    f.ematOpaqueSolid = false;
    f.ematBlends = true;
    REQUIRE(ClassifyPreparedGpuSection(f, 15).route == Route::CpuBlend);
    f.ematBlends = false;
    f.ematGlass = true;
    f.leafCards = true;
    REQUIRE(ClassifyPreparedGpuSection(f, 15).route == Route::CpuBlend);
}

TEST_CASE("Prepared classifier keeps authored coverage and double-sided complement rules",
          "[prepared-gpu-section]")
{
    auto f = Base();
    f.alphaClass = Poseidon::AlphaStats::Blend;
    f.legacyPlantName = true;
    REQUIRE(ClassifyPreparedGpuSection(f, 1).route == Route::Cutout);
    f.legacyPlantName = false;
    f.enfusionCoverageName = true;
    f.alphaHoles = true;
    REQUIRE(ClassifyPreparedGpuSection(f, 1).route == Route::Cutout);
    f.ematCullNone = true;
    REQUIRE(ClassifyPreparedGpuSection(f, 1).route == Route::CpuDoubleSidedCutout);
    f.ematVegetationFamily = true;
    REQUIRE(ClassifyPreparedGpuSection(f, 1).route == Route::Cutout);
    f.ematBlends = true;
    REQUIRE(ClassifyPreparedGpuSection(f, 1).route == Route::CpuBlend);
}
