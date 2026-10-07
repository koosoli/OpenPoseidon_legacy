# ReShade compatibility review — 2026-10-08

Read-only assessment requested by Oliver. No injector installed, game launched,
runtime changed, crash reproduced, or public push made by this review.

## Decision

Keep official ReShade an optional, user-installed experiment. Do not bundle it,
enable it by default, or import RTShade into the engine at this stage. The useful
initial experiment is color grading/sharpening with one lightweight effect, then
depth visualization; it does not require integrating an injector into our renderer.
Native engine effects retain access to material, lighting, fog and pre-tonemap data
that generic presentation-time post-processing cannot reliably reconstruct.

## The three links

- [crosire/reshade](https://github.com/crosire/reshade) is the project linked by
  [reshade.me](https://reshade.me/). It supports Vulkan and Direct3D 12, so WGPU is
  not in itself an incompatibility. The core is BSD-3-Clause; some files are
  additionally MIT. Retain applicable notices if importing source. Individual
  shaders and add-ons have their own licences. The website requests sharing
  presets and linking to its installer instead of redistributing binaries/shaders;
  optional external installation is the practical distribution choice.
- [ReShade-2026](https://github.com/ReShade-2026) currently exposes a `.github`
  profile repository, rather than an alternative renderer/injector source tree.
  Its affiliation with the official project was not established. Its branding is
  not evidence of official status; use the installer linked by reshade.me.
- [DHYCIX/RTShade](https://github.com/DHYCIX/RTShade) contains game-specific
  presets, principally for Red Faction Guerilla, and a REST shader-toggling add-on.
  It is not an OpenPoseidon integration or a new scene ray-tracing renderer.
  Its README lists DH_Uber_RT, iMMERSE and qUINT dependencies. Its MIT preset
  licence does not establish redistribution rights for those separate projects or
  bundled add-on binaries. Game-specific shader toggles cannot be assumed to work
  on our WGSL-generated pipelines. Do not install its REST binaries merely to try
  generic ReShade effects.

## Our renderer and likely compatibility work

Source examined at `bfc53566`; installed provenance was
`codex/whole-world-streaming-20261007 6c0075ee 2026-10-07_22:54`.

- `engine/WgpuRenderer/rust/src/lib.rs:1077` creates WGPU from environment;
  `lib.rs:1175` logs the actual selected adapter/backend. Check that log on the
  affected machine rather than assuming classic OFP Direct3D 8/9. Target the
  **64-bit OpenPoseidon.exe**, not retail ColdWarAssault.exe. Use Vulkan ReShade
  installation if the log says Vulkan; use the corresponding DX12 installation
  only when the log says Dx12.
- The optional DLSS path creates a Vulkan instance through ash/wgpu-hal
  (`dlss_device.rs:48`). Nothing inspected explicitly disables implicit Vulkan
  layers. This is not a compatibility guarantee; test DLSS disabled first to
  isolate its different device/upscaling path.
- Scene depth uses reversed-Z, floating-point/stencil where supported, and
  separate MSAA resolves (`gfx3d/mod.rs:107`, `:159`, `:4348`). Shadow maps and UI
  can have different depth conventions or extents. A generic selector may pick a
  shadow/UI buffer instead of scene depth. DLSS/render scale can additionally
  change scene depth dimensions relative to the output. Verify depth visually
  before enabling AO/GI/DoF. ReShade's depth settings include reversed-Z and
  logarithmic switches; our main scene is reversed-Z, not logarithmic. Infinite
  far projection also means its generic far-plane slider is not a literal match
  for engine view distance.
- The engine already performs GTAO, GI, fog, bloom, exposure, upscaling and
  tonemapping. Adding depth-based AO/GI after fog can darken obscured geometry or
  produce halos. RTShade itself acknowledges such fog artifacts and employs
  per-game workarounds; this does not fix an engine fog defect.

## Reported crash: evidence and bounded reproduction

No ReShade proxy DLL/config was found in the installed game folder. No ReShade
entry was found in the checked HKCU/HKLM Vulkan ImplicitLayers keys or the usual
ProgramData/Local ReShade folders. Existing unrelated overlay layers are present;
their presence alone does not demonstrate a conflict. The inspected old cwr.log
records Vulkan but contains no ReShade marker. These local observations do not
explain the other player's crash.

Request the affected person's OpenPoseidon exe/DLL version or hashes, launch
command, GPU/driver, ReShade version and source, API selected, preset/add-ons,
`ReShade.log`, engine log and crash dump/exception. Separate these cases:

1. Baseline without ReShade; verify the engine binary pair.
2. Official installer, matching API/x64, empty preset, no third-party add-ons.
3. One color-only effect; then depth visualization and one depth effect.
4. Compare MSAA off/on, render scale 100%/upscaled, DLSS off/on, and other overlays
   individually. Test window resize/fullscreen transition once basic launch works.

A crash before any effect runs points first toward loader/layer/driver or add-on
interaction. Failure only with a particular effect points toward that effect's
resource or compilation path. These are diagnostic hypotheses, not an established
root cause. Do not delete unknown DLLs or disable machine-wide Vulkan layers on
the user's behalf; first identify the installed component and use its uninstaller.

## Primary references

- [ReShade source licence](https://github.com/crosire/reshade/blob/main/LICENSE.md)
- [Official installer source: API selection, x64 and Vulkan layers](https://github.com/crosire/reshade/blob/main/setup/MainWindow.xaml.cs)
- [ReShade depth preprocessing and linearization](https://github.com/crosire/reshade-shaders/blob/slim/Shaders/ReShade.fxh)
- [RTShade README, dependencies and installation](https://github.com/DHYCIX/RTShade/blob/main/README.md)
