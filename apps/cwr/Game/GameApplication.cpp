#include <Poseidon/Dev/Diag/FrameProfiler.hpp>
#include <Poseidon/Foundation/Platform/VersionNo.h>
#include "GameApplication.hpp"
#include <Poseidon/Foundation/Platform/InitBridge.hpp>
#include <Poseidon/Core/Game/GameLoop.hpp>
#include <Poseidon/Core/Version.hpp>
#include <Poseidon/Foundation/Platform/FPUSetup.hpp>
#include <Poseidon/Foundation/Platform/PoseidonInit.hpp>
#include <Poseidon/Core/Config/Config.hpp>
#include <Poseidon/Core/Config/UserConfig.hpp>
#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/Foundation/Platform/GamePaths.hpp>
#include <Poseidon/Foundation/Platform/StartupError.hpp>
#include <Poseidon/Audio/AudioFactory.hpp>
#include <Poseidon/Audio/Voice/VoiceBackend.hpp>
#include <Poseidon/UI/Settings/GameSettingsConfig.hpp>
#include <Poseidon/UI/Settings/DisplayStartupOverrides.hpp>
#include <Poseidon/Dev/Diag/PerfTrace.hpp>
#include <Poseidon/Core/TaskPool.hpp>
#include <Poseidon/Core/Progress.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Game/Mission/MissionPathLoader.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/Rendering/Draw/FontSystem.hpp>
#include <Poseidon/World/Scene/ScenePreloader.hpp>
#include <Poseidon/Graphics/Shared/RenderDocCapture.hpp>
#include <Poseidon/World/World.hpp>
#include <Evaluator/express.hpp>
#include <Poseidon/Dev/Debug/DebugTrap.hpp>
#include <Poseidon/UI/UITestEngine.hpp>
#include <Poseidon/UI/GameModule.hpp>
#include <Poseidon/UI/Missions/MissionsModule.hpp>
#include <Poseidon/UI/Campaigns/CampaignsModule.hpp>
#include <Poseidon/Core/ModSystem.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/Asset/Formats/Material/MaterialChannels.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp>
#include <Poseidon/World/Scene/Camera/CamEffects.hpp>
#include <Poseidon/World/Scene/Camera/CameraHold.hpp>
#include <Poseidon/Foundation/Memory/CheckMem.hpp>
#include <Poseidon/UI/Multiplayer/MultiplayerModule.hpp>
#include <Poseidon/UI/Editor/EditorModule.hpp>
#include <Poseidon/UI/Mods/ModsModule.hpp>
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>
#include <cjson/cJSON.h>
#include <stdint.h>
#include <cmath>
#include <fstream>
#include <stdlib.h>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Framework/AppFrame.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/GlobalAlive.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/Memtype.h>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <Poseidon/Foundation/platform.hpp>

using namespace Poseidon;
namespace Poseidon
{
void CreateClient(RString, int, RString);
RString GetUserParams();
} // namespace Poseidon

namespace Poseidon
{
IFilebankEncryption* CreateEncryptXOR1024(const void* context);
void ApplyGamePathsToLegacyGlobals();
extern RString ServerConfig;
void CreateServer();
} // namespace Poseidon

namespace
{
// Test hook for --remount-fail-selftest. One-shot; cleared on the first check.
bool s_forceRemountReloadFailOnce = false;

// Capture sidecar for reproducible renderer evidence.  It deliberately reports
// unavailable timer regions instead of synthesising zeroes, so a non-WGPU run
// cannot be mistaken for a measured GPU result.
bool WriteCaptureMetrics(const std::string& path)
{
    if (path.empty() || !GEngine)
        return false;

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "schema_version", 1);
    cJSON_AddStringToObject(root, "renderer", GEngine->GetRendererName().Data());
    cJSON_AddStringToObject(root, "runtime", GEngine->GetDebugName().Data());
    // Sized off kGpuRegionEnd, NOT a literal. This was `float timings[32]` while the region
    // count was 33, and wgr_get_gpu_timings clamps to out_len — so "Terrain: main colour" was
    // silently missing from EVERY capture ever written. A hardcoded buffer here does not fail
    // loudly when regions are appended, it just truncates the newest ones, which is the exact
    // failure mode this file exists to avoid.
    constexpr int kRegionCap = Engine::kGpuRegionEnd;
    float timings[kRegionCap] = {};
    const int count = GEngine->GetWaterGpuTimings(timings, kRegionCap);
    cJSON_AddBoolToObject(root, "gpu_timestamps_available", count > 0);
    // PERF-005: CPU encode ms per region, same indices. Always available (no adapter feature),
    // so a run without timestamp queries still reports where the CPU's recording time went.
    float cpuMs[kRegionCap] = {};
    const int cpuCount = GEngine->GetCpuTimings(cpuMs, kRegionCap);
    cJSON* regions = cJSON_AddArrayToObject(root, "gpu_timings_ms");
    for (int i = 0; i < count; ++i)
    {
        cJSON* region = cJSON_CreateObject();
        cJSON_AddStringToObject(region, "name", GEngine->GetWaterGpuTimingName(i));
        cJSON_AddNumberToObject(region, "milliseconds", timings[i]);
        cJSON_AddNumberToObject(region, "index", i);
        // Region nesting. Several regions physically enclose others (the two segment containers
        // enclose terrain/grass/object draws; each shadow cascade encloses its grass blades), so
        // a consumer that adds every row up over-counts. Emitted per row rather than documented
        // elsewhere because the only readers of this file are scripts:
        //     frame total ~= sum of rows where contained_by == -1
        cJSON_AddNumberToObject(region, "contained_by", (double)Engine::GpuTimerContainedBy(i));
        cJSON_AddBoolToObject(region, "container",
                              i >= Engine::kObjectGpuContainerBegin && i < Engine::kObjectGpuContainerEnd);
        if (i < cpuCount)
            cJSON_AddNumberToObject(region, "cpu_milliseconds", cpuMs[i]);
        cJSON_AddItemToArray(regions, region);
    }
    // PERF-005 object accounting. Emitted as its own object rather than folded into the region
    // array because these are counts, not times, and the two are summed differently.
    Engine::ObjectStatsOut objects;
    if (GEngine->GetObjectStats(objects))
    {
        cJSON* o = cJSON_AddObjectToObject(root, "objects");
        // valid == false means no readback has landed yet; every count below is then unknown,
        // not zero. Consumers must check this before drawing conclusions from a quiet frame.
        cJSON_AddBoolToObject(o, "valid", objects.valid);
        cJSON_AddNumberToObject(o, "registered_instances", objects.registeredInstances);
        cJSON_AddNumberToObject(o, "main_instances", objects.mainInstances);
        cJSON_AddNumberToObject(o, "main_records", objects.mainRecords);
        cJSON_AddNumberToObject(o, "main_draws", objects.mainDraws);
        cJSON_AddNumberToObject(o, "main_tris", objects.mainTris);
        cJSON_AddNumberToObject(o, "color_instances", objects.colorInstances);
        cJSON_AddNumberToObject(o, "color_records", objects.colorRecords);
        cJSON_AddNumberToObject(o, "color_draws", objects.colorDraws);
        cJSON_AddNumberToObject(o, "color_tris", objects.colorTris);
        cJSON_AddNumberToObject(o, "color_draws_solid", objects.colorDrawsSolid);
        cJSON_AddNumberToObject(o, "color_draws_alpha", objects.colorDrawsAlpha);
        cJSON_AddNumberToObject(o, "color_tris_solid", objects.colorTrisSolid);
        cJSON_AddNumberToObject(o, "color_tris_alpha", objects.colorTrisAlpha);
        cJSON_AddNumberToObject(o, "direct_calls", objects.directCalls);
        cJSON_AddNumberToObject(o, "direct_indirect_calls", objects.directIndirectCalls);
        cJSON_AddNumberToObject(o, "direct_instances", objects.directInstances);
        cJSON_AddNumberToObject(o, "direct_tris", objects.directTris);
        cJSON* cascades = cJSON_AddArrayToObject(o, "shadow_cascades");
        for (int c = 0; c < 4; ++c)
        {
            cJSON* entry = cJSON_CreateObject();
            cJSON_AddNumberToObject(entry, "cascade", c);
            cJSON_AddNumberToObject(entry, "instances", objects.shadowInstances[c]);
            cJSON_AddNumberToObject(entry, "draws", objects.shadowDraws[c]);
            cJSON_AddNumberToObject(entry, "tris", objects.shadowTris[c]);
            cJSON_AddItemToArray(cascades, entry);
        }
        // Index 0..6 = LOD 0..6, index 7 = "LOD 7 or coarser". Over the MAIN view's survivors.
        cJSON* lods = cJSON_AddArrayToObject(o, "main_lod_histogram");
        for (int l = 0; l < 8; ++l)
            cJSON_AddItemToArray(lods, cJSON_CreateNumber(objects.mainLodHist[l]));
    }
    Engine::GrassStatsOut grass;
    if (GEngine->GetGrassStats(grass))
    {
        cJSON* grassObject = cJSON_AddObjectToObject(root, "grass");
        cJSON_AddNumberToObject(grassObject, "near_instances", grass.nearInstances);
        cJSON_AddNumberToObject(grassObject, "mid_instances", grass.midInstances);
        cJSON_AddNumberToObject(grassObject, "far_instances", grass.farInstances);
        cJSON_AddNumberToObject(grassObject, "near_candidates", grass.nearCandidates);
        cJSON_AddNumberToObject(grassObject, "mid_candidates", grass.midCandidates);
        cJSON_AddNumberToObject(grassObject, "far_candidates", grass.farCandidates);
        cJSON_AddNumberToObject(grassObject, "near_vertices", grass.nearVertices);
        cJSON_AddNumberToObject(grassObject, "mid_vertices", grass.midVertices);
        cJSON_AddNumberToObject(grassObject, "far_vertices", grass.farVertices);
    }
    // CPU frame phases. FrameProfiler has been recording these on every frame all
    // along (it is always-on and unconditional), but nothing ever wrote them to a
    // capture artifact, so the CPU side of the frame was only ever visible through
    // the dev panel or the triPerfStats console command -- i.e. only to someone
    // sitting at the keyboard. That is why a world can be 90% CPU-bound and have
    // every recorded capture show only GPU numbers.
    //
    // Read the phase names, not the indices: object streaming currently bills to
    // `land:gnd` (UpdateModernObjectResidency is called from TerrainWgpu::DrawTerrain,
    // inside the terrain phase), so a large `land:gnd` on a heavy world is object
    // work, not terrain work.
    {
        const Poseidon::Dev::FrameProfiler& profiler = Poseidon::Dev::GFrameProfiler();
        if (profiler.FrameCount() > 0)
        {
            cJSON* phases = cJSON_AddObjectToObject(root, "cpu_frame_phases_ms");
            cJSON_AddNumberToObject(phases, "sampled_frames", profiler.FrameCount());
            cJSON_AddNumberToObject(phases, "avg_fps", profiler.AvgFps());
            const Poseidon::Dev::FrameProfiler::PhaseStats total = profiler.TotalStats();
            cJSON_AddNumberToObject(phases, "frame_avg", total.avgMs);
            cJSON_AddNumberToObject(phases, "frame_p95", total.p95Ms);
            // The minimum is the figure to quote when comparing arms; the profiler
            // exposes avg/p95/max, so the cheapest-frame rule is applied downstream
            // by taking the cheapest repeat rather than the cheapest frame here.
            cJSON_AddNumberToObject(phases, "frame_max", total.maxMs);
            cJSON* byPhase = cJSON_AddObjectToObject(phases, "phases");
            for (int p = 0; p < Poseidon::Dev::FrameProfiler::PhaseCount; ++p)
            {
                const Poseidon::Dev::FrameProfiler::PhaseStats s =
                    profiler.Stats(static_cast<Poseidon::Dev::FrameProfiler::Phase>(p));
                cJSON* entry = cJSON_AddObjectToObject(byPhase, Poseidon::Dev::FrameProfiler::PhaseName(p));
                cJSON_AddNumberToObject(entry, "avg", s.avgMs);
                cJSON_AddNumberToObject(entry, "p95", s.p95Ms);
                cJSON_AddNumberToObject(entry, "max", s.maxMs);
            }
        }
    }

    char* rendered = cJSON_Print(root);
    cJSON_Delete(root);
    if (!rendered)
        return false;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << rendered << '\n';
    cJSON_free(rendered);
    return static_cast<bool>(file);
}
} // namespace

// Force-link INIT_MODULE registrations that have no other references.
// The linker strips unreferenced .o files from static archives; this undefined
// symbol forces the linker to pull in gameStateExtTest.cpp.o.
extern void InitModuleGameStateExtTest();
__attribute__((used)) static void (*_forceLinkGameStateExtTest)() = &InitModuleGameStateExtTest;

#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/IO/ParamFile/InitLibraryElement.hpp>

