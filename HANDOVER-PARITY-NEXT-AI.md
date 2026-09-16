# HANDOVER: Reforger Parity & Defect Resolution

## 1. WHY THE UPSTREAM API ERROR OCCURS & HOW TO PREVENT IT
**Error message:**
`Error from provider (Console): Upstream request failed: [invalid_request_error] The request contains invalid parameters. Check the request body for any errors or inconsistencies.`

### Root Cause
1. **Accumulated Images in Conversation Context:**
   Every image uploaded in chat (like user screenshots) or read by tools stays in the message history payload sent to the LLM API on every turn. Once total images approach or exceed provider limits (usually 50 images or total request body size limits), the API rejects the request with `invalid_request_error`.
2. **Excessive Context / Payload Size:**
   Long-running sessions with multi-megabyte tool outputs produce massive JSON request bodies that trigger upstream timeouts or validation rejections.

### Strict Rules for the Next AI
1. **START IN A FRESH SESSION:**
   The user should start a new chat session to reset the message history payload to zero.
2. **NEVER use `read` or image inspection tools on image files (`.png`, `.jpg`, `.bmp`):**
   This repository's instructions strictly prohibit reading images directly.
3. **Use text CLI tools & logs for diagnosis:**
   - `dist\x64-win-rwdi\PoseidonTools.exe pak cat <pak> -e <file>`
   - `dist\x64-win-rwdi\PoseidonTools.exe image edds <file>` (prints text stats: channels, mips, dimensions)
   - `D:\SteamLibrary\steamapps\common\ARMA Cold War Assault\cwr.log` (read using PowerShell / Grep)
4. **Deploy strictly with `scripts/Deploy.ps1`** wrapped with `$env:LOCK_OWNER` and ensure the game is not running.

---

## 2. REPO & BRANCH STATE
- **Worktree:** `.claude/worktrees/farvisible`
- **Active Branch:** `parity/foliage-lighting-001`
- **Safety Remote:** `safety` (`https://github.com/koosoli/PoseidonWGPU-private-safety-mirror.git`)
- **Main repo root:** `C:\Users\mail\OneDrive\Documents\GitHub\CWR-CE-paavohuhtalafork-` (on `consolidated-v2`)
- **Game directory:** `D:\SteamLibrary\steamapps\common\ARMA Cold War Assault`
- **Installed binary stamp:** `parity/foliage-lighting-001 fefcc26f 2026-09-16_16:44 deploy-ps1`

---

## 3. WHAT WAS DONE & CONFIRMED WORKING
1. **Crown AO Disabled by Default:**
   In `engine/WgpuRenderer/EngineWgpu.cpp` (`ResolveMaterialCrownAo`), crown AO defaults to OFF unless `WGR_FOLIAGE_CROWN_AO=1`. Confirmed by user.
2. **Tree Regression Repaired:**
   Vegetation families (`MatPBRTreeCrown`, `MatPBRTreeTrunk`, `Grass`) are strictly kept on the GPU cutout path and excluded from the CPU cull-none carve-out (`IsEmatVegetationFamily`). User verified: "baum ist repariert".
3. **Solid Walls / Ground No Longer Transparent:**
   Opaque Enfusion materials (`MatPBRMulti`, `MatPBRBasic`, `MatPBR2Layers`) with roughness-in-alpha (BCR) are routed as opaque geometry rather than soft-blended.
4. **Full 238-Key `.emat` Audit Completed:**
   Committed to `docs/roadmap/decisions/REFORGER-EMAT-COVERAGE-AUDIT-20260916.md` with full frequency rankings from the 10,312 Reforger materials corpus.

---

## 4. CURRENT DEFECTS & EXACT NEXT STEPS

