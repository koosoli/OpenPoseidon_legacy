#include <Poseidon/Dev/Debug/ReShadeOptional.hpp>
#include <Poseidon/Dev/Debug/ReShadeSceneDepthContract.hpp>
#include <Poseidon/Dev/Debug/ReShadePreviewPolicy.hpp>
#include <Poseidon/Dev/Debug/ReShadeTimingContract.hpp>
#include <Poseidon/Dev/Debug/DevPanelWidgets.hpp>
#include <Poseidon/Dev/Harness/HarnessServer.hpp>
#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/Foundation/Algorithms/Sha256.hpp>
#include <Poseidon/Foundation/Common/GamePaths.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <Poseidon/UI/Settings/GraphicsConfig.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <cmath>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace Dev = Poseidon::Dev;

namespace
{
std::string startupMessage;

std::filesystem::path RuntimeDirectory()
{
    // An explicit launcher may choose another prepared cache.
    if (const char* path = std::getenv("RESHADE_BASE_PATH_OVERRIDE"); path && *path)
        return path;
    if (const char* local = std::getenv("LOCALAPPDATA"); local && *local)
        return std::filesystem::path(local) / "OpenPoseidon" / "reshade" / "6.8.0";
    return {};
}

bool FileDigestMatches(const std::filesystem::path& path, const char* expected)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    Poseidon::Foundation::Sha256 hash;
    std::array<char, 64 * 1024> bytes;
    while (file)
    {
        file.read(bytes.data(), bytes.size());
        hash.Update(bytes.data(), static_cast<size_t>(file.gcount()));
    }
    return file.eof() && hash.Hex() == expected;
}

bool ValidateRuntime(const std::filesystem::path& directory)
{
    if (directory.empty()) return false;
    std::error_code ec;
    for (const char* name : {"OpenPoseidonReShade.addon64", "ReShade.ini", "OpenPoseidon.ini", "Shaders/OpenPoseidonLook.fx"})
        if (!std::filesystem::is_regular_file(directory / name, ec)) return false;
    return FileDigestMatches(directory / "ReShade64.dll", "0cee63f9c9f13f3ac909c5b4903f4dbb4b719a7ab3b4f13b0deaf83c814b94f7") &&
           FileDigestMatches(directory / "ReShade64.json", "aa21713718843e531da396e2bfc80772c9cb3c369d6c30b836be6b0ae812d503");
}

bool ValidatePreview(unsigned mode)
{
    const auto root = RuntimeDirectory();
    if (mode == OpReShadePreview::Off) return true;
    if (mode == OpReShadePreview::ColorLook)
    {
        if (!FileDigestMatches(root / "Shaders/OpenPoseidonLook.fx", "ec0db3cb49f9c990d061a7a02c7e1e7a4e2e59c1f19b8db919481098c24451e6")) return false;
        std::ifstream preset(root / "OpenPoseidon.ini"); std::string line;
        while (std::getline(preset, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.starts_with("Techniques=")) return line == "Techniques=OpenPoseidonLook@OpenPoseidonLook.fx";
        }
        return false;
    }
    if (!OpReShadePreview::UsesDepth(mode)) return false;
    std::ifstream receipt(root / "DEPTH-PROVENANCE.json", std::ios::binary | std::ios::ate);
    const auto length = receipt.tellg();
    if (!receipt || length <= 0 || length > 32768) return false;
    std::string text(static_cast<size_t>(length), '\0'); receipt.seekg(0); receipt.read(text.data(), text.size());
    auto* json = cJSON_Parse(text.c_str());
    if (!json) return false;
    const auto matches = [](const cJSON* object, const char* key, const char* expected) {
        const auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
        return cJSON_IsString(value) && value->valuestring && std::strcmp(value->valuestring, expected) == 0;
    };
    bool validReceipt = matches(json, "probe_license", "GPL-3.0-or-later");
    if (mode != OpReShadePreview::SceneDepth)
    {
        const auto* adapter = cJSON_GetObjectItemCaseSensitive(json, "scene_adapter");
        validReceipt = validReceipt && matches(json, "dh_license", "GPL-2.0 (upstream LICENSE)") &&
            matches(adapter, "source_sha256", OpReShadePreview::DepthAssets[4].digest) &&
            matches(adapter, "generated_sha256", OpReShadePreview::DepthAssets[1].digest) &&
            matches(adapter, "helper_sha256", OpReShadePreview::DepthAssets[2].digest) && matches(adapter, "helper_license", "MIT");
    }
    cJSON_Delete(json);
    if (!validReceipt) return false;
    for (size_t i = 0; i < OpReShadePreview::DepthAssets.size(); ++i)
    {
        if (mode == OpReShadePreview::SceneDepth && i != 0 && i != 6) continue;
        const auto& asset = OpReShadePreview::DepthAssets[i];
        if (!FileDigestMatches(root / asset.path, asset.digest)) return false;
    }
    if (mode == OpReShadePreview::DhDepth)
        return FileDigestMatches(root / OpReShadePreview::DhDebugPreset.path, OpReShadePreview::DhDebugPreset.digest);
    if (mode == OpReShadePreview::DhGi)
        return FileDigestMatches(root / OpReShadePreview::DhGiPreset.path, OpReShadePreview::DhGiPreset.digest);
    return true;
}

