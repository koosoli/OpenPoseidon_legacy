// SPDX-License-Identifier: GPL-3.0-or-later
//
// Arma 3's campaign and scenario screens (RscDisplayCampaignLoad, RscDisplaySingleMission), drawn
// by Arma3Menu from Arma 3's config and filled as Arma 3 fills them: from CfgMissions, merged over
// every mounted add-on -- Campaigns, Missions (Scenarios), Showcases, Challenges -- each entry with
// its own briefingName, overviewPicture, overviewText and author. Arma 3's scripts behind those
// screens (search, sorting, the DLC store, the Steam workshop) are not run; their controls are
// hidden. The choice is started by DisplayMain like the other Arma content (ArmaContentChoice).
#include <Poseidon/UI/Arma3Menu.hpp>
#include <Poseidon/UI/ArmaContentMenu.hpp>
#include <Poseidon/UI/Controls/UIControls.hpp>
#include <Poseidon/UI/OptionsUICommon.hpp>
#include <Poseidon/UI/Locale/Stringtable/Stringtable.hpp>
#include <Poseidon/Core/resincl.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Game/Compat/Arma1Compat.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <cstring>
#include <string>
#include <vector>

namespace Poseidon::Arma3Menu
{
namespace
{
// RscDisplaySingleMission / RscDisplayCampaignLoad (a3\ui_f\hpp\defineResincl.inc)
constexpr int kList = 101, kOk = 1, kCancel = 2, kGameOptions = 307;
struct Layout
{
    const char* screen;
    int title, name, author, picture, overview;
    std::vector<int> hidden; // Arma 3's script-driven controls
};
const Layout kMissionLayout{"RscDisplaySingleMission", 106, 110, 111, 112, 113,
                            {104, 105, 107, 108, 109, 114, 117, 118, 119, 120, 121, 122, 645, 646, 1005, 1201,
                             8434}};
const Layout kCampaignLayout{"RscDisplayCampaignLoad", 1000, 104, 105, 106, 107,
                             {102, 103, 108, 110, 111, 112, 645, 646, 1005, 1020, 1201, 8434}};

struct Entry
{
    RString label;
    std::string value; // campaign name, or the mission.sqm path
    bool campaign = false;
    RString picture, overview, author;
    bool described = false; // the mission's description.ext was read for what CfgMissions leaves out
};

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// A config string as Arma 3 shows it: "$STR_..." the config localizes, "@STR_..." (CfgMissions) too.
RString Shown(const ParamEntry& cls, const char* name)
{
    const ParamEntry* e = cls.FindEntry(name);
    if (!e || e->IsClass() || e->IsArray())
        return RString();
    const RString text = *e;
    if (text.GetLength() > 1 && text[0] == '@')
        return LocalizeString(static_cast<const char*>(text) + 1);
    return text;
}

// The first of `names` the class (or its Campaign class) gives.
RString First(const ParamEntry& cls, std::initializer_list<const char*> names)
{
    const ParamEntry* campaign = cls.FindEntry("Campaign");
    for (const char* name : names)
    {
        if (RString text = Shown(cls, name); text.GetLength() > 0)
            return text;
        if (campaign && campaign->IsClass())
            if (RString text = Shown(*campaign, name); text.GetLength() > 0)
                return text;
    }
    return RString();
}

std::string Directory(const ParamEntry& cls)
{
    std::string dir = (const char*)Shown(cls, "directory");
    std::replace(dir.begin(), dir.end(), '/', '\\');
    while (!dir.empty() && (dir.front() == '\\'))
        dir.erase(dir.begin());
    while (!dir.empty() && dir.back() == '\\')
        dir.pop_back();
    return Lower(dir);
}

Entry Describe(const ParamEntry& cls, RString fallbackLabel)
{
    Entry e;
    e.label = First(cls, {"briefingName", "name"});
    // a name whose stringtable is not installed (a DLC's) stays "$STR_..." / "STR_...": the class name
    if (e.label.GetLength() == 0 || e.label[0] == '$' || _strnicmp(e.label, "str_", 4) == 0)
        e.label = fallbackLabel;
    e.picture = First(cls, {"overviewPicture"});
    e.overview = First(cls, {"overviewText"});
    e.author = First(cls, {"author"});
    return e;
}

// Every class under `group` (nested ones too: Challenges keep theirs in categories) that names
// a mission folder holding a mission.sqm.
void CollectMissions(const ParamEntry& group, std::vector<Entry>& out, bool campaignFoldersOnly)
{
    for (int i = 0; i < group.GetEntryCount(); i++)
    {
        const ParamEntry& cls = group.GetEntry(i);
        if (!cls.IsClass())
            continue;
        const std::string dir = Directory(cls);
        if (dir.empty())
        {
            CollectMissions(cls, out, campaignFoldersOnly);
            continue;
        }
        if (campaignFoldersOnly && dir.find("\\campaign\\") == std::string::npos)
            continue;
        const std::string sqm = dir + "\\mission.sqm";
        if (!QIFStreamB::FileExist(sqm.c_str()))
            continue;
        Entry e = Describe(cls, RString(cls.GetName()));
        e.value = sqm;
        out.push_back(e);
    }
}

// The mission archives the engine mounted that CfgMissions does not list (Arma 3's Missions folder,
// the DLC scenarios): Arma 3's scenario list shows those too. Campaign missions, intros and
// cutscenes, multiplayer missions and templates are left out; the label is the folder's name.
void CollectArchives(std::vector<Entry>& out)
{
    for (const std::string& path : Arma1Compat::MissionFiles())
    {
        std::string sqm = Lower(path);
        std::replace(sqm.begin(), sqm.end(), '/', '\\');
        if (sqm.find("mpmissions") != std::string::npos || sqm.find("mpscenarios") != std::string::npos ||
            sqm.find("template") != std::string::npos || sqm.find("\\campaign") != std::string::npos ||
            sqm.find("\\scenes\\") != std::string::npos)
            continue;
        std::string folder = sqm.substr(0, sqm.find_last_of('\\'));
        folder = folder.substr(folder.find_last_of('\\') + 1);
        if (folder.rfind("intro", 0) == 0 || folder.rfind("cs_", 0) == 0 || folder.rfind("mp_", 0) == 0 ||
            folder.rfind("repro_", 0) == 0 || folder.rfind("faction_", 0) == 0 || folder.find('.') == std::string::npos ||
            folder.find("_intro") != std::string::npos || folder.find("camera_shots") != std::string::npos)
            continue;
        if (std::any_of(out.begin(), out.end(), [&](const Entry& e) { return e.value == sqm; }))
            continue;
        Entry e;
        e.label = RString(folder.c_str());
        e.value = sqm;
        out.push_back(e);
    }
}

// What CfgMissions leaves out of a mission's entry -- most give only briefingName and directory --
// from the mission's own description.ext (binarised or text): overviewPicture, overviewText, author,
// and a name for one that has none. Strings of the mission's own stringtable are not loaded; such a
// "$STR_..." is left out rather than shown.
void ReadDescription(Entry& e)
{
    const size_t slash = e.value.find_last_of('\\');
    if (slash == std::string::npos)
        return;
    const std::string dir = e.value.substr(0, slash);
    const std::string ext = dir + "\\description.ext";
    if (!QIFStreamB::FileExist(ext.c_str()))
        return;
    ParamFile file;
    std::string text;
    if (Arma1Compat::Arma1RapFileToText(ext.c_str(), text))
    {
        QIStream in(text.data(), static_cast<int>(text.size()));
        file.Parse(in);
    }
    else
        file.Parse(ext.c_str());
    const auto usable = [](const RString& s)
    { return s.GetLength() > 0 && s[0] != '$' && _strnicmp(s, "str_", 4) != 0 && s[0] != '@'; };
    const auto take = [&](RString& field, const char* name)
    {
        if (field.GetLength() > 0)
            return;
        const RString value = Shown(file, name);
        if (usable(value))
            field = value;
    };
    take(e.overview, "overviewText");
    take(e.author, "author");
    if (e.picture.GetLength() == 0)
    {
        RString picture = Shown(file, "overviewPicture");
        // a picture of the mission's own folder is named relative to it
        if (picture.GetLength() > 0 && picture[0] != '\\' && _strnicmp(picture, "a3\\", 3) != 0)
            picture = RString((dir + "\\").c_str()) + picture;
        e.picture = picture;
    }
    if (const RString name = Shown(file, "onLoadName"); usable(name) && _stricmp(e.label, "") != 0 &&
                                                         _strnicmp(e.label, "showcase_", 9) == 0)
        e.label = name;
}

void CollectCampaigns(const ParamEntry& missions, std::vector<Entry>& out)
{
    if (const ParamEntry* campaigns = missions.FindEntry("Campaigns"); campaigns && campaigns->IsClass())
        for (int i = 0; i < campaigns->GetEntryCount(); i++)
        {
            const ParamEntry& cls = campaigns->GetEntry(i);
            const std::string dir = cls.IsClass() ? Directory(cls) : std::string();
            if (dir.empty() || !QIFStreamB::FileExist((dir + "\\description.ext").c_str()))
                continue;
            Entry e = Describe(cls, RString(cls.GetName()));
            std::string base = dir + "\\";
            std::replace(base.begin(), base.end(), '\\', '/');
            e.value = Arma1Compat::CampaignName(base); // also registers the folder for StartCampaign
            e.campaign = true;
            out.push_back(e);
        }
    // Arma 3 lists scenarios kept in a campaign's folder (Old Man) with the campaigns; they are
    // played as single missions.
    if (const ParamEntry* scenarios = missions.FindEntry("Missions"); scenarios && scenarios->IsClass())
        CollectMissions(*scenarios, out, true);
}

class ContentDisplay : public Display
{
    Screen _screen;
    const Layout& _layout;
    std::vector<Entry> _entries;
    int _chosen = -1;

