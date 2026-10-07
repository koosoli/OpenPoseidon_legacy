// Adapted from the author-supplied CWR-Physical Inventory source donation (2026).
// Distributed under this project's GPL-3.0-or-later licence and Section 7 terms; see LICENSE.
#include "InventoryModel.hpp"
#include "../../World/Entities/Weapons/ItemMass.hpp"

#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <Poseidon/World/Entities/Infantry/ManActs.hpp>
#include <Poseidon/World/Entities/Infantry/SoldierOld.hpp>
#include <Poseidon/World/Entities/Weapons/Weapons.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/AI/VehicleAI.hpp>
#include <Poseidon/AI/EntityAI.hpp>
#include <Poseidon/AI/EntityAIType.hpp>
#include <Poseidon/AI/AIUnit.hpp>
#include <Poseidon/AI/AIGroup.hpp>
#include <Poseidon/AI/AICenter.hpp>
#include <Poseidon/World/Scene/Object.hpp>   // TargetSide
#include <Poseidon/Network/Network.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/Foundation/platform.hpp>   // strcmpi

#include <cstring>   // strcmp
#include <Poseidon/Foundation/Common/GamePaths.hpp>
#include <filesystem>
#include <cmath>
#include <cstdio>    // fopen (settings file)

namespace Poseidon
{

namespace
{
FILE *OpenInventoryUserFile(const char *name, const char *mode)
{
    const auto &userDir = Foundation::GamePaths::Instance().UserDir();
    if (userDir.empty()) return nullptr;
    std::error_code ec;
    std::filesystem::path directory(userDir);
    if (mode[0] == 'w')
    {
        std::filesystem::create_directories(directory, ec);
        if (ec) return nullptr;
    }
    return fopen((directory / name).string().c_str(), mode);
}
}

// --- Weapon hotkeys & inventory settings ----------------------------------

// A hotkey binding. `cls` is a KIND-QUALIFIED key ("w:M16" / "m:HandGrenade") so a
// weapon and its like-named magazine don't collide (OFP gives them the same class
// name). `inst` is the specific clicked item pointer (WeaponType*/Magazine*) - used
// only so the number badge shows on the ONE item clicked, not every copy.
struct HotkeyBind { RString cls; const void *inst = nullptr; };
static HotkeyBind g_weaponHotkeys[10];
static int  g_invOpenKey  = 18;      // SDL_SCANCODE_O (matches the UAInventory default)
static bool g_invEditMenu = false;

// --- Global inventory UI settings (edit menu; persisted to the cfg file) -----
// Defaults live in one place so "Reset to default" is a straight copy-back.
struct InvUISettings
{
    float iconScale   = 1.00f;   //!< multiplies EVERY icon's drawn size (0.40..2.00)
    int   gridCols    = 7;       //!< inventory grid columns (1..12)
    float volCapOverride = 0.0f; //!< 0 = use per-side default; else litres (5..150)
    bool  accentTheme  = false;  //!< tint panel backgrounds/headers with the accent
    bool  accentBorder = false;  //!< tint borders/outlines with the accent
    float accentR = 0.35f;       //!< accent colour (also used for highlights)
    float accentG = 0.62f;
    float accentB = 1.00f;
    float vicW = 1.00f;          //!< vicinity panel width  scale (0.50..1.60)
    float vicH = 1.00f;          //!< vicinity panel height scale
    float invW = 1.00f;          //!< inventory panel width  scale
    float invH = 1.00f;          //!< inventory panel height scale
    bool  weaponsInBackpack = false; //!< allow rifles/launchers in the grid (as 3x2 tiles)
    bool  infiniteWeight  = false;   //!< ignore the carry-weight cap
    bool  infiniteVolume  = false;   //!< ignore the volume cap
};
static InvUISettings g_ui;

float InventoryGlobalIconScale()            { return g_ui.iconScale; }
void  SetInventoryGlobalIconScale(float v)  { g_ui.iconScale = v < 0.40f ? 0.40f : (v > 2.00f ? 2.00f : v); }
int   InventoryGridCols()                   { return g_ui.gridCols; }
void  SetInventoryGridCols(int c)           { g_ui.gridCols = c < 1 ? 1 : (c > 12 ? 12 : c); }
float InventoryVolumeCapOverride()          { return g_ui.volCapOverride; }
void  SetInventoryVolumeCapOverride(float v){ g_ui.volCapOverride = v < 0.0f ? 0.0f : (v > 150.0f ? 150.0f : v); }
bool  InventoryAccentTheming()              { return g_ui.accentTheme; }
void  SetInventoryAccentTheming(bool b)     { g_ui.accentTheme = b; }
bool  InventoryAccentBorders()              { return g_ui.accentBorder; }
void  SetInventoryAccentBorders(bool b)     { g_ui.accentBorder = b; }
void  InventoryAccentColor(float &r, float &g, float &b) { r = g_ui.accentR; g = g_ui.accentG; b = g_ui.accentB; }
void  SetInventoryAccentColor(float r, float g, float b)
{
    auto cl = [](float x){ return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); };
    g_ui.accentR = cl(r); g_ui.accentG = cl(g); g_ui.accentB = cl(b);
}
float InventoryVicWinScaleW() { return g_ui.vicW; }
float InventoryVicWinScaleH() { return g_ui.vicH; }
float InventoryInvWinScaleW() { return g_ui.invW; }
float InventoryInvWinScaleH() { return g_ui.invH; }
static float ClampWin(float v) { return v < 0.50f ? 0.50f : (v > 1.60f ? 1.60f : v); }
void  SetInventoryVicWinScaleW(float v) { g_ui.vicW = ClampWin(v); }
void  SetInventoryVicWinScaleH(float v) { g_ui.vicH = ClampWin(v); }
void  SetInventoryInvWinScaleW(float v) { g_ui.invW = ClampWin(v); }
void  SetInventoryInvWinScaleH(float v) { g_ui.invH = ClampWin(v); }
bool  InventoryWeaponsInBackpack()          { return g_ui.weaponsInBackpack; }
void  SetInventoryWeaponsInBackpack(bool b) { g_ui.weaponsInBackpack = b; }
bool  InventoryInfiniteWeight()             { return g_ui.infiniteWeight; }
void  SetInventoryInfiniteWeight(bool b)    { g_ui.infiniteWeight = b; }
bool  InventoryInfiniteVolume()             { return g_ui.infiniteVolume; }
void  SetInventoryInfiniteVolume(bool b)    { g_ui.infiniteVolume = b; }
void  ResetInventorySettingsToDefault() { g_ui = InvUISettings(); }

void SetWeaponHotkey(int slot, RString weaponName, const void *inst)
{
    if (slot >= 0 && slot < 10)
    {
        g_weaponHotkeys[slot].cls = weaponName;
        g_weaponHotkeys[slot].inst = inst;
    }
}

RString GetWeaponHotkey(int slot)
{
    return (slot >= 0 && slot < 10) ? g_weaponHotkeys[slot].cls : RString();
}

int WeaponHotkeyBadgeSlot(const char *key, const void *inst)
{
    if (!key || !*key)
        return -1;
    const bool isMag = (key[0] == 'm' && key[1] == ':');
    for (int s = 0; s < 10; s++)
    {
        const HotkeyBind &b = g_weaponHotkeys[s];
        if (b.cls.GetLength() == 0 || strcmp((const char *)b.cls, key) != 0)
            continue;
        // Throwables share a class, so a magazine badge only lights the exact clicked
        // instance - not every grenade of that type.
        if (isMag && b.inst && inst && b.inst != inst)
            continue;
        return s;
    }
    return -1;
}

bool ApplyWeaponHotkey(Person *player, int slot)
{
    if (!player)
        return false;
    RString wn = GetWeaponHotkey(slot);
    if (wn.GetLength() == 0)
        return false;
    const char *key = (const char *)wn;

    // A slot binding ("slot:0/1/2") selects whatever weapon currently occupies the
    // primary / secondary / handgun loadout slot - so the key follows the slot, not a
    // specific weapon, and keeps working after the player swaps guns.
    if (key[0] == 's' && key[1] == 'l' && key[2] == 'o' && key[3] == 't' && key[4] == ':')
    {
        const int kind = key[5] - '0';
        const int mask = (kind == 0) ? MaskSlotPrimary
                       : (kind == 1) ? MaskSlotSecondary
                                     : MaskSlotHandGun;
        for (int i = 0; i < player->NMagazineSlots(); i++)
        {
            const MagazineSlot &ms = player->GetMagazineSlot(i);
            if (ms._weapon && (ms._weapon->_weaponType & mask))
            {
                player->SelectWeapon(i, true);
                return true;
            }
        }
        return false;
    }

    // Bindings are kind-qualified ("w:<weapon>" / "m:<magazine>") because a weapon and
    // its magazine share a class name in OFP. Strip the prefix; tolerate legacy
    // unprefixed keys by trying the weapon path then the throwable path.
    const bool isMag = (key[0] == 'm' && key[1] == ':');
    const bool isWpn = (key[0] == 'w' && key[1] == ':');
    const char *name = (isMag || isWpn) ? key + 2 : key;

    // IMPORTANT: EntityAI::SelectWeapon() indexes MAGAZINE SLOTS (muzzles), not the
    // weapon-systems list. The old code passed a weapon-systems index straight to
    // SelectWeapon(), so anything whose slot index differed (notably the launcher)
    // selected the wrong slot and the keypress fell through to the command menu.

    // A real weapon (rifle / launcher / pistol): match the slot's muzzle weapon.
    if (!isMag)
    {
        for (int i = 0; i < player->NMagazineSlots(); i++)
        {
            const MagazineSlot &ms = player->GetMagazineSlot(i);
            if (ms._weapon && strcmp((const char *)ms._weapon->GetName(), name) == 0)
            {
                player->SelectWeapon(i, true);
                return true;
            }
        }
    }

    // A throwable (grenade / mine / satchel): the binding is a MAGAZINE class.
    if (!isWpn)
    {
        // Selecting a throwable must preserve ordinary reload/cooldown timing.
        for (int i = 0; i < player->NMagazineSlots(); i++)
        {
            const MagazineSlot &ms = player->GetMagazineSlot(i);
            if (ms._magazine && ms._magazine->_type &&
                strcmp((const char *)ms._magazine->_type->GetName(), name) == 0)
            {
                player->SelectWeapon(i, true);
                return true;
            }
        }
        // Otherwise (e.g. smoke while frag is loaded, or a mine/satchel not yet in
        // hand): request the normal timed magazine swap, then select its muzzle.
        for (int m = 0; m < player->NMagazines(); m++)
        {
            Magazine *mag = player->GetMagazine(m);
            if (!mag || !mag->_type ||
                strcmp((const char *)mag->_type->GetName(), name) != 0)
                continue;
            for (int i = 0; i < player->NMagazineSlots(); i++)
            {
                const MagazineSlot &ms = player->GetMagazineSlot(i);
                if (ms._muzzle && ms._muzzle->CanUse(mag->_type))
                {
                    if (!player->ReloadMagazineTimed(i, m, false))
                        continue;
                    player->SelectWeapon(i, true);
                    return true;
                }
            }
        }
    }
    return false;
}

int  InventoryOpenKey()          { return g_invOpenKey; }
bool InventoryEditMenuVisible()  { return g_invEditMenu; }
void SetInventoryOpenKey(int sc) { g_invOpenKey = sc; }
void SetInventoryEditMenuVisible(bool s) { g_invEditMenu = s; }

// The player's explicitly-chosen weapon for each correctable slot, set when a weapon is
// equipped from the inventory (dragging it onto a loadout slot). This lets you switch
// between two guns of the same slot: the enforcement and the loadout strip keep THIS one
// wielded/shown instead of always defaulting to "the first". nullptr = no explicit choice
// yet (use the first). One player (single-player), so a simple global suffices.
static const WeaponType *g_equipPref[3] = { nullptr, nullptr, nullptr };  // 0=Primary 1=Secondary 2=HandGun

static int SlotPrefIndex(int weaponType)
{
    if (weaponType & MaskSlotPrimary)   return 0;
    if (weaponType & MaskSlotSecondary) return 1;
    if (weaponType & MaskSlotHandGun)   return 2;
    return -1;
}

// The weapon that should be wielded/shown in a slot: the player's chosen one if they still
// carry it, otherwise the first weapon system with that slot bit. `outIdx` = its weapon
// system index, or -1.
static const WeaponType *EquippedForSlot(EntityAI *unit, int slotMask, int &outIdx)
{
    outIdx = -1;
    if (!unit)
        return nullptr;
    const int pi = SlotPrefIndex(slotMask);
    const WeaponType *pref = (pi >= 0) ? g_equipPref[pi] : nullptr;
    if (pref)   // honour the choice only while it's actually still carried
    {
        for (int i = 0; i < unit->NWeaponSystems(); i++)
            if (unit->GetWeaponSystem(i) == pref) { outIdx = i; return pref; }
    }
    for (int i = 0; i < unit->NWeaponSystems(); i++)
    {
        const WeaponType *w = unit->GetWeaponSystem(i);
        if (w && (w->_weaponType & slotMask)) { outIdx = i; return w; }
    }
    return nullptr;
}

