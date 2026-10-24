// SPDX-License-Identifier: GPL-3.0-or-later
#include <Poseidon/UI/ArmaContentMenu.hpp>
#include <Poseidon/UI/Arma3Menu.hpp>
#include <Poseidon/UI/Controls/UIControls.hpp>
#include <Poseidon/UI/Controls/UIControlsWidgets.hpp>
#include <Poseidon/Core/resincl.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Game/Compat/Arma1Compat.hpp>
#include <Poseidon/Game/Compat/ArmaOriginalUi.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/Config/UserConfig.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/UI/Map/UIMap.hpp>
#include <Poseidon/UI/Locale/Stringtable/Stringtable.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <set>
#include <string>
#include <vector>

namespace Poseidon
{
RString GetOverviewFile(RString dir); // OptionsUIImpl.cpp: a mission folder's localised overview.html

namespace
{
// Arma 1's main menu (its ui.pbo RscDisplayMain): these have no CWA idc of their own.
constexpr int A1Campaign = 120, A1Play = 1050, A1Tutorials = 1110, A1Armory = 1130, A1Options = 1060,
              A1Video = 301, A1Audio = 302, A1Controls = 303, A1Difficulty = 304;
constexpr int ListId = 62101, OkId = IDC_OK, BackId = IDC_CANCEL, TitleId = 62102, PanelId = 62103,
              StatusId = 62104;
// The controls of Arma 1's own RscDisplaySingleMission and RscDisplayCampaignSelect (ui.pbo).
constexpr int NativeList = 101, NativeMissionTitle = 106, NativeMissionOverview = 102, NativeDifficulty = 104,
              NativeLoad = 105, NativeDelete = 107, NativeCopy = 108, NativeEdit = 109, NativeCampaignTitle = 102,
              NativeCampaignOverview = 103;

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

struct Entry
{
    std::string label;
    std::string value;    // campaign name or mission.sqm path
    std::string overview; // folder holding overview.html, with a trailing backslash
};

// A campaign names itself through its own stringtable ("$STRC_RC_NAME"), which is read only once
// the campaign runs; Arma 1's three campaigns by their published names, any other by its folder.
std::string CampaignLabel(const std::string& name)
{
    const std::string n = Lower(name);
    if (n == "ca")
        return "Armed Assault (campaign)";
    if (n == "ra" || n == "rc")
        return "Rahmadi Conflict";
    if (n == "merc")
        return "Royal Flush (Queen's Gambit)";
    // Arma 2 and 3 keep campaigns inside add-on archives; CampaignName joins the archive path
    static const std::pair<const char*, const char*> known[] = {
        {"ca_missions_campaign", "Harvest Red"},
        {"ca_missions_ew_campaign", "Eagle Wing"},
        {"a3_missions_f_bootcamp_campaign", "Bootcamp"},
        {"a3_missions_f_epa_campaign", "The East Wind"},
        {"a3_missions_f_exp_campaign", "Apex Protocol"},
        {"a3_missions_f_oldman_campaign", "Old Man"},
        {"a3_missions_f_orange_campaign", "Laws of War: Remnants of War"},
        {"a3_missions_f_tacops_campaign_1", "Tac-Ops: Beyond Hope"},
        {"a3_missions_f_tacops_campaign_2", "Tac-Ops: Stepping Stone"},
        {"a3_missions_f_tacops_campaign_3", "Tac-Ops: Steel Pegasus"},
        {"a3_missions_f_tank_campaign", "Tanks: Altis Requiem"},
    };
    for (const auto& [folder, label] : known)
        if (n == folder)
            return label;
    return name;
}

// The mounted game's campaigns, single missions or tutorials. Multiplayer missions and editor
// templates are not single-player content; a campaign's missions are played through it.
std::vector<Entry> Collect(ArmaContentKind kind)
{
    std::vector<Entry> out;
    std::set<std::string> campaigns;
    for (const std::string& path : Arma1Compat::MissionFiles())
    {
        const std::string lower = Lower(path);
        if (lower.find("mpmissions") != std::string::npos || lower.find("template") != std::string::npos)
            continue;
        const std::string base = Arma1Compat::CampaignBaseForMission(path);
        if (!base.empty())
        {
            // CampaignName also registers the folder, which StartCampaign then reads from
            const std::string name = Arma1Compat::CampaignName(base);
            if (kind == ArmaContentKind::Campaigns && !name.empty() && campaigns.insert(name).second)
            {
                std::string folder = base;
                std::replace(folder.begin(), folder.end(), '/', '\\');
                out.push_back({CampaignLabel(name), name, folder});
            }
            continue;
        }
        // the mission folder: "...\m02minerconvoy.sara\mission.sqm" -> "m02minerconvoy.sara"
        std::string folder = path.substr(0, path.find_last_of("\\/"));
        folder = folder.substr(folder.find_last_of("\\/") + 1);
        const std::string f = Lower(folder);
        // The menu-background cutscenes (intro.sara, intro.porto, library.sara, armory1.intro) are
        // missions too, but nothing to play.
        if (f.rfind("intro.", 0) == 0 || f.rfind("library.", 0) == 0 || f.rfind("armory", 0) == 0)
            continue;
        // A mission folder carries its world ("sp_eyeforeye.chernarus"); "scripts" and the like do not.
        // Multiplayer scenarios (mp_*) and Arma 3's cutscenes (cs_*, introNN, *_camera_shots,
        // <world>_intro*) are not single-player missions either.
        if (f.find('.') == std::string::npos || f.rfind("mp_", 0) == 0 || f.rfind("cs_", 0) == 0 ||
            f.find("camera_shots") != std::string::npos || f.find("_intro") != std::string::npos ||
            // Arma 3's menu-background intros (introexp.altis, introorange.altis), developer repro
            // missions and faction test scenes are not in its own Scenarios list either
            (Arma1Compat::Generation() == 3 &&
             (f.rfind("intro", 0) == 0 || f.rfind("repro_", 0) == 0 || f.rfind("faction_", 0) == 0)) ||
            (f.size() > 7 && f.rfind("intro", 0) == 0 && std::isdigit(static_cast<unsigned char>(f[5]))))
            continue;
        // Arma 1 mounts Missions/tutorial/t01_obstacle_course.sara.pbo as arma1/missions/
        // t01_obstacle_course.sara: the folder is not in the path, the tNN_ name is.
        const bool tutorial = lower.find("tutorial") != std::string::npos ||
                              (f.size() > 4 && f[0] == 't' && std::isdigit(static_cast<unsigned char>(f[1])) &&
                               std::isdigit(static_cast<unsigned char>(f[2])) && f[3] == '_');
        if (kind == ArmaContentKind::Campaigns || tutorial != (kind == ArmaContentKind::Tutorials))
            continue;
        out.push_back({folder, path, path.substr(0, path.find_last_of("\\/") + 1)});
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return Lower(a.label) < Lower(b.label); });
    return out;
}

// POSEIDON_MENU_PICK=<text> (captures only): the list starts on the first entry whose label or
// value contains <text> (case-insensitive) rather than on the first entry, so a capture driven by
// POSEIDON_MENU_CLICKS can play a chosen campaign or mission.
int InitialEntry(const std::vector<Entry>& entries)
{
    const char* pick = std::getenv("POSEIDON_MENU_PICK");
    if (!pick || !*pick)
        return 0;
    const std::string want = Lower(pick);
    for (size_t i = 0; i < entries.size(); i++)
        if (Lower(entries[i].label).find(want) != std::string::npos ||
            Lower(entries[i].value).find(want) != std::string::npos)
            return static_cast<int>(i);
    return 0;
}

class ArmaContentButton : public CButton
{
  public:
    ArmaContentButton(ControlsContainer* parent, int id, const ParamEntry& cls) : CButton(parent, id, cls)
    {
        _color1 = GetPackedColor(cls >> "colorBackground");
        _color2 = GetPackedColor(cls >> "colorActive");
        _color3 = _color1;
        _color4 = _color2;
        _color5 = _color1;
    }
};

class ArmaContentDisplay : public Display
{
    ParamFile _resources;
    std::vector<Entry> _entries;
    ArmaContentKind _kind;
    bool _native = false;
    bool _cadet = false;
    int _list = ListId;

