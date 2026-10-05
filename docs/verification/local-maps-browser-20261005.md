# Local installed map browser, 2026-10-05

Source-only implementation; parent integration session owns full build, deployment and runtime checks.

Normal main menu: **Local maps**. The browser searches C:/SteamLibrary, D:/SteamLibrary, the normal Steam installation and libraryfolders.vdf, with an optional extra folder. Discovery runs in a worker and reads archive directories plus eight-byte WRP format headers; no game assets are copied or downloaded.

Arma 1, Arma 2/OA, Arma 3 and DayZ candidates use existing OPRW 18/20/24/25/29 readers. Unsupported/compressed or unrecognised headers remain disabled. Arma Reforger Eden, Arland and Cain use the existing native PAK loader; ambiguous editor/showcase folders remain disabled. These are engine terrain previews, not imports of original missions or all original gameplay features.

**Open selected** creates a small local cache mission, restarts the same executable, and waits for the old process to finish before initializing the game. Arguments are passed as an array through SDL, never through a command shell. External PBO roots use --map-archive-root, which mounts assets without parsing foreign gameplay/configuration classes. Existing CWA classes remain the preview's gameplay source. A native Reforger selection uses --reforger-world. The browser is accessible without --dev. Harness port is preserved when explicitly configured.

The **Vehicles** list activates only existing locally installed CWA bridge packs validated by LocalVehiclesCatalog, and opens their small controllable test mission. It does not enable arbitrary foreign CfgVehicles or claim universal vehicle compatibility. The prototype has inherited OFP physics and limited original animation support.

Validation: all modified translation units passed clang syntax checks with the configured game compiler flags. Device-free catalog tests passed 50 assertions in 5 cases, covering Steam VDF layouts, terrain revision guards, PBO prefix resolution, malformed archives, complete/incomplete vehicle dependencies and bridge mission generation. Actual installed metadata scan found six Arma 3 WRP maps, two DayZ maps and Reforger terrain entries with zero archive errors; directory scan took approximately seven seconds, outside the render thread. UI and restart still require parent installed-game smoke tests.
