// Adapted from the author-supplied CWR-Physical Inventory source donation (2026).
// Distributed under this project's GPL-3.0-or-later licence and Section 7 terms; see LICENSE.
#pragma once

// Poseidon/UI/Inventory/InventoryModel.hpp
//
// Single-player inventory: equipped slots plus paginated storage and vicinity.
// Grid placement is cosmetic. Pickups enforce live ownership, duplicate/slot,
// weight and volume checks; extra long guns require the explicit backpack option.
// Direct transfers are not network-authoritative; multiplayer uses classic Gear.

#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <Poseidon/Foundation/Types/LLinks.hpp>
#include <Poseidon/AI/EntityAIType.hpp> // Magazine definition for snapshot references

#include "ItemFootprint.hpp"

namespace Poseidon
{

class EntityAI;
class Person;
class VehicleSupply;
class WeaponType;
class MagazineType;
class Magazine;

enum class InvPane
{
    Ground,   //!< vicinity containers - not part of the player's inventory
    Grid,     //!< the player's single monolithic inventory
};

enum class InvItemKind
{
    Empty,
    Weapon,
    Magazine,
};

// --- Weapon hotkeys -------------------------------------------------------
// Slots 0..9 correspond to the number keys 1..9 then 0. A slot stores a weapon
// class name; pressing the matching number in-game selects that weapon if the
// player is carrying it. Shared so DisplayInventory can set them and the mission
// display can read them.
//! Bind a hotkey. `weaponName` is a kind-qualified key ("w:<weapon>"/"m:<magazine>");
//! `inst` is the clicked item pointer, used only to badge the exact item clicked.
void SetWeaponHotkey(int slot, RString weaponName, const void *inst = nullptr);
RString GetWeaponHotkey(int slot);
//! Which hotkey slot (0-9) the item with kind-qualified `key` and instance `inst` is
//! bound to, or -1. For magazines the instance must match, so only the clicked
//! throwable badges. Used to draw the persistent number badge on an inventory tile.
int WeaponHotkeyBadgeSlot(const char *key, const void *inst);
//! Select the weapon bound to `slot` if the player is carrying it. Returns true
//! on a successful switch. Lives here so callers need no weapon-system headers.
bool ApplyWeaponHotkey(Person *player, int slot);


// --- Inventory settings ---------------------------------------------------
// Loaded from the Open Poseidon Engine user directory, inventory_settings.cfg ("inventoryKey=<scancode>",
// "showEditMenu=<0|1>"). Groundwork for a proper settings screen.
void LoadInventorySettings();
void SaveInventorySettings();     //!< persist current settings back to the cfg file
int  InventoryOpenKey();          //!< SDL scancode that opens/closes the inventory
bool InventoryEditMenuVisible();  //!< show the debug/edit sliders overlay?
void SetInventoryOpenKey(int scancode);
void SetInventoryEditMenuVisible(bool show);

// --- Global inventory UI settings (edit menu; persisted) ------------------
// All are live-adjustable from the in-inventory edit menu and saved as defaults.
float InventoryGlobalIconScale();             void SetInventoryGlobalIconScale(float v);   //!< 0.40..2.00
int   InventoryGridCols();                    void SetInventoryGridCols(int cols);         //!< 1..12
float InventoryVolumeCapOverride();           void SetInventoryVolumeCapOverride(float l); //!< 0 = per-side default
bool  InventoryAccentTheming();               void SetInventoryAccentTheming(bool b);
bool  InventoryAccentBorders();               void SetInventoryAccentBorders(bool b);
void  InventoryAccentColor(float &r, float &g, float &b);
void  SetInventoryAccentColor(float r, float g, float b);
float InventoryVicWinScaleW();                void SetInventoryVicWinScaleW(float v);      //!< 0.50..1.60
float InventoryVicWinScaleH();                void SetInventoryVicWinScaleH(float v);
float InventoryInvWinScaleW();                void SetInventoryInvWinScaleW(float v);
float InventoryInvWinScaleH();                void SetInventoryInvWinScaleH(float v);
bool  InventoryWeaponsInBackpack();           void SetInventoryWeaponsInBackpack(bool b); //!< rifles/launchers in grid
bool  InventoryInfiniteWeight();              void SetInventoryInfiniteWeight(bool b);
bool  InventoryInfiniteVolume();              void SetInventoryInfiniteVolume(bool b);
void  ResetInventorySettingsToDefault();      //!< restore every UI setting above

// Call each in-game frame: keeps the wielded rifle's stats matching the equipped
// primary when "Rifles in backpack" leaves a second active rifle around (fixes the
// M16-model/AK-stats desync after a grenade->rifle switch). No-op unless enabled.
void  EnforceEquippedPrimaryWeapon(EntityAI *player);


// Auto-remove spare (not-chambered) magazines that are completely empty, so 0-round mags
// don't pile up in the inventory. Call each in-game frame for the player.
void  CleanupEmptyMagazines(EntityAI *player);

// Shared action-menu/drag-drop checks: duplicates, weapon slots, weight and volume.
bool  InventoryCanCarryWeapon(EntityAI *player, const WeaponType *w);
bool  InventoryCanCarryMagazine(EntityAI *player, const Magazine *m);

// --- Per-icon tuning (debug tool + runtime overrides) ---------------------
// Keyed by weapon/magazine class name. Zero footprint fields mean "use default".
// Persisted to inventory_icon_tuning.txt in the user directory so tuned values survive and can be baked in.
struct IconTune
{
    // Inventory (grid) side.
    float size = 1.0f;   //!< grid image size multiplier (on top of the normal fill)
    float ox = 0.0f;     //!< grid image x nudge, in tile fractions (+ = right)
    float oy = 0.0f;     //!< grid image y nudge, in tile fractions (+ = down)
    int gw = 0;          //!< grid footprint width  (0 = default)
    int gh = 0;          //!< grid footprint height (0 = default)
    // Vicinity side (independent of the grid side).
    float vsize = 1.0f;  //!< vicinity image size multiplier
    float vox = 0.0f;    //!< vicinity image x nudge
    float voy = 0.0f;    //!< vicinity image y nudge
    int vw = 0;          //!< vicinity width         (0 = default)
    // Equipped/loadout slot (independent again; the slot itself is fixed-size).
    float lsize = 1.0f;  //!< equipped-slot image size multiplier
    float lox = 0.0f;    //!< equipped-slot image x nudge
    float loy = 0.0f;    //!< equipped-slot image y nudge
};
IconTune *FindIconTune(const char *className);   //!< null if no override exists
IconTune &EditIconTune(const char *className);   //!< get-or-create for editing
void LoadIconTuning();
void SaveIconTuning();

/// One item. For Grid-pane cells this also carries its grid position -
/// cosmetic, per the design note above, but persisted so items don't jump
/// around every time the screen redraws.
struct InvCell
{
    InvPane pane = InvPane::Ground;
    InvItemKind kind = InvItemKind::Empty;

