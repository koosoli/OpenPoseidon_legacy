#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Asset/Formats/P3D/MLODLoader.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/Graphics/Dummy/EngineDummy.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>
#include <Poseidon/World/Scene/Scene.hpp>

#include "../../test_fixtures.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace Poseidon;

namespace
{
class TrackedLogicalBuffer final : public VertexBuffer
{
public:
    explicit TrackedLogicalBuffer(int& live) : _live(live) { ++_live; }
    ~TrackedLogicalBuffer() override { --_live; }
    void Update(const Shape&, bool) override {}
private:
    int& _live;
};

class LogicalLoadEngine final : public EngineDummy
{
public:
    int vertexBufferCalls = 0;
    int liveBuffers = 0;
    int throwOnBufferCall = 0;
    bool returnBuffers = false;
    bool throwOnWorldLoad = false;
    std::vector<LODShapeWithShadow*> worldLoads;
    std::vector<VBType> bufferTypes;

    void WorldShapeLoaded(LODShapeWithShadow* shape) override
    {
        if (throwOnWorldLoad)
            throw std::runtime_error("mock retained registration failure");
        worldLoads.push_back(shape);
    }

    VertexBuffer* CreateVertexBuffer(const Shape&, VBType type) override
    {
        ++vertexBufferCalls;
        bufferTypes.push_back(type);
        if (vertexBufferCalls == throwOnBufferCall)
            throw std::runtime_error("mock later-LOD allocation failure");
        if (returnBuffers)
            return new TrackedLogicalBuffer(liveBuffers);
        // A null buffer is a supported backend fallback. Counting the actual
        // callback proves logical loads did not reach either GPU ownership path.
        return nullptr;
    }
};

struct RenderingGuard
{
    Engine* previousEngine = GEngine;
    bool previousHWTL = ENGINE_CONFIG.enableHWTL;
    bool previousPIII = ENGINE_CONFIG.enablePIII;
    explicit RenderingGuard(Engine* engine)
    {
        Foundation::CaptureMainThread();
        GEngine = engine;
        ENGINE_CONFIG.enableHWTL = true;
        ENGINE_CONFIG.enablePIII = false;
    }
    ~RenderingGuard()
    {
        GEngine = previousEngine;
        ENGINE_CONFIG.enableHWTL = previousHWTL;
        ENGINE_CONFIG.enablePIII = previousPIII;
    }
};

std::shared_ptr<Model::Model> BoxModel()
{
    return std::make_shared<Model::Model>(Asset::Formats::MLODLoader::load(
        GET_FIXTURE("mlod/p3dm_geometry_mass.p3d")));
}

void RequireCollisionGeometry(const LODShapeWithShadow& shape)
{
    REQUIRE(shape.FindGeometryLevel() == 1);
    REQUIRE(shape.GeometryLevel()->NFaces() == 6);
    REQUIRE(shape.Mass() == Catch::Approx(1200.0f).margin(0.5f));
    const ConvexComponents& components = shape.GetGeomComponents();
    REQUIRE(components.Size() == 1);
    REQUIRE(components[0]->NPlanes() == 6);
    REQUIRE(components[0]->IsInside(::Vector3(0, 0, 0)));
    REQUIRE_FALSE(components[0]->IsInside(::Vector3(3, 0, 0)));
}
}

