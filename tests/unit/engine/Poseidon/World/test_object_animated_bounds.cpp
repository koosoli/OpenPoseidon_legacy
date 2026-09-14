#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/Graphics/Shadow/ShadowCasterLod.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>

using namespace Poseidon;

TEST_CASE("Interior draw LODs require an exterior shadow caster", "[World][shadow-caster-lod]")
{
    Ref<LODShapeWithShadow> shape = new LODShapeWithShadow();
    shape->AddShape(new Shape(), 0.5f);
    shape->AddShape(new Shape(), 10.0f);
    shape->AddShape(new Shape(), VIEW_GUNNER);
    shape->AddShape(new Shape(), VIEW_PILOT);
    shape->AddShape(new Shape(), VIEW_CARGO);
    CHECK_FALSE(shadow::NeedsExteriorCasterLod(*shape, -1));
    CHECK_FALSE(shadow::NeedsExteriorCasterLod(*shape, 0));
    CHECK_FALSE(shadow::NeedsExteriorCasterLod(*shape, 1));
    CHECK(shadow::NeedsExteriorCasterLod(*shape, 2));
    CHECK(shadow::NeedsExteriorCasterLod(*shape, 3));
    CHECK(shadow::NeedsExteriorCasterLod(*shape, 4));
    CHECK_FALSE(shadow::NeedsExteriorCasterLod(*shape, 5));
}

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

class CountingObject : public ObjectPlain
{
  public:
    CountingObject(LODShapeWithShadow* shape, int id) : ObjectPlain(shape, id) {}

    // ObjectPlain uses the class-keyed fixed-slot fast allocator
    // (DEFINE_FAST_ALLOCATOR(ObjectPlain), slot = sizeof(ObjectPlain)); this subclass is
    // LARGER, and inheriting that operator new writes _animateCalls past the slot --
    // freelist corruption that SEGVed the NEXT test to allocate an ObjectPlain (found
    // the hard way: suite crashed in test_landscape_object_ids). Same rule the header
    // states for ObjectColored subclasses. Plain heap for the test double.
    static void* operator new(size_t size) { return ::operator new(size); }
    static void operator delete(void* p) { ::operator delete(p); }

    void Animate(int level) override
    {
        ++_animateCalls;
        ObjectPlain::Animate(level);
    }

    int AnimateCalls() const { return _animateCalls; }

    // _isDestroyed / _destroyPhase are protected and normally set by the damage system
    // (Dammage.cpp SetDestroyPhase); the test double reaches them directly.
    void MarkDestroyed()
    {
        _isDestroyed = true;
        _destroyPhase = 255;
    }

  private:
    int _animateCalls = 0;
};

void SetBounds(Shape& shape, Vector3Val min, Vector3Val max)
{
    shape.SetMinMax(min, max, (min + max) * 0.5f, (max - min).Size() * 0.5f);
    shape.StoreOriginalMinMax();
    // OpenPoseidon's IsAnimated additionally requires a non-empty face table (an empty
    // level is never animated), and the cacheable test rides on IsAnimated -- so the
    // synthetic level needs at least one (degenerate) face for the cache to be legal.
    Poly face;
    face.Init();
    face.SetN(0);
    shape.AddFace(face);
}
} // namespace

TEST_CASE("Object caches undamaged land-clipped bounds", "[World][Object][Bounds]")
{
    Landscape landscape(nullptr, nullptr);
    GlobalLandscapeScope globalLandscape(&landscape);

    Ref<LODShapeWithShadow> lod = new LODShapeWithShadow();
    Shape* level = new Shape();
    level->SetHints(ClipLandOn, ClipLandOn);
    SetBounds(*level, Vector3(1, 2, 3), Vector3(4, 5, 6));
    lod->AddShape(level, 0.0f);

    Ref<CountingObject> object = new CountingObject(lod, 1);
    Vector3 bounds[2];
    object->AnimatedMinMax(0, bounds);
    REQUIRE(bounds[0] == Vector3(1, 2, 3));
    REQUIRE(bounds[1] == Vector3(4, 5, 6));
    REQUIRE(object->AnimateCalls() == 1);

    object->AnimatedMinMax(0, bounds);
    REQUIRE(object->AnimateCalls() == 1);

    object->SetDammage(0.5f);
    object->AnimatedMinMax(0, bounds);
    REQUIRE(object->AnimateCalls() == 2);
}

