#include <Poseidon/UI/Settings/GraphicsApply.hpp>
#ifdef _WIN32
#include <windows.h>
#endif

#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp> // pulls Engine + extern GEngine
#include <Poseidon/World/Scene/Scene.hpp>

namespace Poseidon
{

} // namespace Poseidon
#include <Poseidon/Core/Game/GameLoop.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <cstdlib>
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
// Anchor the ladder to the shared shipped coverage default. A startup quality
// preset must not overwrite the owner's new half-coverage meadow default.
void ApplyGrassTier(GraphicsConfig::Tier tier, Engine::GrassSettings& g)
{
    const float defaultCoverage = Engine::GrassSettings{}.density;
    switch (tier)
    {
        case GraphicsConfig::TierOff:
            g.enabled = false;
            return;
        case GraphicsConfig::TierLow:
            g.enabled = true;
            g.density = defaultCoverage * 0.35f;
            g.radius = 15.0f;
            g.castShadows = false;
            return;
        case GraphicsConfig::TierMedium:
            g.enabled = true;
            g.density = defaultCoverage * 0.60f;
            g.radius = 25.0f;
            return;
        case GraphicsConfig::TierHigh:
            g.enabled = true;
            g.density = defaultCoverage * 0.85f;
            g.radius = 33.0f;
            return;
        case GraphicsConfig::TierUltra:
        case GraphicsConfig::TierExtreme:
        default:
            g.enabled = true;
            g.density = defaultCoverage;
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
        // Upscaler rows (REN-TEMP-001): live through the temporal tuning. Auto =
        // DLSS when its route is up, else FSR 1 at Quality; Off = pure native.
        // The legacy SSAA row (renderScale > 1) owns the scale when pinned.
        if (GEngine->SupportsTemporalTuning())
        {
            Engine::TemporalSettings t = GEngine->GetTemporalTuning();
            const Engine::TemporalInfo info = GEngine->GetTemporalInfo();
            t.dlssOn = (cfg.dlssMode == -1 || cfg.dlssMode == 1);
            t.fsrOn = (cfg.dlssMode == -1 || cfg.dlssMode == 2);
            t.fsrSharpness = std::clamp(cfg.fsrSharpness, 0.0f, 2.0f);
            t.dlssSharpen = cfg.dlssSharpen != 0;
            t.jitterPhases = std::clamp(cfg.jitterPhases, 0, 64);
            const bool ssaaPinned = cfg.renderScale > 1.01f;
            if (!ssaaPinned)
            {
                if (cfg.dlssMode == 0)
                {
                    t.temporalOn = false;
                    t.renderScalePct = 100;
                }
                else if (cfg.upscalerQuality >= 50 && cfg.upscalerQuality <= 100)
                {
                    t.renderScalePct = cfg.upscalerQuality;
                }
                else if (cfg.dlssMode == 2 || (cfg.dlssMode == -1 && !info.dlssRoute))
                {
                    // FSR chosen (or Auto without DLSS): Quality point.
                    t.renderScalePct = 67;
                }
                if (cfg.dlssMode == 1 || (cfg.dlssMode == -1 && info.dlssRoute))
                    t.temporalOn = true;
            }
            GEngine->SetTemporalTuning(t);
        }
        GEngine->SetMultitexturing(cfg.multitexturing);
    }
    // POSEIDON_FPS_CAP=<n> pins the cap for a benchmark run without touching the saved setting
    // (same shape as POSEIDON_VSYNC in EngineWgpu::SetSwapInterval). 0 = uncapped.
    int fpsCap = cfg.fpsCap;
    if (const char* pin = std::getenv("POSEIDON_FPS_CAP"); pin != nullptr && pin[0] != 0)
    {
        fpsCap = std::atoi(pin);
        if (fpsCap != cfg.fpsCap)
            LOG_INFO(Graphics, "Graphics: fpsCap {} requested, pinned to {} by POSEIDON_FPS_CAP", cfg.fpsCap, fpsCap);
    }
    gUserFpsCap = fpsCap;
}

} // namespace Poseidon

namespace Poseidon
{
std::string DlssRuntimeVersionString()
{
#ifdef _WIN32
    static std::string cached = []() -> std::string {
        // Prefer the module NGX actually loaded; fall back to the file beside the exe
        // (present before the first DLSS frame, and on boots where DLSS stays idle).
        char path[MAX_PATH]{};
        if (HMODULE m = GetModuleHandleA("nvngx_dlss.dll"))
        {
            if (!GetModuleFileNameA(m, path, MAX_PATH))
                path[0] = 0;
        }
        if (!path[0])
        {
            char exe[MAX_PATH]{};
            if (GetModuleFileNameA(nullptr, exe, MAX_PATH))
            {
                std::string p(exe);
                const size_t slash = p.find_last_of("\\/");
                p = (slash == std::string::npos ? std::string() : p.substr(0, slash + 1)) + "nvngx_dlss.dll";
                if (p.size() < MAX_PATH)
                    memcpy(path, p.c_str(), p.size() + 1);
            }
        }
        if (!path[0])
            return {};
        DWORD handle = 0;
        const DWORD size = GetFileVersionInfoSizeA(path, &handle);
        if (!size)
            return {};
        std::string blob(size, '\0');
        if (!GetFileVersionInfoA(path, 0, size, blob.data()))
            return {};
        VS_FIXEDFILEINFO* ffi = nullptr;
        UINT len = 0;
        if (!VerQueryValueA(blob.data(), "\\", reinterpret_cast<void**>(&ffi), &len) || !ffi)
            return {};
        char out[48];
        // Marketing generation in front of the file version: 300+ file versions are
        // the DLSS 4 transformer generation, 3xx below that the DLSS 3 line - the
        // owner reasonably asked "which DLSS is 310.7.0?".
        const unsigned major = HIWORD(ffi->dwFileVersionMS);
        const char* gen = major >= 300 ? "4" : major >= 200 ? "3" : "2";
        snprintf(out, sizeof(out), "%s (%u.%u.%u)", gen, major, LOWORD(ffi->dwFileVersionMS),
                 HIWORD(ffi->dwFileVersionLS));
        return out;
    }();
    return cached;
#else
    return {};
#endif
}
} // namespace Poseidon