TEST_CASE("Logical ShapeBank load retains collision geometry without registering rendering",
          "[ShapeBank][streaming-logical-shape]")
{
    LogicalLoadEngine engine;
    RenderingGuard guard(&engine);
    ShapeBank bank;
    bank.OptimizeAll(); // Exercise the late-load path that formerly uploaded logical objects.
    Ref<LODShapeWithShadow> shape;
    {
        ShapeBank::WorldModelScope world(bank);
        ShapeBank::CpuOnlyLoadScope logical;
        shape = bank.NewFromModel("logical_box.p3d", false, true, BoxModel(), nullptr);
        REQUIRE(shape->DeferredVisualRendering());
        REQUIRE_FALSE(shape->GetCompressedModelSourceBirth());
        REQUIRE(shape->GetCompressedModelSourceBirthId() == 0);
        RequireCollisionGeometry(*shape);
        REQUIRE(engine.worldLoads.empty());
        REQUIRE(engine.vertexBufferCalls == 0);
        REQUIRE(bank.New("logical_box.p3d", false, true) == shape.GetRef());
    }
    bank.OptimizeAll(); // Bulk optimisation must not promote unused logical shapes.
    REQUIRE(shape->DeferredVisualRendering());
    REQUIRE(engine.worldLoads.empty());
    REQUIRE(engine.vertexBufferCalls == 0);

    REQUIRE(bank.New("logical_box.p3d", false, true) == shape.GetRef());
    REQUIRE_FALSE(shape->DeferredVisualRendering());
    REQUIRE_FALSE(shape->GetCompressedModelSourceBirth()); // cache promotion cannot retrofit a birth
    REQUIRE(shape->GetCompressedModelSourceBirthId() == 0);
    REQUIRE(engine.worldLoads == std::vector<LODShapeWithShadow*>{shape.GetRef()});
    REQUIRE(engine.vertexBufferCalls > 0);
    RequireCollisionGeometry(*shape);
    const int promotedBufferCalls = engine.vertexBufferCalls;
    bank.EnsureVisualRendering(shape);
    REQUIRE(bank.New("logical_box.p3d", false, true) == shape.GetRef());
    REQUIRE(engine.worldLoads.size() == 1);
    REQUIRE(engine.vertexBufferCalls == promotedBufferCalls);
}

TEST_CASE("Deferred visual promotion survives unavailable engine and failed registration",
          "[ShapeBank][streaming-logical-shape]")
{
    LogicalLoadEngine engine;
    RenderingGuard guard(&engine);
    ShapeBank bank;
    bank.OptimizeAll();
    Ref<LODShapeWithShadow> shape;
    {
        ShapeBank::WorldModelScope world(bank);
        ShapeBank::CpuOnlyLoadScope logical;
        shape = bank.NewFromModel("logical_retry.p3d", false, true, BoxModel(), nullptr);
    }
    GEngine = nullptr;
    REQUIRE(bank.New("logical_retry.p3d", false, true) == shape.GetRef());
    REQUIRE(shape->DeferredVisualRendering());
    GEngine = &engine;
    engine.throwOnWorldLoad = true;
    REQUIRE_THROWS_AS(bank.New("logical_retry.p3d", false, true), std::runtime_error);
    REQUIRE(shape->DeferredVisualRendering());
    REQUIRE(engine.vertexBufferCalls == 0);
    engine.throwOnWorldLoad = false;
    REQUIRE(bank.New("logical_retry.p3d", false, true) == shape.GetRef());
    REQUIRE_FALSE(shape->DeferredVisualRendering());
    REQUIRE(engine.worldLoads.size() == 1);
    REQUIRE(engine.vertexBufferCalls > 0);
}

TEST_CASE("Fresh worker shape is destroyed when owner registration throws",
          "[ShapeBank][streaming-logical-shape]")
{
    LogicalLoadEngine engine;
    RenderingGuard guard(&engine);
    ShapeBank bank;
    bank.OptimizeAll();
    ShapeBank::WorldModelScope world(bank);
    auto model = BoxModel();
    auto* prepared = Model::ShapeAdapter::convertToLODShape(*model, false);
    REQUIRE(prepared != nullptr);
    Link<LODShapeWithShadow> witness = prepared;
    engine.throwOnWorldLoad = true;
    REQUIRE_THROWS_AS(bank.NewFromModel("fresh_throw.p3d", false, true, model, nullptr, prepared), std::runtime_error);
    REQUIRE(static_cast<LODShapeWithShadow*>(witness) == nullptr);
    REQUIRE(bank.Find("fresh_throw.p3d", false, true) == nullptr);
    REQUIRE(engine.liveBuffers == 0);
    engine.throwOnWorldLoad = false;
    Ref<LODShapeWithShadow> retry = bank.NewFromModel("fresh_throw.p3d", false, true, BoxModel(), nullptr);
    REQUIRE(retry.GetRef() != nullptr);
    REQUIRE(bank.Find("fresh_throw.p3d", false, true) == retry.GetRef());
    RequireCollisionGeometry(*retry);
}

