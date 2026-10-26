// SPDX-License-Identifier: GPL-3.0-or-later
// Main menu CONQUEST: which island the war is fought on. Each is one of the @OP_Conquest pack's missions in
// Missions\OpenPoseidon; DisplayMain::OnChildDestroyed starts the one chosen (ConquestIslandChoice).
#include <Poseidon/UI/LocalMapsDisplay.hpp>
#include <Poseidon/UI/Controls/UIControls.hpp>
#include <Poseidon/UI/Controls/UIControlsWidgets.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/Core/resincl.hpp>

#include <string>

namespace Poseidon
{
namespace
{
constexpr int PanelId = 62311;
constexpr int TitleId = 62312;
constexpr int NoteId = 62313;
constexpr int BackId = 62314;
constexpr int FirstIslandId = 62320;

struct Island
{
    const char* label;
    const char* mission;
};
constexpr Island Islands[] = {
    {"Everon  -  18 villages, the hand-made front", "conquest-everon.Eden"},
    {"Malden  -  its 14 towns", "conquest-malden.Abel"},
    {"Kolgujev  -  21 settlements", "conquest-kolgujev.Cain"},
    {"Nogova  -  its villages", "conquest-nogova.Noe"},
};

std::string& Choice()
{
    static std::string choice;
    return choice;
}

class ConquestIslandDisplay : public Display
{
    ParamFile _resources;

    void AddControl(const char* base, const char* name, int id, const char* text, float x, float y, float w, float h)
    {
        const ParamEntry* inherited = Res.FindEntry(base);
        if (!inherited || !inherited->IsClass())
            return;
        ParamClass* cls = _resources.AddClass(name);
        cls->Add("idc", id);
        cls->Add("text", RString(text));
        cls->Add("x", x);
        cls->Add("y", y);
        cls->Add("w", w);
        cls->Add("h", h);
        cls->Add("sizeEx", id == NoteId ? 0.025f : 0.032f);
        const auto color = [&](const char* key, float r, float g, float b, float a)
        {
            ParamEntry* values = cls->AddArray(key);
            for (float value : {r, g, b, a})
                values->AddValue(value);
        };
        color("colorText", 0.94f, 0.95f, 0.97f, 1.0f);
        color("colorSelect", 1.0f, 1.0f, 1.0f, 1.0f);
        color("colorActive", 0.24f, 0.38f, 0.49f, 1.0f);
        const bool button = std::string(base) == "RscButton";
        color("colorBackground", button ? 0.12f : 0.04f, button ? 0.19f : 0.05f, button ? 0.25f : 0.065f,
              id == PanelId ? 0.94f : (button ? 1.0f : 0.0f));
        if (id == NoteId)
        {
            cls->Add("style", ST_MULTI);
            cls->Add("lineSpacing", 1.0f);
        }
        cls->SetBase(inherited->GetClassInterface());
        LoadControl(*cls);
    }

  public:
    explicit ConquestIslandDisplay(ControlsContainer* parent) : Display(parent)
    {
        _idd = ConquestIslandIdd;
        AddControl("RscText", "Panel", PanelId, "", 0.25f, 0.18f, 0.50f, 0.60f);
        AddControl("RscText", "Title", TitleId, "CONQUEST  -  CHOOSE THE ISLAND", 0.28f, 0.21f, 0.44f, 0.05f);
        float y = 0.29f;
        for (int i = 0; i < static_cast<int>(std::size(Islands)); i++, y += 0.07f)
            AddControl("RscButton", ("Island" + std::to_string(i)).c_str(), FirstIslandId + i, Islands[i].label, 0.28f, y,
                       0.44f, 0.055f);
        AddControl("RscText", "Note", NoteId,
                   "Everon's front is drawn by hand; the other islands' wars are made from their own towns. "
                   "The setup screen that follows picks the side and the options.",
                   0.28f, y + 0.01f, 0.44f, 0.08f);
        AddControl("RscButton", "Back", BackId, "Back", 0.28f, y + 0.10f, 0.20f, 0.05f);
    }

    void OnButtonClicked(int idc) override
    {
        if (idc >= FirstIslandId && idc < FirstIslandId + static_cast<int>(std::size(Islands)))
        {
            Choice() = Islands[idc - FirstIslandId].mission;
            Exit(IDC_OK);
            return;
        }
        if (idc == BackId)
        {
            Exit(IDC_CANCEL);
            return;
        }
        Display::OnButtonClicked(idc);
    }
};
} // namespace

Display* CreateConquestIslandDisplay(ControlsContainer* parent)
{
    Choice().clear();
    return new ConquestIslandDisplay(parent);
}

std::string ConquestIslandChoice()
{
    return Choice();
}
} // namespace Poseidon
