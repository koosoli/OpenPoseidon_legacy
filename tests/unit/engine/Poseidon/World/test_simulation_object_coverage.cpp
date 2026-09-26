#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Asset/Formats/P3D/MLODLoader.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Terrain/SimulationObjectCoverage.hpp>
#include <Poseidon/World/Terrain/WorldObjectPlacementFrame.hpp>
#include "test_fixtures.hpp"

#include <cstddef>
#include <new>
#include <limits>
#include <thread>

using namespace Poseidon;
using namespace Poseidon::Streaming;

namespace
{
class RevisionShape final : public LODShapeWithShadow
{
public:
    void ExhaustRevisionForTest() { _queryPolicyRevision = std::numeric_limits<uint64_t>::max(); }
};

class CoverageObject final : public ObjectPlain
{
public:
    CoverageObject(LODShapeWithShadow* shape, int id) : ObjectPlain(shape, id)
    {
        SetType(Network);
        SetDestructType(DestructBuilding);
        SetVisualResident(false);
    }
    void SetStaticForTest(bool value) { _static = value; }
    void ReplaceShapeForTest(LODShapeWithShadow* shape) { _shape = shape; }
};

// Deterministic address reuse, with cleanup even when a REQUIRE throws.
class PlacementCoverageObject
{
    alignas(CoverageObject) std::byte _storage[sizeof(CoverageObject)];
    CoverageObject* _object = nullptr;
public:
    ~PlacementCoverageObject() { Reset(); }
    void Reset()
    {
        if (_object)
            _object->~CoverageObject();
        _object = nullptr;
    }
    CoverageObject& Emplace(LODShapeWithShadow* shape, int id)
    {
        Reset();
        _object = ::new (static_cast<void*>(_storage)) CoverageObject(shape, id);
        Matrix4 frame = MIdentity;
        frame.SetPosition(Vector3(120, 30, -80));
        _object->SetTransform(frame);
        return *_object;
    }
};

Ref<LODShapeWithShadow> CoverageShape()
{
    const auto model = Asset::Formats::MLODLoader::load(GET_FIXTURE("mlod/p3dm_geometry_mass.p3d"));
    return Model::ShapeAdapter::convertToLODShape(model, false);
}

Ref<CoverageObject> MakeCoverageObject(LODShapeWithShadow* shape, int id = 42)
{
    Ref<CoverageObject> object = new CoverageObject(shape, id);
    Matrix4 frame = MIdentity;
    frame.SetPosition(Vector3(120, 30, -80));
    object->SetTransform(frame);
    return object;
}
}

TEST_CASE("Instance coverage samples actual repaired frame and actual constructed radius",
          "[streaming-object-coverage]")
{
    Foundation::CaptureMainThread();
    Ref<LODShapeWithShadow> shape = CoverageShape();
    auto object = MakeCoverageObject(shape);
    Matrix4 raw = MIdentity;
    raw.SetDirectionAside(Vector3(2, 1, 0));
    raw.SetDirectionUp(Vector3(0, 3, 1));
    raw.SetDirection(Vector3(1, 0, 4));
    raw.SetPosition(Vector3(120, 30, -80));
    object->SetTransform(RepairWorldObjectPlacementFrame(raw, false, false, false));
    OwnerCoverageObservationScope scope;
    const auto observed = scope.Observe(*object);
    REQUIRE(observed.Status() == InstanceCoverageStatus::Observed);
    REQUIRE(observed.ObjectId() == 42);
    REQUIRE(observed.Envelope().has_value());
    const double radius = 2 * double(object->Scale()) * shape->BoundingSphere();
    const auto& envelope = *observed.Envelope();
    const float position[3] = {object->Position().X(), object->Position().Y(), object->Position().Z()};
    for (size_t axis = 0; axis < 3; ++axis)
    {
        REQUIRE(envelope.min[axis] < position[axis] - radius);
        REQUIRE(envelope.max[axis] > position[axis] + radius);
    }
    REQUIRE(scope.Validate(*object, observed).Status() == InstanceCoverageStatus::Observed);
    REQUIRE_FALSE(object->IsVisualResident()); // Observation did not promote or pin anything.
}