    // The game's own screen, when it can be drawn: Arma 1's mission and campaign lists.
    bool LoadNative()
    {
        const char* screen =
            _kind == ArmaContentKind::Campaigns ? "RscDisplayCampaignSelect" : "RscDisplaySingleMission";
        const ParamEntry* cls = ArmaOriginalUi::NativeScreen(screen, OkId);
        if (!cls)
            return false;
        Load(*cls);
        _idd = ArmaContentIdd;
        if (!dynamic_cast<CListBox*>(GetCtrl(NativeList)))
            return false;
        _list = NativeList;
        // editing, copying, deleting and loading saves are the editor's and the save system's
        for (int idc : {NativeLoad, NativeDelete, NativeCopy, NativeEdit})
            if (IControl* ctrl = GetCtrl(idc); ctrl && _kind != ArmaContentKind::Campaigns)
                ctrl->ShowCtrl(false);
        // Arma 1's scripts label its mission screen's Continue button; nothing else here does
        if (auto* ok = dynamic_cast<CButton*>(GetCtrl(OkId)); ok && ok->GetText().GetLength() == 0)
        {
            RString text = LocalizeString("STR_CA_MAIN_CONTINUE");
            if (text.GetLength() == 0 || text == RString("STR_CA_MAIN_CONTINUE"))
                text = LocalizeString("STR_DISP_OK");
            ok->SetText(text);
        }
        _cadet = USER_CONFIG.easyMode;
        ShowDifficulty();
        return true;
    }

