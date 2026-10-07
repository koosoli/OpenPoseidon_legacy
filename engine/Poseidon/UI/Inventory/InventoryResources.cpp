// SPDX-License-Identifier: GPL-3.0-or-later
#include "InventoryResources.hpp"

#include <Poseidon/Core/resincl.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>

namespace Poseidon
{
const ParamEntry* InventoryDisplayResource()
{
    if (const ParamEntry* custom = Res.FindEntry("RscDisplayInventory");
        custom && custom->IsClass() && custom->FindEntry("idd"))
        return custom;

    // Keep the config alive for the display and its control references. Build
    // directly rather than parsing/writing installation files during startup.
    static ParamFile fallback;
    static const ParamEntry* resource = []() -> const ParamEntry*
    {
        ParamClass* display = fallback.AddClass("RscDisplayInventory");
        display->Add("idd", 62020);
        display->Add("movingEnable", 0);
        display->AddArray("controls");
        // A live world shape cannot be rendered safely as a legacy ControlObject:
        // it selects the HDR world pipeline inside the LDR UI pass. Keep the
        // native scene visible between the panes until a separate preview pass
        // exists, rather than modifying shared soldier geometry/material flags.
        return display;
    }();
    return resource;
}
} // namespace Poseidon
