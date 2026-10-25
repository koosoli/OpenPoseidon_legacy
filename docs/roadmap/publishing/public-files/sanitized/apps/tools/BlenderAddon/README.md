# Poseidon P3D Tools for Blender

Blender add-on for Arma: Cold War Assault - Remastered assets, both ways:

* **Models:** import P3D (MLOD source and ODOL v7 binarized) with textures, selections and proxies; **export MLOD**
  through the engine's own writer, with textures as PAA.
* **Animations:** an editable rig of the MC soldier; import RTM animations as actions, edit them, **export RTM**,
  with joint limits and a collision check.


## Quick Install

### Option A: Install from ZIP

1. Build the DLL (if not already):
   ```
   cmake --build build\win-x64-clang-rwdi --target PoseidonFormats_shared
   ```

2. Package the addon:
   ```
   powershell -File apps\tools\BlenderAddon\package.ps1
   ```

3. In Blender: **Edit > Preferences > Add-ons > Install from Disk** > select `dist\io_import_p3d.zip`

4. Enable the addon: search for **"Poseidon P3D"** and tick the checkbox

### Option B: Manual / Development Install

1. Copy (or symlink) the `apps\tools\BlenderAddon\io_import_p3d` folder into your Blender addons directory:
   ```
   %APPDATA%\Blender Foundation\Blender\5.0\scripts\addons\io_import_p3d
   ```

2. Copy `dist\x64-win-rwdi\PoseidonFormats.dll` into that same `io_import_p3d` folder

3. Enable in Blender: **Edit > Preferences > Add-ons** > search **"Poseidon P3D"**

### Symlink for Development

To avoid copying files after each change, create a symlink (run as Administrator):
```
mklink /D "%APPDATA%\Blender Foundation\Blender\5.0\scripts\addons\io_import_p3d" "C:\src\OpenPoseidon\apps\tools\BlenderAddon\io_import_p3d"
```
Then just copy the DLL once into the addon folder:
```
copy dist\x64-win-rwdi\PoseidonFormats.dll apps\tools\BlenderAddon\io_import_p3d\
```

## Usage

1. **File > Import > Poseidon P3D (.p3d)**
2. Browse to a `.p3d` file (from game data or unpacked PBOs)
3. Set import options in the sidebar panel:

| Option | Default | Description |
|--------|---------|-------------|
| LODs | First Only | All / First Only / Visual Only (skip shadow/geo LODs) |
| Max LOD Resolution | 100000 | Skip LODs above this distance threshold |
| Materials | On | Create materials with PAA textures |
| Normals | On | Import custom split normals |
| Named Selections | On | Import as vertex groups |
| Proxies | On | Import as empty objects |
| Properties | On | Import as custom object properties |
| Texture Directory | (empty) | Extra folder to search for PAA/PAC textures |

## Export a model

**File > Export > Poseidon P3D (.p3d)** writes the selected mesh objects as one MLOD model, through the engine's
MLODWriter (`pf_mlod_*` in PoseidonFormats.dll). Each object is a LOD:

| Blender | P3D |
|--------|-----|
| object custom property `cwr_lod_resolution` | LOD resolution (else 1, 2, ... in name order) |
| object origin, rotation and scale | the model's origin and axes (Blender Z up is the model's Y up) |
| faces (n-gons split), the active UV map | triangles and quads, per-corner UVs |
| material `cwr_texture`, else its image | the face's texture path (`Texture Prefix` + image name); **Write Textures as PAA** writes the image (power-of-two sizes) |
| vertex groups | named selections (weights as O2 stores them) |
| custom properties `cwr_<name>` | named properties |
| face attribute `cwr_face_flags` | the game's face flags |
| child objects with `cwr_proxy_path` | proxies (marker triangle + `proxy:` selection, and the selections their corners were in) |

