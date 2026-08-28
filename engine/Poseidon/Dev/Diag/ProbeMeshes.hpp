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

} // namespace Poseidon::Dev