#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/Graphics/GraphicsEngineFactory.hpp>
#include <Poseidon/UI/Settings/DisplayConfig.hpp>
#include <Poseidon/UI/Settings/AspectRatio.hpp>
#include <Poseidon/UI/Settings/Presentation.hpp>
#include <Poseidon/UI/Settings/GraphicsConfig.hpp>
#include <Poseidon/UI/Settings/GraphicsApply.hpp>
#include <Poseidon/Foundation/Common/GamePaths.hpp>
#include <SDL3/SDL.h>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Terrain/TerrainProfile.hpp>
#include <Poseidon/UI/Controls/UIControls.hpp>
#include <Poseidon/Core/resincl.hpp>
#include <Poseidon/Dev/Harness/HarnessServer.hpp>
#include <Poseidon/Dev/Harness/HarnessBuiltins.hpp>
#include <Poseidon/Dev/Debug/DebugOverlay.hpp>
#include <Poseidon/Dev/Harness/HarnessPlayerTracker.hpp>
#include <Poseidon/Dev/Harness/HarnessMissionStateTracker.hpp>

using namespace Poseidon::Dev;
using Poseidon::GEngine;
#include <Poseidon/Network/Network.hpp>

// gVonReceiveCallback defined in vonApp.cpp; avoid pulling vonApp.hpp (winsock order issue)
namespace Poseidon
{
extern std::function<void(uint32_t, int)> gVonReceiveCallback;
}

#include <filesystem>
#include <algorithm>
#include <cstring>

namespace Poseidon
{
extern Scene* GScene;
}
using Poseidon::GScene;
#include <memory>
#include <functional>
#include <random>

namespace Poseidon
{
extern World* GWorld;
}
using Poseidon::GWorld;

// Defined at global scope in engine/Poseidon/World/WorldSetup.cpp, next to the
// `showCinemaBorder` global it writes.  Declared here the same way DebugOverlay.cpp
// declares it, rather than pulling in a World header for one setter.
void ShowCinemaBorder(bool show);

namespace
{
// Isolated test-mission staging: --test-mission copies the mission into
// TempDir/mission-smoke/<rand>/Missions/<name>/ and loads it from there, so the
// game never writes a Missions/ tree into the (shared/read-only) game dir. The
// staged copy is removed on clean exit (RunMainLoop).
std::filesystem::path s_testMissionStageRoot;

std::filesystem::path BuildIsolatedMissionStageRoot()
{
    namespace fs = std::filesystem;
    fs::path tempRoot = fs::path(GamePaths::Instance().TempDir()) / "mission-smoke";
    std::error_code ec;
    fs::create_directories(tempRoot, ec);
    std::random_device rd;
    char suffix[17];
    snprintf(suffix, sizeof(suffix), "%08x%08x", rd(), rd());
    return tempRoot / suffix;
}

// Copy the --test-mission input into isolated temp storage and return the staged
// path for ResolveMissionFile. On any failure returns the input unchanged so the
// caller's resolve step reports the error rather than this masking it.
std::string StageTestMissionForGame(const std::string& testMission)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path srcPath(testMission);
    if (!fs::exists(srcPath, ec) || ec)
        return testMission;

    fs::path stageRoot = BuildIsolatedMissionStageRoot();
    fs::path missionsDir = stageRoot / GameDirs::Missions;
    fs::create_directories(missionsDir, ec);

    const fs::path dest = missionsDir / srcPath.filename();
    if (fs::is_directory(srcPath, ec))
        fs::copy(srcPath, dest, fs::copy_options::overwrite_existing | fs::copy_options::recursive, ec);
    else
        fs::copy_file(srcPath, dest, fs::copy_options::overwrite_existing, ec);
    if (ec)
        return testMission;

    s_testMissionStageRoot = stageRoot;
    return dest.string();
}
// DisplayConfig::Environment impl backed by the live IGraphicsEngine.
// Mirrors LiveAudioEnv in SoundSystemOAL.cpp.
struct LiveDisplayEnv : DisplayConfig::Environment
{
    IGraphicsEngine* engine;
    explicit LiveDisplayEnv(IGraphicsEngine* e) : engine(e) {}

    int GetMonitorCount() const override
    {
        if (!engine)
            return 1;
        FindArray<MonitorInfo> list;
        const_cast<IGraphicsEngine*>(engine)->ListMonitors(list);
        return list.Size() > 0 ? list.Size() : 1;
    }

    std::vector<std::pair<int, int>> ListResolutions(int /*monitorIdx*/) const override
    {
        std::vector<std::pair<int, int>> out;
        if (!engine)
            return out;
        FindArray<ResolutionInfo> list;
        const_cast<IGraphicsEngine*>(engine)->ListResolutions(list);
        for (int i = 0; i < list.Size(); ++i)
            out.emplace_back(list[i].w, list[i].h);
        return out;
    }

    std::vector<int> ListRefreshRates(int /*monitorIdx*/, int /*w*/, int /*h*/) const override
    {
        std::vector<int> out;
        if (!engine)
            return out;
        FindArray<int> list;
        const_cast<IGraphicsEngine*>(engine)->ListRefreshRates(list);
        for (int i = 0; i < list.Size(); ++i)
            out.push_back(list[i]);
        return out;
    }
};

std::string DisplayConfigPath()
{
    return GamePaths::Instance().UserDir() + "display.cfg";
}

// Resize hook — called from EngineGL33::OnWindowResized after _w/_h
// are updated.  Re-runs the aspect policy for the new viewport so
// UI doesn't stay pillarboxed at a stale boot-time rectangle.  Also the
// re-apply path for live dev-panel / tri aspect changes, which mutate
// AspectRatio::Live() and then FireResizePostHook() to take effect.
void OnViewportResized(int w, int h)
{
    if (!GEngine || w <= 0 || h <= 0)
        return;
    const AspectRatio::Settings settings = Poseidon::Presentation::Apply(w, h);

    LOG_INFO(Graphics,
             "Aspect (resize): viewport={}x{} override={} uiX=[{:.3f}..{:.3f}] world=[{:.3f}..{:.3f}]x[{:.3f}..{:.3f}] "
             "leftFOV={:.3f} topFOV={:.3f}",
             w, h, AspectRatio::Live().overrideEnabled ? 1 : 0, settings.uiTopLeftX, settings.uiBottomRightX,
             settings.worldLeft, settings.worldRight, settings.worldTop, settings.worldBottom, settings.leftFOV,
             settings.topFOV);
}

void ApplyAspectPolicy(DisplayConfig& cfg)
{
    if (!GEngine)
        return;

    GEngine->SetResizePostHook(&OnViewportResized);
    Poseidon::Presentation::SetPolicy(cfg.displayStyle, cfg.ultrawideClamp);

    const int w = GEngine->Width();
    const int h = GEngine->Height();
    UserConfig& userConfig = USER_CONFIG;
    if (Poseidon::Presentation::ConfigureUserFov(userConfig, w, h))
    {
        const RString userPath = Poseidon::GetUserParams();
        userConfig.SaveToFile(userPath);
    }
    const AspectRatio::Settings settings = Poseidon::Presentation::Apply(w, h);

    // Diagnostic — full resolved policy so log inspection makes
    // "UI is pillarboxed, why?" debuggable without re-running.  Modern +
    // viewportRatio in [4/3, 16/9] should yield uiX=[0..1] (full-width UI);
    // any other config falls back to the centered Faguss 4:3 strip.  When
    // override=1 the live dev-panel/tri controls drive it instead.
    LOG_INFO(Graphics,
             "Aspect: viewport={}x{} style={} clamp={} override={} -> leftFOV={:.3f} topFOV={:.3f} "
             "uiX=[{:.3f}..{:.3f}] uiY=[{:.3f}..{:.3f}] world=[{:.3f}..{:.3f}]x[{:.3f}..{:.3f}]",
             w, h, (int)cfg.displayStyle, (int)cfg.ultrawideClamp, AspectRatio::Live().overrideEnabled ? 1 : 0,
             settings.leftFOV, settings.topFOV, settings.uiTopLeftX, settings.uiBottomRightX, settings.uiTopLeftY,
             settings.uiBottomRightY, settings.worldLeft, settings.worldRight, settings.worldTop, settings.worldBottom);
}

std::optional<DisplayConfig::WindowMode> GetCliWindowModeOverride(const AppConfig& cli)
{
    if (cli.IsDisplayModeExplicit())
    {
        if (cli.GetDisplayMode() == "windowed")
            return DisplayConfig::Windowed;
        if (cli.GetDisplayMode() == "exclusive")
            return DisplayConfig::Fullscreen;
        return DisplayConfig::Borderless;
    }

    if (cli.IsWindowMode())
        return DisplayConfig::Windowed;

    return std::nullopt;
}

DisplayStartupOverrides GetCliDisplayOverrides(const AppConfig& cli)
{
    DisplayStartupOverrideRequest request;
    request.windowFlagExplicit = cli.IsWindowFlagExplicit() || cli.IsWindowMode();
    request.windowMode = GetCliWindowModeOverride(cli);
    if (cli.IsWidthExplicit())
        request.resolutionWidth = cli.GetWindowWidth();
    if (cli.IsHeightExplicit())
        request.resolutionHeight = cli.GetWindowHeight();
    return BuildDisplayStartupOverrides(request);
}

// Eager-write defaults if the file is missing, then apply the cfg
// values to the live graphics engine.  Normalize-but-don't-persist
// so a temporarily disconnected monitor keeps its remembered name.
void LoadAndApplyDisplayConfig()
{
    if (!GEngine)
        return;

    const std::string path = DisplayConfigPath();
    DisplayConfig cfg;
    if (!cfg.Load(path))
    {
        cfg.LoadDefaults();
        cfg.Save(path);
        LOG_INFO(Graphics, "LoadDisplayConfig: created defaults at '{}'", path);
    }

    LiveDisplayEnv env(GEngine);
    if (cfg.Normalize(env))
        LOG_INFO(Graphics, "LoadDisplayConfig: normalized invalid fields (not persisted)");

    DisplayStartupOverrides overrides = GetCliDisplayOverrides(AppConfig::Instance());
    if (ApplyDisplayStartupOverrides(cfg, overrides))
        LOG_INFO(Graphics, "LoadDisplayConfig: applied explicit CLI display overrides");

    // Apply order matters: monitor first (window may move),
    // window mode next, resolution + refresh rate last.
    if (cfg.monitor != GEngine->GetCurrentMonitor())
        GEngine->SwitchMonitor(cfg.monitor);

    // Honour the --window CLI override — when the user/test runner
    // explicitly asked for Windowed, applying the cfg's Borderless or
    // Fullscreen via SetWindowMode would route through
    // SDL_SetWindowFullscreen(true), which fires WINDOW_ENTER_FULLSCREEN
    // and flips the engine's _windowed back to false.  Tests that boot
    // with --window then assert IsWindowed() would race-fail.
    const auto effectiveWindowMode =
        ENGINE_CONFIG.useWindow ? DisplayConfig::Windowed : static_cast<DisplayConfig::WindowMode>(cfg.windowMode);
    GEngine->SetWindowMode(static_cast<WindowMode>(effectiveWindowMode));
    if (cfg.resolutionWidth > 0 && cfg.resolutionHeight > 0)
        GEngine->SwitchRes(cfg.resolutionWidth, cfg.resolutionHeight, /*bpp*/ 32);
    if (cfg.refreshRate > 0 && effectiveWindowMode == DisplayConfig::Fullscreen)
        GEngine->SwitchRefreshRate(cfg.refreshRate);
    ApplyAspectPolicy(cfg);

    LOG_DEBUG(Graphics, "LoadDisplayConfig: monitor={} mode={} res={}x{} refresh={}", cfg.monitor, (int)cfg.windowMode,
              cfg.resolutionWidth, cfg.resolutionHeight, cfg.refreshRate);
}

// GraphicsConfig::Environment impl — only consults system RAM today.
struct LiveGraphicsEnv : GraphicsConfig::Environment
{
    int GetSystemRamMB() const override { return SDL_GetSystemRAM(); }
};

std::string GraphicsConfigPath()
{
    return GamePaths::Instance().UserDir() + "graphics.cfg";
}

