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

// Seabirds over the coast and the sea (stage 2 of the Tidewater wildlife port; see BirdFlocks.cpp):
// gulls, terns, pelicans and frigatebirds flying around the camera. Visual only: simulated on the
// CPU near the camera, never networked, never touches gameplay.
class BirdFlocks
{
  public:
    BirdFlocks(EngineWgpu& engine, WgrRenderer* renderer) : _engine(engine), _renderer(renderer) {}
    // Once per drawn frame: fly the birds around `camera` and submit them.
    void Update(const Vector3& camera, float time);

  private:
    struct Flyer
    {
        int species = 0;
        float x = 0, y = 0, z = 0, vy = 0;
        float yaw = 0, gamma = 0, bank = 0, pitch = 0, speed = 10;
        float phase = 0, flap = 0, flapWant = 0, burst = 0, freqMul = 1, wander = 0, scale = 1;
        bool bursting = false;
        // behaviour: 0 roam (circle a drifting centre), 1 patrol a lane, 2 skim in a line, 3 soar
        int mode = 0;
        float cx = 0, cz = 0, cy = 0, radius = 30, dir = 1, t = 0;
        float lx0 = 0, lz0 = 0, lx1 = 0, lz1 = 0; // patrol lane ends
        int leader = -1, slot = 0;                // skimming line
    };
    void EnsureMeshes();
    void Spawn(const Vector3& camera);
    void Steer(Flyer& f, float dt, float tx, float ty, float tz, float speed, float power, float turnGain,
               float windX, float windZ);
    void Animate(Flyer& f, float dt);
    float Floor(float x, float z) const; // ground or sea surface under (x, z)

    EngineWgpu& _engine;
    WgrRenderer* _renderer;
    bool _built = false;
    // [species][pose]: the flap cycle's phases, then the glide pose
    std::vector<WgrMesh> _meshes;
    std::vector<uint32_t> _indexCounts;
    std::vector<WgrTexture> _textures;
    std::vector<Flyer> _birds;
    std::mt19937 _rng{20261011u};
    float _lastTime = -1;
    float _respawn = 0;
    float _nearSea = 0; // the camera's distance to the nearest sea, sampled now and then
};
} // namespace Poseidon
