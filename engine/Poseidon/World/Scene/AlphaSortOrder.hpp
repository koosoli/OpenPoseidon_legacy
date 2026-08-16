#pragma once

namespace Poseidon::AlphaSort
{

// The alpha pass draws cloudlets (dust/smoke) and objects' blend sections together,
// far-to-near by their camera-space depth (zCoord) so nearer entries composite on top.
// Depth must be the planar camera-space Z, not Euclidean distance, which diverges at
// oblique angles and lets a near wheel sort ahead of the dust over it.

// An object owning blend sections sorts by its far extent (centre depth + radius) so all of
// its blend sections draw before the dust that interpenetrates it, and the dust then hazes
// each section. Cloudlets keep centre depth.
//
// MAT-048 changed the premise this key was chosen under, without changing the key. It was
// originally justified by the blend sections WRITING depth: the object drew first, then dust
// nearer than the glass hazed it while dust behind the glass was depth-rejected. Those
// sections are now depth-test-only (Graphics/Rendering/BuildRenderPassDescriptor.hpp,
// `GBlendSectionDepthReadOnly`) because depth-write was discarding whole cockpit panes, so
// there is no longer a depth test standing behind this ordering: dust drawn after the object
// now composites over its glass whether it is in front of the pane or behind it.
//
// The key is deliberately left as the far extent anyway. Switching it to centre depth would
// be the coherent partner change, but the key is written in World/Scene/Scene.cpp:1000 --
// not this file's problem to move -- it is asserted by
// tests/unit/.../test_alpha_sort_order.cpp, and for objects that do not overlap each other
// the two keys produce the identical order, so the change would buy only the object-vs-dust
// case and would risk every object-vs-object case to get it. The residual is: smoke around a
// vehicle reads slightly in front of its canopy rather than being clipped by it. That is a
// transient, already-translucent effect, traded against a canopy pane that was missing
// permanently. `WGR_COCKPIT_BLEND_LEGACY=1` restores the depth-writing behaviour this key was
// designed for, in the same binary, if the trade needs re-examining.
inline float AlphaObjectDepth(float centreZ, float radius)
{
    return centreZ + radius;
}

// Returns <0 if a is farther than b (a is drawn first).
inline int CompareAlphaDepth(float zCoordA, float zCoordB)
{
    if (zCoordA > zCoordB)
    {
        return -1;
    }
    if (zCoordA < zCoordB)
    {
        return +1;
    }
    return 0;
}

} // namespace Poseidon::AlphaSort
