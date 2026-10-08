# Reforger material parity: bounded first correction

Status: source and focused automated checks complete; paired installed-game
build and visual acceptance remain pending. No claim of native-renderer parity.

## Finding and change

The existing retained opaque PBR preview already reads native BCR alpha as
roughness and NMO B/A as metalness/ambient occlusion. The EMAT adapter retained
authored `RoughnessScale` and `MetalnessScale`, but the preview ignored them.
It also silently needed the independent provenance switch enabled.

`WGR_REFORGER_PBR_PREVIEW=1` now enables the metadata it needs. Eligible native
`MatPBRBasic` sections carry explicit roughness/metalness multipliers through
the C++/Rust/material-buffer ABI. Channels are multiplied before the existing
preview clamps them. Missing or nonfinite scalars use identity; authored zero
is preserved. Layered materials retain identity until their separate scalar
contract is established. Opacity and original OFP shading are unchanged.

The preview remains opt-in and still uses the existing specular-power
approximation, rather than claiming a full Reforger BRDF. The multiplication
interpretation is source-based; comparison against Reforger's rendering is
still necessary before declaring exact material parity.

The material ABI grew. Build and deploy the matching EXE and renderer DLL
together; the existing ABI layout hash rejects a mismatched pair.

## Validation

- `cargo test -p wgpu_renderer --offline --lib native_pbr -- --nocapture`:
  two tests passed, including execution of the production channel helper on
  the GPU and six direct/retained/shadow shader variants with preview on/off.
- `cargo test -p wgpu_renderer --offline --lib multi_layer_normals_keep_every_layout_in_step -- --nocapture`:
  matching C++/Rust/WGSL sizes and offsets passed.
- Modified adapter unit-test translation unit and `EngineWgpu.cpp` passed
  MSVC syntax checks against the worktree's own source and ABI header.
- A standalone C++ probe executed nine parser/scalar/default/gate checks,
  including the preview-only environment switch. The full Catch suite has
  not been linked or executed for this change.
- Fixture generation, model/world inspection, PowerShell parsing and
  `git diff --check` passed. No game was launched by the material subagent.

## Installed-game A/B

After deploying a matching build, run `scripts/Test-NativePbrBasic.ps1`
through `scripts/with-game-lock.sh`. `-PrepareOnly` generates the owned fixture
without starting the game. The normal run refuses an existing game session,
creates isolated graphics profiles, captures preview off/on at the same
camera, date, lighting and fixed exposure, records frame performance and
requires the eligible-material log for the preview capture.

Four spheres share the same synthetic geometry and constant BCR/NMO pixels.
Left to right: rough metallic, smooth metallic, rough dielectric, smooth
dielectric. The control should render all four alike; the preview should show
different highlight widths and metallic/dielectric diffuse contribution.
The generated DDS/EMAT/MLOD/world data stay under ignored `build/`; no game
assets are distributed. The script is prepared, not yet runtime-validated.

## Next bounded investigations

Check a locally installed opaque material with known nonunity scales against
its native appearance before enabling the preview by default. Then establish
constant-color/far-LOD material and `AOScale` semantics, and inspect normal
strength and texture color-space handling. Avoid broad shader rewrites or
new textures until a fixed-camera comparison isolates the responsible path.

The recommendation's proposed source-first investigation is useful. Binary
decompilation, Ghidra installation and proprietary shader extraction are not
needed for this correction and were not performed. The source patch and
fixture contain no proprietary material or texture copies.
