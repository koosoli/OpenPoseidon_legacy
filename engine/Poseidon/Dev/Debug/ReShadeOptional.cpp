#include <Poseidon/Dev/Debug/ReShadeOptional.hpp>
#include <Poseidon/Dev/Debug/DevPanelWidgets.hpp>
#include <Poseidon/Dev/Harness/HarnessServer.hpp>
#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/World/World.hpp>
#include <cstring>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace Dev = Poseidon::Dev;

namespace
{
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

void DrawOptionalReShadeControls()
{
    Dev::PanelHelp("ReShade (optional, single-player preview)");
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
    Dev::PanelHelp("Inactive. Start using scripts/Launch-ReShade.ps1 to opt in.");
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
