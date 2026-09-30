#include "../../../../engine/WgpuRenderer/ObjectSnowSurface.hpp"

#include <cassert>
#include <limits>

using namespace Poseidon::ObjectSnowSurface;

int main()
{
    assert(ConformAllowed(0.0f, false, false));
    assert(ConformAllowed(2.0f, true, false)); // stock Keep-only roof
    assert(!ConformAllowed(2.0f, true, true)); // mixed pinned road remains dry
    assert(!ConformAllowed(2.0f, false, true));
    assert(!ConformAllowed(2.0f, false, false)); // missing source proof
    assert(!ConformAllowed(1.0f, true, false)); // forest plane
    assert(!ConformAllowed(3.0f, true, false));
    assert(!ConformAllowed(std::numeric_limits<float>::quiet_NaN(), true, false));
    std::array<PosePair, 8> mesh{};
    for (int i = 0; i < 8; ++i)
    {
        mesh[i].original = {float(i), 1.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        mesh[i].current = mesh[i].original;
    }
    int reads = 0;
    const auto read = [&](int i) { ++reads; return mesh[i]; };
    // A moving wheel does not disqualify the separately drawn unchanged roof.
    mesh[6].current[0] += 1.0f;
    assert(CurrentSectionRigid(0, 4, 8, true, read));
    assert(!CurrentSectionRigid(4, 8, 8, true, read));
    // Position and normal changes independently defeat the current-pose proof.
    mesh[2].current[1] += 0.000001f;
    assert(!CurrentSectionRigid(0, 4, 8, true, read));
    mesh[2].current = mesh[2].original;
    mesh[2].current[4] = 0.99f;
    assert(!CurrentSectionRigid(0, 4, 8, true, read));
    mesh[2].current = mesh[2].original;
    mesh[2].current[3] = std::numeric_limits<float>::quiet_NaN();
    assert(!CurrentSectionRigid(0, 4, 8, true, read));
    mesh[2].current = mesh[2].original;
    mesh[2].original[1] = std::numeric_limits<float>::infinity();
    assert(!CurrentSectionRigid(0, 4, 8, true, read));
    mesh[2].original = mesh[2].current;

    reads = 0;
    assert(!CurrentSectionRigid(0, 4, 8, false, read));
    assert(!CurrentSectionRigid(-1, 4, 8, true, read));
    assert(!CurrentSectionRigid(4, 4, 8, true, read));
    assert(!CurrentSectionRigid(0, 9, 8, true, read));
    assert(!CurrentSectionRigid(0, MaxProofVertices + 1, MaxProofVertices + 1, true, read));
    assert(reads == 0); // invalid/over-budget spans never dereference source data
    assert(CurrentSectionRigid(0, MaxProofVertices, MaxProofVertices, true,
        [](int) { return PosePair{}; }));

    std::array<VisualPose, 3> visuals{{
        {true, true, false, false, true, 4},
        {true, true, false, false, true, 4},
        {false, false, false, false, false, 0}}}; // memory LOD is not visual
    const auto visual = [&](int i) { return visuals[i]; };
    const auto pose = [&](int level, int vertex) { ++reads; return mesh[level * 4 + vertex]; };
    mesh[6].current = mesh[6].original;
    assert(RetainedVisualsRigid(3, true, false, visual, pose));
    mesh[6].current[4] = 0.99f; // a different visual LOD deforms
    assert(!RetainedVisualsRigid(3, true, false, visual, pose));
    mesh[6].current = mesh[6].original;
    visuals[1].loaded = false;
    assert(!RetainedVisualsRigid(3, true, false, visual, pose));
    visuals[1].loaded = true;
    visuals[1].originalsValid = false;
    assert(!RetainedVisualsRigid(3, true, false, visual, pose));
    visuals[1].originalsValid = true;
    visuals[1].deforms = true;
    assert(!RetainedVisualsRigid(3, false, false, visual, pose));
    visuals[1].deforms = false;
    visuals[1].animated = true;
    assert(!RetainedVisualsRigid(3, false, false, visual, pose));
    visuals[1].animated = false;
    reads = 0;
    assert(!RetainedVisualsRigid(3, true, true, visual, pose)); // neutral configured door
    assert(reads == 0);
    assert(RetainedVisualsRigid(3, false, false, visual, pose));
    assert(reads == 0); // no-animation static source still needs no vertex scan
    const auto zeroPose = [&](int, int) { ++reads; return PosePair{}; };
    visuals[0].vertices = MaxProofVertices;
    visuals[1].vertices = 1;
    assert(!RetainedVisualsRigid(3, true, false, visual, zeroPose));
    assert(reads == MaxProofVertices); // shared budget, second LOD not read
    visuals[0].vertices = 0;
    visuals[1].vertices = 0;
    assert(!RetainedVisualsRigid(3, true, false, visual, zeroPose));

    assert(CoverPossible(true, 0.18f, false, 100.0f, 25.0f, 0.06f, 1.0f));
    assert(!CoverPossible(false, 0.18f, false, 100.0f, 25.0f, 0.06f, 1.0f));
    assert(!CoverPossible(true, 0.0f, false, 100.0f, 25.0f, 0.06f, 200.0f));
    assert(!CoverPossible(false, 0.0f, true, 100.0f, 25.0f, 0.06f, 100.0f));
    assert(CoverPossible(false, 0.0f, true, 100.0f, 25.0f, 0.06f, 100.01f));
    assert(!CoverPossible(false, 0.0f, true, 100.0f, 0.0f, 0.06f, 200.0f));
    assert(!CoverPossible(false, 0.0f, true, 100.0f, 25.0f, 0.0f, 200.0f));
    assert(!CoverPossible(false, 0.0f, true, 100.0f, 25.0f, 0.06f,
        std::numeric_limits<float>::quiet_NaN()));
    assert(!CoverPossible(true, std::numeric_limits<float>::infinity(), false,
        100.0f, 25.0f, 0.06f, 200.0f));
}
