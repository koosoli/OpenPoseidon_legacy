#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Terrain/ObjectStreamTreeFixture.hpp>

using namespace Poseidon::Streaming;

TEST_CASE("PBO tree fixture policy admits only exact two virtual models")
{
    CHECK(TreeFixtureModelOrdinal(R"(dz\plants\tree\t_piceaabies_2d.p3d)") == 0);
    CHECK(TreeFixtureModelOrdinal(R"(dz\plants\tree\d_piceaabies_stumpb.p3d)") == 1);
    CHECK_FALSE(SelectedTreeFixtureModel(R"(dz\plants\tree\t_piceaabies_2d.p3d.bak)"));
    CHECK_FALSE(SelectedTreeFixtureModel(R"(dz\plants\tree\other.p3d)"));
    CHECK_FALSE(SelectedTreeFixtureModel(R"(DZ\plants\tree\t_piceaabies_2d.p3d)"));
    CHECK_FALSE(EligibleTreeFixtureBytes(0));
    CHECK(EligibleTreeFixtureBytes(518472));
    CHECK(EligibleTreeFixtureBytes(8 * 1024 * 1024));
    CHECK_FALSE(EligibleTreeFixtureBytes(8 * 1024 * 1024 + 1));
}