// Keep the wielded gun consistent when "Rifles in backpack" is on. That option leaves
// a spare gun (e.g. a second AK, or a second pistol) as a second ACTIVE weapon system
// in the SAME slot as the equipped one. The vanilla weapon cycle selects by MUZZLE
// SLOT and can land on the spare's muzzle, so you get one gun's model in hand but
// another's name / sounds / fire-modes. This runs each in-game frame and, if the
// currently selected weapon is NOT the equipped weapon of its slot (the first weapon
// system with that slot bit, matching ScanLoadout's loadout slot), re-selects the
// equipped one's muzzle.
//
// It covers every gun slot the "Rifles in backpack" option can leave a spare active in:
// primary (rifle/MG/SMG), secondary (rocket/AT launcher) and handgun. Originally only
// primaries were corrected, so the identical desync reappeared first on pistols and
// would equally hit launchers (the backpack holds those too). Grenade / binocular /
// item selection is left alone, and it's a no-op unless a second, different weapon of
// the same slot is actually being wielded.
void EnforceEquippedPrimaryWeapon(EntityAI *player)
{
    if (!player || !InventoryWeaponsInBackpack())
        return;

    const int sel = player->SelectedWeapon();
    if (sel < 0 || sel >= player->NMagazineSlots())
        return;

    const WeaponType *selW = player->GetMagazineSlot(sel)._weapon;
    if (!selW)
        return;

    // Which correctable slot does the selected weapon belong to? Primary, secondary
    // (launcher) and handgun all get the "spare stays active" treatment, so all three
    // can desync; nothing else can.
    const int slot = selW->_weaponType & (MaskSlotPrimary | MaskSlotSecondary | MaskSlotHandGun);
    if (slot == 0)
        return;

    // Equipped = the player's chosen weapon for this slot (if still carried), else the
    // first weapon system with that slot bit - same rule ScanLoadout uses, so the wielded
    // gun matches the loadout strip.
    int equippedIdx = -1;
    const WeaponType *equipped = EquippedForSlot(player, slot, equippedIdx);
    // Nothing to fix if none found, or the equipped one is already what's selected
    // (two identical guns share one WeaponType, so that's a no-op too).
    if (!equipped || equipped == selW)
        return;

    for (int i = 0; i < player->NMagazineSlots(); i++)
    {
        if (player->GetMagazineSlot(i)._weapon == equipped)
        {
            player->SelectWeapon(i, true);
            return;
        }
    }
}

static bool NameHasCI(const char *hay, const char *needleLower);   // defined later in this file

// Break-action single load: a real over/under (the Kozlice) has two barrels that share
// ONE ammo type at a time, but the engine gives each muzzle its own magazine slot - so a
// soldier can end up with both a ball AND a shell magazine chambered at once (the "acts
// as two separate magazines" bug). Keep at most one of the Kozlice's muzzles loaded:
// prefer the currently-selected muzzle if it's loaded, else the first loaded one, and
// unchamber the rest (their magazines stay in the unit's list, so they return to the
// inventory as spares). Runs each in-game frame for the player. STRICTLY gated to the
// Kozlice by weapon name, so ordinary multi-muzzle weapons (rifle + grenade launcher)
// keep both muzzles loaded exactly as before.
// Auto-remove spare magazines that are completely empty (0 rounds). Vanilla only destroys
// an empty mag when it's replaced during a reload, so an empty mag that was picked up, or
// left over after a weapon/ammo swap, lingers uselessly (the reported 0-round Beretta mag).
// This clears any 0-round magazine that is NOT currently chambered in a muzzle - a
// chambered-empty is left alone so the vanilla reload can consume it. Player, each frame.
void CleanupEmptyMagazines(EntityAI *player)
{
    if (!player)
        return;
    for (int i = 0; i < player->NMagazines();)
    {
        const Magazine *m = player->GetMagazine(i);
        if (!m || (int)m->_ammo > 0)
        {
            i++;
            continue;
        }
        bool chambered = false;
        for (int k = 0; k < player->NMagazineSlots(); k++)
        {
            if (player->GetMagazineSlot(k)._magazine == m) { chambered = true; break; }
        }
        if (chambered)
        {
            i++;
            continue;
        }
        player->RemoveMagazine(m);   // removes from the list - do NOT advance i
    }
}

void LoadInventorySettings()
{
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    FILE *f = OpenInventoryUserFile("inventory_settings.cfg", "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char key[128];
        double val;
        if (sscanf(line, " %127[^= ] = %lf", key, &val) != 2 || !std::isfinite(val))
            continue;

        if (strstr(key, "inventoryKey"))            g_invOpenKey = (int)val;
        // showEditMenu is intentionally NOT restored: the debug/edit menu always
        // starts hidden and is only turned on in-session via its bound key, so it
        // never pops up by default even if a tuning session left it enabled.
        else if (strstr(key, "iconScale"))          SetInventoryGlobalIconScale((float)val);
        else if (strstr(key, "gridCols"))           SetInventoryGridCols((int)val);
        else if (strstr(key, "volCapOverride"))      SetInventoryVolumeCapOverride((float)val);
        else if (strstr(key, "accentTheme"))         SetInventoryAccentTheming(val != 0.0);
        else if (strstr(key, "accentBorder"))        SetInventoryAccentBorders(val != 0.0);
        else if (strstr(key, "accentR"))             g_ui.accentR = (float)val;
        else if (strstr(key, "accentG"))             g_ui.accentG = (float)val;
        else if (strstr(key, "accentB"))             g_ui.accentB = (float)val;
        else if (strstr(key, "vicW"))                SetInventoryVicWinScaleW((float)val);
        else if (strstr(key, "vicH"))                SetInventoryVicWinScaleH((float)val);
        else if (strstr(key, "invW"))                SetInventoryInvWinScaleW((float)val);
        else if (strstr(key, "invH"))                SetInventoryInvWinScaleH((float)val);
        else if (strstr(key, "weaponsInBackpack"))   SetInventoryWeaponsInBackpack(val != 0.0);
        else if (strstr(key, "infiniteWeight"))      SetInventoryInfiniteWeight(val != 0.0);
        else if (strstr(key, "infiniteVolume"))      SetInventoryInfiniteVolume(val != 0.0);
    }
    fclose(f);
}

void SaveInventorySettings()
{
    FILE *f = OpenInventoryUserFile("inventory_settings.cfg", "w");
    if (!f)
        return;
    fprintf(f, "// Inventory settings (written by the in-game edit menu).\n");
    fprintf(f, "// inventoryKey = SDL scancode (O=18, K=14, I=12, TAB=43, ...). showEditMenu = 0/1.\n");
    fprintf(f, "inventoryKey=%d\n", g_invOpenKey);
    fprintf(f, "showEditMenu=%d\n", g_invEditMenu ? 1 : 0);
    fprintf(f, "// --- edit-menu UI settings ---\n");
    fprintf(f, "iconScale=%.4f\n",      g_ui.iconScale);
    fprintf(f, "gridCols=%d\n",         g_ui.gridCols);
    fprintf(f, "volCapOverride=%.4f\n", g_ui.volCapOverride);
    fprintf(f, "accentTheme=%d\n",      g_ui.accentTheme ? 1 : 0);
    fprintf(f, "accentBorder=%d\n",     g_ui.accentBorder ? 1 : 0);
    fprintf(f, "accentR=%.4f\n",        g_ui.accentR);
    fprintf(f, "accentG=%.4f\n",        g_ui.accentG);
    fprintf(f, "accentB=%.4f\n",        g_ui.accentB);
    fprintf(f, "vicW=%.4f\n",           g_ui.vicW);
    fprintf(f, "vicH=%.4f\n",           g_ui.vicH);
    fprintf(f, "invW=%.4f\n",           g_ui.invW);
    fprintf(f, "invH=%.4f\n",           g_ui.invH);
    fprintf(f, "weaponsInBackpack=%d\n", g_ui.weaponsInBackpack ? 1 : 0);
    fprintf(f, "infiniteWeight=%d\n",    g_ui.infiniteWeight ? 1 : 0);
    fprintf(f, "infiniteVolume=%d\n",    g_ui.infiniteVolume ? 1 : 0);
    fclose(f);
}

// --- Per-icon tuning (debug tool + runtime overrides) ---------------------
namespace
{
struct TuneRec { RStringB name; IconTune t; };
static AutoArray<TuneRec> g_tunes;
}

static IconTune *FindIconTuneExact(const char *key)
{
    if (!key || !*key)
        return nullptr;
    for (int i = 0; i < g_tunes.Size(); i++)
        if (strcmp((const char *)g_tunes[i].name, key) == 0)
            return &g_tunes[i].t;
    return nullptr;
}

IconTune *FindIconTune(const char *key)
{
    if (IconTune *e = FindIconTuneExact(key))
        return e;
    // Legacy fallback: tunes saved before weapon/mag keys were split by kind were
    // keyed by the bare class name. Keep applying those until the item is re-edited
    // (which writes a kind-qualified key and stops using this fallback).
    const char *bare = (key && key[0] && key[1] == ':') ? key + 2 : nullptr;
    return bare ? FindIconTuneExact(bare) : nullptr;
}

IconTune &EditIconTune(const char *key)
{
    if (IconTune *e = FindIconTuneExact(key))   // exact only - never edit via the legacy fallback
        return *e;
    TuneRec r;
    r.name = RStringB(key);
    // Seed a freshly split "w:<class>"/"m:<class>" entry from any legacy bare-class
    // tune, so the previously-tuned value carries over as the starting point.
    if (key && key[0] && key[1] == ':')
        if (const IconTune *legacy = FindIconTuneExact(key + 2))
            r.t = *legacy;
    g_tunes.Add(r);
    return g_tunes[g_tunes.Size() - 1].t;
}

// Kind-qualified tuning key ("w:<class>" / "m:<class>") so a weapon and its
// same-named magazine (a launcher and its rocket) tune independently.
RString IconTuneKey(const InvCell &cell)
{
    const char *pre = (cell.kind == InvItemKind::Weapon) ? "w:" : "m:";
    return RString(pre) + RString((const char *)cell.ClassName());
}

// The laser designator is a weapon whose "magazine" is really just its battery.
// We don't want that battery listed as a separate inventory item or given an ammo
// counter, so it reads as a tool that simply emits a laser.
static bool IsLaserDesignatorClass(const char *cn)
{
    return cn && (strcmp(cn, "LaserDesignator") == 0 || strcmp(cn, "LaserDesignatorOH") == 0);
}