    /// Index within the owning container's cargo array (Ground), or the
    /// unit's weapon/magazine array (Grid). -1 for empty cells.
    int index = -1;

    /// Ground-pane only: which container this came from (a supply crate, or a
    /// dead soldier's body - Person is itself a VehicleSupply).
    LLink<VehicleSupply> container;
    bool fromBody = false;   //!< source is a dead soldier (read/take via weapon systems)

    const WeaponType *weapon = nullptr;
    // The world continues simulating while the UI is open. Keep displayed and
    // dragged magazines alive even if their live owner removes them; ownership
    // is checked again before any mutation.
    Ref<const Magazine> magazine;

    // Arsenal source: an infinite "one of everything" list. Weapons use `weapon`;
    // magazines have no instance yet, so they carry the TYPE and a fresh instance
    // is created on take.
    bool fromArsenal = false;
    const MagazineType *magType = nullptr;

    /// Boxed loose-round ammo (shotgun shells). When isBox is true this ONE tile
    /// represents several identical shell magazines grouped as a "box of N shells",
    /// so the inventory shows a box count instead of one tile per shell (and never
    /// the raw pellet count of a single buckshot shell). Every entry in boxMags is
    /// still a real, separate engine magazine - one reload for the weapon - so
    /// firing/reloading is completely unaffected. Non-box cells leave isBox=false,
    /// stackCount=1 and boxMags empty.
    bool isBox = false;
    int  stackCount = 1;                   //!< shells in this box (box cells only)
    AutoArray<Ref<const Magazine>> boxMags; //!< retained grouped shell magazines

    /// Grid-pane only: top-left cell and footprint size, in grid units.
    int gx = 0, gy = 0;
    GridSize footprint{1, 1};

