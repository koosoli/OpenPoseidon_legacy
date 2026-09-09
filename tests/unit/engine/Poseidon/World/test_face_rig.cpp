#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Entities/Infantry/Head.hpp>

using namespace Poseidon;

TEST_CASE("Shared soldier face rig survives repeated setup and skeleton preparation", "[World][face-rig]")
{
    REQUIRE(FaceBoneRigEnabled());
    Ref<LODShapeWithShadow> lod = new LODShapeWithShadow();
    Shape* shape = new Shape();
    shape->Resize(1);
    SelInfo points[] = {SelInfo(0, 255)};
    shape->AddNamedSel(NamedSelection("head", points, 1, nullptr, 0));
    shape->AddNamedSel(NamedSelection("spodni ret", points, 1, nullptr, 0));
    lod->AddShape(shape, 0.0f);
    Skeleton skeleton;
    const int parent = skeleton.NewBone("head");
    WeightInfo weights;
    skeleton.Prepare(lod, weights);
    SetupFaceBoneRig(&skeleton, lod, weights);
    const int base = weights._faceBoneBase;
    REQUIRE(base >= 0);
    REQUIRE(weights._faceParent[0][FaceLip] == parent);
    REQUIRE(weights[0][0][0].GetSel() == base + FaceLip);

    // Loading another config with the same model/skeleton may reuse the cached
    // WeightInfo without calling Skeleton::Prepare again.
    for (int i = 0; i < 100; ++i)
    {
        SetupFaceBoneRig(&skeleton, lod, weights);
        CHECK(weights._faceBoneBase == base);
        CHECK(weights._faceParent[0][FaceLip] == parent);
        CHECK(weights[0][0][0].GetSel() == base + FaceLip);
    }

    // Prepare is incremental: Init retains existing entries and AddSelection
    // skips selections already registered. It must preserve the derived rig too.
    skeleton.Prepare(lod, weights);
    CHECK(weights._faceBoneBase == base);
    CHECK(weights._faceParent[0][FaceLip] == parent);
    CHECK(weights[0][0][0].GetSel() == base + FaceLip);
    SetupFaceBoneRig(&skeleton, lod, weights);
    CHECK(weights._faceBoneBase == base);
    CHECK(weights._faceParent[0][FaceLip] == parent);
    CHECK(weights[0][0][0].GetSel() == base + FaceLip);
}
