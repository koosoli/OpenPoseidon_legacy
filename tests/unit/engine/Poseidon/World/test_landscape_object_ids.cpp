#include <catch2/catch_test_macros.hpp>
#include <Poseidon/AI/Path/AITypes.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Terrain/WrpReader.hpp>
#include "test_fixtures.hpp"

using namespace Poseidon;

#ifdef GetObject
#undef GetObject
#endif

namespace
{
class GlobalLandscapeScope
{
  public:
    explicit GlobalLandscapeScope(Landscape* landscape) : _previous(GLandscape) { GLandscape = landscape; }

    ~GlobalLandscapeScope() { GLandscape = _previous; }

  private:
    Landscape* _previous;
};

Ref<ObjectPlain> AddLandscapeObject(Landscape& landscape, int id)
{
    Ref<ObjectPlain> object = new ObjectPlain(nullptr, id);
    landscape.AddObject(object, nullptr, nullptr, true);
    return object;
}
} // namespace

TEST_CASE("Terrain cache flush preserves live AI lock references", "[World][Terrain][lock-cache]")
{
    Landscape landscape(nullptr, nullptr);
    GlobalLandscapeScope globalLandscape(&landscape);
    ILockCache* cache = landscape.LockingCache();
    REQUIRE(cache != nullptr);
    REQUIRE(cache->IsEmpty());

    // Multiple units can own the same field; each must retain its original
    // reference through crater/editor cache invalidation and release it once.
    LockField* field = cache->GetLockField(1, 1);
    REQUIRE(field != nullptr);
    for (int unit = 1; unit < 63; ++unit)
        REQUIRE(cache->GetLockField(1, 1) == field);
    LockField* neighbor = cache->GetLockField(2, 1);
    REQUIRE(neighbor != nullptr);
    field->Lock(0, 0, false, true);
    neighbor->Lock(1, 0, true, true);

    for (int impact = 0; impact < 4; ++impact)
    {
        landscape.FlushCache();
        // Require identity before touching the fields: old code deleted both.
        REQUIRE(landscape.LockingCache() == cache);
        REQUIRE(cache->FindLockField(1, 1) == field);
        REQUIRE(cache->FindLockField(2, 1) == neighbor);
        CHECK(field->IsLocked(0, 0, false));
        CHECK(neighbor->IsLocked(1, 0, false));
    }

    field->Lock(0, 0, false, false);
    for (int unit = 1; unit < 63; ++unit)
    {
        cache->ReleaseLockField(1, 1);
        REQUIRE(cache->FindLockField(1, 1) == field);
    }
    cache->ReleaseLockField(1, 1);
    CHECK(cache->FindLockField(1, 1) == nullptr);
    CHECK_FALSE(cache->IsEmpty());
    neighbor->Lock(1, 0, true, false);
    cache->ReleaseLockField(2, 1);
    CHECK(cache->IsEmpty());
    landscape.FlushCache();
    REQUIRE(landscape.LockingCache() != nullptr);
    CHECK(landscape.LockingCache()->IsEmpty());
}

TEST_CASE("Landscape upload view preserves row-major float heights", "[World][Terrain][heightmap-view]")
{
    class Heightfield : public Landscape
    {
      public:
        Heightfield() : Landscape(nullptr, nullptr) {}
        using Landscape::Dim;
        using Landscape::SetData;
    };
    Heightfield landscape;
    GlobalLandscapeScope globalLandscape(&landscape);
    landscape.Dim(16, 16, 32, 32, 50.0f);
    const int range = landscape.GetTerrainRange();
    for (int z = 0; z < range; ++z)
        for (int x = 0; x < range; ++x)
            landscape.SetData(x, z, x * 0.25f - z * 1.5f);
    const float* heights = landscape.HeightmapData();
    REQUIRE(heights != nullptr);
    for (int z = 0; z < range; ++z)
        for (int x = 0; x < range; ++x)
            CHECK(heights[z * range + x] == landscape.GetHeight(z, x));
    landscape.SetData(5, 7, -123.5f);
    CHECK(heights[7 * range + 5] == -123.5f);
}