    void ShowDifficulty()
    {
        if (_kind == ArmaContentKind::Campaigns)
            return;
        if (auto* button = dynamic_cast<CButton*>(GetCtrl(NativeDifficulty)))
            button->SetText(_cadet ? "Cadet" : "Veteran");
        else if (auto* text = dynamic_cast<CActiveText*>(GetCtrl(NativeDifficulty)))
            text->SetText(_cadet ? "Cadet" : "Veteran");
    }

    // The selected entry's name and overview.html in the native screen's title and HTML panel.
    void ShowSelected()
    {
        const int sel = Selected();
        if (!_native || sel < 0)
            return;
        const bool campaign = _kind == ArmaContentKind::Campaigns;
        if (auto* title = dynamic_cast<CStatic*>(GetCtrl(campaign ? NativeCampaignTitle : NativeMissionTitle)))
            title->SetText(_entries[sel].label.c_str());
        if (auto* html = dynamic_cast<CHTMLContainer*>(GetCtrl(campaign ? NativeCampaignOverview : NativeMissionOverview)))
        {
            html->Init();
            const RString file = GetOverviewFile(RString(_entries[sel].overview.c_str()));
            if (file.GetLength() > 0)
                html->Load(file);
        }
    }

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
        cls->Add("sizeEx", id == StatusId ? 0.025f : 0.032f);
        if (const ParamEntry* inheritedSize = inherited->FindEntry("size"))
            cls->Add("size", static_cast<float>(*inheritedSize) * 1.6f);
        cls->Add("rowHeight", 0.033f);
        const auto color = [&](const char* key, float r, float g, float b, float a)
        {
            ParamEntry* values = cls->AddArray(key);
            for (float value : {r, g, b, a})
                values->AddValue(value);
        };
        color("colorText", 0.94f, 0.95f, 0.97f, 1.0f);
        if (id == ListId)
            color("colorSelect", 0.03f, 0.08f, 0.14f, 1.0f); // dark text on the light selection bar
        else
            color("colorSelect", 1.0f, 1.0f, 1.0f, 1.0f);
        color("colorActive", 0.24f, 0.38f, 0.49f, 1.0f);
        const bool button = std::string(base) == "RscButton";
        color("colorBackground", button ? 0.12f : 0.04f, button ? 0.19f : 0.05f, button ? 0.25f : 0.065f,
              id == PanelId ? 0.94f : (button ? 1.0f : 0.0f));
        if (id == StatusId)
        {
            cls->Add("style", ST_MULTI);
            cls->Add("lineSpacing", 1.0f);
        }
        cls->SetBase(inherited->GetClassInterface());
        LoadControl(*cls);
    }

