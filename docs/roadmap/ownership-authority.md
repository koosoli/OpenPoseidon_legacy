# Ownership and authority table (roadmap Phase 4.1)

**Status:** second pass, 2026-08-31, from code reading at `agent-ownership-table`
(based on `safety/asset-obs-001` @ `48a26cff`; every citation below was opened and
read at that commit). The first pass (2026-08-30) established the thread inventory
and thirteen rows; this pass extends it to **36 rows** across the whole spine the
roadmap names — simulation clocks, entities, AI, scripting, physics, terrain and
streaming, the scene and its lights, GPU resources, audio, network, and the
save/load and multiplayer authority §4.1 explicitly requires — and marks the rows
that are *contested* or *unenforced* rather than smoothing them.

This is the document Phase 4.1 requires before any subsystem is parallelised: who
may mutate each class of state, from which thread, and where the handoff back to
the owner happens. **The rules below are what the code does today, not what it
should do.** Where a header states a rule that the code no longer obeys, the row
says so and cites both.

The audit's surprises — including one stale rule that shipping code contradicts,
and one absent check the roadmap assumes exists — are written up in
[`decisions/SIM-805-ownership-audit-20260831.md`](decisions/SIM-805-ownership-audit-20260831.md).

Companions: [`render-snapshot-design.md`](render-snapshot-design.md) (the renderer's
read surface), [`decisions/SIM-801`](decisions/SIM-801-worker-fp-environment-20260831.md)
(worker FP environment), [`SIM-803`](decisions/SIM-803-stable-iteration-and-tie-breaking-20260831.md)
(stable iteration), [`SIM-804`](decisions/SIM-804-physics-integration-boundary-20260831.md)
(physics boundary), [`SIM-806`](decisions/SIM-806-worker-count-determinism-harness-20260831.md)
and [`SIM-807`](decisions/SIM-807-preparer-determinism-20260831.md) (what is genuinely
parallel, and the heap race that was found in it),
[`REN-ID-001`](decisions/REN-ID-001-stable-render-object-identity-20260831.md) (identity).

## How to read this

**Owner** is the thread that may mutate. **Others** is what any other thread may do.
**Boundary** is the mechanism that makes the handoff safe — a lock, a barrier, an
atomic flip, or the honest answer "nothing; it holds because nobody does it".

The **Rule** column is the most important one. Three values:

- **stated** — a comment or banner in the source says the rule, and the code obeys it.
- **implicit** — the code obeys a rule nothing states. True today, true by accident.
- **checked** — something in the running program notices a violation.

There are, after this pass, **four** checked rows in the entire engine. Three of them
were added by this pass; the fourth is the RNG guard that inspired them. A fifth (row 3.9)
was added on 2026-09-02, the day after the render thread made the question live.

## Threads the process actually runs

| # | Thread | Created at | Count / priority |
|---|---|---|---|
| T1 | Main: simulation, draw traversal, publish | OS | 1 |
| T2 | enkiTS TaskPool workers | `apps/cwr/GameBase/GameBase.cpp:124` | auto, cap 8 (`Core/TaskPool.hpp:107`), `--max-threads` |
| T3 | ObjectStreamPreparer workers | `World/Terrain/ObjectStreamPrepare.cpp:397-404`, entry `WorkerMain` `:152` | 2 default (1–16), BELOW_NORMAL |
| T4 | Audio stream pump | `PoseidonOpenAL/SoundSystemOAL.cpp:206` | 1, ~10 ms cadence |
| T5 | Legacy UDP peer listen/send | `Network/Legacy/NetPeer.cpp:607,615` | 2 per peer, priority 2 |
| T6 | ~~FileServer~~ — **not a thread** | `IO/FileServer.cpp:19` compiles the async path out | — |
| T7 | Rust/wgpu | none spawned by our code; every `wgr_*` runs on the caller | — |
| T8 | Auxiliary (WatchDog, DownloadWorker, MasterServerBrowser, workshop fetch, dev harness) | inventory in `SIM-801` table T8a–T8f | occasional |
| T9 | **Wgpu render worker** — runs `RunConsumerBlock`: the queue drains and `wgr_render_frame` | `WgpuRenderer/EngineWgpu.cpp` `StartRenderThread`; default ON since 2026-09-01 (`WGR_RENDER_THREAD=0` off, `WGR_RENDER_OVERLAP=0` lockstep) | 1 |
| — | ~~Secondary AI thread~~ — **does not exist** | `Core/Types.hpp:36` forward-declares `SecondaryThread`; no definition anywhere | see §Simulation note |

