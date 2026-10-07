# Physical inventory feasibility review — 2026-10-07

State: **SELECTIVELY_PORTED / INSTALLED_SP_SMOKE_PASSED / MP_CLASSIC_GEAR**.

Reviewed the owner-supplied local folder
`inspiration/CWR-Physical Inventory/CWR-Physical Inventory` in the main checkout.
This is a complete older CWR engine snapshot without Git metadata, not a standalone
inventory plugin. Its LICENSE is byte-identical to this project's LICENSE:
SHA-256 `ad3e819baa7a89b32204478da77adc5bb5569a4a4f846716295c92c29794d0ce`.
Both describe GPL-3.0-or-later with the same Section 7 terms. The owner reports
receiving the author's GPL release. No source-license mismatch was identified.
Retain attribution and license notices when porting. This is not an asset rights
clearance: the supplied README explicitly separates game data from source licensing.
GPL reference: https://www.gnu.org/licenses/gpl-3.0.html .

## What can be reused

`UI/Inventory/{DisplayInventory,InventoryModel,ItemFootprint}` and
`World/Entities/Weapons/ItemMass` implement a graphical inventory with vicinity
pages, drag/drop, loadout slots, hotkeys, footprint layout and carrying budgets.
Integration also changes input actions, in-game UI, transport pickup, soldier
simulation and startup. The snapshot includes adjacent recoil/medical/fuel
behavior: do not import the whole fork or those unrelated gameplay changes
implicitly. Port against the current engine and retain the classic inventory as
a fallback. Save/load, multiplayer, AI, slot-mask and pickup/duplication behavior
require testing; no runtime acceptance of this inventory has been performed.

## Data needed besides executable and renderer DLL

- `DisplayInventory.cpp` calls `Load("RscDisplayInventory")`, including an optional
  3D-preview control. The named UI configuration is absent from the supplied
  source snapshot. Provide a compatible engine-owned definition rather than
  replacing the user's retail BIN files.
- Transparent images are referenced as `invicons/w_*.paa`, `m_*.paa` and additional
  class/basename-keyed icons. No inventory icon pack is present in the supplied
  source or dist directory. These images need their own source/license record.
- `invicons/_crops.txt` is optional precomputed crop metadata;
  `invicons/_tuning.txt` optionally overrides compiled defaults.
- `Remastered/inventory_settings.cfg` is optional and missing-file loading keeps
  defaults. During a port, move writable settings to the engine's user directory.
- Character preview shapes and stock gear pictures come from the player's
  installed game/addons, not a new set of redistributed game archives.

## Recommended delivery and missing-data behavior

Use the existing default-content packaging mechanism for a small versioned
`Mods/@OP_Inventory/AddOns/op_inventory_ui.pbo`, plus `mod.json` and the relevant
notices. Include only approved icons/configuration, with a unique PBO prefix.
Make the build stage it and Deploy/the future release packager install it with
the matching executable. Users receive one release archive containing code and
the data pack; no manual asset copying or overwriting original PBOs is needed.
This package is a proposal, not implemented or included in current builds.

Make inventory transactions independent of cosmetic data. When an icon is absent,
try the item's config picture/stock gear picture, then a built-in labelled generic
tile. If the UI definition cannot be loaded, open the classic gear interface and
log one useful message. Missing crop/tuning/settings files should use defaults.
Do not block game startup, discard items or silently disable pickups for a missing
cosmetic package.

The donated code already attempts stock picture fallback, but its layout can
mark a successfully resolved stock icon as blocked when the preferred `invicons`
file is absent. Its ghost icons also require that missing pack. Therefore good
missing-asset behavior must be fixed and tested during the port, not assumed from
the presence of fallback code alone. Existing `fopen` metadata access is also a
loose-file path and needs archive-aware reads before metadata is put in a PBO.

## Integration update

The owner subsequently requested implementation. The eight scoped sources are
now adapted against the current branch, with an engine-owned fallback resource
instead of the proposed additional PBO. No external inventory icon pack is needed:
stock gear pictures or labelled tiles remain usable. Settings live in the user
directory. O/K are configurable with selective v5 profile migration. Multiplayer
keeps classic Gear because donated direct transfers need networking work.
The source receipt is [PHYSICAL-INVENTORY-SOURCE-20261007.md](PHYSICAL-INVENTORY-SOURCE-20261007.md).
Build, 11 controls-profile tests, six developer-command tests and installed SP
pickup/drop/open/settings/icon smoke tests passed. A private hosted Everon session
rejects developer harness commands. See [acceptance](../../verification/physical-inventory-and-mp-guard-20261007.md).
The optional 3D character preview is omitted after exposing an HDR/UI pass mismatch;
save/load, all mod classes and the full transfer matrix remain unverified.
