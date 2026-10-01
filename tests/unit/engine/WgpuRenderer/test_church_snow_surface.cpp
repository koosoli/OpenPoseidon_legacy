#include "../../../../engine/WgpuRenderer/ChurchSnowSurface.hpp"
#include <cassert>
#include <limits>

using namespace Poseidon;

int main()
{
    std::array<ObjectSnowSurface::PosePair, 8> vertices{};
    for (int i = 0; i < 8; ++i)
        vertices[i] = {{float(i),1,0,0,1,0}, {float(i),1,0,0,1,0}};
    const auto mask = ChurchSnowSurface::BuildMask(8,
        [](int selection) { return selection == 0 ? 2 : 0; },
        [](int, int member) { return member + 3; });
    const auto read = [&](int vertex) { return vertices[vertex]; };
    assert(mask.valid && mask.animated[3] && mask.animated[4]);
    assert(!ChurchSnowSurface::SectionDisjoint(mask, 0, 8));
    // Actual roof corners interleave with clock indices. A broad span is wrong.
    const std::array<int, 4> roof{0, 2, 5, 7};
    const auto index = [&](int corner) { return roof[corner]; };
    assert(ChurchSnowSurface::FixedCornersRigid(mask, 4, true, index, read));
    // Clock is denied even at its original neutral position.
    assert(!ChurchSnowSurface::CornersDisjoint(mask, 1, [](int) { return 3; }));
    vertices[3].current[0] = 100;
    vertices[4].current[4] = 0;
    assert(ChurchSnowSurface::FixedVerticesRigid(mask, true, read));
    assert(ChurchSnowSurface::FixedCornersRigid(mask, 4, true, index, read));
    vertices[5].current[0] += 0.01f;
    assert(!ChurchSnowSurface::FixedVerticesRigid(mask, true, read));
    assert(!ChurchSnowSurface::FixedCornersRigid(mask, 4, true, index, read));
    vertices[5].current = vertices[5].original;
    vertices[5].current[4] = 0.99f;
    assert(!ChurchSnowSurface::FixedCornersRigid(mask, 4, true, index, read));
    vertices[5].current = vertices[5].original;
    vertices[5].original[1] = std::numeric_limits<float>::infinity();
    assert(!ChurchSnowSurface::FixedCornersRigid(mask, 4, true, index, read));
    vertices[5].original = vertices[5].current;
    assert(!ChurchSnowSurface::FixedVerticesRigid(mask, false, read));
    assert(!ChurchSnowSurface::FixedCornersRigid(mask, 4, false, index, read));
    int reads = 0;
    const auto counted = [&](int) { ++reads; return 0; };
    assert(!ChurchSnowSurface::CornersDisjoint(mask, 0, counted));
    assert(!ChurchSnowSurface::CornersDisjoint(mask, ObjectSnowSurface::MaxProofVertices + 1, counted));
    assert(reads == 0);
    assert(!ChurchSnowSurface::CornersDisjoint(mask, 1, [](int) { return -1; }));
    assert(!ChurchSnowSurface::CornersDisjoint(mask, 1, [](int) { return 8; }));
    assert(!ChurchSnowSurface::BuildMask(8, [](int) { return 1; }, [](int, int) { return 8; }).valid);
    assert(!ChurchSnowSurface::BuildMask(8, [](int) { return -1; }, [](int, int) { return 0; }).valid);
    assert(!ChurchSnowSurface::BuildMask(ObjectSnowSurface::MaxProofVertices + 1,
        [](int) { return 0; }, [](int, int) { return 0; }).valid);
    const auto clockless = ChurchSnowSurface::BuildMask(8,
        [](int) { return 0; }, [](int, int) { return 0; });
    assert(!ChurchSnowSurface::FixedVerticesRigid(clockless, true, read));
    vertices[3].current = vertices[3].original;
    vertices[4].current = vertices[4].original;
    assert(ChurchSnowSurface::FixedVerticesRigid(clockless, true, read));
    const auto allClock = ChurchSnowSurface::BuildMask(8,
        [](int s) { return s == 0 ? 8 : 0; }, [](int, int member) { return member; });
    assert(!ChurchSnowSurface::FixedVerticesRigid(allClock, true, read));
    assert(!ChurchSnowSurface::CornersDisjoint(allClock, 4, index));
}
