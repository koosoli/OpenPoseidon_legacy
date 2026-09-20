#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Model/ModelCache.hpp>
#include <Poseidon/World/Model/ModelBlob.hpp>
#include <Poseidon/World/Model/Model.hpp>
#include "../../test_fixtures.hpp"
#include <fstream>
#include <iterator>
using Poseidon::ModelCache;

TEST_CASE("Owned model bytes preserve complete compiled P3D content", "[model][owned-read]")
{
    for (const char* fixture : {"mlod/complex_vehicle_mlod.p3d", "p3d/complex_vehicle.p3d"})
    {
        const std::string path = GET_FIXTURE(fixture);
        CAPTURE(path);
        std::ifstream input(path, std::ios::binary);
        REQUIRE(input.good());
        std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
        std::string error;
        auto owned = ModelCache::LoadOwnedBytes(bytes.data(), bytes.size(), path, error);
        REQUIRE(owned);
        REQUIRE(owned->isCompiled());
        ModelCache cache;
        auto baseline = cache.load(path);
        REQUIRE(baseline);
        const bool identical = Poseidon::ModelBlob::Serialize(*owned) == Poseidon::ModelBlob::Serialize(*baseline);
        CHECK(identical); // every serialized IR field, including special/collision LODs
        CHECK_FALSE(ModelCache::LoadOwnedBytes(bytes.data(), 3, path, error));
        CHECK_FALSE(ModelCache::LoadOwnedBytes(nullptr, bytes.size(), path, error));
        CHECK_FALSE(ModelCache::LoadOwnedBytes("invalid-model", 13, path, error));
    }
}