// NOT tested here: Move() invalidation. Object::Move routes through
// GLandscape->MoveObject, which walks the object grid a bare test Landscape does not
// have (it SIGSEGVs) -- the invalidation itself is two assignments at the top of both
// Move overloads (Object.cpp) and is exercised by every streamed world. A unit test for
// it needs the landscape test scaffolding, not a bigger bare-object test.

TEST_CASE("Destroyed objects report shared shape deformation", "[World][Object][Instancing]")
{
    // The instanced-run batcher in SceneDraw asks this before it makes an object the head
    // of a run, or admits one to a run: Static() alone is true for a wreck, and a wreck
    // deforms the shared Shape that every other copy of the model draws from.
    Ref<LODShapeWithShadow> lod = new LODShapeWithShadow();
    Shape* level = new Shape();
    lod->AddShape(level, 0.0f);

    Ref<CountingObject> object = new CountingObject(lod, 1);
    // An empty level is never deformed, whatever its damage state.
    CHECK_FALSE(object->DeformsSharedShape(0));

    Poly face;
    face.Init();
    face.SetN(0);
    level->AddFace(face);
    // Intact: batchable.
    CHECK_FALSE(object->DeformsSharedShape(0));

    object->SetDestructType(DestructBuilding);
    object->MarkDestroyed();
    CHECK(object->DeformsSharedShape(0));

    // DestructTree swaps the shape instead of deforming it, so a felled tree stays batchable.
    object->SetDestructType(DestructTree);
    CHECK_FALSE(object->DeformsSharedShape(0));
}

TEST_CASE("AnimatedBSphere answers from the warm bounds cache", "[World][Object][Bounds]")
{
    // UP-9a298d09: a cache-warm query must not touch Animate/Deanimate, whether
    // standalone or nested inside an outer Animate block.
    Landscape landscape(nullptr, nullptr);
    GlobalLandscapeScope globalLandscape(&landscape);

    Ref<LODShapeWithShadow> lod = new LODShapeWithShadow();
    Shape* level = new Shape();
    level->SetHints(ClipLandOn, ClipLandOn);
    SetBounds(*level, Vector3(1, 2, 3), Vector3(4, 5, 6));
    lod->AddShape(level, 0.0f);

    Ref<CountingObject> object = new CountingObject(lod, 1);
    Vector3 bounds[2];
    object->AnimatedMinMax(0, bounds);
    REQUIRE(object->AnimateCalls() == 1);

    Vector3 center;
    float radius = 0.0f;
    object->AnimatedBSphere(0, center, radius, false);
    REQUIRE(object->AnimateCalls() == 1);
    REQUIRE(center == Vector3(2.5f, 3.5f, 4.5f));
    REQUIRE(radius == Vector3(3, 3, 3).Size() * 0.5f);

    object->AnimatedBSphere(0, center, radius, true);
    REQUIRE(object->AnimateCalls() == 1);
    REQUIRE(center == Vector3(2.5f, 3.5f, 4.5f));
}

TEST_CASE("Animated bounds do not nest object animation", "[World][Object][Bounds]")
{
    // Upstream 9a298d09 regression shape: a non-cacheable (destroyed) object
    // queried from inside an Animate block must not start its own animation.
    Landscape landscape(nullptr, nullptr);
    GlobalLandscapeScope globalLandscape(&landscape);

    Ref<LODShapeWithShadow> lod = new LODShapeWithShadow();
    Shape* level = new Shape();
    level->SetHints(ClipLandOn, ClipLandOn);
    SetBounds(*level, Vector3(1, 2, 3), Vector3(4, 5, 6));
    lod->AddShape(level, 0.0f);

    Ref<CountingObject> object = new CountingObject(lod, 1);
    object->SetDestructType(DestructBuilding);
    object->MarkDestroyed();

    Vector3 center;
    float radius = 0.0f;
    object->AnimatedBSphere(0, center, radius, true);

    CHECK(object->AnimateCalls() == 0);
}

TEST_CASE("Object::RenderId is stable, unique and nonzero", "[World][Object][RenderId]")
{
    // ID-1 (render-snapshot design section 4): the render identity lives on the Object
    // and dies with it. Assigned lazily, stable across calls, distinct between objects.
    Ref<ObjectPlain> a = new ObjectPlain(nullptr, 100);
    Ref<ObjectPlain> b = new ObjectPlain(nullptr, 101);

    const uint32_t idA = a->RenderId();
    REQUIRE(idA != 0);
    REQUIRE(a->RenderId() == idA);

    const uint32_t idB = b->RenderId();
    REQUIRE(idB != 0);
    REQUIRE(idB != idA);
}
