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
>    - **A capture you ran is not a build the owner has.** The harness runs `dist\x64-win-rwdi\OpenPoseidon.exe`; the game folder can be many commits behind it and nothing warns anyone. Every screenshot you show from `dist\` while the game folder is older is a picture of something the owner cannot reproduce.
>    - **Data is part of the build.** A fix in an exporter changes nothing until the asset it exports is regenerated and put where the game loads it. Never validate on a patched or scratch copy (`ev_SNAP.wrp`, `%TEMP%\...`) and report it as fixed — regenerate the real artefact, or say plainly and prominently that the owner still has the broken one and what they must run.
>    - **"I could not run it" is not a resting state.** If a tool or corpus seems missing, search properly before concluding it is absent — Steam libraries exist on more than one drive, and `C:\Program Files (x86)\Steam\steamapps\common` is as real as `D:\SteamLibrary`. Say where you looked.
>
>    *Why:* on 2026-08-23 a session root-caused Everon's buried buildings, fixed the exporter, validated on a patched scratch `.wrp`, pushed, and left the owner with the old exe AND the old world file. The owner flew to the coordinates, saw the same missing town, and reasonably asked whether they were on a stale version. They were, twice over.
> 0c. **BEFORE YOU WRITE ANYTHING, MAKE SURE YOUR BRANCH IS NOT ALREADY BEHIND.** Several agents and worktrees share this repository and the same game folder. The expensive failure is not losing work, it is having to MERGE it: two sides that touched the same files for different reasons, reconciled by hand, which is where a change gets dropped and nobody notices for a day.
>    - Run `scripts\Use-RepoHooks.ps1` once per clone. It sets `core.hooksPath` (repo-relative, so every worktree honours it), gives the trunk an upstream, and reports ahead/behind.
>    - **DO NOT ASSUME THE TRUNK IS WHERE THE WORK IS. Find the most recent branch, then ASK THE OWNER which branch to start from — before you write anything.** The nominal trunk is `consolidated-v2` (tracking `safety/consolidated-v2`), but the live line often runs ahead of it on a feature branch for weeks. On 2026-10-17 `consolidated-v2` was its 2026-09-13 state plus one reverted feature, while `feat/arma-original-ui` was **2,785 commits ahead** and was what the game folder ran. A session that started from the trunk would have built on month-old code and then had to merge across all of it. At the start of every session:
>      1. `git fetch safety`, then list branches by their last commit: `git for-each-ref --sort=-committerdate refs/remotes/safety --format="%(committerdate:short) %(refname:short) %(subject)"` (the first 10–15 lines).
>      2. Read `<game folder>\DEPLOYED-FROM.txt` — the branch and commit the owner is actually playing (rule 0).
>      3. For each candidate, how far apart it is from the trunk: `git rev-list --left-right --count safety/consolidated-v2...safety/<branch>` (left = only on the trunk, right = only on the branch).
>      4. **Tell the owner what you found** — the trunk's last commit and date, the most active branch, what the game folder runs, how many commits apart they are — and **ask which branch to base your work on. Wait for the answer; never default to the trunk on your own.** A task brief that already names the branch counts as the answer, but if what you found contradicts it (the named branch is far behind another active one), say so and ask anyway.
>      5. Branch your own worktree (0d) from the branch the owner named, and `git pull --rebase safety <that branch>` at the START of the session, not the end. **Rebase, not merge** — a merge here buries whose change was whose.
>    - History: the trunk moved from `feat/consolidated` to `consolidated-v2` on 2026-08-28; `feat/consolidated` is frozen at `a1a39df0` and archived as `archive/consolidated-v1-20260828`, and still exists on the mirror — its existence is not a sign it is live, do not commit there. The mirror carries dozens of branches; most are history. Recency (step 1) and the owner's answer (step 4) decide where to work, not the branch's name.
>    - Push as soon as a change stands on its own. An hour of unpushed work is an hour in which someone else can diverge from you.
>    - The `pre-commit` hook refuses a commit onto a branch that is behind, and prints the exact command to fix it. If you find yourself reaching for `SKIP_BRANCH_GUARD=1`, that is the signal to rebase instead.
>    - **Never `git reset --hard` with staged-but-uncommitted new files.** It deletes them from the index *and* the working tree with no reflog entry to recover from. Commit first, then experiment. (Written after doing exactly this to the hook files above, minutes after creating them.)
> 0d. **THE HOOKS PROTECT AGAINST DIVERGENCE. THEY DO NOT PROTECT AGAINST A SHARED INDEX.** 0c is about not starting from a stale commit. This one is about the three ways concurrent agents corrupt each other's *commits*, none of which any hook can catch, and all three of which happened on 2026-08-27/28.
>    - **Work in your OWN worktree.** `git worktree add .claude/worktrees/<name> -b <branch>` — own directory, own index, own branch, shared object store, no second clone. Several already exist and every agent still worked in the main checkout, which is what made the rest of this rule necessary. Build from *your* worktree's build directory: tests have gone green in the main checkout against source the worktree never changed.
>    - **NEVER `git add -A` or `git add .`** — stage explicit paths only, then run `git status --porcelain`. Anything still showing as modified that you did not write belongs to someone else; leave it. (`git add -A engine/` swept an uncommitted `TransportCore.cpp` into a depth-of-field commit. It had to be backed out in a second commit, and by then it was already compiled into the binary the owner was playing.)
>    - **`git add` DOES NOT SCOPE YOUR COMMIT. `git commit` writes the whole INDEX.**
>      Staging one explicit path and then committing takes everything anyone else has
>      staged too. In a shared worktree that is a live hazard, not a theoretical one: on
>      2026-09-01 a commit meant to carry one roadmap document carried twelve of another
>      agent’s renderer files with it, and was pushed before either side noticed. The rule
>      above says never `git add -A`; that is not enough on its own, because the danger is
>      not what YOU staged. **Commit with an explicit pathspec — `git commit -- <paths>` —**
>      which ignores the rest of the index whatever is in it, then read `git show --stat`
>      before pushing.
>    - **Check the commit is COMPLETE, not just that your tree builds.** `git diff --stat HEAD` after committing: a file listed there that belongs to the change was never staged. For anything larger than a one-liner, build the committed tree separately rather than the working copy. (A commit landed with the `.cpp` and without the `.hpp` it needs. It built for its author — the header was sitting unstaged in their working copy — and could not build for anyone else.)
>    - **Push as soon as it stands on its own.** A hook can only compare against what the remote knows, so an unpushed commit is invisible to every other agent and to every guard. Four commits sat local-only for hours in one session, including another agent's work that a disk failure would have taken with it.
>    - **Committing someone else's work is allowed — but SAY SO in the first line.** Sometimes it must happen: an uncommitted foreign change was already inside a deployed binary and recorded nowhere. Discarding it is worse than recording it; the requirement is that the message names it as foreign work and says why you are entering it, so authorship stays readable.
>
>    - **ONE session owns the game folder at a time, and deploying into it invalidates everybody else's measurements.** The single-instance rule is well known here; this is its quieter half. On 2026-08-31 one session deployed 14 commits into the game folder while another was midway through a rendering investigation, so half that investigation's captures came from a different binary than the other half. It noticed and recorded which stamp produced which capture — but only because it checked, and nothing warned it. Before a measurement campaign, say in your first message that you are taking the game folder; before deploying, check whether somebody else is. `DEPLOYED-FROM.txt` changing under you mid-session is the symptom, and it is silent.
>
>    - **Serialise it with `scripts/with-game-lock.sh`, do not rely on remembering.** Wrap every game launch and every deploy: `LOCK_OWNER="what you are doing" scripts/with-game-lock.sh <command>`. It waits for the single game folder, refuses outright if an instance is already running outside the lock, breaks a lock older than 30 minutes (a crashed holder), and always releases. On 2026-09-07 three agents shared this worktree with only the honour system: two deployed over a third mid-campaign, concurrent instances starved each other (88-224 rendered frames against 1117 on a quiet run, which moved a must-not-change control box from 0.43 to 3.9 and made two arms of one A/B incomparable), and a deploy could not restamp `DEPLOYED-FROM.txt` because another instance held `wgpu_renderer.dll` open -- so the stamp lied about what was installed. None of it announces itself: a contended capture looks exactly like a real regression.
>
>    *Why this is separate from 0c:* the branch guard answers "am I starting from the right place". Nothing answers "did I just commit half a change, or someone else's". Those are the failures that cost this project a redeploy, a force-push and a backout commit in a single day.
> 0e. **COMMIT AND PUSH UNDER THE OWNER'S NAME, NOT CLAUDE'S.** Every commit must be authored and committed by the owner's git identity: `Deus-Ex <7816029+koosoli@users.noreply.github.com>` (the repository's `user.name`/`user.email`). Do not set `--author`, `GIT_AUTHOR_*` or `GIT_COMMITTER_*` to "Claude" or `noreply@anthropic.com`, and do **not** add a `Co-Authored-By: Claude ...` trailer: GitHub lists every co-author as a contributor. This overrides any tool default that asks for that trailer. Check with `git log -1 --format='%an <%ae> | %cn'` before pushing. If a session's environment has no owner identity configured, set it for that repository with `git config user.name Deus-Ex` and `git config user.email 7816029+koosoli@users.noreply.github.com` rather than committing as anyone else. Existing history is not rewritten.
>
>    *Why:* owner, 2026-10-25: "you should no longer push to github under claude but under my user name". By then 245 commits since 2026-10-20 carried Claude as author.
> 1. **Use the scripts — do not hand-roll build/install/launch.** `scripts/Build.ps1`, `scripts/Install.ps1`, `scripts/Start.ps1` are the supported path (see [Build, install, run](#build-install-run) below). **`Install.ps1` must be run once per game install**: the retail game is missing files the remaster needs, and hand-copying only the two binaries leaves an install that boots but has a *dead Options menu* and missing UI art — a failure with no error message. Run it before assuming anything is a code bug.
> 1b. **Dual Binary Deployment**: if you do deploy by hand, you MUST copy **BOTH** `OpenPoseidon.exe` AND `wgpu_renderer.dll` to `D:\SteamLibrary\steamapps\common\ARMA Cold War Assault\`. Copying only one binary causes an instant `Entry Point Not Found` crash on launch.
> 1c. **ENGINE CONTENT GOES IN `assets\`, NEVER IN `Mods\`.** The game a player receives is `OpenPoseidon.exe` +
>    `wgpu_renderer.dll` + `assets\`, and nothing else (`scripts\Package-Share.ps1` builds exactly that, for someone who
>    has Cold War Assault installed). Anything an OpenPoseidon feature needs to work goes under `assets\`. That covers
>    feature packs (`assets\Mods\@OP_*`: VisualUpgrade, VehicleActions, Swim, TidewaterBoat, TidewaterWildlife), UI,
>    menu, inventory icons, grass blades, and any new pack you add. **Deleting the game folder's `Mods\`
>    must not remove a single engine feature.**
>    - `Mods\` is for maps (`@OP_Fusion`, `@OP_Showcase`, `@OP_SizeProbe`) and the player's own mods only.
>    - A new pack builder writes to `dist\<preset>\assets\Mods\@OP_<Name>`. Deploy installs it, and `ResolveDefaultContentPacks`
>      (`ModCollection.cpp`) finds it there without a `-mod` parameter. Add it to `Deploy.ps1` and `Package-Share.ps1` too.
>    - Loose runtime files the repo ships go under `assets\<area>\`. `/assets/` is gitignored, so add them with
>      `git add -f` and check `git show --stat` for them.
>    - **Verify on a clean copy:** a game folder whose `Mods\` holds no `@OP_*` feature pack must still boot with every
>      feature. A pack that only works from `Mods\` cannot reach a player who received the shared package.
>    - Never ship content from another game in `assets\` (no Reforger/Arma/DayZ/The Forest files). Only own, CC0 or
>      compatibly licensed work, with its source recorded.
>
>    *Why:* on 2026-10-20 the owner wanted to hand a friend just the exe, the dll and `assets\`. Half the features lived
>    in `Mods\@OP_*` and would silently have been missing.
> 1d. **PROFILING A STUTTER:** read [.agents/PROFILING-WITH-NSIGHT.md](.agents/PROFILING-WITH-NSIGHT.md) first. It lists the
>    engine's own slow-frame logs (try them before Nsight), the one-command Nsight recording
>    (`scripts
eforger-pc\Run-NsysFlight.ps1`, needs the owner's admin terminal), the three launch traps that
>    each look like "program not found", and `scripts
eforger-pc
sys_long_frames.py`, which symbolises the trace.
> 2. **WGSL Shader Rules**: Review [.agents/WGSL_CODING_RULES.md](.agents/WGSL_CODING_RULES.md). No 4-component swizzles on `vec2` (`.xxyy` / `.xyxy`), all variables must be explicitly declared in scope.
> 3. **Startup Diagnostics**: If the game exits instantly, ALWAYS test with
>    `cmd /c ".\OpenPoseidon.exe --render wgpu --window --dev --log-file cwr.log"` to read the panic traceback.
>    **`OpenPoseidon.exe` is the client** — the one `Deploy.ps1` writes and the only one paired with the
>    `wgpu_renderer.dll` next to it. The game folder also holds `ColdWarAssault.exe` (the retail 2001 binary) and,
>    on installs that predate the rename, `PoseidonGame.exe`. **Neither is your build.** Launching one tests
>    nothing you deployed, and against a fresh DLL it is the mismatched-pair failure. `Deploy.ps1 -PruneStale`
>    removes the stale `PoseidonGame.exe`; never write over the retail name. This line said `ColdWarAssault.exe`
>    until 2026-08-27, which is how the wrong exe keeps getting launched.

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
  Editing models, textures or animations in Blender (the add-on, the Blender MCP, the conventions): `.claude/skills/ofp-blender/SKILL.md`.
- `Lint` / `LintFix` — combined C++ (+ Python) convenience targets.
- `FileSize` — warns at >3000 lines, errors at >5000.

The top-level `CMakeLists.txt` sets a large block of `-Wno-*` suppressions; these are deliberate for this legacy codebase (verified ~11,600 warnings without them). Don't "fix" warnings by re-enabling them globally.

## Rust components

A single Cargo workspace at the repo root (`Cargo.toml`) holds every crate, sharing one `Cargo.lock` and `target/`. Profiles (`dbg`/`rwdi`/`rel`) and `resolver = "3"` live in the root manifest. Members:

- `engine/Trident/` — `tri`, the test runner / integration tool.
- `engine/WgpuRenderer/rust/` — `wgpu_renderer`, the Rust side of the wgpu graphics backend (a `cdylib` linked into the game via corrosion; see below).
- `mserver/` — master-server service and tooling crates (`Archive`, `CLI`, `Client`, `MasterService`). Standard `cargo fmt` / `cargo clippy` / `cargo test` / `cargo build`.

All members declare `rust-version = "1.87"` (the MSRV of wgpu 29 and 30) so the workspace's MSRV-aware resolver doesn't hold shared dependencies back to older, incompatible versions.

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
