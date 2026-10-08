// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <mutex>
#include <reshade.hpp>
#include "../../engine/Poseidon/Dev/Debug/ReShadeTimingContract.hpp"

// Developer-triggered bounded query window. Never wraps/reuses query indices,
// avoiding the Vulkan stale-result race before a GPU-side reset executes.
class OpReShadeTiming
{
    std::mutex mutex;
    std::atomic<unsigned> requested{0};
    OpReShadeTimingV1 state{};
    reshade::api::effect_runtime* owner = nullptr;
    reshade::api::query_heap heap{};
    std::array<bool, 240> pending{};
    int open = -1;
    unsigned drain = 0;
    bool enabled = false;
    LARGE_INTEGER last{}, qpcFrequency{};
    void Release()
    {
        if (owner && heap.handle)
        {
            // Only explicit reset/destruction, never measured frame readback.
            owner->get_command_queue()->wait_idle();
            owner->get_device()->destroy_query_heap(heap);
        }
        heap = {}; owner = nullptr; pending.fill(false); open = -1;
    }
public:
    bool Busy()
    {
        std::lock_guard lock(mutex);
        return requested.load() || (state.status >= 2 && state.status <= 4);
    }
    unsigned Request(unsigned frames)
    {
        if (frames < 30 || frames > 240) return 0;
        std::lock_guard lock(mutex);
        if (state.status >= 2 && state.status <= 4) return 0;
        unsigned empty = 0;
        return requested.compare_exchange_strong(empty, frames) ? 1 : 0;
    }
    unsigned Query(OpReShadeTimingV1* out, unsigned bytes, unsigned version)
    {
        if (!out || bytes != sizeof(*out) || version != 1) return 0;
        std::lock_guard lock(mutex); *out = state;
        out->bytes = sizeof(*out); out->version = 1; return 1;
    }
    void Begin(reshade::api::effect_runtime* runtime, reshade::api::command_list* commands)
    {
        std::lock_guard lock(mutex);
        if (owner != runtime || state.status != 3 || state.recorded >= state.frames || open >= 0) return;
        open = static_cast<int>(state.recorded);
        commands->end_query(heap, reshade::api::query_type::timestamp, open * 2);
    }
    void End(reshade::api::effect_runtime* runtime, reshade::api::command_list* commands)
    {
        std::lock_guard lock(mutex);
        if (owner != runtime || open < 0) return;
        commands->end_query(heap, reshade::api::query_type::timestamp, open * 2 + 1);
        pending[open] = true; open = -1;
    }
    void Present(reshade::api::effect_runtime* runtime, unsigned preview)
    {
        std::lock_guard lock(mutex);
        if (const unsigned frames = requested.exchange(0))
        {
            Release();
            const auto generation = state.generation + 1;
            state = {}; state.bytes = sizeof(state); state.version = 1;
            state.generation = generation; state.frames = frames;
            state.preview = preview;
            owner = runtime; enabled = runtime->get_effects_state();
            auto* queue = runtime->get_command_queue();
            state.frequency = queue->get_timestamp_frequency();
            if (runtime->get_device()->get_api() != reshade::api::device_api::vulkan || !state.frequency ||
                !runtime->get_device()->create_query_heap(reshade::api::query_type::timestamp, frames * 2, &heap))
            { state.status = 6; return; }
            // Pinned Vulkan create_query_heap may submit an initial reset.
            // Complete initialization BEFORE the excluded warmup/measurement.
            queue->wait_idle();
            QueryPerformanceFrequency(&qpcFrequency); QueryPerformanceCounter(&last);
            state.status = 2; state.warmup = 20; drain = 0; return;
        }
        if (owner != runtime || state.status < 2 || state.status > 4) return;
        if (runtime->get_effects_state() != enabled || preview != state.preview) { state.status = 7; return; }
        LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
        const double interval = 1000.0 * static_cast<double>(now.QuadPart - last.QuadPart) / qpcFrequency.QuadPart;
        last = now;
        if (state.status == 2)
        { if (--state.warmup == 0) state.status = 3; return; }
        if (state.status == 3)
        {
            auto& sample = state.samples[state.recorded++];
            sample.presentMs = interval; sample.flags = 1u | (enabled ? 4u : 0u);
            if (state.recorded == state.frames) state.status = 4;
        }
        bool waiting = false;
        for (unsigned i = 0; i < state.recorded; ++i)
        {
            if (!pending[i]) continue;
            std::uint64_t ticks[2]{};
            // Official Vulkan API uses VK_QUERY_RESULT_64_BIT without WAIT_BIT.
            if (!runtime->get_device()->get_query_heap_results(heap, reshade::api::query_type::timestamp,
                    i * 2, 2, ticks, sizeof(ticks[0]))) { waiting = true; continue; }
            pending[i] = false;
            if (ticks[1] >= ticks[0])
            {
                const double ms = 1000.0 * static_cast<double>(ticks[1] - ticks[0]) / state.frequency;
                if (ms >= 0 && ms < 1000)
                { state.samples[i].gpuMs = ms; state.samples[i].flags |= 2; ++state.completedGpu; }
            }
        }
        if (state.status == 4)
        {
            if (!waiting) state.status = 5;
            else if (++drain > 240) state.status = 7;
        }
    }
    void Destroy(reshade::api::effect_runtime* runtime)
    {
        std::lock_guard lock(mutex);
        if (owner == runtime)
        { if (state.status >= 2 && state.status <= 4) state.status = 8; Release(); }
    }
};
