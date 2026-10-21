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

// Beach debris around the camera: Tidewater's wrack line and shore clutter (shells, pebbles,
// driftwood, seaweed mats; see BeachDebris.cpp). Visual only, never touches gameplay.
class BeachDebris
{
  public:
    BeachDebris(EngineWgpu& engine, WgrRenderer* renderer) : _engine(engine), _renderer(renderer) {}
    void Update(const Vector3& camera);

    enum Kind : uint8_t
    {
        Scallop = 0,
        Whelk,
        Pebble,
        Driftwood,
        Wrack,
        KindCount
    };

  private:
    struct Item
    {
        float x, y, z, yaw, sx, sy, sz;
        uint8_t kind, variant;
    };
    void EnsureMeshes();
    const std::vector<Item>& Cell(int cx, int cz);

    EngineWgpu& _engine;
    WgrRenderer* _renderer;
    bool _built = false;
    // [kind][variant]
    std::vector<WgrMesh> _meshes;
    std::vector<uint32_t> _indexCounts;
    std::vector<WgrTexture> _textures; // [kind][variant]
    std::unordered_map<int64_t, std::vector<Item>> _cells;
    std::vector<std::pair<float, const Item*>> _near;
};
} // namespace Poseidon
