#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Poseidon
{
// Mission-local persistent signed geometry. Dry sand compacts; small rims
// displace some loose grains. Camera residency never deletes a footprint.
class SandField
{
public:
    static constexpr int WindowSize = 512;
    static constexpr int ChunkSize = 16;
    static constexpr float CellSize = 0.125f;
    static constexpr float MaxDepth = 0.15f;
    static constexpr float MaxRim = 0.02f;
    static constexpr size_t MaxChunks = 8192;
    bool enabled = true;

    void Reset();
    // Called once by the actual simulation tick, not by a renderer or getter.
    // Pass UniformWettingDensity: actual rain and visible LIQUID override only.
    void Advance(float seconds, float liquidRain);
    float Wetness() const { return _wetness; }
    bool ContactActive() const { return enabled; }
    float ContactDepth() const;
    float ContactRim() const;
    size_t Chunks() const { return _chunks.size(); }
    size_t Rejected() const { return _rejected; }
    uint64_t Revision() const { return _revision; }

    // One oriented boot capsule: toe/heel segment +/-7.5cm, depression radius
    // 20cm, loose rim extending to 34cm. No additive digging on repeated edges.
    bool StampBoot(float x, float z, float directionX, float directionZ,
                   float depth, float rim);
    float HeightOffsetAt(float x, float z) const;
    std::array<float, 2> HeightGradientAt(float x, float z) const;
    // [originX, originZ, .125, .15], then 512^2 signed cell-centre offsets.
    // Positive rims MUST survive upload and sampling; this is not a mud depth.
    // Invalid/disabled snapshots have header.w=0 and zero offsets.
    std::vector<float> Snapshot(float cameraX, float cameraZ) const;

private:
    struct Chunk { std::array<float, ChunkSize * ChunkSize> cells{}; };
    static bool FinitePosition(float x, float z);
    static int FloorChunk(int x);
    static uint64_t Key(int x, int z);
    float CellOffset(int ix, int iz) const;
    std::unordered_map<uint64_t, Chunk> _chunks;
    float _wetness = 0.0f;
    size_t _rejected = 0;
    uint64_t _revision = 0;
};

// Defined once in SandField.cpp; no separate per-module copies.
SandField& GSand();
} // namespace Poseidon
