#include <Poseidon/Dev/Debug/ReShadeOptional.hpp>
#include <Poseidon/Dev/Debug/DevPanelWidgets.hpp>

#if defined(_WIN32)
#include <Windows.h>
#endif

void DrawOptionalReShadeControls()
{
    Dev::PanelHelp("ReShade (optional, single-player preview)");
#if defined(_WIN32)
    static HMODULE module = nullptr;
    // Pin only an add-on already loaded by ReShade. No LoadLibrary, dependency,
    // Vulkan layer, registry modification, or startup hook on the normal path.
    if (module || GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"OpenPoseidonReShade.addon64", &module))
    {
        using Version = unsigned (*)();
        using Status = unsigned (*)();
        using SetEnabled = void (*)(int);
        using Capture = void (*)();
        const auto version = reinterpret_cast<Version>(GetProcAddress(module, "OpReShadeBridgeVersion"));
        const auto status = reinterpret_cast<Status>(GetProcAddress(module, "OpReShadeGetStatus"));
        const auto setEnabled = reinterpret_cast<SetEnabled>(GetProcAddress(module, "OpReShadeSetEnabled"));
        const auto capture = reinterpret_cast<Capture>(GetProcAddress(module, "OpReShadeCaptureComparison"));
        if (version && status && setEnabled && version() == 1)
        {
            const unsigned current = status();
            if (current & 1u)
            {
                bool enabled = (current & 2u) != 0;
                if (Dev::Checkbox("ReShade effects", &enabled))
                    setEnabled(enabled ? 1 : 0);
                Dev::PanelTooltip("Live control of the actual optional ReShade runtime.\n"
                                  "Home opens its own preset controls. This does not change native fog, GTAO or GI.");
                if (capture && Dev::Button("Save ReShade before/after"))
                    capture();
                Dev::PanelTooltip("Hold the camera still. Saves Native and ReShade images in the optional\n"
                                  "runtime Screenshots directory, then restores the original effects state.");
                return;
            }
            Dev::PanelHelp("Add-on loaded; waiting for ReShade presentation.");
            return;
        }
        Dev::PanelHelp("Incompatible ReShade bridge; rebuild the optional add-on.");
        return;
    }
#endif
    Dev::PanelHelp("Inactive. Start using scripts/Launch-ReShade.ps1 to opt in.");
    Dev::PanelTooltip("Optional external post-processing; no ReShade dependency in a normal launch.\n"
                      "Use the official runtime, matching graphics API, and test without other add-ons first.");
}
