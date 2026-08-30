#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>

using namespace Poseidon;

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
