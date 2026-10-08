// SPDX-License-Identifier: GPL-3.0-or-later
// The optional official ReShade API is BSD-3-Clause OR MIT; downloaded separately.
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>
#include <reshade.hpp>
#include "../../engine/Poseidon/Dev/Debug/ReShadeSceneDepthContract.hpp"

namespace
{
// The engine only exchanges atomics. ReShade methods run in its presentation
// callback, never from the engine's Dev Tools/UI thread.
std::atomic<int> requested{-1};
std::atomic<int> observed{-1};
std::atomic<bool> comparisonRequested{false};
std::atomic<unsigned> techniqueCount{0};
std::atomic<unsigned> activeTechniqueCount{0};
std::atomic<unsigned> captureCount{0};
std::atomic<unsigned> captureStage{0};
std::atomic<unsigned> presentCount{0};
int comparisonStage = 0;
int settleFrames = 0;
bool restoreEffects = false;
std::mutex depthMutex;
OpReShadeSceneDepthStateV1 depthState{};
std::uint64_t depthBoundQueue = 0;
std::vector<reshade::api::effect_runtime*> depthRuntimes;

void DepthReady(reshade::api::effect_runtime* runtime, bool ready)
{
    // FX reload invalidates uniform handles: look up the original probe each time.
    const auto uniform = runtime->find_uniform_variable("OpenPoseidonDepthProbe.fx", "DepthReady");
    if (uniform.handle) runtime->set_uniform_value_bool(uniform, &ready, 1);
    const auto dhReady = runtime->find_uniform_variable("OpenPoseidonDH.fx", "OpSceneReady");
    if (dhReady.handle) runtime->set_uniform_value_bool(dhReady, &ready, 1);
    if (ready)
    {
        const auto& p = depthState.publication;
        const float projection[] = {p.projection[0], p.projection[5], p.nearPlane, 0};
        const float offset[] = {p.projection[8], p.projection[9]};
        const auto dhProjection = runtime->find_uniform_variable("OpenPoseidonDH.fx", "OpSceneProjection");
        const auto dhOffset = runtime->find_uniform_variable("OpenPoseidonDH.fx", "OpSceneOffset");
        if (dhProjection.handle) runtime->set_uniform_value_float(dhProjection, projection, 4);
        if (dhOffset.handle) runtime->set_uniform_value_float(dhOffset, offset, 2);
    }
}

void OnBeginEffects(reshade::api::effect_runtime* runtime, reshade::api::command_list*,
                    reshade::api::resource_view, reshade::api::resource_view)
{
    std::lock_guard lock(depthMutex);
    const auto& p = depthState.publication;
    bool ready = false;
    if (p.flags & 1u)
    {
        auto* device = runtime->get_device();
        auto* queue = runtime->get_command_queue();
        uint32_t width = 0, height = 0;
        runtime->get_screenshot_width_and_height(&width, &height);
        if (device->get_api() != reshade::api::device_api::vulkan ||
            device->get_native() != p.device || !queue || queue->get_native() != p.queue)
            depthState.status = 3;
        else if (width != p.width || height != p.height || p.samples != 1)
            depthState.status = 4;
        else if (!(p.flags & 2u) || p.sampleGeneration != p.generation ||
                 p.finiteSamples != 256 || p.nonzeroSamples == 0 || !(p.maximum > p.minimum) ||
                 p.frame - p.sampleFrame > 120)
            depthState.status = 1;
        else
        {
            // API 6.8 explicitly defines resource_view=VkImageView and
            // resource=VkImage. vkCreateImage/View hooks have tracked these wgpu
            // objects; reject an untracked or mismatched view rather than guessing.
            const reshade::api::resource_view view{p.view};
            const auto resource = device->get_resource_from_view(view);
            if (resource.handle != p.image)
                depthState.status = 5;
            else
            {
                const auto desc = device->get_resource_desc(resource);
                ready = desc.type == reshade::api::resource_type::texture_2d &&
                    desc.texture.format == reshade::api::format::r32_float &&
                    desc.texture.width == p.width && desc.texture.height == p.height;
                depthState.status = ready ? 2u : 5u;
            }
        }
    }
    runtime->update_texture_bindings("OP_SCENE_DEPTH", {ready ? p.view : 0}, {ready ? p.view : 0});
    // GenericDepth owns bufready_depth and DEPTH; neither is modified here.
    // Only engine-semantic presets opt into this separately driven uniform.
    if ((p.flags & 1u) || depthState.boundFrame) DepthReady(runtime, ready);
    if (ready)
    {
        depthState.boundFrame = p.frame;
        depthState.boundGeneration = p.generation;
        depthBoundQueue = p.queue;
    }
}

void OnInit(reshade::api::effect_runtime* runtime)
{
    observed.store(runtime->get_effects_state() ? 1 : 0);
    std::lock_guard lock(depthMutex);
    depthRuntimes.push_back(runtime);
}

void OnDestroy(reshade::api::effect_runtime* runtime)
{
    {
        std::lock_guard lock(depthMutex);
        std::erase(depthRuntimes, runtime);
        depthState.boundFrame = depthState.boundGeneration = 0;
        depthState.status = 0;
    }
    observed.store(-1);
    requested.store(-1);
    comparisonStage = 0;
    comparisonRequested.store(false);
    techniqueCount.store(0);
    activeTechniqueCount.store(0);
    captureStage.store(0);
}

void OnPresent(reshade::api::effect_runtime* runtime)
{
    presentCount.fetch_add(1);
    unsigned techniques = 0;
    unsigned active = 0;
    runtime->enumerate_techniques(nullptr, [&](reshade::api::effect_runtime* current,
                                             reshade::api::effect_technique technique)
    {
        ++techniques;
        if (current->get_technique_state(technique)) ++active;
    });
    techniqueCount.store(techniques);
    activeTechniqueCount.store(active);
    const int next = requested.exchange(-1);
    if (next >= 0)
        runtime->set_effects_state(next != 0);
    // This event runs after effects. Capture each state only after it has
    // actually rendered, rather than relabelling the engine's pre-injector frame.
    if (comparisonStage == 0 && techniques > 0 && comparisonRequested.exchange(false))
    {
        restoreEffects = runtime->get_effects_state();
        runtime->set_effects_state(false);
        comparisonStage = 1;
        settleFrames = 4;
    }
    else if (comparisonStage != 0 && --settleFrames <= 0)
    {
        runtime->save_screenshot(comparisonStage == 1 ? "Native" : "ReShade");
        if (comparisonStage == 1)
        {
            runtime->set_effects_state(true);
            comparisonStage = 2;
            settleFrames = 4;
        }
        else
        {
            runtime->set_effects_state(restoreEffects);
            comparisonStage = 0;
            captureCount.fetch_add(1);
        }
    }
    observed.store(runtime->get_effects_state() ? 1 : 0);
    captureStage.store(comparisonStage);
}
}