// Apply a renderer default without overriding a value the caller already set.
//
// This used to overwrite unconditionally, which silently disabled every WGR_*
// switch the renderer documents: the process set its own profile before renderer
// creation read the variables, so `WGR_GPU_WATER=0` (and MSAA, HDR, prepass,
// shadow maps, …) had no effect at all. That is not a hypothetical — it is why
// water could not be turned off to satisfy WTR-GATE-1's "water can be disabled
// independently", and it makes the dev panel's own hint
// ("run the wgpu backend with WGR_GPU_WATER") impossible to act on.
//
// Defaults still apply when the caller says nothing, so the shipped profile is
// unchanged.
void SetRendererEnvironmentDefault(const char* name, const char* value)
{
    if (const char* existing = std::getenv(name); existing && *existing)
        return;
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void ConfigureWgpuUltraEnvironment()
{
    // Renderer creation reads these once. Keep the process-level profile explicit
    // while the in-game Graphics page remains unavailable — but as defaults, so an
    // explicit environment override still wins.
    SetRendererEnvironmentDefault("WGR_HDR", "1");
    SetRendererEnvironmentDefault("WGR_MSAA", "4");
    SetRendererEnvironmentDefault("WGR_PREPASS", "1");
    SetRendererEnvironmentDefault("WGR_INDIRECT", "1");
    SetRendererEnvironmentDefault("WGR_GPU_DRIVEN", "1");
    SetRendererEnvironmentDefault("WGR_GPU_WATER", "1");
    SetRendererEnvironmentDefault("WGR_WATER_FFT", "1");
    SetRendererEnvironmentDefault("WGR_SHADOW_MAPS", "1");
}

// Eager-write defaults (autodetected) if the file is missing, then
// apply the cfg values to the live engine.  Normalize-but-don't-
// persist mirrors AudioConfig + DisplayConfig.
void LoadAndApplyGraphicsConfig()
{
    const std::string path = GraphicsConfigPath();
    const int refreshHz = GEngine ? GEngine->RefreshRate() : 0;
    GraphicsConfig cfg;
    if (!cfg.Load(path))
    {
        // First boot autodetects from hardware.  vsync / brightness / gamma
        // keep their class-default values.
        cfg.LoadDefaults();
        const int ramMB = SDL_GetSystemRAM();
        cfg.qualityPreset = GraphicsConfig::PickPresetFromRam(ramMB);
        cfg.ApplyPresetToTiers(cfg.qualityPreset);
        cfg.fpsCap = GraphicsConfig::FpsCapForRefreshRate(refreshHz);
        cfg.Save(path);
        LOG_INFO(Graphics,
                 "LoadGraphicsConfig: autodetected preset={} (RAM={} MB) fpsCap={} (display {} Hz), wrote '{}'",
                 (int)cfg.qualityPreset, ramMB, cfg.fpsCap, refreshHz, path);
    }

    if (cfg.Migrate(refreshHz) && cfg.Save(path))
        LOG_INFO(Graphics, "LoadGraphicsConfig: migrated '{}' to version {} (fpsCap={}, display {} Hz)", path,
                 GraphicsConfig::kVersion, cfg.fpsCap, refreshHz);

    LiveGraphicsEnv env;
    if (cfg.Normalize(env))
        LOG_INFO(Graphics, "LoadGraphicsConfig: normalized invalid fields (not persisted)");

    // The saved config is applied as saved.
    //
    // This used to force the Ultra bundle + MSAA 4x + renderScale 1.0 here on EVERY
    // boot, which made every quality row inert: the Graphics page wrote a value, the
    // next launch discarded it, and the first-boot PickPresetFromRam autodetect was
    // written to disk and then overridden three lines later in this same function.
    // The comment justifying it ("the in-game Graphics page is currently unavailable")
    // had outlived the condition -- GraphicsPage is mounted, tested, and reachable.
    //
    // Nobody's picture changes: GraphicsConfig::Migrate stamps that exact bundle into
    // any file below v3, so an existing install carries forward what it was already
    // being shown and starts honouring the file from the next change onward.
    ApplyGraphicsConfigToEngine(cfg);

    LOG_DEBUG(Graphics,
              "LoadGraphicsConfig: preset={} terrain={} objectLod={} shadow={} particles={} "
              "grass={} vsync={} fpsCap={} brightness={} gamma={}",
              (int)cfg.qualityPreset, (int)cfg.terrainDetail, (int)cfg.objectLod, (int)cfg.shadowQuality,
              (int)cfg.particlesQuality, (int)cfg.grassQuality, (int)cfg.vsync, cfg.fpsCap, cfg.brightness,
              cfg.gamma);
}
} // namespace

#include <sstream>

#ifndef _WIN32
#include <unistd.h>
#include <cstdio>
#endif
#include <SDL3/SDL.h>

#include <Poseidon/Core/Application.hpp>

// Game-side harness query handler — network/world/mission state. UI-level
// queries (display/controls) belong to PoseidonUITest. Network targets
// (players/mission/ngs) live in HarnessBuiltins; `world` is Game-only.
static std::string HarnessHandleQuery(const char* what, cJSON* root)
{
    if (what && std::strcmp(what, "world") == 0)
    {
        cJSON* resp = cJSON_CreateObject();
        cJSON_AddNumberToObject(resp, "mode", GWorld ? static_cast<int>(GWorld->GetMode()) : -1);
        return HarnessProtocol::JsonResponse(resp);
    }
    std::string network = HarnessBuiltins::AnswerNetworkQuery(what);
    if (!network.empty())
        return network;
    std::string service = HarnessBuiltins::AnswerServiceQuery(what, root);
    if (!service.empty())
        return service;
    return HarnessProtocol::ErrorResponse("unknown query target");
}

// Auto-key / auto-screenshot scheduler. Trident parity + flicker scripts
// ship specs like "700:path" (frame-based) or "2.5s:path" (time-based);
// TimedTrigger stores both and fires once when either condition matches.
struct TimedTrigger
{
    int frame = -1;
    int timeMs = -1;
    bool fired = false;
};

// Parse the trigger portion of a spec token (everything before the first ':').
// "700" → {frame=700, timeMs=-1}; "2.5s" → {frame=-1, timeMs=2500}.
static TimedTrigger ParseTriggerTime(const std::string& spec, size_t colonPos)
{
    std::string timeStr = spec.substr(0, colonPos);
    if (!timeStr.empty() && (timeStr.back() == 's' || timeStr.back() == 'S'))
    {
        float secs = std::stof(timeStr.substr(0, timeStr.size() - 1));
        return {-1, static_cast<int>(secs * 1000.0f), false};
    }
    return {std::stoi(timeStr), -1, false};
}

// Returns true once per trigger, when either frame or elapsed time matches.
static bool TriggerReady(TimedTrigger& t, int frame, uint32_t elapsedMs)
{
    if (t.fired)
        return false;
    if (t.frame >= 0 && frame == t.frame)
    {
        t.fired = true;
        return true;
    }
    if (t.timeMs >= 0 && static_cast<int>(elapsedMs) >= t.timeMs)
    {
        t.fired = true;
        return true;
    }
    return false;
}

struct AutoKeyEvent
{
    TimedTrigger trigger;
    SDL_Scancode scancode;
    SDL_Keymod mod;
};

struct AutoScreenshotSpec
{
    TimedTrigger trigger;
    std::string path;
};

// Parse --auto-keys ("trigger:scancode[:modflags],..."). Missing-colon tokens skipped.
static std::vector<AutoKeyEvent> ParseAutoKeys(const std::string& spec)
{
    std::vector<AutoKeyEvent> out;
    if (spec.empty())
        return out;
    std::istringstream ss(spec);
    std::string token;
    while (std::getline(ss, token, ','))
    {
        auto colon1 = token.find(':');
        if (colon1 == std::string::npos)
            continue;
        TimedTrigger trig = ParseTriggerTime(token, colon1);
        std::string rest = token.substr(colon1 + 1);
        auto colon2 = rest.find(':');
        int sc;
        SDL_Keymod mod = SDL_KMOD_NONE;
        if (colon2 != std::string::npos)
        {
            sc = std::stoi(rest.substr(0, colon2));
            mod = (SDL_Keymod)std::stoi(rest.substr(colon2 + 1));
        }
        else
        {
            sc = std::stoi(rest);
        }
        out.push_back({trig, (SDL_Scancode)sc, mod});
    }
    if (!out.empty())
        LOG_INFO(Core, "Auto-keys: {} events scheduled", out.size());
    return out;
}

// Parse --auto-screenshot ("trigger:path,...").
static std::vector<AutoScreenshotSpec> ParseAutoScreenshots(const std::string& spec)
{
    std::vector<AutoScreenshotSpec> out;
    if (spec.empty())
        return out;
    std::istringstream ss(spec);
    std::string token;
    while (std::getline(ss, token, ','))
    {
        auto colon = token.find(':');
        if (colon == std::string::npos)
            continue;
        out.push_back({ParseTriggerTime(token, colon), token.substr(colon + 1)});
    }
    if (!out.empty())
        LOG_INFO(Core, "Auto-screenshot: {} captures scheduled", out.size());
    return out;
}

// Build the harness server for PoseidonGame. Consolidates the full setup —
// bind, VoN callback wiring, command + event catalogue — so both the Windows
// and Linux main loops get the same registrations without duplicating ~100
// lines of lambdas. Returns null if no --harness-port was requested.
static std::unique_ptr<HarnessServer> CreateGameHarness()
{
    const int harnessPort = AppConfig::Instance().GetHarnessPort();
    if (harnessPort < 0)
        return nullptr;

    auto hs = std::make_unique<HarnessServer>();
    if (!hs->Start(harnessPort))
    {
        LOG_ERROR(Core, "Failed to start harness server on port {}", harnessPort);
        return nullptr;
    }

    auto* raw = hs.get();
    Poseidon::gVonReceiveCallback = [raw](uint32_t channel, int frames)
    { raw->PushEvent(HarnessProtocol::VonReceivedEvent(channel, frames)); };

    HarnessBuiltins::RegisterScreenshot(*hs);
    HarnessBuiltins::RegisterSqf(*hs);
    HarnessBuiltins::RegisterHttpFixtures(*hs);
    // Multiplayer PTT tests (triHoldKey / triReleaseKey) drive VoN
    // transmission via these commands.
    HarnessBuiltins::RegisterKeyInjection(*hs);

    hs->RegisterCommand({"query",
                         "Query game state (what: players, mission, ngs, world, master_server_server_detail, "
                         "master_server_mod_detail, master_server_mod_versions, master_server_mod_servers)",
                         {{"what", "string", true}}},
                        [](const std::string&, cJSON* root) -> std::string
                        { return HarnessHandleQuery(HarnessProtocol::GetString(root, "what"), root); });

    hs->RegisterEvent({"ready", "Game initialized, first display shown", {{"idd", "int"}}});
    hs->RegisterEvent({"display", "Active display changed", {{"idd", "int"}, {"name", "string"}}});
    hs->RegisterEvent({"player_joined", "Player connected", {{"dpid", "int"}, {"name", "string"}}});
    hs->RegisterEvent({"player_left", "Player disconnected", {{"dpid", "int"}, {"name", "string"}}});
    hs->RegisterEvent({"mission_state", "Server game state transition", {{"state", "string"}, {"prev", "string"}}});
    hs->RegisterEvent({"von_received", "Voice data received", {{"sender", "int"}, {"frames", "int"}}});
    return hs;
}

#ifdef _WIN32
#include <SDL3/SDL.h>

int GameApplication::Run(HINSTANCE hInstance, LPSTR commandLine, int showCmd)
{
    m_hInstance = hInstance;
    m_showCmd = showCmd;

    extern int __argc;
    extern char** __argv;
    int rc = RunBootstrap(__argc, __argv, commandLine);
    if (rc >= 0)
        return rc;

    return RunAfterArgumentParsing();
}
#endif

int GameApplication::Run(const char* commandLine)
{
    // Bridge for non-Windows (not yet fully implemented)
    return 0;
}

#ifndef _WIN32
int GameApplication::Run(int argc, char** argv)
{
    int rc = RunBootstrap(argc, argv, nullptr);
    if (rc >= 0)
        return rc;

    return RunAfterArgumentParsing();
}
#endif

