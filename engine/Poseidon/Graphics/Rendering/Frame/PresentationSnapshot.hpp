#pragma once

// RenderSnapshot slice S1 (docs/roadmap/render-snapshot-design.md §3): the ENVIRONMENT
// block of the presentation snapshot -- every scalar the WGPU render path used to pull
// from live global state mid-frame (GScene's main light, fog ranges, camera pose,
// Glob's two clocks, the landscape's weather, GWind, GRain), captured ONCE per frame
// into a value the renderer then consumes.
//
// Why a value and not "just read the globals": the reads are scattered across ~30
// sites in three renderer files, several of them repeated per frame (NightEffect was
// read five times), and each carries its own null-check spelling. Collapsing them here
// makes the renderer's environment input auditable, makes the frame's values constant
// by construction, and turns the eventual move to a world-built snapshot (Phase 5
// step 1 proper) into relocating ONE call instead of re-touching every site.
//
// The has* booleans mirror the exact null-checks the sites used to perform (GScene
// set but MainLight missing is a real state in tools), so behaviour with partial
// worlds is unchanged. Main thread only; captured by the renderer at InitDraw today
// -- before Landscape::Draw, so the terrain/water submodules read the same frame's
// values -- and by World::SimulateAndDraw in a later slice.

#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Graphics/Rendering/Colors.hpp>

namespace Poseidon::render::frame
{

struct PresentationSnapshot
{
    bool valid = false;

    // GScene: distance fog.
    bool hasScene = false;
    float fogMinRange = 0.0f;
    float fogMaxRange = 0.0f;

    // GScene->GetCamera(): the pose the environment passes key off (sky raymarch
    // origin, sun-glare focus). The pushed per-camera matrices are untouched.
    bool hasCamera = false;
    Vector3 cameraPos = VZero;
    Vector3 cameraDir = VForward;

    // GScene->MainLight() (LightSun). sunDiffuse/sunAmbient are the RAW colours;
    // eye accommodation is engine-side state and stays with the consumer.
    bool hasMainLight = false;
    Color sunDiffuse = HWhite;
    Color sunAmbient = HWhite;
    Vector3 sunTravelDir = Vector3(-0.4f, -0.85f, -0.3f); // Direction(): light travel (moon-swung at night)
    Vector3 sunDirection = VUp;                           // SunDirection(): always astronomical
    float nightEffect = 0.0f;
    Vector3 moonDirection = VUp;
    float moonLightAmount = 0.0f;
    float moonPhase = 0.5f;
    float moonIllumination = 1.0f;
    float moonAngularRadius = 0.00452f;
    float moonBrightness = 0.0f;
    Vector3 moonSunDirection = VUp;

    // Glob's two clocks.
    float timeSeconds = 0.0f; // Glob.time.toFloat(): the water/cloud/sway clock
    float timeOfDay = 0.5f;   // Glob.clock.GetTimeOfDay(): 0..1, drives ToD presets

    // GLandscape weather + sea state.
    bool hasLandscape = false;
    float overcast = 0.0f;
    float seaLevel = 0.0f;
    float landscapeRainDensity = 0.0f;

    // GRain (the visible-drops system) and GWind.
    float rainEffectiveDensity = 0.0f;
    float seaRippleGain = 0.0f;
    bool windActive = false;
    float windSpeed = 0.0f;
    float windDirectionRad = 0.0f;
    float windGustFraction = 0.0f;
    float windMeanSpeed = 0.0f;        // gust-free mean: the cloud deck / sea spectrum inputs
    float windMeanDirectionRad = 0.0f;
};

// The one instance. Main thread only, rewritten every frame.
PresentationSnapshot& GPresentationSnapshot();

// Fill the instance from live globals. Null-safe: with no world/scene up, the has*
// flags stay false and every field keeps the default the consuming sites used.
void CapturePresentationSnapshot();

} // namespace Poseidon::render::frame
