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
        ParamClass* objects = display->AddClass("objects");
        ParamClass* preview = objects->AddClass("SoldierPreview");
        preview->Add("type", CT_OBJECT);
        preview->Add("idc", IDC_INVENTORY_PREVIEW);
        // Start shapeless; CInventoryPreview::SetSubject uses the player's own
        // shape. Loading that shape here would mark the shared world model as a
        // cockpit and disable its sun lighting in ControlObject's constructor.
        preview->Add("model", RString(""));
        preview->Add("scale", 0.8f);
        const auto vector = [&](const char* name, float x, float y, float z)
        {
            ParamEntry* values = preview->AddArray(name);
            for (float value : {x, y, z})
                values->AddValue(value);
        };
        vector("position", 0.0f, -0.65f, 1.7f);
        vector("direction", 0.0f, 0.0f, -1.0f);
        vector("up", 0.0f, 1.0f, 0.0f);
        return display;
    }();
    return resource;
}
} // namespace Poseidon
