# AUD-103 — Audio thread ownership, and stable voice handles

Date: 2026-08-31
Branch: `agent-audio-architecture` (from `safety/asset-obs-001`)
Roadmap: `docs/roadmap/POST-DLSS-MODERNISATION-ROADMAP.md` parallel track A, §A.1 and §A.2.
Related: AUD-101 (HRTF probe), AUD-102 (EFX from world state), REN-ID-001 (the renderer's
generation-checked instance handles, whose scheme this copies).

## Summary

A.1 asks for one thread owning every OpenAL call. **There are two today**, and the second was
introduced deliberately, is documented, and is not going away. A.1 is therefore a real gap, but
the honest description of it is "two owners with a stated lock discipline", not "arbitrary
threads call the backend".

A.2 asks for an `AudioSnapshot` and stable, generation-checked voice handles. **Neither existed.**
Voices were identified by raw `WaveOAL*` throughout, and there was one place that had to remember
a voice across time and defended itself with a pointer comparison that cannot see the case it was
written for.

This change implements the **handles only**, wires them to the one consumer that already needed
them, and deliberately does not build the snapshot. Reasoning in §5.

---

## 1. The threading audit (A.1)

### 1.1 Nothing in `engine/Poseidon/Audio/` starts a thread

`grep -rn "std::thread"` across `engine/Poseidon/Audio/` returns nothing. The whole engine-side
audio layer — `SoundScene`, `Speaker`, `DynSound`, `VoiceBudget`, the streaming and voice
subtrees — is called from whatever thread calls it, and that is the main thread.

The per-frame drive is one call pair, both on the main thread inside `World::Simulate`:

- `engine/Poseidon/World/World.cpp:1862` — `GSoundScene->AdvanceAll(deltaT, ...)`
- `engine/Poseidon/World/World.cpp:1866` — `GSoundsys->Commit()`

The only other `Commit()` caller is the asset probe `engine/Poseidon/Asset/Probes/SoundPlayer.cpp:70`.

### 1.2 The backend owns the device and context, and has a second thread

`SoundSystemOAL` holds `ALCdevice*`/`ALCcontext*` as `void*` members
(`engine/PoseidonOpenAL/SoundSystemOAL.hpp:24-25`) and starts one worker in `Init()`:

- `engine/PoseidonOpenAL/SoundSystemOAL.cpp:261` — `StartStreamThread()`
- `engine/PoseidonOpenAL/SoundSystemOAL.cpp:286-320` — `StreamPumpLoop()`, a 10 ms cadence loop.

That loop makes **AL calls of its own**: pass 1 walks `_waves` under `_audioMutex` and calls
`WaveOAL::PumpDrainUpload()`, which recycles and re-queues the streamed source's AL buffer ring;
pass 2 runs the multi-millisecond codec read `DecodeQueuedChunk()` outside the lock. So the answer
to "who calls OpenAL" is **two threads**: the main thread, and the stream pump.

The rule that keeps them apart is stated at `engine/PoseidonOpenAL/SoundSystemOAL.hpp:33-42`:
`_audioMutex` (a `std::recursive_mutex`) serializes the registry walk and per-wave streaming
state, **but the hot per-frame parameter path is deliberately left lock-free** —
`WaveOAL::SetVolume` (`WaveOAL.cpp:1596`) and `WaveOAL::SetPosition` (`WaveOAL.cpp:1641`) call
`alSourcef`/`alSource3f` with no lock held, on the stated grounds that OpenAL Soft already
serializes individual `al*` calls per source.

**This is the A.1 gap, precisely stated.** It is not "gameplay calls the backend from anywhere".
It is that correctness currently rests on a property of the *driver* rather than on our own
ownership, and that the invariant lives in a header comment rather than in a type. A single
audio-owning thread would replace a driver guarantee with a structural one.

### 1.3 Decode workers already look the way A.1 wants

A.1 says "decode workers produce PCM; only the audio-owning thread uploads buffers". Two of the
three producers already obey that:

- **Mission preload.** `SoundScene::PreloadMissionAudio` (`engine/Poseidon/Audio/SoundScene.cpp:390`)
  is called from the loading screen at `engine/Poseidon/World/WorldInit.cpp:765`, and
  `SoundSystemOAL::PreloadWave` (`SoundSystemOAL.cpp:1462`) submits the file read + decode to the
  global `TaskPool`. Those workers touch **only** `Audio::PcmCache`, which is thread-safe. No AL
  call, no `_waves` access.
