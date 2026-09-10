# Default material and Jeep content packs

Implementation baseline: bfcba639 on asset-obs-001, equal to the fetched
safety/consolidated-v2 before edits. This is an implementation ledger, not a
completion report. The owner's prior outstanding work remains authorized.

## First increment: mount contract (in progress)

- Reuse ModCollection/ModSystem, AppConfig and the existing addon loader.
- Only @OP_VisualUpgrade/AddOns/op_ground_materials.pbo and
  @OP_VehicleActions/AddOns/op_jeep_actions.pbo are automatic candidates.
- Require mod.json string defaultContentApi="1", a nonempty version and the
  expected nonempty archive. Archive parsing remains the existing loader's job.
- Original data / RES precede defaults; explicit user mods follow defaults.
  A user-selected identical folder identity suppresses the automatic candidate
  with a diagnostic. Unknown folders are not auto-mounted.
- --no-visual-upgrade and --no-vehicle-actions independently suppress automatic
  discovery at startup. Explicit --mod selection is still an explicit selection.
- Validated defaults persist across ordinary in-process mod remounts. There is
  no per-frame filesystem discovery. With no packs, normal content remains.
- Unit coverage includes incompatible/missing metadata, absent/empty archive,
  independent suppression, explicit identity ownership and remount ordering.

Not delivered yet: content generators/assets, material remapping, release
packaging, real terrain/road A/B/C acceptance, Jeep animations or FFV. No FPS,
memory saving, visual upgrade or multiplayer compatibility claim follows from
the mount contract. No placeholder package will be presented as a finished pack.

## Evidence and next steps

Installed original Dta/Data.pbo contains asfaltka.paa, asfaltka_kr.paa,
cesta.paa, cesta_kr.paa, silnice.paa and silnice_kr.paa. These are candidates,
not yet selected without checking model references, UVs, edge alpha and scale.
Dta/LandText.pbo provides the distinct landscape texture corpus. No original
texture pixels have been copied into authored/distributable content.

EngineWgpu::LegacyEnhancementNormal resolves <stem>_nohq.paa and caches misses.
TextureProfileCommand has a NOHQ encoder (X in A, Y in G); its height-in-R
parallax convention must NOT be enabled accidentally for this assignment.
QIFStreamB::AutoBank chooses longest archive prefix and does not fall through
between partial banks. A replacement data.pbo containing only three textures
would therefore be unsafe. Audit the replacement mapping path before shipping.

Next: prove exact asset mapping and user-mod priority, build one authored road
material through the existing encoder and material paths, installed acceptance,
then expand to three surfaces. Jeep remains its own gameplay increment, SP first.