TEST_CASE("Only requested logical shapes promote after the bulk boundary",
          "[ShapeBank][streaming-logical-shape]")
{
    LogicalLoadEngine engine;
    RenderingGuard guard(&engine);
    ShapeBank bank;
    Ref<LODShapeWithShadow> requested, unused;
    {
        ShapeBank::WorldModelScope world(bank);
        ShapeBank::CpuOnlyLoadScope logical;
        requested = bank.NewFromModel("logical_requested.p3d", false, true, BoxModel(), nullptr);
        unused = bank.NewFromModel("logical_unused.p3d", false, true, BoxModel(), nullptr);
    }
    bank.New("logical_requested.p3d", false, true);
    REQUIRE(requested->DeferredVisualRendering());
    REQUIRE(engine.vertexBufferCalls == 0);
    bank.OptimizeAll();
    REQUIRE_FALSE(requested->DeferredVisualRendering());
    REQUIRE(unused->DeferredVisualRendering());
    REQUIRE(engine.worldLoads == std::vector<LODShapeWithShadow*>{requested.GetRef()});
    REQUIRE(engine.vertexBufferCalls > 0);
}

TEST_CASE("Partial visual promotion releases acquired buffers before retry",
          "[ShapeBank][streaming-logical-shape]")
{
    LogicalLoadEngine engine;
    engine.returnBuffers = true;
    RenderingGuard guard(&engine);
    ShapeBank bank;
    bank.OptimizeAll();
    Ref<LODShapeWithShadow> shape;
    {
        ShapeBank::WorldModelScope world(bank);
        ShapeBank::CpuOnlyLoadScope logical;
        auto model = BoxModel();
        model->lodLevels.push_back(model->lodLevels.front());
        model->lodLevels.back().resolution = 2.0f;
        shape = bank.NewFromModel("logical_partial.p3d", false, true, model, nullptr);
    }
    engine.throwOnBufferCall = 2;
    REQUIRE_THROWS_AS(bank.New("logical_partial.p3d", false, true), std::runtime_error);
    REQUIRE(shape->DeferredVisualRendering());
    REQUIRE(engine.vertexBufferCalls == 2);
    REQUIRE(engine.liveBuffers == 0);
    for (int level = 0; level < shape->NLevels(); ++level)
        REQUIRE(shape->Level(level)->GetVertexBuffer() == nullptr);
    RequireCollisionGeometry(*shape);
    engine.throwOnBufferCall = 0;
    bank.New("logical_partial.p3d", false, true);
    REQUIRE_FALSE(shape->DeferredVisualRendering());
    REQUIRE(engine.vertexBufferCalls == 4);
    REQUIRE(engine.liveBuffers == 2);
    // WGPU's retained registration is idempotent by shape; the bank retries
    // that callback after a failed optimisation, then completes exactly once.
    REQUIRE(engine.worldLoads.size() == 2);
    bank.New("logical_partial.p3d", false, true);
    REQUIRE(engine.vertexBufferCalls == 4);
    REQUIRE(engine.worldLoads.size() == 2);
}

TEST_CASE("CPU-only shape scope restores nesting on exception and rejects non-owner activation",
          "[ShapeBank][streaming-logical-shape]")
{
    Foundation::CaptureMainThread();
    REQUIRE_FALSE(ShapeBank::CpuOnlyLoadActive());
    {
        ShapeBank::CpuOnlyLoadScope disabled(false);
        REQUIRE_FALSE(disabled.Active());
        ShapeBank::CpuOnlyLoadScope outer;
        REQUIRE(outer.Active());
        try
        {
            ShapeBank::CpuOnlyLoadScope inner;
            REQUIRE(inner.Active());
            throw std::runtime_error("scope unwind");
        }
        catch (const std::runtime_error&) {}
        REQUIRE(ShapeBank::CpuOnlyLoadActive());
        bool workerActive = true;
        std::thread worker([&]
        {
            ShapeBank::CpuOnlyLoadScope nonOwner;
            workerActive = nonOwner.Active() || ShapeBank::CpuOnlyLoadActive();
        });
        worker.join();
        REQUIRE_FALSE(workerActive);
        REQUIRE(ShapeBank::CpuOnlyLoadActive());
    }
    REQUIRE_FALSE(ShapeBank::CpuOnlyLoadActive());
}

