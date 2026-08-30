#pragma once

// Per-frame local-light gauges (roadmap Phase 3: "active-light count and lighting
// shader cost"). Written once per frame by EngineWgpu's light selection, read by the
// dev panel and the --capture-metrics sidecar. The COST half of the roadmap bullet is
// answered by A/B, not by a counter: WGR_MAX_ACTIVE_LIGHTS=0 removes every local light
// from the flat loop, and the GPU frame delta on a night scene is the number the
// Forward+ gate (B.1) reads. Storage follows the StreamingDiag pattern -- Poseidon::Dev
// side of the boundary, so no new link edge and no Engine vtable slot.

#include <cstdint>

namespace Poseidon::Dev
{

struct LightCounters
{
    // Last frame's values; single-writer (render prep, main thread), plain fields.
    uint32_t candidates = 0; // world lights considered this frame
    uint32_t selected = 0;   // survivors handed to the shader (<= WGR_MAX_LIGHTS and the env clamp)
    uint32_t cap = 0;        // the effective cap that clipped them (for the capture's context)
};

LightCounters& GLightCounters();

} // namespace Poseidon::Dev
