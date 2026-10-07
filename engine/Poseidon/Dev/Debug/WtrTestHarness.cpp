#include <Poseidon/World/World.hpp>
#include "WtrTestHarness.hpp"
#include <Poseidon/Graphics/Rendering/WaterInteractionBridge.hpp>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace Poseidon
{

WtrTestHarness& WtrTestHarness::Instance()
{
    static WtrTestHarness s_instance;
    return s_instance;
}

WtrTestHarness::WtrTestHarness()
{
    _presets = {{1, "WTR-Test-01 — Seabed Checkerboard", "Clear shallows, Snell's law refraction, depth extinction",
                 WtrTestAvailability::Available, "Available"},
                {2, "WTR-Test-02 — Pitch Sweep", "Reflection ownership & pitch-stability verification",
                 WtrTestAvailability::Available, "Available"},
                {3, "WTR-Test-03 — Ocean Altitude", "Altitude sequence (2m to 2000m) & horizon swell",
                 WtrTestAvailability::Available, "Available"},
                {4, "WTR-Test-04 — Projectile Grid", "Edge-triggered impact solver ring propagation",
                 WtrTestAvailability::Available, "Available"},
                {5, "WTR-Test-05 — Vessel Wake & Drag", "Vessel displacement wake & stern foam",
                 WtrTestAvailability::Available, "Available"},
                {6, "WTR-Test-06 — Wind-Sea & Swell", "JONSWAP spectrum, directional spreading, cross-swell",
                 WtrTestAvailability::Available, "Available"},
                {7, "WTR-Test-07 — Shoreline Swash", "Swash oscillation, coast fade, intertidal sand dampening",
                 WtrTestAvailability::Available, "Available"},
                {8, "WTR-Test-08 — Persistent Foam", "Crest foam generation, history advection, decay",
                 WtrTestAvailability::Available, "Available"},
                {9, "WTR-Test-09 — Underwater Froxels", "Submerged volumetric lighting & in-scattering",
                 WtrTestAvailability::Available, "Available"},
                {10, "WTR-Test-10 — Caustics & Sun Shafts", "Seabed directional caustics & sun shafts",
                 WtrTestAvailability::Available, "Available"}};
}

const WtrTestPresetInfo* WtrTestHarness::GetPresetInfo(int presetId) const
{
    for (const auto& p : _presets)
    {
        if (p.id == presetId)
            return &p;
    }
    return nullptr;
}

void WtrTestHarness::SelectPreset(int presetId, Engine::WaterSettings& settings, int debugView)
{
    if (!_active)
    {
        _savedSettings = settings;
        _savedDebugView = debugView;
        _hasSavedState = true;
    }
    _currentPresetId = presetId;
    ApplyPresetSettings(presetId, settings);
}

void WtrTestHarness::Start(Engine::WaterSettings& settings, int debugView)
{
    if (GWorld && GWorld->GetMode() == GModeNetware)
        return;
    if (!_active)
    {
        _savedSettings = settings;
        _savedDebugView = debugView;
        _hasSavedState = true;
    }
    _active = true;
    _paused = false;
    _singleStep = false;
    _frameIndex = 0;
    _testTime = 0.0f;
    _triggeredEventCount = 0;
    if (_currentPresetId > 0)
    {
        ApplyPresetSettings(_currentPresetId, settings);
    }
}

void WtrTestHarness::Pause()
{
    if (_active)
    {
        _paused = !_paused;
    }
}

void WtrTestHarness::StepFrame(Engine::WaterSettings& settings)
{
    if (_active)
    {
        _paused = true;
        _singleStep = true;
    }
}

void WtrTestHarness::Restart(Engine::WaterSettings& settings)
{
    _frameIndex = 0;
    _testTime = 0.0f;
    _triggeredEventCount = 0;
    _paused = false;
    _singleStep = false;
    if (_currentPresetId > 0)
    {
        ApplyPresetSettings(_currentPresetId, settings);
    }
}

void WtrTestHarness::Stop(Engine::WaterSettings& settings, int& outDebugView)
{
    if (_hasSavedState)
    {
        settings = _savedSettings;
        outDebugView = _savedDebugView;
        _hasSavedState = false;
    }
    _active = false;
    _paused = false;
    _singleStep = false;
    _frameIndex = 0;
    _testTime = 0.0f;
    _triggeredEventCount = 0;
}

void WtrTestHarness::ApplyPresetSettings(int presetId, Engine::WaterSettings& settings)
{
    settings.testScene = presetId;
    switch (presetId)
    {
        case 1: // Seabed Checkerboard
            settings.waveAmp = 0.35f;
            settings.waveSpeed = 1.0f;
            settings.foamIntensity = 0.2f;
            settings.colorExt = 0.05f;
            settings.debugView = 18; // Water-column depth
            break;
        case 2: // Pitch Sweep
            settings.waveAmp = 0.8f;
            settings.waveSpeed = 1.0f;
            settings.debugView = 25; // Reflection-source selection
            break;
        case 3: // Ocean Altitude
            settings.waveAmp = 1.8f;
            settings.waveSpeed = 1.2f;
            settings.fadeStart = 1000.0f;
            settings.fadeEnd = 4000.0f;
            settings.debugView = 0;
            break;
        case 4: // Projectile Grid
            settings.waveAmp = 0.20f;
            settings.waveSpeed = 1.0f;
            settings.debugView = 12; // Interaction height
            break;
        case 5: // Vessel Wake
            settings.waveAmp = 0.50f;
            settings.debugView = 17; // Surface velocity
            break;
        case 6: // Wind-Sea & Swell
            settings.waveAmp = 2.2f;
            settings.waveSpeed = 1.4f;
            settings.debugView = 1; // FFT displacement
            break;
        case 7: // Shoreline Swash
            settings.waveAmp = 0.6f;
            settings.swashAmp = 1.2f;
            settings.coastFade = 0.5f;
            settings.debugView = 18; // Water-column depth (the swash band animates it)
            break;
        case 8: // Persistent Foam
            settings.waveAmp = 2.0f;
            settings.foamIntensity = 1.5f;
            settings.debugView = 16; // Persistent foam history
            break;
        case 9: // Underwater froxel volume
            settings.waveAmp = 0.65f;
            settings.underwaterEffect = true;
            settings.debugView = 31; // In-scattering
            break;
        case 10: // FFT caustics + shadowed sun shafts
            settings.waveAmp = 0.80f;
            settings.underwaterEffect = true;
            settings.debugView = 33; // Caustic intensity
            break;
        default:
            break;
    }
}

bool WtrTestHarness::Update(float frameDt, Engine::WaterSettings& settings, Vector3& camPos, Vector3& camRot)
{
    if (GWorld && GWorld->GetMode() == GModeNetware)
    {
        // Do not restore single-player renderer settings into a network session.
        _active = false;
        _hasSavedState = false;
        return false;
    }
    if (!_active)
        return false;

    if (_paused && !_singleStep)
        return false;

    _singleStep = false;
    _frameIndex++;
    _testTime += _fixedDeltaTime;

    ComputeCameraTransform(_testTime, camPos, camRot);
    InjectEdgeTriggeredEvents(settings);
    return true;
}

void WtrTestHarness::ComputeCameraTransform(float t, Vector3& camPos, Vector3& camRot)
{
    switch (_currentPresetId)
    {
        case 1: // Seabed Checkerboard: Static overhead
            camPos = Vector3(100.0f, 1.5f, 100.0f);
            camRot = Vector3(-35.0f, 0.0f, 0.0f);
            break;
        case 2: // Pitch Sweep: Sweeps pitch from -45 to +45 deg
        {
            float pitch = -45.0f + 90.0f * (0.5f + 0.5f * std::sin(0.5f * t));
            camPos = Vector3(100.0f, 5.0f, 100.0f);
            camRot = Vector3(pitch, 0.0f, 0.0f);
            break;
        }
        case 3: // Altitude Sequence: 2m, 20m, 200m, 2000m
        {
            int stage = (static_cast<int>(t) / 4) % 4;
            static const float alts[4] = {2.0f, 20.0f, 200.0f, 2000.0f};
            camPos = Vector3(500.0f, alts[stage], 500.0f);
            camRot = Vector3(-15.0f, 0.0f, 0.0f);
            break;
        }
        case 4: // Projectile Grid: Fixed overhead
            camPos = Vector3(128.0f, 40.0f, 128.0f);
            camRot = Vector3(-85.0f, 0.0f, 0.0f);
            break;
        case 5: // Vessel wake: follow the deterministic synthetic stern emitter
        {
            const float wakeZ = 128.0f + 4.0f * t;
            camPos = Vector3(138.0f, 8.0f, wakeZ - 18.0f);
            camRot = Vector3(-22.0f, -25.0f, 0.0f);
            break;
        }
        case 7: // Shoreline Swash: Linear camera motion
            camPos = Vector3(50.0f + 5.0f * std::fmod(t, 20.0f), 3.0f, 100.0f);
            camRot = Vector3(-10.0f, 90.0f, 0.0f);
            break;
        case 9: // Underwater froxels: slow submerged pitch/yaw sweep
            camPos = Vector3(128.0f, -2.0f, 128.0f);
            camRot = Vector3(-8.0f + 10.0f * std::sin(t * 0.20f), 18.0f * std::sin(t * 0.12f), 0.0f);
            break;
        case 10: // Caustics: shallow submerged view aimed at the seabed
            camPos = Vector3(128.0f, -0.75f, 128.0f);
            camRot = Vector3(-42.0f, 12.0f * std::sin(t * 0.16f), 0.0f);
            break;
        default:
            break;
    }
}

void WtrTestHarness::InjectEdgeTriggeredEvents(Engine::WaterSettings& settings)
{
    (void)settings;
    if (_currentPresetId == 4) // Projectile Grid
    {
        // WTR-Test-04: known impact positions and radii (5 cm, 10 cm, 20 cm, 50 cm, 1 m),
        // one impact every 30 frames starting at frame 30. Deterministic grid centred on
        // the fixed overhead camera at (128, 40, 128) so every impact lands inside the
        // camera-relative interaction domain.
        static const float kRadii[5] = {0.05f, 0.10f, 0.20f, 0.50f, 1.00f};
        constexpr int kFirstFrame = 30;
        constexpr int kFrameStep = 30;
        if (_frameIndex >= kFirstFrame && (_frameIndex - kFirstFrame) % kFrameStep == 0)
        {
            const int slot = static_cast<int>((_frameIndex - kFirstFrame) / kFrameStep) % 5;
            const float spacing = 1.5f;
            const float gridX = 128.0f + (static_cast<float>(slot) - 2.0f) * spacing;
            const float gridZ = 128.0f;

            HydroWaterInteractionEvent ev{};
            ev.positionRadius[0] = gridX;
            ev.positionRadius[1] = gridZ;
            ev.positionRadius[2] = kRadii[slot];
            ev.positionRadius[3] = 0.30f; // strength
            ev.velocityKind[0] = 0.0f;
            ev.velocityKind[1] = 0.0f;
            ev.velocityKind[2] = -6.0f; // downward entry speed
            ev.velocityKind[3] = static_cast<float>(HydroWaterInteractionBullet);
            ev.timeLifeFoamMass[0] = 0.0f;  // 0 = stamp with the solver's now
            ev.timeLifeFoamMass[1] = 1.6f;  // lifetime (s)
            ev.timeLifeFoamMass[2] = 0.35f; // foam
            ev.timeLifeFoamMass[3] = 0.0f;
            ev.directionDepthFlags[0] = 0.0f;
            ev.directionDepthFlags[1] = 1.0f;
            ev.directionDepthFlags[2] = 0.0f;
            ev.directionDepthFlags[3] = static_cast<float>(HydroWaterInteractionPendingImpulse);
            SubmitWaterInteraction(ev);
            _triggeredEventCount++;
        }
    }
    else if (_currentPresetId == 5) // Deterministic moving large-body stern wake
    {
        // Retain one continuous emitter by using the same stable id every frame.
        // It travels north at 4 m/s; the interaction shader gates its Kelvin wedge
        // behind the stern and derives foam from the same trailing velocity field.
        HydroWaterInteractionEvent ev{};
        ev.positionRadius[0] = 128.0f;
        ev.positionRadius[1] = 128.0f + 4.0f * _testTime;
        ev.positionRadius[2] = 3.0f;
        ev.positionRadius[3] = 0.48f;
        ev.velocityKind[0] = 0.0f;
        ev.velocityKind[1] = 0.0f;
        ev.velocityKind[2] = 4.0f;
        ev.velocityKind[3] = static_cast<float>(HydroWaterInteractionContinuous);
        ev.timeLifeFoamMass[0] = 0.0f;
        ev.timeLifeFoamMass[1] = 1.0f;
        ev.timeLifeFoamMass[2] = 0.38f;
        ev.timeLifeFoamMass[3] = 4242.0f; // stable retained-emitter id
        ev.directionDepthFlags[0] = 0.0f;
        ev.directionDepthFlags[1] = 1.0f;
        ev.directionDepthFlags[2] = 0.0f;
        ev.directionDepthFlags[3] = static_cast<float>(HydroWaterInteractionCapsule | HydroWaterInteractionLargeBody);
        SubmitWaterInteraction(ev);
        _triggeredEventCount++;
    }
}

std::string WtrTestHarness::GenerateMetadataLog(const Engine::WaterSettings& settings, const Vector3& camPos,
                                                const Vector3& camRot) const
{
    std::ostringstream ss;
    ss << "{\n"
       << "  \"presetId\": " << _currentPresetId << ",\n"
       << "  \"presetName\": \"" << (_currentPresetId > 0 ? GetPresetInfo(_currentPresetId)->name : "None") << "\",\n"
       << "  \"frameIndex\": " << _frameIndex << ",\n"
       << "  \"fixedDeltaTime\": " << std::fixed << std::setprecision(6) << _fixedDeltaTime << ",\n"
       << "  \"testTime\": " << std::fixed << std::setprecision(3) << _testTime << ",\n"
       << "  \"cameraPos\": [" << camPos.X() << ", " << camPos.Y() << ", " << camPos.Z() << "],\n"
       << "  \"cameraRot\": [" << camRot.X() << ", " << camRot.Y() << ", " << camRot.Z() << "],\n"
       << "  \"debugView\": " << settings.debugView << ",\n"
       << "  \"freezeState\": {\n"
       << "    \"freezeTime\": " << (settings.freeze.freezeTime ? "true" : "false") << ",\n"
       << "    \"freezeFft\": " << (settings.freeze.freezeFft ? "true" : "false") << ",\n"
       << "    \"freezeInteraction\": " << (settings.freeze.freezeInteraction ? "true" : "false") << ",\n"
       << "    \"freezeFoam\": " << (settings.freeze.freezeFoam ? "true" : "false") << "\n"
       << "  },\n"
       << "  \"triggeredEventCount\": " << _triggeredEventCount << "\n"
       << "}";
    return ss.str();
}

} // namespace Poseidon