TEST_CASE("Ordinary late shape loads retain their rendering ownership behaviour",
          "[ShapeBank][streaming-logical-shape]")
{
    LogicalLoadEngine engine;
    RenderingGuard guard(&engine);
    ShapeBank bank;
    bank.OptimizeAll();
    ShapeBank::WorldModelScope world(bank);
    ShapeBank::CpuOnlyLoadScope disabled(false);
    Ref<LODShapeWithShadow> shape = bank.NewFromModel("visual_box.p3d", false, true, BoxModel(), nullptr);
    REQUIRE_FALSE(shape->DeferredVisualRendering());
    REQUIRE_FALSE(shape->GetCompressedModelSourceBirth()); // caller-supplied IR has no selected read
    REQUIRE(shape->GetCompressedModelSourceBirthId() == 0);
    REQUIRE(engine.worldLoads.size() == 1);
    REQUIRE(engine.vertexBufferCalls > 0);
    const int calls = engine.vertexBufferCalls;
    REQUIRE(bank.New("visual_box.p3d", false, true) == shape.GetRef());
    REQUIRE(engine.worldLoads.size() == 1);
    REQUIRE(engine.vertexBufferCalls == calls);
}

TEST_CASE("Logical proxy loads inherit CPU ownership across banks and promote each shape once",
          "[ShapeBank][streaming-logical-shape][proxy]")
{
    LogicalLoadEngine engine;
    RenderingGuard guard(&engine);
    const auto path = std::filesystem::temp_directory_path() / "poseidonlogicalproxy.p3d";
    std::filesystem::copy_file(GET_FIXTURE("mlod/p3dm_geometry_mass.p3d"), path,
                               std::filesystem::copy_options::overwrite_existing);
    struct RemoveFixture
    {
        std::filesystem::path path;
        ~RemoveFixture() { std::error_code error; std::filesystem::remove(path, error); }
    } removeFixture{path};
    // Adapter proxies load through global Shapes, independently of the parent bank.
    ShapeBank bank;
    bank.OptimizeAll();
    Ref<LODShapeWithShadow> parent;
    {
        ShapeBank::WorldModelScope world(bank);
        ShapeBank::CpuOnlyLoadScope logical;
        auto model = BoxModel();
        auto proxyPath = path;
        const std::string proxyName = proxyPath.replace_extension().generic_string();
        model->lodLevels[0].mesh.proxies.emplace_back(proxyName + ".001");
        model->lodLevels[0].mesh.proxies.emplace_back(proxyName + ".002");
        // Reuse the fixture's source mesh in an explicit ODOL IR: only ODOL
        // carries proxy records and takes the split preparation/admission tail.
        model->sourceFormat = "ODOL";
        model->sourcePath = "logical_parent.p3d";
        model->geometryIdx = 1;
        model->mass = 1200.0f;
        model->invMass = 1.0f / model->mass;
        model->massArray.assign(8, 150.0f);
        model->boundingBox.min = {0, 0, 0};
        model->boundingBox.max = {2, 3, 4};
        model->boundingCenter = {1, 1.5f, 2};
        model->boundingSphere.radius = 2.7f;
        model->geometrySphere.radius = 2.7f;
        // NewFromModel must resolve actual proxies through the deferred ODOL
        // tail, including the global Shapes cache and real child file load.
        auto* prepared = Model::ShapeAdapter::convertToLODShape(*model, false, nullptr, false);
        REQUIRE(prepared->Level(0)->NProxies() == 0);
        parent = bank.NewFromModel("logical_parent.p3d", false, true, model, nullptr, prepared);
        REQUIRE(parent->Level(0)->NProxies() == 2);
        auto* child = parent->Level(0)->Proxy(0).obj->GetShape();
        REQUIRE(child != nullptr);
        REQUIRE(child == parent->Level(0)->Proxy(1).obj->GetShape());
        REQUIRE(child->DeferredVisualRendering());
        RequireCollisionGeometry(*child);
        REQUIRE(engine.worldLoads.empty());
        REQUIRE(engine.vertexBufferCalls == 0);
    }
    auto* child = parent->Level(0)->Proxy(0).obj->GetShape();
    SECTION("parent visual demand promotes the shared child") {}
    SECTION("independent child demand preserves the inherited world intent")
    {
        bank.EnsureVisualRendering(child);
        REQUIRE_FALSE(child->DeferredVisualRendering());
        REQUIRE(parent->DeferredVisualRendering());
        REQUIRE(engine.worldLoads == std::vector<LODShapeWithShadow*>{child});
    }
    bank.EnsureVisualRendering(parent);
    REQUIRE_FALSE(parent->DeferredVisualRendering());
    REQUIRE_FALSE(child->DeferredVisualRendering());
    REQUIRE(engine.worldLoads.size() == 2);
    REQUIRE(std::count(engine.worldLoads.begin(), engine.worldLoads.end(), child) == 1);
    REQUIRE(std::count(engine.worldLoads.begin(), engine.worldLoads.end(), parent.GetRef()) == 1);
    const int calls = engine.vertexBufferCalls;
    bank.EnsureVisualRendering(parent);
    REQUIRE(engine.worldLoads.size() == 2);
    REQUIRE(engine.vertexBufferCalls == calls);
}

