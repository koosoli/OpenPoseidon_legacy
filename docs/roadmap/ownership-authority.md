# Ownership and authority table (roadmap Phase 4.1)

**Status:** first complete inventory, 2026-08-30, from code reading at `asset-obs-001`
(all citations verified at that commit). This is the document Phase 4.1 requires
before any subsystem is parallelised: who may mutate each class of state, from which
thread, and where the handoff back to the owner happens. The rules below are not
proposals — they are what the code does today, with the file:line that proves it.
Where a rule exists only as a header banner, this table cites it rather than
restating it, so the source of truth stays beside the code.

## Threads the process actually runs

| # | Thread | Created at | Count / priority |
|---|---|---|---|
| T1 | Main / render | OS | 1 |
| T2 | enkiTS TaskPool workers | `apps/cwr/GameBase/GameBase.cpp:124` | auto, cap 8 (`Core/TaskPool.hpp:107`), `--max-threads` |
| T3 | ObjectStreamPreparer workers | `World/Terrain/ObjectStreamPrepare.cpp:397-404` | 2 default (1–16), BELOW_NORMAL |
| T4 | Audio stream pump | `PoseidonOpenAL/SoundSystemOAL.cpp:206` | 1, ~10 ms cadence |
| T5 | Legacy UDP peer listen/send | `Network/Legacy/NetPeer.cpp:607,615` | 2 per peer, priority 2 |
| T6 | ~~FileServer~~ — **not a thread** | `IO/FileServer.cpp:19` compiles the async path out | — |
| T7 | Rust/wgpu | none spawned by our code; every `wgr_*` runs on the caller | — |
| T8 | Auxiliary (WatchDog, DownloadWorker, MasterServerBrowser, workshop fetch, dev harness) | see inventory notes below | occasional |

## Authority table

| State class | Owner (may mutate) | Others' access | Handoff boundary | Rule written at |
|---|---|---|---|---|
| `GWorld` / `GLandscape` / `GScene`, entities, AI | T1 only | T2 reads landscape grids treated as immutable during a `Fill` | `ParallelFor` barrier; pass-4 commit on T1 (`Landscape.cpp:1381-1398`) | implied; master list `ObjectStreamPrepare.hpp:16-34` |
| `ShapeBank` + `AssetCache`, `TexMaterial` bank | T1 only (`AssetCache` explicitly not thread-safe) | none | — | `Asset/Cache/AssetCache.hpp:25`, `ObjectStreamPrepare.hpp:26-27` |
| `GFileServer` / `FileCache` / `QFBank` (PBO reads) | T1 only — `FileCache` keeps an **unlocked** MRU array, `QFBank` compiles `MT_SAFE` out | workers must use the pure loose-file readers instead (`ModelCache::LoadLooseFile`, `ReadPAABlockChain`, `ReadEmatFileLoose`) | — | `ObjectStreamPrepare.hpp:29-33`, `IO/Streams/QBStream.hpp:196`, `World/Model/ModelCache.hpp:24-39` |
| GPU resources (`wgr_*` textures, meshes, models) | T1 only; "workers produce bytes; the main thread owns every GPU and bank object" | none | worker bytes cross via `PreparedTextureStore` (mutex + `Take`) | `Graphics/Textures/PreparedTextures.hpp:3-35` |
| Model IR (parsed `Model::Model`) | T3 produces, T1 consumes | T1 polls lock-free `Query` (atomic states) | `Take()` under `Impl::mutex`; stale worlds discarded by `generation` | `ObjectStreamPrepare.cpp:83-92`, `.hpp:42-46` |
| OpenAL device/context/sources/buffers | T1 (context owner); T4 may only run the documented two-pass pump | T4 pass 1 under `_audioMutex`, pass 2 lock-free decode with the `decodeInFlight` lifetime latch | `_audioMutex`; `~WaveOAL` spins on the latch | `SoundSystemOAL.cpp:234-256`, `WaveOAL.hpp:24-31`, `WaveOAL.cpp:429-434` |
| Decoded PCM (`PcmCache`) | T1, T2 (decode jobs) — internally mutex-guarded true LRU | any | its own mutex | `Audio/Shared/PcmCache.hpp` |
| Network socket state | T5, in `PoCritical` idiom (the one subsystem predating the modernised vocabulary) | T1 receives POD marshalling structs | `SendCompleteInfo` / `CreatePlayerInfo` / `DeletePlayerInfo` | `Network/NetTransport.hpp:138,145,155` |
| Terrain segments during async fill | T2 mutates **pre-allocated** `LandSegment`s it exclusively owns (`FastCAlloc` is not thread-safe → allocation stays on T1) | shared texture registration serialised by `GSegTextureRegMutex` | pass-4 commit + `CreateNearVBuffers` on T1 | `Landscape.cpp:1345-1352`, `LandscapeRender.cpp:48-53` |
| TaskPool submission | T1 only submits; captured state must outlive the job | — | `ParallelFor` blocks / `Submit` completion | `Core/TaskPool.hpp:51-57,66-75` |
| Diagnostics singletons (`ObjectAdmitProfile`, `FrameProfiler`, light gauges) | T1 only (plain fields); streaming counters are atomics because alpha work can touch them off-thread | — | — | `ObjectAdmitProfile.hpp:78`, `Dev/Diag/StreamingDiag.hpp` |

## Deferred reclamation today (feeds Phase 4.3)

- **Geometry buffers have an epoch:** `gfx3d/pool.rs:195-253` — `retire()` parks a
  buffer, `frame_submitted()` is the precise lifetime boundary.
- **Textures do not:** `textures.rs:611-620` destroys immediately and recycles the
  bindless slot. Safe only because create/destroy/draw all run on T1 in frame order.
  Any off-thread upload or render-thread split must first bring the texture registry
  up to the pool's epoch model.

## Gaps this inventory exposed (the 4.1 work list)

1. **The forbidden-set rule is written only from T3's point of view.** Nothing tells a
   TaskPool job or the audio pump that `ShapeBank`/`QFBank`/`GWorld` are off-limits —
   it holds by symmetry, not by statement. This table is now the symmetric statement.
2. **`FileServer.hpp` advertised "suitable for background (multithreaded) usage"**
   while `FileServer.cpp:19` compiles the async path out and the cache is unlocked.
   Corrected in the same commit as this document.
3. **The renderer ABI had no thread contract** — zero mentions of threading in
   `wgpu_renderer.hpp` despite being strictly main-thread-only. A contract line now
   sits at the top of the header; Phase 4.2 turns it into a checked rule.
4. **Two synchronisation idioms coexist** (`std::mutex`/atomics vs. `PoCritical` in
   `NetPeer`). Phase 11 work should not "unify" them casually — the network rows are
   correct as they are — but new code uses the modern idiom.
5. **Save/load and multiplayer authority are absent from every banner.** No ephemeral
   handle currently leaks into savegame/network identity (render/audio handles never
   serialise), but nothing *states* it; the Phase 4.1 gate requires that statement
   before stable handles are introduced.