int GameApplication::RunAfterArgumentParsing()
{
    LOG_INFO(Core, "Game starting: version {}", (const char*)GetVersionString());

    constexpr const char* kStartupErrorTitle = "Cold War Assault - Startup Error";

    if (!ReadConfiguration())
    {
        Poseidon::Foundation::ShowStartupError(
            kStartupErrorTitle, "Failed to load the game configuration.\nThe game data may be missing or invalid.");
        return 1;
    }

    ConfigureWgpuUltraEnvironment();

    if (!InitializeGraphicsEngine())
    {
        Poseidon::Foundation::ShowStartupError(kStartupErrorTitle, "Failed to initialize the graphics engine.");
        return 1;
    }

    if (!CreateAndSetGraphicsEngine())
    {
        Poseidon::Foundation::ShowStartupError(kStartupErrorTitle, "Failed to create the graphics engine.");
        return 1;
    }

    // Load display.cfg (eager-write defaults if missing) and apply
    // the persisted monitor / window-mode / resolution / refresh-rate
    // to the engine.  Mirrors GSoundsys->LoadConfig — same shape.
    LoadAndApplyDisplayConfig();
    // Same shape for graphics.cfg, but with autodetect-from-RAM on
    // first boot since most graphics quality knobs benefit from
    // sensible defaults out of the box.
    LoadAndApplyGraphicsConfig();

    if (!InitializeWorld())
    {
        Poseidon::Foundation::ShowStartupError(kStartupErrorTitle, "Failed to initialize the game world.");
        return 1;
    }

    // Scene-owned tier settings (terrain grid, object LOD, shadows and cloudlets)
    // need a live world. Reapply the forced Ultra bundle after InitializeWorld.
    LoadAndApplyGraphicsConfig();

    if (!InitializeSound())
    {
        Poseidon::Foundation::ShowStartupError(kStartupErrorTitle, "Failed to initialize sound.");
        return 1;
    }

    if (!InitializeSubsystems())
    {
        Poseidon::Foundation::ShowStartupError(kStartupErrorTitle, "Failed to initialize a game subsystem.");
        return 1;
    }

    ProgressFinish();
    EnableRendering();
    VerifySerialKey();

    if (ENGINE_CONFIG.checkInitAndExit && !AppConfig::Instance().IsMissionSmokeCheck())
    {
        LOG_INFO(Core, "Initialization check complete - exiting");
        if (Poseidon::Foundation::LoggingSystem::StrictTripped())
        {
            LOG_WARN(Core, "strict mode: ERROR logged during initialization check is fatal - exiting with code 3");
            return 3;
        }
#ifdef _WIN32
        ExitProcess(0); // bypass static destructors and CRT assertions
#else
        return 0;
#endif
    }

    if (AppConfig::Instance().IsMissionSmokeCheck())
    {
        LOG_INFO(Core, "Mission smoke check: initialization complete, continuing into mission startup");
    }

    StartGameMode();
    FinalizeInitialization();

    // Boot fully, perform one in-process reload, verify the engine and world came back, then exit.
    if (AppConfig::Instance().IsRemountSelfTest())
    {
        LOG_INFO(Core, "Re-mount self-test: performing one in-process reload");
        const bool reloaded = ReloadGameContent();
        const bool alive = reloaded && GWorld != nullptr && GEngine != nullptr;
        LOG_INFO(Core, "Re-mount self-test {}", alive ? "passed" : "FAILED");
        return alive ? 0 : 1;
    }

    if (AppConfig::Instance().IsErrorResilienceSelfTest())
    {
        LOG_INFO(Core, "Error-resilience self-test: raising a simulated critical error");
        Poseidon::Foundation::ErrorMessage("error-resilience self-test: simulated critical error");
        LOG_INFO(Core, "Error-resilience self-test passed (ErrorMessage survived under --no-strict)");
        return 0;
    }

    // Repeatedly re-mount bare <-> --mod and verify the engine, world, and content counts survive every cycle.
    if (AppConfig::Instance().IsModCycleSelfTest())
    {
        const RString modPath = Poseidon::ModSystem::GetModList();
        if (modPath.GetLength() == 0)
        {
            LOG_ERROR(Core, "Mod-cycle self-test requires --mod (the mod to re-mount)");
            return 1;
        }
        LOG_INFO(Core, "Mod-cycle self-test: repeated bare <-> mod re-mounts (mod='{}')", (const char*)modPath);

        // Each addon contributes a CfgPatches entry, so the count tracks mounted content.
        // Assert it rises with the mod and returns *exactly* to the bare baseline each time
        // — proving the mod loads from scratch and fully unmounts (no content left behind).
        const char* phases[] = {"", (const char*)modPath, "", (const char*)modPath, ""};
        const int nPhases = (int)(sizeof(phases) / sizeof(*phases));
        int bareBaseline = -1;
        bool ok = true;
        for (int i = 0; i < nPhases && ok; i++)
        {
            ok = Remount(phases[i]) && GWorld != nullptr && GEngine != nullptr;
            const bool bare = phases[i][0] == 0;
            const int patches = (Pars >> "CfgPatches").GetEntryCount();
            if (ok && bare && bareBaseline < 0)
                bareBaseline = patches; // first bare phase establishes the baseline
            else if (ok && bare && patches != bareBaseline)
                ok = false; // mod content was not fully unmounted on reload-to-bare
            else if (ok && !bare && patches <= bareBaseline)
                ok = false; // mod failed to mount its content
            LOG_INFO(Core, "  re-mount {}/{} (mod='{}') alive={} CfgPatches={}", i + 1, nPhases,
                     bare ? "(bare)" : phases[i], ok, patches);
        }
        LOG_INFO(Core, "Mod-cycle self-test {}", ok ? "passed" : "FAILED");
        return ok ? 0 : 1;
    }

    // Reload-leaves-clean-state smoke test: the intro cutscene loads real content
    // (terrain, vehicles, their meshes/textures). Re-mount repeatedly and assert the
    // loaded-content metrics return to a stable baseline every cycle — if the shape
    // cache, vehicle-type bank, or heap grows reload-over-reload, something loaded is
    // not being released by UnloadGameData/Globals::Clear.
    if (AppConfig::Instance().IsReloadCleanSelfTest())
    {
        struct Snapshot
        {
            int shapes;
            int vtypes;
            int ntex;
            size_t heapKB;
        };
        auto snap = []()
        {
            int shapes = 0;
            Shapes.ForEach([&](Poseidon::LODShapeWithShadow&) { shapes++; });
            const int ntex = GEngine ? GEngine->TextBank()->NTextures() : 0;
            return Snapshot{shapes, VehicleTypes.Size(), ntex, Poseidon::Foundation::MemoryUsed() / 1024};
        };

        LOG_INFO(Core, "Reload-clean self-test: capturing baseline + 4 re-mounts");
        if (!ReloadGameContent() || GWorld == nullptr) // settle into reload steady state
        {
            LOG_INFO(Core, "Reload-clean self-test FAILED (initial reload)");
            return 1;
        }
        const Snapshot base = snap();
        LOG_INFO(Core, "  baseline: shapes={} vtypes={} ntex={} heap={}KB", base.shapes, base.vtypes, base.ntex,
                 base.heapKB);

        bool ok = true;
        for (int i = 0; i < 4 && ok; i++)
        {
            ok = ReloadGameContent() && GWorld != nullptr && GEngine != nullptr;
            const Snapshot s = snap();
            // Content banks and the texture-bank cache must match the baseline exactly
            // (deterministic intro). Heap can wobble with allocator fragmentation, so
            // bound it rather than equate.
            const bool banksStable = s.shapes == base.shapes && s.vtypes == base.vtypes && s.ntex == base.ntex;
            const bool heapStable = s.heapKB <= base.heapKB + base.heapKB / 10 + 1024; // +10% / +1MB slack
            ok = ok && banksStable && heapStable;
            LOG_INFO(Core, "  re-mount {}/4: shapes={} vtypes={} ntex={} heap={}KB {}", i + 1, s.shapes, s.vtypes,
                     s.ntex, s.heapKB, (banksStable && heapStable) ? "stable" : "GREW");
        }
        LOG_INFO(Core, "Reload-clean self-test {}", ok ? "passed" : "FAILED");
        return ok ? 0 : 1;
    }

    // Exercise the deferred dev-panel reload path while the menu world keeps simulating.
    if (AppConfig::Instance().IsRemountSimSelfTest())
    {
        LOG_INFO(Core, "Re-mount+simulate self-test: pump intro frames, re-mount, pump simulate frames");

        auto pumpFrames = [](int n)
        {
            for (int i = 0; i < n; i++)
            {
                Poseidon::AppIdle();
            }
        };
        // GWorld->GetSensorList() must be live before World::Simulate dereferences it.
        auto sensorListAlive = []() -> bool { return GWorld != nullptr && GWorld->GetSensorList() != nullptr; };

        // Alternate the mounted set each cycle so we exercise reload-to-mod and reload-to-bare,
        // each followed by real simulate frames. modPath is the --mod value (empty if none), so
        // with `--mods-dir packages/mods --mod @x` this reproduces the dev-panel "reload with a
        // mod selected" flow; without --mod it degrades to repeated bare reloads.
        const RString modPath = Poseidon::ModSystem::GetModList();
        const char* phases[] = {(const char*)modPath, "", (const char*)modPath, ""};
        const int nPhases = (int)(sizeof(phases) / sizeof(*phases));

        bool ok = true;
        // Let the intro fully load and simulate before the reload cycle.
        pumpFrames(180);
        for (int cycle = 0; cycle < nPhases && ok; cycle++)
        {
            const std::string mod = phases[cycle];
            Poseidon::Dev::DebugOverlay::RequestDeferredReload(mod.c_str());
            // Pump real World::Simulate frames on the re-mounted world.
            pumpFrames(60);
            if (GWorld == nullptr)
            {
                LOG_ERROR(Core, "Re-mount+sim self-test: GWorld null after reload (cycle {}, mod='{}')", cycle,
                          mod.c_str());
                ok = false;
                break;
            }
            LOG_INFO(Core, "  cycle {}: re-mounted (mod='{}'), sensorList={}", cycle, mod.c_str(),
                     static_cast<const void*>(GWorld->GetSensorList()));
            if (!sensorListAlive())
            {
                LOG_ERROR(Core, "Re-mount+sim self-test: GetSensorList() went null after deferred reload (cycle {})",
                          cycle);
                ok = false;
                break;
            }
        }
        LOG_INFO(Core, "Re-mount+simulate self-test {}", ok ? "passed" : "FAILED");
        return ok ? 0 : 1;
    }

    // Force a reload failure, then verify rollback leaves a live rendering world.
    if (AppConfig::Instance().IsRemountFailSelfTest())
    {
        LOG_INFO(Core, "Re-mount-fail self-test: force a failed reload, then pump frames");
        auto pumpFrames = [](int n)
        {
            for (int i = 0; i < n; i++)
                Poseidon::AppIdle();
        };

        // Warm up so the menu world is fully loaded and simulating.
        pumpFrames(180);

        s_forceRemountReloadFailOnce = true; // next reload fails like a bad / unsupported mod
        // GLandscape proves the rollback rebuilt terrain before rendering resumes.
        const bool reportedFailure = !ReloadGameContent();
        const bool errorReported = GApp->m_remountFailed; // set by Remount; AppIdle clears it as it shows the message
        pumpFrames(60);                                   // the crash window: real frames over the recovered world

        const bool alive = GWorld != nullptr && GEngine != nullptr && GLandscape != nullptr && GApp->m_canRender;
        const bool ok = reportedFailure && errorReported && alive;
        LOG_INFO(Core, "Re-mount-fail self-test {} (reportedFailure={} errorReported={} GLandscape={} canRender={})",
                 ok ? "passed" : "FAILED", reportedFailure, errorReported, static_cast<const void*>(GLandscape),
                 GApp->m_canRender);
        return ok ? 0 : 1;
    }

    RunMainLoop();

    return m_exitCode;
}

bool GameApplication::InitializeSubsystems()
{
    // Enable PIII FPU optimizations if configured
    if (ENGINE_CONFIG.enablePIII)
    {
        extern void SetFlushToZero();
        SetFlushToZero();
    }

    // FontSystem must be up before any UI / overlay draws — every Font
    // load short-circuits to empty until Initialize succeeds.
    FontSystem::Instance().Initialize();

    // ScenePreloader populates Scene._preloaded[] from CfgScenePreload.
    // Apps that skip this get null slots and the rendering paths' guards
    // short-circuit the affected features (cloudlets, craters, etc.).
    if (GScene)
        ScenePreloader::Instance().Initialize(*GScene);

    return true;
}

void GameApplication::PollStrictAbort()
{
    // Exit code 3 is a strict-mode abort; exit code 2 is reserved for auto-test script errors.
    if (m_closeRequest || !Poseidon::Foundation::LoggingSystem::StrictTripped())
        return;
    LOG_WARN(Core, "strict mode: ERROR logged above is fatal — requesting clean shutdown (exit 3)");
    if (m_exitCode == 0)
        m_exitCode = 3;
    m_closeRequest = true;
}

