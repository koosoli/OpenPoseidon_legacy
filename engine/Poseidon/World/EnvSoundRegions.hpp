#pragma once

// OFP Fusion: ambient loops per island. A world made of several islands gives each its own EnvSounds (Malden's
// quieter sea and crickets are not Everon's):
//
//     class SoundRegions
//     {
//         class Malden
//         {
//             area[] = {8000, 4800, 20800, 17600};        // xMin, zMin, xMax, zMax (m)
//             class EnvSounds: CfgEnvSounds { ... };      // Sea, Trees, Meadows, Hills, Rain, as a world's
//         };
//         ...
//     };
//
// In an area its loops play. Over the water the nearest area's do, crossfaded with the next nearest within
// kBlend metres either side of the line halfway between them. Without the class: the world's EnvSounds, as before.
// The map's sound cells (which of Sea / Trees / ... where) are the terrain's, unchanged; only what they play is
// per region. "Default" (a mission's environment override) stays the world's.

#include <Poseidon/IO/ParamFileExt.hpp>

namespace Poseidon
{

struct EnvSoundSet
{
    // Sea, Trees, Meadows, Hills, Rain (the indices of EnvSoundPars)
    static constexpr int Count = 5;
    SoundPars day[Count];
    SoundPars night[Count];
};

// The world's SoundRegions (none: cleared).
void InitEnvSoundRegions(const ParamEntry& world);
// The regions heard at x, z and their weights (summing to 1): 0 when the world has none, else 1 or 2.
int EnvSoundRegionsAt(float x, float z, const EnvSoundSet* sets[2], float weights[2]);

} // namespace Poseidon
