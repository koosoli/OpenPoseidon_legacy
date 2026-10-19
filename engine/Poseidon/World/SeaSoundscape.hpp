#pragma once

// (OP) Tidewater's sea sounds (@OP_TidewaterWildlife, config class CfgTidewaterSounds; port of Tidewater's
// src/audio/SoundScape.js, the parts about the sea): wave-by-wave surf on the shores near the listener, the distant
// surf bed, and one-shots the boat and the whale play. Silent when the pack is not installed. See SeaSoundscape.cpp.

#include <Poseidon/Foundation/Math/Math3D.hpp>

namespace Poseidon
{
class IWave;

// Once a frame from the world's environment sounds (camera position, frame time).
void UpdateSeaSoundscape(Vector3Par listener, float deltaT);

// A one-shot: a random file of the sound `name` (CfgTidewaterSounds), at its mixed level plus `gainDb`.
void PlaySeaSound(const char* name, Vector3Par pos, Vector3Par speed, float gainDb = 0, float freq = 1);

// A loop of `name` (its first file) for a caller that keeps it and moves it: started at volume 0.
IWave* OpenSeaLoop(const char* name, Vector3Par pos);
// The mixed level of `name` (linear), times `gain`.
float SeaSoundVolume(const char* name, float gain = 1);

} // namespace Poseidon