---

## 1. Simulation: clocks, entities, AI, scripting, physics

| # | State | Owner | Others | Boundary | Rule | Cited at |
|---|---|---|---|---|---|---|
| 1.1 | `Glob.time` — the authoritative simulation clock | T1, one add per fixed tick | read everywhere (709 read sites) | fixed-step loop | implicit | write `World/World.cpp:1919`; loop `World.cpp:912`; decl `Core/Global.hpp:151,166` |
| 1.2 | `Glob.uiTime` — frame-rate clock for UI and effects | T1, one add per **frame** | read | none | implicit | `World/World.cpp:398` |
| 1.3 | `Glob.clock` — date / time-of-day | T1 | read | none | implicit | `World/WorldSetup.cpp:1232`, script setter `Game/Commands/GameStateExtUi.cpp:1761` |
| 1.4 | `Glob.time` **written by deserialization** | T1, during load only | — | load is synchronous, inside the frame | implicit | `World/WorldImpl.cpp:1790`, `World/WorldInit.cpp:1116` |
| 1.5 | `GWorld` / entities / `GScene` | T1 only | T2 reads landscape grids treated as immutable during a `Fill` | `ParallelFor` barrier; pass-4 commit on T1 | implicit | `World/Terrain/Landscape.cpp:1381-1398`; forbidden set `World/Terrain/ObjectStreamPrepare.hpp:16-34` |
| 1.6 | `AICenter` / `AIGroup` / `AISubgroup` / `AIUnit` | T1 only, inside the fixed tick | none | fixed tick | implicit | step `World/World.cpp:1934` → `World/WorldImpl.cpp:429`; `AICenter::Think` mutates plain members at `AI/AICenterImplPreview.cpp:527` |
| 1.7 | **The background-AI path** — `_secThread` | nobody | nobody | — | **dead code** | `#define BACKGROUND_AI 0` at `World/WorldInit.cpp:90` and `World/WorldSetup.cpp:163`; **undefined** (so also 0) in `World.cpp:1401,1527,1931`; `SecondaryThread` is forward-declared only at `Core/Types.hpp:36` and `new SecondaryThread` at `World/WorldInit.cpp:119` would not compile |
| 1.8 | Pathfinding (`AI/Path/`) | T1 | none | — | implicit | no threading primitive exists in the subtree: `grep -rn "std::thread\|TaskPool\|ParallelFor\|mutex\|atomic" engine/Poseidon/AI/` returns nothing |
| 1.9 | `GGameState` — the script variable bank | T1 | none | **none — no lock exists** | implicit | singleton `Evaluator/express.hpp:910`; `_vars` `express.hpp:556`; mutated `express.cpp:2313,2401,2483,2527,2518,2614`; `grep -rn "mutex\|atomic\|thread" engine/Evaluator/` returns nothing |
| 1.10 | `EvalState` / `SqsRunner` | T1 | none | — | implicit | `EvalState` is a by-value member `Evaluator/EvaluatorHost.hpp:18`; `SqsRunner` is a stack local at `EvalState.cpp:728`, `EvaluatorHost.cpp:163,275`, `Validate.cpp:186` |
| 1.11 | Script stepping | T1 — fixed tick when opted in, otherwise per frame and from ~11 UI sites | none | — | stated | tick `World/World.cpp:1928` (gated on `Dev::FixedStepScriptsEnabled()`); frame `World.cpp:972`; body `World/WorldSetup.cpp:1423` |
| 1.12 | box3d bodies (`PhysicsWorld`) | T1, exactly one `Step` per fixed tick | nothing reads transforms back | interface has no transform getter, deliberately | **stated** | `World/World.cpp:1963`; the rule is written at `World/Physics/PhysicsBackend.hpp:63-66`; three scoped readbacks only (`:83-86`, `GetBodyMotion`) |
| 1.13 | Physics **probes and wind** | T1, per **frame**, not per tick | — | — | stated | `World/World.cpp:899-901`, commented as deliberate |
| 1.14 | `GRandGen` **sequential** stream (`RandomValue()`, `Gauss`, `PlusMinus`, `SetSeed`) | first thread to use it — in practice T1 | none | none — it is one shared `mutable int _seed` | **checked** | guard `Random/randomGen.cpp:23-40`; now called from the draw as well as the seed, `randomGen.cpp:49`; rule `randomGen.hpp:28-59` |
| 1.15 | `GRandGen` / `Landscape::_randGen` **positional** (`RandomValue(x,z[,y])`) | anyone | anyone | pure — reads const tables only | stated | `Random/randomGen.hpp:66-68`; the worker-legal terrain call sites are `Landscape.cpp:1517-1520`, `LandscapeRender.cpp:1975-1991`, `LandSave.cpp:944-946` |
| 1.16 | Worker floating-point environment | T1 captures; each worker conforms itself | — | `EnsureFpEnvironmentMatchesMain()` per task | **checked** | `Foundation/Platform/FpEnvironment.hpp`, capture at `Foundation/Platform/FPUSetup.cpp:61`; see SIM-801 |