    bool IsEmpty() const { return kind == InvItemKind::Empty; }

    RString DisplayName() const;
    RStringB PictureName() const;
    RStringB ClassName() const;   //!< weapon/magazine class name (for w_<>/m_<> icon lookup)
    bool GetAmmo(int &current, int &capacity) const;
    float Mass() const;
    /// Name of the source this ground item came from (crate / dead soldier).
    RString SourceLabel() const;
    /// Pointer identifying the source, for grouping ground items by source.
    const void *SourceKey() const;
};

/// Kind-qualified tuning key ("w:<class>" / "m:<class>") so a weapon and its
/// same-named magazine (e.g. a launcher and its rocket) tune independently.
RString IconTuneKey(const InvCell &cell);

enum class InvTransferResult
{
    Ok,
    SameCell,
    OverCarryWeight,     //!< would exceed MaxCarryWeight
    OverVolume,          //!< would exceed the side's (invisible) volume capacity
    NotDroppable,         //!< WeaponType::_canDrop == false
    Unavailable,          //!< source vanished mid-drag (MP)
    SlotOccupied,         //!< rifle/launcher slot already taken (no invisible stuffing)
};

/// One slot of the Reforger-style top loadout strip. Four fixed slots: the
/// equipped primary/secondary/handgun weapons and a throwable (grenade)
/// summary. Weapon slots carry the loaded magazine's rounds and a count of
/// spare compatible magazines; the throwable slot carries the grenade count.
struct LoadoutSlot
{
    enum Kind { Primary, Secondary, Handgun, Throwable };

    Kind kind = Primary;
    InvCell cell;            //!< the weapon (P/S/H) or a representative grenade
    bool hasItem = false;    //!< false -> draw an empty slot
    int count = 0;           //!< weapon: rounds in the loaded mag; throwable: grenades
    int spare = 0;           //!< weapon: spare compatible magazines carried
};

class InventoryModel
{
public:
    explicit InventoryModel(Person *player);

    void Refresh();

    Person *GetPlayer() const { return _player; }

    /// The player's single grid. Ground-pane items live in Vicinity().
    /// v3: the grid is magazines-only (storage). Equipped weapons live in the
    /// loadout strip - see Loadout().
    const AutoArray<InvCell> &Grid() const { return _grid; }
    const AutoArray<InvCell> &Vicinity() const { return _vicinity; }

    /// One vicinity "page": a nearby container/body (even if EMPTY, so an empty car
    /// still shows its name), the arsenal, or the always-present Ground drop target.
    struct VicSource
    {
        RString label;                    //!< page header text
        LLink<VehicleSupply> container;   //!< drop target (null for ground/arsenal)
        bool isGround = false;            //!< the "Ground" drop page
        bool isArsenal = false;           //!< the unlimited arsenal page
        bool fromBody = false;            //!< a dead-soldier source (loot only)
    };
    const AutoArray<VicSource> &VicinitySources() const { return _vicSources; }
    //! True if `cell` belongs on the page for source index `srcIdx`.
    bool CellOnSource(const InvCell &cell, int srcIdx) const;

    /// The four top-strip slots (primary, secondary, handgun, throwable),
    /// always four entries; slots with no item have hasItem == false.
    const AutoArray<LoadoutSlot> &Loadout() const { return _loadout; }

    /// True if `cell` is a throwable magazine (grenade/mine/satchel) - i.e. usable
    /// by a Throw/Put muzzle. Such items can be hotkey-bound like weapons.
    bool IsThrowableItem(const InvCell &cell) const;

    /// Human-readable label for what the Vicinity list is showing (nearest
    /// container's name, plus a count if several are in range).
    RString VicinitySourceLabel() const;

    float CarriedMass() const { return _carriedMass; }

    //! Player's faction as an index: 0=West, 1=East, 2=Resistance, 3=Civilian, -1=other.
    //! Used to pick the per-side loadout ghost silhouettes.
    int PlayerSideIndex() const;
    float MaxCarryWeight() const;    //!< per-side carry budget (kg)
                                     //!< so the display doesn't need to
                                     //!< include ItemMass directly.

    static float VicinityRadius() { return 4.0f; }

    /// If weapon w has a magazine loaded in one of the unit's muzzle slots,
    /// returns its current/capacity rounds. Used to show the rifle's ammo on
    /// hover - the loaded magazine isn't listed as a separate grid item.
    bool WeaponLoadedAmmo(const WeaponType *w, int &cur, int &cap) const;

