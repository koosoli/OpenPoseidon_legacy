# Shipped content sources (work in progress)

Road and terrain normal integration is implemented. Three-surface visual acceptance
remains open. Jeep NPC/player firing, reload, save/load and lifecycle are implemented
for one SP seat; automatic package integration is under acceptance, not yet a claim
that the complete movement/visual/compatibility test matrix is finished.

## Build and installation

The supported Windows `scripts/Build.ps1` client build also invokes the existing
PoseidonTools asset encoder and `Build-DefaultContent.ps1`. Material generation
requires Python 3 with NumPy and Pillow on the build machine only. Runtime has no
Python requirement. For an already built tool:

```powershell
./scripts/Build-DefaultContent.ps1
```

Output is `dist/x64-win-<configuration>/Mods/@OP_VisualUpgrade`, containing mod.json
and AddOns/op_ground_materials.pbo, plus `@OP_VehicleActions` with mod.json and
AddOns/op_jeep_actions.pbo. Deploy.ps1 copies these own files and verifies
their SHA256; it does not modify Dta/Data.pbo, LandText.pbo, original models or WRP.
Source-only roughness/height/soil artifacts are not claimed as bound maps.

All engine-owned runtime content has versioned sources here, under `content/menu`
or `assets/ui` and `assets/inventory`, plus the generators in `scripts/default-content`. Deploy installs
the generated PBOs, their mod.json files, menu mission and logo alongside the
matching executable/DLL. Distribute that complete staged layout; the two binaries
alone do not include the material/animation packs. Generated PBOs belong in the
release archive and are reproducible from these sources, rather than requiring
unversioned files on a developer's machine. Original retail/addon data remains
the player's separately installed content.

The physical inventory (October 7) has a compiled fallback UI and 17 original
realistic equipment-category icons. Their source PNGs and prepared runtime PAAs
are versioned in `assets/inventory`; prompts and hashes are recorded under
`content/inventory-icons`. Build stages the PAAs in `dist/.../assets/inventory`
and Deploy installs and verifies every file. Ship this folder alongside the
exe/DLL. No extra download or AI connection is needed. Missing categories fall
back to installed game/addon pictures or labelled tiles. Optional `invicons`
artwork from third parties is not bundled or required.
Inventory settings and icon tuning are created in the writable user directory;
these personal files are not release assets and must not be committed.
VehicleActions includes six reproducible own RTMs and an exact Jeep cargo-0
mapping using the version-3 personal primary bullet weapon family. Legacy
version-1/2 profiles retain explicit weapon-name restrictions. Family eligibility
does not mean every weapon model has passed pose/handling acceptance. It is SP-only; MP,
drivers, other vehicles and ordinary cargo retain the original behaviour.
The authored MC-skeleton calibration/IK source is under `vehicle-actions` and
`scripts/default-content/generate_jeep_animations.py`. Runtime uses the existing
RTM/move graph, magazine and bullet implementation. Recoil uses the engine's
existing procedural weapon recoil; the separate source recoil clip is available
for authoring comparison, not a second source of firing events.

At normal startup the engine discovers compatible installed packs without a mod
parameter. Original/RES data precede default packs; explicit user mods follow.
An exact original-archive ownership check prevents the default road mapping from
replacing a texture selected from a different user-mod archive. End-to-end user
mod precedence and clean-release tests remain on the acceptance list.

## Disable (restart required)

```powershell
./OpenPoseidon.exe --no-visual-upgrade
./OpenPoseidon.exe --no-vehicle-actions
```

These suppress automatic loading independently. Explicitly passing the same
package via `--mod` is an explicit user selection, not automatic loading.
For the development albedo-only comparison set
`POSEIDON_VISUAL_UPGRADE_NORMALS=0` before starting. This affects only the mapped
default normals, not other authored material normal maps. Unset it for normal use.

## Source provenance and channel contract

The generator takes numerical material parameters, not original texture pixels.
No copied game art is included in these sources. Read-only original references
used to identify texture names and UV dimensions live under ignored build output.
The authored files follow the repository source license.

Albedo is display/sRGB colour; NOHQ is linear data, with X in alpha and Y in green.
The red channel is zero to avoid enabling the existing optional height/parallax
path. An image-inspection tool will call a NOHQ's alpha translucent: that channel
is normal X, NOT road opacity. Only the albedo's alpha controls shoulder coverage.
Normals derive from periodic metric heights, not from tyre-wear colours.

The first original mapping is `data/silnice.paa` in `Dta/Data.pbo`, confirmed in
the `data3d/silnice25.p3d` OnSurface section. Its U=0..1 spans 9.1 m and its
V=-2..2 spans 25 m; the authored tile is consequently 9.1 by 6.25 m. Curve and
junction mappings still need their own inspection rather than name guesses.

The sand candidate maps `eden/ps.paa` from `Dta/Eden.pbo`, with a 50 m tile matching
the actual legacy landscape grid. Test centre: Everon 5800/3400, height 10.6 m.
Its optional periodic, warped ripples affect height/normals, never albedo lighting.
The gravel candidate maps `data/cesta.paa` from `Dta/Data.pbo`; inspected
`cesta25.p3d` has width 3.5 m and V=-2..2 over 25 m, giving a 3.5 by 6.25 m tile.
Test placement: 2221.7/4788.8, height 38.2 m. These are authored candidates, not a
claim that photorealism or the complete visual/roughness test matrix is accepted.