struct DepthPreviewSettings
{
    bool hdr = false;
    Poseidon::Engine::TemporalInfo targets{};
    bool Supported() const
    {
        return OpReShadePreview::SupportsDepthSettings(hdr, targets.renderWidth, targets.renderHeight,
            targets.outputWidth, targets.outputHeight, targets.msaaSamples);
    }
};
DepthPreviewSettings ReadDepthPreviewSettings()
{
    DepthPreviewSettings settings;
    auto* engine = Poseidon::GEngine;
    settings.hdr = engine && engine->SupportsTonemap();
    // WGPU only overrides the legacy setters: GetMsaaSamples() is the base 0
    // and GetRenderScale() the base 1. Actual dimensions/MSAA come from Rust.
    if (settings.hdr) settings.targets = engine->GetTemporalInfo();
    return settings;
}
bool DepthPreviewSettingsSupported() { return ReadDepthPreviewSettings().Supported(); }

#if defined(_WIN32)
void AddEnvironmentEntry(const char* name, const std::string& value)
{
    const char* current = std::getenv(name);
    const std::string list = current ? current : "";
    // Do not append duplicates when an explicit opt-in launcher already set it.
    if ((";" + list + ";").find(";" + value + ";") == std::string::npos)
        _putenv_s(name, (value + (list.empty() ? "" : ";" + list)).c_str());
}
#endif

using Query = unsigned (*)();
using SetEnabled = void (*)(int);
using Capture = void (*)();
using QuerySceneDepth = unsigned (*)(OpReShadeSceneDepthStateV1*, unsigned, unsigned);
using SelectPreview = unsigned (*)(unsigned);
using QueryTiming = unsigned (*)(OpReShadeTimingV1*, unsigned, unsigned);
struct Bridge
{
    Query status = nullptr;
    SetEnabled setEnabled = nullptr;
    Capture capture = nullptr;
    Query techniques = nullptr;
    Query activeTechniques = nullptr;
    Query captureCount = nullptr;
    Query captureStage = nullptr;
    Query presentCount = nullptr;
    QuerySceneDepth sceneDepth = nullptr;
    SelectPreview selectPreview = nullptr;
    Query preview = nullptr;
    Query previewStatus = nullptr;
    Query previewCapabilities = nullptr;
    SelectPreview startTiming = nullptr;
    QueryTiming timing = nullptr;
};