> **1.7 in words.** There is no secondary AI thread and there has not been one for as
> long as this tree has existed. Two files disable it with an explicit `#define
> BACKGROUND_AI 0`; `World.cpp` disables it by *never defining the macro at all*, so
> the preprocessor's undefined-identifier rule does the work. The two arrangements
> agree today, which is why nobody has noticed that the class the enabled arm
> instantiates has no definition in the repository.

---

## 2. Terrain, streaming and the asset banks

| # | State | Owner | Others | Boundary | Rule | Cited at |
|---|---|---|---|---|---|---|
| 2.1 | `LandSegment`s during an async fill | T2 mutates **pre-allocated** segments it exclusively owns (`FastCAlloc` is not thread-safe, so allocation stays on T1) | shared texture registration serialised by `GSegTextureRegMutex` | pass-4 commit + `CreateNearVBuffers` on T1 | stated | `World/Terrain/Landscape.cpp:1345-1352`, `LandscapeRender.cpp:48-53`; the only `ParallelFor` in the Poseidon library is `Landscape.cpp:1358` (SIM-806) |
| 2.2 | Streamed-object residency (`UpdateModernObjectResidency`) | T1, world-side | — | called from the simulation half of the frame | stated | `World/World.cpp:1518`; body `World/Terrain/LandSave.cpp:3100`. **Snapshot slice S4 is closed** — the renderer no longer calls it; `render-snapshot-design.md` §2 still says otherwise and is stale there |
| 2.3 | Model IR (parsed `Model::Model`) | T3 produces, T1 consumes | T1 polls lock-free `Query` (atomic states) | `Take()` under `Impl::mutex`; stale worlds discarded by `generation` | stated | `World/Terrain/ObjectStreamPrepare.cpp:83-92`, `.hpp:42-46` |
| 2.4 | **`ShapeAdapter::convertToLODShape`** | **T3 workers, by default** | T1 pre-resolves the bank tables and installs the result | `SubmitConvert` / `TakeConverted`, generation-checked | **CONTESTED — see below** | runs on the worker at `World/Terrain/ObjectStreamPrepare.cpp:198`; enabled by default at `World/Terrain/LandSave.cpp:3631-3636`; **the forbidden-set banner still calls it MAIN THREAD ONLY at `ObjectStreamPrepare.hpp:21-25`** |
| 2.5 | `AdapterBankTables` — the bank lookups 2.4 needs | T1 resolves, hands a `shared_ptr<const>` to T3 | T3 reads only | immutable after `BuildAdapterBankTables` | stated | `World/Terrain/LandSave.cpp:3646`; consumed `ObjectStreamPrepare.cpp:198` |
| 2.6 | `CompactBuffer` / `RString` / `RStringB` refcounts | any thread | any thread | `__atomic_*` on the count | stated | `Foundation/Containers/CompactBuf.hpp:38-50`. **This was a plain `int` until 2026-08-31** and is the write that corrupted the heap in SIM-807 |
| 2.7 | Prepared texture bytes (`PreparedTextureStore`) | T3 produces bytes, T1 uploads | — | one mutex; `Take` moves the vector out | stated | `Graphics/Textures/PreparedTextures.hpp:3-35` |
| 2.8 | `ShapeBank` + `AssetCache`, `TexMaterial` bank | T1 only | none | — | **checked** | `Asset/Cache/AssetCache.hpp:25` ("not thread-safe. Callers serialise"), `ObjectStreamPrepare.hpp:26-27`; guard now at `Graphics/Rendering/Shape/ShapeDraw.cpp` (`ShapeBank::NewFromModel`) |
| 2.9 | `GFileServer` / `FileCache` / `QFBank` (PBO reads) | T1 only — `FileCache` keeps an **unlocked** MRU array, `QFBank` compiles `MT_SAFE` out | workers must use the pure loose-file readers (`ModelCache::LoadLooseFile`, `ReadPAABlockChain`, `ReadEmatFileLoose`) | — | **checked** | `IO/FileServer.hpp:5-13`, `IO/Streams/QBStream.hpp:196`, `World/Model/ModelCache.hpp:24-39`; guard now at `IO/FileServer.cpp` (`FileCache::Find`) |
| 2.10 | `Pars` — the global config bank | T1, at world/mission load and shutdown | read everywhere | none | implicit | decl `IO/ParamFileExt.hpp:8`; cleared `Core/GameState.cpp:436`, `Foundation/Platform/Shutdown.cpp:164` |
| 2.11 | `ENGINE_CONFIG` | T1 | read | none | implicit | `Core/Config/EngineConfig.hpp:115` — it is a macro reaching through the global `GApp` |