TEST_CASE("Instance coverage refuses stale movement scalar shape identity and mutation evidence",
          "[streaming-object-coverage]")
{
    Foundation::CaptureMainThread();
    Ref<LODShapeWithShadow> shape = CoverageShape();
    auto object = MakeCoverageObject(shape);
    OwnerCoverageObservationScope scope;
    const auto observed = scope.Observe(*object);
    REQUIRE(observed.Status() == InstanceCoverageStatus::Observed);
    SECTION("translation") { object->SetPosition(Vector3(121, 30, -80)); }
    SECTION("scale") { object->SetScale(2); }
    SECTION("frame orientation even when radius is unchanged")
    {
        Matrix4 frame = MIdentity;
        frame.SetDirectionAside(Vector3(-1, 0, 0));
        frame.SetDirection(Vector3(0, 0, -1));
        frame.SetPosition(object->Position());
        object->SetTransform(frame);
    }
    SECTION("shape identity even with equal numeric radius")
    {
        Ref<LODShapeWithShadow> replacement = CoverageShape();
        object->ReplaceShapeForTest(replacement);
    }
    SECTION("shared shape radius recomputed by a config constructor")
    {
        const float previousRadius = shape->BoundingSphere();
        shape->SetAutoCenter(false);
        shape->CalculateBoundingSphere();
        REQUIRE(shape->BoundingSphere() != previousRadius);
    }
    SECTION("damage") { object->SetTotalDammageValue(0.25f); }
    SECTION("CPU destruction phase") { object->SetDestroyed(0.5f); }
    SECTION("ID replacement") { object->SetID(99); }
    const auto validation = scope.Validate(*object, observed);
    REQUIRE(validation.Status() == InstanceCoverageStatus::Unknown);
    REQUIRE_FALSE(validation.Envelope().has_value());
}

TEST_CASE("Instance coverage leases cannot cross owner operation scopes or workers",
          "[streaming-object-coverage]")
{
    Foundation::CaptureMainThread();
    Ref<LODShapeWithShadow> shape = CoverageShape();
    auto object = MakeCoverageObject(shape);
    InstanceCoverageObservation closed;
    {
        OwnerCoverageObservationScope outer;
        closed = outer.Observe(*object);
        {
            OwnerCoverageObservationScope nested;
            REQUIRE_FALSE(outer.Active());
            REQUIRE(outer.Validate(*object, closed).Status() == InstanceCoverageStatus::ExpiredLease);
            REQUIRE(nested.Validate(*object, closed).Status() == InstanceCoverageStatus::ExpiredLease);
        }
        REQUIRE(outer.Validate(*object, closed).Status() == InstanceCoverageStatus::Observed);
        InstanceCoverageStatus workerObservation = InstanceCoverageStatus::Observed;
        InstanceCoverageStatus workerValidation = InstanceCoverageStatus::Observed;
        bool workerActive = true;
        std::thread worker([&]
        {
            OwnerCoverageObservationScope rejected;
            workerActive = rejected.Active();
            workerObservation = rejected.Observe(*object).Status();
            workerValidation = outer.Validate(*object, closed).Status();
        });
        worker.join();
        REQUIRE_FALSE(workerActive);
        REQUIRE(workerObservation == InstanceCoverageStatus::WrongOwner);
        REQUIRE(workerValidation == InstanceCoverageStatus::WrongOwner);
    }
    OwnerCoverageObservationScope later;
    const auto expired = later.Validate(*object, closed);
    REQUIRE(expired.Status() == InstanceCoverageStatus::ExpiredLease);
    REQUIRE_FALSE(expired.Envelope().has_value());
}

TEST_CASE("Instance coverage rejects incomplete dynamic and invalid live objects",
          "[streaming-object-coverage]")
{
    Foundation::CaptureMainThread();
    Ref<LODShapeWithShadow> shape = CoverageShape();
    auto object = MakeCoverageObject(shape);
    OwnerCoverageObservationScope scope;
    SECTION("missing shape")
    {
        object->ReplaceShapeForTest(nullptr);
        REQUIRE(scope.Observe(*object).Status() == InstanceCoverageStatus::Unknown);
    }
    SECTION("dynamic object")
    {
        object->SetStaticForTest(false);
        REQUIRE(scope.Observe(*object).Status() == InstanceCoverageStatus::Unknown);
    }
    SECTION("unsupported vehicle object type")
    {
        object->SetType(TypeVehicle);
        REQUIRE(scope.Observe(*object).Status() == InstanceCoverageStatus::Unknown);
    }
    SECTION("nonfinite frame")
    {
        object->SetPosition(Vector3(std::numeric_limits<float>::quiet_NaN(), 0, 0));
        REQUIRE(scope.Observe(*object).Status() == InstanceCoverageStatus::Invalid);
    }
    SECTION("singular but positive RMS frame")
    {
        Matrix4 frame = MIdentity;
        frame.SetDirectionUp(frame.DirectionAside());
        object->SetTransform(frame);
        REQUIRE(object->Scale() > 0);
        REQUIRE(scope.Observe(*object).Status() == InstanceCoverageStatus::Invalid);
    }
    SECTION("nonfinite damage state")
    {
        object->SetTotalDammageValue(std::numeric_limits<float>::infinity());
        REQUIRE(scope.Observe(*object).Status() == InstanceCoverageStatus::Invalid);
    }
}