extern "C" __declspec(dllexport) const char* NAME = "Open Poseidon Engine controls";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Optional Dev Tools control of ReShade effects.";

// Own, versioned, minimal C ABI: 0 = unavailable, 1 = ready/off, 3 = ready/on.
extern "C" __declspec(dllexport) unsigned OpReShadeBridgeVersion() { return 1; }
extern "C" __declspec(dllexport) unsigned OpReShadeGetStatus()
{
    const int state = observed.load();
    return state < 0 ? 0u : 1u | (state ? 2u : 0u);
}
extern "C" __declspec(dllexport) void OpReShadeSetEnabled(int enabled)
{
    requested.store(enabled != 0 ? 1 : 0);
}
extern "C" __declspec(dllexport) void OpReShadeCaptureComparison()
{
    comparisonRequested.store(true);
}
extern "C" __declspec(dllexport) unsigned OpReShadeGetTechniqueCount() { return techniqueCount.load(); }
extern "C" __declspec(dllexport) unsigned OpReShadeGetActiveTechniqueCount() { return activeTechniqueCount.load(); }
extern "C" __declspec(dllexport) unsigned OpReShadeGetCaptureCount() { return captureCount.load(); }
extern "C" __declspec(dllexport) unsigned OpReShadeGetCaptureStage() { return captureStage.load(); }
extern "C" __declspec(dllexport) unsigned OpReShadeGetPresentCount() { return presentCount.load(); }

