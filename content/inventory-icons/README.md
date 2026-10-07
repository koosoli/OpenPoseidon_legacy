# Inventory icons

The default inventory icon family uses **original AI-generated realistic product
renders** for Open Poseidon Engine. The PNGs are the editable source images and
have transparent backgrounds; the accompanying PAA files are the prepared runtime
textures. Both live in `assets/inventory/`.

The images were generated from written equipment descriptions. No retail-game
texture, donor icon, screenshot, logo or background-removal result was supplied as
an image input. They depict generic equipment categories, without game branding.
The active `manifest.json` and prompt records document each category's generation
prompt, source file and prepared output. Image hashes identify the exact files
that are shipped; changing a prompt is a new generation, not deterministic raster
regeneration of the existing art.

Project-authored icon files are supplied under **GPL-3.0-or-later**, with the
project's Section 7 terms in the root `LICENSE`. This provenance statement applies
to the new icon family; it does not change the licence of third-party game data.

PAA conversion is an explicit asset-authoring step using the project's
PoseidonTools converter. Normal builds and installation copy the prepared PAA
files. No AI service, image processing, external download or optional donor icon
pack is needed to run the game. Stock or addon icons remain fallbacks for items
without a category mapping; a labelled tile handles unavailable icon textures.

After building PoseidonTools on Windows, run `scripts/Prepare-InventoryIcons.ps1`
to prepare the runtime PAAs. It preserves the source PNGs, crops only transparent
margins, scales into power-of-two canvases no larger than 512 pixels and encodes
DXT5 alpha textures. Runtime images total about 4.3 MiB; source PNGs are not needed
in the installed game. `scripts/Build-InventoryIcons.ps1` stages those versioned
PAAs without regeneration.

The earlier vector experiment is retained in `vector-draft/` as **inactive draft
art**. It is not used by the default inventory. Its original SVGs, preview and
manifest document that experiment separately from the active realistic images.
The old generator is now deliberately confined to that draft directory, so running
it cannot overwrite `assets/inventory/`:

```powershell
python scripts/default-content/generate_inventory_icons.py
python scripts/default-content/generate_inventory_icons.py --check
```

These commands regenerate/check only the inactive SVG and PNG vector draft, using
Pillow. They do not regenerate the active realistic icons.

Images: m16 (dedicated M16A2), rifle, ak_rifle, scoped_rifle, smg, machinegun, launcher, pistol,
binoculars, nvg, magazine, curved_magazine, grenade, smoke, satchel, mine, rocket.

M16 now has its own identifiable image instead of the generic rifle category.
Other mapped weapons still use category illustrations; this is not a claim of
individual portraits for every classic or addon weapon. The classic Gear screen
is unchanged by this inventory icon family.
