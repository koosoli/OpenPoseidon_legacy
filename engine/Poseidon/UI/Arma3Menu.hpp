// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Arma 3's own main menu (--arma3 with --arma-original-ui).
//
// Arma 3 does not draw its screens the way CWA, Arma 1 and Arma 2 do. RscDisplayMain is a
// tree of controls groups (type 15) holding shortcut buttons (type 16), structured text
// (type 13) and pictures, sized in pixel-grid expressions ("10 * (pixelW * pixelGrid * 2)"),
// and its behaviour -- the drop-down rows under SINGLEPLAYER / TUTORIALS / OPTIONS, the
// spotlight tiles -- is RscDisplayMain.sqf run through BIS_fnc_initDisplay. This engine has
// none of those control types and does not run that script, so the ArmaOriginalUi merge
// (Arma 1 / 2) cannot draw it.
//
// Here the screen is read straight from Arma 3's config (Arma1Compat::ConfigRoot, never
// merged into Res): groups are flattened into the display at their offset and shown/hidden
// as a unit, and each control is drawn by a small Arma 3 control of this file that reads
// Arma 3's members (animTexture*, colorBackgroundFocused, TextPos, Attributes, ...). What
// RscDisplayMain.sqf does is done natively: a title opens its row on hover, a spotlight
// shows its hover frame. Videos and the Steam info panels are skipped and logged.

#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>

#include <memory>
#include <string>
#include <vector>

class Control;
class ControlsContainer;
class IControl;
class Display;

namespace Poseidon::Arma3Menu
{
// --arma-original-ui was given, Arma 3 is mounted and its config has RscDisplayMain.
bool Active();

// Arma 3's campaign or scenario screen for main-menu entry `idc` (Campaign, Scenarios, Showcases,
// Challenges, All Missions), filled from CfgMissions (Arma3Screens.cpp); null when Arma 3's menu is
// not active or CfgMissions has nothing for it. Its idd is ArmaContentIdd.
Display* CreateContentDisplay(ControlsContainer* parent, int idc);
// An Arma 3 screen a script opens (createDisplay / createDialog) that this engine runs natively,
// in place of the scripts behind it (Arma3Screens.cpp); null for any other class.
Display* CreateScriptDisplay(ControlsContainer* parent, const char* className);

// ctrlSetText / ctrlSetStructuredText / ctrlText on a control of an Arma 3 screen (they are not CWA's
// CStatic and the like though their types read alike); false for any other control.
bool SetControlText(IControl* control, const RString& text);
bool ControlText(IControl* control, RString& text);

// The entry chosen in a closed CreateContentDisplay screen.
bool ContentChoice(ControlsContainer* display, bool& campaign, std::string& value);

// The backdrop of Arma 3's main menu (when it is up), for its campaign / scenario screens.
void DrawMenuBackdrop(float alpha);

// Called each frame while Arma 3's main menu is up: its menu music, started and looped as its intro
// scenes do.
void TickMenuMusic();

// Arma 3's HUD (RscInGameUI): its soldier's weapon panel (RscUnitInfoSoldier) and stance indicator
// (RscStanceInfo), drawn in place of CWA's unit info. The in-game UI fills what it shows each frame.
struct HudState
{
    RString weapon, ammo, magazines, ammoType, grenade, grenades;
    int stance = 0; // 0 standing, 1 crouched, 2 prone
    bool weaponShown = true;
};
// Arma 3's menu is active and its config has RscInGameUI.
bool HudActive();
void DrawHud(const HudState& state, float alpha);

// What a click on an Arma 3 main-menu entry does here.
enum class Action
{
    None,        // not an Arma 3 entry: DisplayMain's own handling
    Handled,     // the menu used it itself (a row title)
    Campaigns,   // the mounted game's campaigns (ArmaContentMenu)
    Missions,    // its scenarios, showcases and challenges
    Tutorials,   // its tutorials and VR training
    Options,     // CWA's Options screen (Video, Audio, Controls, Game)
    Unavailable, // with a message: needs what this engine does not have
};

// A loaded Arma 3 screen: its controls live in the display's own lists; this keeps how they
// nest in groups, for showing and hiding.
class Screen
{
  public:
    Screen();
    ~Screen();

    // Loads Arma 3's class `name` ("RscInGameUI/RscStanceInfo" for a nested one) into `display`,
    // appending to its control lists; without `backdrop` its ControlsBackground is left out (the map
    // screens: the engine's own map is the backdrop).
    bool Load(ControlsContainer& display, const char* name, RefArray<Control>& background,
              RefArray<Control>& foreground, bool backdrop = true);

    // RscDisplayMain.sqf's onLoad, natively: rows collapsed, spotlight frames hidden.
    void InitMainMenu();
    // Per frame, with the cursor in screen units: rows open under the title the cursor is on
    // and stay open while it is over them; a spotlight shows its frame under the cursor.
    void UpdateMainMenu(float mouseX, float mouseY);
    // The controls (or groups) with `idc`: their text, picture, rows; whether they are shown.
    void SetText(int idc, const RString& text);
    void SetPicture(int idc, const RString& name);
    void SetRows(int idc, const std::vector<RString>& rows, int selected);
    int Selected(int idc) const;
    bool Checked(int idc) const; // a check box
    void SetChecked(int idc, bool checked);
    void Show(int idc, bool shown);
    bool Has(int idc) const;
    // The meaning of a main-menu click; `message` is set for Action::Unavailable.
    Action MainMenuClick(int idc, const char*& message);

    // Its ControlsBackground (the backdrop) alone, as drawn under a screen opened over it.
    void DrawBackdrop(float alpha);
    // All of it that is shown, in order (a screen that is not a display of its own: the HUD).
    void Draw(float alpha);

    // RscDisplayInterrupt (the pause menu): what applies here shown, the mission's name set; and
    // a click's idc as CWA's pause menu knows it (Arma 3's Load, the Options shortcuts).
    void InitInterrupt(bool multiplayer, bool saveExists, const RString& missionName);
    int InterruptClick(int idc) const;

    // RscDisplayGetReady / RscDisplayMainMap (the briefing and the map): the diary over the map. Its
    // subjects, their records and the chosen record's page come from the mission's diary and tasks
    // (createDiaryRecord, createSimpleTask); what scripts add while it is open shows up
    // (UpdateDiary, per frame, with the clock). DiaryEvent takes a list's selection.
    void InitDiary(const RString& missionName, const RString& playerName);
    void UpdateDiary();
    bool DiaryEvent(int idc, int row);

  private:
    void AddCentreSpotlight();

    struct Impl;
    std::unique_ptr<Impl> _impl;
};
} // namespace Poseidon::Arma3Menu