    /// Pure live ownership/slot/weight/volume check. Grid space is cosmetic.
    InvTransferResult CanTransfer(const InvCell &from, InvPane toPane) const;

    /// Performs the transfer via EntityAI::AddWeapon/AddMagazine(force=true)
    /// and the matching container Remove/Add. MP note: in multiplayer this
    /// should be routed through the same network messages the action-menu
    /// take/drop path already uses (NetworkClientActions.cpp:1815/:1831) so
    /// the server stays authoritative - see the NOTE in the .cpp.
    InvTransferResult Transfer(const InvCell &from, InvPane toPane);

    /// Reposition within the grid only - pure UI, no weight check, always
    /// legal (the item's already carried).
    void MoveWithinGrid(InvCell &cell, int newGx, int newGy);

    /// Equip (select) the weapon in `cell` - make it the active weapon. Used to change
    /// weapons by dragging a carried weapon onto a loadout slot. No-op for non-weapons.
    void EquipWeapon(const InvCell &cell);

    InvTransferResult DropToGround(const InvCell &from);

    /// Move a grid item to the vicinity page at `sourceIndex` (see
    /// VicinitySources()): into that specific container, or - for the Ground page -
    /// forced onto the ground as a WeaponHolder regardless of nearby containers.
    InvTransferResult DropToSource(const InvCell &from, int sourceIndex);

    /// Dump (delete) a carried item by dropping it onto the infinite arsenal page.
    InvTransferResult DiscardToArsenal(const InvCell &from);

    /// Finds a free footprint-sized rect starting from (0,0), scanning
    /// row-major. If the visible rows are full, returns a position in a new
    /// row past the current bottom - the grid just grows. Never fails.
    void FindPlacement(GridSize footprint, int &outX, int &outY) const;

private:
    void ScanGrid();
    void ScanVicinity();
    void ScanLoadout();
    /// Append the infinite arsenal (one of every public weapon and magazine type).
    void AddArsenal();
    /// Scan nearby supply crates (into _containers) and dead soldiers (into
    /// _bodies) within VicinityRadius.
    void CollectContainers();

    /// Core drop routine. `preferred` (if non-null and it has room) receives the
    /// item; otherwise, when `forceGround` is false, the first nearby container
    /// with room is used, falling back to a freshly spawned ground WeaponHolder.
    /// When `forceGround` is true, containers are ignored and a ground holder is
    /// always spawned.
    InvTransferResult DropToTarget(const InvCell &from, VehicleSupply *preferred,
                                   bool forceGround);

    /// Hand a carried item to another person's worn gear (a dead body OR a living
    /// squad-mate you command): removes it from the player and adds it to `person`.
    InvTransferResult DropToPerson(const InvCell &from, Person *person);

    /// True if the player already carries a weapon occupying the given slot mask.
    bool HasWeaponWithMask(int mask) const;

    /// Spare magazines carried (not currently loaded) that any muzzle of w accepts.
    int SpareMagCount(const WeaponType *w) const;
    /// True if mt is used by a throw/put pseudo-weapon muzzle (grenade/mine).
    bool IsThrowable(const MagazineType *mt) const;

    static constexpr int GridCols = 7;   //!< visible width; height is open-ended

private:
    LLink<Person> _player;

    AutoArray<InvCell> _grid;
    AutoArray<InvCell> _vicinity;
    AutoArray<LoadoutSlot> _loadout;
    AutoArray<int> _equippedIdx;   //!< weapon-system indices shown in loadout slots
    AutoArray<LLink<VehicleSupply>> _containers;
    AutoArray<LLink<Person>> _bodies;   //!< nearby dead soldiers
    AutoArray<LLink<Person>> _commandUnits; //!< nearby living squad-mates you command
    AutoArray<VicSource> _vicSources;   //!< one vicinity page per container/body/soldier/ground

    AutoArray<InvCell> _arsenalCache;   //!< built once per open (enumerating the
    bool _arsenalBuilt = false;         //!< type banks is not free) then reused

    float _carriedMass = 0.0f;
    float _carriedVolume = 0.0f;    //!< total packed volume (L) currently carried
    float _volumeCapacity = 0.0f;   //!< this side's invisible volume budget (L)
};

} // namespace Poseidon