TEST_CASE("Landscape coverage observes existing ID cache without loading or owning identities",
          "[streaming-object-coverage]")
{
    Foundation::CaptureMainThread();
    Landscape landscape(nullptr, nullptr);
    Ref<LODShapeWithShadow> shape = CoverageShape();
    auto object = MakeCoverageObject(shape);
    OwnerCoverageObservationScope scope;
    REQUIRE(landscape.ObserveConstructedObjectCoverage(42, scope).Status() == InstanceCoverageStatus::Unknown);
    landscape.AddToIDCache(object);
    const auto observed = landscape.ObserveConstructedObjectCoverage(42, scope);
    REQUIRE(observed.Status() == InstanceCoverageStatus::Observed);
    REQUIRE(landscape.ValidateConstructedObjectCoverage(observed, scope).Status() == InstanceCoverageStatus::Observed);
    REQUIRE(landscape.ObserveConstructedObjectCoverage(-1, scope).Status() == InstanceCoverageStatus::Unknown);
    object.Free(); // The observation and cache must not retain the instance.
    REQUIRE(landscape.ValidateConstructedObjectCoverage(observed, scope).Status() == InstanceCoverageStatus::Unknown);
    auto replacement = MakeCoverageObject(shape);
    replacement->SetPosition(Vector3(125, 30, -80));
    landscape.AddToIDCache(replacement);
    const auto stale = landscape.ValidateConstructedObjectCoverage(observed, scope);
    REQUIRE(stale.Status() == InstanceCoverageStatus::Unknown);
    REQUIRE_FALSE(stale.Envelope().has_value());
    REQUIRE_FALSE(replacement->IsVisualResident());
}

TEST_CASE("Shape query policy revision rejects equal-valued mutations without a global counter",
          "[streaming-object-coverage][streaming-shape-policy]")
{
    Foundation::CaptureMainThread();
    Ref<LODShapeWithShadow> shape = CoverageShape();
    // Establish the exact recalculated scalar before testing an equal recompute.
    shape->CalculateBoundingSphere();
    Ref<LODShapeWithShadow> unrelated = CoverageShape();
    const auto unrelatedRevision = unrelated->QueryPolicyRevision();
    auto object = MakeCoverageObject(shape);
    OwnerCoverageObservationScope scope;
    const auto prior = scope.Observe(*object);
    REQUIRE(prior.Status() == InstanceCoverageStatus::Observed);
    const auto revision = shape->QueryPolicyRevision();
    const auto radius = shape->BoundingSphere();
    const auto center = shape->BoundingCenter();
    REQUIRE(revision != 0);

    SECTION("same autocenter value") { shape->SetAutoCenter(shape->IsAutoCenter()); }
    SECTION("autocenter lock policy") { shape->LockAutoCenter(true); }
    SECTION("same animation value") { shape->AllowAnimation(shape->GetAllowAnimation()); }
    SECTION("animation enabled") { shape->AllowAnimation(!shape->GetAllowAnimation()); }
    SECTION("autocenter round trip")
    {
        shape->SetAutoCenter(!shape->IsAutoCenter());
        shape->SetAutoCenter(!shape->IsAutoCenter());
    }
    SECTION("same radius recompute") { shape->CalculateBoundingSphere(); }
    SECTION("same radius-only recompute") { shape->CalculateBoundingSphereRadius(); }
    SECTION("same clipping hints") { shape->SetHints(shape->GetOrHints(), shape->GetAndHints()); }
    SECTION("hints rescan") { shape->CalculateHints(); }
    SECTION("same LOD classification") { shape->ScanShapes(); }
    SECTION("same LOD resolution") { shape->SetResolution(0, shape->Resolution(0)); }
    SECTION("same geometry level reference") { shape->ChangeShape(0, shape->Level(0)); }
    SECTION("zero translation") { shape->Translate(VZero); }
    SECTION("reload identical optimized shape")
    {
        QOStream output;
        shape->SaveOptimized(output);
        REQUIRE(shape->QueryPolicyRevision() == revision); // Saving is observational.
        QIStream input(output.str(), output.pcount());
        shape->Reload(input, false);
    }
    SECTION("direct optimized loading identical shape")
    {
        QOStream output;
        shape->SaveOptimized(output);
        QIStream input(output.str(), output.pcount());
        REQUIRE(shape->LoadOptimized(input));
    }
    SECTION("assignment preserves destination lineage")
    {
        Ref<LODShape> copy = new LODShape(*shape);
        // Destination must advance, rather than adopting the source's revision.
        static_cast<LODShape&>(*shape) = *copy;
    }
    REQUIRE(shape->QueryPolicyRevision() > revision);
    REQUIRE(shape->BoundingSphere() == radius);
    REQUIRE(shape->BoundingCenter().X() == center.X());
    REQUIRE(shape->BoundingCenter().Y() == center.Y());
    REQUIRE(shape->BoundingCenter().Z() == center.Z());
    REQUIRE(unrelated->QueryPolicyRevision() == unrelatedRevision);
    REQUIRE(scope.Validate(*object, prior).Status() == InstanceCoverageStatus::Unknown);
    REQUIRE(scope.Observe(*object).Status() == InstanceCoverageStatus::Observed);
}