TEST_CASE("Logical destruction preserves shared visual buffers until object promotion",
          "[ShapeBank][streaming-logical-shape][destruction]")
{
    LogicalLoadEngine engine;
    engine.returnBuffers = true;
    RenderingGuard guard(&engine);
    ShapeBank bank;
    bank.OptimizeAll();
    Ref<LODShapeWithShadow> shape = bank.NewFromModel("shared_destroyed.p3d", false, true, BoxModel(), nullptr);
    REQUIRE(engine.vertexBufferCalls == 1);
    VertexBuffer* sharedBuffer = shape->Level(0)->GetVertexBuffer();
    Ref<ObjectPlain> object = new ObjectPlain(shape, -1);
    object->SetType(Network); // This fixture has no terrain operational cache.
    object->SetDestructType(DestructBuilding);
    object->SetVisualResident(false);
    object->SetDestroyed(0.5f);
    REQUIRE(object->IsDestroyed());
    REQUIRE(object->GetDestroyed() == Catch::Approx(127.0f / 255));
    REQUIRE_FALSE(shape->DeferredVisualRendering());
    REQUIRE(shape->Level(0)->GetVertexBuffer() == sharedBuffer);
    REQUIRE(engine.vertexBufferCalls == 1);
    RequireCollisionGeometry(*shape);
    const ::Vector3 originalCorner = shape->GeometryLevel()->Pos(4);
    object->AnimateGeometry();
    REQUIRE(shape->GeometryLevel()->Pos(4).Distance2(originalCorner) > 0);
    object->DeanimateGeometry();
    REQUIRE(engine.vertexBufferCalls == 1);
    object->PrepareDestroyedVisualRendering();
    REQUIRE(engine.vertexBufferCalls == 1);

    object->SetVisualResident(true);
    object->PrepareDestroyedVisualRendering();
    REQUIRE(engine.vertexBufferCalls == 2);
    REQUIRE(engine.bufferTypes.back() == VBDynamic);
    REQUIRE(engine.liveBuffers == 1);
    object->SetDestroyed(0.75f);
    object->PrepareDestroyedVisualRendering();
    REQUIRE(engine.vertexBufferCalls == 2);
}

