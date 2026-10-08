// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
struct OpReShadeTimingSampleV1 { double gpuMs, presentMs; std::uint32_t flags, reserved; };
struct OpReShadeTimingV1
{
    std::uint32_t bytes, version, status, frames, recorded, completedGpu, warmup, preview;
    std::uint64_t frequency, generation;
    OpReShadeTimingSampleV1 samples[240];
};
// status: 0=idle, 2=warmup, 3=recording, 4=draining, 5=complete,
// 6=unsupported, 7=state changed/timeout, 8=runtime destroyed.
// sample flags: 1=present interval, 2=completed GPU bracket, 4=effects enabled.
// GPU scope: all ReShade effects between official begin/finish events; excludes
// engine snapshot/resolve and ReShade overlay. Present interval is total wall
// frame cadence, NOT GPU busy duration. No query slot is reused within a window.
static_assert(sizeof(OpReShadeTimingSampleV1) == 24);
static_assert(sizeof(OpReShadeTimingV1) == 5808);