void GameApplication::RunMainLoop()
{
    // Wait for startup progress script (startup.sqs) to complete.
    // Dedicated servers skip this — the dummy engine can't advance the script.
    if (!ENGINE_CONFIG.doCreateDedicatedServer)
    {
        // I_AM_ALIVE() triggers GlobalAliveImplementation::Alive() → ProgressSystem::Refresh() → Draw() →
        // ProgressScript->Simulate()
        while (Poseidon::ProgressScript)
        {
            I_AM_ALIVE();
            if (Poseidon::ProgressScript && Poseidon::ProgressScript->IsTerminated())
            {
                Poseidon::ProgressScript.Free();
            }
        }
    }
    else if (Poseidon::ProgressScript)
    {
        Poseidon::ProgressScript.Free();
    }
    GWorld->SetTitleEffect(nullptr);
    GWorld->SetCutEffect(nullptr);

    // Screenshot mode (menu only, no --test-mission): render a few frames for menu to settle, capture, then exit
    const std::string& screenshotPath = AppConfig::Instance().GetScreenshotPath();
    const bool screenshotTestMode =
        AppConfig::Instance().IsScreenshotTest() && !AppConfig::Instance().GetTestMissionPath().empty();
    if (!screenshotPath.empty() && !screenshotTestMode)
    {
        for (int i = 0; i < 5; i++)
        {
            m_forceRender = true;
            Poseidon::AppIdle();
        }
        GEngine->Screenshot(screenshotPath.c_str());
        m_forceRender = true;
        Poseidon::AppIdle(); // renders frame that captures screenshot before swap
        const std::string& captureMetricsPath = AppConfig::Instance().GetCaptureMetricsPath();
        if (!captureMetricsPath.empty())
            LOG_INFO(Core, "Capture metrics {}: {}", WriteCaptureMetrics(captureMetricsPath) ? "saved" : "failed",
                     captureMetricsPath);
        LOG_INFO(Core, "Screenshot saved to: {}", screenshotPath);
        _exit(0);
    }

    // Screenshot test mode (--test-mission + --test-type screenshot):
    // Wait for mission to enter gameplay (GModeArcade), render frames, capture, exit
    const int screenshotDelay = AppConfig::Instance().GetScreenshotDelay();
    int screenshotFrameCount = 0;
    bool screenshotCaptured = false;
    std::string screenshotTestPath;
    // --test-model rides on the mission screenshot mode: the model needs a world
    // to stand in, and that mode already waits for one.
    const std::vector<std::string>& testModelPaths = AppConfig::Instance().GetTestModelPaths();
    const bool spawnTestModel = !testModelPaths.empty();
    bool testModelSpawned = false;
    bool testWorldRepositioned = false;
    if (spawnTestModel && !screenshotTestMode)
        LOG_WARN(Core, "--test-model needs --test-mission to have a world to place the model in");

    if (screenshotTestMode)
    {
        screenshotTestPath = screenshotPath.empty() ? "tmp/test_screenshot.png" : screenshotPath;
        std::filesystem::create_directories(std::filesystem::path(screenshotTestPath).parent_path());
        LOG_INFO(Core, "Screenshot test mode: waiting for mission to enter gameplay...");
    }

#ifdef _WIN32
    const bool benchmarkMode = AppConfig::Instance().BenchmarkMode();
    const int benchmarkMaxFrames = 300; // ~10s at 30fps
    int benchmarkFrameCount = 0;
    DWORD benchmarkStartTick = 0;
    DWORD benchmarkLastLogTick = 0;
    int benchmarkLastLogFrame = 0;

    std::vector<AutoScreenshotSpec> autoScreenshots = ParseAutoScreenshots(AppConfig::Instance().GetAutoScreenshot());
    std::vector<AutoKeyEvent> autoKeyEvents = ParseAutoKeys(AppConfig::Instance().GetAutoKeys());
    // RenderDoc trigger: same trigger format as auto-keys/auto-screenshot
    // (frame number or "<sec>s") but no path — RenderDoc decides where
    // the .rdc file lands.  Trigger.fired=true (inert) when --rdc-trigger
    // wasn't given on the CLI.
    TimedTrigger rdcTrigger{-1, -1, true};
    {
        const std::string& rdcSpec = AppConfig::Instance().GetRdcTrigger();
        if (!rdcSpec.empty())
        {
            rdcTrigger = ParseTriggerTime(rdcSpec + ":", rdcSpec.size());
            rdcTrigger.fired = false;
        }
    }
    int mainFrameCounter = 0;
    size_t nextAutoScreenshot = 0;
    // Screenshot readback is asynchronous in WGPU. Keep two ordinary game
    // frames alive after requesting it; otherwise the shutdown screen becomes
    // the captured frame.
    int screenshotCloseAfterFrame = -1;
    DWORD loopStartTick = GetTickCount();

    // Display tracker — Trident clients wait for `ready` before issuing
    // commands and consume `display` events for navigation. Not dump/record.
    UITestEngine displayTracker;
    displayTracker.SetFrameCounter(&mainFrameCounter);
    bool harnessReadySent = false;

    // Harness server for Trident-driven game tests (SQF eval/exec, network/
    // mission queries, player tracking). UI-level commands (click, query=
    // display, wait_display) live in PoseidonUITest.
    std::unique_ptr<HarnessServer> harnessServer = CreateGameHarness();
    HarnessPlayerTracker harnessPlayerTracker;
    HarnessMissionStateTracker harnessMissionStateTracker;

    while (!m_closeRequest)
    {
        GDebugger.ProcessAlive(); // keep watchdog thread happy
        Poseidon::AppIdle();      // simulate + render one frame

        PollStrictAbort(); // --strict: stop on any ERROR logged this frame

        // Poll SDL events (input, focus, resize, close)
        if (GEngine)
        {
            GEngine->HandleEvents();
            if (!GEngine->IsOpen())
                break;
        }

        // Inject auto-key events via SDL (scancodes)
        DWORD elapsedMs = GetTickCount() - loopStartTick;
        for (auto& ak : autoKeyEvents)
        {
            if (TriggerReady(ak.trigger, mainFrameCounter, elapsedMs))
            {
                SDL_Event ev = {};
                ev.type = SDL_EVENT_KEY_DOWN;
                ev.key.scancode = ak.scancode;
                ev.key.mod = ak.mod;
                SDL_PushEvent(&ev);
                ev.type = SDL_EVENT_KEY_UP;
                SDL_PushEvent(&ev);
                LOG_INFO(Core, "Auto-key injected: frame={} t={:.1f}s sc={}", mainFrameCounter, elapsedMs / 1000.0f,
                         (int)ak.scancode);
            }
        }

        // Benchmark FPS tracking (only in arcade/gameplay mode)
        if (benchmarkMode && GWorld->GetMode() == GModeArcade)
        {
            if (benchmarkFrameCount == 0)
            {
                benchmarkStartTick = GetTickCount();
                benchmarkLastLogTick = benchmarkStartTick;
                GTerrainProfile.Reset();
            }
            benchmarkFrameCount++;
            DWORD now = GetTickCount();
            DWORD sinceLast = now - benchmarkLastLogTick;
            if (sinceLast >= 1000)
            {
                int framesSinceLast = benchmarkFrameCount - benchmarkLastLogFrame;
                float intervalFps = framesSinceLast * 1000.0f / sinceLast;
                float totalElapsed = (now - benchmarkStartTick) / 1000.0f;
                float avgFps = benchmarkFrameCount * 1000.0f / (now - benchmarkStartTick);
                auto& tp = GTerrainProfile;
                LOG_INFO(Core,
                         "BENCHMARK: t={:.1f}s frame={} iFPS={:.1f} aFPS={:.1f}"
                         " | seg={} hit={} miss={} steps={} avgStep={:.0f}"
                         " | ground={:.0f}Mc genSeg={:.0f}Mc",
                         totalElapsed, benchmarkFrameCount, intervalFps, avgFps, tp.segmentsDrawn, tp.segmentsCacheHit,
                         tp.segmentsCacheMiss, tp.cacheSearchSteps,
                         tp.segmentsDrawn > 0 ? (double)tp.cacheSearchSteps / tp.segmentsDrawn : 0.0,
                         tp.drawGroundCycles / 1e6, tp.generateSegCycles / 1e6);
                tp.Reset();
                benchmarkLastLogTick = now;
                benchmarkLastLogFrame = benchmarkFrameCount;
            }
            if (benchmarkFrameCount >= benchmarkMaxFrames)
            {
                DWORD elapsed = GetTickCount() - benchmarkStartTick;
                float avgFps = benchmarkFrameCount * 1000.0f / (elapsed > 0 ? elapsed : 1);
                LOG_INFO(Core, "BENCHMARK RESULT: {} frames in {:.1f}s = {:.1f} avg FPS", benchmarkFrameCount,
                         elapsed / 1000.0f, avgFps);
                m_closeRequest = true;
            }
        }

        // Screenshot test: capture after configured delay frames
        // A --test-mission screenshot is evidence for the mission, not the
        // menu/intro transition.  Waiting for GModeIntro here could capture a
        // valid PNG before staged mission startup had reached gameplay.
        // --test-model: load one P3D into a live mission world.
        //
        // This exists because nothing else exercises an arbitrary model through
        // the graphics backend -- PoseidonTools and Studio both preview with the
        // CPU rasteriser in ModelRenderer, so a newly-supported format can be
        // read, converted, and still never proven to draw.
        //
        // Static objects loaded with a mission do not go through createVehicle.
        // Landscape::ObjectCreate loads a shape, creates its Object subclass from
        // the shape's class hint, assigns the transform, and adds it to the
        // landscape grid. This matters for external models: a valid Arma 3 rock
        // has no CWA CfgVehicles entry, so wrapping it in the generic "Building"
        // vehicle type is not the lifecycle a normal static world object follows.
        // --test-world-pos: a mission written for one world starts the player at
        // coordinates that mean nothing in another. The OFP training start is
        // open sea on Stratis, so without this the first A3 world render is a
        // picture of water.
        const std::vector<float>& testWorldFreeFly = AppConfig::Instance().GetTestWorldFreeFly();
        const std::vector<float>& testWorldPos = AppConfig::Instance().GetTestWorldPos();
        if (!testWorldRepositioned && (!testWorldFreeFly.empty() || !testWorldPos.empty()) &&
            GWorld->GetMode() == GModeArcade)
        {
            testWorldRepositioned = true;
            const bool freeFlyPose = !testWorldFreeFly.empty();
            const std::vector<float>& target = freeFlyPose ? testWorldFreeFly : testWorldPos;
            const float testWorldHour = AppConfig::Instance().GetTestWorldHour();
            if (testWorldHour >= 0.0f)
            {
                const int hour = static_cast<int>(testWorldHour);
                const int minute = static_cast<int>((testWorldHour - hour) * 60.0f + 0.5f);
                GWorld->SetDate(1985, 5, 11, hour, minute);
                LOG_INFO(Core, "--test-world-hour: set world time to {:02d}:{:02d}", hour, minute);
            }
            Object* cameraOn = GWorld->CameraOn();
            if (!freeFlyPose && (!cameraOn || !GLandscape))
            {
                LOG_ERROR(Core, "--test-world-pos: no camera or landscape to reposition against");
            }
            else
            {
                const float x = target[0];
                const float z = target[1];
                const std::vector<float>& lookAt = AppConfig::Instance().GetTestWorldLookAt();
                const float yawDegrees = AppConfig::Instance().GetTestWorldYaw();
                if (freeFlyPose)
                {
                    // A high player would fall before a delayed capture. Use the
                    // same native CameraVehicle as the developer's free-fly tool:
                    // it is an independent manual camera effect, so it preserves
                    // the captured pose without moving or disturbing the player.
                    const Vector3 position(x, target[2], z);
                    Vector3 direction;
                    if (!lookAt.empty())
                    {
                        direction = (Vector3(lookAt[0], lookAt[2], lookAt[1]) - position).Normalized();
                    }
                    else
                    {
                        const float azimuth = target[3] * (H_PI / 180.0f);
                        const float elevation = target[4] * (H_PI / 180.0f);
                        const float horizontal = std::cos(elevation);
                        direction = Vector3(horizontal * std::sin(azimuth), std::sin(elevation),
                                            horizontal * std::cos(azimuth));
                    }
                    auto* camera = new CameraVehicle();
                    camera->SetPosition(position);
                    camera->SetDirectionAndUp(direction, VUp);
                    camera->SetManual(true);
                    camera->SetAltitudeSpeedScaling(true);
                    camera->SetMouseLookRequiresRightButton(true);
                    camera->SetCrossHairs(false);
                    camera->ResetTargets();
                    GWorld->AddAnimal(camera);
                    GWorld->SetCameraEffect(CreateCameraEffect(camera, "Internal", CamEffectTop, true));
                    // A camera effect is a cutscene as far as CameraEffect::Draw is
                    // concerned (WorldImpl.cpp), and `showCinemaBorder` is a global that
                    // defaults to true (WorldSetup.cpp) -- so installing this camera also
                    // switched on the cutscene letterbox: the 4:3-authored CinemaBorder
                    // model's top/bottom bars plus Object::DrawWidescreenPillarbox's black
                    // side bars, which are (w - 4h/3)/2 wide.  On a 16:9 surface that is
                    // 240 px a side at 1920x1080 on top of 127 px top and bottom, so a
                    // capture taken through this camera showed the world in 1440x826 of
                    // the 1920x1080 PNG -- 58% of the pixels -- with the rest a dimmed bar
                    // that reads exactly like a renderer drawing into a sub-rect.  Every
                    // per-pixel measurement taken that way was against the wrong area.
                    //
                    // Zeus already does this (DebugOverlay::EnableZeusCamera): right for a
                    // cutscene, wrong for a developer camera.  Its AdoptFreeFlyCamera hook
                    // below only *saves* the flag, and is compiled out of release builds
                    // and skipped under --no-dev, so the suppression has to live here.
                    //
                    // Ordered after the adopt call on purpose: AdoptFreeFlyCamera snapshots
                    // `showCinemaBorder` so DisableZeusCamera can put it back, and that
                    // snapshot has to be the mission's value, not ours.
                    //
                    // Hand it to the dev panel. Without this the panel cannot see a
                    // camera it did not create, so a command-line flight had neither
                    // a coordinate readout on the Game tab nor "Beam player here".
                    Dev::DebugOverlay::AdoptFreeFlyCamera(camera);
                    ::ShowCinemaBorder(false);
                    LOG_INFO(Core, "--test-world-freefly: cinema border suppressed (developer camera, not a cutscene)");
                    // Put the player on solid ground under the camera.
                    //
                    // The camera above deliberately does not disturb the player, which is
                    // right for the pose but leaves the player wherever the mission put it
                    // -- coordinates authored for a different island. On Sahrani and
                    // Chernarus that is frequently open sea, so the player drowns, and
                    // GModeArcade's end-mission check (WorldImpl: no unit or LSDead ->
                    // EMKilled) ends the mission, which exits the whole app. The session
                    // died about half a minute into a flight with exit code 0 and nothing
                    // in the log resembling a fault, which is a miserable thing to debug.
                    //
                    // Only the position moves; the camera effect still owns the view, so
                    // the captured pose is unchanged.
                    if (cameraOn && GLandscape)
                    {
                        const float groundY = GLandscape->SurfaceY(x, z) + 1.8f;
                        cameraOn->Move(Vector3(x, groundY, z));
                        LOG_INFO(Core,
                                 "--test-world-freefly: grounded the player at {}, {}, {} so a"
                                 " drowning player cannot end the mission",
                                 x, groundY, z);
                    }
                    LOG_INFO(Core,
                             "--test-world-freefly: camera at {}, {}, {}; azimuth {} elevation {} degrees{}", x,
                             target[2], z, target[3], target[4],
                             lookAt.empty() ? "" : " (aimed using --test-world-look-at)");
                }
                else
                {
                    const float y = GLandscape->SurfaceY(x, z) + 1.8f;
                    // Move, rather than SetPosition: the player is already registered
                    // in Landscape's spatial grid. Directly changing the transform left
                    // it in the old cell and caused a recoverable but noisy full-grid
                    // search on every subsequent movement.
                    cameraOn->Move(Vector3(x, y, z));
                    if (!lookAt.empty())
                    {
                        const Vector3 target(lookAt[0], lookAt[2], lookAt[1]);
                        cameraOn->SetOrient((target - cameraOn->Position()).Normalized(), VUp);
                        LOG_INFO(Core, "--test-world-pos: moved player to {}, {}, {}; looking at {}, {}, {}", x, y, z,
                                 lookAt[0], lookAt[2], lookAt[1]);
                    }
                    else if (yawDegrees != 0.0f)
                    {
                        const Matrix3 yaw(MRotationY, HDegree(yawDegrees));
                        cameraOn->SetOrient(yaw * cameraOn->Direction(), VUp);
                        LOG_INFO(Core, "--test-world-pos: moved player to {}, {}, {}; yaw {} degrees", x, y, z,
                                 yawDegrees);
                    }
                    else
                    {
                        LOG_INFO(Core, "--test-world-pos: moved player to {}, {}, {}", x, y, z);
                    }
                }
            }
        }

        if (spawnTestModel && !testModelSpawned && GWorld->GetMode() == GModeArcade)
        {
            testModelSpawned = true; // one attempt: a retry loop would spam the log every frame
            Object* cameraOn = GWorld->CameraOn();
            if (!cameraOn || !GLandscape)
            {
                LOG_ERROR(Core, "--test-model: no camera or landscape to place {} model(s) against",
                          testModelPaths.size());
            }
            else
            {
                for (size_t testModelIndex = 0; testModelIndex < testModelPaths.size(); ++testModelIndex)
                {
                    const std::string& testModelPath = testModelPaths[testModelIndex];
                    // Along the camera's own view direction, not a fixed offset:
                    // a constant +x/+z lands behind the camera as often as in front
                    // of it, and an invisible model reads as a render failure.
                    const Matrix4 view = cameraOn->WorldTransform();
                    const float testModelDistance = AppConfig::Instance().GetTestModelDistance();
                    const Vector3 ahead = view.Position() + view.Direction() * testModelDistance;
                    // A multi-model smoke scene is laid out perpendicular to the view
                    // direction.  This preserves the same distance/LOD selection for
                    // every fixture and makes a normal-map comparison one screenshot.
                    const float lateralOffset =
                        (static_cast<float>(testModelIndex) - static_cast<float>(testModelPaths.size() - 1) * 0.5f) *
                        6.0f;
                    const Vector3 positioned = ahead + view.DirectionAside() * lateralOffset;
                    const float x = positioned.X();
                    const float z = positioned.Z();
                    // This is a render harness, not normal placement gameplay. Keep
                    // the test object on the camera's elevation: an empty dev scene
                    // can look out over water, where SurfaceY is the sea floor and
                    // would put a perfectly valid fixture tens of metres below view.
                    const float y = view.Position().Y();
                    // Match Landscape::ObjectCreate precisely up to the unique object
                    // id: Shapes.New is the production ModelCache/detector route and
                    // NewObject chooses the normal static-object class (ObjectPlain
                    // for an unconfigured external model).
                    Ref<LODShapeWithShadow> shape = Shapes.New(testModelPath.c_str(), false, true);
                    if (!shape)
                    {
                        LOG_ERROR(Core, "--test-model: failed to load {}", testModelPath);
                    }
                    else
                    {
                        Matrix4 transform;
                        transform.SetIdentity();
                        const float requestedScale = AppConfig::Instance().GetTestModelScale();
                        // A house and a lamp have radically different authored units.
                        // In a multi-model material smoke, normalize their bounds to a
                        // two-metre inspection radius so a large fixture cannot hide
                        // every other model.  A single requested model retains its
                        // exact caller-selected scale for reproduction work.
                        const float normalizedScale =
                            testModelPaths.size() > 1 && shape->BoundingSphere() > 0.001f
                                ? std::max(0.05f, std::min(20.0f, 2.0f / shape->BoundingSphere()))
                                : 1.0f;
                        const float effectiveScale = requestedScale * normalizedScale;
                        transform.SetScale(effectiveScale);
                        transform.SetPosition(Vector3(x, y, z));
                        Object* placed = NewObject(shape, -1);
                        placed->SetTransform(transform);
                        // Landscape owns the object and walks this grid every frame.
                        // Unlike the dynamic vehicle path, no CfgVehicles type or
                        // simulation list is involved for a normal static object.
                        GLandscape->AddObject(placed);
                        // Report what actually loaded. Shapes.New hands back a
                        // usable object even when the model behind it is empty, so
                        // "placed" alone does not mean there is anything to draw.
                        const int levels = shape->NLevels();
                        const int faces = levels > 0 ? shape->Level(0)->NFaces() : 0;
                        LOG_INFO(Core, "--test-model: placed {} at {} {} {} (lods={} faces={})", testModelPath, x, y, z,
                                 levels, faces);
                        LOG_INFO(Core, "--test-model: camera at {} {} {} dir {} {} {}", view.Position().X(),
                                 view.Position().Y(), view.Position().Z(), view.Direction().X(), view.Direction().Y(),
                                 view.Direction().Z());
                        LOG_INFO(Core, "--test-model: object pos {} {} {} bounding radius {} effective scale {}",
                                 placed->Position().X(), placed->Position().Y(), placed->Position().Z(),
                                 shape->BoundingSphere(), effectiveScale);
                        // Resolutions decide which LOD is drawable at all: a shape
                        // whose levels are every one of them special (geometry,
                        // memory, shadow) has nothing to draw at any distance, which
                        // looks identical to a rendering failure.
                        for (int level = 0; level < levels; ++level)
                        {
                            LOG_INFO(Core, "--test-model:   lod {} resolution {} faces {}", level,
                                     shape->Resolution(level), shape->Level(level)->NFaces());
                            // The P3D may be drawable even if its companion assets
                            // are absent. List the loader's resolved references here
                            // so a white fallback is not later presented as a material
                            // or texture compatibility result.
                            const Shape* lod = shape->Level(level);
                            for (int texture = 0; texture < lod->NTextures(); ++texture)
                            {
                                const Texture* tex = lod->GetTexture(texture);
                                if (tex)
                                    LOG_INFO(Core, "--test-model:     texture {} {} exists={}", texture, tex->Name(),
                                             QIFStream::FileExists(tex->Name()));
                            }
                            for (int section = 0; section < lod->NSections(); ++section)
                            {
                                const ShapeSection& sec = lod->GetSection(section);
                                if (sec.surfMat)
                                {
                                    const char* materialPath = sec.surfMat->GetName().Data();
                                    LOG_INFO(Core, "--test-model:     section {} material {} exists={}", section,
                                             materialPath, QIFStream::FileExists(materialPath));
                                    Asset::Material::RvMaterialSource source;
                                    try
                                    {
                                        source = Asset::Material::ParseRvMaterialFile(materialPath);
                                    }
                                    catch (const std::exception& error)
                                    {
                                        LOG_ERROR(Core, "--test-model:     material parse failed {}: {}", materialPath,
                                                  error.what());
                                        continue;
                                    }
                                    LOG_INFO(
                                        Core,
                                        "--test-model:     material source shader={} stages={} extras={} classes={}",
                                        source.pixelShaderId, source.stages.size(), source.extra.size(),
                                        source.otherClasses.size());
                                    if (source.pixelShaderId.empty() && source.stages.empty())
                                    {
                                        LOG_WARN(Core,
                                                 "--test-model:     material yielded no entries; no stages are bound");
                                        continue;
                                    }
                                    const auto translated = Asset::Material::TranslateSemantics(
                                        source, Asset::Material::TranslateMaterial(source));
                                    LOG_INFO(Core, "--test-model:     material shader={} schema={}",
                                             translated.shaderFamily, translated.schemaKnown);
                                    for (const auto& slot : translated.slots)
                                    {
                                        const bool procedural = !slot.texture.raw.empty() && slot.texture.raw[0] == '#';
                                        LOG_INFO(Core,
                                                 "--test-model:       slot {} stage {} uv={} texture={} exists={} "
                                                 "procedural={}",
                                                 Asset::Material::ToString(slot.slot), slot.sourceStage, slot.uvSource,
                                                 slot.texture.raw,
                                                 procedural || QIFStream::FileExists(slot.texture.raw.c_str()),
                                                 procedural);
                                    }
                                }
                            }
                        }
                        if (faces <= 0)
                            LOG_ERROR(Core, "--test-model: {} has no faces to draw", testModelPath);
                    }
                }
            }
        }

        if (screenshotTestMode && !screenshotCaptured && GWorld->GetMode() == GModeArcade)
        {
            screenshotFrameCount++;
            if (screenshotFrameCount >= screenshotDelay)
            {
                GEngine->Screenshot(screenshotTestPath.c_str());
                m_forceRender = true;
                Poseidon::AppIdle(); // render frame that captures the screenshot
                const std::string& captureMetricsPath = AppConfig::Instance().GetCaptureMetricsPath();
                if (!captureMetricsPath.empty())
                    LOG_INFO(Core, "Capture metrics {}: {}",
                             WriteCaptureMetrics(captureMetricsPath) ? "saved" : "failed", captureMetricsPath);
                LOG_INFO(Core, "Screenshot test: saved to {}", screenshotTestPath);
                LOG_INFO(Core, "AUTO-TEST SUCCESS");
                screenshotCaptured = true;
                screenshotCloseAfterFrame = mainFrameCounter + 2;
            }
        }

        // Auto-screenshot: capture at specified frame/time triggers
        if (nextAutoScreenshot < autoScreenshots.size() &&
            TriggerReady(autoScreenshots[nextAutoScreenshot].trigger, mainFrameCounter, elapsedMs))
        {
            const auto& as = autoScreenshots[nextAutoScreenshot];
            std::filesystem::create_directories(std::filesystem::path(as.path).parent_path());
            GEngine->Screenshot(as.path.c_str());
            m_forceRender = true;
            Poseidon::AppIdle();
            const std::string& captureMetricsPath = AppConfig::Instance().GetCaptureMetricsPath();
            if (!captureMetricsPath.empty())
                LOG_INFO(Core, "Capture metrics {}: {}", WriteCaptureMetrics(captureMetricsPath) ? "saved" : "failed",
                         captureMetricsPath);
            LOG_INFO(Core, "Auto-screenshot saved: frame={} t={:.1f}s -> {}", mainFrameCounter, elapsedMs / 1000.0f,
                     as.path);
            nextAutoScreenshot++;
            if (nextAutoScreenshot >= autoScreenshots.size())
                screenshotCloseAfterFrame = mainFrameCounter + 2;
        }

        // RenderDoc trigger — single shot.  TriggerCapture() captures
        // the next swap; the path is whatever RenderDoc's template
        // resolves to (defaults to RenderDoc's session directory).
        // No-op if game wasn't launched from RenderDoc UI.
        if (TriggerReady(rdcTrigger, mainFrameCounter, elapsedMs))
        {
            if (RdcCapture::Available())
            {
                RdcCapture::Trigger();
                LOG_INFO(Core, "RenderDoc trigger fired: frame={} t={:.1f}s", mainFrameCounter, elapsedMs / 1000.0f);
            }
            else
            {
                LOG_WARN(Core,
                         "RenderDoc trigger fired at frame={} but API not loaded — "
                         "launch the game from RenderDoc's UI to enable capture",
                         mainFrameCounter);
            }
        }

        if (harnessServer)
        {
            int changedIDD = -1;
            if (displayTracker.PollDisplayChanged(changedIDD))
            {
                harnessServer->PushEvent(HarnessProtocol::DisplayEvent(changedIDD, nullptr));
                if (!harnessReadySent)
                {
                    harnessServer->PushEvent(HarnessProtocol::ReadyEvent(changedIDD));
                    harnessReadySent = true;
                }
            }

            harnessPlayerTracker.Poll(*harnessServer);
            harnessMissionStateTracker.Poll(*harnessServer);

            if (harnessServer->IsExitRequested())
                m_closeRequest = true;

            HarnessCommand cmd;
            if (harnessServer->PopCommand(cmd))
                harnessServer->ProcessCommand(cmd);
        }

        mainFrameCounter++;
        if (screenshotCloseAfterFrame >= 0 && mainFrameCounter >= screenshotCloseAfterFrame)
            m_closeRequest = true;
    }
    if (harnessServer)
    {
        harnessServer->PushEvent(HarnessProtocol::ExitEvent(m_exitCode));
        harnessServer->Stop();
    }
#else
    std::vector<AutoKeyEvent> autoKeyEvents = ParseAutoKeys(AppConfig::Instance().GetAutoKeys());
    std::vector<AutoScreenshotSpec> autoScreenshotList =
        ParseAutoScreenshots(AppConfig::Instance().GetAutoScreenshot());
    // RenderDoc trigger: same trigger format as auto-keys/auto-screenshot
    // (frame number or "<sec>s") but no path — RenderDoc decides where
    // the .rdc lands.  Empty if --rdc-trigger not given.
    TimedTrigger rdcTrigger{-1, -1, true /*fired by default = inert*/};
    {
        const std::string& rdcSpec = AppConfig::Instance().GetRdcTrigger();
        if (!rdcSpec.empty())
        {
            rdcTrigger = ParseTriggerTime(rdcSpec + ":", rdcSpec.size());
            rdcTrigger.fired = false;
        }
    }
    DWORD loopStartTick = Poseidon::Foundation::GlobalTickCount();

    int mainFrameCounter = 0;
    int screenshotCloseAfterFrame = -1;

    UITestEngine displayTracker;
    displayTracker.SetFrameCounter(&mainFrameCounter);
    bool harnessReadySent = false;

    // Benchmark tracking (mirrors Windows loop)
    const bool benchmarkMode = AppConfig::Instance().BenchmarkMode();
    const int benchmarkMaxFrames = 300;
    int benchmarkFrameCount = 0;
    DWORD benchmarkStartTick = 0;
    DWORD benchmarkLastLogTick = 0;
    int benchmarkLastLogFrame = 0;

    // Harness server for Trident-driven game tests — see CreateGameHarness().
    std::unique_ptr<HarnessServer> harnessServer = CreateGameHarness();
    HarnessPlayerTracker harnessPlayerTracker;
    HarnessMissionStateTracker harnessMissionStateTracker;

    while (!m_closeRequest)
    {
        PollStrictAbort(); // --strict: stop on any ERROR logged since last iteration

        if (GEngine)
        {
            GEngine->HandleEvents();
            if (!GEngine->IsOpen())
                break;
        }

        DWORD elapsedMs = Poseidon::Foundation::GlobalTickCount() - loopStartTick;

        // Inject auto-key events for this frame / time
        for (auto& ak : autoKeyEvents)
        {
            if (!TriggerReady(ak.trigger, mainFrameCounter, elapsedMs))
                continue;
            // Scancode 0 = inject SDL_QUIT (simulates Alt+F4 / window close)
            if (ak.scancode == SDL_SCANCODE_UNKNOWN)
            {
                SDL_Event ev = {};
                ev.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&ev);
                LOG_INFO(Core, "Auto-key injected: frame={} SDL_QUIT", mainFrameCounter);
                continue;
            }
            SDL_Event ev = {};
            ev.type = SDL_EVENT_KEY_DOWN;
            ev.key.scancode = ak.scancode;
            ev.key.key = SDL_GetKeyFromScancode(ak.scancode, ak.mod, false);
            ev.key.mod = ak.mod;
            ev.key.down = true;
            SDL_PushEvent(&ev);

            // Also push KEY_UP on the next tick
            ev.type = SDL_EVENT_KEY_UP;
            ev.key.down = false;
            SDL_PushEvent(&ev);
            LOG_INFO(Core, "Auto-key injected: frame={} t={:.1f}s scancode={} mod={}", mainFrameCounter,
                     elapsedMs / 1000.0f, (int)ak.scancode, (int)ak.mod);
        }

        auto frameT0 = TerrainProfile::Now();
        Poseidon::AppIdle();
        auto frameT1 = TerrainProfile::Now();

        // Benchmark FPS tracking (only in arcade/gameplay mode)
        if (benchmarkMode && GWorld && GWorld->GetMode() == GModeArcade)
        {
            if (benchmarkFrameCount == 0)
            {
                benchmarkStartTick = Poseidon::Foundation::GlobalTickCount();
                benchmarkLastLogTick = benchmarkStartTick;
                GTerrainProfile.Reset();
            }
            benchmarkFrameCount++;
            DWORD now = Poseidon::Foundation::GlobalTickCount();
            DWORD sinceLast = now - benchmarkLastLogTick;
            if (sinceLast >= 1000)
            {
                int framesSinceLast = benchmarkFrameCount - benchmarkLastLogFrame;
                float intervalFps = framesSinceLast * 1000.0f / sinceLast;
                float totalElapsed = (now - benchmarkStartTick) / 1000.0f;
                float avgFps = benchmarkFrameCount * 1000.0f / (now - benchmarkStartTick);
                auto& tp = GTerrainProfile;
                double frameCycles = (double)(frameT1 - frameT0);
                LOG_INFO(Core,
                         "BENCHMARK: t={:.1f}s frame={} iFPS={:.1f} aFPS={:.1f}"
                         " | seg={} hit={} miss={} steps={} avgStep={:.0f}"
                         " | ground={:.0f}Mc genSeg={:.0f}Mc frame={:.0f}Mc"
                         " | draw={} clip={}",
                         totalElapsed, benchmarkFrameCount, intervalFps, avgFps, tp.segmentsDrawn, tp.segmentsCacheHit,
                         tp.segmentsCacheMiss, tp.cacheSearchSteps,
                         tp.segmentsDrawn > 0 ? (double)tp.cacheSearchSteps / tp.segmentsDrawn : 0.0,
                         tp.drawGroundCycles / 1e6, tp.generateSegCycles / 1e6, frameCycles / 1e6, tp.drawMeshCalls,
                         tp.drawMeshClipped);
                tp.Reset();
                benchmarkLastLogTick = now;
                benchmarkLastLogFrame = benchmarkFrameCount;
            }
            if (benchmarkFrameCount >= benchmarkMaxFrames)
            {
                DWORD elapsed = Poseidon::Foundation::GlobalTickCount() - benchmarkStartTick;
                float avgFps = benchmarkFrameCount * 1000.0f / (elapsed > 0 ? elapsed : 1);
                LOG_INFO(Core, "BENCHMARK RESULT: {} frames in {:.1f}s = {:.1f} avg FPS", benchmarkFrameCount,
                         elapsed / 1000.0f, avgFps);
                m_closeRequest = true;
            }
        }

        // Auto-screenshot capture
        {
            auto it = std::find_if(autoScreenshotList.begin(), autoScreenshotList.end(), [&](AutoScreenshotSpec& as)
                                   { return TriggerReady(as.trigger, mainFrameCounter, elapsedMs); });
            if (it != autoScreenshotList.end())
            {
                std::filesystem::create_directories(std::filesystem::path(it->path).parent_path());
                GEngine->Screenshot(it->path.c_str());
                m_forceRender = true;
                Poseidon::AppIdle();
                const std::string& captureMetricsPath = AppConfig::Instance().GetCaptureMetricsPath();
                if (!captureMetricsPath.empty())
                    LOG_INFO(Core, "Capture metrics {}: {}",
                             WriteCaptureMetrics(captureMetricsPath) ? "saved" : "failed", captureMetricsPath);
                LOG_INFO(Core, "Auto-screenshot saved: frame={} t={:.1f}s -> {}", mainFrameCounter, elapsedMs / 1000.0f,
                         it->path);
                autoScreenshotList.erase(it);
                if (autoScreenshotList.empty())
                    screenshotCloseAfterFrame = mainFrameCounter + 2;
            }
        }

        // RenderDoc trigger — single shot.  TriggerCapture() captures
        // the next swap; the path is whatever RenderDoc's template
        // resolves to.  If the API isn't loaded (no RenderDoc UI),
        // Trigger() is a no-op and the LOG_WARN flags it once.
        if (TriggerReady(rdcTrigger, mainFrameCounter, elapsedMs))
        {
            if (RdcCapture::Available())
            {
                RdcCapture::Trigger();
                LOG_INFO(Core, "RenderDoc trigger fired: frame={} t={:.1f}s", mainFrameCounter, elapsedMs / 1000.0f);
            }
            else
            {
                LOG_WARN(Core,
                         "RenderDoc trigger fired at frame={} but API not loaded — "
                         "launch the game from RenderDoc's UI to enable capture",
                         mainFrameCounter);
            }
        }

        if (harnessServer)
        {
            int changedIDD = -1;
            if (displayTracker.PollDisplayChanged(changedIDD))
            {
                harnessServer->PushEvent(HarnessProtocol::DisplayEvent(changedIDD, nullptr));
                if (!harnessReadySent)
                {
                    harnessServer->PushEvent(HarnessProtocol::ReadyEvent(changedIDD));
                    harnessReadySent = true;
                }
            }

            harnessPlayerTracker.Poll(*harnessServer);
            harnessMissionStateTracker.Poll(*harnessServer);

            if (harnessServer->IsExitRequested())
                m_closeRequest = true;

            HarnessCommand cmd;
            if (harnessServer->PopCommand(cmd))
                harnessServer->ProcessCommand(cmd);
        }

        mainFrameCounter++;

        // Screenshot test: capture after configured delay frames
        if (screenshotTestMode && !screenshotCaptured &&
            (GWorld->GetMode() == GModeArcade || GWorld->GetMode() == GModeIntro))
        {
            screenshotFrameCount++;
            if (screenshotFrameCount >= screenshotDelay)
            {
                GEngine->Screenshot(screenshotTestPath.c_str());
                m_forceRender = true;
                Poseidon::AppIdle();
                LOG_INFO(Core, "Screenshot test: saved to {}", screenshotTestPath);
                LOG_INFO(Core, "AUTO-TEST SUCCESS");
                screenshotCaptured = true;
                screenshotCloseAfterFrame = mainFrameCounter + 2;
            }
        }
        if (screenshotCloseAfterFrame >= 0 && mainFrameCounter >= screenshotCloseAfterFrame)
            m_closeRequest = true;
    }

    if (harnessServer)
    {
        harnessServer->PushEvent(HarnessProtocol::ExitEvent(m_exitCode));
        harnessServer->Stop();
    }