  public:
    ArmaContentDisplay(ControlsContainer* parent, ArmaContentKind kind) : Display(parent), _kind(kind)
    {
        _idd = ArmaContentIdd;
        _entries = Collect(kind);
        _native = LoadNative();
        if (_native)
        {
            if (auto* list = dynamic_cast<CListBox*>(GetCtrl(_list)))
            {
                for (const Entry& e : _entries)
                    list->AddString(e.label.c_str());
                list->SetCurSel(InitialEntry(_entries));
            }
            ShowSelected();
            LOG_INFO(UI, "{} content list (its own {} screen): {} entries", Arma1Compat::Label(),
                     kind == ArmaContentKind::Campaigns ? "campaign" : "mission", _entries.size());
            return;
        }
        const std::string title = std::string(Arma1Compat::Label()) +
                                  (kind == ArmaContentKind::Campaigns   ? " -- campaigns"
                                   : kind == ArmaContentKind::Tutorials ? " -- tutorials"
                                                                        : " -- missions");
        AddControl("RscText", "Panel", PanelId, "", 0.04f, 0.10f, 0.92f, 0.80f);
        AddControl("RscText", "Title", TitleId, title.c_str(), 0.06f, 0.12f, 0.88f, 0.05f);
        AddControl("RscListBox", "List", ListId, "", 0.06f, 0.19f, 0.88f, 0.55f);
        AddControl("RscText", "Status", StatusId, "", 0.06f, 0.75f, 0.88f, 0.06f);
        AddControl("RscButton", "Play", OkId, "Play", 0.06f, 0.83f, 0.30f, 0.05f);
        AddControl("RscButton", "Back", BackId, "Back", 0.74f, 0.83f, 0.20f, 0.05f);
        if (auto* list = dynamic_cast<CListBox*>(GetCtrl(ListId)))
        {
            for (const Entry& e : _entries)
                list->AddString(e.label.c_str());
            list->SetCurSel(InitialEntry(_entries));
        }
        if (auto* status = dynamic_cast<CStatic*>(GetCtrl(StatusId)))
            status->SetText(_entries.empty()
                                ? RString("Nothing of this kind was found in the mounted game's archives.")
                                : RString("Played from the game's own archives; units the engine lacks are stood in "
                                          "for by OFP ones."));
        LOG_INFO(UI, "{} content list: {} {}", Arma1Compat::Label(), _entries.size(),
                 kind == ArmaContentKind::Campaigns ? "campaigns" : "missions");
    }

    Control* OnCreateCtrl(int type, int idc, const ParamEntry& cls) override
    {
        if (type == CT_BUTTON)
            return new ArmaContentButton(this, idc, cls);
        return Display::OnCreateCtrl(type, idc, cls);
    }

    void OnButtonClicked(int idc) override
    {
        if (_native && idc == NativeDifficulty && _kind != ArmaContentKind::Campaigns)
        {
            _cadet = !_cadet;
            ShowDifficulty();
            return;
        }
        if (idc == OkId && Selected() < 0)
            return;
        if (idc == OkId && _native && _kind != ArmaContentKind::Campaigns)
            USER_CONFIG.easyMode = _cadet;
        Display::OnButtonClicked(idc); // OK / Cancel close the display with that exit code
    }

    void OnLBSelChanged(int idc, int curSel) override
    {
        if (idc == _list)
            ShowSelected();
        Display::OnLBSelChanged(idc, curSel);
    }

    void OnLBDblClick(int idc, int curSel) override
    {
        if (idc == _list)
            OnButtonClicked(OkId);
        else
            Display::OnLBDblClick(idc, curSel);
    }

