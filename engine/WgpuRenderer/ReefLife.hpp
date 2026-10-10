// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "include/wgpu_renderer.hpp"
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Poseidon
{
class EngineWgpu;

// Seagrass, algae, corals, sponges and other life on the sea floor around the camera (stage 3 of
// the Tidewater wildlife port; see ReefLife.cpp). Visual only: placed deterministically per 6 m
// cell from the water depth, so it does not change as you come and go; never touches gameplay.
class ReefLife
{
  public:
    ReefLife(EngineWgpu& engine, WgrRenderer* renderer) : _engine(engine), _renderer(renderer) {}
    void Update(const Vector3& camera, float time);

  private:
    struct Item
    {
        int species, variant;
        float x, y, z, yaw, scale, phase;
    };
    void EnsureMeshes();
    const std::vector<Item>& Cell(int cx, int cz);

    EngineWgpu& _engine;
    WgrRenderer* _renderer;
    bool _built = false;
    // [species][variant][lod][sway phase] (rigid species use phase 0 only)
    std::vector<WgrMesh> _meshes;
    std::vector<uint32_t> _indexCounts;
    std::vector<WgrTexture> _textures; // per species
    std::unordered_map<int64_t, std::vector<Item>> _cells;
};
} // namespace Poseidon
