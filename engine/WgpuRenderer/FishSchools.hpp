// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "include/wgpu_renderer.hpp"
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <cstdint>
#include <random>
#include <vector>

namespace Poseidon
{
class EngineWgpu;

// Reef and bay fish around the camera (stage 1 of the Tidewater wildlife port; see FishSchools.cpp).
// Visual only: simulated on the CPU near the camera, never networked, never touches gameplay.
class FishSchools
{
  public:
    FishSchools(EngineWgpu& engine, WgrRenderer* renderer) : _engine(engine), _renderer(renderer) {}
    ~FishSchools();
    // Once per drawn frame: simulate the schools around `camera` and submit the fish.
    void Update(const Vector3& camera, float time);

  private:
    struct Fish
    {
        Vector3 pos, vel;
        float length = 0.2f, phase = 0, wander = 0;
    };
    struct Group
    {
        int behaviour = 0;
        Vector3 home;
        float wanderAngle = 0, homeY = 0;
        float surface = 0; // LAKE-FISH-001: water level the group lives under
        std::vector<Fish> fish;
    };
    void EnsureMeshes();
    bool SpawnGroup(const Vector3& camera);
    void Step(Group& group, const Vector3& camera, float dt);

    EngineWgpu& _engine;
    WgrRenderer* _renderer;
    bool _built = false;
    // [species][lod][phase]
    std::vector<WgrMesh> _meshes;
    std::vector<uint32_t> _indexCounts;
    std::vector<WgrTexture> _textures;
    std::vector<Group> _groups;
    std::mt19937 _rng{20261010u};
    float _lastTime = -1;
    float _spawnCooldown = 0;
};
} // namespace Poseidon