TEST_CASE("Shape revision exhaustion permanently refuses observation rather than wrapping freshness",
          "[streaming-object-coverage][streaming-shape-policy]")
{
    Foundation::CaptureMainThread();
    Ref<LODShapeWithShadow> source = CoverageShape();
    Ref<RevisionShape> shape = new RevisionShape();
    static_cast<LODShape&>(*shape) = *source;
    shape->ExhaustRevisionForTest();
    auto object = MakeCoverageObject(shape);
    OwnerCoverageObservationScope scope;
    const auto prior = scope.Observe(*object);
    REQUIRE(prior.Status() == InstanceCoverageStatus::Observed);
    shape->SetAutoCenter(shape->IsAutoCenter());
    REQUIRE(shape->QueryPolicyRevision() == 0);
    REQUIRE(scope.Validate(*object, prior).Status() == InstanceCoverageStatus::Unknown);
    REQUIRE(scope.Observe(*object).Status() == InstanceCoverageStatus::Unknown);
    shape->AllowAnimation(false);
    static_cast<LODShape&>(*shape) = *source;
    REQUIRE(shape->QueryPolicyRevision() == 0);
    REQUIRE(scope.Observe(*object).Status() == InstanceCoverageStatus::Unknown);
}

TEST_CASE("Copy constructed shapes start their own query policy lineage",
          "[streaming-object-coverage][streaming-shape-policy]")
{
    Ref<LODShapeWithShadow> source = CoverageShape();
    for (int i = 0; i < 20; ++i)
        source->SetAutoCenter(source->IsAutoCenter());
    const auto sourceRevision = source->QueryPolicyRevision();
    Ref<LODShape> copy = new LODShape(*source);
    REQUIRE(copy->QueryPolicyRevision() != 0);
    REQUIRE(copy->QueryPolicyRevision() < sourceRevision);
    REQUIRE(copy->BoundingSphere() == source->BoundingSphere());
    copy->AllowAnimation(copy->GetAllowAnimation());
    REQUIRE(source->QueryPolicyRevision() == sourceRevision);
}

TEST_CASE("Weak observation rejects a replacement with identical address ID shape frame and radius",
          "[streaming-object-coverage][streaming-shape-policy]")
{
    Foundation::CaptureMainThread();
    Ref<LODShapeWithShadow> shape = CoverageShape();
    PlacementCoverageObject slot;
    OwnerCoverageObservationScope scope;
    Object& original = slot.Emplace(shape, 42);
    const uintptr_t address = reinterpret_cast<uintptr_t>(&original);
    const auto prior = scope.Observe(original);
    const auto copied = prior; // Copies must share the original weak lifetime.
    REQUIRE(prior.Status() == InstanceCoverageStatus::Observed);
    REQUIRE(scope.Validate(original, prior).Status() == InstanceCoverageStatus::Observed);
    const auto oldEnvelope = prior.Envelope();
    const auto oldRadius = original.GetRadius();
    const auto shapeRevision = shape->QueryPolicyRevision();
    Object& replacement = slot.Emplace(shape, 42);
    REQUIRE(reinterpret_cast<uintptr_t>(&replacement) == address);
    REQUIRE(replacement.ID() == 42);
    REQUIRE(replacement.GetShape() == shape);
    REQUIRE(replacement.GetRadius() == oldRadius);
    REQUIRE(shape->QueryPolicyRevision() == shapeRevision);
    const auto fresh = scope.Observe(replacement);
    REQUIRE(fresh.Status() == InstanceCoverageStatus::Observed);
    REQUIRE(fresh.Envelope()->min == oldEnvelope->min);
    REQUIRE(fresh.Envelope()->max == oldEnvelope->max);
    REQUIRE(scope.Validate(replacement, prior).Status() == InstanceCoverageStatus::Unknown);
    REQUIRE(scope.Validate(replacement, copied).Status() == InstanceCoverageStatus::Unknown);
    slot.Reset(); // Neither observation nor its copy can retain the replacement.
}