  public:
    ContentDisplay(ControlsContainer* parent, const Layout& layout, std::vector<Entry> entries, const RString& title,
                   int selected)
        : Display(parent), _layout(layout), _entries(std::move(entries))
    {
        _idd = ArmaContentIdd;
        _screen.Load(*this, layout.screen, _controlsBackground, _controlsForeground);
        for (const int idc : layout.hidden)
            _screen.Show(idc, false);
        _screen.SetText(layout.title, title);
        std::vector<RString> rows;
        for (const Entry& e : _entries)
            rows.push_back(e.label);
        _screen.SetRows(kList, rows, selected);
        ShowEntry(selected);
        NextCtrl();
    }

    bool Choice(bool& campaign, std::string& value) const
    {
        if (_chosen < 0 || _chosen >= static_cast<int>(_entries.size()))
            return false;
        campaign = _entries[_chosen].campaign;
        value = _entries[_chosen].value;
        return true;
    }

    // Arma 3 opens these screens over its main menu's backdrop.
    void OnDraw(EntityAI* vehicle, float alpha) override
    {
        DrawMenuBackdrop(alpha);
        Display::OnDraw(vehicle, alpha);
    }

    void OnLBSelChanged(int idc, int curSel) override
    {
        if (idc == kList)
            ShowEntry(curSel);
    }
    void OnLBDblClick(int idc, int curSel) override
    {
        if (idc == kList)
            Choose(curSel);
    }
    void OnButtonClicked(int idc) override
    {
        if (idc == kOk)
            Choose(_screen.Selected(kList));
        else if (idc == kCancel)
            Exit(IDC_CANCEL);
        else if (idc == kGameOptions)
            CreateChild(CreateOptionsDisplay(this, false, false));
    }

