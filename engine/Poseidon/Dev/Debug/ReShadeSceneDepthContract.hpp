// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

// Optional loaded add-on ABI, independent of the normal engine/DLL handshake.
// Vulkan native handles follow pinned ReShade 6.8 resource/device API contracts.
// Borrowed image/view: producer owns them, consumers never destroy or transition
// them. Producer publishes only after submission in shader-read state on queue.
// Invalidate MUST clear bindings and wait that queue before producer destruction.
struct OpReShadeSceneDepthV1
{
    std::uint32_t bytes, version;
    std::uint64_t generation, frame, device, queue, image, view, sampleFrame, sampleGeneration;
    std::uint32_t width, height, samples, flags, finiteSamples, nonzeroSamples, gridWidth, gridHeight;
    float nearPlane, minimum, maximum, reserved;
    float projection[16]; // Original forward infinite projection; stored depth is reversed-Z.
    float grid[256]; // Row-major centres of 16x16 bins, not a full-image reduction.
};
struct OpReShadeSceneDepthStateV1
{
    OpReShadeSceneDepthV1 publication;
    std::uint64_t boundFrame, boundGeneration;
    std::uint32_t status, reserved;
};
// publication flags: 1 = captured this frame, 2 = completed finite/nonempty grid,
// 4 = source was 4x MSAA (nearest-covered-sample resolve); published image is 1x.
// status: 0=no publication, 1=waiting numeric proof, 2=bound, 3=device/queue mismatch,
// 4=unsupported dimensions/samples, 5=invalid publication.
static_assert(sizeof(OpReShadeSceneDepthV1) == 1208);
static_assert(offsetof(OpReShadeSceneDepthV1, nearPlane) == 104);
static_assert(offsetof(OpReShadeSceneDepthV1, grid) == 184);
static_assert(sizeof(OpReShadeSceneDepthStateV1) == 1232);