- **Voice over network.** `VoNClient` (`engine/Poseidon/Audio/Voice/VonApp.cpp`) decodes Opus on
  the network thread under its own `_replayerLock` (`:249, :350, :359, :370`) and hands out PCM
  through `pullSpeaker`. It never touches OpenAL.

The exception is the stream pump, which both decodes *and* uploads. That is the one producer A.1
would have to split.

### 1.4 Lifetime today is a hand-rolled spin-wait

Because the pump holds **raw, non-owning** `WaveOAL*` (it cannot hold a `Ref` — a wave sits at
refcount 0 until a `SoundScene` wraps it, `SoundSystemOAL.cpp:303-307`), the destructor has to
wait for it:

```
engine/PoseidonOpenAL/WaveOAL.cpp:114-118
    _streaming.aborted.store(true, std::memory_order_release);
    while (_streaming.decodeInFlight.load(std::memory_order_acquire))
        std::this_thread::yield();
```

It is correct, and it is a bespoke answer to exactly the hazard A.2 names ("destroying a game
object must not invalidate a voice still in use by the audio thread") — bespoke, single-purpose,
and only covering the one raw-pointer consumer that happens to exist.

---

## 2. What already existed for A.2, and what did not

| A.2 asks for | Status before this change |
|---|---|
| Stable voice handles | **Absent.** Voices are `IWave*` / `WaveOAL*` everywhere. |
| Generation checking | **Absent.** |
| Stable asset handles | Partial and informal: `Audio::PcmCache` is keyed by normalised filename, which is stable, but it is a cache key, not a handle, and carries no liveness. |
| An `AudioSnapshot` | **Absent**, in every sense. |
| Listener transform + velocity published at a bounded rate | Exists as **live mutable state**, not a snapshot: `_listenerPos/_listenerVel/_listenerDir/_listenerUp` (`SoundSystemOAL.hpp:99-102`), written by `SetListener` and read by `Commit`. |
| Source transforms/velocities | Live per-wave fields, `WaveOAL::_position/_velocity`, pushed straight to AL. |
| Category / importance / audible radius | Category exists (`WaveKind`); importance and radius exist only inside the per-frame budget pass, `engine/Poseidon/Audio/Core/VoiceBudget.*`. |
| Environment identifiers | Exists as live state (`_eaxEnv`) plus the blend in `EnvironmentBlend.hpp`. |
| Audio interpolation between sim updates | **Absent** for transforms. The one thing that *is* eased over time is the reverb parameter set (`EnvironmentBlender`), which is the environment half, not the transform half. |

**On the "informal snapshot" question, specifically.** There is a struct called `AudioState`
(`engine/Poseidon/Audio/Core/AudioState.hpp`) and it is *not* a snapshot. It is the per-wave live
playback state, it lives inside `WaveOAL`, and it is explicitly non-copyable
(`AudioState.hpp:56-57`). Nothing is versioned, generation-stamped, double-buffered or published.
There is no per-frame audio handoff, informal or otherwise: `Commit()` reads live globals and
writes AL directly.

---

## 3. The bug that gave the handles a consumer

`SoundSystemOAL::SuppressMusicForPreview` / `ResumeMusicForPreview` duck music while the Audio
options screen is mounted. The suppressed set was `std::vector<WaveOAL*>`, remembered across an
arbitrary span of frames, and Resume defended itself like this:

```
        // The wave may have been deleted while suppressed.  Cross-check
        // against the live registry — UnregisterWave erases from _waves
        // on destruction, so membership there proves the pointer is
        // still valid.
        if (std::find(_waves.begin(), _waves.end(), wave) == _waves.end())
            continue;
```

**That comment is false.** Membership in `_waves` proves *some* live wave is at that address. If a
suppressed music wave ends while the screen is open and the allocator hands its block to the next
wave of the same size class — likely, not exotic — the `find()` succeeds against a **different**
voice, and Resume clears *its* preview-mute and rewrites *its* gain. That is ABA, and no pointer
comparison can see it. It is exactly the failure a generation counter exists to catch.

This is a latent bug, not an observed one: it needs a music wave to end during a specific UI state
and the allocator to recycle the block. I did not reproduce it in the game (this session does not
launch the game). It is reproduced deterministically in a unit test with placement `new`.

---

## 4. What was built

### 4.1 `engine/Poseidon/Audio/Core/VoiceHandle.hpp` (new, header-only)

`Poseidon::Audio::VoiceHandle` + `VoiceHandleTable<T>`, a slot table over non-owning pointers.

Layout is **copied verbatim from the renderer's `InstanceTable`**
(`engine/WgpuRenderer/rust/src/gfx3d/cull.rs`), not reinvented:

```
    bits  0..23 : slot + 1   -- handle 0 is never issued and means "none"
    bits 24..31 : generation -- 8 bits, per slot
```

`Acquire` recycles from a free list; `Release` clears the slot, bumps its generation and returns
it to the list; `Resolve` refuses a handle that is zero, out of range, points at a free slot, or
carries a stale generation. Every refusal increments a **readable** `StaleOps()` counter — the
specific gap REN-ID-001 found in the renderer, where a write-only counter made a rejection
indistinguishable from a no-op.

The table is **not internally synchronized, on purpose**: its owner already holds `_audioMutex`
over the same data, and a second, finer lock would only be a second lock order to get wrong.
Being lock-free of its own also keeps it pure bookkeeping with no device dependency, which is what
makes it testable — the other REN-ID-001 lesson.

### 4.2 Wiring, so this is not machinery ahead of its consumer

- `WaveOAL::_voiceHandle` (`WaveOAL.hpp`), issued in `SoundSystemOAL::RegisterWave` and retired in
  `UnregisterWave`, so a handle is valid for exactly the wave's registered lifetime.
- `_suppressedMusic` changed from `std::vector<WaveOAL*>` to `std::vector<Audio::VoiceHandle>`;
  `ResumeMusicForPreview` now goes through `Resolve`. The `std::find` liveness check is gone.
- `AudioCounters` gained `staleVoiceHandles` and `liveVoices` (`IAudioSystem.hpp`), filled in
  `GetCounters()`. A non-zero `staleVoiceHandles` is **normal** and means the guard fired; it is
  evidence, not an error.

### 4.3 Where this legitimately differs from `PresentationSnapshot`

The brief asked that an `AudioSnapshot`, if warranted, follow `PresentationSnapshot`'s shape. It
is not warranted yet (§5), but the comparison is worth recording because the two subsystems are
in *opposite* positions:

| | `PresentationSnapshot` | Audio today |
|---|---|---|
| Payload | Fixed-size, by value, **no handles at all** | n/a — no snapshot |
| Second thread consuming it | **None yet** (the ring is single-threaded, with release/acquire written now for a future edge) | **Already exists** (the stream pump) |
| Identity / epoch | `resourceEpoch` is a deliberately **unfed seam** — nothing bumps it, nothing compares it, because there is nothing in the payload it could invalidate | Identity is the load-bearing part **today**, because a live second thread holds raw pointers |

So the renderer needed the buffer first and can defer identity; audio needs identity first and can
defer the buffer. Copying the renderer's *snapshot* shape now would reproduce its one dead part
(an epoch with nothing to guard) into a subsystem that does not even have the consumer that would
justify the buffer. Copying its *handle* shape is the part that transfers, and that is what was
copied. If an `AudioSnapshot` is later built, it should follow the ring, the generation stamp and
the diagnostics counters as written there, and it should carry `VoiceHandle`s — at which point the
renderer's `resourceEpoch` seam will finally have an audio analogue with something real to guard.

---

## 5. What was deliberately NOT built, and why

**No `AudioSnapshot`.** A.1's audio thread does not exist. A snapshot ring published to nobody
costs memory, costs a publish per frame, and buys nothing — this project has a standing lesson
about exactly that, and `PresentationSnapshot`'s own `kPresentationSnapshotRingSize = 2` comment
makes the same argument in the other direction (no third slot until a producer can run ahead).
The snapshot should land **with** the audio thread, in one change, so its shape is decided by a
real consumer rather than guessed. Note that the transform-interpolation requirement in A.2 is
part of the same package: there is nothing to interpolate *between* until there is a bounded
publish rate.

**No audio thread.** Out of scope for one verifiable slice, and it is the larger and riskier half:
it means splitting `StreamPumpLoop`'s decode from its upload, moving the lock-free
`SetVolume`/`SetPosition` path onto a command queue, and deciding what happens to the
`decodeInFlight` spin-wait (which the handles make replaceable, but only once the pump resolves
voices by handle rather than holding raw pointers).

**Voice handles are backend-side only.** `IWave` and `SoundScene` still traffic in `IWave*`. Those
call sites are refcounted (`Ref<IWave>`), so they are memory-safe as they stand; converting them
would be a large mechanical change with no bug behind it today. The handle exists where a
non-owning cross-time reference exists, which is the backend.

**The stream pump was not converted to handles.** It is the other raw-pointer consumer and the
obvious next user, but converting it means touching the `decodeInFlight` teardown protocol, which
wants its own change with its own test.

---

## 6. Evidence

Built with the Bash-shell CMake path (`cmake --preset win-x64-clang-rwdi`), targets
`PoseidonTests` and `PoseidonGame`, both clean. `PoseidonGame` was built specifically to prove the
backend changes link in the game exe, for the same reason AUD-101 did it.

The work was rebased onto `safety/asset-obs-001` mid-session (the branch had fallen 11 commits
behind, and those commits add tests of their own), so the before/after is given twice: once at the
commit this branched from, and once at the rebased base — the second measured by running the same
binary with `~[VoiceHandle]`, which is an exact subtraction rather than a second build.

| | test cases | assertions | failures |
|---|---|---|---|
| Original base `48a26cff`, unmodified | 3019 (20 skipped) | 240,905 | 0 |
| Rebased base (`~[VoiceHandle]`) | 3033 (20 skipped) | 241,798 | 0 |
| Rebased, full suite | 3044 (20 skipped) | 242,623 | 0 |

+11 cases, +825 assertions against the rebased base, and nothing pre-existing regressed. Nine are device-free table tests
(`tests/unit/engine/Poseidon/Audio/test_voice_handle.cpp`); two are integration tests against a
real `SoundSystemOAL` (`test_voice_handle_oal.cpp`), and **both were confirmed to actually run**
on this workstation rather than hit their `SKIP` guards — the suppressed-music scenario really
built (`CountSuppressedMusicWaves() == 2`) and `staleVoiceHandles` really went 0 → 1 through the
production `ResumeMusicForPreview` path.

### Ablations

A test that cannot fail is worthless, so each check was deleted and the suite re-run.

**Ablation 1 — delete the generation compare** (`if (!e.live || e.generation != ...)` →
`if (!e.live)`, i.e. pre-handle behaviour):

> `[VoiceHandle]`: **9 cases, 7 passed, 2 failed; 814 assertions, 6 failed.**

The two that fail are the ABA case and the double-release case. Seven still pass, correctly: they
pin the *other* refusal reasons (the `+1` bias, the range check, the layout) and the generation
*arithmetic*, none of which the compare is responsible for. This matches REN-ID-001's finding that
3 of its 5 handle tests survived the same ablation.

**Ablation 2 — delete the free-slot (`live`) check**, keeping the generation compare:

> First run: **9 cases, all passed.** The flag had **no test**.

That is the REN-ID-001 failure mode reproduced in my own change, caught only because the ablation
was run. The hole is real: the check that survives is `generation == handle.generation`, and a
*freed* slot's post-bump generation matches a handle carrying that generation. `Resolve` still
answered `nullptr` (a freed slot's pointer is cleared), which is why a Resolve-only test could not
see it — but `IsLive` returned true and `Release` returned true, double-freeing the slot onto the
free list so two live voices would later share it. A case was added asserting `Resolve`, `IsLive`,
`Release` **and** the free list's integrity afterwards. Re-ablated:

> **9 cases, 8 passed, 1 failed; 817 assertions, 3 failed.**

**Ablation 3 — bypass the table in the integration path** (`ResumeMusicForPreview` scans the
registry instead of calling `Resolve`):

> `[VoiceHandle]`: **11 cases, 10 passed, 1 failed** — `staleVoiceHandles == baseStale + 1`
> observed `0 == 1`.

This is what pins that the handles are actually *on* the production path and not merely
unit-tested beside it.

## 7. What the owner should know

Nothing here changes what the game sounds like, and nothing was deployed (this session was asked
not to launch the game or deploy). The one behavioural change is that if a music track ends while
the Audio options screen is open, the engine now correctly does nothing to the wave that inherited
its memory, instead of possibly un-ducking and re-gaining an unrelated voice. `staleVoiceHandles`
in `GetCounters()` counts how often that guard fires.

## 8. Verdict on A.1 / A.2

- **A.1: not started, and now accurately described.** Two threads make AL calls; the discipline is
  a documented lock plus a driver guarantee, not an owner. Nothing in this change moves that.
- **A.2: the identity half is done for the backend, with tests and ablation evidence.** The
  snapshot half is untouched and should land with A.1's thread.
