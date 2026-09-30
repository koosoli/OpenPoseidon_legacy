#include "../../../../engine/WgpuRenderer/TreeSnowSurface.hpp"
#include <array>
#include <cassert>
#include <limits>

using namespace Poseidon::TreeSnowSurface;
int main()
{
    const auto bounds = AdmitBounds(-6.0f, 14.0f);
    assert(bounds.minY == -6.0f && bounds.inverseHeight == 0.05f);
    for (auto pair : std::array<std::array<float, 2>, 6>{{
        {0,0}, {4,2}, {0,0.00001f}, {0, std::numeric_limits<float>::infinity()},
        {std::numeric_limits<float>::quiet_NaN(), 2}, {-3.4e38f, 3.4e38f}}})
        assert(AdmitBounds(pair[0], pair[1]).inverseHeight == 0);
    // Every source gate independently rejects a real owner/leaf candidate.
    assert(OwnerAllowed(true,true,true,true,true,true,true));
    for (int rejected = 0; rejected < 7; ++rejected)
    {
        std::array<bool,7> gates{true,true,true,true,true,true,true};
        gates[rejected] = false;
        assert(!OwnerAllowed(gates[0],gates[1],gates[2],gates[3],gates[4],gates[5],gates[6]));
    }
    assert(LeafSectionAllowed(true,true,true,true));
    for (int rejected = 0; rejected < 4; ++rejected)
    {
        std::array<bool,4> gates{true,true,true,true}; gates[rejected] = false;
        assert(!LeafSectionAllowed(gates[0],gates[1],gates[2],gates[3]));
    }
    ProbeBudget budget;
    budget.BeginFrame(0); assert(!budget.Take());
    budget.BeginFrame(1);
    for (int i=0; i<8; ++i) assert(budget.Take());
    assert(!budget.Take());
    budget.BeginFrame(1); assert(!budget.Take()); // cascade/pass cannot refill
    budget.BeginFrame(2); assert(budget.Take()); assert(budget.remaining == 7);
    budget.BeginFrame(2); assert(budget.remaining == 7);
    std::array<float,16> transform{};
    auto moved = transform; moved[12] = 0.000001f;
    int shapeA = 0, shapeB = 0;
    assert(SameOwner(7,7,&shapeA,&shapeA,transform.data(),transform.data()));
    assert(!SameOwner(7,8,&shapeA,&shapeA,transform.data(),transform.data())); // recycled pointer
    assert(!SameOwner(7,7,&shapeA,&shapeB,transform.data(),transform.data())); // swapped model
    assert(!SameOwner(7,7,&shapeA,&shapeA,transform.data(),moved.data()));
    assert(ReuseProof(true,0)); assert(ReuseProof(true,0.999999));
    assert(!ReuseProof(true,1)); assert(!ReuseProof(true,2)); assert(!ReuseProof(false,0));
    assert(!ReuseProof(true,-0.001)); assert(!ReuseProof(true,std::numeric_limits<double>::quiet_NaN()));
    // Production two-geometry transport binds literal ignored owner and ray.
    const std::array<float,3> origin{20,14.15f,30}, end{20,44.15f,30};
    for (auto hits : std::array<std::array<bool,2>,3>{{{false,false},{true,false},{false,true}}})
    {
        int calls = 0;
        const bool open = ProbeOpen(&shapeA,origin,end,
            [&](int* ignore, const auto& begin, const auto& finish, RoofGeometry geometry) {
                assert(ignore == &shapeA); assert(begin == origin); assert(finish == end);
                const bool fire = geometry == RoofGeometry::Fire;
                assert(fire == (calls == 0)); ++calls;
                return hits[fire ? 0 : 1];
            });
        assert(open == (!hits[0] && !hits[1]));
        assert(calls == (hits[0] ? 1 : 2));
    }
    static_assert(MaxCachedOwners == 4096);
}
