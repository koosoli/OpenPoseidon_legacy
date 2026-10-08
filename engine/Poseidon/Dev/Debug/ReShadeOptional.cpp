#include <Poseidon/Dev/Debug/ReShadeOptional.hpp>
#include <Poseidon/Dev/Debug/DevPanelWidgets.hpp>
#include <Poseidon/Dev/Harness/HarnessServer.hpp>
#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/Foundation/Algorithms/Sha256.hpp>
#include <Poseidon/Foundation/Common/GamePaths.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <Poseidon/UI/Settings/GraphicsConfig.hpp>
#include <Poseidon/World/World.hpp>
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
        }
    }
#endif
    return bridge;
}

unsigned Read(Query query) { return query ? query() : 0; }
}

void PrepareOptionalReShadeEnvironment(bool enabled)
{
    if (!enabled) return;
#if defined(_WIN32)
    const auto& config = Poseidon::Foundation::AppConfig::Instance();
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
        bool enabled = (status & 2u) != 0;
        if (Dev::Checkbox("ReShade effects", &enabled))
            bridge.setEnabled(enabled ? 1 : 0);
        Dev::PanelTooltip("Live control of the actual optional ReShade runtime.\n"
                          "Home opens its own preset controls. This does not change native fog, GTAO or GI.");
        Dev::PanelHelp("Loaded techniques: %u; enabled techniques: %u", Read(bridge.techniques), Read(bridge.activeTechniques));
        if (bridge.capture && Dev::Button("Save ReShade before/after"))
            bridge.capture();
        Dev::PanelTooltip("Hold the camera still. Saves Native and ReShade images in the optional\n"
                          "runtime Screenshots directory, then restores the original effects state.");
        return;
    }
    Dev::PanelHelp("Inactive. Prepare the optional runtime, enable loading above, then restart.");
    Dev::PanelTooltip("Optional external post-processing; no ReShade dependency in a normal launch.\n"
                      "Use the official runtime, matching graphics API, and test without other add-ons first.");
}

void RegisterOptionalReShadeHarness(Dev::HarnessServer& harness)
{
    harness.RegisterCommand({"optional_reshade", "Dev/SP optional runtime state, on/off or final-frame comparison", {{"action", "string", false}}},
        [](const std::string&, cJSON* root) -> std::string
        {
            if (!Poseidon::Foundation::AppConfig::Instance().DevMode() ||
                (Poseidon::GWorld && Poseidon::GWorld->GetMode() == Poseidon::GModeNetware))
                return Dev::HarnessProtocol::ErrorResponse("optional ReShade controls require --dev and single-player");
            const auto* item = cJSON_GetObjectItemCaseSensitive(root, "action");
            if (item && (!cJSON_IsString(item) || !item->valuestring))
                return Dev::HarnessProtocol::ErrorResponse("action must be state, on, off or capture");
            const char* action = item ? item->valuestring : "state";
            const bool state = std::strcmp(action, "state") == 0;
            const bool on = std::strcmp(action, "on") == 0;
            const bool off = std::strcmp(action, "off") == 0;
            const bool capture = std::strcmp(action, "capture") == 0;
            if (!state && !on && !off && !capture)
                return Dev::HarnessProtocol::ErrorResponse("action must be state, on, off or capture");
            const Bridge bridge = GetBridge();
            const unsigned status = Read(bridge.status);
            if (!state && (!(status & 1u) || !bridge.setEnabled || !bridge.capture))
                return Dev::HarnessProtocol::ErrorResponse("optional ReShade runtime unavailable; use opt-in launcher");
            if (on || off) bridge.setEnabled(on ? 1 : 0);
            if (capture) bridge.capture();
            auto* result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "ready", (status & 1u) != 0);
            cJSON_AddBoolToObject(result, "enabled", (status & 2u) != 0);
            cJSON_AddBoolToObject(result, "queued", !state);
            cJSON_AddNumberToObject(result, "techniques", Read(bridge.techniques));
            cJSON_AddNumberToObject(result, "activeTechniques", Read(bridge.activeTechniques));
            cJSON_AddNumberToObject(result, "captureCount", Read(bridge.captureCount));
            cJSON_AddNumberToObject(result, "captureStage", Read(bridge.captureStage));
            cJSON_AddNumberToObject(result, "presentCount", Read(bridge.presentCount));
            cJSON_AddStringToObject(result, "scope", "Atomic published runtime state; requests apply on ReShade presentation. Capture count records paired save_screenshot calls; verify actual files separately.");
            return Dev::HarnessProtocol::JsonResponse(result);
        });
}
