# Original OFP map weather and actual standard US player coverage

Source baseline: `aa33a726`. This change adds a source census and isolated
installed runner. It changes no Man rendering/movement, contact producer,
surface admission, terrain shader or cloth material. The root owns GPU/game
execution, deployment, appearance inspection and the authoritative ledger.

## Actual existing player proof

Current matched `e5445111` controlled-player runs now pass functional gates
on all four original maps. Everon/Nogova/Malden sand walking changed real
boot cells; Everon/Nogova wet soil walking changed mud cells. Person wetness
reached .339/.323/.318/.322 on Eden/Noe/Abel/Cain. Every run retained actual
external body draw and wet stock atlas uploads, paused camera return, stable
EXE/DLL hashes and normal exit0. Records are under `build/original-weather-player/`
with labels `original-everon-native-player`, `original-nogova-centred-native-player`,
`original-malden-player` and `original-kolgujev-player` dated 20261001.
Root inspected Everon/Nogova bodies and cloth creases; third-person contact
relief remains weak pending the bounded boot-focus build. These checks do
not fill the unsupported material families below or accept every appearance.

The first new Nogova run failed its class spelling check (`Default` versus
`default`), after successful sand walking. `f4881226` uses the engine's
case-insensitive config-class semantics while keeping texture/files/sound/
character comparisons exact. The centred rerun passed and retained the
complete unchanged native tile24/23 at origin4787.5/4587.5, 1089 vertices.

The matched installed `7e2f1df7` run
`build/weather-player/standard-us-clear-east-20260930-235443` completed normally
with unchanged EXE/DLL hashes. Its actual controlled **SoldierWB**, using SDL W,
travelled 4.880 m on Noe sand and 4.877 m on wet cultivated soil. Sand and mud
each acquired five chunks; actual Person uniform wetness reached 0.306318.
The actor was not replaced by an AI walker. First/third-person/return pictures
and actual queue/draw/body-section traces were retained. This does not establish
other maps, current shader appearance or every stance/skin/mod combination.

The new parser was exercised against that saved real log, recognizing four
manual changed sand boot events, four manual changed mud boot events, actual
external queue/draw/primary body rows and two wet stock cloth upload rows.
No new game session was launched for this source check.

## Original corpus and conservative source census

`Audit-OriginalOFPContactSources.py` uses the existing PoseidonTools CPU config
decoder and legacy terrain exporter. It reads original installed
`Worlds/eden.wrp`, `Worlds/abel.wrp`, `Worlds/cain.wrp` and the actual `noe.wrp`
member of `AddOns/Noe.pbo`. It merges installed base CfgSurfaces with the Noe
addon CfgSurfaces, matching the shared stock definitions rather than applying
Noe metadata only on Noe. No CfgSurfaces/config entries were present in the
Cain/Abel/Eden terrain PBOs inspected here. Runtime metadata guards remain
necessary for other loaded overlays/mods.

The script models actual TextureBank basename/four-two-character-quadrant
splitting and the existing trailing-six-question-mark compatibility rule. It
records exact class/files/sound/character metadata and the current conservative
Mud/Sand source policy. This is source inference, not execution of Landscape or
the physical contact helper. The checked-in census records corpus hashes,
all qualifying palette identities, representative unknown identities and
complete quadrant-class cell counts. The reproducible full report also retains
every used palette and nearby source owner records under ignored `build/`.

| Original map | Admitted pure-sand palette cells | Admitted pure-mud palette cells | Source candidate sand X/Z | Source candidate soil X/Z |
|---|---:|---:|---|---|
| Eden / Everon | 42634 | 135 | 5975 / 3325 | 6425 / 7175 |
| Noe / Nogova | 27867 | 170 | 2675 / 5125, previously actual-player tested | 4975 / 4675, previously actual-player tested |
| Abel / Malden | 43286 | 0 | 7575 / 10675 | No admitted soil family |
| Cain / Kolgujev | 0 | 0 | No admitted sand family | No admitted soil family |

These cell counts include underwater/shore/steep cells and are **not usable
soft-ground area or contact acceptance**. Candidate search additionally requires
all coarse corner heights above 3 m, range at most 2 m across a 50 m cell and
no owner centre within 12 m. Owner centres are not collision bounds; a distant
wall, forest mesh or roof can still intersect a lane. Noe's previously passed
sand lane is preferred over the generic source search because the search
conservatively rejects its nearby ruin centre even though the actual eastward
manual lane passed. It must pass current runtime guards again.

Eden sand is palette2 `eden/ps.paa`, all four Sand/`ps??????`/sand/empty-character,
source centre height 9.315 m; corner range 1.89 m/50 m. No owner centres occur
in the surrounding 3x3 cells. Eden soil is palette6 `eden/pol.paa`, all four
Field/pol/dirt/empty-character, all corners 141.885 m. A source bush centre is
about 25.2 m west; this is not a roof or collision guarantee.

Noe sand uses `o/pt.paa`, SandDark/`pt??????`/sand/empty-character. Noe soil is
palette12 `o/pole2.paa`, the exact previously audited cultivated asset plus
unchanged Default/default/normalExt/empty-character fallback, source centre
21.555 m. This exact asset exception is independent of map name, without
licensing other Default materials.