What the importer stores (`cwr_center_offset`, `cwr_bounding_center`) is added back, so an imported model is written
where it came from; `mc vojakw2.p3d` round-trips with every LOD, selection, proxy and texture intact. The
PoseidonFormats.dll must be new enough to have the writers (else export reports it).

## Soldier rig and animations

Sidebar > **Poseidon > Soldier Rig**:

* **Build Soldier Rig**: the MC skeleton as an armature (`MC_Rig`), skinned and textured from the mounted game data
  (or File > Import > Poseidon Soldier Rig from a `.p3d`).
* **Import RTM** (or Animations > *Load onto Soldier Rig*): an RTM as an editable action, one key per phase.
* **Limits On/Off**: joint limits from the stock animations' full range (knees and elbows bend one way; overhead
  arms and pointed toes allowed). Importing a game move switches them off.
* **Check Motion**: frames where a limb passes into another, the hands swap sides, or a joint breaks its limit.
* **Export RTM**: the action sampled into an RTM (phases, step per cycle); checked first unless *Check First* is off.

Import and export round-trip the game's moves to 1e-6. Bone axes for posing by hand: local X flexes (spine forward,
limbs forward, forearm up, toes up; a knee bends with -X); upper arms and thighs rotate Y, Z (out to the side), X.

Sidebar > **Poseidon > New Moves from Clips** (File > Import > Humanoid Clips for the Soldier):

* **Import Clips (.glb)**: a humanoid clip library with UE-mannequin bone names (Quaternius' Universal Animation
  Library, ActionForge); every clip becomes an action. Licences: CC0 and CMU mocap yes, Mixamo no.
* **Retarget onto Soldier**: a clip as a soldier action (each bone along the clip's, root travel dropped, joints in their
  limits); optionally the arms and rifle follow a reference move, for unarmed clips under a rifle.
* **Add to Comparison**: the move in the game and a candidate (or a new move alone), each on its own soldier at its
  in-game cycle time, labelled CURRENT / CANDIDATE / NEW; Space plays them.

## What Gets Imported

- **Mesh**: Vertices, faces, UVs (V-flipped), normals, coordinate-converted (Y-up to Z-up)
- **Materials**: One per unique texture, with Principled BSDF node and PAA texture if found
- **Vertex Groups**: From named selections (e.g., "velka vrtule", "pravy volant") with weights
- **Empties**: From proxy attachment points with correct transforms
- **Custom Properties**: `ofp_lod_index`, `ofp_lod_resolution`, `ofp_format`, model properties
- **Collections**: Each LOD in its own sub-collection (when importing multiple LODs)

## Texture Resolution

The addon searches for PAA/PAC textures in this order:
1. Same directory as the P3D file
2. Game root directory (auto-detected by looking for `dta/` or `Addons/` folders)
3. Custom texture directory (if specified in import options)

For best results, point the Texture Directory to your game installation root.

## Troubleshooting

**"PoseidonFormats.dll not found"**: The DLL must be in the addon folder or set `POSEIDON_FORMATS_DLL` env var.

**No textures loaded**: Set the Texture Directory to your game root. Textures use relative paths like `data\uh60_infra_side.pac`.

**Blender crashes on import**: Check Blender's System Console (Window > Toggle System Console) for error messages.

## Architecture

```
PoseidonFormats.dll    C++ engine (P3D/PAA/PBO readers)
       |
   ctypes (C API)
       |
   p3d_lib.py          Python wrapper (P3DModel, PAAImage classes)
       |
   import_p3d.py       Blender import operator
   export_p3d.py       MLOD export (the engine's MLODWriter through the DLL)
   rig.py / rig_ops.py the MC soldier rig, RTM import/export, joint limits, collision check
   retarget.py         outside clips (UE-mannequin names) onto the soldier rig
   compare.py          moves side by side for choosing; anim_ops.py their operators and panel
   rtm_io.py           RTM read/write (plain Python, also used by scripts/swim)
       |
   __init__.py          Addon registration
```