> **2.4 is the contested row, and it is the most important line in this document.**
> `ObjectStreamPrepare.hpp:21-25` — the banner the *previous* version of this table
> cited as the authority for the whole worker forbidden set — lists
> `ShapeAdapter::convertToLODShape` under **MAIN THREAD ONLY**, with its reasons
> (global texture bank, `GTexMaterialBank.New`, recursive `ShapeBank::New` for
> proxies, `RStringB` property tables). Since 2026-08-31 the shipping default runs it
> on two worker threads. The arrangement is now *safe* — the bank lookups were lifted
> out into `AdapterBankTables`, resolved on T1 and passed in read-only (2.5), and the
> one genuine race left behind it (2.6) was found and fixed — and it is backed by the
> best evidence in this area of the tree: SIM-807's harness reports 30/30 bit-identical
> conversions across 1/2/4/8/16 threads. **The code is right and the rule is wrong.**
> That is the failure mode this whole document exists to prevent: a reader following
> the header would conclude a worker may not run the adapter, and would either
> re-serialise a path that costs 3+ s of cold admission or, worse, decide the shipping
> default is a bug and revert it. Fix the banner; do not fix the code.

---

## 3. Renderer, GPU resources and identity

| # | State | Owner | Others | Boundary | Rule | Cited at |
|---|---|---|---|---|---|---|
| 3.1 | `PresentationSnapshot` — the renderer's pull set | T1 publishes, T9 consumes (T1 with the render thread off) | — | two-slot ring, atomic index, release store | stated | `Graphics/Rendering/Frame/PresentationSnapshot.hpp:162-190`, `.cpp:36-49,274` |
| 3.2 | The **push** path — `_draws3d`, `_verts`, `_cmds`, `_palette`, `_cameras`, `_shadowCasters` | T1 clears in `InitDraw`, T1 appends during traversal | — | **none — not double-buffered** | implicit | `render-snapshot-design.md` §3b blocker 4. This is what step 6 must fix before a render thread exists |
| 3.3 | `GCurrentConformPlane` / `GCurrentIsVegetation` / `GCurrentFoliageKind` | T1, transient, save/restore nesting | — | scope discipline in the callers | implicit | decl `Graphics/Rendering/Shape/Shape.hpp:289,296,300`; defn `ShapeDraw.cpp:223-225`; save/restore `World/Scene/Object.cpp:506,531,1097-1141`, `ObjectClasses.cpp:476-480` |
| 3.4 | `GScene::_lights` | T1 | the snapshot carries a scored, capped copy by value | snapshot publish | stated | `World/Scene/Scene.hpp:191,281-284`; snapshot slice S3 |
| 3.5 | `_gpuInstances` / `_gpuProxies` (`unordered_map<const Object*, …>`) | T1 mutates **while** the liveness sweep walks it | — | **none** | implicit | `WgpuRenderer/EngineWgpu.hpp:670,695`; rolling cursor and its rehash restart at `:770-780`. REN-ID-001 calls this container ownership, not identity, and ties its fix to 3.2 |
| 3.6 | `Object::RenderId()` / `_renderId` | T1, lazily assigned in the draw path | — | monotonic, never recycled, dies with the object | stated | `World/Scene/Object.cpp:1744-1757`, `Object.hpp:284`; folded to 16 bits at `WgpuRenderer/EngineWgpu.cpp:6448` |
| 3.7 | Rust retained-instance slots | T9, through the queued instance ops; T1 directly only inside the producer window (row 3.10) | — | handle is `(slot+1) \| generation<<24`, checked on every op | **checked** (in Rust) | `WgpuRenderer/rust/src/gfx3d/cull.rs`; ABI contract `include/wgpu_renderer.hpp:1907-1917`; stale ops counted as `stale_instance_ops` |
| 3.8 | GPU geometry buffers | T9 (queued mesh ops); T1 uploads by the `wgr_uploader_*` borrow | — | `retire()` parks, `frame_submitted()` is the lifetime edge | stated | `rust/src/gfx3d/pool.rs:195-253` |
| 3.9 | GPU **textures** | T1 creates, leases and destroys — inside the producer window only; T9 draws | — | **no epoch** — destroyed immediately, bindless slot recycled at once; safe because the window keeps create/destroy/draw in frame order | **checked** — `ProducerMayTouchRenderer()` asserted at destroy and slot acquire (`WgpuRenderer/TextureWgpu.cpp`, logged then counted) | `rust/src/textures.rs`. The render-thread split this row said must bring textures up to 3.8's model first shipped on 2026-09-01 without doing so; REN-THR-012 (2026-09-02) closed the window instead of adding the epoch |
| 3.10 | The whole `wgr_*` ABI | **one thread at a time, in frame order**: T9 from publish until the worker is idle, T1 from `WaitRenderIdle` at `InitDraw` until publish | none | the producer window (`EngineWgpu::WaitRenderIdle`, `InitDraw`; closed at publish; rare direct callers — screenshot, resize, vsync, MSAA, shadow dumps, far instances — wait themselves) | stated for the ABI, **checked** on the texture paths (3.9) | contract rewritten 2026-09-02 (REN-THR-012): for one day after the render thread shipped it still read MAIN THREAD ONLY while T9 ran `wgr_render_frame` and T1 made direct calls under overlap. Phase 4.2 makes the Rust side check it |
| 3.11 | TaskPool submission | T1 only submits; captured state must outlive the job | — | `ParallelFor` blocks / `Submit` completion | stated | `Core/TaskPool.hpp:51-57,66-75` |
| 3.13 | `GEngine` — the engine singleton, the most-used global in the tree (123 files) | T1 creates it once (`apps/cwr/Game/GameApplication.cpp:2629`; dummies `apps/cwr/Server/ServerApplication.cpp:251`, `engine/PoseidonFormats/PoseidonFormats.cpp:107,122`) and never reassigns or deletes it | T9 reaches the same object through `this` inside `RunConsumerBlock` — the only non-T1 use of the object; none of the worker files (`ObjectStreamPrepare.cpp`, `SoundSystemOAL.cpp`, `NetPeer.cpp`, `WatchDog.cpp`) references the global | the POINTER is immutable after startup; the OBJECT's thread rule is row 3.10 | implicit | `Graphics/Core/Engine.cpp:22`, `Engine.hpp:2782` |
| 3.12 | Diagnostics singletons (`ObjectAdmitProfile`, `FrameProfiler`, light and snapshot gauges) | T1 (plain fields); streaming counters are atomics because alpha work can touch them off-thread | — | — | stated | `Graphics/Rendering/Shape/ObjectAdmitProfile.hpp:78`, `Dev/Diag/StreamingDiag.hpp`, `Dev/Diag/SnapshotDiag.hpp` |

