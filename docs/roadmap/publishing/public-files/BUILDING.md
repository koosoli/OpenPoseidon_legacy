# Building Open Poseidon Engine

## Windows WGPU build

Install Git, CMake 3.25 or newer, Ninja, ccache, LLVM with clang-cl, Visual Studio
C++ build tools and a Windows 10/11 SDK. Install stable Rust supporting the
minimum version specified in the Cargo manifests. Content generation also uses
Python 3, Pillow and NumPy. Set `VCPKG_ROOT` to a vcpkg checkout; the manifest baseline
and repository overlays select the dependencies.

From the source root in PowerShell:

```powershell
$env:VCPKG_ROOT = 'C:\dev\vcpkg'
python -m pip install Pillow numpy
.\scripts\Build.ps1 -Preset win-x64-clang-rwdi -Target PoseidonGame
```

The build helper configures the toolchain, builds the game and required tools,
and stages generated default content. Outputs are under `dist/x64-win-rwdi`.
Keep the engine executable and renderer DLL from the same build together.
The default renderer configuration excludes the optional NVIDIA NGX/DLSS SDK.
Do not package local SDK libraries as part of an Open Poseidon Engine release.

For a configure-only dependency check:

```powershell
.\scripts\Build.ps1 -Preset win-x64-clang-rwdi -ConfigureOnly
```

## Testing and local game data

The CMake presets include native tests. Build the test targets before running
CTest; a successful configure alone does not establish a working game build.
Runtime tests require a compatible GPU and your own game installation.

```powershell
cmake --build --preset win-x64-clang-rwdi
ctest --test-dir build/win-x64-clang-rwdi --output-on-failure
```

The local install/start helpers are `scripts/Install.ps1` and `scripts/Start.ps1`.
Read their parameters before selecting a game directory. Preserve a backup of
your installation and close the game before replacing binaries. Retail data
located by those helpers stays on your computer; it must not be included in
a source export or release download.

Release packaging needs a clean build of the exact released source, dependency
license notices and a separately reviewed file manifest. The historical preview
packager is not a release distribution procedure.


## Fixture availability in this source release

Prebuilt binary test fixtures are deliberately omitted from this release because
their individual provenance is not fully documented. This includes archived PBOs,
P3D models, PAA/PAC images, animation, audio, font, world and savegame test files.
Their omission does not remove runtime engine features or require additional
files to play: these files are parser and regression-test inputs only.

Test source, textual fixtures and synthetic fixture generators remain included.
Tests that open omitted files require independently authored local replacements;
the complete asset-dependent test suite cannot pass on this export alone. Missing
fixtures are not counted as passing tests. Do not obtain replacements by copying
retail game data into a public source tree. See [ASSETS.md](ASSETS.md).