Abel sand is palette11 `abel/pi.paa`, all four
SandAbel/`pi??????`/sand/empty-character, source centre 27.765 m and corner range
0.675 m/50 m. Its nearest source owner centre in nearby cells is reed
`data3d/ker rakosi.p3d` about 49.6 m away. Mud remains unsupported there.

Cain's cloth/body-only candidate is X5925/Z6075, palette2 `cain/t9.paa`, all four
Default/default/normalExt/empty-character, all coarse corners 415.935 m. The
nearest source tree centre is about 13.3 m away. It has no soft admission;
actual exposed rain, movement and body guards must still pass.

## Unknown material assessment; no production alias proposed

Actual `Dta/Cain.pbo` j9/t9/k5/u2/l4 and `Dta/Abel.pbo` pb members were extracted,
hashed and decoded with the repo PAA decoder. Cain j9/t9/k5/u2 are mottled
vegetation/moss/rock surfaces, not demonstrable bare cultivated soil. Cain l4
is bare grey granular ground, but the source examined does not distinguish
sand from hard gravel. Abel pb is dark mottled vegetated/rocky ground. Brown
or grey colour does not establish physical softness.

None has an authored soil/sand CfgSurfaces identity in this corpus. The actual
`normalExt` infantry footstep table mixes dirt_L/R and grass_L/R, so its fallback
is not an authoritative soil classification either. No Cain/Abel alias is
proposed. Further expansion needs explicit source-specific authored evidence,
whole support-footprint policy, hard/road/shore controls and its own tests.
This limitation does not restrict the independently implemented all-ground
wetness/puddle/physical rainwater policy.

## New serial installed runner

`Test-OriginalOFPWeatherPlayerRuntime.ps1` creates four private original-map
missions by copying the existing one-player SoldierWB mission into `.eden`,
`.noe`, `.abel`, `.cain` output folders. It uses the actual original world
selected by the mission suffix, without a modern loader, `selectPlayer`, AI
surrogate, animation override, fake stamps or fake wetness. Existing runner
defaults and checked-in missions remain unchanged.

For admitted sand/soil it queries actual `dev_sand`/`dev_mud` metadata across
the bounded lane and all four real source quadrants before walking. Ordinary
SDL W must move the actual living controlled player at least 4 m within the
source-proved lane. Positive contacts require fresh `manual=true` changed boot
rows, increasing physical revision and sampled negative signed offsets at
actual boot coordinates. Dry soil must not generate changed mud contacts.
Abel/Cain unsupported soil instead receives an explicit unqualified wet-mud
negative; Cain also has a dry ordinary manual walk. Missing sand/mud positives
are recorded in `softCoverage`, not promoted to complete map support.

All four maps require dry Person wetness, real liquid rain and Person wetness
at least 0.3, actual standard WB cloth atlas upload/encoding, and actual external
resident/non-invisible queue, living/non-hidden draw and primary body sections.
The queue and draw must be within 0.65 simulation seconds of the current stage;
a pre-weather body row cannot satisfy that gate. Section identities are bounded
deduplicated producer facts, while current cloth encoding is checked separately.
This source draw chain cannot prove visible pixels. Root must inspect third
person pictures, including after real soft contacts and first-to-third return.
The first-person picture preserves the ordinary classic inside view; it does
not assert that first person should show an entire body.

Pause checks cover simulation time, actor position, wetness, both physical
stores and real weather. Contact geometry is resampled after camera return.
The runner requires map corpus hash, matched installed stamp/EXE/DLL pair,
lock/private profile, complete PNG/hash, source/head/script/helper provenance,
bounded deadlines and normal exit. Forced owned-PID cleanup cannot pass.
No bitwise picture equality or wet-material style acceptance is asserted.

Root execution after installing the matched current pair:

```powershell
python scripts/Audit-OriginalOFPContactSources.py --self-test
pwsh -NoProfile -File scripts/Test-OriginalOFPWeatherPlayerRuntime.ps1 -SelfTest
```

```powershell
python scripts/Audit-OriginalOFPContactSources.py --game-dir "D:/SteamLibrary/steamapps/common/ARMA Cold War Assault" --tools "build/win-x64-clang-rwdi/apps/tools/Tools/PoseidonTools.exe" --out build/original-contact-source
```

```bash
LOCK_OWNER="original OFP actual player weather" scripts/with-game-lock.sh pwsh -NoProfile -File scripts/Test-OriginalOFPWeatherPlayerRuntime.ps1 -ExpectedCommit <installed-commit>
```

Use `-Maps eden` etc for one serial map arm. A refreshed full report can be
supplied through `-SourceCensus`; it still needs the original corpus and actual
runtime tuple guards. Records live under `build/original-weather-player/`.

## Checks and remaining acceptance

Source-census falsifiers and runner self-tests passed, including mixed/hard
quadrant, wrong-source/clutter, AI-surrogate, proxy-only body and wrong cloth
encoding refusals, including stale body-row refusal. Exact four-world CPU
export/census completed. Saved actual
Noe log parsers passed. No game, GPU, full build or deployment was run by the
agent. All new installed arms, actual visible body pixels, wet appearance and
new Eden/Abel physical contacts remain root runtime acceptance work. Unsupported
soft materials remain open scope, not a passing all-map deformation claim.