TEST_CASE("Landscape ID cache uses the highest sparse object ID", "[World][Terrain][ObjectID]")
{
    WrpReader reader;
    REQUIRE(reader.Load(GET_FIXTURE("wrp/test_world.wrp")));

    Landscape landscape(nullptr, nullptr);
    GlobalLandscapeScope globalLandscape(&landscape);
    Ref<ObjectPlain> highObject = AddLandscapeObject(landscape, reader.GetObject(0).id);
    Ref<ObjectPlain> lowObject = AddLandscapeObject(landscape, reader.GetObject(1).id);
    Ref<ObjectPlain> middleObject = AddLandscapeObject(landscape, reader.GetObject(2).id);

    landscape.RebuildIDCache();

    REQUIRE(landscape.GetLastObjectID() == 17);
    REQUIRE(landscape.GetObject(2) == lowObject);
    REQUIRE(landscape.GetObject(9) == middleObject);
    REQUIRE(landscape.GetObject(17) == highObject);
}

TEST_CASE("Landscape runtime IDs preserve terrain cache entries", "[World][Terrain][ObjectID]")
{
    Landscape landscape(nullptr, nullptr);
    GlobalLandscapeScope globalLandscape(&landscape);
    Ref<ObjectPlain> terrainObject = AddLandscapeObject(landscape, 2);
    Ref<ObjectPlain> highTerrainObject = AddLandscapeObject(landscape, 17);

    landscape.RebuildIDCache();

    Ref<ObjectPlain> firstRuntimeObject = new ObjectPlain(nullptr, landscape.NewObjectID());
    Ref<ObjectPlain> secondRuntimeObject = new ObjectPlain(nullptr, landscape.NewObjectID());
    Ref<ObjectPlain> thirdRuntimeObject = new ObjectPlain(nullptr, landscape.NewObjectID());
    landscape.AddToIDCache(firstRuntimeObject);
    landscape.AddToIDCache(secondRuntimeObject);
    landscape.AddToIDCache(thirdRuntimeObject);

    CHECK(firstRuntimeObject->ID() == 18);
    CHECK(secondRuntimeObject->ID() == 19);
    CHECK(thirdRuntimeObject->ID() == 20);
    CHECK(landscape.GetObject(18) == firstRuntimeObject);
    CHECK(landscape.GetObject(19) == secondRuntimeObject);
    CHECK(landscape.GetObject(20) == thirdRuntimeObject);
    REQUIRE(landscape.GetObject(2) == terrainObject);
    REQUIRE(landscape.GetObject(17) == highTerrainObject);

    landscape.GetObject(2)->SetDammage(0.5f);
    landscape.GetObject(17)->SetDammage(0.25f);
    REQUIRE(terrainObject->GetTotalDammage() == 0.5f);
    REQUIRE(highTerrainObject->GetTotalDammage() == 0.25f);
}

TEST_CASE("Landscape ID cache rebuild raises the runtime ID floor", "[World][Terrain][ObjectID]")
{
    Landscape landscape(nullptr, nullptr);
    GlobalLandscapeScope globalLandscape(&landscape);
    Ref<ObjectPlain> terrainObject = AddLandscapeObject(landscape, 2);

    landscape.RebuildIDCache();
    REQUIRE(landscape.GetLastObjectID() == 2);

    Ref<ObjectPlain> runtimeObject = AddLandscapeObject(landscape, landscape.NewObjectID());
    Ref<ObjectPlain> laterTerrainObject = AddLandscapeObject(landscape, 42);
    landscape.RebuildIDCache();

    REQUIRE(landscape.GetLastObjectID() == 42);
    REQUIRE(landscape.GetObject(2) == terrainObject);
    REQUIRE(landscape.GetObject(3) == runtimeObject);
    REQUIRE(landscape.GetObject(42) == laterTerrainObject);
    REQUIRE(landscape.NewObjectID() == 43);
}
