// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "EarthFeatureStream.hpp"
#include "include/wgpu_renderer.hpp"
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <vector>

namespace Poseidon
{
class EngineWgpu;

// Whole-world trees near the camera: detailed procedural crowns after Tidewater's vegetation
// (see EarthTreeDetail.cpp). The coarse feature-stream batch keeps the far trees; the ones this tier
// draws are collapsed out of it, so each tree is drawn exactly once.
class EarthTreeDetail
{
  public:
    EarthTreeDetail(EngineWgpu& engine, WgrRenderer* renderer) : _engine(engine), _renderer(renderer) {}
    // The trees of the current feature mesh (its frame), replaced whenever a new mesh arrives.
    // `coarse` is the batch mesh drawing them and `coarseVertices` its vertices, EarthTrees::VertexCount
    // per tree in the same order as `trees`.
    void SetTrees(std::vector<EarthFeatureStream::TreeInstance> trees, WgrMesh coarse,
                  std::vector<WgrMeshVertex> coarseVertices);
    void Clear();
    void Draw(const Vector3& camera, float time);

  private:
    void EnsureMeshes();
    void Hide(const Vector3& centre);
    EngineWgpu& _engine;
    WgrRenderer* _renderer;
    bool _built = false;
    // [species variant][lod]
    std::vector<WgrMesh> _meshes;
    std::vector<uint32_t> _indexCounts;
    WgrTexture _broadleaf = 0, _conifer = 0;
    std::vector<EarthFeatureStream::TreeInstance> _trees;
    // the coarse batch, and which of its trees are collapsed (the ones drawn here), chosen around
    // _centre and refreshed when the camera has moved Rehide metres from it
    WgrMesh _coarse = 0;
    std::vector<WgrMeshVertex> _coarseVertices, _upload;
    std::vector<uint8_t> _hidden;
    Vector3 _centre{};
    bool _hasCentre = false;
};
} // namespace Poseidon