Bridge GetBridge()
{
    Bridge bridge;
#if defined(_WIN32)
    static HMODULE module = nullptr;
    // Pin only an add-on already loaded by ReShade; never LoadLibrary.
    if (module || GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"OpenPoseidonReShade.addon64", &module))
    {
        const auto version = reinterpret_cast<Query>(GetProcAddress(module, "OpReShadeBridgeVersion"));
        if (version && version() == 1)
        {
            bridge.status = reinterpret_cast<Query>(GetProcAddress(module, "OpReShadeGetStatus"));
            bridge.setEnabled = reinterpret_cast<SetEnabled>(GetProcAddress(module, "OpReShadeSetEnabled"));
            bridge.capture = reinterpret_cast<Capture>(GetProcAddress(module, "OpReShadeCaptureComparison"));
            bridge.techniques = reinterpret_cast<Query>(GetProcAddress(module, "OpReShadeGetTechniqueCount"));
            bridge.activeTechniques = reinterpret_cast<Query>(GetProcAddress(module, "OpReShadeGetActiveTechniqueCount"));
            bridge.captureCount = reinterpret_cast<Query>(GetProcAddress(module, "OpReShadeGetCaptureCount"));
            bridge.captureStage = reinterpret_cast<Query>(GetProcAddress(module, "OpReShadeGetCaptureStage"));
            bridge.presentCount = reinterpret_cast<Query>(GetProcAddress(module, "OpReShadeGetPresentCount"));
            bridge.sceneDepth = reinterpret_cast<QuerySceneDepth>(GetProcAddress(module, "OpReShadeGetSceneDepthV1"));
            bridge.selectPreview = reinterpret_cast<SelectPreview>(GetProcAddress(module, "OpReShadeSelectPreviewV1"));
            bridge.preview = reinterpret_cast<Query>(GetProcAddress(module, "OpReShadeGetPreviewV1"));
            bridge.previewStatus = reinterpret_cast<Query>(GetProcAddress(module, "OpReShadeGetPreviewStatusV1"));
            bridge.previewCapabilities = reinterpret_cast<Query>(GetProcAddress(module, "OpReShadeGetPreviewCapabilitiesV1"));
            bridge.startTiming = reinterpret_cast<SelectPreview>(GetProcAddress(module, "OpReShadeStartTimingV1"));
            bridge.timing = reinterpret_cast<QueryTiming>(GetProcAddress(module, "OpReShadeGetTimingV1"));
        }
    }
#endif
    return bridge;
}

unsigned Read(Query query) { return query ? query() : 0; }
}

void PrepareOptionalReShadeEnvironment(bool enabled)
{
    if (!enabled)
    {
#if defined(_WIN32)
        _putenv_s("WGR_RESHADE_SCENE_DEPTH", "0");
#endif
        return;
    }
#if defined(_WIN32)
    const auto& config = Poseidon::Foundation::AppConfig::Instance();
    // An opt-in diagnostic only; never let a remembered user profile enable a
    // raw native handoff in ordinary/multiplayer/server launches.
    if (!config.DevMode() || config.GetRenderBackend() != "wgpu" ||
        config.IsHostServer() || config.GetConnectIP().GetLength() > 0)
        _putenv_s("WGR_RESHADE_SCENE_DEPTH", "0");
    if (config.GetRenderBackend() != "wgpu" || config.IsHostServer() || config.GetConnectIP().GetLength() > 0)
    {
        startupMessage = "Optional ReShade was skipped: this preview requires a single-player WGPU launch.";
        LOG_WARN(Graphics, "{}", startupMessage);
        return;
    }
    const auto directory = RuntimeDirectory();
    if (!ValidateRuntime(directory))
    {
        startupMessage = "ReShade cache missing or mismatched. Run scripts/Prepare-ReShade.ps1; normal renderer remains active.";
        LOG_WARN(Graphics, "{}", startupMessage);
        return;
    }
    // 1 is the explicit diagnostic harness; 2 only makes live developer
    // selection available. The renderer does no snapshot/resolve until requested.
    if (config.DevMode() && ValidatePreview(OpReShadePreview::SceneDepth) && !std::getenv("WGR_RESHADE_SCENE_DEPTH"))
        _putenv_s("WGR_RESHADE_SCENE_DEPTH", "2");
    AddEnvironmentEntry("VK_ADD_LAYER_PATH", directory.string());
    AddEnvironmentEntry("VK_INSTANCE_LAYERS", "VK_LAYER_reshade");
    _putenv_s("RESHADE_BASE_PATH_OVERRIDE", directory.string().c_str());
    _putenv_s("WGPU_BACKEND", "vulkan");
    // Isolate this preview from DLSS unless the launch explicitly opts in.
    if (!std::getenv("WGR_DLSS")) _putenv_s("WGR_DLSS", "0");
    startupMessage = "Optional ReShade requested for this launch (Vulkan).";
    LOG_INFO(Graphics, "{}", startupMessage);
#else
    startupMessage = "Optional ReShade preview currently requires Windows.";
#endif
}

