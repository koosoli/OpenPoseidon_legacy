# Physical inventory and multiplayer developer lockout — 2026-10-07

Source commits: `d164ad3b` (selective donated inventory plus agent integration/MP
guards), `15c4bc08` (omit unsafe optional shared-model 3D preview).
Branch: `codex/private-release-prep-20261006`. Both binaries deployed together
through the locked supported installer. Acceptance stamp was `15c4bc08+dirty`:
only this smoke-test script was uncommitted; engine/renderer code matched the
committed build. This is the baseline acceptance before adding original icons.

The eight donated sources retain attribution and use the existing GPL-3.0-or-later
and Section 7 terms. Original hashes are recorded in
[the source receipt](../roadmap/decisions/PHYSICAL-INVENTORY-SOURCE-20261007.md).
The donor's older engine, binaries, recoil/medical/fuel/altered-ammunition changes
and missing icon pack are excluded.

The built-in display opens with configurable **O**; **K** toggles its settings.
Vicinity pages cover ground/container/person gear; a storage grid and equipped
slots support transfer/equip and number-key binding. Weight/volume checks apply
to this SP inventory and normal SP player cargo pickups. Transaction code checks
live ownership, duplicate/occupied weapon slots, destination capacity and add
success before removing the source. Multiplayer retains classic Gear.

`Build.ps1 -Target PoseidonGame` and `PoseidonTests` passed. Controls migration:
117 assertions in 11 cases, including custom/unbound settings. Developer command
registry: 15 assertions in six cases. Installed `Test-PhysicalInventory.ps1`
passed with isolated settings and no custom icon pack: open, drag one of two
grenades to ground (2→1), click to retrieve (1→2), settings toggle, Escape/O close
and reopen, and equipped M16/spare magazine icons. Native screenshots were
inspected, with no crash or GPU validation errors. Receipts:
[SP](physical-inventory-20261007-runtime.json),
[hosted MP](multiplayer-dev-guard-20261007-runtime.json).
Captures: `build/physical-inventory/20261007-181547-898/`.

An initial attempt exposed an actual rendering error: the legacy 3D character
control selected an HDR world pipeline inside the LDR UI pass, invalidating
frames. The fallback resource now omits this optional preview; native world
rendering remains visible between the inventory panes. No shared soldier shape
flags or renderer workaround was used. A future isolated preview pass remains
out of this bounded integration.

The MP runner creates a private local Everon wizard session through the ordinary
menus. After the hosted setup enters the network world, harness `eval` is rejected
with `developer harness disabled in multiplayer`; ping and orderly exit still
work. No crash/validation error occurred. This verifies the central live-session
harness policy, not a complete multiplayer match or every visual key combination.
Source gates also close the dev overlay, discard queued mutations, release Zeus
camera/input capture, disable brush/cave/dev-torch and sticky cheats, and deny
console calls and legacy developer shortcuts in `GModeNetware`. Standard gameplay
input stays available. This protects the standard client's developer features;
it is not server-side protection against modified clients.

No inventory PBO or downloaded artwork is required. Sixteen original realistic
equipment-category illustrations are now versioned as unmodified generated PNG
sources and prepared DXT5 PAAs in `assets/inventory`, with written prompts and
SHA256 records under `content/inventory-icons`. No game/donor image was supplied
to image generation. Native Windows sprite preparation only trims transparent
margins/scales for runtime; active runtime payload is about 4.3 MiB. Build stages
and Deploy verifies all 16 PAAs. Known classic classes use these icons; unknown
addon classes retain their own configured pictures. Missing artwork falls back
to installed pictures/labelled tiles independently of item ownership. Final new
icon runtime verification is pending. Optional metadata/settings use the
user directory. Other engine-owned shipped menu/logo/material/vehicle-action
data and reproducible generators are versioned; the complete staged `Mods` and
`assets` payload is installed by `Deploy.ps1`. See
[content delivery](../../content/default-packs/README.md). Save/load, every mod
class, corpse/squadmate transfers and an exhaustive capacity/failure matrix were
not runtime-certified by this bounded smoke test.