### Defect A: Wire Mesh Fence Drawn as Solid White Sheet (`NetFence_Wire.emat`)
- **User report:** "zaun ist nicht transparent" (screenshot attached by user shows fence panels as opaque white rectangles with grid lines).
- **Material:** `Assets/Structures/Walls/Net/Data/NetFence_Wire.emat`
  ```
  MatPBRBasic {
    Color 0.117 0.047 0.048 1      <-- Dark reddish-brown tint!
    Cull none
    AlphaTest 1.5
    AlphaMul 2.3
    BlendMode AlphaBlend
    BCRMap "Assets/_SharedData/Metal/ST_Metal_Peel_01_BCR.edds"    <-- Raw RGB is 232,228,225 (WHITE!)
    OpacityMap "Assets/_SharedData/Metal/ST_Steel_Net_01_S_mips_A.edds"  <-- Opacity map!
    GlobalMapsUVTransform { TilingU 24 TilingV 24 OffsetU -0.17 }
  }
  ```
- **Root Cause:**
  1. `ST_Metal_Peel_01_BCR` is almost pure white. In `MatPBRBasic`, the tint `Color 0.117 0.047 0.048` must multiply the albedo, otherwise the metal reads blinding white! Currently, `Color` tint from `MatPBRBasic` is not modulating the texture in the shader or DrawSectionTL!
  2. In `DdsImport.cpp`, `enfa|ST_Steel_Net_01_S_mips_A.edds|ST_Metal_Peel_01_BCR.edds` merges the opacity map into alpha. But `NetFence_Wire.emat` specifies `AlphaMul 2.3` and `AlphaTest 1.5`, or `BlendMode AlphaBlend`.
  3. In `EngineWgpu.cpp` / `ShapeDraw.cpp`, check why `NetFence_Wire.emat` ended up drawn opaque instead of alpha-tested / blended:
     In `cwr.log`: `classifier REFUSED (SectionGpuOwned false) surfMat='...NetFence_Wire.emat' face='enfa|...'`
     `Stage0 NOT overriding an enfa| composite face texture rvmat=...NetFence_Wire.emat stage0=Assets/_SharedData/Metal/ST_Metal_Peel_01_BCR.edds`.
     Because Stage0 did NOT override, the CPU path drew it using the raw face texture or un-tested pass!
- **Fix:**
  - In `EngineWgpu.cpp`, ensure `Stage0` overrides correctly or that the `enfa|` composite with `NetFence_Wire.emat` applies its alpha test (`AlphaMode::Test`) and `Color` diffuse tint.

### Defect B: Rock Texture at `[4787.26, 3860.66, 21.28]`
- **Material:** `GraniteStone_01.emat` (`MatPBR2Layers`) or `GraniteCliff_02.emat` (`MatPBRMulti`).
- **Status:** Commit `9468031f` wired `BCRMap2`/`NMOMap2`/`Color2` for `MatPBR2Layers`.
- **Next Step:** Check `cwr.log` for the exact shape name at `[4787, 3860]` and verify which material/texture is active and if its mask is sampling uv0 vs uv1.

### Defect C: Workshop Roof Striping (`UserP02-roof` at `[5096.83, 4013.61, 27.39]`)
- **Material:** `HouseAddon_Workshop_E_01_EXT_roof.emat` (`MatPBRMulti`).
- **Cause:** `BCR_2` is `ST_House_E_1I03_det_1_BCR.edds` with `UVTransform_2 { TilingU 0.5 }`. This texture has severe horizontal stripes (`rowVar 1842` vs `colVar 4.5`).
- **Fix:** In `gpu_driven.wgsl`, check how `layer_w` combines `mask.r, mask.g, mask.b`. The mask channel weights must properly attenuate the detail stripe layer.

### Defect D: Window Glass (`UserP10a/b` at `[5211.17, 4056.84]`)
- **Material:** `window_glass_atlas.emat` (`MatPBRBasicGlass`).
- User reported glass is too subtle from outside and brown from inside.
- Verify two-sided culling (`Cull none`) and ensure reflection/specular is active on glass.

---

## 5. BUILD & VERIFICATION COMMANDS
```powershell
# In worktree .claude/worktrees/farvisible:
.\scripts\Build.ps1

# Deploy to game:
$env:LOCK_OWNER = "deploy parity fix"
.\scripts\Deploy.ps1 -GameDir "D:\SteamLibrary\steamapps\common\ARMA Cold War Assault"
```