---

## 4. Audio

| # | State | Owner | Others | Boundary | Rule | Cited at |
|---|---|---|---|---|---|---|
| 4.1 | OpenAL device / context / sources / buffers | T1 owns the context; T4 may only run the documented two-pass pump | T4 pass 1 under `_audioMutex`, pass 2 lock-free decode behind the `decodeInFlight` lifetime latch | `_audioMutex`; `~WaveOAL` spins on the latch | stated | `PoseidonOpenAL/SoundSystemOAL.cpp:234-256`, `WaveOAL.hpp:24-31`, `WaveOAL.cpp:429-434` |
| 4.2 | Decoded PCM (`PcmCache`) | T1 and T2 decode jobs; internally a mutex-guarded true LRU | any | its own mutex | stated | `Audio/Shared/PcmCache.hpp` |

---

## 5. Network authority

| # | State | Owner | Others | Boundary | Rule | Cited at |
|---|---|---|---|---|---|---|
| 5.1 | Socket state | T5, in the `PoCritical` idiom | T1 receives POD marshalling structs | `SendCompleteInfo` / `CreatePlayerInfo` / `DeletePlayerInfo` | stated | `Network/NetTransport.hpp:138,145,155` |
| 5.2 | **Who may mutate a networked entity** | the machine where `IsLocal()` is true | everyone, in practice | **none — every mutator is public and unguarded** | **UNENFORCED — see below** | decl `Network/NetworkObject.hpp` (`virtual bool IsLocal() const = 0`); flags `World/Entities/Vehicles/Vehicle.hpp:285`, `AI/AIUnit.hpp:539`; transfer `Network/NetworkClientOnMessage.cpp:402,454`; representative guards `Network/NetworkClientActions.cpp:274-278`, `AI/AIUnitImpl.cpp:80,225,827,885`, `World/Entities/Vehicles/Ground/Car.cpp:670,1395` |
| 5.3 | `NetworkId{creator, id}` | the creating client allocates | — | `_nextId++`, monotonic within a session | stated | alloc `Network/NetworkClientActions.cpp:149`; init `Network/NetworkMisc.cpp:707`; wire `Network/Network.cpp:416` |
| 5.4 | Static scene objects' network identity | derived from the **savegame** `_id`, not allocated | — | `SetNetworkId` is `Fail("Cannot set network id")` | stated | `World/Entities/Weapons/Dammage.cpp:698-711`; `STATIC_OBJECT` = 1, `Network/NetworkIface.hpp:17` |