#endif

    m_validateQuit = true;
    // --strict finalize: an error logged during boot (before the main loop, e.g.
    // --check) or anywhere else must still surface as a non-zero exit code.
    if (m_exitCode == 0 && Poseidon::Foundation::LoggingSystem::StrictTripped())
        m_exitCode = 3;

    LOG_INFO(Core, "Shutdown: begin (exit code {})", m_exitCode);

    extern void CleanupSimulateMission();
    CleanupSimulateMission();

    if (!AppConfig::Instance().GetMPAssign().empty())
    {
        INetworkManager& networkManager = GetNetworkManager();
        const bool missionReachedPlay = networkManager.WasServerPlaying() ||
                                        networkManager.GetServerState() >= NGSPlay ||
                                        networkManager.GetGameState() >= NGSPlay;
        m_exitCode = ResolveMultiplayerAutoTestExitCode(m_exitCode, missionReachedPlay, m_cleanTestEndRequested);
        networkManager.Close();
        Sleep(100);
        LOG_INFO(Core, "MP auto-test: exiting with code {}", m_exitCode);
        _exit(m_exitCode);
    }

    // Clear progress system before shutdown to prevent GlobalAlive callbacks during cleanup.
    ProgressFinish();

    LOG_INFO(Core, "Shutdown: DDTerm");
    extern void DDTerm();
    DDTerm();

    // Remove the isolated test-mission staging now that DDTerm has released any
    // open handles on the mission assets (on Windows, open sound/*.ogg handles
    // block remove_all and would leak the staged copy).
    if (!s_testMissionStageRoot.empty())
    {
        std::error_code ec;
        std::filesystem::remove_all(s_testMissionStageRoot, ec);
        if (ec)
            LOG_ERROR(Core, "Failed to clean up test-mission staging '{}': {}", s_testMissionStageRoot.string(),
                      ec.message());
        s_testMissionStageRoot.clear();
    }

    LOG_INFO(Core, "Shutdown complete");
}