extern "C" __declspec(dllexport) unsigned OpReShadePublishSceneDepthV1(
    const OpReShadeSceneDepthV1* publication, unsigned bytes, unsigned version)
{
    if (!publication || bytes != sizeof(*publication) || version != 1 ||
        publication->bytes != sizeof(*publication) || publication->version != 1)
        return 0;
    std::lock_guard lock(depthMutex);
    const auto& p = *publication;
    if ((p.flags & ~7u) || ((p.flags & 1u) && (!p.generation || !p.frame || !p.device || !p.queue || !p.image || !p.view ||
        !p.width || !p.height || p.width > 8192 || p.height > 8192 || p.samples != 1 ||
        p.gridWidth != 16 || p.gridHeight != 16 || !std::isfinite(p.nearPlane) || p.nearPlane <= 0 ||
        p.generation < depthState.publication.generation ||
        (p.generation == depthState.publication.generation && p.frame <= depthState.publication.frame) ||
        !std::all_of(std::begin(p.projection), std::end(p.projection), [](float x) { return std::isfinite(x); }) ||
        p.projection[0] <= 0 || p.projection[5] <= 0)))
        return 0;
    if ((p.flags & 2u) && (p.sampleGeneration != p.generation || !p.sampleFrame || p.sampleFrame > p.frame ||
        p.finiteSamples != 256 || p.nonzeroSamples == 0 || p.nonzeroSamples > 256 ||
        !std::isfinite(p.minimum) || !std::isfinite(p.maximum) || p.minimum < 0 || p.maximum > 1 || p.maximum <= p.minimum ||
        !std::all_of(std::begin(p.grid), std::end(p.grid), [](float x) { return std::isfinite(x) && x >= 0 && x <= 1; })))
        return 0;
    if (p.flags & 2u)
    {
        const auto limits = std::minmax_element(std::begin(p.grid), std::end(p.grid));
        const auto nonzero = std::count_if(std::begin(p.grid), std::end(p.grid), [](float x) { return x > 0; });
        if (*limits.first != p.minimum || *limits.second != p.maximum || nonzero != p.nonzeroSamples)
            return 0;
    }
    depthState.publication = p;
    depthState.status = (p.flags & 1u) ? 1u : 0u;
    return 1;
}

extern "C" __declspec(dllexport) unsigned OpReShadeGetSceneDepthV1(
    OpReShadeSceneDepthStateV1* output, unsigned bytes, unsigned version)
{
    if (!output || bytes != sizeof(*output) || version != 1) return 0;
    std::lock_guard lock(depthMutex);
    *output = depthState;
    return 1;
}

extern "C" __declspec(dllexport) void OpReShadeInvalidateSceneDepthV1()
{
    std::lock_guard lock(depthMutex);
    for (auto* runtime : depthRuntimes)
    {
        runtime->update_texture_bindings("OP_SCENE_DEPTH", {}, {});
        DepthReady(runtime, false);
        // A wgpu completion fence cannot cover externally submitted effects.
        // Producer calls this on its serialized render/present thread before
        // dropping/replacing its borrowed texture/view. Wait the external queue.
        auto* queue = runtime->get_command_queue();
        if (queue && (queue->get_native() == depthState.publication.queue || queue->get_native() == depthBoundQueue))
            queue->wait_idle();
    }
    depthState = {};
    depthBoundQueue = 0;
}

extern "C" __declspec(dllexport) bool AddonInit(HMODULE module, HMODULE reshadeModule)
{
    if (!reshade::register_addon(module, reshadeModule))
        return false;
    reshade::register_event<reshade::addon_event::init_effect_runtime>(OnInit);
    reshade::register_event<reshade::addon_event::reshade_begin_effects>(OnBeginEffects);
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(OnDestroy);
    reshade::register_event<reshade::addon_event::reshade_present>(OnPresent);
    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE module, HMODULE)
{
    OpReShadeInvalidateSceneDepthV1();
    {
        std::lock_guard lock(depthMutex);
        depthRuntimes.clear();
    }
    observed.store(-1);
    requested.store(-1);
    reshade::unregister_addon(module);
}
