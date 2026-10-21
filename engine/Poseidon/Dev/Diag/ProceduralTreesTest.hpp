#pragma once

namespace Poseidon::Dev
{
// Owner's test (2026-10-13): OFP's trees near the camera swapped for procedurally generated ones
// (a port of dgreenheck/ez-tree, engine/WgpuRenderer/ProceduralTrees.cpp). Far trees keep their
// original models. Off by default; the Map editor tab switches it; WGR_EZ_TREES=1 at start-up.
struct ProceduralTreesTest
{
    bool enabled = false;
    float range = 120.0f; //!< metres around the camera where the trees are swapped
    int maxTrees = 160;   //!< the nearest this many are swapped
};
ProceduralTreesTest& ProceduralTrees();
} // namespace Poseidon::Dev
