#include <catch2/catch_test_macros.hpp>
#include <Poseidon/IO/Streams/RetailRigidAssetProfile.hpp>

namespace Profile=Poseidon::RetailRigidAssetProfile;

TEST_CASE("Retail rigid profiles select exactly two measured source names",
          "[retail-rigid-profile]")
{
    const auto* skala=Profile::ByModelPath(R"(DATA3D/skala_new.p3d)");
    const auto* second=Profile::ByModelPath(R"(data3d\SKALA2.P3D)");
    REQUIRE(skala==Profile::ById(Profile::Id::SkalaNew));
    REQUIRE(second==Profile::ById(Profile::Id::Skala2));
    REQUIRE(skala!=second);
    REQUIRE(Profile::ByModelMember("skala_new.p3d")==skala);
    REQUIRE(Profile::ByModelMember("SKALA2.P3D")==second);
    REQUIRE_FALSE(Profile::ById(static_cast<Profile::Id>(0)));
    REQUIRE_FALSE(Profile::ByModelPath({}));
    REQUIRE_FALSE(Profile::ByModelPath(R"(data3d\skala2.p3d.bak)"));
    REQUIRE_FALSE(Profile::ByModelPath(R"(data3d\..\skala2.p3d)"));
    REQUIRE_FALSE(Profile::ByModelPath(R"(other\skala2.p3d)"));
    REQUIRE_FALSE(Profile::ByModelMember(R"(other\skala2.p3d)"));
}

TEST_CASE("Retail rigid profiles keep each model and sole PAC pair distinct",
          "[retail-rigid-profile]")
{
    const auto& skala=*Profile::ById(Profile::Id::SkalaNew);
    const auto& second=*Profile::ById(Profile::Id::Skala2);
    REQUIRE(skala.visualLevels==4);REQUIRE(second.visualLevels==3);
    REQUIRE(skala.fineSourceLod==0);REQUIRE(skala.coarseSourceLod==1);
    REQUIRE(second.fineSourceLod==0);REQUIRE(second.coarseSourceLod==1);
    REQUIRE(skala.selectedNoShadow);REQUIRE_FALSE(second.selectedNoShadow);
    REQUIRE(skala.MatchesModelMember("skala_new.p3d"));
    REQUIRE_FALSE(skala.MatchesModelMember("skala2.p3d"));
    REQUIRE(second.MatchesModelMember("skala2.p3d"));
    REQUIRE_FALSE(second.MatchesModelMember("skala_new.p3d"));
    REQUIRE(skala.MatchesTexturePath(R"(DATA/skala_piskovec2.pac)"));
    REQUIRE_FALSE(skala.MatchesTexturePath(R"(data\piskovec.pac)"));
    REQUIRE(second.MatchesTexturePath(R"(DATA/piskovec.pac)"));
    REQUIRE_FALSE(second.MatchesTexturePath(R"(data\skala_piskovec2.pac)"));
    REQUIRE(skala.MatchesTextureMember("skala_piskovec2.pac"));
    REQUIRE(second.MatchesTextureMember("piskovec.pac"));
    REQUIRE_FALSE(second.MatchesTextureMember("skala_piskovec2.pac"));
}

TEST_CASE("Retail rigid archive suffixes require exact member archive and path boundary",
          "[retail-rigid-profile]")
{
    const auto& second=*Profile::ById(Profile::Id::Skala2);
    REQUIRE(second.MatchesModelArchive(R"(D:\SteamLibrary\ARMA Cold War Assault\DTA\data3d.pbo)"));
    REQUIRE(second.MatchesModelArchive(R"(dta/data3d.pbo)"));
    REQUIRE(second.MatchesTextureArchive(R"(D:/SteamLibrary/ARMA Cold War Assault/DTA/data.pbo)"));
    REQUIRE_FALSE(second.MatchesModelArchive(R"(D:\SteamLibrary\otherdata3d.pbo)"));
    REQUIRE_FALSE(second.MatchesModelArchive(R"(D:\SteamLibrary\DTA\data3d.pbo.bak)"));
    REQUIRE_FALSE(second.MatchesModelArchive(R"(D:\SteamLibrary\DTA\data.pbo)"));
    REQUIRE_FALSE(second.MatchesTextureArchive(R"(D:\SteamLibrary\DTA\data3d.pbo)"));
    REQUIRE_FALSE(second.MatchesTextureArchive(R"(D:\SteamLibrary\DTA\data.pbo.bak)"));
}
