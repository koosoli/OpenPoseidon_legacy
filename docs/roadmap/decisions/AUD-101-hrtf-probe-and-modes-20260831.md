# AUD-101 — HRTF probe, headphone modes, and why a refusal is now legible

Date: 2026-08-31
Branch: `agent-audio-hrtf`
Roadmap: `docs/roadmap/POST-DLSS-MODERNISATION-ROADMAP.md` § 1.4 "Small audio wins", **item 1 only**.
Item 2 (EFX environment driven from world state) is deliberately untouched here.

## What was already done, and what this adds

Item 1 was half-landed by `5ec08e32` ("HRTF headphone modes: Off / Auto / On, first
half") on the base branch. That commit established the mode enum, the `audio.cfg`
key, the options row, and — the important part — applied the mode through
`alcResetDeviceSOFT`, which reconfigures the device **in place**. Sources, buffers and
the context all survive, so the delicate `SwitchOutputDevice` recreation sequence was
never touched. `SwitchOutputDevice` re-runs `ProbeHrtf()` and `ApplyHrtfMode()` after
the new context is current, so a forced On/Off follows the user across a device change.
That behaviour is preserved unchanged by this work.

What `5ec08e32` did **not** do, and this change adds:

1. **Dataset enumeration by name.** It logged only the count from
   `ALC_NUM_HRTF_SPECIFIERS_SOFT`. `alcGetStringiSOFT` is now resolved and the names are
   enumerated once per device, cached in `_hrtfSpecifiers`, and exposed as
   `IAudioSystem::ListHrtfSpecifiers()`.
2. **The reason HRTF is off.** It queried only `ALC_HRTF_SOFT`, a bool. A bool cannot
   tell "off because the user chose Off" from "the user asked for On and the driver
   refused" — and those two need different responses from us. `ALC_HRTF_STATUS_SOFT` is
   now read and mapped to `IAudioSystem::HrtfDeviceStatus`.

## The three modes

| Mode | Attribute passed to `alcResetDeviceSOFT` | Meaning |
|---|---|---|
| `Off` (0) | `ALC_HRTF_SOFT = ALC_FALSE` | Force HRTF off. Device reports `Disabled`. |
| `Auto` (1, default) | `ALC_HRTF_SOFT = ALC_DONT_CARE_SOFT` | Leave the decision to OpenAL Soft, which engages HRTF when the output looks like headphones. |
| `On` (2) | `ALC_HRTF_SOFT = ALC_TRUE` | Request HRTF. The device may still refuse — see below. |

Persisted as `hrtfMode` in `audio.cfg`. A missing key means `Auto`; an out-of-range
hand-edited value normalizes to `Auto` (`AudioConfig::Normalize`). Surfaced on the
Audio options page as a 3-way stepper (`kRowHrtf`).

## What the probe reports on this machine

Measured, not inferred — by running the `[.device]`-tagged cases in
`tests/unit/engine/Poseidon/Audio/test_hrtf_mode.cpp` against the real default playback
device on this workstation (`PoseidonTests.exe "[hrtf]"`, all 6 cases pass):

- `ALC_SOFT_HRTF` **is** present, and `alcResetDeviceSOFT` resolves.
- **1 dataset, named `Built-In HRTF`** — OpenAL Soft's bundled default.
- Mode `On` → status active, specifier `Built-In HRTF`, and the specifier is a member of
  the enumerated list (the test asserts this, so the two readouts cannot drift apart).
- Mode `Off` → inactive, and the device status reads `Disabled`, **not** `Denied`. This
  is the distinction the new status code exists to make, and it is confirmed on real
  hardware rather than only in the mapping test.
- Untouched device reports `ALC_HRTF_SOFT == 1`, and a `ALC_DONT_CARE_SOFT` reset leaves
  it at `1`.

That last point resolves an open question left in the base branch. A comment in the
existing test hypothesised that the 2026-08-30 owner report ("no Auto-vs-On difference")
might mean `Auto` was silently *engaging* HRTF at boot on a device that would otherwise
have had it off, and that `Auto` should therefore be changed to "do not touch the
device". **On this machine that is not what happens**: HRTF is already on before
anything touches the device, so `Auto` and `On` are genuinely identical here, and the
owner observing no difference between them is correct behaviour, not a bug. `Auto`
semantics are therefore left alone. This is one machine's evidence; it does not prove
the same on the owner's, which is why the log now prints the untouched-vs-applied
outcome explicitly.

## What happens on a driver without the extension

Nothing fails and the game starts normally. `ProbeHrtf()` leaves `_canHrtf == false`,
`SetHrtfMode()` returns `false` without touching the device, `GetHrtfStatus()` reports
`supported == false` / `deviceStatus == Unknown`, and `ListHrtfSpecifiers()` is empty.
The options row still renders and still persists the user's choice — it simply has no
effect, which is preferable to the row vanishing depending on the sound card.

Three degradation steps, each logged distinctly:

- Extension absent → `HRTF: ALC_SOFT_HRTF not available on this device -- headphone mode has no effect` (INFO).
- Extension present but `alcResetDeviceSOFT` missing → WARN, treated as unsupported.
- Supported but **0 datasets** → WARN `forcing On will be denied`, said at probe time so
  the later refusal has its explanation next to it in the log.
- Supported, datasets present, but `alcGetStringiSOFT` missing → still fully supported;
  logs the count and says names are unknown. A driver exposing one SOFT entry point and
  not the other degrades to "names unknown", never to "unsupported".

## Reading the log

`ApplyHrtfMode()` emits one line per application:

```
HRTF: mode On applied, now ACTIVE -- enabled (Built-In HRTF)
HRTF: mode Off applied, now inactive -- disabled (no dataset)
```

and, only when the user asked for `On` and the device refused:

```
HRTF: requested On but the device refused (denied by the device) -- output stays non-binaural
```

`HrtfWasRefused()` is true for exactly `Denied` and `UnsupportedFormat` — the latter is
what a surround/multichannel output returns. `Disabled` is not a refusal and does not
warn, because the user choosing Off is not a fault.

## Implementation note worth keeping

`alcGetStringiSOFT` is resolved per device via `alcGetProcAddress`, exactly like
`alcResetDeviceSOFT`, and **must** be. `OpenALRuntime.hpp` shims the base OpenAL API
through macros but carries no SOFT entries, so a direct call binds to the `dllimport`
prototype in `alext.h` and fails to link in the game exe. The unit tests cannot catch
this, because they link OpenAL directly. `PoseidonGame` was built as part of this work
specifically to prove the link, and it links clean.

SOFT entry points are also re-resolved on every device switch — a pointer obtained from
one device is not guaranteed valid on another.

## What is NOT covered

- **No listening test was performed.** The roadmap asks for "an audible test scene
  covering front, rear, above, below and moving-source localisation". That needs the
  owner's ears and was not attempted; nothing here has been confirmed to *sound*
  correct, only to be configured correctly and to report honestly.
- **The mono-3D-only requirement is unverified.** The roadmap requires music, UI, radio
  and other intentionally non-spatial stereo content to keep its presentation. HRTF in
  OpenAL Soft applies at the device mixer, so stereo sources are not spatialized, but
  this was not measured against the game's actual mixing of those categories.
- **`Auto` semantics on the owner's hardware.** See above — refuted on this machine
  only. If the owner reports HRTF apparently doing nothing, the new probe log answers it
  directly, which is the point of the change.
- **The status readout is not surfaced in the UI.** `deviceStatus` and the dataset list
  reach `IAudioSystem` but the options page still shows only Off/Auto/On. Telling the
  user "your device refused this" in the Audio screen wants a stringtable key, and the
  modern Options resources are data this repo does not ship (see CLAUDE.md on
  `Install.ps1`) — the existing HRTF row already falls back to hardcoded English labels
  for the same reason. Adding a status line was deliberately not half-done here; the
  information is in the log, which is what the owner is asked for.
- **Roadmap item 2 (EFX environment from world state) is untouched.**

## Owner smoke test

1. Launch the game and open Options → Audio. The "3D Headphone Audio (HRTF)" row should
   read `Auto`.
2. Step it Off → On → Auto. Each change should be audible on the preview sample and must
   not glitch, stall or drop other playing audio (the device reconfigures in place, so
   nothing should stop).
3. Quit and check the log for `HRTF:` lines. Expect the dataset enumeration at startup
   and one `mode ... applied` line per change. If any line says `requested On but the
   device refused`, that text plus the reason in brackets is exactly what to send back.
4. Change output device while the game runs (if the Audio page offers it) and confirm the
   HRTF setting survives, with a fresh probe logged for the new device.
5. The actual localisation question — do front/rear/above/below and moving sources place
   correctly on headphones with On versus Off — is untested and is the reason this is
   "first half plus observability", not "item 1 complete".
