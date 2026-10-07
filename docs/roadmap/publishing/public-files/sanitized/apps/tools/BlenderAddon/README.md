# Poseidon P3D Importer for Blender

Blender addon for importing Arma: Cold War Assault - Remastered P3D models.
Supports both MLOD (source) and ODOL v7 (binarized) formats with textures, selections, and proxies.

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
       |
   __init__.py          Addon registration
```
