#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Asset/Formats/P3D/ODOLLoader.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>

#include "../../test_fixtures.hpp"
#include <memory>

using namespace Poseidon::Model;

TEST_CASE("ShapeAdapter: ODOL morph frames become animation phases", "[ShapeAdapter][odol][animation]")
{
    auto model = Poseidon::Asset::Formats::ODOLLoader::load(GET_FIXTURE("p3d/animated_morph_odol.p3d"));

    REQUIRE(model.sourceFormat == "ODOL");
    REQUIRE(model.lodLevels.size() == 1);

    const Mesh& mesh = model.lodLevels[0].mesh;
    REQUIRE(mesh.vertices.size() == 4);
    REQUIRE(mesh.frames.size() == 3);
    REQUIRE(mesh.frames[1].time == Catch::Approx(0.5f));
    REQUIRE(mesh.frames[1].positions.size() == 4);
    REQUIRE(mesh.frames[1].positions[0].y == Catch::Approx(0.45f));
    REQUIRE(mesh.frames[1].positions[1].y == Catch::Approx(-0.35f));

    std::unique_ptr<LODShapeWithShadow> shape(ShapeAdapter::convertToLODShape(model, false));
    REQUIRE(shape != nullptr);
    REQUIRE(shape->NLevels() == 1);

    Shape* lod = shape->Level(0);
    REQUIRE(lod != nullptr);
    REQUIRE(lod->NPos() == 4);
    REQUIRE(lod->IsAnimated());
    REQUIRE(lod->NAnimationPhases() == 3);
    REQUIRE(lod->AnimationPhaseTime(0) == Catch::Approx(0.0f));
    REQUIRE(lod->AnimationPhaseTime(1) == Catch::Approx(0.5f));
    REQUIRE(lod->AnimationPhaseTime(2) == Catch::Approx(1.0f));

    lod->SetPhase(0.5f, 0.0f);
    REQUIRE(lod->Pos(0)[1] == Catch::Approx(0.45f));
    REQUIRE(lod->Pos(1)[1] == Catch::Approx(-0.35f));

    lod->SetPhase(0.25f, 0.0f);
    REQUIRE(lod->Pos(0)[1] == Catch::Approx(0.225f));
    REQUIRE(lod->Pos(1)[1] == Catch::Approx(-0.175f));

    // Reverse used to index each phase by the phase number instead of the
    // vertex number, corrupting morph frames (and sometimes reading past them).
    std::unique_ptr<LODShapeWithShadow> reversed(ShapeAdapter::convertToLODShape(model, true));
    REQUIRE(reversed);
    Shape* reversedLod = reversed->Level(0);
    REQUIRE(reversedLod->NAnimationPhases() == lod->NAnimationPhases());
    for (int phase = 0; phase < lod->NAnimationPhases(); ++phase)
        for (int vertex = 0; vertex < lod->NVertex(); ++vertex)
        {
            const auto& a = lod->GetAnimationPhase(phase)[vertex];
            const auto& b = reversedLod->GetAnimationPhase(phase)[vertex];
            REQUIRE(b[0] == Catch::Approx(-a[0]));
            REQUIRE(b[1] == Catch::Approx(a[1]));
            REQUIRE(b[2] == Catch::Approx(-a[2]));
        }
}
