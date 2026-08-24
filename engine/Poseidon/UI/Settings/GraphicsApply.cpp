#include <Poseidon/UI/Settings/GraphicsApply.hpp>

#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp> // pulls Engine + extern GEngine
#include <Poseidon/World/Scene/Scene.hpp>

namespace Poseidon
{

} // namespace Poseidon
#include <Poseidon/Core/Game/GameLoop.hpp>
namespace Poseidon
{
using Poseidon::gUserFpsCap;

float TerrainGridForTier(GraphicsConfig::Tier tier)
{
    switch (tier)
    {
        case GraphicsConfig::TierLow:
            return 50.0f;
        case GraphicsConfig::TierMedium:
            return 25.0f;
        case GraphicsConfig::TierHigh:
            return 12.5f;
        case GraphicsConfig::TierUltra:
            return 6.25f;
        case GraphicsConfig::TierExtreme:
            return 3.125f;
        default:
            return 6.25f;
    }
}

namespace
{
// Tier → projected-screen-size multiplier for LOD selection.  Low
// halves the apparent size (picks coarser LOD); Ultra doubles
// (picks finer LOD).  Range matches Scene::SetObjectLODBias clamp
// of [0.25, 4.0].
float ObjectLodTierToBias(GraphicsConfig::Tier t)
{
    switch (t)
    {
        case GraphicsConfig::TierLow:
            return 0.5f;
        case GraphicsConfig::TierMedium:
            return 0.75f;
        case GraphicsConfig::TierHigh:
            return 1.0f;
        case GraphicsConfig::TierUltra:
            return 2.0f;
        default:
            return 1.0f;
    }
}

// VsyncMode enum → SDL swap interval ints (0 / 1 / -1).
int VsyncToInterval(GraphicsConfig::VsyncMode v)
{
    switch (v)
    {
        case GraphicsConfig::VsyncOff:
            return 0;
        case GraphicsConfig::VsyncOn:
            return 1;
        case GraphicsConfig::VsyncAdaptive:
            return -1;
    }
    return 1;
}
// Grass / ground clutter tier -> the renderer's GrassSettings.
//
// Grass is the single largest item in the GPU frame: measured on perf_abel at
// 1920x1080 on the wgpu backend, turning it off took the GPU frame from 33.40 ms
// to 23.85 ms -- 9.55 ms, 28.6%. For comparison the next lever down, GTAO, is
// 2.22 ms. Until this row existed the only way to spend that budget elsewhere was
// an undocumented WGR_GRASS environment variable.
//
// Only the three fields that drive COST are touched -- enabled, density and the
// detail radius -- plus shadow casting at the bottom tier. Everything else in
// GrassSettings is authored look (saturation, colour variation, species mix, dry
// patches, wind); a tier that reset those would change what the world looks like
// rather than how much it costs, and would also stamp on the dev panel's Grass tab.
// Hence read-modify-write through GetGrassSettings rather than a fresh struct.
//
// Ultra reproduces the historical shipped values exactly (see GrassSettings'
// in-class initialisers), so an install that never opens this row is unchanged.
void ApplyGrassTier(GraphicsConfig::Tier tier, Engine::GrassSettings& g)
{
    switch (tier)
    {
        case GraphicsConfig::TierOff:
            g.enabled = false;
            return;
        case GraphicsConfig::TierLow:
            g.enabled = true;
            g.density = 0.35f;
            g.radius = 15.0f;
            g.castShadows = false;
            return;
        case GraphicsConfig::TierMedium:
            g.enabled = true;
            g.density = 0.60f;
            g.radius = 25.0f;
            return;
        case GraphicsConfig::TierHigh:
            g.enabled = true;
            g.density = 0.85f;
            g.radius = 33.0f;
            return;
        case GraphicsConfig::TierUltra:
        case GraphicsConfig::TierExtreme:
        default:
            g.enabled = true;
            g.density = 1.0f;
            g.radius = 41.0f;
            return;
    }
}
} // namespace

void ApplyGraphicsConfigToEngine(const GraphicsConfig& cfg)
{
    if (GScene)
    {
        GScene->SetPreferredTerrainGrid(TerrainGridForTier(cfg.terrainDetail));
        GScene->SetObjectLODBias(ObjectLodTierToBias(cfg.objectLod));
        // Shadow tier — Off → both off; Low+ → both on.  Low/Med/High
        // discrimination is UI-only until a shadow-distance bias hook
        // lands.
        const bool shadowsOn = cfg.shadowQuality != GraphicsConfig::TierOff;
        GScene->SetObjectShadows(shadowsOn);
        GScene->SetVehicleShadows(shadowsOn);
        // Particles tier — Off → cloudlets off; Low/High → on.  Low vs
        // High is UI-only until the cloudlet system grows tiered density.
        GScene->SetCloudlets(cfg.particlesQuality != GraphicsConfig::TierOff);
    }
    if (GEngine)
    {
        Engine::GrassSettings grass = GEngine->GetGrassSettings();
        ApplyGrassTier(cfg.grassQuality, grass);
        GEngine->SetGrassSettings(grass);
    }
    if (GEngine)
    {
        GEngine->SetSwapInterval(VsyncToInterval(cfg.vsync));
        GEngine->SetBrightness(cfg.brightness);
        GEngine->SetGamma(cfg.gamma);
        GEngine->SetAlphaToCoverage(cfg.alphaToCoverage);
        GEngine->SetRenderScale(cfg.renderScale);
        GEngine->SetMsaaSamples(cfg.msaaSamples);
        GEngine->SetMultitexturing(cfg.multitexturing);
    }
    gUserFpsCap = cfg.fpsCap;
}

} // namespace Poseidon
