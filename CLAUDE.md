# CLAUDE.md

> [!IMPORTANT]
> **MANDATORY RULES FOR ALL AI AGENTS**: Before making code or shader modifications, you MUST inspect [.agents/README.md](.agents/README.md) and [.agents/AGENT_BOOTSTRAP_AND_DIAGNOSTICS.md](.agents/AGENT_BOOTSTRAP_AND_DIAGNOSTICS.md).
> Key rules:
> 0. **ESTABLISH WHICH BINARY IS INSTALLED — before you measure anything, and before you deploy anything.** This repo has several worktrees that all deploy into the *same* game folder, and `wgpu_renderer.dll` is shared by every `.exe` in it. **The ABI handshake will not save you**: it only trips when a struct layout changes, so a DLL a hundred commits out of date loads perfectly cleanly and simply lacks a hundred commits of fixes. A stale DLL is indistinguishable from a fresh rendering regression.
>    - Read `<game folder>\DEPLOYED-FROM.txt` **first** — branch, commit and time of the last deploy. It is the first file you open, not the one you reach for after a failed diagnosis.
>    - `git merge-base --is-ancestor HEAD <installed-commit>` — if it succeeds, **your build is behind what is installed** and deploying it silently removes work. `git rev-list --count HEAD..<installed-commit>` says how much.
>    - The exe and the DLL in the game folder must have **matching file times**. Mismatched-but-loadable is the failure mode, and there is no error.
>    - Deploy **only** via `scripts\Deploy.ps1`, which enforces all three and stamps the file afterwards. A hand-rolled `Copy-Item` does none of it.
>    - If a capture disagrees with what the source says, **suspect the binary before the code**.
>    - **Never report a rendering regression, and never attribute one to your own change, until you have shown the exe and the DLL came from the same commit.** Put that commit in the write-up next to the numbers.
>
>    *Why this is rule 0:* on 2026-08-22 a session deployed a renderer 152 commits behind the installed one over a matched pair, then spent hours writing up roads-glowing-at-night, texture patchwork and white trees as fresh findings and ablating its own shaders to explain them. Every measurement was worthless. `DEPLOYED-FROM.txt` was in the first directory listing that session ran, and was never opened.
> 0b. **NEVER LEAVE THE OWNER ON A STALE BUILD.** Rule 0 is about not fooling *yourself*; this one is about not stranding the *owner*. The owner plays the binary in the game folder — not your `dist\`, not your worktree, not the capture harness's copy. If you changed anything that affects what the game does, run `scripts\Deploy.ps1` and say the stamped commit in your reply. Do it before you hand the work over, not when asked.
>    - **A capture you ran is not a build the owner has.** The harness runs `dist\x64-win-rwdi\PoseidonGame.exe`; the game folder can be many commits behind it and nothing warns anyone. Every screenshot you show from `dist\` while the game folder is older is a picture of something the owner cannot reproduce.
>    - **Data is part of the build.** A fix in an exporter changes nothing until the asset it exports is regenerated and put where the game loads it. Never validate on a patched or scratch copy (`ev_SNAP.wrp`, `%TEMP%\...`) and report it as fixed — regenerate the real artefact, or say plainly and prominently that the owner still has the broken one and what they must run.
>    - **"I could not run it" is not a resting state.** If a tool or corpus seems missing, search properly before concluding it is absent — Steam libraries exist on more than one drive, and `C:\Program Files (x86)\Steam\steamapps\common` is as real as `D:\SteamLibrary`. Say where you looked.
>
>    *Why:* on 2026-08-23 a session root-caused Everon's buried buildings, fixed the exporter, validated on a patched scratch `.wrp`, pushed, and left the owner with the old exe AND the old world file. The owner flew to the coordinates, saw the same missing town, and reasonably asked whether they were on a stale version. They were, twice over.
> 0c. **BEFORE YOU WRITE ANYTHING, MAKE SURE YOUR BRANCH IS NOT ALREADY BEHIND.** Several agents and worktrees share this repository and the same game folder. The expensive failure is not losing work, it is having to MERGE it: two sides that touched the same files for different reasons, reconciled by hand, which is where a change gets dropped and nobody notices for a day.
>    - Run `scripts\Use-RepoHooks.ps1` once per clone. It sets `core.hooksPath` (repo-relative, so every worktree honours it), gives the trunk an upstream, and reports ahead/behind.
>    - Trunk is **`feat/consolidated`**, and it tracks `safety/feat/consolidated`. Anything meant to ship belongs there. The mirror carries 29 branches; almost all are history, not work in progress.
>    - `git pull --rebase safety feat/consolidated` at the START of a session, not the end. **Rebase, not merge** — a merge here buries whose change was whose.
>    - Push as soon as a change stands on its own. An hour of unpushed work is an hour in which someone else can diverge from you.
>    - The `pre-commit` hook refuses a commit onto a branch that is behind, and prints the exact command to fix it. If you find yourself reaching for `SKIP_BRANCH_GUARD=1`, that is the signal to rebase instead.
>    - **Never `git reset --hard` with staged-but-uncommitted new files.** It deletes them from the index *and* the working tree with no reflog entry to recover from. Commit first, then experiment. (Written after doing exactly this to the hook files above, minutes after creating them.)
> 1. **Use the scripts — do not hand-roll build/install/launch.** `scripts/Build.ps1`, `scripts/Install.ps1`, `scripts/Start.ps1` are the supported path (see [Build, install, run](#build-install-run) below). **`Install.ps1` must be run once per game install**: the retail game is missing files the remaster needs, and hand-copying only the two binaries leaves an install that boots but has a *dead Options menu* and missing UI art — a failure with no error message. Run it before assuming anything is a code bug.
> 1b. **Dual Binary Deployment**: if you do deploy by hand, you MUST copy **BOTH** `PoseidonGame.exe` AND `wgpu_renderer.dll` to `D:\SteamLibrary\steamapps\common\ARMA Cold War Assault\`. Copying only one binary causes an instant `Entry Point Not Found` crash on launch.
> 2. **WGSL Shader Rules**: Review [.agents/WGSL_CODING_RULES.md](.agents/WGSL_CODING_RULES.md). No 4-component swizzles on `vec2` (`.xxyy` / `.xyxy`), all variables must be explicitly declared in scope.
> 3. **Startup Diagnostics**: If the game exits instantly, ALWAYS test with `cmd /c ".\ColdWarAssault.exe --render wgpu --window --dev --log-file cwr.log"` to read the panic traceback.

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

The engine and game source for *Arma: Cold War Assault* (released 2001 as *Operation Flashpoint: Cold War Crisis*), codename **Poseidon** — the original technology lineage that later became Real Virtuality, Arma, and Enfusion. Bohemia Interactive released it as GPL-3.0-or-later (with Section 7 additional terms). The code is a ~2001 C++ engine modernized to C++20, built with CMake + Clang for Windows x64 and Linux x64.

Important separations: the **source code** here is GPL; the **name/trademarks** ("ARMA", "Operation Flashpoint") are *not* granted (a fork must be renamed); the **game data** (models, textures, sounds, missions) is *not* in this repo — it ships separately under APL-SA, and the compiled binaries need it to run (free Demo data is on Steam).

## Build, install, run

On Windows there is a supported three-step workflow. It was undocumented here until 2026-08-04,
which is how an agent ends up hand-copying binaries into a game folder and then debugging the
resulting half-installed game as if it were a renderer fault:

```powershell
.\scripts\Build.ps1                 # configure + build (default preset win-x64-clang-rwdi)
.\scripts\Install.ps1               # ONCE per game install — see below
.\scripts\Start.ps1                 # launch the built binary against the game data
```

**`Install.ps1` is not optional.** The retail *ARMA: Cold War Assault* install does not ship
several things the remaster needs — `BIN\*.hpp` (the whole modern Options/main-menu resource
set), `fonts`, `dtaExt` (equipment icons), `AddOns\cwr_logo.pbo`. It copies them from the Demo
install, skipping anything already present (`-Force` to overwrite). On a fresh retail install
that is ~64 files.

Skipping it does **not** produce an error. `Load()` on a resource class the data does not define
returns a display with zero controls, so the symptom is a menu entry that opens nothing at all,
silently. The engine now falls back to the classic Options screen when `RscOptionsShell` is
absent (`CreateOptionsDisplay`), so it degrades rather than dying — but you get the old UI, not
the intended one.

`Start.ps1 -Demo` runs against Demo data instead; `-Fullscreen`, `-LogFile <path>` and extra
passthrough args are supported.

## Build (raw CMake)

Configure + build with a CMake preset (presets live in `cmake/presets/`, defined relative to `CMakePresets.json`):

```sh
cmake --preset win-x64-clang-rwdi          # or linux-x64-clang-rwdi
cmake --build build/win-x64-clang-rwdi
```

Build directory always mirrors the preset name: `build/<preset>`. Key presets: `*-clang-dbg` (Debug), `*-clang-rwdi` (RelWithDebInfo, the default workhorse), `*-clang-rel` (Release). Sanitizer/fuzzer presets: `*-clang-san` (ASan+UBSan), `linux-x64-clang-tsan` (TSan), `*-clang-fuzz` (libFuzzer, sets `POSEIDON_BUILD_FUZZERS=ON`). There is also `linux-x64-steamrt4` for the Steam Runtime.

Requirements: a **Clang** toolchain (the toolchain files in `cmake/toolchains/` chainload it), **Ninja**, **vcpkg** (`VCPKG_ROOT` must be set; deps are in `vcpkg.json`), and **ccache** (referenced by the base preset as the compiler launcher). Distributables are staged into `dist/<arch>-<platform>-<suffix>/` on build.

## Test

Unit tests (Catch2 + ImGui Test Engine) build with the normal build and run via CTest:

```sh
ctest --test-dir build/linux-x64-clang-rwdi --output-on-failure
ctest --test-dir build/linux-x64-clang-rwdi -R "<test name>" --output-on-failure   # single test/regex
```

Test trees live under `tests/` — `unit/` (C++), `integration/` (SQF-driven, run by Trident), `smoke/` (Pester boot-log checks), `fixtures/` (binary test data: P3D/PAA/PBO/RTM). Unit suites are split by area: `PoseidonFoundationTests`, `PoseidonCoreTests`, `PoseidonTests` (engine; filter subsets with Catch2 tags like `[config]`, `[graphics]`), `PoseidonServerTests`, `PoseidonEvaluatorTests`, `PoseidonTetrisTests`.

**Integration tests** need game data and the Trident CLI (`tri`, a Rust tool in `engine/Trident/`):

```sh
cargo build -p tri   # repo-root Cargo workspace; binary lands in ./target/<profile>/
# copy .trident.env.example -> .trident.env, set OFPR_GAME_DIR (built binaries) and OFPR_DATA_DIR (Demo data)
./target/debug/tri test -j6 --retries 2 tests/integration
```

The recommended local layout puts Demo data in `packages/Demo` (the whole `packages/` tree is gitignored).

## Lint / format

Formatting and static analysis are wired as CMake custom targets (build them like `cmake --build build/<preset> --target Format`):

- `Format` / `FormatFix` — clang-format check / apply (config in `.clang-format`: 4-space indent, Allman braces, 120 cols).
- `Tidy` / `TidyFix` — clang-tidy (config in `.clang-tidy`).
- `PythonLint` / `PythonLintFix` — ruff (via `uv`) for the Blender addon, only if `uv` is present.
- `Lint` / `LintFix` — combined C++ (+ Python) convenience targets.
- `FileSize` — warns at >3000 lines, errors at >5000.

The top-level `CMakeLists.txt` sets a large block of `-Wno-*` suppressions; these are deliberate for this legacy codebase (verified ~11,600 warnings without them). Don't "fix" warnings by re-enabling them globally.

## Rust components

A single Cargo workspace at the repo root (`Cargo.toml`) holds every crate, sharing one `Cargo.lock` and `target/`. Profiles (`dbg`/`rwdi`/`rel`) and `resolver = "3"` live in the root manifest. Members:

- `engine/Trident/` — `tri`, the test runner / integration tool.
- `engine/WgpuRenderer/rust/` — `wgpu_renderer`, the Rust side of the wgpu graphics backend (a `cdylib` linked into the game via corrosion; see below).
- `mserver/` — master-server service and tooling crates (`Archive`, `CLI`, `Client`, `MasterService`). Standard `cargo fmt` / `cargo clippy` / `cargo test` / `cargo build`.

All members declare `rust-version = "1.87"` (wgpu 29's MSRV) so the workspace's MSRV-aware resolver doesn't hold shared dependencies back to older, incompatible versions.

The **wgpu backend** (`engine/WgpuRenderer/`) is a `POSEIDON_ENABLE_WGPU`-gated graphics backend: the Rust `cdylib` is built by corrosion (fetched via CMake `FetchContent`) and driven from C++ (`EngineWgpu`) across a hand-written C ABI (`include/wgpu_renderer.h`). WGPU is the default backend as of 2026-08-06; GL33 remains fully supported and is one flag away (`--render gl33`). In non-release builds the dev panel is also on by default (`--no-dev` disables it); release builds do not register the flag at all.

## Architecture

The build is a stack of static libs (`engine/`) consumed by app targets (`apps/`). Top-level `CMakeLists.txt` lists the subdirectory order; `engine/README.md` and `apps/README.md` have the authoritative target tables.

**Engine libraries:**
- **Poseidon** (`engine/Poseidon/`) — the whole engine in one lib: AI, audio, entities, world/scene/terrain, scripting, foundation/runtime, IO, graphics *interface* (not backend), UI, networking, locale. Note: `engine/Evaluator/` and `engine/Random/` source files are compiled *into* Poseidon (see `engine/Poseidon/CMakeLists.txt`), not built as separate libs.
- **PoseidonGL33** / **PoseidonOpenAL** — the concrete OpenGL 3.3 and OpenAL backends, behind the engine's graphics/audio interfaces; client apps link these, tools generally don't. Backend selection goes through `engine/Poseidon/Graphics/GraphicsEngineFactory.*`.
- **PoseidonFormats** (`engine/PoseidonFormats/`) — a C-API shared lib (DLL) exposing P3D/PAA/PBO/RTM readers; consumed by the Blender addon.

**Apps** (`apps/`): `cwr/Game` + `cwr/GameDemo` (GUI clients sharing `cwr/GameBase`), `cwr/Server` (dedicated server), and tools under `tools/` (`PoseidonTools` asset CLI, `PoseidonEvaluator` SQF CLI, `PoseidonStudio` ImGui shell, `TcPbo`→`pbo`, `TcLister`→`poseidon`). `tetris/` and `fuzzers/` are sample/harness targets.

**Foundation** (`engine/Poseidon/Foundation/`) is the base layer everything sits on — custom containers, strings (`RString`), math, memory allocators, threading, logging, module/framework glue. Most substantial targets precompile `Foundation/PoseidonPCH.hpp` as a PCH; building with `-DPOSEIDON_DISABLE_PCH=ON` is the way to audit include self-containment.

**Scripting (SQF/SQS):** the core expression evaluator is `engine/Evaluator/express.*` (the classic OFP "GameState" interpreter, with `EvalState`, `SqsRunner`, `Validate`). The engine extends it with game commands in `engine/Poseidon/Game/Commands/GameStateExt*` and `engine/Poseidon/Game/Scripting/`. `PoseidonEvaluator` is a standalone CLI around this evaluator.

**World / entities / AI:** `engine/Poseidon/World/` owns the scene, terrain, simulation, and entity model (`World`/`GameState` live here); `engine/Poseidon/AI/` holds the agent hierarchy (`AICenter` → `AIGroup` → `AISubgroup` → `AIUnit`, plus vehicle AI and pathfinding under `Path/`). Much logic is split across `*.cpp` + `*Impl*.cpp` + `*.inc` files for one class.

**Networking:** `engine/Poseidon/Network/` is large and layered — `NetTransport*` is the low-level transport/session/voice layer; `Network*`/`NetworkServer*`/`NetworkClient*` is the gameplay messaging layer; master-server browser/publisher integrate with the Rust `mserver/` service.

**IO / file formats:** `engine/Poseidon/IO/` — PBO archives (`PackFiles`), the `ParamFile` config system (`.cpp`/preprocessor in `ParamFile/`, `PreprocC/`), streams, serialization, and the threaded `FileServer`.

### Conventions worth knowing

- `__FILE__` is rewritten to repo-root-relative on GNU-driver clang (`-fmacro-prefix-map`) so log/assert lines read `engine/Poseidon/...:NN`. clang-cl is intentionally left with absolute `__FILE__` because test helpers (`RepoPath()`) walk `__FILE__`'s parents expecting an absolute path.
- A lot of code uses `memcpy`/`memset` on non-trivial types intentionally (the `ClassIsMovableZeroed` pattern) — hence `-Wno-nontrivial-memcall`. Preserve these patterns rather than rewriting to copy constructors.
- Large classes are commonly partitioned across multiple translation units (`Foo.cpp`, `FooImpl.cpp`, `FooImplHealth.cpp`) and `.inc` includes; look for sibling files before assuming a method is missing.
