#pragma once

namespace Poseidon
{
class LODShapeWithShadow;
}
namespace Poseidon::Model
{
struct Model;
}
namespace Poseidon::Dev
{
struct TrenchMeshParams
{
    float width = 1.5f;
    float length = 12.0f;
    float depth = 1.6f;
    float rampLength = 4.8f;
    float wallThickness = 0.4f;
};

/// Open top. Clear x=-width/2..width/2; z=0 is an open ground-level entrance.
/// Roadway descends from y=0 to y=-depth by z=rampLength, then stays flat to
/// z=length. The back end is closed. Dimensions are metres, not object scaling.
/// Side/back earth banks rise 0.3 m above the y=0 ground rim to cover a small seam.
/// Requires rampLength>=3*depth; tank authoring should select >=6*depth.
/// A ramp exactly as long as the trench is valid and has no flat parking bed.
bool ValidTrenchMeshParams(const TrenchMeshParams& params);
TrenchMeshParams TankTrenchMeshParams();

/// CPU-only IR with unique source identity, convex slabs and a full unbounded
/// terrain_hole footprint. Invalid input returns an empty Model. No roof,
/// ceiling marker or AI Paths. No terrain placement/clipping is performed here.
Model::Model BuildTrenchModel(const TrenchMeshParams& params);

/// Main-thread bank/adapter access. Caller owns the returned shape, normally in
/// Ref<LODShapeWithShadow>. Invalid input/non-owner thread returns nullptr.
/// Editor must preserve authored elevation, align y=0 to the local ground rim,
/// and verify actual soldier/tank traversal separately from CPU geometry tests.
LODShapeWithShadow* BuildTrenchMesh(const TrenchMeshParams& params);
} // namespace Poseidon::Dev
