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

## First road integration, awaiting visual acceptance

Mount-contract commit 8388cca3 is pushed to both safety refs and installed. Its
missing-pack smoke is build/modernisation-cpu/cpu-20260910-192132-Field: exit 0,
67.2 s wall, screenshot inspected. Eleven targeted cases / 85 assertions pass.
The early diagnostics appeared only in stdout, so the follow-up stores them in
AppConfig and emits them after GameBase initializes the normal log.

The existing tool now inspects UVs on ODOL7 via the existing engine reader.
silnice25.p3d: 9.1 m across U=0..1, 25 m along V=-2..2, so a texture repeat is
9.1 by 6.25 m. LOD0 uses data\\silnice.paa with OnSurface/IsAlpha/ClampU flags.
Read-only reference extracts remain under build/default-content, not in the pack.

Own periodic source generator: scripts/default-content/generate_materials.py,
editable parameters: content/default-packs/visual/materials.json. Five Python
tests pass (determinism, dimensions, flat AG layout, periodic derivative/sign,
colour-only road wear). 1024-square source PNGs and linear height arrays generated.
R=0 in NOHQ prevents implicit parallax. The gravel/soil source recipes have NOT
yet been mapped to actual terrain assets or visually accepted.

Build-DefaultContent.ps1 encodes the road via the existing DXT5/PAA encoder and
packages it under op_ground_materials. The catalog maps an EXACT source texture
and original archive, not a name heuristic or partial data.pbo replacement.
TextureWgpu retains the original Name() while SourceName() selects image data,
including alpha scans, prepared-texture lookup and automatic legacy Satmap input.
Legacy Satmaps are built afresh in memory, not a persistent old-look disk cache.
Normal binding uses the existing direct/retained LegacyEnhancementNormal hook.

Still not accepted: roughness binding, terrain normal slots, three surfaces,
crossroads/curves/alpha edges, user-mod precedence at runtime, reload/eviction,
release build integration, all visual/performance A/B/C arms, Jeep package/FFV.
The current road is a development integration, NOT the complete default upgrade.
Measure-DefaultMaterial.ps1 fixes the selected original Everon road position and
graphics settings. POSEIDON_VISUAL_UPGRADE_NORMALS=0 is the albedo-only arm;
--no-visual-upgrade is the original arm. No FPS or resource improvement claimed.

First installed ABC on 684d02bc: Original 201148, Albedo 201321, Full 201520
under build/default-content/acceptance/material-20260910-*. All exit 0 and
screenshots inspected; same 52507 registered / 15992 main instances and 886666
main triangles. Logs prove automatic package discovery and the road replacement.
These runs do NOT accept normal mapping: the Full log had no road normal binding.
Root cause is LegacyEnhancementNormal's QIFStream::FileExists probe, which checks
loose disk files only, not PBO members. The follow-up checks the already mounted
bank member as well. Repeat B/C on the corrected installed binary before claiming
the normal effect. Single-run frame figures are not a performance acceptance.

Deployment first failed because Get-FileHash was unavailable in the lock wrapper's
PowerShell environment, after binary copying and before the final stamp. No run
was measured in that state. 684d02bc replaces the cmdlet with streaming .NET SHA256;
the full repeated deployment succeeded and restamped before all three runs.

Corrected installed 24ac4dfe: Full 202639 and Albedo 202901 both exit 0, with
the same instance/triangle counts above. Full now logs the PBO road NOHQ binding.
The road ROI (x400:850, y470:700) has mean absolute RGB difference 0.580/255,
maximum 4; the grass control (x50:170, y290:360) differs by 0.000159/255.
This proves a subtle local normal effect, not performance or complete visual
acceptance. No equivalent repeat of Original on this commit has been made yet.

The supported scripts/Build.ps1 Windows clang client/default build now builds
PoseidonTools and calls Build-DefaultContent.ps1 automatically. A full supported
PoseidonGame build with --parallel 2 passed and staged the 2 MB road archive.
Targeted unit builds do not regenerate assets. Python/numpy/Pillow are build-time
dependencies only. Fresh-release startup and the other two surfaces remain open;
no empty Jeep package is produced to disguise the missing FFV implementation.

## Legacy landscape normal integration candidate

Shared TextureBankWgpu::LoadLegacyEnhancementNormal now provides the same PBO,
sibling and albedo-only rules to direct/retained objects and legacy terrain.
TerrainWgpu binds the existing tile_normal slot on legacy descriptors only;
the shader decodes this path as AG NOHQ, not the authored RGB tile-normal path.
The normal is faded to flat with the Satmap and not sampled past its far exit.
Cell clamp flags were previously lost between index lookup and material sampling;
sample_authored_cell now passes them into the legacy colour and normal sampling.
Modern authored and Enfusion material selection is not replaced.

First real landscape candidate: exact eden\\ps.paa in Dta/Eden.pbo, palette index
2 of installed Worlds/eden.wrp. A read-only RVW4 export shows a 3x3 sand area at
5800/3400, about 10.6 m high. LandSave::LoadOptimized sets 50 m cells. The own sand
recipe therefore uses a 50 m physical period, not the generic 4 m gravel recipe.
Pack 0.1.1 adds this candidate, with no copied source-game pixels. Terrain normals
and clamp propagation have a Rust contract test; 5 generator tests pass and the
client build passes. Installed Sand Original/Albedo/Full acceptance is next.
These facts do not establish photorealism, roughness integration or all surfaces.

Installed a25309aa Sand runs: 212632 Original, 212805 Full, 213055 Albedo. All exit
0 with 52507 registered / 15208 main instances and 730061 main triangles. Log
confirms eden/ps replacement and PBO terrain normal binding. Images inspected:
the candidate removes much broad blotching but is still too uniform to declare
photorealistic. Sand ROI x250:1100/y350:700 Full-vs-Albedo absolute RGB mean 1.458,
max 10; distant hill control x100:900/y30:95 mean 0.162. This is local normal-effect
evidence, not identical sky/vegetation or FPS proof. Three single captures do not
establish a tail-latency or memory improvement.

Next candidate 0.1.2 adds an own periodic sand ripple height field (albedo unchanged,
6 generator tests pass) and the verified data/cesta.paa road. cesta25.p3d has 3.5 m
width, V=-2..2 over 25 m, OnSurface/IsAlpha/ClampU; the recipe uses 3.5/6.25 m and
two wheel tracks. Original placement 7489: 2221.7/4788.8, Y38.2. Packaging includes
three mapped surfaces, but that count is NOT three-surface acceptance. Roughness
still needs real binding: the terrain currently has diffuse lighting only and a
source roughness PNG alone cannot change it. No specular benefit is claimed.