void DrawOptionalReShadeControls()
{
    Dev::PanelHelp("ReShade (optional, single-player preview)");
    static bool preferenceLoaded = false;
    static bool loadOnNextLaunch = false;
    static std::string preferenceMessage;
    const std::string path = Poseidon::Foundation::GamePaths::Instance().UserDir() + "graphics.cfg";
    if (!preferenceLoaded)
    {
        Poseidon::GraphicsConfig config;
        config.Load(path);
        loadOnNextLaunch = config.optionalReShade;
        preferenceLoaded = true;
    }
    if (Dev::Checkbox("Load optional ReShade on next launch", &loadOnNextLaunch))
    {
        Poseidon::GraphicsConfig config;
        config.Load(path);
        config.optionalReShade = loadOnNextLaunch;
        preferenceMessage = config.Save(path) ? "Saved. Restart the game to change runtime loading." : "Could not save graphics.cfg.";
        if (loadOnNextLaunch && !ValidateRuntime(RuntimeDirectory()))
            preferenceMessage += " Optional cache missing/mismatched: run scripts/Prepare-ReShade.ps1 first.";
    }
    Dev::PanelTooltip("Requires the separately prepared official local runtime (scripts/Prepare-ReShade.ps1).\n"
                      "Default off. Restart required to load/unload the Vulkan layer; live effects switch below.");
    if (!preferenceMessage.empty()) Dev::PanelHelp("%s", preferenceMessage.c_str());
    if (!startupMessage.empty()) Dev::PanelHelp("%s", startupMessage.c_str());
    const Bridge bridge = GetBridge();
    const unsigned status = Read(bridge.status);
    if ((status & 1u) && bridge.setEnabled)
    {
        if (bridge.selectPreview && bridge.preview)
        {
            const char* choices[] = {"Off", "Color look", "Scene depth", "DH depth debug", "DH GI (experimental)", "External preset"};
            int preview = static_cast<int>(Read(bridge.preview));
            if (Dev::Combo("ReShade preview", &preview, choices, 6) && preview < OpReShadePreview::External)
            {
                const auto& app = Poseidon::Foundation::AppConfig::Instance();
                const char* available = std::getenv("WGR_RESHADE_SCENE_DEPTH");
                const bool devSp = app.DevMode() && app.GetRenderBackend() == "wgpu" && !app.IsHostServer() &&
                    (!Poseidon::GWorld || Poseidon::GWorld->GetMode() != Poseidon::GModeNetware);
                if (!devSp) preferenceMessage = "Preview selection requires developer single-player WGPU.";
                else if (preview == OpReShadePreview::DhGi && !(Read(bridge.previewCapabilities) & 1u))
                    preferenceMessage = "DH GI requires the updated optional add-on with native fog compatibility checks. Prepare the local stage and restart.";
                else if (!ValidatePreview(preview)) preferenceMessage = "Prepared preset/shader/licence checks failed. Prepare the optional depth files first.";
                else if (OpReShadePreview::UsesDepth(preview) && (!available || (*available != '1' && *available != '2')))
                    preferenceMessage = "Depth preview was not prepared at startup. Restart after preparing the optional depth files.";
                else if (OpReShadePreview::UsesDepth(preview) && !DepthPreviewSettingsSupported())
                    preferenceMessage = "Depth previews require 1x or 4x MSAA, 100% render scale and HDR. Change graphics settings separately.";
                else preferenceMessage = bridge.selectPreview(preview) ? "Preview requested for this session." : "Preview request refused.";
            }
            Dev::PanelTooltip("Uses only the separately prepared, verified presets. Depth/GI stays off on the next launch.\n"
                "DH GI requires native surface fog disabled; fogged color is not a valid lighting donor.\n"
                "It never changes weather/fog. Fog-free image quality still needs acceptance.");
            const unsigned selectionStatus = Read(bridge.previewStatus);
            if (selectionStatus == 6) Dev::PanelHelp("Waiting for exactly the selected compiled technique; effects remain off.");
            if (selectionStatus == 5) Dev::PanelHelp("Waiting for a valid world-depth snapshot; effects remain off.");
            if (selectionStatus == 7) Dev::PanelHelp("DH GI paused: native fog is active or unverified. Unfogged surface lighting/transport is unavailable; native fog is preserved. Depth debug remains available.");
            if (OpReShadePreview::UsesDepth(Read(bridge.preview)) && !DepthPreviewSettingsSupported())
                Dev::PanelHelp("Depth preview paused: requires 1x/4x MSAA, 100%% render scale and HDR. Graphics settings are unchanged.");
            else if (selectionStatus == 3 || selectionStatus == 4) Dev::PanelHelp("Preset selection failed; check the prepared local cache.");
        }
        bool enabled = (status & 2u) != 0;
        const bool fogBlocked = Read(bridge.previewStatus) == 7;
        if (!fogBlocked && (!bridge.preview || Read(bridge.preview) != OpReShadePreview::Off) && Dev::Checkbox("ReShade effects", &enabled))
            bridge.setEnabled(enabled ? 1 : 0);
        Dev::PanelTooltip("Live control of the actual optional ReShade runtime.\n"
                          "Home opens its own preset controls. This does not change native fog, GTAO or GI.");
        Dev::PanelHelp("Loaded techniques: %u; enabled techniques: %u", Read(bridge.techniques), Read(bridge.activeTechniques));
        if (!fogBlocked && bridge.capture && (!bridge.preview || Read(bridge.preview) != OpReShadePreview::Off) && Dev::Button("Save ReShade before/after"))
            bridge.capture();
        Dev::PanelTooltip("Hold the camera still. Saves Native and ReShade images in the optional\n"
                          "runtime Screenshots directory, then restores the original effects state.");
        if (!fogBlocked && bridge.startTiming && Dev::Button("Measure ReShade GPU (120 frames)")) bridge.startTiming(120);
        Dev::PanelTooltip("Measures all ReShade effects with GPU timestamps and total frame intervals.\n"
            "Hold the camera/settings still. Initialization may wait before excluded warmup; readback never waits per frame.");
        OpReShadeTimingV1 timing{};
        if (bridge.timing && bridge.timing(&timing, sizeof(timing), 1))
        {
            if (timing.status >= 2 && timing.status <= 4) Dev::PanelHelp("Timing: %u/%u frames, %u GPU brackets complete.", timing.recorded, timing.frames, timing.completedGpu);
            else if (timing.status == 5)
            {
                double gpu = 0, wall = 0; unsigned count = 0;
                for (unsigned i = 0; i < timing.recorded; ++i)
                { wall += timing.samples[i].presentMs; if (timing.samples[i].flags & 2) { gpu += timing.samples[i].gpuMs; ++count; } }
                if (count) Dev::PanelHelp("Mean ReShade GPU %.3f ms (%u brackets); total frame interval %.3f ms.", gpu/count, count, timing.recorded ? wall/timing.recorded : 0);
                else Dev::PanelHelp("No GPU bracket samples (effects off); total frame interval %.3f ms.", timing.recorded ? wall/timing.recorded : 0);
            }
            else if (timing.status >= 6) Dev::PanelHelp("Timing unavailable/interrupted (status %u); no cost acceptance.", timing.status);
        }
        return;
    }
    Dev::PanelHelp("Inactive. Prepare the optional runtime, enable loading above, then restart.");
    Dev::PanelTooltip("Optional external post-processing; no ReShade dependency in a normal launch.\n"
                      "Use the official runtime, matching graphics API, and test without other add-ons first.");
}