void GameApplication::ShutdownSubsystems() {}

bool GameApplication::InitializeSound()
{
    extern void CleanupSoundSystem();
    CleanupSoundSystem();

    extern IAudioSystem* CreateAudioSystem(void* hwnd, bool noSound, bool isDedicatedServer);
#ifdef _WIN32
    GSoundsys = CreateAudioSystem(GApp->m_hwnd, ENGINE_CONFIG.noSound, false);
#else
    GSoundsys = CreateAudioSystem(nullptr, ENGINE_CONFIG.noSound, false);
#endif

    if (!GSoundsys)
    {
        return false;
    }

    // Load (or create defaults for) audio.cfg.  This is the eager-write
    // boot dance: file missing → write defaults; file present →
    // Normalize against live device lists, apply normalized values to
    // the runtime, but do NOT persist normalization (a temporarily
    // unplugged device must not silently lose its remembered name).
    // The Pester smoke test exercises this via --check + an ephemeral
    // POSEIDON_USER_DIR.
    GSoundsys->LoadConfig();

    GSoundScene = CreateSoundScene();

    return true;
}

void GameApplication::RegisterAudioBackends()
{
    Poseidon::RegisterDummyAudioBackend();
    Poseidon::RegisterTextAudioBackend();
    Poseidon::RegisterOpenALAudioBackend();
    Poseidon::RegisterOpenALVoiceBackend();
    Poseidon::RegisterTestToneVoiceBackend();
}

void GameApplication::RegisterGraphicsBackends()
{
    RegisterDummyGraphicsBackend();
    RegisterGL33GraphicsBackend();
#if POSEIDON_ENABLE_WGPU
    RegisterWgpuGraphicsBackend();
#endif
}

bool GameApplication::InitializeInput()
{
    return true;
}

bool GameApplication::InitializeNetwork()
{
    return true;
}

bool GameApplication::InitializeGraphicsEngine()
{
    // Filebank decryptors are a process-once registry (RegisterFilebankEncryption
    // dedupes by name) — register here, then run the re-runnable content load.
    // Splitting it out lets a mod re-mount call InitializeGameContent() again
    // without re-registering.
    Poseidon::RegisterFilebankEncryption("XOR1024", Poseidon::CreateEncryptXOR1024);

    return InitializeGameContent();
}

bool GameApplication::InitializeGameContent()
{
    // The re-runnable engine-core load (banks/addons + config-derived tables); a mod
    // re-mount calls it again after UnloadGameData. InitMan must be preceded by
    // ManCleanUp on a reload — UnloadGameData provides that. Deltas via the hooks below.
    return InitializeEngineCore();
}

