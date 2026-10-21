// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "include/wgpu_renderer.hpp"
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <array>
#include <cstdint>
#include <random>
#include <vector>

namespace Poseidon
{
class EngineWgpu;

// Seabirds over the coast and the sea (stage 2 of the Tidewater wildlife port; see BirdFlocks.cpp):
// gulls, terns, pelicans and frigatebirds flying around the camera, and (Shorebirds.js) flocks of
// sanderlings working the swash on the beaches near it. Visual only: simulated on the CPU near the
// camera, never networked, never touches gameplay.
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
    // Shorebirds.js: a sanderling on the ground, or flying with its flock
    struct Sand
    {
        int flock = 0;
        bool flying = false;
        float x = 0, y = 0, z = 0, yaw = 0, vx = 0, vz = 0, v = 0;
        float offset = 0, margin = 0, phase = 0, peckT = 0, head = 0, stop = 0, hop = 0, t = 0;
        float lx = 0, lz = 0;  // its landing spot after a flush
        float form[3] = {};    // its place in the flying flock
        Flyer f;               // the flight state while flying
    };
    // a flock on one stretch of beach: its centre, the along-shore axis there, where it is along it
    struct ShoreFlock
    {
        float cx = 0, cz = 0, tx = 1, tz = 0; // centre and the along-shore unit vector
        float s = 0, drift = 1, range = 60;   // the flock's place along the axis (m from the centre)
        float alarm = 0, away = 1;
        bool wary = false, flying = false;
        // the flush: a flight path out over the surf and back to land further along
        std::vector<std::array<float, 3>> path;
        float pathS = 0, pathLen = 0, px = 0, py = 0, pz = 0, hx = 0, hz = 1;
    };
    void UpdateShorebirds(const Vector3& camera, float dt, float time);
    void SpawnShoreFlock(const Vector3& camera);
    bool SwashAt(float x, float z, float& runup, float& speed) const;
    void GroundSand(Sand& b, ShoreFlock& F, float dt);
    void FlySand(Sand& b, ShoreFlock& F, float dt);
    void FlushFlock(int fi, const Vector3& camera);
    void DrawSand(const Vector3& camera, int& drawn);
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
    // sanderlings: ground poses (8 running phases, standing, probing) after the flight meshes
    std::vector<WgrMesh> _groundMeshes;
    std::vector<uint32_t> _groundCounts;
    std::vector<ShoreFlock> _flocks;
    std::vector<Sand> _sand;
    std::vector<float> _swash;      // the swash field (wgr_water_swash_field), 4 floats per texel
    float _swashLanes[4] = {};
    int _swashTexels = 0;
    float _shoreScan = 0;
    std::mt19937 _rng{20261011u};
    float _lastTime = -1;
    float _respawn = 0;
    float _nearSea = 0; // the camera's distance to the nearest sea, sampled now and then
};
} // namespace Poseidon