  private:
    void ShowEntry(int index)
    {
        if (index < 0 || index >= static_cast<int>(_entries.size()))
            return;
        Entry& e = _entries[index];
        if (!e.described && !e.campaign)
        {
            e.described = true;
            ReadDescription(e);
        }
        _screen.SetText(_layout.name, e.label);
        _screen.SetText(_layout.author, e.author);
        _screen.SetText(_layout.overview, e.overview);
        _screen.SetPicture(_layout.picture, e.picture);
    }
    void Choose(int index)
    {
        if (index < 0 || index >= static_cast<int>(_entries.size()))
            return;
        _chosen = index;
        Exit(IDC_OK);
    }
};

RString Upper(const RString& text)
{
    std::string s = (const char*)text;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return RString(s.c_str());
}
} // namespace

Display* CreateContentDisplay(ControlsContainer* parent, int idc)
{
    if (!Active())
        return nullptr;
    const ParamEntry* root = Arma1Compat::ConfigRoot();
    const ParamEntry* missions = root ? root->FindEntry("CfgMissions") : nullptr;
    if (!missions || !missions->IsClass())
        return nullptr;
    std::vector<Entry> entries;
    const Layout* layout = &kMissionLayout;
    RString title;
    const auto group = [&](const char* name)
    {
        if (const ParamEntry* g = missions->FindEntry(name); g && g->IsClass())
            CollectMissions(*g, entries, false);
    };
    // The titles RscDisplayMain.sqf passes on (RscDisplaySingleMission_title, ..._CampaignLoad_title).
    switch (idc)
    {
        case 101: // Campaign
        case 149: // Bootcamp
        case 1020: // the campaign spotlight
            layout = &kCampaignLayout;
            CollectCampaigns(*missions, entries);
            title = LocalizeString("STR_A3_RSCDISPLAYMAIN_BUTTONCAMPAIGN");
            break;
        case 150:
            group("Showcases");
            title = LocalizeString("STR_A3_RSCDISPLAYMAIN_BUTTONSHOWCASES");
            break;
        case 151:
            group("Challenges");
            title = LocalizeString("STR_A3_RSCDISPLAYMAIN_BUTTONCHALLENGES");
            break;
        case 103:
            group("Missions");
            CollectArchives(entries);
            title = LocalizeString("STR_A3_RSCDISPLAYMAIN_BUTTONSCENARIOS");
            break;
        case 104: // All Missions
            group("Missions");
            group("Showcases");
            group("Challenges");
            CollectArchives(entries);
            title = LocalizeString("STR_A3_RscDisplayMain_ButtonAllMissions");
            break;
        default:
            return nullptr;
    }
    if (entries.empty())
    {
        LOG_WARN(UI, "Arma 3 menu: CfgMissions has nothing for menu entry {}; the plain list is shown", idc);
        return nullptr;
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b)
              { return _stricmp(a.label, b.label) < 0; });
    int selected = 0;
    if (idc == 149) // Bootcamp: the campaign list on Bootcamp
        for (size_t i = 0; i < entries.size(); i++)
            if (Lower(entries[i].value).find("bootcamp") != std::string::npos)
                selected = static_cast<int>(i);
    if (const char* pick = std::getenv("POSEIDON_MENU_PICK"); pick && *pick) // captures only
        for (size_t i = 0; i < entries.size(); i++)
            if (Lower((const char*)entries[i].label).find(Lower(pick)) != std::string::npos ||
                Lower(entries[i].value).find(Lower(pick)) != std::string::npos)
            {
                selected = static_cast<int>(i);
                break;
            }
    LOG_INFO(UI, "Arma 3 menu: {} lists {} entries from CfgMissions", layout->screen, entries.size());
    return new ContentDisplay(parent, *layout, std::move(entries), Upper(title), selected);
}

bool ContentChoice(ControlsContainer* display, bool& campaign, std::string& value)
{
    auto* content = dynamic_cast<ContentDisplay*>(display);
    return content && content->Choice(campaign, value);
}
} // namespace Poseidon::Arma3Menu