void RegisterOptionalReShadeHarness(Dev::HarnessServer& harness)
{
    harness.RegisterCommand({"optional_reshade", "Dev/SP optional runtime state, preview, timing or comparison", {{"action", "string", false}, {"preview", "number", false}, {"frames", "number", false}}},
        [](const std::string&, cJSON* root) -> std::string
        {
            if (!Poseidon::Foundation::AppConfig::Instance().DevMode() ||
                (Poseidon::GWorld && Poseidon::GWorld->GetMode() == Poseidon::GModeNetware))
                return Dev::HarnessProtocol::ErrorResponse("optional ReShade controls require --dev and single-player");
            const auto* item = cJSON_GetObjectItemCaseSensitive(root, "action");
            if (item && (!cJSON_IsString(item) || !item->valuestring))
                return Dev::HarnessProtocol::ErrorResponse("action must be state, select, measure, on, off or capture");
            const char* action = item ? item->valuestring : "state";
            const bool state = std::strcmp(action, "state") == 0;
            const bool on = std::strcmp(action, "on") == 0;
            const bool off = std::strcmp(action, "off") == 0;
            const bool capture = std::strcmp(action, "capture") == 0;
            const bool select = std::strcmp(action, "select") == 0;
            const bool measure = std::strcmp(action, "measure") == 0;
            if (!state && !on && !off && !capture && !select && !measure)
                return Dev::HarnessProtocol::ErrorResponse("action must be state, select, measure, on, off or capture");
            const Bridge bridge = GetBridge();
            const unsigned status = Read(bridge.status);
            if (!state && (!(status & 1u) || !bridge.setEnabled || !bridge.capture))
                return Dev::HarnessProtocol::ErrorResponse("optional ReShade runtime unavailable; use opt-in launcher");
            if ((on || capture || measure) && Read(bridge.previewStatus) == 7)
                return Dev::HarnessProtocol::ErrorResponse("DH GI unsupported: native fog is active or unverified; depth debug remains available; fog unchanged");
            if (on || off) bridge.setEnabled(on ? 1 : 0);
            if (capture)
            {
                if (bridge.preview && Read(bridge.preview) == OpReShadePreview::Off)
                    return Dev::HarnessProtocol::ErrorResponse("select a preview before capture");
                OpReShadeTimingV1 timing{};
                if (bridge.timing && bridge.timing(&timing, sizeof(timing), 1) && timing.status >= 2 && timing.status <= 4)
                    return Dev::HarnessProtocol::ErrorResponse("wait for the bounded timing window before capture");
                bridge.capture();
            }
            if (select)
            {
                const auto* value = cJSON_GetObjectItemCaseSensitive(root, "preview");
                if (!bridge.selectPreview || !cJSON_IsNumber(value) || value->valuedouble < 0 ||
                    value->valuedouble > static_cast<double>(OpReShadePreview::DhGi) || value->valuedouble != value->valueint)
                    return Dev::HarnessProtocol::ErrorResponse("preview must be 0=off, 1=color, 2=scene depth, 3=DH depth, 4=DH GI; updated add-on required");
                const auto mode = static_cast<unsigned>(value->valueint);
                if (mode == OpReShadePreview::DhGi && !(Read(bridge.previewCapabilities) & 1u))
                    return Dev::HarnessProtocol::ErrorResponse("DH GI requires updated add-on with native fog compatibility checks; restart after local preparation");
                const char* available = std::getenv("WGR_RESHADE_SCENE_DEPTH");
                if (!ValidatePreview(mode)) return Dev::HarnessProtocol::ErrorResponse("prepared preview shader/preset/licence digest checks failed");
                if (OpReShadePreview::UsesDepth(mode) && (!available || (*available != '1' && *available != '2') || !DepthPreviewSettingsSupported()))
                    return Dev::HarnessProtocol::ErrorResponse("depth preview requires prepared startup, HDR, render scale 1 and 1x/4x MSAA; settings unchanged");
                if (!bridge.selectPreview(mode)) return Dev::HarnessProtocol::ErrorResponse("preview request refused");
            }
            if (measure)
            {
                const auto* frames = cJSON_GetObjectItemCaseSensitive(root, "frames");
                if (!bridge.startTiming || !cJSON_IsNumber(frames) || frames->valuedouble != frames->valueint ||
                    frames->valueint < 30 || frames->valueint > 240 || !bridge.startTiming(frames->valueint))
                    return Dev::HarnessProtocol::ErrorResponse("timing needs updated add-on, frames 30..240 and no queued request");
            }
            auto* result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "ready", (status & 1u) != 0);
            cJSON_AddBoolToObject(result, "enabled", (status & 2u) != 0);
            cJSON_AddBoolToObject(result, "queued", !state);
            cJSON_AddNumberToObject(result, "techniques", Read(bridge.techniques));
            cJSON_AddNumberToObject(result, "activeTechniques", Read(bridge.activeTechniques));
            cJSON_AddNumberToObject(result, "captureCount", Read(bridge.captureCount));
            cJSON_AddNumberToObject(result, "captureStage", Read(bridge.captureStage));
            cJSON_AddNumberToObject(result, "presentCount", Read(bridge.presentCount));
            cJSON_AddNumberToObject(result, "preview", bridge.preview ? Read(bridge.preview) : OpReShadePreview::External);
            cJSON_AddNumberToObject(result, "previewStatus", Read(bridge.previewStatus));
            cJSON_AddNumberToObject(result, "previewCapabilities", Read(bridge.previewCapabilities));
            cJSON_AddStringToObject(result, "previewReason", Read(bridge.previewStatus) == 7 ?
                "DH GI unsupported: native fog active or unverified; unfogged surface lighting/transport unavailable; native scene preserved" : "");
            const auto settings = ReadDepthPreviewSettings();
            const char* availability = std::getenv("WGR_RESHADE_SCENE_DEPTH");
            auto* settingsJson = cJSON_AddObjectToObject(result, "previewSettings");
            cJSON_AddBoolToObject(settingsJson, "hdr", settings.hdr);
            cJSON_AddBoolToObject(settingsJson, "supported", settings.Supported());
            cJSON_AddStringToObject(settingsJson, "startupDepth", availability ? availability : "");
            cJSON_AddNumberToObject(settingsJson, "renderWidth", settings.targets.renderWidth);
            cJSON_AddNumberToObject(settingsJson, "renderHeight", settings.targets.renderHeight);
            cJSON_AddNumberToObject(settingsJson, "outputWidth", settings.targets.outputWidth);
            cJSON_AddNumberToObject(settingsJson, "outputHeight", settings.targets.outputHeight);
            cJSON_AddNumberToObject(settingsJson, "activeMsaaSamples", settings.targets.msaaSamples);
            OpReShadeTimingV1 timing{};
            if (bridge.timing && bridge.timing(&timing, sizeof(timing), 1))
            {
                auto* row = cJSON_AddObjectToObject(result, "timing");
                cJSON_AddNumberToObject(row, "status", timing.status);
                cJSON_AddNumberToObject(row, "generation", static_cast<double>(timing.generation));
                cJSON_AddNumberToObject(row, "frequency", static_cast<double>(timing.frequency));
                cJSON_AddNumberToObject(row, "frames", timing.frames);
                cJSON_AddNumberToObject(row, "recorded", timing.recorded);
                cJSON_AddNumberToObject(row, "completedGpu", timing.completedGpu);
                cJSON_AddNumberToObject(row, "preview", timing.preview);
                auto* samples = cJSON_AddArrayToObject(row, "samples");
                for (unsigned i = 0; i < timing.recorded && i < 240; ++i)
                {
                    auto* sample = cJSON_CreateObject(); cJSON_AddItemToArray(samples, sample);
                    cJSON_AddNumberToObject(sample, "gpuMs", timing.samples[i].gpuMs);
                    cJSON_AddNumberToObject(sample, "presentMs", timing.samples[i].presentMs);
                    cJSON_AddNumberToObject(sample, "flags", timing.samples[i].flags);
                }
                cJSON_AddStringToObject(row, "scope", "GPU: complete ReShade begin/finish-effects bracket, excludes engine snapshot/overlay. Present intervals: total wall frame cadence, not GPU busy time. Initialization wait and 20 warmup frames excluded; no query reuse or per-frame waits.");
            }
            OpReShadeSceneDepthStateV1 depth{};
            if (bridge.sceneDepth && bridge.sceneDepth(&depth, sizeof(depth), 1) == 1)
            {
                const auto& p = depth.publication;
                auto* row = cJSON_AddObjectToObject(result, "sceneDepth");
                cJSON_AddBoolToObject(row, "nativeSurfaceFogDisabled", (p.flags & 8u) != 0);
                cJSON_AddNumberToObject(row, "version", 1);
                cJSON_AddNumberToObject(row, "status", depth.status);
                cJSON_AddBoolToObject(row, "bound", depth.status == 2 && depth.boundGeneration == p.generation);
                cJSON_AddNumberToObject(row, "frame", static_cast<double>(p.frame));
                cJSON_AddNumberToObject(row, "generation", static_cast<double>(p.generation));
                cJSON_AddNumberToObject(row, "boundFrame", static_cast<double>(depth.boundFrame));
                cJSON_AddNumberToObject(row, "sampleFrame", static_cast<double>(p.sampleFrame));
                cJSON_AddNumberToObject(row, "sampleGeneration", static_cast<double>(p.sampleGeneration));
                cJSON_AddNumberToObject(row, "width", p.width);
                cJSON_AddNumberToObject(row, "sourceSamples", (p.flags & 4u) ? 4 : 1);
                cJSON_AddNumberToObject(row, "height", p.height);
                cJSON_AddNumberToObject(row, "finiteSamples", p.finiteSamples);
                cJSON_AddNumberToObject(row, "nonzeroSamples", p.nonzeroSamples);
                cJSON_AddNumberToObject(row, "minimum", p.minimum);
                cJSON_AddNumberToObject(row, "maximum", p.maximum);
                cJSON_AddNumberToObject(row, "nearPlane", p.nearPlane);
                auto* grid = cJSON_AddArrayToObject(row, "grid");
                for (float value : p.grid) cJSON_AddItemToArray(grid, cJSON_CreateNumber(value));
                auto* projection = cJSON_AddArrayToObject(row, "projection");
                for (float value : p.projection) cJSON_AddItemToArray(projection, cJSON_CreateNumber(value));
                cJSON_AddStringToObject(row, "scope", "Developer world snapshot; 16x16 bin-centre witness, not full-image reduction. Raw reversed-Z; visual alignment unverified.");
            }
            cJSON_AddStringToObject(result, "scope", "Atomic published runtime state; requests apply on ReShade presentation. Capture count records paired save_screenshot calls; verify actual files separately.");
            return Dev::HarnessProtocol::JsonResponse(result);
        });
}