// These apply even with no invicons\_tuning.txt; the file, if present, overrides
// individual entries (so the debug tool can still tweak on top).
namespace
{
struct DefaultTune { const char *key; IconTune t; };
static const DefaultTune kDefaultTunes[] = {
    {"HandGrenade", {0.9843f,0.0400f,0.0100f,1,1,0.9843f,0.0400f,0.0100f,1,0.9843f,0.0400f,0.0100f}},
    {"M16", {1.0000f,0.0000f,0.0000f,1,1,1.0000f,0.0000f,0.0000f,1,1.0000f,0.0000f,0.0000f}},
    {"M21", {1.0000f,0.0000f,0.0000f,4,1,1.0000f,0.0000f,0.0000f,2,1.0000f,0.0000f,0.0000f}},
    {"LAWLauncher", {0.9936f,0.0000f,0.0000f,2,1,0.9936f,0.0000f,0.0000f,2,0.9936f,0.0000f,0.0000f}},
    {"CarlGustavLauncher", {1.0879f,0.0000f,0.0000f,4,1,1.0879f,0.0000f,0.0000f,2,1.0879f,0.0000f,0.0000f}},
    {"m:LAWLauncher", {0.9936f,0.0000f,0.0000f,2,1,0.9936f,0.0000f,0.0000f,1,0.9936f,0.0000f,0.0000f}},
    {"w:LAWLauncher", {1.0251f,0.0000f,0.0000f,2,1,1.0251f,0.0000f,0.0000f,2,1.0251f,0.0000f,0.0000f}},
    {"w:M16", {1.0000f,0.0000f,0.0000f,1,1,1.0000f,0.0000f,0.0000f,2,1.0093f,0.0000f,0.0000f}},
    {"m:Mortar", {1.0408f,0.0000f,0.0000f,2,1,1.0408f,0.0000f,0.0000f,1,1.0408f,0.0000f,0.0000f}},
    {"w:NVGoggles", {0.7893f,0.0100f,0.0000f,3,1,0.7893f,0.0100f,0.0000f,2,0.7893f,0.0100f,0.0000f}},
    {"m:M60", {1.0722f,0.0000f,0.0000f,2,1,1.0093f,0.0000f,0.0000f,2,1.0722f,0.0000f,0.0000f}},
    {"w:M60", {1.0722f,0.0000f,0.0000f,4,1,1.3708f,0.0000f,0.0000f,3,1.0722f,0.0000f,0.0000f}},
    {"w:CarlGustavLauncher", {1.0565f,0.0000f,0.0000f,4,1,1.0565f,0.0000f,0.0000f,2,1.0565f,0.0000f,0.0000f}},
    {"m:CarlGustavLauncher", {1.1164f,-0.0100f,0.0200f,3,1,1.1036f,0.0000f,-0.0300f,2,1.1164f,-0.0100f,0.0200f}},
    {"w:AALauncher", {1.0565f,0.0000f,0.0000f,4,1,1.0565f,0.0000f,0.0000f,2,1.0565f,0.0000f,0.0000f}},
    {"m:AALauncher", {1.1007f,0.0000f,0.0000f,4,1,1.0408f,0.0000f,0.0000f,2,1.1007f,0.0000f,0.0000f}},
    {"w:M21", {1.1792f,0.0000f,0.0000f,4,1,1.6065f,0.0000f,0.0000f,3,1.1792f,0.0000f,0.0000f}},
    {"m:M21", {0.9622f,0.0000f,0.0000f,1,1,0.9622f,0.0000f,0.0000f,1,0.9622f,0.0000f,0.0000f}},
    {"w:Binocular", {0.9121f,0.0000f,0.0000f,2,1,0.8679f,0.0000f,0.0000f,2,0.9121f,0.0000f,0.0000f}},
    {"m:HK", {1.0000f,0.0000f,0.0000f,1,1,1.0000f,0.0000f,0.0000f,1,1.0000f,0.0000f,0.0000f}},
    {"m:AK74", {0.9936f,0.0000f,0.0000f,1,1,0.9936f,0.0000f,0.0000f,1,0.9936f,0.0000f,0.0000f}},
    {"w:AK74GrenadeLauncher", {1.0251f,0.0000f,0.0000f,4,1,1.0251f,0.0000f,0.0000f,2,1.0251f,0.0000f,0.0000f}},
    {"w:AK74SU", {1.0565f,0.0000f,0.0000f,4,1,0.9622f,0.0000f,0.0000f,2,1.0565f,0.0000f,0.0000f}},
    {"m:PK", {1.0000f,0.0000f,0.0000f,2,1,1.0093f,0.0000f,0.0000f,2,1.0000f,0.0000f,0.0000f}},
    {"w:PK", {0.9936f,0.0000f,0.0000f,4,1,1.5436f,0.0000f,0.0000f,3,0.9936f,0.0000f,0.0000f}},
    {"m:AT4Launcher", {0.9779f,0.0000f,0.0000f,2,1,0.9779f,0.0000f,0.0000f,1,0.9779f,0.0000f,0.0000f}},
    {"m:RPGLauncher", {0.9779f,0.0000f,-0.0300f,2,1,1.0000f,0.0000f,0.0000f,1,0.9779f,0.0000f,-0.0300f}},
    {"w:9K32Launcher", {1.1036f,0.0000f,0.0500f,4,1,1.4965f,0.0000f,0.0500f,3,1.1036f,0.0000f,0.0500f}},
    {"w:AT4Launcher", {1.0000f,0.0000f,0.0000f,4,1,1.0000f,0.0000f,0.0000f,2,1.0000f,0.0000f,0.0000f}},
    {"w:RPGLauncher", {1.0408f,0.0000f,0.0300f,4,1,1.0408f,0.0000f,0.0300f,2,1.0408f,0.0000f,0.0300f}},
    {"m:9K32Launcher", {0.9936f,0.0000f,-0.0200f,6,1,1.0000f,0.0000f,-0.0200f,3,0.9936f,0.0000f,-0.0200f}},
    {"w:SVDDragunov", {1.1036f,0.0000f,0.0000f,4,1,1.5751f,0.0000f,0.0000f,3,1.1036f,0.0000f,0.0000f}},
    {"m:SVDDragunov", {0.9308f,0.0000f,0.0000f,1,1,0.9308f,0.0000f,0.0000f,1,0.9308f,0.0000f,0.0000f}},
    {"m:6G30Magazine", {1.6851f,0.0000f,0.0000f,3,1,1.7000f,0.0000f,0.0000f,2,1.6851f,0.0000f,0.0000f}},
    {"m:MM1Magazine", {1.5751f,0.0000f,-0.0200f,3,1,1.6222f,0.0000f,-0.0200f,2,1.5751f,0.0000f,-0.0200f}},
    {"m:RevolverMag", {1.0093f,0.0600f,0.0500f,1,1,1.0093f,0.0600f,0.0500f,1,1.0093f,0.0600f,0.0500f}},
    {"m:PipeBomb", {1.2451f,-0.0100f,0.0300f,2,1,1.1979f,0.0000f,0.0000f,2,1.2451f,-0.0100f,0.0300f}},
    {"w:Skorpion", {1.0000f,0.0000f,0.0000f,3,1,1.0000f,0.0000f,0.0000f,2,1.1508f,0.0000f,0.0000f}},
    {"w:Tokarev", {1.0565f,0.0000f,0.0000f,3,1,0.9622f,0.0000f,0.0000f,2,1.1508f,0.0000f,0.0000f}},
    {"w:Kozlice", {1.1351f,0.0000f,0.0000f,4,1,1.7000f,0.0000f,0.0000f,3,1.1351f,0.0000f,0.0000f}},
    {"w:Glock", {0.7265f,0.0000f,0.0000f,3,1,0.6793f,0.0000f,0.0000f,2,0.7265f,0.0000f,0.0000f}},
    {"w:Beretta", {1.1665f,0.0000f,0.0000f,2,1,0.8365f,0.0000f,0.0000f,2,1.1665f,0.0000f,0.0000f}},
    {"m:TokarevMag", {0.8993f,0.0000f,0.0000f,1,1,0.8993f,0.0000f,0.0000f,1,0.8993f,0.0000f,0.0000f}},
    {"w:CZ75", {1.0250f,0.0000f,0.0000f,2,1,1.0251f,0.0000f,0.0000f,2,1.2608f,0.0000f,0.0000f}},
    {"w:Ingram", {1.0000f,0.0000f,0.0000f,2,1,1.1351f,0.0000f,0.0000f,2,1.0000f,0.0000f,0.0000f}},
    {"w:Revolver", {1.0000f,0.0000f,0.0000f,2,1,1.0000f,0.0000f,0.0000f,2,1.2136f,0.0000f,0.0000f}},
    {"w:GlockS", {1.0000f,0.0000f,0.0000f,2,1,1.0000f,0.0000f,0.0000f,2,1.0000f,0.0000f,0.0000f}},
    {"m:Mine", {1.0565f,0.0000f,0.0000f,2,1,1.0565f,0.0000f,0.0000f,2,1.0565f,0.0000f,0.0000f}},
    {"m:MineE", {1.0000f,0.0000f,0.0000f,2,1,1.0000f,0.0000f,0.0000f,2,1.0000f,0.0000f,0.0000f}},
    {"w:M4", {0.9465f,0.0000f,0.0000f,4,1,0.9465f,0.0000f,0.0000f,2,1.0250f,0.0000f,0.0000f}},
    {"m:HandGrenade", {0.9151f,0.0300f,0.0100f,1,1,0.9151f,0.0300f,0.0100f,1,0.9151f,0.0300f,0.0100f}},
    {"m:GrenadeLauncher", {1.1036f,0.0000f,0.0000f,1,1,1.0000f,0.0000f,0.0000f,1,1.1036f,0.0000f,0.0000f}},
    {"w:6G30", {1.0565f,0.0000f,0.0000f,4,1,1.0000f,0.0000f,0.0000f,2,1.0565f,0.0000f,0.0000f}},
    {"w:LaserDesignator", {0.9779f,0.0000f,0.0000f,4,1,0.9465f,0.0000f,0.0000f,2,0.9779f,0.0000f,0.0000f}},
    {"w:HuntingRifle", {1.0000f,0.0000f,0.0000f,5,1,1.7000f,0.0000f,0.0000f,3,1.1665f,0.0000f,0.0000f}},
    {"w:AK47GrenadeLauncher", {1.0000f,0.0000f,0.0000f,4,1,1.0251f,0.0000f,0.0000f,2,1.0000f,0.0000f,0.0000f}},
    {"w:FAL", {1.0000f,0.0000f,0.0000f,4,1,1.0408f,0.0000f,0.0000f,2,1.0000f,0.0000f,0.0000f}},
    {"w:HKG3", {1.0000f,0.0000f,0.0000f,4,1,1.5908f,0.0000f,0.0000f,3,1.1193f,0.0000f,0.0000f}},
    {"w:Steyr", {1.0000f,0.0000f,0.0000f,4,1,1.0879f,0.0000f,0.0000f,2,1.0000f,0.0000f,0.0000f}},
    {"w:G36a", {1.0000f,0.0000f,0.0000f,4,1,1.1164f,0.0000f,0.0000f,2,1.1164f,0.0000f,0.0000f}},
    // First-aid kits: baked from the tuning pass so they default to their current
    // on-screen size even with no invicons\_tuning.txt present.
    {"m:US_FirstAidKit",  {0.8624f,0.0200f,0.0000f,1,1,0.8817f,0.0000f,0.0000f,1,1.0000f,0.0000f,0.0000f}},
    {"m:AI2_FirstAidKit", {0.8624f,0.0000f,0.0000f,1,1,0.9010f,0.0000f,0.0000f,1,1.0000f,0.0000f,0.0000f}},
};
}

void LoadIconTuning()
{
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    g_tunes.Clear();

    // 1) Baked-in defaults (the finalized tuning pass) - apply even with no file.
    for (const DefaultTune &d : kDefaultTunes)
    {
        TuneRec r;
        r.name = RStringB(d.key);
        r.t = d.t;
        g_tunes.Add(r);
    }

    // 2) Optional override file (the debug tool writes this). Upsert on top of the
    //    baked defaults so tweaks still work; delete the file to revert to defaults.
    FILE *f = OpenInventoryUserFile("inventory_icon_tuning.txt", "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char nm[128];
        float sz, ox, oy, vsz, vox, voy, lsz, lox, loy;
        int gw, gh, vw;
        // Field growth over time: 7 -> 10 (adds vicinity) -> 13 (adds equipped).
        int n = sscanf(line, "%127s %f %f %f %d %d %d %f %f %f %f %f %f",
                       nm, &sz, &ox, &oy, &gw, &gh, &vw, &vsz, &vox, &voy, &lsz, &lox, &loy);
        if (n < 7)
            continue;
        if (n < 10) { vsz = sz; vox = ox; voy = oy; }
        if (n < 13) { lsz = sz; lox = ox; loy = oy; }
        IconTune nt;
        nt.size = sz; nt.ox = ox; nt.oy = oy; nt.gw = gw; nt.gh = gh;
        nt.vsize = vsz; nt.vox = vox; nt.voy = voy; nt.vw = vw;
        nt.lsize = lsz; nt.lox = lox; nt.loy = loy;
        if (IconTune *ex = FindIconTuneExact(nm))
            *ex = nt;
        else
        {
            TuneRec r; r.name = RStringB(nm); r.t = nt;
            g_tunes.Add(r);
        }
    }
    fclose(f);
}

void SaveIconTuning()
{
    FILE *f = OpenInventoryUserFile("inventory_icon_tuning.txt", "w");
    if (!f)
        return;
    for (int i = 0; i < g_tunes.Size(); i++)
    {
        const IconTune &t = g_tunes[i].t;
        fprintf(f, "%s %.4f %.4f %.4f %d %d %d %.4f %.4f %.4f %.4f %.4f %.4f\n",
                (const char *)g_tunes[i].name, t.size, t.ox, t.oy, t.gw, t.gh, t.vw,
                t.vsize, t.vox, t.voy, t.lsize, t.lox, t.loy);
    }
    fclose(f);
}

// Grid footprint with any per-class override applied (gw>0 means "use override").
static GridSize TunedFootprint(const char *cn, GridSize def)
{
    if (const IconTune *t = FindIconTune(cn))
        if (t->gw > 0)
            return {t->gw, t->gh > 0 ? t->gh : def.h};
    return def;
}

// Vanilla take/drop feedback: shows the on-screen title (e.g. "Taking ammo")
// and plays the pickup sound from CfgCutScenes. Defined in Transport.cpp; the
// engine's own take/drop code forward-declares it the same way.
void CutScene(const char *name);

// ---------------------------------------------------------------------------
// InvCell
// ---------------------------------------------------------------------------

// Effective magazine type: the loaded instance's type, or (arsenal) the bare type.
static const MagazineType *CellMagType(const InvCell &c)
{
    if (c.magazine && c.magazine->_type) return c.magazine->_type;
    return c.magType;
}

// How many shells make one inventory "box". Soviet/Russian (Fetter) AND Czechoslovak
// (Sellier & Bellot - the Kozlice's home country) civilian/hunting 12-ga cartridges
// were both sold in flat boxes of 10.
static constexpr int kShellBoxSize = 10;

