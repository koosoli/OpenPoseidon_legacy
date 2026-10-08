// SPDX-License-Identifier: GPL-3.0-or-later
// The optional official ReShade API is BSD-3-Clause OR MIT; downloaded separately.
#include <atomic>
#include <reshade.hpp>

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

void OnInit(reshade::api::effect_runtime* runtime)
{
    observed.store(runtime->get_effects_state() ? 1 : 0);
}

void OnDestroy(reshade::api::effect_runtime*)
{
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

extern "C" __declspec(dllexport) bool AddonInit(HMODULE module, HMODULE reshadeModule)
{
    if (!reshade::register_addon(module, reshadeModule))
        return false;
    reshade::register_event<reshade::addon_event::init_effect_runtime>(OnInit);
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(OnDestroy);
    reshade::register_event<reshade::addon_event::reshade_present>(OnPresent);
    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE module, HMODULE)
{
    observed.store(-1);
    requested.store(-1);
    reshade::unregister_addon(module);
}
