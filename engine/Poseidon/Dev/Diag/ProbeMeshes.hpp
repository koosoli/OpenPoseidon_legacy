#pragma once

// Solid meshes for the physics probes.
//
// The wireframe view shows where a body is; a solid one shows what it looks like
// resting against something, which is the thing you actually want to judge when a
// domino row half-falls or a crate lands on a slope.
//
// Every mesh is built at UNIT SIZE once and scaled per instance through the
// object's orientation matrix -- an Object has no scale of its own, but the
// orientation is three column vectors and scaling them scales the model. So one
// box mesh serves every box, whatever its dimensions.

class LODShapeWithShadow;

namespace Poseidon::Dev
{

/// Cube from -1 to +1 on every axis. Faces carry their own vertices so the
/// shading stays flat rather than smoothing across an edge.
LODShapeWithShadow* ProbeBoxMesh();
/// Radius 1, centred on the origin.
LODShapeWithShadow* ProbeSphereMesh();
/// Radius 1, from y = -1 to y = +1, with caps.
LODShapeWithShadow* ProbeCylinderMesh();

/// PROBE-LIGHT: draw the probe meshes UNLIT at full colour (`IsLight`, which
/// BuildRenderPassDescriptor routes to LightingMode::Unlit) instead of shaded by the
/// scene. Only meaningful for a probe that is carrying its own light: otherwise the
/// probe should be lit like everything around it, which is half of what a physics
/// check is judging. Applies to every already-built mesh, so it takes effect on the
/// next frame with no respawn.
void SetProbeMeshSelfIllum(bool unlit);
void ReleaseProbeMeshes();

} // namespace Poseidon::Dev