TEST_CASE("Cold logical destruction preserves CPU state and completes dynamic buffers on admission",
          "[ShapeBank][streaming-logical-shape][destruction]")
{
    LogicalLoadEngine engine;
    engine.returnBuffers = true;
    RenderingGuard guard(&engine);
    ShapeBank bank;
    bank.OptimizeAll();
    Ref<LODShapeWithShadow> shape;
    {
        ShapeBank::CpuOnlyLoadScope logical;
        shape = bank.NewFromModel("cold_destroyed.p3d", false, true, BoxModel(), nullptr);
    }
    Ref<ObjectPlain> object = new ObjectPlain(shape, -1);
    object->SetType(Network);
    object->SetDestructType(DestructBuilding);
    object->SetVisualResident(false);
    object->SetDestroyed(0.5f);
    REQUIRE(object->IsDestroyed());
    REQUIRE(shape->DeferredVisualRendering());
    REQUIRE(engine.vertexBufferCalls == 0);
    RequireCollisionGeometry(*shape);
    const ::Vector3 originalCorner = shape->GeometryLevel()->Pos(4);
    object->AnimateGeometry();
    REQUIRE(shape->GeometryLevel()->Pos(4).Distance2(originalCorner) > 0);
    object->DeanimateGeometry();
    REQUIRE(engine.vertexBufferCalls == 0);
    REQUIRE(shape->DeferredVisualRendering());
    bank.EnsureVisualRendering(shape);
    object->SetVisualResident(true);
    object->PrepareDestroyedVisualRendering();
    REQUIRE_FALSE(shape->DeferredVisualRendering());
    REQUIRE(engine.vertexBufferCalls == 2);
    REQUIRE(engine.bufferTypes.back() == VBDynamic);
    REQUIRE(engine.liveBuffers == 1);
    object->PrepareDestroyedVisualRendering();
    REQUIRE(engine.vertexBufferCalls == 2);
}

TEST_CASE("Destroyed visual buffers retry partial allocation without changing CPU destruction",
          "[ShapeBank][streaming-logical-shape][destruction]")
{
    LogicalLoadEngine engine;
    engine.returnBuffers = true;
    RenderingGuard guard(&engine);
    ShapeBank bank;
    bank.OptimizeAll();
    auto model = BoxModel();
    model->lodLevels.push_back(model->lodLevels.front());
    model->lodLevels.back().resolution = 2.0f;
    Ref<LODShapeWithShadow> shape = bank.NewFromModel("destroyed_retry.p3d", false, true, model, nullptr);
    REQUIRE(engine.vertexBufferCalls == 2);
    Ref<ObjectPlain> object = new ObjectPlain(shape, -1);
    object->SetType(Network);
    object->SetDestructType(DestructBuilding);
    engine.throwOnBufferCall = 4;
    REQUIRE_THROWS_AS(object->SetDestroyed(0.5f), std::runtime_error);
    REQUIRE(object->IsDestroyed());
    REQUIRE(object->GetDestroyed() == Catch::Approx(127.0f / 255));
    REQUIRE(engine.liveBuffers == 0);
    RequireCollisionGeometry(*shape);
    engine.throwOnBufferCall = 0;
    object->PrepareDestroyedVisualRendering();
    REQUIRE(engine.vertexBufferCalls == 6);
    REQUIRE(engine.liveBuffers == 2);
    REQUIRE(engine.bufferTypes[4] == VBDynamic);
    REQUIRE(engine.bufferTypes[5] == VBDynamic);
    object->PrepareDestroyedVisualRendering();
    REQUIRE(engine.vertexBufferCalls == 6);
}

TEST_CASE("An intact object admission does not rebuild an already visual shape",
          "[ShapeBank][streaming-logical-shape][destruction]")
{
    LogicalLoadEngine engine;
    RenderingGuard guard(&engine);
    ShapeBank bank;
    bank.OptimizeAll();
    Ref<LODShapeWithShadow> shape = bank.NewFromModel("intact_shared.p3d", false, true, BoxModel(), nullptr);
    REQUIRE(engine.vertexBufferCalls == 1);
    Ref<ObjectPlain> object = new ObjectPlain(shape, -1);
    object->SetType(Network);
    object->SetDestructType(DestructBuilding);
    object->SetVisualResident(false);
    object->SetVisualResident(true);
    bank.EnsureVisualRendering(shape);
    object->PrepareDestroyedVisualRendering();
    REQUIRE(engine.vertexBufferCalls == 1);
}
