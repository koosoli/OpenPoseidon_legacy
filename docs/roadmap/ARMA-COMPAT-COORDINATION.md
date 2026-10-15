# Arma 1 / 2 / 3 compat -- coordination between the sessions

Three sessions work on playing original Arma missions straight from an installed game
folder (nothing copied into the CWA install). They share one compat layer
(`engine/Poseidon/Game/Compat/`, namespace `Poseidon::Arma1Compat`) and one game folder.

| Session | Branch | Started with | Doc |
|---|---|---|---|
| "ARMA 1 mission in Open Poseidon" | `MrDudeX-arma1` | `--arma1 <ArmA Gold>` | `docs/verification/arma1-direct-missions-20261011.md` |
| "Arma 2 Open Poseidon integration" | `MrDudeX-arma2` (merged into `MrDudeX-arma1` at `13f1773`) | `--arma2 <Arma 2>` | `docs/verification/arma2-direct-missions-20261012.md` |
| "Arma 3" (this one) | `MrDudeX-arma3`, branched from `MrDudeX-arma1` `178de3d` | `--arma3 <Arma 3>` | `docs/verification/arma3-direct-missions-20261014.md` |

## Rules for the shared layer

- `Arma1Compat::Generation()` is 1, 2 or 3. **Generation 3 is Arma 3.** A check that means
  "Arma 2 *and later*" (Dta layout, XML stringtables, BMTR animations, fibers, single-quoted
  strings, ...) is written `Generation() >= 2`; `== 2` is kept only where the behaviour is
  really Arma 2's alone. The Arma 3 branch converts the existing `== 2` checks accordingly;
  the conversion does not change anything for `--arma1` / `--arma2`.
- Arma 3 specifics live in new files (`Arma3Compat.cpp`, `Arma3Eden.cpp`, ...) so the
  three branches touch the shared files as little as possible.
- `MrDudeX-arma3` is rebased onto `MrDudeX-arma1` (and through it `MrDudeX-arma2`) when those
  move; it is never merged the other way without the owner saying so.
- One session owns the game folder at a time. Every deploy rewrites
  `Remastered\DEPLOYED-FROM.txt` (branch + commit); read it before measuring anything.
- Copy-protected or encrypted archives are never read: Arma 2's `hprotect` DLC payloads and
  Arma 3's Creator-DLC `.ebo` files (Global Mobilization, S.O.G., CSLA, Spearhead,
  Western Sahara, Expeditionary Forces, Reaction Forces) are skipped and logged.
- Clean room: only the documented data formats are read (PBO, raP, ODOL/OPRW, PAA, RTM,
  SQF/SQS text). No Bohemia executable is disassembled or decompiled, and no Bohemia data
  is committed.

## State of `MrDudeX-arma3` (2026-10-15)

- Rebased onto `MrDudeX-arma1` `d0cdd15`, plus the 16 `MrDudeX-arma2` commits `d721a0e..0608cd7`
  that `MrDudeX-arma1` has not merged yet, picked unchanged (a later rebase drops them again).
- Arma 3 men follow the Arma 2 man handling as it stands (turned, `reversed=1`, moves posed
  for the turned model since `91d70f9`); nothing Arma 3 specific is needed there.
- **Shared code touched (2026-10-15):** `QFBank::Load` reads PBO headers through a 64 KB
  buffer (`BankHeaderReader`; checked byte-identical on 552 Arma 3 and 1,355 CWA archives) --
  Arma 3's 521 archives took 59 s to mount with one `ReadFile` per byte. The test-mode stall
  monitor in `GameStateExtA2.cpp` counts from the first simulation step and allows 120 s
  under `--arma3` only; Arma 2 keeps 60 s from the start.
