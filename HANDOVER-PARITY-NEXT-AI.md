# HANDOVER: Reforger Parity & Defect Investigation

## 1. Branch & Repo Status
- **Worktree**: `.claude/worktrees/farvisible`
- **Branch**: `parity/foliage-lighting-001`
- **Remote**: `safety`
- **Latest Commit**: `c802c71c` (`fix(parity): disable foliage crown AO by default`)
- **Main repo root**: `C:\Users\mail\OneDrive\Documents\GitHub\CWR-CE-paavohuhtalafork-` (on `consolidated-v2`)

## 2. Completed in this Session
- **Crown AO disabled by default**:
  In `engine/WgpuRenderer/EngineWgpu.cpp`, `ResolveMaterialCrownAo`:
  `static const bool enabled` requires `WGR_FOLIAGE_CROWN_AO=1` to activate. Crown AO is completely OFF by default as requested by the user.
- **Root-Cause Analysis of User Defects**:
  1. **UserP02-roof (Striping on Workshop Roof)**:
     - Model: `Assets/Structures/Houses/Village/HouseAddon/HouseAddon_Workshop_E_01.xob`
     - Material: `Assets/Structures/Houses/Village/HouseAddon/Data/HouseAddon_Workshop_E_01_EXT_roof.emat`
     - Family: `MatPBRMulti`
     - Cause: Layer 2 uses `BCR_2` = `ST_House_E_1I03_det_1_BCR.edds` (a striped/detail texture) with `UVTransform_2 { TilingU 0.5 }`.
     - In `engine/WgpuRenderer/rust/src/gfx3d/gpu_driven.wgsl`: `layer_w` combines layers using `mask = textureSampleBias(textures[sm.mask_slot], samplers[sm.sampler_idx], mask_uv, frame.renscale.x).rgb`. Layer 2 is weighted by `mask.g`. The blend mixes the striped texture over the roof base tiles. Check if mask channel assignment or normalization in `gpu_driven.wgsl` corresponds to Enfusion's shader logic.
  2. **UserP03-ground, UserP04-walls, UserP11a-wall, UserP11b-wall2 (Transparent / Missing Walls and Ground)**:
     - In `engine/Poseidon/Graphics/Rendering/Shape/ShapeDraw.cpp` and `engine/WgpuRenderer/EngineWgpu.cpp`:
     - Sections with `.emat` and alpha class `Blend` are rejected by `SectionRetainedRefuses(sec)` or `SectionGpuOwned(sec)` (line 254: `if (mat && *mat && Asset::Material::IsEmatPath(mat)) return false;`).
     - At the same time, the per-draw CPU path drops them if they are classified as GPU-owned or if `.emat` blend routing refuses. Result: neither GPU nor CPU draws them!
     - Fix: Ensure `.emat` sections that are opaque or alpha-tested are owned properly or routed to the correct render pass without dropping them into a void.
  3. **UserP10a-window / UserP10b-windowback (Glass/Greenhouse)**:
     - Material family `MatPBRBasicGlass`. Must be rendered in the transparent/glass pass or blended correctly.

## 3. Critical Rules for Successor
- **DO NOT USE `read` ON IMAGES (`.png`, `.jpg`, `.bmp`)**: It quickly blows past the 50-image API limit and crashes the session! Check screenshots using external viewer or file metadata/logs.
- **ALWAYS USE `scripts/Deploy.ps1`**: Do not manually copy files. Wrap game runs and deploy with `scripts/with-game-lock.sh` or `$env:LOCK_OWNER`.
- **PRESET**: `win-x64-clang-rwdi`.

## 4. Next Tasks
1. Push branch `parity/foliage-lighting-001` to `safety`.
2. Build `win-x64-clang-rwdi` and deploy with `scripts/Deploy.ps1`.
3. Proceed with fixing `HouseAddon_Workshop_E_01_EXT_roof.emat` multi-layer blend and the transparent wall/ground section classification in `ShapeDraw.cpp` / `EngineWgpu.cpp`.