    int Selected()
    {
        auto* list = dynamic_cast<CListBox*>(GetCtrl(_list));
        const int sel = list ? list->GetCurSel() : -1;
        return sel >= 0 && sel < static_cast<int>(_entries.size()) ? sel : -1;
    }

    bool Choice(bool& campaign, std::string& value)
    {
        const int sel = Selected();
        if (sel < 0)
            return false;
        campaign = _kind == ArmaContentKind::Campaigns;
        value = _entries[sel].value;
        return true;
    }
};
} // namespace

bool ArmaMenuContentKind(int idc, ArmaContentKind& kind)
{
    if (!Arma1Compat::Active())
        return false;
    switch (idc)
    {
        case IDC_MAIN_GAME: // CWA's Campaign entry
        case A1Campaign:
        case A1Play: // Arma 1's Play opens its campaign / missions / tutorial row; the campaign is its first
            kind = ArmaContentKind::Campaigns;
            return true;
        case IDC_MAIN_SINGLE:
            kind = ArmaContentKind::Missions;
            return true;
        case A1Tutorials:
            kind = ArmaContentKind::Tutorials;
            return true;
        default:
            return false;
    }
}

bool ArmaMenuOpensOptions(int idc)
{
    return Arma1Compat::Active() &&
           (idc == A1Options || idc == A1Video || idc == A1Audio || idc == A1Controls || idc == A1Difficulty);
}

const char* ArmaMenuUnavailable(int idc)
{
    if (Arma1Compat::Active() && idc == A1Armory)
        return "The Armory is not available in OpenPoseidon: Arma builds it from scripts this engine does not run.";
    return nullptr;
}

Display* CreateArmaContentDisplay(ControlsContainer* parent, ArmaContentKind kind)
{
    return new ArmaContentDisplay(parent, kind);
}

bool ArmaContentChoice(ControlsContainer* display, bool& campaign, std::string& value)
{
    if (Arma3Menu::ContentChoice(display, campaign, value))
        return true;
    auto* content = dynamic_cast<ArmaContentDisplay*>(display);
    return content && content->Choice(campaign, value);
}
void TickMenuClickHook()
{
    static const std::vector<int> clicks = []
    {
        std::vector<int> out;
        if (const char* spec = std::getenv("POSEIDON_MENU_CLICKS"))
        {
            std::stringstream ss(spec);
            for (std::string token; std::getline(ss, token, ',');)
                if (!token.empty())
                    out.push_back(std::atoi(token.c_str()));
        }
        return out;
    }();
    static size_t next = 0;
    static bool started = false;
    static Foundation::UITime last;
    if (next >= clicks.size() || !GWorld)
        return;
    auto* top = dynamic_cast<ControlsContainer*>(GWorld->Options());
    if (!top)
        return;
    if (!started)
    {
        started = true;
        last = Glob.uiTime;
        return;
    }
    if (Glob.uiTime - last < 2.0f)
        return;
    last = Glob.uiTime;
    while (top->Child())
        top = top->Child();
    LOG_INFO(UI, "POSEIDON_MENU_CLICKS: pressing idc {} on display {}", clicks[next], top->IDD());
    if (clicks[next] == -3) // -3: run POSEIDON_MENU_SCRIPT (e.g. arm the player for a capture)
    {
        if (const char* script = std::getenv("POSEIDON_MENU_SCRIPT"); script && *script && GWorld->GetGameState())
        {
            LOG_INFO(UI, "POSEIDON_MENU_CLICKS: running {}", script);
            GWorld->GetGameState()->EvaluateMultiple(script);
        }
        next++;
        return;
    }
    if (clicks[next] == -2) // -2: V in a mission, the player's aiming (gunner) view
    {
        if (Object* on = GWorld->CameraOn())
            GWorld->SwitchCameraTo(on, CamGunner);
        next++;
        return;
    }
    if (clicks[next] == -1) // -1: Esc in a mission, the pause menu
    {
        if (auto* mission = dynamic_cast<DisplayMission*>(top))
            mission->OpenInterrupt();
        next++;
        return;
    }
    top->OnButtonClicked(clicks[next++]);
}
} // namespace Poseidon