> **5.2 in words.** Multiplayer authority is a `bool` that roughly 150 call sites must
> each remember to test. `Vehicle::SetTransform`, the damage setters and every other
> mutator are public and callable on a remote object with no check; the closest thing
> to a chokepoint (`NetworkClientActions.cpp:274-278`) only refuses to *send* an
> update that has already been applied locally. `Object::IsLocal()` returns `true`
> unconditionally (`Dammage.cpp:712-720`), so static objects are mutable everywhere by
> construction. Nothing in the type system distinguishes a locally-owned handle from a
> remote one. This is an accurate description, not a complaint: 4.1 asks the table to
> *identify* multiplayer authority, and the honest answer is that it is a convention.

---

## 6. Persistence

| # | State | Owner | Others | Boundary | Rule | Cited at |
|---|---|---|---|---|---|---|
| 6.1 | Save / load | T1, synchronously **inside the frame** (it pumps its own progress screen) | — | — | implicit | `World/World.cpp:274,292,313` inside `World::Simulate`; body `World/WorldImpl.cpp:1940,1995` |
| 6.2 | Persisted entity identity | `Object::_id`, an `int` slot index — and it is **renumbered at every save** | — | `ResetIDs()` + `RebuildIDCache()` before serializing | stated | write `World/Entities/Weapons/Dammage.cpp:637`, read `:671-679`; entities `World/Simulation/Simul.cpp:1439-1440`; cross-refs `World/Scene/Object.cpp:1759-1773`; renumber `World/WorldImpl.cpp:1664-1730,1968-1969`, `World/Terrain/LandSave.cpp:4059-4090`; high-water `LandSave.cpp:3989` |
| 6.3 | **Ephemeral handles must not become persisted identities** (§4.1's explicit requirement) | — | — | — | **verified clean** | `grep -rn "Serialize(.*RenderId\|Serialize(.*_renderId\|Serialize(.*gpuInstance\|Serialize(.*wgpu\|Serialize(.*alSource" engine/Poseidon/` returns **zero hits**; `grep -rln ParamArchive engine/PoseidonOpenAL engine/WgpuRenderer engine/PoseidonGL33` returns **zero files** — the backends have no serialization surface at all. See the caveat below |
| 6.4 | Savegame **version** | `WorldSerializeVersion = 13` | — | **none** | **UNENFORCED — see below** | constant `Core/SaveVersion.hpp:6`; written `IO/Serialization/ParamArchive.cpp:625-633` from `World/WorldImpl.cpp:1823,1874`; read `ParamArchive.cpp:590-593,604-612` |

> **6.4 in words.** The version number is written and then never compared against
> anything. `WorldSerializeVersion` appears in exactly three places and all three are
> writes. The only failure the reader can produce is `LSNoEntry` when the field is
> absent; **any value that parses is accepted, including one from the future.** Its
> sole consumption is per-field gating of the form `if (_version < minVersion) return
> LSOK;` (`ParamArchive.cpp:132,157,181,…`), which is exactly backwards for a
> newer-than-expected save: every gate opens, and fields that do not exist are read.
> Roadmap 4.1 says "persisted identities require an explicit versioned serialization
> contract". The serialization is versioned. The contract is not checked.

> **6.3's caveat.** No render or audio handle is serialized — that part is clean and
> was checked by grep, not assumed. But 6.2 and 5.4 are coupled in a way worth stating:
> a static object's `NetworkId` is derived from the same `_id` that `World::ResetIDs`
> renumbers at save time, so a save taken mid-session can change the value a live
> network identity was built from. That is not an ephemeral-handle leak, which is what
> §4.1 warns about — it is the mirror image, a persisted identity reaching into the
> network namespace — and it belongs on the Phase 11 list rather than this one.

---

## What is now checked, and why these

Four rows in the table say **checked**. Two guards were added by this pass
(row 2.8 and row 2.9, sharing one facility), one existing guard was extended to
cover the case it was written for (row 1.14), and one predates this work (1.16).

**The facility.** `Foundation/Threads/ThreadAffinity.{hpp,cpp}` —
`CaptureMainThread()` beside the existing `CaptureMainFpEnvironment()` in `InitFPU`
(`Foundation/Platform/FPUSetup.cpp`), and `POSEIDON_MAIN_THREAD_ONLY("…")` at a
guarded call site. It emits **one `LOG_WARN` per site**, ever. It is not a lock and
not an assert, and that is copied deliberately from
`RandomGenerator::NoteSequentialUse`: a mutex on one of these objects would make the
symptom disappear while leaving the ordering dependency in place, and an assert turns
a diagnosable warning into a crash in the build the owner is playing. Cost on the
owning thread is one relaxed atomic load and an integer compare. Tests:
`tests/unit/engine/Poseidon/Foundation/Threads/test_thread_affinity.cpp`, 7 cases,
tags `[threads][affinity]`, no game data.

**Why `FileCache::Find` (2.9).** It is the single funnel — the `Use`, `Store` and
eviction paths all reach the unlocked MRU through it — so one line covers every way a
worker could read a PBO-packed file through `GFileServer`. It is also the boundary
whose violation is *silent*: a worker racing `MoveToFront`'s array shuffle corrupts
memory and produces a crash somewhere else entirely, which is precisely the shape of
the bug SIM-807 spent a session chasing. The rule has been written in the header since
2026-08-30; this makes it audible.

**Why `ShapeBank::NewFromModel` (2.8).** It is the entry point row 2.4 is contested
about. The streamed-admission path now hands it worker-produced IR *from the main
thread*, and the difference between that and calling it *on* the worker is one
function call in one branch. A guard here is what keeps 2.4's arrangement honest as
Phase 5 and Phase 8 keep moving work outward. It also covers the `AssetCache` insert
and the vertex-buffer creation underneath it, so row 2.8 gets its check without
putting logging into a header-only template.

**Why the RNG one-liner (1.14).** The audit found that the repo's worked example of a
cheap enforced rule *did not cover the case it was written for*. `NoteSequentialUse`
was called only from `SetSeed`. A worker that reached the sequential stream would
overwhelmingly **draw** from it, not re-seed it — and `_seed++` in `RandomValue()` is
the unsynchronised read-modify-write that makes the draw a race. `Gauss` and
`PlusMinus` both route through `RandomValue()`, so one call at `randomGen.cpp:49`
closes all three sequential entry points named in the header. No new machinery, and
the guard now guards.

## Gaps this inventory exposes (the 4.1 work list)

Carried forward from the first pass, with three added:

1. **The forbidden-set rule is written only from T3's point of view.** Nothing tells a
   TaskPool job or the audio pump that `ShapeBank`/`QFBank`/`GWorld` are off-limits.
   Rows 2.8 and 2.9 now say it in the running program as well as here.
2. ~~`FileServer.hpp` advertised "suitable for background (multithreaded) usage"~~ —
   corrected 2026-08-30; row 2.9 now also checks it.
3. **The renderer ABI's thread contract is prose.** A line sits at the top of
   `wgpu_renderer.hpp`; Phase 4.2 turns it into a checked rule (row 3.10).
4. **Two synchronisation idioms coexist** (`std::mutex`/atomics vs. `PoCritical` in
   `NetPeer`). Phase 11 should not "unify" them casually — the network rows are correct
   as they are — but new code uses the modern idiom.
5. ~~Save/load and multiplayer authority are absent from every banner.~~ **Now audited
   — sections 5 and 6.** The finding is worse than "unstated": multiplayer authority is
   an unenforced bool (5.2) and the savegame version is written but never compared
   (6.4). Both need to be answered before Phase 4.1 can be called done; neither is a
   threading problem, which is why neither surfaced in the first pass.
6. **NEW — one stated rule is stale and contradicts shipping code** (row 2.4).
   `ObjectStreamPrepare.hpp:21-25` is the banner this document cited as its own source
   of truth for the worker forbidden set, and part of it is no longer true. Correcting
   it is a doc change, not a code change, and it belongs to whoever owns that header.
7. **NEW — the push path is not double-buffered** (row 3.2) and `_gpuInstances` is
   mutated while it is walked (row 3.5). These are the two container-ownership
   questions Phase 5 step 6 cannot start without; REN-ID-001 already ties them
   together and this table keeps them together.
9. **NEW (2026-09-02) — the render thread shipped a day before this table knew it.**
   Rows T1, 3.1 and 3.7–3.10 all said "T1 only" while T9 ran `wgr_render_frame` and the
   producer, under overlap, made direct `&mut` calls into the same renderer (terrain
   params, render params, sky runtime, far instances, texture leases). REN-THR-012 closed
   the window rather than adding locks; what it costs is the traversal's share of the
   overlap, and the way to win it back is the row-3.10 work list: queue the remaining
   producer-side calls as the instance and mesh ops already are. A table that is a day
   behind the code is the failure mode this document exists to prevent, so the render
   thread's commit should have edited it.
8. **NEW — an entire threading arm of the simulation is uncompilable dead code**
   (row 1.7). Deleting `BACKGROUND_AI` and `_secThread` would remove three `#if` arms
   from `World.cpp`, two from `WorldSetup.cpp` and one from `WorldInit.cpp`, and would
   remove the possibility of someone reading them as a design to revive.

## Deferred reclamation today (feeds Phase 4.3)

- **Geometry buffers have an epoch:** `rust/src/gfx3d/pool.rs:195-253` — `retire()`
  parks a buffer, `frame_submitted()` is the precise lifetime boundary (row 3.8).
- **Textures do not:** `rust/src/textures.rs:611-620` destroys immediately and recycles
  the bindless slot (row 3.9). Safe only because create/destroy/draw all run on T1 in
  frame order. Any off-thread upload or render-thread split must first bring the
  texture registry up to the pool's epoch model.