- **Shared code touched (2026-10-15, second round)** -- all generic or gated, listed so a
  rebase knows where to look:
  - `Pactext.cpp` `PacLevelMem::GetPixelInt` decodes DXT2-5 and ARGB8888 (the inventory
    icon preload sampled DXT5 Arma icons as "Bad texture format", 82 per mission).
  - `ProceduralTexture.hpp`: `#(...)r2t(rendertarget0,...)` is generated as a dark screen.
  - `Preproc.cpp` `GlobalScan`: a backslash ending a line outside `#define` continues the
    line, as in C (Arma 3's `BIS_fnc_moduleInit` carries a stray one).
  - `ObjAnimate` accepts Arma's third element; `ArcadeTemplate` reads `lock="LOCKEDPLAYER"`
    as LOCKED (under `Arma1Compat::Active()`); `ObjAmmo` counts a CWA stand-in vehicle's
    rounds by shot kind (Generation 3 only).
  - `Odol73.hpp` keeps the binariser's bounding centre (`storedBoundingCenter`);
    `ODOLLoader` uses it only for models on the man skeleton (pelvis, spine, head), the
    Arma 2 man path. ModelInfo's `boundingCenter` stays zero for revision 73.
  - `ODOLLoader` skinned synthesis: a bone-less proxy under `\proxies\` (headgear,
    glasses, hair, hmd, nvg -> head; equipment, backpack*, radio -> spine3) gets a bone.
  - `ManType`/`Man::DrawProxies`: Arma 3 gear slots (head, headgear, vest, backpack), only
    with Generation 3.
  - Picked from `MrDudeX-arma2`: `341a466` (hiddenSelectionsTextures), gate widened to
    `Generation() >= 2`; `RegisterWorldClasses` carries latitude/longitude for Generation 3.
- **Shared code touched (2026-10-15, third round)** -- worth taking on the Arma 1 / 2 branches:
  - **`dyn_cast` class ids** (`a82fa18`, engine-wide, all game modes): `StreetLamp`,
    `Airplane`, `Car`, `Tank`, `HelicopterAuto`, `Explosion` and `ObjectDestructed` had no
    `USE_CASTING`, so `dyn_cast<T>` answered yes for every object of the base. Crash found on
    an Arma 3 mission (`this switchLight "OFF"` on a UGV freed a "lamp light" in the car's
    memory); latent: `vehicle setDammage 0` called `Airplane::RepairGear` on any Transport.
    Every `dyn_cast` target in the tree now has an id.
  - `PreprocC.cpp`: `#include "\a3\..."` (leading separator = file-system root).
  - `GameStateExtGrp.cpp` `addAction`: Generation >= 2 strips structured-text tags from the
    title. `GameStateExtA2.cpp` `getGroupIconParams` answers `[[1,1,1,1],"",1,true]`.
  - `DisplayIntro::OnSimulate`: `--arma1-test-seconds` also ends an intro-only mission;
    `DisplayMission` logs the player's final position before `ARMA1-TEST PASS`.
  - `GameStateExtObj.cpp` `EngineMuzzleName`: a weapon's name stands for its first muzzle.
- **Shared code touched (2026-10-15, fourth round)** -- worth taking on the Arma 1 / 2 branches:
  - **`enableAI`** (`2d9d89a`, `GameStateExt.cpp` / `GameStateExtUi.cpp`, every game mode) was
    registered as an ignored command, so a unit held back with `disableAI "MOVE"` never moved
    again. It now clears what `disableAI` set; `"PATH"` reads as MOVE, `"ALL"` as every bit.
  - **Step sounds** (`e1885f6`, `SoldierOldSimProxy.cpp`, all moves): when a move has no
    `soundEdge1`/`soundEdge2`, the first two entries of Arma's `soundEdge[]` are the step
    times. Every imported Arma move read 0/0 and no Arma soldier made a step sound.
  - **`findDisplay 46`** (`GameStateExtWorldDialog.cpp`, Generation >= 2) is the mission's
    display while a mission runs (it answered -1, displayNull).
  - **Title resources** (`GameStateExtA3Rsc.cpp`, Generation 3 only; hooks in `CutRsc` /
    `GameTitleRsc` / `CutText` / `GameTitleText`, `A2CutRscLayer` / `A2CutTextLayer` and
    `A2IsNullHandle`): a cutRsc'd class with onLoad/onUnload runs them on a stand-in display.
  - **Ambient sound map of OPRW worlds** (`caaaac7`, `LandSave.cpp` `LoadOprwModern`, every
    Arma 1/2/3 world): it was never filled, so no cell played sea, birds, meadow or wind. Now
    made like `InitSoundMap` from the grid's forest bit, sea depth and height.
  - **Weapon shot override** (`4339389`, `Weapons.hpp/.cpp`, `VehicleAICombat.cpp`): a muzzle
    with `a3SoundShot[]` plays it for every fire mode (only generated Arma 3 weapons have it).
  - `allMissionObjects` (Arma 3 macro) goes over every world entity (`opA3AllEntities`); Arma 3
    modules are generated as `A1x_<module>` on the CWA logic (`32cd409`).
  - Test runs log a **motion census** (AI soldiers that never moved, and why) and a **sound
    census** (`Audio/SoundTestLog.hpp`, hooked in `WaveOAL`) before `ARMA1-TEST PASS`.
- **Foreign commits on this branch.** On 2026-10-15 09:54 another session pushed an upstream
  follow-up (`22d6461`, `58f1fe3`, `c479cde`, CWR-CE main) onto `MrDudeX-arma3` as onto the
  Arma 1 and Arma 2 branches; this branch was rebased onto it, not merged.
- **Game folder.** The Arma 3 test runs use their own binaries in
  `<CWA>\_OpenPoseidon-Arma3\bin` with `Remastered` as working directory
  (`scripts/arma3-pc/go.ps1`), so they never replace the exe/dll the Arma 1 / Arma 2 sessions
  deployed. `Remastered` itself is only written when this branch contains the installed commit
  (rule 0), with `DEPLOYED-FROM.txt` naming `MrDudeX-arma3`.
- **Delivery of CI builds to the PC.** The cloud session cannot download Actions artifacts
  (the artifact storage host is not reachable from it); a build reaches the PC as the zip
  the owner downloads, which `go.ps1` (`DEPLOY=1`) unpacks from `Downloads` or `incoming`.
