// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "include/wgpu_renderer.hpp"
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

namespace Poseidon
{
class EngineWgpu;

// Ghost crabs (with their burrows) and hermit crabs on the beaches around the camera (stage 4 of
// the Tidewater wildlife port; see BeachCritters.cpp). Visual only, never touches gameplay.
class BeachCritters
{
  public:
    BeachCritters(EngineWgpu& engine, WgrRenderer* renderer) : _engine(engine), _renderer(renderer) {}
    void Update(const Vector3& camera, float time);

  private:
    struct Home
    {
        float x, y, z, yaw, seed, size;
        bool hermit;
    };
    struct Crab
    {
        Home h{};          // its burrow / home
        bool hermit = false;
        int state = 0;     // ghost: 0 hidden, 1 peek, 2 emerge, 3 forage, 4 pause, 5 flee, 6 enter; hermit: 0 walk, 1 idle, 2 retracted
        float x = 0, z = 0, tx = 0, tz = 0, t = 0, speed = 0, yaw = 0, phase = 0, stride = 0, out = 0, size = 0.04f, seed = 0;
    };
    void EnsureMeshes();
    void Scan(const Vector3& camera);
    void StepGhost(Crab& c, float dt, const Vector3* threats, int nThreats);
    void StepHermit(Crab& c, float dt, const Vector3* threats, int nThreats);
    float Rand() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(_rng); }

    EngineWgpu& _engine;
    WgrRenderer* _renderer;
    bool _built = false;
    // [species 0 ghost / 1 hermit][pose]: 8 gait phases, standing, (hermit) retracted; species 2 burrow: pose 0
    std::vector<WgrMesh> _meshes;
    std::vector<uint32_t> _indexCounts;
    std::vector<WgrTexture> _textures;
    std::unordered_map<int64_t, std::vector<Home>> _cells;
    std::vector<Home> _homes;     // the homes near the camera this scan
    std::vector<Crab> _crabs;
    std::mt19937 _rng{20261012u};
    float _lastTime = -1, _scan = 0;
};
} // namespace Poseidon