void GameApplication::ConfigureBankMerge()
{
    ENGINE_CONFIG.gMergeTextures = ENGINE_CONFIG.enableHWTL;
    if (ENGINE_CONFIG.gMergeTextures)
        SetGFileBankPrefix("HWTL"); // HW config dependent banks
}

void GameApplication::OnGlobInitialized()
{
    Config::InitDifficulties();
}

void GameApplication::OnManagersInit()
{
    AI_InitTables(); // Resize+fill from CfgExperience — idempotent
    GStats_ClearAll();
}

bool GameApplication::CreateAndSetGraphicsEngine()
{
    extern Engine* CreateEngineWithParams(void* hInstance, int showCmd);
    RegisterGraphicsBackends();
#ifdef _WIN32
    GEngine = CreateEngineWithParams(m_hInstance, m_showCmd);
#else
    GEngine = CreateEngineWithParams(nullptr, 0);
#endif

    if (!GEngine)
        return false;

    // --show-fps uses the same engine toggle as the in-game cheat key.
    if (ENGINE_CONFIG.showFps > 0)
        GEngine->ToggleFps(ENGINE_CONFIG.showFps);

    return true;
}

Engine* GameApplication::CreateGraphicsEngine(const GraphicsEngineParams& params)
{
    const std::string& renderBackend = AppConfig::Instance().GetRenderBackend();
    Engine* engine = GraphicsEngineFactory::Create(renderBackend, params);
    if (!engine && !renderBackend.empty() && _stricmp(renderBackend.c_str(), "auto") != 0)
    {
        // An explicit WGPU request is a test/diagnostic contract, not a hint:
        // silently falling through to Auto could run GL33 and falsely report a
        // successful WGPU smoke test. Auto remains the deliberate fallback path.
        if (_stricmp(renderBackend.c_str(), "wgpu") == 0)
        {
            RptF("Requested WGPU renderer is unavailable; refusing automatic GL33 fallback");
            return nullptr;
        }
        RptF("Unknown or unavailable render backend '%s', defaulting to Auto", renderBackend.c_str());
        engine = GraphicsEngineFactory::Create(GraphicsBackend::Auto, params);
    }

    if (engine && AppConfig::Instance().NoMouseGrab())
        engine->SetMouseGrab(false);

    return engine;
}

bool GameApplication::InitializeWorld()
{
    GWorld = CreateWorld(GEngine, ENGINE_CONFIG.landEditor);

    GPreloadedTextures_Preload(true);

    extern const void* ClientIP_GetPtr();
    // Arm the startup splash only on the genuine first boot. A mod re-mount re-runs this path;
    // replaying the splash drew "Bohemia Interactive presents" over the rebuilt main menu.
    const bool firstBoot = !m_startupSplashArmed;
    m_startupSplashArmed = true;
    if (Poseidon::ShouldArmStartupSplash(firstBoot, ENGINE_CONFIG.noSplash, ENGINE_CONFIG.landEditor,
                                         ENGINE_CONFIG.doCreateServer, RString_GetLength(ClientIP_GetPtr()) != 0))
    {
        // Remaster splash: CWR_BIS (BI logo + "presents" on top, legal
        // copyright on the bottom) -> CWR_CWA (hi-res ARGB8888 game logo)
        // -> game.  Uses our RscTitles.CWR_* from splashLogo.hpp instead
        // of the locked vanilla CWA class.  Shipped in AddOns/cwr_logo.pbo
        // alongside cwr_logo.paa.
        // Leading backslash → OpenScript skips FindScript's default
        // `scripts\` prefix and resolves verbatim against the bank
        // mounted at `cwr_logo\` (our addon PBO).
        SetProgressScript(CreateProgressScript("\\cwr_logo\\cwr_startup.sqs"));
    }

    ProgressStart_Wrapper(IDS_LOAD_INIT);

    LOG_DEBUG(Core, "Creating landscape...");
    GLandscape = CreateLandscape(GEngine, GWorld, false);

    if (!ENGINE_CONFIG.landEditor)
    {
        LOG_DEBUG(Core, "Initializing world landscape...");
        World_InitLandscape(GWorld, GLandscape);
        I_AM_ALIVE();

        LOG_DEBUG(Core, "Preloading vehicle types...");
        VehicleTypes_Preload();
        if (AppConfig::Instance().AuditCfgVehiclesModels())
        {
            VehicleTypes_AuditEditorVisibleModels();
        }

        GFileServer_FlushBank();
    }

    LOG_DEBUG(Core, "Final world initialization...");
    InitWorld();
    I_AM_ALIVE();

    LOG_INFO(Core, "World initialized successfully");

    return true;
}

void GameApplication::ProcessWindowMessages() {}

void GameApplication::EnableRendering()
{
    GApp->m_canRender = true;

    GEngine->SetTimeStartGame(Poseidon::Foundation::GlobalTickCount());
}

void GameApplication::VerifySerialKey()
{
#if _VERIFY_KEY
    m_keyVerified = true;
#else
    m_keyVerified = true;
#endif

#if _VERIFY_KEY_EXT
    m_keyVerified = true;
#endif

    if (!m_keyVerified)
    {
        Poseidon::Foundation::ErrorMessage("Bad serial number given in Setup");
        exit(1);
    }
}

void GameApplication::StartGameMode()
{
    if (ENGINE_CONFIG.doCreateDedicatedServer)
    {
        // Dedicated server needs CreateDedicatedServer which calls SetDedicated()
        // to enable the SimulateDS() state machine
        extern bool CreateDedicatedServer(RString config);
        CreateDedicatedServer(Poseidon::ServerConfig);
        return;
    }

    if (ENGINE_CONFIG.doCreateServer)
    {
        Poseidon::CreateServer();
        return;
    }

    extern RString ClientIP;
    if (ClientIP.GetLength() > 0)
    {
        extern int GetNetworkPort();
        extern int GetNetworkConnectPort();
        extern RString GetNetworkPassword();
        void __cdecl CreateClient(RString ip, int port, RString password);
        int connectPort = GetNetworkConnectPort();
        if (connectPort <= 0)
        {
            connectPort = GetNetworkPort();
        }
        Poseidon::CreateClient(ClientIP, connectPort, GetNetworkPassword());
        return;
    }

    if (!ENGINE_CONFIG.landEditor)
    {
        extern bool Benchmark; // Synced from AppConfig in appConfig.cpp
        extern bool AutoTest;
        extern char LoadFile[256];
        const std::string& testMission = AppConfig::Instance().GetTestMissionPath();
        if (!testMission.empty())
        {
            const std::string staged = StageTestMissionForGame(testMission);
            const auto loadPath = MissionPathLoader::Loader::ResolveMissionFile(staged);
            if (!loadPath)
            {
                LOG_ERROR(Core, "Test mission path '{}' does not resolve to a mission.sqm file", testMission);
                m_exitCode = 44;
                m_closeRequest = true;
                return;
            }

            strncpy(LoadFile, loadPath->c_str(), sizeof(LoadFile) - 1);
            LoadFile[sizeof(LoadFile) - 1] = 0;
            AutoTest = true;
            LOG_INFO(Core, "Test mission: {} -> {}", testMission, *loadPath);
        }
        else if (Benchmark)
        {
            snprintf(LoadFile, sizeof(LoadFile), "%s",
                     (const char*)"Users\\Test\\Missions\\Benchmark.Abel\\mission.sqm");
        }
        if (AppConfig::Instance().IsViewerMode())
        {
            const std::string& model = AppConfig::Instance().GetViewerModelPath();
            const std::string& anim = AppConfig::Instance().GetViewerAnimPath();
            GWorld->StartViewer(model.c_str(), anim.c_str());
        }
        else if (!ENGINE_CONFIG.noMenuScene)
        {
            GWorld->StartIntro();
        }
    }
}

void GameApplication::FinalizeInitialization()
{
    GDebugger.ResumeCheckingAlive();
}

bool GameApplication::CanRemount() const
{
    // Re-mount only from the main menu, which runs the GModeIntro background
    // world. A loaded mission (GModeArcade single-player, GModeNetware MP) holds
    // simulation state that tearing down banks/addons would invalidate. Note the
    // menu's intro scene DOES simulate, so IsSimulationEnabled() is true here —
    // the game mode is the correct discriminator, not the simulation flag.
    return GWorld != nullptr && GWorld->GetMode() == GModeIntro;
}

bool GameApplication::LoadGameData()
{
    // One-shot forced failure for the failed re-mount rollback path.
    if (s_forceRemountReloadFailOnce)
    {
        s_forceRemountReloadFailOnce = false;
        LOG_WARN(Core, "LoadGameData: forced failure (re-mount-fail self-test)");
        return false;
    }

    // Replays the boot data-layer init in boot order, minus the persisted
    // platform steps (engine/window creation + display/graphics config, which
    // stay live). Symmetric with UnloadGameData(keepEngine=true).
    if (!ReadConfiguration())
        return false;
    if (!InitializeGameContent())
        return false;
    if (!InitializeWorld())
        return false;
    if (!InitializeSound())
        return false;
    if (!InitializeSubsystems())
        return false;
    return true;
}

bool GameApplication::Remount(const char* newModPath)
{
    if (!CanRemount())
    {
        LOG_WARN(Core, "Re-mount refused: a mission is active");
        Poseidon::DiscardStagedModInstalls(GApp->m_remountInstalls);
        return false;
    }

    LOG_INFO(Core, "Re-mounting game content (mod='{}')", newModPath != nullptr ? newModPath : "");

    // Mod set to fall back to if the new one fails to load (RStringB is returned
    // by value, so this survives the SetModPath below).
    const auto prevModPath = Poseidon::ModSystem::GetModList();

    // Loading screen on the live window — ProgressSystem is Application-owned and
    // survives the teardown below.
    ProgressStart_Wrapper(IDS_LOAD_INIT);

    // Stop the main loop drawing the world while the content layer is gone.
    // UnloadGameData frees GWorld / GLandscape; a render frame that reaches
    // Landscape::DrawGround -> LandCache::Segment on the freed terrain cache is a
    // use-after-free.  RenderFrame is gated on m_canRender (GameLoop.cpp), so
    // clearing it here closes the window; EnableRendering() restores it only once
    // a load succeeds.
    GApp->m_canRender = false;

    // Trash the game-data layer, keeping window/device/memory/logging alive.
    extern void UnloadGameData(bool keepEngine);
    UnloadGameData(/*keepEngine*/ true);

    // Drop + rebuild GPU resources tied to the old content (no-op on headless).
    if (GEngine)
    {
        GEngine->ResetForRemount();
    }

    std::string swapError;
    if (!GApp->m_remountInstalls.empty() && !Poseidon::SwapStagedModInstalls(GApp->m_remountInstalls, &swapError))
    {
        LOG_ERROR(Core, "Re-mount install swap failed: {}", swapError);
        Poseidon::ModSystem::SetModPath(prevModPath);
        if (LoadGameData())
        {
            if (GWorld)
                GWorld->StartIntro();
            EnableRendering();
        }
        GApp->m_remountFailed = true;
        Poseidon::DiscardStagedModInstalls(GApp->m_remountInstalls);
        ProgressFinish();
        return false;
    }

    // Swap the active mod set, then reload everything from scratch.
    Poseidon::ModSystem::SetModPath(newModPath != nullptr ? newModPath : "");

    if (!LoadGameData())
    {
        // Bad / unsupported mod (e.g. a Workshop package the unpacker rejected).
        // Roll back to the previous set so the app returns to a usable, rendering
        // state instead of a frozen half-mount that the next render frame would
        // crash on (LandCache::Segment on a freed cache), then report failure to
        // the caller so it can surface the error.
        LOG_ERROR(Core, "Re-mount reload failed for mod '{}' — rolling back", newModPath != nullptr ? newModPath : "");
        UnloadGameData(/*keepEngine*/ true);
        if (GEngine)
        {
            GEngine->ResetForRemount();
        }
        Poseidon::RestoreStagedModInstalls(GApp->m_remountInstalls);
        Poseidon::ModSystem::SetModPath(prevModPath);
        if (LoadGameData())
        {
            if (GWorld)
            {
                GWorld->StartIntro();
            }
            EnableRendering();
        }
        else
        {
            LOG_ERROR(Core, "Re-mount rollback also failed — rendering left disabled");
        }
        GApp->m_remountFailed = true; // the menu surfaces this once it is live again (AppIdle)
        Poseidon::DiscardStagedModInstalls(GApp->m_remountInstalls);
        ProgressFinish();
        return false;
    }

    if (GWorld)
    {
        GWorld->StartIntro();
    }

    EnableRendering();
    Poseidon::CommitStagedModInstalls(GApp->m_remountInstalls);
    ProgressFinish();
    LOG_INFO(Core, "Re-mount complete");
    return true;
}

bool GameApplication::ReloadGameContent()
{
    return Remount(Poseidon::ModSystem::GetModList());
}

bool GameApplication::ReloadGameContentWithMods(const char* modPath)
{
    return Remount(modPath != nullptr ? modPath : "");
}

void GameApplication::RegisterGameModules()
{
    Poseidon::MissionsModule::Register();
    Poseidon::CampaignsModule::Register();
    Poseidon::MultiplayerModule::Register();
    Poseidon::EditorModule::Register();
    ModsModule::Register();
}
