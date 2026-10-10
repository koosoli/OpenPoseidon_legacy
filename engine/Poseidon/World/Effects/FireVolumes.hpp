// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <cstdint>
#include <vector>

namespace Poseidon
{
// FIRE-001: the fires burning this frame, for the renderer's volumetric fire (wgpu
// fire/mod.rs). Whatever burns reports itself every simulation step it burns; a fire not
// reported for half a second of game time is gone (and the renderer fades its box out).
// Timed fires (the dev panel's, explosions') are spawned once and burn out on their own.
enum FireVolumeKind
{
    FireKindBed = 0,       // a fireplace, a campfire: a steady flame
    FireKindWreck = 1,     // a burning vehicle: wide, smoky
    FireKindExplosion = 2, // a burst: hot, fast, short
    FireKindSmoke = 3,     // smoke without flame: a smoke grenade, a smouldering source
};

struct FireVolumeReport
{
    Vector3 base;        // the bed of the fire on the ground (world)
    float radius = 0;    // m
    float intensity = 0; // 0..1
    float smoke = 0;     // how much it smokes, 0..1
    uint32_t key = 0;    // stable per fire
    int kind = 0;        // FireVolumeKind
    float time = 0;      // game time of the report
    // the smoke's colour (linear albedo) and density scale; soot by default
    float color[4] = {0.0185f, 0.0212f, 0.0273f, 1.0f};
    // The solids around the fire, probed once when it starts (and again if it moves): the
    // renderer's box is boxWidth square on a FireSolidColumns x FireSolidColumns grid of
    // columns centred on `base`; each column is open between floor and ceiling (world y). A
    // column the bed cannot see at flame height (behind a wall) has ceiling < floor.
    float boxWidth = 0;
    float floor[100] = {};
    float ceiling[100] = {};
    Vector3 solidsAt{VZero};
    bool solidsValid = false;
};
constexpr int FireSolidColumns = 10;

void ReportFireVolume(const void* owner, Vector3Par base, float radius, float intensity, float smoke, int kind,
                      const float* color = nullptr);
// A fire that burns for `duration` seconds of game time (negative: until extinguished).
// Explosions fade from full strength to nothing over theirs; the others hold and fade at the end.
void SpawnFireVolume(Vector3Par base, float radius, float smoke, float duration, int kind);
void ExtinguishFireVolumes();
int TimedFireVolumeCount();
// The live fires, stale ones dropped, and remembers which are near enough to `camera` to be
// drawn (the renderer draws the nearest MaxDrawnFireVolumes within FireVolumeRange).
void CollectFireVolumes(std::vector<FireVolumeReport>& out, Vector3Par camera);
// Whether the renderer draws this owner's flame: sprite flames step aside for it.
bool FireVolumeDrawn(const void* owner);
// False when the volumetric fire is switched off (WGR_FIRE=0).
bool FireVolumesEnabled();

constexpr int MaxDrawnFireVolumes = 4;
constexpr float FireVolumeRange = 300.0f;
} // namespace Poseidon
