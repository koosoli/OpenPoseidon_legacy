#include <catch2/catch_test_macros.hpp>
#include <WgpuRenderer/GeometrySourceRevisions.hpp>

using Poseidon::render::GeometrySourceRevisions;

TEST_CASE("Geometry source retirement rejects an in-flight fact after the same IDs re-register",
    "[geometry-source-revision]")
{
    GeometrySourceRevisions sources(31);
    const auto old = sources.Register(7, 12, {100, 101}, true);
    REQUIRE(old != 0);
    REQUIRE(sources.Fresh(7, 12, 31, old));

    sources.Retire(7);
    CHECK_FALSE(sources.Fresh(7, 12, 31, old));
    const auto replacement = sources.Register(7, 12, {200}, true);
    REQUIRE(replacement > old);
    CHECK_FALSE(sources.Fresh(7, 12, 31, old));
    CHECK(sources.Fresh(7, 12, 31, replacement));
}

TEST_CASE("A duplicate live producer poisons the original count witness",
    "[geometry-source-revision]")
{
    GeometrySourceRevisions sources(42);
    const auto first = sources.Register(5, 9, {33}, true);
    REQUIRE(first != 0);
    sources.Duplicate(5); // the renderer drain rejects this before a second Rust registration
    CHECK(sources.Register(5, 10, {44}, true) == 0);
    CHECK_FALSE(sources.Fresh(5, 9, 42, first));
    CHECK_FALSE(sources.Fresh(5, 10, 42, first));
    CHECK(sources.Revision(5, 9) == 0);
    sources.Retire(5);
    CHECK(sources.Register(5, 10, {44}, true) > first);
}

TEST_CASE("Mesh death invalidates only registrations that reference that mesh",
    "[geometry-source-revision]")
{
    GeometrySourceRevisions sources(10);
    const auto changed = sources.Register(1, 11, {100, 101}, true);
    const auto unrelated = sources.Register(2, 22, {200}, true);
    REQUIRE(changed != 0);
    REQUIRE(unrelated != 0);
    sources.MeshChanged(999);
    CHECK(sources.Fresh(1, 11, 10, changed));
    sources.MeshChanged(101);
    CHECK_FALSE(sources.Fresh(1, 11, 10, changed));
    CHECK(sources.Fresh(2, 22, 10, unrelated));
    CHECK(sources.Register(1, 11, {101}, true) == 0); // no revival before retire
}

TEST_CASE("An incomplete registration and a different renderer incarnation stay unknown",
    "[geometry-source-revision]")
{
    GeometrySourceRevisions sources(70);
    CHECK(sources.Register(1, 2, {3}, false) == 0);
    CHECK(sources.Revision(1, 2) == 0);
    sources.Retire(1);
    const auto revision = sources.Register(1, 2, {3}, true);
    REQUIRE(revision != 0);
    CHECK_FALSE(sources.Fresh(1, 2, 71, revision));
    CHECK_FALSE(sources.Fresh(1, 3, 70, revision));
    CHECK(sources.Fresh(1, 2, 70, revision));
}

TEST_CASE("Revision and producer handle exhaustion never recycle identity",
    "[geometry-source-revision]")
{
    GeometrySourceRevisions sources(1, UINT64_MAX - 2);
    const auto penultimate = sources.Register(1, 1, {10}, true);
    const auto last = sources.Register(2, 2, {20}, true);
    REQUIRE(penultimate == UINT64_MAX - 2);
    REQUIRE(last == UINT64_MAX - 1);
    CHECK(sources.Register(3, 3, {30}, true) == 0);
    sources.MeshChanged(10);
    CHECK_FALSE(sources.Fresh(1, 1, 1, penultimate));
    CHECK(sources.Fresh(2, 2, 1, last));

    uint32_t nextModel = UINT32_MAX - 1;
    CHECK(GeometrySourceRevisions::TakeModelHandle(nextModel) == UINT32_MAX - 1);
    CHECK(GeometrySourceRevisions::TakeModelHandle(nextModel) == UINT32_MAX);
    CHECK(GeometrySourceRevisions::TakeModelHandle(nextModel) == UINT32_MAX);
    uint64_t nextMesh = UINT64_MAX - 1;
    CHECK(GeometrySourceRevisions::TakeMeshHandle(nextMesh) == UINT64_MAX - 1);
    CHECK(GeometrySourceRevisions::TakeMeshHandle(nextMesh) == 0);
    CHECK(GeometrySourceRevisions::TakeMeshHandle(nextMesh) == 0);
}
