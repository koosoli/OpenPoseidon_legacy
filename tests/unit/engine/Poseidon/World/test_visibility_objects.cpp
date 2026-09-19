#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Scene/Object.hpp>
using namespace Poseidon;
using namespace Poseidon::Foundation;
TEST_CASE("visibility cell index excludes only temporary objects and preserves order", "[visibility-index]")
{
    class Heightfield : public Landscape {
    public:
        Heightfield() : Landscape(nullptr, nullptr) {}
        using Landscape::Dim;
        using Landscape::SetData;
    } landscape;
    struct LandscapeScope { Landscape* old = GLandscape; LandscapeScope(Landscape* l) { GLandscape = l; } ~LandscapeScope() { GLandscape = old; } } scope(&landscape);
    landscape.Dim(16, 16, 32, 32, 50.0f);
    for (int z = 0; z < 32; ++z)
        for (int x = 0; x < 32; ++x) landscape.SetData(x, z, 0);
    ObjectListFull cell(0, 0);
    Ref<Object> first = new Object(static_cast<LODShapeWithShadow*>(nullptr), -1);
    Ref<Object> last = new Object(static_cast<LODShapeWithShadow*>(nullptr), -1);
    first->SetType(TypeVehicle);
    last->SetType(Primary);
    cell.Add(first, true);
    for (int i = 0; i < 20000; ++i)
    {
        Ref<Object> mark = new Object(static_cast<LODShapeWithShadow*>(nullptr), -1);
        mark->SetType(TypeTempVehicle);
        cell.Add(mark, true);
    }
    cell.Add(last, true);
    REQUIRE(cell.Size() == 20002);
    REQUIRE(cell.GetVisibilityObjectCount() == 2);
    CHECK(cell.GetVisibilityObject(0) == first.GetRef());
    CHECK(cell.GetVisibilityObject(1) == last.GetRef());
    cell.Delete(1);
    CHECK(cell.GetVisibilityObjectCount() == 2);
    cell.Delete(0);
    REQUIRE(cell.GetVisibilityObjectCount() == 1);
    CHECK(cell.GetVisibilityObject(0) == last.GetRef());
    cell.Clear();
    CHECK(cell.GetVisibilityObjectCount() == 0);
    CHECK(cell.Size() == 0);
}
