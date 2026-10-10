// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <vector>

namespace Poseidon
{
// MUD-001: mud thrown this frame, for the renderer's particle mud (wgpu mud/mod.rs). Whatever
// throws mud -- a wheel turning in wet soft ground, a boot stepping in it, the dev panel --
// reports a spray with a (possibly fractional) number of blobs; the counts add up over the
// simulation steps of a frame and the renderer takes them once per frame.
struct MudSprayReport
{
    Vector3 pos;              // where (world)
    Vector3 vel;              // launch velocity (m/s)
    float count = 0;          // blobs
    float velSpread = 0.5f;   // m/s
    float size = 0.03f;       // drawn blob radius (m)
    float posSpread = 0.05f;  // m
};

void ReportMudSpray(const MudSprayReport& spray);
// This frame's sprays, whole counts only (the fractions carry to the next frame); clears them.
void TakeMudSprays(std::vector<MudSprayReport>& out);
// WGR_MUD=0 (the renderer's switch) turns the reports off too.
bool MudSprayEnabled();
// A mud fountain 6 m in front of the camera, every frame while on: the Particles tab's toggle, or
// POSEIDON_MUD_FOUNTAIN=1 from the start (for captures). Reported by the renderer's frame setup.
bool& MudFountain();
} // namespace Poseidon
