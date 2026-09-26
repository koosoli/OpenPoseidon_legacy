#include <catch2/catch_test_macros.hpp>
#include "../../test_fixtures.hpp"
#include <Poseidon/World/Terrain/ObjectStreamPrepare.hpp>
#include <Poseidon/World/Model/ModelCache.hpp>
#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using Poseidon::ObjectStreamPreparer;
namespace IR = Poseidon::Model;
namespace Adapter = Poseidon::Model::ShapeAdapter;
namespace
{
template<class Predicate> bool WaitRadius(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do
    {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}
struct RestoreExternalLoader
{
    Poseidon::ModelCache::ExternalLoader previous = Poseidon::ModelCache::GetExternalLoader();
    ~RestoreExternalLoader() { Poseidon::ModelCache::SetExternalLoader(previous); }
};
std::shared_ptr<IR::Model> RadiusModel(const std::string& identity, float radius)
{
    auto model = std::make_shared<IR::Model>();
    model->sourcePath = identity;
    model->sourceFormat = "ODOL";
    model->sourceVersion = 7;
    model->boundingSphere.radius = radius;
    model->allowAnimation = 1; // engine certificate must not reject animated models
    model->lodLevels.emplace_back(1.0f);
    auto& mesh = model->lodLevels[0].mesh;
    mesh.materials.emplace_back("untextured");
    for (const IR::Vector3 position : {IR::Vector3{0,0,0}, IR::Vector3{3,0,0}, IR::Vector3{0,4,0}})
    {
        IR::Vertex vertex;
        vertex.position = position;
        vertex.normal = {0,0,1};
        mesh.vertices.push_back(vertex);
    }
    mesh.triangles.emplace_back(0,1,2,0);
    mesh.boundingBox.min = {0,0,0};
    mesh.boundingBox.max = {3,4,0};
    model->boundingBox = mesh.boundingBox;
    if (!model->compile()) throw std::runtime_error("radius fixture did not compile");
    return model;
}
}

TEST_CASE("Preparer radius install seam validates inventory tokens and finite bounds", "[preparer][radius-certificate]")
{
    ObjectStreamPreparer prep;
    const std::string path = "Radius\\Model.P3D";
    prep.Reset(&path, 1);
    const auto token = prep.QueryRadius(0);
    REQUIRE(token.state == ObjectStreamPreparer::RadiusState::Unknown);
    CHECK(token.provenance == ObjectStreamPreparer::RadiusProvenance::None);
    CHECK(token.modelIdentity == path);
    CHECK(prep.SnapshotStats().radiusCertificateBytes >= sizeof(float));
    CHECK_FALSE(prep.RecordAdaptedRadius(1, token.generation, path, 3));
    CHECK_FALSE(prep.RecordAdaptedRadius(0, token.generation, "other.p3d", 3));
    for (float invalid : {-1.0f, std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
        CHECK_FALSE(prep.RecordAdaptedRadius(0, token.generation, path, invalid));
    CHECK(prep.QueryRadius(0).state == ObjectStreamPreparer::RadiusState::Unknown);
    CHECK_FALSE(prep.RecordAdaptedRadius(0, token.generation, "radius\\model.p3d", 0));
    CHECK_FALSE(prep.RecordAdaptedRadius(0, token.generation, "Radius/Model.P3D", 0));
    REQUIRE(prep.RecordAdaptedRadius(0, token.generation, path, 0));
    CHECK(prep.QueryRadius(0).state == ObjectStreamPreparer::RadiusState::Certified);
    REQUIRE(prep.RecordAdaptedRadius(0, token.generation, path, 12.75f));
    CHECK_FALSE(prep.RecordAdaptedRadius(0, token.generation, "radius\\model.p3d",
                                       std::numeric_limits<float>::quiet_NaN()));
    CHECK(prep.QueryRadius(0).radius == 12.75f); // identity mismatch must not invalidate this slot
    CHECK_FALSE(prep.RecordAdaptedRadius(0, token.generation, path,
                                       std::numeric_limits<float>::quiet_NaN()));
    CHECK(prep.QueryRadius(0).state == ObjectStreamPreparer::RadiusState::Unknown);
    CHECK(prep.QueryRadius(0).provenance == ObjectStreamPreparer::RadiusProvenance::None);
    REQUIRE(prep.RecordAdaptedRadius(0, token.generation, path, 12.75f));
    const std::string replacement = "replacement.p3d";
    prep.Reset(&replacement, 1);
    CHECK(prep.QueryRadius(0).generation != token.generation);
    CHECK(prep.QueryRadius(0).state == ObjectStreamPreparer::RadiusState::Unknown);
    CHECK_FALSE(prep.RecordAdaptedRadius(0, token.generation, path, 99));
    const auto current = prep.QueryRadius(0);
    CHECK_FALSE(prep.RecordAdaptedRadius(0, current.generation, path, 99));
    prep.Reset(&replacement, 0);
    CHECK(prep.SnapshotStats().radiusCertificateBytes == 0);
    CHECK(prep.QueryRadius(0).state == ObjectStreamPreparer::RadiusState::Unknown);
}

TEST_CASE("Canonical ODOL parse and adapted radius survive consumption and payload retirement", "[preparer][radius-certificate][ShapeAdapter]")
{
    if (!ObjectStreamPreparer::AsyncEnabled()) SKIP("Requires workers");
    ObjectStreamPreparer prep;
    const std::string path = TestFixtures::GetTestFixturePath("p3d/animated_morph_odol.p3d");
    prep.Reset(&path, 1);
    REQUIRE(prep.Request(0));
    REQUIRE(WaitRadius([&] { return prep.Query(0) == ObjectStreamPreparer::State::Ready; }));
    const auto parsed = prep.QueryRadius(0);
    REQUIRE(parsed.state == ObjectStreamPreparer::RadiusState::Certified);
    CHECK(parsed.provenance == ObjectStreamPreparer::RadiusProvenance::ParsedODOL);
    auto model = prep.Take(0);
    REQUIRE(model);
    CHECK(parsed.radius == model->boundingSphere.radius);
    CHECK(prep.QueryRadius(0).radius == parsed.radius);
    auto tables = std::make_shared<Adapter::AdapterBankTables>();
    Adapter::BuildAdapterBankTables(*model, *tables);
    REQUIRE(prep.SubmitConvert(0, model, tables));
    REQUIRE(WaitRadius([&] { return prep.Query(0) == ObjectStreamPreparer::State::Converted; }));
    const auto adapted = prep.QueryRadius(0);
    REQUIRE(adapted.provenance == ObjectStreamPreparer::RadiusProvenance::AdaptedShape);
    auto converted = prep.TakeConverted(0);
    std::unique_ptr<Poseidon::LODShapeWithShadow> shape(converted.shape);
    REQUIRE(shape);
    CHECK(adapted.radius == shape->BoundingSphere());
    Adapter::FinishOdolAdapterTail(shape.get(), *converted.model, false);
    CHECK(adapted.radius == shape->BoundingSphere());
    CHECK(prep.QueryRadius(0).radius == adapted.radius);
    REQUIRE(prep.SubmitConvert(0, model, tables));
    REQUIRE(WaitRadius([&] { return prep.Query(0) == ObjectStreamPreparer::State::Converted; }));
    const uint32_t epoch = 0;
    CHECK(prep.DropStale(&epoch, 1, 1) == 1);
    CHECK(prep.Query(0) == ObjectStreamPreparer::State::Unknown);
    CHECK(prep.QueryRadius(0).state == ObjectStreamPreparer::RadiusState::Certified);
    CHECK(prep.QueryRadius(0).radius == adapted.radius);
    REQUIRE(prep.Request(0));
    REQUIRE(WaitRadius([&] { return prep.Query(0) == ObjectStreamPreparer::State::Ready; }));
    CHECK(prep.QueryRadius(0).provenance == ObjectStreamPreparer::RadiusProvenance::AdaptedShape);
    CHECK(prep.DropStale(&epoch, 1, 1) == 1);
    CHECK(prep.QueryRadius(0).radius == adapted.radius);
}

TEST_CASE("MLOD and external Xob IR bounds cannot certify engine radius before adaptation", "[preparer][radius-certificate][ShapeAdapter]")
{
    if (!ObjectStreamPreparer::AsyncEnabled()) SKIP("Requires workers");
    RestoreExternalLoader restore;
    Poseidon::ModelCache::SetExternalLoader(+[](const std::string& path, std::string&) {
        return RadiusModel(path, 17.5f); // even an external ODOL label is not a canonical parser guarantee
    });
    for (const std::string path : {std::string(TestFixtures::GetTestFixturePath("mlod/p3dm_two_lod.p3d")),
                                    std::string("radius_external.xob")})
    {
        ObjectStreamPreparer prep;
        prep.Reset(&path, 1);
        REQUIRE(prep.Request(0));
        REQUIRE(WaitRadius([&] { return prep.Query(0) == ObjectStreamPreparer::State::Ready; }));
        CHECK(prep.QueryRadius(0).state == ObjectStreamPreparer::RadiusState::Unknown);
        auto model = prep.Take(0);
        REQUIRE(model);
        auto tables = std::make_shared<Adapter::AdapterBankTables>();
        Adapter::BuildAdapterBankTables(*model, *tables);
        REQUIRE(prep.SubmitConvert(0, model, tables));
        REQUIRE(WaitRadius([&] { return prep.Query(0) == ObjectStreamPreparer::State::Converted; }));
        auto converted = prep.TakeConverted(0);
        std::unique_ptr<Poseidon::LODShapeWithShadow> shape(converted.shape);
        REQUIRE(shape);
        const auto certificate = prep.QueryRadius(0);
        REQUIRE(certificate.state == ObjectStreamPreparer::RadiusState::Certified);
        CHECK(certificate.radius == shape->BoundingSphere());
        CHECK(certificate.provenance == ObjectStreamPreparer::RadiusProvenance::AdaptedShape);
    }
}

TEST_CASE("Worker adaptation rejects invalid radius and mismatched model identity", "[preparer][radius-certificate][ShapeAdapter]")
{
    if (!ObjectStreamPreparer::AsyncEnabled()) SKIP("Requires workers");
    const std::string path = "radius_valid.p3d";
    for (int arm = 0; arm < 4; ++arm)
    {
        ObjectStreamPreparer prep;
        prep.Reset(&path, 1);
        const auto token = prep.QueryRadius(0);
        REQUIRE(prep.RecordAdaptedRadius(0, token.generation, path, 7));
        const float radius = arm == 0 ? -3.0f : arm == 1 ? std::numeric_limits<float>::quiet_NaN() :
                             arm == 2 ? std::numeric_limits<float>::infinity() : 5.0f;
        auto model = RadiusModel(arm == 3 ? "different.p3d" : path, radius);
        auto tables = std::make_shared<Adapter::AdapterBankTables>();
        Adapter::BuildAdapterBankTables(*model, *tables);
        REQUIRE(prep.SubmitConvert(0, model, tables));
        REQUIRE(WaitRadius([&] { return prep.Query(0) == ObjectStreamPreparer::State::Converted; }));
        if (arm == 3)
        {
            CHECK(prep.QueryRadius(0).state == ObjectStreamPreparer::RadiusState::Certified);
            CHECK(prep.QueryRadius(0).radius == 7); // wrong model cannot alter existing evidence
        }
        else
            CHECK(prep.QueryRadius(0).state == ObjectStreamPreparer::RadiusState::Unknown);
        auto converted = prep.TakeConverted(0);
        delete converted.shape;
    }
}

TEST_CASE("Only exact owner tokens authorize renamed sourcePath certification", "[preparer][radius-certificate][ShapeAdapter]")
{
    if (!ObjectStreamPreparer::AsyncEnabled()) SKIP("Requires workers");
    const std::string path = "RadiusOwner\\Model.P3D";
    for (int arm = 0; arm < 4; ++arm)
    {
        ObjectStreamPreparer prep;
        prep.Reset(&path, 1);
        auto token = prep.QueryRadius(0);
        auto model = RadiusModel(arm <= 1 ? "radiusowner\\model.p3d" : path, 9.25f);
        auto tables = std::make_shared<Adapter::AdapterBankTables>();
        Adapter::BuildAdapterBankTables(*model, *tables);
        if (arm == 2) token.modelIdentity = "radiusowner\\model.p3d";
        if (arm == 3)
        {
            prep.Reset(&path, 1); // exact string retained, generation replaced
            const auto current = prep.QueryRadius(0);
            REQUIRE(prep.RecordAdaptedRadius(0, current.generation, path, 2));
        }
        REQUIRE(prep.SubmitConvert(0, model, tables, arm == 0 ? nullptr : &token));
        token.modelIdentity.clear(); // SubmitConvert must own a copy, not borrow this token
        token.generation = 0;
        REQUIRE(WaitRadius([&] { return prep.Query(0) == ObjectStreamPreparer::State::Converted; }));
        auto converted = prep.TakeConverted(0);
        std::unique_ptr<Poseidon::LODShapeWithShadow> shape(converted.shape);
        REQUIRE(shape); // certification rejection must not reject conversion
        const auto certificate = prep.QueryRadius(0);
        CHECK(certificate.modelIdentity == path);
        if (arm == 1)
        {
            REQUIRE(certificate.state == ObjectStreamPreparer::RadiusState::Certified);
            CHECK(certificate.radius == shape->BoundingSphere());
        }
        else if (arm == 3)
        {
            CHECK(certificate.state == ObjectStreamPreparer::RadiusState::Certified);
            CHECK(certificate.radius == 2); // rejected stale token cannot mutate current evidence
        }
        else
            CHECK(certificate.state == ObjectStreamPreparer::RadiusState::Unknown);
    }
}

TEST_CASE("Reset prevents blocked old workers and queued conversions certifying replacement inventory", "[preparer][radius-certificate][cancellation]")
{
    if (!ObjectStreamPreparer::AsyncEnabled()) SKIP("Requires workers");
    static std::atomic<unsigned> entered{0};
    static std::atomic<bool> release{false};
    entered = 0; release = false;
    RestoreExternalLoader restore;
    Poseidon::ModelCache::SetExternalLoader(+[](const std::string& path, std::string&) {
        ++entered;
        WaitRadius([] { return release.load(); });
        return RadiusModel(path, 91.0f);
    });
    ObjectStreamPreparer prep;
    struct ReleaseBeforeJoin { ~ReleaseBeforeJoin() { release = true; } } unblock;
    const size_t workers = ObjectStreamPreparer::WorkerCount();
    std::vector<std::string> paths(workers + 1, "radius_blocked.xob");
    paths.back() = "radius_queued.p3d";
    prep.Reset(paths.data(), paths.size());
    for (size_t i = 0; i < workers; ++i) REQUIRE(prep.Request(static_cast<uint32_t>(i)));
    REQUIRE(WaitRadius([&] { return entered.load() == workers; }));
    auto model = RadiusModel(paths.back(), 91);
    auto tables = std::make_shared<Adapter::AdapterBankTables>();
    Adapter::BuildAdapterBankTables(*model, *tables);
    REQUIRE(prep.SubmitConvert(static_cast<uint32_t>(workers), model, tables));
    const auto old = prep.QueryRadius(static_cast<uint32_t>(workers));
    const std::string replacement = paths.back();
    prep.Reset(&replacement, 1);
    const auto current = prep.QueryRadius(0);
    CHECK_FALSE(prep.RecordAdaptedRadius(0, old.generation, replacement, 91));
    REQUIRE(prep.RecordAdaptedRadius(0, current.generation, replacement, 4.25f));
    release = true;
    REQUIRE(WaitRadius([&] { return prep.SnapshotStats().parseReservedBytes == 0; }));
    CHECK(prep.QueryRadius(0).radius == 4.25f);
    CHECK(prep.QueryRadius(0).generation == current.generation);
    CHECK(prep.Query(0) == ObjectStreamPreparer::State::Unknown);
}