// Case-insensitive substring match (no <cctype> dependency).
static bool NameHasCI(const char *hay, const char *needleLower)
{
    if (!hay || !needleLower) return false;
    const size_t nl = strlen(needleLower);
    if (nl == 0) return false;
    for (const char *p = hay; *p; ++p)
    {
        size_t k = 0;
        while (k < nl && p[k])
        {
            char a = p[k], b = needleLower[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (a != b) break;
            k++;
        }
        if (k == nl) return true;
    }
    return false;
}

// Shotgun shells (Kozlice slug + buckshot) are the only loose-round ammo we box up.
// Each in-game "magazine" is a single shell (the buckshot one reports 7 - its pellet
// count - as _maxAmmo), so instead of one pellet-counting tile per shell we group them
// into boxes of kShellBoxSize.
static bool IsShotgunShellMag(const MagazineType *mt)
{
    // Legacy OFP magazines retain their original ammo semantics and separate tiles.
    return false;
}

// Real shells packed in one magazine. Buckshot stores `division` engine-rounds (pellets)
// per shell, so shells = capacity / division; a slug's division is 1 so shells = capacity.
// With the 2-shell break-action capacity, this is 2 for both Kozlice loads. Always >= 1.
static int ShellsPerMag(const MagazineType *mt)
{
    if (!mt) return 1;
    const int div = 1;
    const int s = mt->_maxAmmo / div;
    return s > 0 ? s : 1;
}

RString InvCell::DisplayName() const
{
    switch (kind)
    {
        case InvItemKind::Weapon:
            return weapon ? weapon->GetDisplayName() : RString();
        case InvItemKind::Magazine:
        {
            const MagazineType *mt = CellMagType(*this);
            if (mt)
            {
                // The AP mine (MineE) and AT mine (Mine) share the config display
                // name "Mine"; relabel them so they read distinctly in the inventory.
                if (strcmp((const char *)mt->GetName(), "MineE") == 0)
                    return RString("AP Mine");
                if (strcmp((const char *)mt->GetName(), "Mine") == 0)
                    return RString("AT Mine");
            }
            return mt ? RString(mt->GetDisplayName()) : RString();
        }
        default:
            return RString();
    }
}

RStringB InvCell::ClassName() const
{
    switch (kind)
    {
        case InvItemKind::Weapon:
            return weapon ? weapon->GetName() : RStringB();
        case InvItemKind::Magazine:
        {
            const MagazineType *mt = CellMagType(*this);
            return mt ? mt->GetName() : RStringB();
        }
        default:
            return RStringB();
    }
}

RStringB InvCell::PictureName() const
{
    switch (kind)
    {
        case InvItemKind::Weapon:
            return weapon ? weapon->GetPictureName() : RStringB();
        case InvItemKind::Magazine:
        {
            const MagazineType *mt = CellMagType(*this);
            return mt ? mt->GetPictureName() : RStringB();
        }
        default:
            return RStringB();
    }
}

bool InvCell::GetAmmo(int &current, int &capacity) const
{
    if (kind != InvItemKind::Magazine)
    {
        return false;
    }
    // A shell box badges/reads as "N of kShellBoxSize shells", NOT the pellet count.
    if (isBox)
    {
        current = stackCount;
        capacity = kShellBoxSize;
        return true;
    }
    const MagazineType *mt = CellMagType(*this);
    if (!mt)
    {
        return false;
    }
    // Divide by `division` so buckshot reads in SHELLS, not pellets (division=1 for
    // everything else, so this is a no-op for normal magazines).
    const int div = 1;
    const int raw = magazine ? (int)magazine->_ammo : (int)mt->_maxAmmo;   // arsenal mags are full
    current = raw / div;
    capacity = mt->_maxAmmo / div;
    return true;
}

float InvCell::Mass() const
{
    switch (kind)
    {
        case InvItemKind::Weapon:   return ItemMass::Of(weapon);
        case InvItemKind::Magazine:
        {
            if (isBox)
            {
                if (boxMags.Size() > 0)
                {
                    float total = 0.0f;
                    for (int i = 0; i < boxMags.Size(); i++)
                        total += ItemMass::Of(boxMags[i]);
                    return total;
                }
                // Arsenal template box: no instances yet - price the full box from type.
                return magType ? ItemMass::Of(magType) * (float)stackCount : 0.0f;
            }
            return magazine ? ItemMass::Of(magazine)
                            : (magType ? ItemMass::Of(magType) : 0.0f);
        }
        default:                    return 0.0f;
    }
}

const void *InvCell::SourceKey() const
{
    static const int kArsenalKey = 0;
    if (fromArsenal)
    {
        return &kArsenalKey;   // all arsenal cells share one page
    }
    return (const void *)(const VehicleSupply *)container;
}

RString InvCell::SourceLabel() const
{
    if (fromArsenal)
    {
        return RString("Arsenal (unlimited)");
    }
    VehicleSupply *c = container;
    if (!c || !c->GetType())
    {
        return RString("Ground");
    }
    RString name(c->GetType()->GetDisplayName());
    return fromBody ? name + RString(" (body)") : name;
}

// ---------------------------------------------------------------------------
// InventoryModel
// ---------------------------------------------------------------------------

InventoryModel::InventoryModel(Person *player)
    : _player(player)
{
    LoadInventorySettings();
    LoadIconTuning();
    Refresh();
}

static TargetSide PlayerSide(Person *p);   // defined below

float InventoryModel::MaxCarryWeight() const
{
    if (InventoryInfiniteWeight())
        return 1.0e6f;   // debug: effectively unlimited
    // Per-side carry budget (kg): civilians and resistance are less kitted out.
    switch (PlayerSide(_player))
    {
        case TCivilian: return 50.0f;
        case TGuerrila: return 55.0f;
        default:        return 60.0f;   // West / East (and unknown)
    }
}

// Per-side weight budget (kg) as a free function, mirroring MaxCarryWeight() above, so the
// action-menu take path can gate on weight exactly like the drag-drop inventory does.
static float InvWeightBudget(EntityAI *player)
{
    if (InventoryInfiniteWeight())
        return 1.0e6f;
    Person *p = dyn_cast<Person, EntityAI>(player);
    if (!p)
        return 60.0f;
    switch (PlayerSide(p))
    {
        case TCivilian: return 50.0f;
        case TGuerrila: return 55.0f;
        default:        return 60.0f;
    }
}

static float SideVolumeCapacity(TargetSide s);
static float InvVolumeBudget(EntityAI *player)
{
    if (InventoryInfiniteVolume()) return 1.0e6f;
    if (InventoryVolumeCapOverride() > 0.0f) return InventoryVolumeCapOverride();
    return SideVolumeCapacity(PlayerSide(dyn_cast<Person, EntityAI>(player)));
}

bool InventoryCanCarryWeapon(EntityAI *player, const WeaponType *w)
{
    LoadInventorySettings();
    if (!player || !w) return false;
    for (int i = 0; i < player->NWeaponSystems(); ++i)
    {
        const WeaponType *have = player->GetWeaponSystem(i);
        if (have == w) return false;
        if (have && !InventoryWeaponsInBackpack() &&
            (have->_weaponType & w->_weaponType & (MaskSlotPrimary | MaskSlotSecondary)))
            return false;
    }
    return ItemMass::TotalCarried(player) + ItemMass::Of(w) <= InvWeightBudget(player) &&
           ItemMass::TotalVolumeCarried(player) + ItemMass::VolumeOf(w) <= InvVolumeBudget(player);
}

bool InventoryCanCarryMagazine(EntityAI *player, const Magazine *m)
{
    LoadInventorySettings();
    if (!player || !m) return false;
    return ItemMass::TotalCarried(player) + ItemMass::Of(m) <= InvWeightBudget(player) &&
           ItemMass::TotalVolumeCarried(player) + ItemMass::VolumeOf(m) <= InvVolumeBudget(player);
}

RString InventoryModel::VicinitySourceLabel() const
{
    const int total = _containers.Size() + _bodies.Size();
    if (total == 0)
    {
        return RString("Nothing nearby");
    }
    RString name;
    if (_bodies.Size() > 0 && _bodies[0])
    {
        // A dead soldier - show its name, tagged as a body.
        name = (_bodies[0]->GetType())
                   ? RString(_bodies[0]->GetType()->GetDisplayName()) + RString(" (body)")
                   : RString("Body");
    }
    else
    {
        VehicleSupply *c0 = _containers[0];
        name = (c0 && c0->GetType()) ? RString(c0->GetType()->GetDisplayName())
                                     : RString("Container");
    }
    if (total > 1)
    {
        return name + RString("  (+more nearby)");
    }
    return name;
}

bool InventoryModel::HasWeaponWithMask(int mask) const
{
    if (!_player)
    {
        return false;
    }
    EntityAI *unit = _player;
    for (int i = 0; i < unit->NWeaponSystems(); i++)
    {
        const WeaponType *w = unit->GetWeaponSystem(i);
        if (w && (w->_weaponType & mask))
        {
            return true;
        }
    }
    return false;
}

// --- Per-side volume capacity (invisible internal budget) ------------------
static TargetSide PlayerSide(Person *p)
{
    if (!p)
        return TSideUnknown;
    AIUnit *u = p->Brain();
    AIGroup *g = u ? u->GetGroup() : nullptr;
    AICenter *c = g ? g->GetCenter() : nullptr;
    return c ? c->GetSide() : TSideUnknown;
}

static float SideVolumeCapacity(TargetSide s)
{
    switch (s)
    {
        case TWest:     return 75.0f;   // US: large ALICE + LBE
        case TEast:     return 36.0f;   // USSR: veshmeshok + belt kit
        case TGuerrila: return 45.0f;   // Resistance / CSLA: vz.85 + webbing
        case TCivilian: return 28.0f;   // civilian everyday batoh
        default:        return 40.0f;
    }
}

static float CellVolume(const InvCell &c)
{
    if (c.kind == InvItemKind::Weapon)
        return ItemMass::VolumeOf(c.weapon);
    if (c.isBox)
    {
        if (c.boxMags.Size() > 0)
        {
            float v = 0.0f;
            for (int i = 0; i < c.boxMags.Size(); i++)
                if (c.boxMags[i] && c.boxMags[i]->_type)
                    v += ItemMass::VolumeOf(c.boxMags[i]->_type);
            return v;
        }
        // Arsenal template box: price the full box from the type.
        return c.magType ? ItemMass::VolumeOf(c.magType) * (float)c.stackCount : 0.0f;
    }
    const MagazineType *mt = c.magType ? c.magType : (c.magazine ? c.magazine->_type : nullptr);
    return mt ? ItemMass::VolumeOf(mt) : 0.0f;
}

int InventoryModel::PlayerSideIndex() const
{
    switch (PlayerSide(_player))
    {
        case TWest:     return 0;
        case TEast:     return 1;
        case TGuerrila: return 2;
        case TCivilian: return 3;
        default:        return -1;
    }
}

void InventoryModel::Refresh()
{
    _grid.Clear();
    _vicinity.Clear();
    _loadout.Clear();
    _equippedIdx.Clear();
    _containers.Clear();
    _bodies.Clear();
    _commandUnits.Clear();
    _carriedMass = 0.0f;

    if (!_player)
    {
        return;
    }

    ScanLoadout();
    ScanGrid();
    ScanVicinity();

    _carriedMass = ItemMass::TotalCarried(_player);
    _carriedVolume = ItemMass::TotalVolumeCarried(_player);
    // Edit-menu override (litres) wins over the per-side default when non-zero;
    // "infinite volume" wins over everything.
    _volumeCapacity = InventoryInfiniteVolume() ? 1.0e6f
                    : (InventoryVolumeCapOverride() > 0.0f) ? InventoryVolumeCapOverride()
                    : SideVolumeCapacity(PlayerSide(_player));
}

void InventoryModel::ScanGrid()
{
    EntityAI *unit = _player;

    // v3: rifles and launchers ONLY live in the two top loadout slots - never in
    // the storage grid. The grid holds spare PISTOLS (a player can carry more than
    // one handgun and swap which is equipped), binoculars / NVGs (which have no
    // loadout slot), plus magazines.
    for (int i = 0; i < unit->NWeaponSystems(); i++)
    {
        const WeaponType *w = unit->GetWeaponSystem(i);
        if (!w)
        {
            continue;
        }
        // ALWAYS display spare (non-equipped) long guns in the grid. The loadout strip has
        // only ONE primary slot, so a second primary would otherwise be carried invisibly
        // and only reappear when you drop the equipped one - which looks exactly like lost
        // gear. Showing it (as a 3-wide tile) keeps every carried weapon visible. The
        // "Rifles in backpack" option still governs whether you may PICK UP an extra long
        // gun (see CanTransfer's SlotOccupied check), it just no longer hides carried ones.
        int gridMasks = MaskSlotHandGun | MaskSlotBinocular | MaskSlotPrimary | MaskSlotSecondary;
        if ((w->_weaponType & gridMasks) == 0)
        {
            continue;
        }
        // Skip the one already shown in the loadout pistol slot.
        bool equipped = false;
        for (int e = 0; e < _equippedIdx.Size(); e++)
        {
            if (_equippedIdx[e] == i) { equipped = true; break; }
        }
        if (equipped)
        {
            continue;
        }

        InvCell cell;
        cell.pane = InvPane::Grid;
        cell.kind = InvItemKind::Weapon;
        cell.index = i;
        cell.weapon = w;
        const int wt = w->_weaponType;
        const bool laser = IsLaserDesignatorClass((const char *)w->GetName());
        if (wt & MaskSlotBinocular)
        {
            // NVGs share the binocular slot mask but get a wider 2-tile footprint;
            // plain binoculars stay a single tile.
            const char *wn = (const char *)w->GetName();
            if (wn && strcmpi(wn, "NVGoggles") == 0)
            {
                cell.footprint.w = 2;   // night-vision goggles: 2 tiles
                cell.footprint.h = 1;
            }
            else
            {
                cell.footprint.w = 1;   // binoculars: single tile
                cell.footprint.h = 1;
            }
        }
        else if ((wt & (MaskSlotPrimary | MaskSlotSecondary)) && !laser)
        {
            cell.footprint.w = 3;   // rifles/launchers (not the laser): 3 tiles long
            cell.footprint.h = 1;
        }
        else
        {
            cell.footprint = TunedFootprint((const char *)IconTuneKey(cell), ItemFootprint::Of(w));
        }
        FindPlacement(cell.footprint, cell.gx, cell.gy);
        _grid.Add(cell);
    }

    // The grid is otherwise magazines-only storage: every carried magazine that
    // isn't currently loaded in a weapon (spare rifle/pistol mags plus grenades).
    // Shotgun shells are collected here and grouped into boxes after the loop.
    struct ShellEnt { int idx; const Magazine *mag; const MagazineType *type; };
    AutoArray<ShellEnt> shells;
    for (int i = 0; i < unit->NMagazines(); i++)
    {
        const Magazine *m = unit->GetMagazine(i);
        if (!m)
        {
            continue;
        }
        if (m->_type && IsLaserDesignatorClass((const char *)m->_type->GetName()))
        {
            continue;   // designator battery isn't shown as a carried item
        }

        // Skip a magazine that's currently loaded in a weapon - it's counted as
        // that weapon's ammo (shown on hover), not as a separate inventory item.
        // EXCEPTION: throwables (grenades/mines/satchels) stay listed even while
        // "loaded" in the Throw/Put muzzle - selecting one loads it into that muzzle,
        // and we don't want the item to vanish from the inventory when that happens.
        bool loaded = false;
        for (int k = 0; k < unit->NMagazineSlots(); k++)
        {
            if (unit->GetMagazineSlot(k)._magazine == m)
            {
                loaded = true;
                break;
            }
        }
        if (loaded && !IsThrowable(m->_type))
        {
            continue;
        }

        // Shotgun shells don't get an individual tile - collect and box them below.
        if (IsShotgunShellMag(m->_type))
        {
            ShellEnt e{i, m, m->_type};
            shells.Add(e);
            continue;
        }

        InvCell cell;
        cell.pane = InvPane::Grid;
        cell.kind = InvItemKind::Magazine;
        cell.index = i;
        cell.magazine = m;
        if (m->_type && strcmp((const char *)m->_type->GetName(), "JerryCan") == 0)
        {
            cell.footprint.w = 2;   // jerry can: 2 wide x 3 tall
            cell.footprint.h = 3;
        }
        else
        {
            cell.footprint = TunedFootprint((const char *)IconTuneKey(cell),
                                            m->_type ? ItemFootprint::Of(m->_type) : GridSize{1, 1});
        }
        FindPlacement(cell.footprint, cell.gx, cell.gy);
        _grid.Add(cell);
    }

    // Group the collected shotgun shells into boxes of up to kShellBoxSize, one tile
    // per box, per shell TYPE (slug and buckshot box separately). Each box still holds
    // the individual engine magazines, so the weapon reloads shell-by-shell as normal.
    {
        AutoArray<const MagazineType *> types;
        for (int i = 0; i < shells.Size(); i++)
        {
            bool seen = false;
            for (int t = 0; t < types.Size(); t++)
                if (types[t] == shells[i].type) { seen = true; break; }
            if (!seen) types.Add(shells[i].type);
        }
        for (int t = 0; t < types.Size(); t++)
        {
            const MagazineType *ty = types[t];
            AutoArray<int> ord;   // indices into `shells` for this type, in carry order
            for (int i = 0; i < shells.Size(); i++)
                if (shells[i].type == ty) ord.Add(i);

            // A box holds kShellBoxSize SHELLS. Each magazine is ShellsPerMag shells, so
            // step through the magazines that many at a time.
            const int spm = ShellsPerMag(ty);
            const int step = spm > 0 ? (kShellBoxSize / spm) : kShellBoxSize;
            const int magsPerBox = step > 0 ? step : 1;
            for (int off = 0; off < ord.Size(); off += magsPerBox)
            {
                const int rem = ord.Size() - off;
                const int nMags = rem < magsPerBox ? rem : magsPerBox;

                const int div = 1;
                InvCell cell;
                cell.pane = InvPane::Grid;
                cell.kind = InvItemKind::Magazine;
                cell.isBox = true;
                const ShellEnt &head = shells[ord[off]];
                cell.index = head.idx;       // representative shell's magazine index
                cell.magazine = head.mag;    // representative (icon / class name)
                int shellsInBox = 0;
                for (int j = 0; j < nMags; j++)
                {
                    const Magazine *mg = shells[ord[off + j]].mag;
                    cell.boxMags.Add(mg);
                    // Count ACTUAL shells: a partially-fired mag holds fewer than a full
                    // one (this was the "duplication" - counting every mag as full).
                    shellsInBox += mg ? ((int)mg->_ammo / div) : 0;
                }
                cell.stackCount = shellsInBox;   // real shells shown on the box
                // Small 1x1 tile like a single shell; the badge shows the shell count.
                cell.footprint.w = 1;
                cell.footprint.h = 1;
                FindPlacement(cell.footprint, cell.gx, cell.gy);
                _grid.Add(cell);
            }
        }
    }

    // NOTE: this re-derives grid position from scratch on every Refresh(),
    // which means a manual MoveWithinGrid() arrangement gets clobbered the
    // next time a pickup/drop triggers a rescan. If you want arrangements to
    // stick, key a small gx/gy cache off (kind, index) - or off the
    // Magazine's _id (EntityAIType.hpp:227), which is stable per-instance -
    // and consult it here before calling FindPlacement.
}

bool InventoryModel::IsThrowableItem(const InvCell &cell) const
{
    if (cell.kind != InvItemKind::Magazine)
        return false;
    const MagazineType *mt = cell.magType ? cell.magType
                                          : (cell.magazine ? cell.magazine->_type : nullptr);
    return IsThrowable(mt);
}

bool InventoryModel::IsThrowable(const MagazineType *mt) const
{
    if (!_player || !mt)
    {
        return false;
    }
    EntityAI *unit = _player;
    for (int i = 0; i < unit->NWeaponSystems(); i++)
    {
        const WeaponType *w = unit->GetWeaponSystem(i);
        if (!w)
        {
            continue;
        }
        // Only the "Throw"/"Put" muzzle-only pseudo-weapons (no real slot).
        if ((w->_weaponType & (MaskSlotPrimary | MaskSlotSecondary | MaskSlotHandGun)) != 0)
        {
            continue;
        }
        for (int j = 0; j < w->_muzzles.Size(); j++)
        {
            if (w->_muzzles[j] && w->_muzzles[j]->CanUse(mt))
            {
                return true;
            }
        }
    }
    return false;
}

int InventoryModel::SpareMagCount(const WeaponType *w) const
{
    if (!_player || !w)
    {
        return 0;
    }
    EntityAI *unit = _player;
    int count = 0;
    for (int i = 0; i < unit->NMagazines(); i++)
    {
        const Magazine *m = unit->GetMagazine(i);
        if (!m || !m->_type)
        {
            continue;
        }
        // Not the magazine currently loaded in a weapon.
        bool loaded = false;
        for (int k = 0; k < unit->NMagazineSlots(); k++)
        {
            if (unit->GetMagazineSlot(k)._magazine == m)
            {
                loaded = true;
                break;
            }
        }
        if (loaded)
        {
            continue;
        }
        for (int j = 0; j < w->_muzzles.Size(); j++)
        {
            if (w->_muzzles[j] && w->_muzzles[j]->CanUse(m->_type))
            {
                count++;
                break;
            }
        }
    }
    return count;
}

void InventoryModel::ScanLoadout()
{
    EntityAI *unit = _player;
    if (!unit)
    {
        return;
    }

    struct SlotDef { LoadoutSlot::Kind kind; int mask; };
    const SlotDef defs[3] = {
        { LoadoutSlot::Primary,   MaskSlotPrimary },
        { LoadoutSlot::Secondary, MaskSlotSecondary },
        { LoadoutSlot::Handgun,   MaskSlotHandGun },
    };

    for (int s = 0; s < 3; s++)
    {
        LoadoutSlot slot;
        slot.kind = defs[s].kind;

        // The player's chosen weapon for this slot (if still carried), else the first -
        // so the loadout strip shows whatever they equipped, not always the first gun.
        int foundIdx = -1;
        const WeaponType *found = EquippedForSlot(unit, defs[s].mask, foundIdx);

        if (found)
        {
            slot.hasItem = true;
            slot.cell.pane = InvPane::Grid;   // carried item -> DropToGround works
            slot.cell.kind = InvItemKind::Weapon;
            slot.cell.index = foundIdx;
            slot.cell.weapon = found;
            slot.cell.footprint = ItemFootprint::Of(found);
            int cur = 0, cap = 0;
            // No ammo readout for the laser designator - it just emits a laser.
            if (!IsLaserDesignatorClass((const char *)found->GetName()) &&
                WeaponLoadedAmmo(found, cur, cap))
            {
                slot.count = cur;
            }
            slot.spare = SpareMagCount(found);
            _equippedIdx.Add(foundIdx);
        }
        _loadout.Add(slot);
    }
}

bool InventoryModel::WeaponLoadedAmmo(const WeaponType *w, int &cur, int &cap) const
{
    if (!_player || !w)
    {
        return false;
    }
    for (int j = 0; j < w->_muzzles.Size(); j++)
    {
        const MuzzleType *mz = w->_muzzles[j];
        for (int i = 0; i < _player->NMagazineSlots(); i++)
        {
            const MagazineSlot &slot = _player->GetMagazineSlot(i);
            if (slot._muzzle == mz && slot._magazine)
            {
                const MagazineType *mt = slot._magazine->_type;
                const int div = 1;
                cur = (int)slot._magazine->_ammo / div;   // shells, not pellets
                cap = mt ? mt->_maxAmmo / div : cur;
                return true;
            }
        }
    }
    return false;
}

// A ground-drop holder (spawned by DropToTarget) is not a "container" the player
// walked up to - it's loose gear lying on the ground, so it belongs on the single
// shared "Ground" page rather than getting a page of its own.
static bool IsGroundHolderType(const VehicleSupply *c)
{
    if (!c || !c->GetType())
    {
        return false;
    }
    const char *n = (const char *)c->GetType()->GetName();
    return strcmp(n, "WeaponHolder") == 0 || strcmp(n, "SecondaryWeaponHolder") == 0;
}

// True if `m` is a living soldier the player commands: same group, player is the
// group leader, and it isn't the player. Used to expose squad-mates' inventories.
static bool IsCommandedByPlayer(const Person *player, const Man *m)
{
    if (!player || !m || (const Person *)m == player)
        return false;
    AIUnit *pu = player->Brain();
    AIUnit *mu = m->Brain();
    if (!pu || !mu || !pu->IsGroupLeader())
        return false;
    AIGroup *pg = pu->GetGroup();
    return pg && mu->GetGroup() == pg;
}

void InventoryModel::CollectContainers()
{
    if (!_player)
    {
        return;
    }

    const Vector3 pos = _player->Position();
    const float radius = VicinityRadius();

    // VERIFY: see the identical VERIFY note in the previous pass regarding
    // exact Landscape accessor names (GLOB_LAND, LandGrid). Left unchanged
    // here since it's unrelated to the grid/weight redesign.
    Landscape *land = GLOB_LAND;
    if (!land)
    {
        return;
    }

    const float cell = LandGrid;
    // Widen the CELL search window generously so large objects (APCs, the field
    // hospital) whose model ORIGIN sits a cell or two away are still visited. The
    // precise per-object reach test below (player radius + the object's own size)
    // decides what actually qualifies, so small objects still require true nearness.
    const float scan = radius + 40.0f;
    const int xMin = toIntFloor((pos.X() - scan) / cell);
    const int xMax = toIntFloor((pos.X() + scan) / cell);
    const int zMin = toIntFloor((pos.Z() - scan) / cell);
    const int zMax = toIntFloor((pos.Z() + scan) / cell);

    for (int z = zMin; z <= zMax; z++)
    {
        for (int x = xMin; x <= xMax; x++)
        {
            const ObjectList &list = land->GetObjects(z, x);
            for (int i = 0; i < list.Size(); i++)
            {
                Object *obj = list[i];
                if (!obj || obj == _player)
                {
                    continue;
                }
                // Reach = base radius + the object's own bounding size, so you can
                // access a vehicle from anywhere along its body (not just near its
                // model origin) and a big installation like the field hospital from
                // anywhere in its footprint - fixing the "only near one spot" issue.
                const float reach = radius + obj->GetRadius();
                if (obj->Position().Distance2(pos) > reach * reach)
                {
                    continue;
                }

                // Dead soldier? Its gear (worn weapons/magazines) is lootable.
                Man *man = dyn_cast<Man>(obj);
                if (man && man->IsDead())
                {
                    _bodies.Add(man);
                    continue;
                }
                // Living squad-mate you command? Its inventory is accessible for
                // give/take (you are the group leader and it is in your group).
                if (man && !man->IsDead() && IsCommandedByPlayer(_player, man))
                {
                    // Tight 2 m transfer range for squad-mates (you hand gear directly).
                    if (obj->Position().Distance2(pos) <= 2.0f * 2.0f)
                        _commandUnits.Add(man);
                    continue;
                }

                VehicleSupply *supply = dyn_cast<VehicleSupply>(obj);
                if (!supply)
                {
                    continue;
                }

                const EntityAIType *type = supply->GetType();
                if (!type)
                {
                    continue;
                }
                // Show the container if it CAN hold cargo, or if it currently DOES:
                // medic vehicles and the field hospital have no configured cargo
                // capacity but carry the first-aid kits stocked into them at spawn.
                const bool hasCargoCap = (type->GetMaxMagazinesCargo() > 0 || type->_maxWeaponsCargo > 0);
                const bool hasCargoNow = (supply->GetMagazineCargoSize() > 0 || supply->GetWeaponCargoSize() > 0);
                if (!hasCargoCap && !hasCargoNow)
                {
                    continue;
                }

                // Ambulances use a tighter access range: the vehicle body plus a small
                // (half of the default) 2 m margin. Measured from the body size, not a
                // fraction of the whole reach, so you can still reach the ENDS of a long
                // hull (halving the total reach put the vehicle's own rear out of range).
                {
                    const char *cn = (const char *)type->GetName();
                    if (strcmp(cn, "M113Ambul") == 0 || strcmp(cn, "BMPAmbul") == 0)
                    {
                        const float ambReach = obj->GetRadius() + radius * 0.5f;
                        if (obj->Position().Distance2(pos) > ambReach * ambReach)
                        {
                            continue;
                        }
                    }
                    else if (strcmp(cn, "MASH") == 0 || strcmp(cn, "hospital") == 0)
                    {
                        // Half the (generous, body-size-based) field-hospital reach.
                        const float mashReach = (radius + obj->GetRadius()) * 0.5f;
                        if (obj->Position().Distance2(pos) > mashReach * mashReach)
                        {
                            continue;
                        }
                    }
                }

                _containers.Add(supply);
            }
        }
    }
}

void InventoryModel::ScanVicinity()
{
    CollectContainers();

    // Adds a source's magazines to the vicinity list, grouping shotgun shells into
    // boxes of kShellBoxSize (per type) exactly like the carried grid - so every panel
    // (crates, bodies, squad-mates) shows shells as boxes, never one pellet-counting
    // tile per shell. Non-shell magazines are added individually as before.
    auto addVicinityMags = [&](VehicleSupply *cont, bool fromBody, int count, auto getMag)
    {
        struct SE { int idx; const Magazine *mag; const MagazineType *type; };
        AutoArray<SE> shells;
        for (int i = 0; i < count; i++)
        {
            const Magazine *m = getMag(i);
            if (!m) continue;
            if (IsShotgunShellMag(m->_type)) { shells.Add(SE{i, m, m->_type}); continue; }
            InvCell cell;
            cell.pane = InvPane::Ground;
            cell.kind = InvItemKind::Magazine;
            cell.index = i;
            cell.container = cont;
            cell.fromBody = fromBody;
            cell.magazine = m;
            _vicinity.Add(cell);
        }
        AutoArray<const MagazineType *> types;
        for (int i = 0; i < shells.Size(); i++)
        {
            bool seen = false;
            for (int t = 0; t < types.Size(); t++) if (types[t] == shells[i].type) { seen = true; break; }
            if (!seen) types.Add(shells[i].type);
        }
        for (int t = 0; t < types.Size(); t++)
        {
            const MagazineType *ty = types[t];
            AutoArray<int> ord;
            for (int i = 0; i < shells.Size(); i++) if (shells[i].type == ty) ord.Add(i);
            const int spm = ShellsPerMag(ty);
            const int magsPerBox = (spm > 0 && kShellBoxSize / spm > 0) ? kShellBoxSize / spm : 1;
            for (int off = 0; off < ord.Size(); off += magsPerBox)
            {
                const int rem = ord.Size() - off;
                const int nMags = rem < magsPerBox ? rem : magsPerBox;
                const int div = 1;
                InvCell cell;
                cell.pane = InvPane::Ground;
                cell.kind = InvItemKind::Magazine;
                cell.container = cont;
                cell.fromBody = fromBody;
                cell.isBox = true;
                const SE &head = shells[ord[off]];
                cell.index = head.idx;
                cell.magazine = head.mag;
                int shellsInBox = 0;
                for (int j = 0; j < nMags; j++)
                {
                    const Magazine *mg = shells[ord[off + j]].mag;
                    cell.boxMags.Add(mg);
                    shellsInBox += mg ? ((int)mg->_ammo / div) : 0;   // ACTUAL shells (partials count less)
                }
                cell.stackCount = shellsInBox;   // real shells shown on the box
                _vicinity.Add(cell);
            }
        }
    };

    // Dead soldiers: their worn weapons and magazines are lootable. Person is a
    // VehicleSupply, but its gear lives in weapon systems / magazines, not cargo.
    for (int b = 0; b < _bodies.Size(); b++)
    {
        Person *body = _bodies[b];
        if (!body)
        {
            continue;
        }
        for (int i = 0; i < body->NWeaponSystems(); i++)
        {
            const WeaponType *w = body->GetWeaponSystem(i);
            if (!w)
            {
                continue;
            }
            // Skip the Throw/Put muzzle pseudo-weapons.
            if ((w->_weaponType & (MaskSlotPrimary | MaskSlotSecondary | MaskSlotHandGun)) == 0)
            {
                continue;
            }
            InvCell cell;
            cell.pane = InvPane::Ground;
            cell.kind = InvItemKind::Weapon;
            cell.index = i;
            cell.container = body;   // Person is a VehicleSupply
            cell.fromBody = true;
            cell.weapon = w;
            _vicinity.Add(cell);
        }
        addVicinityMags(body, true, body->NMagazines(),
                        [&](int i) { return body->GetMagazine(i); });
    }

    // Living squad-mates you command: same person-gear handling as a body (fromBody),
    // but their page also accepts drops (give), and taking works too.
    for (int u = 0; u < _commandUnits.Size(); u++)
    {
        Person *mate = _commandUnits[u];
        if (!mate)
        {
            continue;
        }
        for (int i = 0; i < mate->NWeaponSystems(); i++)
        {
            const WeaponType *w = mate->GetWeaponSystem(i);
            if (!w ||
                (w->_weaponType & (MaskSlotPrimary | MaskSlotSecondary | MaskSlotHandGun)) == 0)
            {
                continue;
            }
            InvCell cell;
            cell.pane = InvPane::Ground;
            cell.kind = InvItemKind::Weapon;
            cell.index = i;
            cell.container = mate;
            cell.fromBody = true;
            cell.weapon = w;
            _vicinity.Add(cell);
        }
        addVicinityMags(mate, true, mate->NMagazines(),
                        [&](int i) { return mate->GetMagazine(i); });
    }

    for (int c = 0; c < _containers.Size(); c++)
    {
        VehicleSupply *container = _containers[c];
        if (!container)
        {
            continue;
        }
        // The arsenal crate is an infinite source, not storage: show only the clean
        // arsenal (added below), never the ReammoBox default cargo it inherits.
        if (container->GetType() &&
            strcmp((const char *)container->GetType()->GetName(), "ArsenalCrate") == 0)
        {
            continue;
        }

        for (int i = 0; i < container->GetWeaponCargoSize(); i++)
        {
            const WeaponType *w = container->GetWeaponCargo(i);
            if (!w)
            {
                continue;
            }

            InvCell cell;
            cell.pane = InvPane::Ground;
            cell.kind = InvItemKind::Weapon;
            cell.index = i;
            cell.container = container;
            cell.weapon = w;
            _vicinity.Add(cell);
        }

        addVicinityMags(container, false, container->GetMagazineCargoSize(),
                        [&](int i) { return container->GetMagazineCargo(i); });
    }

    // The infinite arsenal only appears for the dedicated "Ammo Crates (Arsenal)"
    // object (CfgVehicles class ArsenalCrate) - not for ordinary ammo crates.
    for (int c = 0; c < _containers.Size(); c++)
    {
        VehicleSupply *v = _containers[c];
        if (v && v->GetType() &&
            strcmp((const char *)v->GetType()->GetName(), "ArsenalCrate") == 0)
        {
            AddArsenal();
            break;
        }
    }

    // -----------------------------------------------------------------------
    // Vicinity source list: one page per nearby container/body (even when EMPTY,
    // so a bare car still shows its name so the player knows where items go), the
    // arsenal if present, and finally an always-available "Ground" drop page.
    // DisplayInventory paginates the vicinity strip by this list.
    _vicSources.Clear();

    bool haveArsenal = false;
    for (int c = 0; c < _containers.Size(); c++)
    {
        VehicleSupply *v = _containers[c];
        if (!v || !v->GetType())
        {
            continue;
        }
        if (strcmp((const char *)v->GetType()->GetName(), "ArsenalCrate") == 0)
        {
            haveArsenal = true;
            continue;   // represented by the single arsenal page below
        }
        if (IsGroundHolderType(v))
        {
            continue;   // loose gear -> shown on the shared Ground page, not its own
        }
        VicSource s;
        s.label = RString(v->GetType()->GetDisplayName());
        s.container = v;
        _vicSources.Add(s);
    }

    for (int b = 0; b < _bodies.Size(); b++)
    {
        Person *body = _bodies[b];
        if (!body)
        {
            continue;
        }
        VicSource s;
        s.label = (body->GetType())
                      ? RString(body->GetType()->GetDisplayName()) + RString(" (body)")
                      : RString("Body");
        s.container = (VehicleSupply *)body;
        s.fromBody = true;
        _vicSources.Add(s);
    }

    // Living squad-mates you command: one page each (give + take).
    for (int u = 0; u < _commandUnits.Size(); u++)
    {
        Person *mate = _commandUnits[u];
        if (!mate)
        {
            continue;
        }
        VicSource s;
        s.label = (mate->GetType())
                      ? RString(mate->GetType()->GetDisplayName())
                      : RString("Squad-mate");
        s.container = (VehicleSupply *)mate;
        s.fromBody = true;
        _vicSources.Add(s);
    }

    if (haveArsenal)
    {
        VicSource s;
        s.label = RString("Arsenal (unlimited)");
        s.isArsenal = true;
        _vicSources.Add(s);
    }

    // Ground is always the final page, so items can be dropped even with no
    // container in range.
    {
        VicSource g;
        g.label = RString("Ground");
        g.isGround = true;
        _vicSources.Add(g);
    }
}

bool InventoryModel::CellOnSource(const InvCell &cell, int srcIdx) const
{
    if (srcIdx < 0 || srcIdx >= _vicSources.Size())
    {
        return false;
    }
    const VicSource &s = _vicSources[srcIdx];
    if (s.isGround)
    {
        // The Ground page gathers all loose gear lying on the ground (any
        // WeaponHolder cargo), so a dropped item shows here rather than on its own.
        if (cell.fromArsenal || cell.fromBody)
        {
            return false;
        }
        return IsGroundHolderType((const VehicleSupply *)cell.container);
    }
    if (s.isArsenal)
    {
        return cell.fromArsenal;
    }
    if (cell.fromArsenal)
    {
        return false;
    }
    return (const VehicleSupply *)cell.container == (const VehicleSupply *)s.container;
}

void InventoryModel::AddArsenal()
{
    // Enumerate the type banks only ONCE per open (it isn't free); reuse after.
    if (!_arsenalBuilt)
    {
        _arsenalBuilt = true;

        // Every magazine type already placed anywhere in the arsenal, so pass 2
        // (loose throwables) doesn't re-add a mag that a weapon already showed.
        AutoArray<const MagazineType *> shownMags;

        // 1) Every carryable weapon, each immediately followed by its compatible
        //    magazine(s) - so a weapon and the mag you need for it sit side by side.
        for (int i = 0; i < WeaponTypes.Size(); i++)
        {
            const WeaponType *w = WeaponTypes.Get(i);
            if (!w || w->_scope < 2)
            {
                continue;
            }
            if ((w->_weaponType & (MaskSlotPrimary | MaskSlotSecondary |
                                   MaskSlotHandGun | MaskSlotBinocular)) == 0)
            {
                continue;   // not a carryable weapon
            }
            InvCell wc;
            wc.pane = InvPane::Ground;
            wc.kind = InvItemKind::Weapon;
            wc.index = i;
            wc.weapon = w;
            wc.fromArsenal = true;
            _arsenalCache.Add(wc);

            // this weapon's magazines, right after it (dedup within the weapon)
            AutoArray<const MagazineType *> localMags;
            for (int j = 0; j < w->_muzzles.Size(); j++)
            {
                const MuzzleType *mz = w->_muzzles[j];
                if (!mz)
                {
                    continue;
                }
                for (int k = 0; k < mz->_magazines.Size(); k++)
                {
                    const MagazineType *m = mz->_magazines[k];
                    if (!m || m->_maxAmmo <= 0)
                    {
                        continue;   // skip proxy/uninitialised mags (show as "0")
                    }
                    if (IsLaserDesignatorClass((const char *)m->GetName()))
                    {
                        continue;   // designator battery isn't a real ammo item
                    }
                    bool seen = false;
                    for (int s = 0; s < localMags.Size(); s++)
                    {
                        if (localMags[s] == m) { seen = true; break; }
                    }
                    if (seen)
                    {
                        continue;
                    }
                    localMags.Add(m);

                    bool inGlobal = false;
                    for (int s = 0; s < shownMags.Size(); s++)
                    {
                        if (shownMags[s] == m) { inGlobal = true; break; }
                    }
                    if (!inGlobal)
                    {
                        shownMags.Add(m);
                    }

                    InvCell mc;
                    mc.pane = InvPane::Ground;
                    mc.kind = InvItemKind::Magazine;
                    mc.index = 0;
                    mc.magType = m;
                    mc.fromArsenal = true;
                    // Shotgun shells show as a full box of 10 in the arsenal too; taking
                    // one yields a whole box (10 fresh shells) - see Transfer().
                    if (IsShotgunShellMag(m))
                    {
                        mc.isBox = true;
                        mc.stackCount = kShellBoxSize;
                    }
                    _arsenalCache.Add(mc);
                }
            }
        }

        // 2) Infantry throwables/placeables not tied to a carryable weapon (grenades,
        //    satchels, mines) - identified by their item slot bits. Vehicle ammo has
        //    none of these bits, so it is excluded from the arsenal.
        for (int i = 0; i < MagazineTypes.Size(); i++)
        {
            const MagazineType *m = MagazineTypes.Get(i);
            if (!m || m->_scope < 2 || m->_maxAmmo <= 0)
            {
                continue;
            }
            if ((m->_magazineType & (MaskSlotItem | MaskSlotHandGunItem)) == 0)
            {
                continue;   // not a throwable/placeable item (e.g. vehicle ammo)
            }
            if (IsLaserDesignatorClass((const char *)m->GetName()))
            {
                continue;   // designator battery isn't a real ammo item
            }
            bool seen = false;
            for (int s = 0; s < shownMags.Size(); s++)
            {
                if (shownMags[s] == m) { seen = true; break; }
            }
            if (seen)
            {
                continue;   // already shown next to a weapon - don't duplicate it here
            }
            shownMags.Add(m);
            InvCell mc;
            mc.pane = InvPane::Ground;
            mc.kind = InvItemKind::Magazine;
            mc.index = i;
            mc.magType = m;
            mc.fromArsenal = true;
            _arsenalCache.Add(mc);
        }
    }
    for (int i = 0; i < _arsenalCache.Size(); i++)
    {
        _vicinity.Add(_arsenalCache[i]);
    }
}

void InventoryModel::FindPlacement(GridSize footprint, int &outX, int &outY) const
{
    // Row-major scan for the first rect of size footprint with no overlap
    // against existing Grid cells. O(rows * cols * items) - fine for an
    // inventory-sized item count; revisit if this ever needs to hold
    // hundreds of items.
    // Columns are user-adjustable; never fewer than the item's own width or the
    // inner loop could never place it (infinite outer loop).
    int cols = InventoryGridCols();
    if (cols < footprint.w) cols = footprint.w;
    int row = 0;
    for (;; row++)
    {
        for (int col = 0; col + footprint.w <= cols; col++)
        {
            bool free = true;
            for (int i = 0; i < _grid.Size() && free; i++)
            {
                const InvCell &c = _grid[i];
                const bool overlapX = col < c.gx + c.footprint.w && c.gx < col + footprint.w;
                const bool overlapY = row < c.gy + c.footprint.h && c.gy < row + footprint.h;
                if (overlapX && overlapY)
                {
                    free = false;
                }
            }
            if (free)
            {
                outX = col;
                outY = row;
                return;
            }
        }
        // Row full - grid just grows. No failure case, per the design note:
        // grid space is cosmetic and never blocks a weight-legal item.
    }
}

// The world keeps simulating while this screen is open. Validate the live owner
// before a transfer, rather than trusting the last UI refresh's indices.
static bool CellStillOwned(const InvCell &cell, EntityAI *player)
{
    if (cell.fromArsenal) return true;
    VehicleSupply *supply = cell.container;
    EntityAI *unit = cell.pane == InvPane::Grid ? player :
        (cell.fromBody ? dyn_cast<Person, VehicleSupply>(supply) : nullptr);
    if (!unit && !supply) return false;
    if (cell.kind == InvItemKind::Weapon)
    {
        const int count = unit ? unit->NWeaponSystems() : supply->GetWeaponCargoSize();
        for (int i = 0; i < count; ++i)
            if ((unit ? unit->GetWeaponSystem(i) : supply->GetWeaponCargo(i)) == cell.weapon)
                return true;
        return false;
    }
    auto hasMagazine = [&](const Magazine *mag)
    {
        const int count = unit ? unit->NMagazines() : supply->GetMagazineCargoSize();
        for (int i = 0; i < count; ++i)
            if ((unit ? unit->GetMagazine(i) : supply->GetMagazineCargo(i)) == mag)
                return true;
        return false;
    };
    if (cell.isBox)
    {
        if (cell.boxMags.Size() == 0) return false;
        for (int i = 0; i < cell.boxMags.Size(); ++i)
            if (!hasMagazine(cell.boxMags[i])) return false;
        return true;
    }
    return cell.magazine && hasMagazine(cell.magazine);
}

InvTransferResult InventoryModel::CanTransfer(const InvCell &from, InvPane toPane) const
{
    if (!_player || from.IsEmpty() || !CellStillOwned(from, _player))
    {
        return InvTransferResult::Unavailable;
    }
    if (from.pane == toPane)
    {
        return InvTransferResult::SameCell;
    }

    if (toPane == InvPane::Grid)
    {
        if (from.kind == InvItemKind::Weapon && from.weapon)
            for (int i = 0; i < _player->NWeaponSystems(); ++i)
                if (_player->GetWeaponSystem(i) == from.weapon)
                    return InvTransferResult::SlotOccupied;
        // Rifles/launchers normally may ONLY occupy their single loadout slot - never
        // the storage grid. If that slot is already taken, refuse the pickup instead
        // of silently carrying an invisible extra long gun. Handguns are fine (they
        // show in the grid and can be swapped into the pistol slot). The debug "Rifles
        // in backpack" option lifts this restriction so extra long guns are carried in
        // the grid (as 3x2 tiles) instead of being refused.
        if (from.kind == InvItemKind::Weapon && from.weapon && !InventoryWeaponsInBackpack())
        {
            const int wt = from.weapon->_weaponType;
            if ((wt & MaskSlotPrimary) && HasWeaponWithMask(MaskSlotPrimary))
            {
                return InvTransferResult::SlotOccupied;
            }
            if ((wt & MaskSlotSecondary) && HasWeaponWithMask(MaskSlotSecondary))
            {
                return InvTransferResult::SlotOccupied;
            }
        }

        // Weight gate.
        const float afterPickup = ItemMass::TotalCarried(_player) + from.Mass();
        if (afterPickup > MaxCarryWeight())
        {
            return InvTransferResult::OverCarryWeight;
        }
        // Volume gate (invisible; per-side capacity). Adding this item must not push
        // the carried packed volume past the side's budget.
        if (ItemMass::TotalVolumeCarried(_player) + CellVolume(from) > InvVolumeBudget(_player))
        {
            return InvTransferResult::OverVolume;
        }
        return InvTransferResult::Ok;
    }

    // Player -> ground: always legal by weight (you're losing mass, not
    // gaining it), just check droppability.
    if (from.kind == InvItemKind::Weapon && from.weapon && !from.weapon->_canDrop)
    {
        return InvTransferResult::NotDroppable;
    }
    return InvTransferResult::Ok;
}

InvTransferResult InventoryModel::Transfer(const InvCell &from, InvPane toPane)
{
    const InvTransferResult check = CanTransfer(from, toPane);
    if (check != InvTransferResult::Ok)
    {
        return check;
    }

    if (toPane == InvPane::Grid)
    {
        // Arsenal: infinite - take a copy, never deplete the source.
        if (from.fromArsenal)
        {
            if (from.kind == InvItemKind::Weapon && from.weapon)
            {
                if (_player->AddWeapon(const_cast<WeaponType *>(from.weapon), /*force=*/true) < 0)
                    return InvTransferResult::Unavailable;
            }
            else if (from.magType)
            {
                // A shell box hands over a whole box of shells as full magazines (each
                // magazine is ShellsPerMag shells); any other arsenal item is one mag.
                int copies = 1;
                if (from.isBox)
                {
                    const int spm = ShellsPerMag(from.magType);
                    copies = spm > 0 ? from.stackCount / spm : from.stackCount;
                    if (copies < 1) copies = 1;
                }
                for (int c = 0; c < copies; c++)
                {
                    Ref<Magazine> m = new Magazine(const_cast<MagazineType *>(from.magType));
                    m->_ammo = from.magType->_maxAmmo;   // arsenal mags come out full, not empty
                    if (_player->AddMagazine(m, /*force=*/true) < 0)
                        return InvTransferResult::Unavailable;
                }
            }
            else
            {
                return InvTransferResult::Unavailable;
            }
            if (_player)
            {
                _player->PlayAction(ManActPutDown);
            }
            CutScene(from.kind == InvItemKind::Weapon ? "TakeWeapon" : "TakeMagazine");
            Refresh();
            return InvTransferResult::Ok;
        }

        VehicleSupply *container = from.container;
        if (!container)
        {
            return InvTransferResult::Unavailable;
        }

        // Looting a dead soldier: its gear is in weapon systems / magazines, so
        // take with RemoveWeapon/RemoveMagazine (not the cargo path).
        if (from.fromBody)
        {
            Person *body = dyn_cast<Person, VehicleSupply>(container);
            if (!body)
            {
                return InvTransferResult::Unavailable;
            }
            if (from.kind == InvItemKind::Weapon)
            {
                Ref<WeaponType> w = const_cast<WeaponType *>(from.weapon);
                if (_player->AddWeapon(w, /*force=*/true) < 0)
                    return InvTransferResult::Unavailable;
                body->RemoveWeapon(w);
            }
            else
            {
                // A shell box loots all its shells at once; otherwise a single mag.
                AutoArray<const Magazine *> mags;
                if (from.isBox) { for (int i = 0; i < from.boxMags.Size(); i++) mags.Add(from.boxMags[i]); }
                else if (from.magazine) mags.Add(from.magazine);
                for (int i = 0; i < mags.Size(); i++)
                {
                    Ref<Magazine> m = const_cast<Magazine *>(mags[i]);
                    if (_player->AddMagazine(m, /*force=*/true) < 0)
                        return InvTransferResult::Unavailable;
                    body->RemoveMagazine(m);
                }
            }
            if (_player)
            {
                _player->PlayAction(ManActPutDown);
            }
            CutScene(from.kind == InvItemKind::Weapon ? "TakeWeapon" : "TakeMagazine");
            Refresh();
            return InvTransferResult::Ok;
        }

        // force=true is what makes this weight-only: it skips CheckWeapon /
        // the slot-mask check entirely (confirmed VehicleAI.cpp:1764-1776 and
        // 1983-2000 - `if (!force) { CheckWeapon(...) }`). AI callers still
        // pass force=false elsewhere, so AI gear logic is untouched.
        //
        // CRITICAL: _magazineCargo is a RefArray<Magazine> (VehicleAI.hpp:33),
        // so RemoveMagazineCargo drops the array's reference and may FREE the
        // Magazine. We must hold our own Ref across remove->add or AddMagazine
        // gets a dangling pointer. This mirrors the engine's own take path,
        // which keeps a Ref<const Magazine> alive across the move
        // (Transport.cpp:805-828).
        if (from.kind == InvItemKind::Weapon)
        {
            Ref<WeaponType> w = const_cast<WeaponType *>(from.weapon);
            if (_player->AddWeapon(w, /*force=*/true) < 0)
                return InvTransferResult::Unavailable;
            container->RemoveWeaponCargo(w);
            if (GWorld->GetMode() == GModeNetware)
            {
                GetNetworkManager().RemoveWeaponCargo(container, w->GetName());
            }
        }
        else
        {
            // A shell box takes all its shells from the crate; otherwise a single mag.
            AutoArray<const Magazine *> mags;
            if (from.isBox) { for (int i = 0; i < from.boxMags.Size(); i++) mags.Add(from.boxMags[i]); }
            else if (from.magazine) mags.Add(from.magazine);
            for (int i = 0; i < mags.Size(); i++)
            {
                Ref<Magazine> m = const_cast<Magazine *>(mags[i]);
                if (_player->AddMagazine(m, /*force=*/true) < 0)
                    return InvTransferResult::Unavailable;
                container->RemoveMagazineCargo(m);
                if (GWorld->GetMode() == GModeNetware)
                {
                    // VERIFY signature against Transport.cpp:825 - the magazine
                    // variant keys off (container, creator, id).
                    GetNetworkManager().RemoveMagazineCargo(container, m->_creator, m->_id);
                }
            }
        }

        if (_player)
        {
            _player->PlayAction(ManActPutDown);   // vanilla gear-handling gesture
        }
        // Vanilla feedback: "Taking weapon/ammo" title + pickup sound.
        CutScene(from.kind == InvItemKind::Weapon ? "TakeWeapon" : "TakeMagazine");
        Refresh();
        return InvTransferResult::Ok;
    }

    return DropToGround(from);
}

void InventoryModel::EquipWeapon(const InvCell &cell)
{
    if (!_player || cell.kind != InvItemKind::Weapon || !cell.weapon)
        return;
    EntityAI *unit = _player;
    // Remember the player's choice so the per-frame enforcement and the loadout strip keep
    // THIS gun wielded (a second primary/handgun would otherwise snap back to the first).
    const int pi = SlotPrefIndex(cell.weapon->_weaponType);
    if (pi >= 0)
        g_equipPref[pi] = cell.weapon;
    // Select the first muzzle slot that belongs to this weapon system - that makes it the
    // active weapon (rifle/launcher/pistol), which is what "equip" means. Two identical
    // weapons share one WeaponType, so the first matching slot is the right one.
    for (int i = 0; i < unit->NMagazineSlots(); i++)
    {
        if (unit->GetMagazineSlot(i)._weapon == cell.weapon)
        {
            unit->SelectWeapon(i, true);
            return;
        }
    }
}

void InventoryModel::MoveWithinGrid(InvCell &cell, int newGx, int newGy)
{
    // Pure UI repositioning - no weight implication, always legal. Caller
    // (DisplayInventory) is responsible for collision-checking against
    // other grid cells if you want tiles to refuse to overlap visually;
    // per the design note this is cosmetic only, so a naive implementation
    // that allows overlap is acceptable too - your call on polish level.
    cell.gx = newGx;
    cell.gy = newGy;
}

InvTransferResult InventoryModel::DropToGround(const InvCell &from)
{
    // Legacy behaviour: auto-pick a nearby container, else the ground.
    return DropToTarget(from, nullptr, false);
}

InvTransferResult InventoryModel::DropToSource(const InvCell &from, int sourceIndex)
{
    if (sourceIndex < 0 || sourceIndex >= _vicSources.Size())
    {
        return DropToTarget(from, nullptr, false);
    }
    const VicSource &s = _vicSources[sourceIndex];
    if (s.isGround)
    {
        return DropToTarget(from, nullptr, /*forceGround*/ true);
    }
    if (s.fromBody)
    {
        // A person page (dead body or living squad-mate): hand the item to their gear.
        VehicleSupply *c = s.container;
        Person *person = c ? dyn_cast<Person, VehicleSupply>(c) : nullptr;
        if (person)
        {
            return DropToPerson(from, person);
        }
        return InvTransferResult::NotDroppable;
    }
    if (s.isArsenal)
    {
        // The arsenal is an infinite source, so dumping an item into it just deletes
        // it (the arsenal can hand back an identical one any time).
        return DiscardToArsenal(from);
    }
    return DropToTarget(from, (VehicleSupply *)s.container, false);
}

InvTransferResult InventoryModel::DropToPerson(const InvCell &from, Person *person)
{
    if (!_player || !person || from.IsEmpty() || !CellStillOwned(from, _player))
    {
        return InvTransferResult::Unavailable;
    }
    if (from.pane != InvPane::Grid)   // only the player's own carried items
    {
        return InvTransferResult::SameCell;
    }
    if (from.kind == InvItemKind::Weapon && from.weapon && !from.weapon->_canDrop)
    {
        return InvTransferResult::NotDroppable;
    }

    // Move the item from the player's worn gear to the other person's (dead body OR a
    // living squad-mate). Mirror of the fromBody take path in Transfer(). Hold a Ref
    // across remove->add so the object isn't freed mid-move.
    if (from.kind == InvItemKind::Weapon)
    {
        Ref<WeaponType> w = const_cast<WeaponType *>(from.weapon);
        for (int i = 0; i < person->NWeaponSystems(); ++i)
            if (person->GetWeaponSystem(i) == w)
                return InvTransferResult::SlotOccupied;
        if (person->AddWeapon(w, /*force=*/true) < 0)
            return InvTransferResult::Unavailable;
        _player->RemoveWeapon(w);
    }
    else
    {
        AutoArray<Ref<const Magazine>> mags = from.boxMags;
        if (!from.isBox && from.magazine) mags.Add(from.magazine);
        if (mags.Size() == 0) return InvTransferResult::Unavailable;
        // Add the entire box first. Roll back any refused addition before
        // removing anything from the player.
        for (int i = 0; i < mags.Size(); ++i)
        {
            if (person->AddMagazine(const_cast<Magazine *>(mags[i].GetRef()), true) < 0)
            {
                for (int added = 0; added < i; ++added)
                    person->RemoveMagazine(mags[added]);
                return InvTransferResult::Unavailable;
            }
        }
        for (int i = 0; i < mags.Size(); ++i)
            _player->RemoveMagazine(mags[i]);
    }

    _player->PlayAction(ManActPutDown);
    CutScene(from.kind == InvItemKind::Weapon ? "TakeWeapon" : "TakeMagazine");
    Refresh();
    return InvTransferResult::Ok;
}

InvTransferResult InventoryModel::DiscardToArsenal(const InvCell &from)
{
    if (!_player || from.IsEmpty() || !CellStillOwned(from, _player))
    {
        return InvTransferResult::Unavailable;
    }
    if (from.pane != InvPane::Grid)   // only the player's own carried items
    {
        return InvTransferResult::SameCell;
    }
    if (from.kind == InvItemKind::Weapon && from.weapon && !from.weapon->_canDrop)
    {
        return InvTransferResult::NotDroppable;
    }

    // Remove from the unit and simply let it go (no container, no ground holder).
    // The arsenal is a single-player preview convenience, so no network routing.
    if (from.kind == InvItemKind::Weapon)
    {
        Ref<WeaponType> w = const_cast<WeaponType *>(from.weapon);
        _player->RemoveWeapon(w);
    }
    else if (from.isBox)
    {
        for (int i = 0; i < from.boxMags.Size(); i++)
        {
            Ref<Magazine> m = const_cast<Magazine *>(from.boxMags[i].GetRef());
            _player->RemoveMagazine(m);
        }
    }
    else
    {
        Ref<Magazine> m = const_cast<Magazine *>(from.magazine.GetRef());
        _player->RemoveMagazine(m);
    }

    _player->PlayAction(ManActPutDown);   // vanilla gear-handling gesture
    Refresh();
    return InvTransferResult::Ok;
}

InvTransferResult InventoryModel::DropToTarget(const InvCell &from,
                                               VehicleSupply *preferred,
                                               bool forceGround)
{
    if (!_player || from.IsEmpty() || !CellStillOwned(from, _player))
    {
        return InvTransferResult::Unavailable;
    }
    if (from.pane != InvPane::Grid)
    {
        return InvTransferResult::SameCell;
    }
    if (from.kind == InvItemKind::Weapon && from.weapon && !from.weapon->_canDrop)
    {
        return InvTransferResult::NotDroppable;
    }

    // Find a nearby container with room. If one exists, its AddWeaponCargo/
    // AddMagazineCargo handles everything. If none has room (or none is near),
    // we spawn a WeaponHolder ourselves using the VERIFIED placement sequence
    // from Transport.cpp:1338-1350 - note AddWeaponCargo/AddMagazineCargo also
    // auto-spawn a holder when a container is full (Transport.cpp:1411-1428),
    // so a fallback holder only needs creating when there's no container at all.
    VehicleSupply *target = nullptr;
    const InvItemKind kind = from.kind;
    const int magazineCount = from.isBox ? from.boxMags.Size() : 1;
    auto hasRoom = [kind, magazineCount](VehicleSupply *c) {
        return (kind == InvItemKind::Weapon) ? c->GetFreeWeaponCargo() > 0
                                             : c->GetFreeMagazineCargo() >= magazineCount;
    };
    // An explicit destination must accept the whole item, never a partial box.
    if (preferred && !hasRoom(preferred))
        return InvTransferResult::Unavailable;
    if (forceGround)
    {
        // Reuse an existing ground holder with room, so repeated drops don't spawn
        // a pile of separate holders (they all share the one Ground page).
        for (int i = 0; i < _containers.Size(); i++)
        {
            VehicleSupply *c = _containers[i];
            if (c && IsGroundHolderType(c) && hasRoom(c))
            {
                target = c;
                break;
            }
        }
    }
    else if (preferred && hasRoom(preferred))
    {
        // Explicit page target (e.g. the car the player is looking at).
        target = preferred;
    }
    else if (!preferred)
    {
        for (int i = 0; i < _containers.Size(); i++)
        {
            VehicleSupply *c = _containers[i];
            if (c && hasRoom(c))
            {
                target = c;
                break;
            }
        }
    }

    if (!target)
    {
        const bool secondary =
            from.kind == InvItemKind::Weapon &&
            from.weapon &&
            (from.weapon->_weaponType & MaskSlotSecondary) != 0 &&
            (from.weapon->_weaponType & MaskSlotPrimary) == 0;

        Ref<EntityAI> holder = NewVehicle(secondary ? "SecondaryWeaponHolder" : "WeaponHolder");
        Ref<VehicleSupply> container = dyn_cast<VehicleSupply, EntityAI>(holder.GetRef());
        if (!container)
        {
            return InvTransferResult::Unavailable;
        }

        // Verified placement sequence (Transport.cpp:1338-1350).
        Matrix4 transform = _player->Transform();
        transform.SetPosition(_player->Position() + _player->Direction() * 0.5f);
        container->PlaceOnSurface(transform);
        container->SetTransform(transform);
        container->Init(transform);
        if (!hasRoom(container))
            return InvTransferResult::Unavailable;
        GWorld->AddBuilding(container);
        if (GWorld->GetMode() == GModeNetware)
        {
            GetNetworkManager().CreateVehicle(container, VLTBuilding, "", -1);
        }

        target = container;
    }

    // Remove from the unit (holding a Ref so nothing is freed mid-move), then
    // hand to the container. RemoveWeapon(const WeaponType*) and
    // RemoveMagazine(const Magazine*) both confirmed (EntityAI.hpp:864,894).
    if (from.kind == InvItemKind::Weapon)
    {
        Ref<WeaponType> w = const_cast<WeaponType *>(from.weapon);
        if (target->GetFreeWeaponCargo() < 1 ||
            target->AddWeaponCargo(w, 1, /*deleteWhenFull=*/true) < 0)
            return InvTransferResult::Unavailable;
        _player->RemoveWeapon(w);
        if (GWorld->GetMode() == GModeNetware)
        {
            GetNetworkManager().AddWeaponCargo(target, w->GetName());
        }
    }
    else
    {
        // Reserve capacity for the whole box before making any mutation.
        AutoArray<const Magazine *> mags;
        if (from.isBox)
        {
            for (int i = 0; i < from.boxMags.Size(); i++)
                mags.Add(from.boxMags[i]);
        }
        else if (from.magazine)
        {
            mags.Add(from.magazine);
        }
        if (mags.Size() == 0 || target->GetFreeMagazineCargo() < mags.Size())
            return InvTransferResult::Unavailable;
        for (int i = 0; i < mags.Size(); i++)
        {
            Ref<Magazine> m = const_cast<Magazine *>(mags[i]);
            if (target->AddMagazineCargo(m, /*deleteWhenFull=*/true) < 0)
            {
                for (int added = 0; added < i; ++added)
                    target->RemoveMagazineCargo(const_cast<Magazine *>(mags[added]));
                return InvTransferResult::Unavailable;
            }
        }
        for (int i = 0; i < mags.Size(); ++i)
        {
            const Magazine *m = mags[i];
            _player->RemoveMagazine(m);
            if (GWorld->GetMode() == GModeNetware)
            {
                GetNetworkManager().AddMagazineCargo(target, m);
            }
        }
    }

    if (_player)
    {
        _player->PlayAction(ManActPutDown);   // vanilla gear-handling gesture
    }
    // Vanilla feedback: "Dropping weapon/ammo" title + drop sound.
    CutScene(from.kind == InvItemKind::Weapon ? "DropWeapon" : "DropMagazine");
    Refresh();
    return InvTransferResult::Ok;
}

} // namespace Poseidon
