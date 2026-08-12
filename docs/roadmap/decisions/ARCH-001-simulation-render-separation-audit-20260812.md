# ARCH-001 — Should we separate simulation from rendering now?

**Date:** 2026-08-12
**Question from the owner:** is simulation/render coupling accruing debt that will be
much harder to unpick in a few months?
**Answer: no large refactor. The boundary already exists and the build already
enforces it. Four small, cheap things are worth doing now — and one of them is not
about rendering at all.**

This is an audit of the code as it stands, not a design opinion.

---

## 1. Coupling audit

### 1.1 The dependency direction is already correct, and the linker proves it

`apps/cwr/Server/CMakeLists.txt` links **`GameBase` + `Poseidon` and nothing else**.
No `PoseidonGL33`, no WGPU, no `PoseidonOpenAL`. The client
(`apps/cwr/Game/CMakeLists.txt`) adds all three. So the dedicated server already
builds and runs with **no graphics backend linked in at all** — a property the
build breaks on, not a convention someone has to remember.

Five binaries already run headless through the same seam
(`Poseidon::CreateEngineDummy()`):

| binary | site |
|---|---|
| dedicated server | `apps/cwr/Server/ServerApplication.cpp:251` |
| PoseidonFormats (Blender DLL) | `engine/PoseidonFormats/PoseidonFormats.cpp:107,122` |
| `poseidon` lister | `apps/tools/TcLister/tc_lister.cpp:109` |
| `pbo` | `apps/tools/TcPbo/tc_pbo.cpp:65` |
| PoseidonTools | `apps/tools/Tools/main.cpp:94` |

**Level 1 ("clean architectural interface") and level 5 ("optional headless mode")
of the owner's list are substantially DONE.** That is the single most important
finding here, and it changes the answer to the headline question.

### 1.2 Simulation never reads renderer state for gameplay — measured

40 of ~394 files in `World/`, `AI/`, `Game/`, `Network/` mention `GEngine` (~10%).
Classifying every `GEngine->` call site in those trees by method:

| what it is | calls | verdict |
|---|---:|---|
| HUD / text / 2D drawing (`DrawText`, `DrawLine`, `GetTextWidth`, `TextBank`, `Width`, `Height`, `LoadFont`, `ShowMessage`…) | ~200 | output-only, harmless |
| draw-queue control (`FlushQueues`, `InitDraw`, `FinishDraw`, `NextFrame`, `SetPassKindHint`) | ~40 | output-only |
| scene deltas (`SceneObjectCreated/Moved/Removed`) | ~10 | **the presentation channel — see 1.3** |
| `GetAccomodateEye` (13), `ProceduralSkyActive` (4) | 17 | inside draw code (`Object.cpp` colour paths, `SceneDraw.cpp`, `LandscapeRender.cpp`) |
| `CanGrass` (4) | 4 | draw-**pass ordering** only (`Man::PassNum`, `Object.cpp:816`) |
| `SamplePixel` (11), `GetFrameCounter` (4) | 15 | **test-only SQF commands** (`GameStateExtTest*.cpp`) — a deliberate diagnostic surface |

**No gameplay path reads renderer state.** The two calls that would have been
alarming — reading back rendered pixels and the render frame counter — exist only
in the scripted-test command set. This is the property that makes multiplayer
authority and determinism reachable, and it currently holds.

### 1.3 A push-based presentation interface already exists

`engine/Poseidon/Graphics/Core/Engine.hpp:418-420` declares three virtuals with
**empty default bodies**:

```cpp
virtual void SceneObjectCreated(Object* /*obj*/) {}
virtual void SceneObjectRemoved(Object* /*obj*/) {}
virtual void SceneObjectMoved  (Object* /*obj*/) {}
```

Simulation pushes (`Landscape.cpp:2131`, `:2162`, `:2255`, `LandSave.cpp:517`);
the WGPU backend consumes them into its retained GPU-driven instance set
(`EngineWgpu.cpp:3381`, `:3450`, `:3487` → `wgr_instance_add` / `wgr_instance_update`).
Because the defaults are no-ops, headless costs nothing.

This is a **delta-based presentation channel**, i.e. an incremental form of the
owner's "render snapshot". It is the right shape already: the renderer is told,
it does not reach in. The retained path now carries essentially the whole world —
the legacy per-draw path is down to roughly **7 sections per frame**.

### 1.4 What is actually still coupled — three things, all small

**(a) Render code is filed inside the simulation tree.** `World/Terrain/LandscapeRender.cpp`
(1,990 lines) and `World/Scene/SceneDraw.cpp` (2,322 lines) are pure render code
living under `World/` — 4,312 of `World/`'s 96,624 lines (**4.5%**). Plus
`Object::Draw(int, ClipFlags, const FrameBase&)` (`Object.hpp:422`) is a virtual
with **38 `::Draw(` definitions across 20 files**. This is the original OFP design
where entities draw themselves. It is legacy-in-practice (see 1.3) but it is the
thing that makes "renderer reads live simulation state" still true on that path.

**(b) The renderer writes back into simulation objects.** Three sites:
`EngineWgpu.cpp:3467`, `:3484`, `:3505` all call `obj->SetGpuCoverage(...)`. It is
render-only data parked on the sim entity, read only by the renderer — harmless
today, but it means an `Object` differs between a rendering client and a headless
server. **This is the one inversion that must not be allowed to grow.**

**(c) Sim and render ticks are welded 1:1.** `engine/Poseidon/Core/Game/GameLoop.cpp:60`:

```cpp
void RenderFrame(float deltaT, bool enableDraw)
{
    ...
    GWorld->SetSimulationFocus(enableDraw);
    GWorld->Simulate(deltaT, enableDraw);
```

One function, one variable `deltaT`, and window focus threaded into the
simulation. `_simulationFocus` gates whether the world simulates at all
(`WorldImpl.cpp:213`) — classic single-player pause semantics, not a behaviour
modifier, but it is presentation state reaching into the tick.

### 1.5 The finding that matters most, and it is not about rendering

**There is no fixed timestep anywhere.** `World::Simulate(float deltaT, bool&)`
(`World.cpp:124`) takes a variable frame delta, and a search for an accumulator /
fixed-step loop across `engine/Poseidon/` and `apps/` returns nothing.

So of the owner's goals:

> deterministic simulation; replay/rollback; multiplayer authority; deterministic
> ballistics with wind/G1/G7

**none of these are blocked by simulation/render coupling. They are blocked by the
variable timestep.** Separating the renderer would not deliver a single one of
them. A fixed-step accumulator would deliver most of them, and it is independent
of the render boundary.

This is the correction I would most want the owner to take from this audit.

---

## 2. Goal-by-goal: what separation would actually buy

| goal | blocked by render coupling? | what actually unlocks it |
|---|---|---|
| dedicated/headless server | **No — already works** | nothing; it ships today |
| multiplayer authority | No | fixed timestep + existing `Network/` authority model |
| deterministic simulation | **No** | **fixed timestep**, float discipline, seeded RNG |
| save/load, late join | No | serialisation coverage of entity state |
| replay / rollback / debugging | No | fixed timestep + input capture |
| Jolt / Box2D physics | No | a physics abstraction; sim-side, renderer irrelevant |
| deterministic ballistics (wind/G1/G7) | No | fixed timestep + a sim subsystem; do **not** make these rigid bodies |
| Zeus / Game Master | No | it is a Dev Tools + command surface, sim-side |
| AI-agent automated testing | **Partly yes** | headless already works; the win is a *snapshot to assert on* |
| renderer replacement / experimentation | **Already true** | GL33 + WGPU + Dummy behind `GraphicsEngineFactory` |
| OFP/A1/A2/A3 compatibility | **No — actively helped by leaving it alone** | format readers; see §5 |

Only two entries genuinely benefit: **AI-agent testing** (wants a stable
snapshot to assert against) and, later, **independent render rate / interpolation**
(a look-and-feel win, not a correctness one).

---

## 3. Recommended target architecture

The owner's proposed shape is broadly right, but the code says it is **already
most of the way there**, and the honest target is to *finish and defend* what
exists rather than to build a parallel structure:

```
Simulation (Poseidon: World/ AI/ Game/ Network/)
    │   pushes deltas — never asks the renderer anything
    ▼
Engine  (abstract interface, engine/Poseidon/Graphics/Core/Engine.hpp)
    │   SceneObjectCreated / Moved / Removed  (+ terrain, weather, effects)
    ▼
Backends:  EngineWgpu   |   EngineGL33   |   EngineDummy (headless)
```

Two deliberate departures from the proposal:

1. **Do not build a versioned immutable snapshot yet.** The delta channel is
   cheaper, already written, already carries the world, and is what a retained GPU
   renderer wants anyway. A full per-frame snapshot buys interpolation and
   threading — neither of which is on the critical path — at the cost of copying
   the world every frame.
2. **Keep ballistics as a simulation subsystem, not rigid bodies.** The owner's
   instinct is right and nothing in the code argues against it.

---

## 4. The minimum boundary to introduce now

Four items. None is a refactor; together they cost days, not months.

**A. An architecture guard test.** The valuable property — simulation cannot reach
a concrete backend — currently holds *by accident of the link graph*. Make it a
test that fails loudly: assert `PoseidonServer` links no graphics/audio backend,
and assert no file under `World/`, `AI/`, `Game/`, `Network/` includes a WGPU or
GL33 header. This is the whole answer to "will it get harder later": it stops the
property from silently eroding while everything else grows. **This is the single
highest-value item in this document.**

**B. Close the render→sim write-back.** Move `SetGpuCoverage` off `Object` into
the renderer's own `_gpuInstances` map (which already exists and is already keyed
by the object). Three call sites. After this, the renderer writes nothing into
simulation state, and that becomes a rule the guard test can enforce too.

**C. A fixed-step simulation accumulator.** Split `RenderFrame` into
`StepSimulation(fixedDt)` (looped by an accumulator) and `RenderFrame(alpha)`.
This is the real unlock for determinism, rollback, replay and MP authority, and it
is where the owner's stated goals actually live. It is also the one item that gets
materially harder as more systems are added, because every new subsystem written
against a variable `deltaT` is another thing to convert.

**D. A rule for new features, written down.** New world/render features go through
the `Engine` interface and the push channel; nothing new gets a `GEngine->` call in
`World/`/`AI/`, and nothing new stores render state on a sim entity.

Explicitly **not now**: immutable snapshots, separate threads, separate processes.
Revisit when interpolation or a render thread is actually wanted.

---

## 5. Why waiting is NOT dangerous here (the honest counter-argument)

The owner's fear — "separating later may become much harder" — is the right fear
in general and mostly does not apply here, for a measurable reason: **the growth
happening right now is on the correct side of the boundary.**

Everything added in this session — the LCA mask decode, the six-slot surface
stack, the OPRW readers, the terrain material IR, A1/A2/A3 compatibility — is
either format-reading (sim side, renderer-agnostic) or lives entirely inside
`engine/WgpuRenderer/`. The one exception is instructive: `A3TerrainMaterial`
(render description) is stored on `Landscape` (a sim class) and read by
`TerrainWgpu`. That is data-only, flows in the correct direction, and costs
nothing to leave.

The parts of the roadmap that *would* accrue real coupling debt are the ones not
started yet — physics, Zeus, replay. They are exactly the ones item **C** protects.

---

## 6. Roadmap items that should move earlier

1. **Fixed-step accumulator (C)** — before Jolt/Box2D and before the new ballistics
   system. Both would otherwise be written against a variable delta and need
   redoing.
2. **Architecture guard test (A)** — before the next wave of features, because its
   entire value is preventive.
3. Nothing else needs to move. Terrain/vegetation/compatibility work is unaffected
   and should continue.

---

## 7. Cost of doing this now vs in a few months

- **A + B + D:** a few days, and the cost is flat over time — they are guards, not
  migrations.
- **C (fixed timestep):** cheap now, and grows roughly linearly with the number of
  subsystems written against variable `deltaT`. Physics and ballistics are the two
  big ones and neither exists yet. **This is the only item with a real clock on
  it.**
- **Full snapshot/threading:** expensive now, and *not obviously more* expensive
  later, because the delta channel it would build on is already in place and is
  itself getting cleaner as the retained path absorbs the direct path.

So: doing the small items now is materially cheaper. Doing the big one now is not.

---

## 8. Verdict

**No, do not do the large separation now.** The premise it rests on — that
simulation and rendering are tightly coupled — is not what the code shows. The
dependency direction is correct, the build enforces it, five binaries already run
headless, gameplay never reads renderer state, and a push-based presentation
channel already carries the world.

**Do the four small items.** Then continue feature work.

**First narrow slice: the architecture guard test (A).** It is small, entirely
reversible, adds no runtime code, cannot regress OFP/A2/A3 behaviour, and directly
answers the question that prompted this audit — it converts "we currently happen
to be separated" into "we cannot silently stop being separated". Item **C** is the
one to schedule next and before physics.

## Constraints check

- OFP behaviour: untouched (no runtime change in A, B is renderer-internal).
- A2/OA/A3 compatibility: untouched.
- WGPU primary, GL33 possible: unchanged — both already sit behind the factory.
- MP correctness: improved by C, unaffected by the rest.
- Zeus from Dev Tools: unaffected (sim-side).
- Physics replaceable: C is the precondition; the abstraction itself is a later,
  sim-side decision.
- Ballistics deterministic, not rigid bodies: endorsed.
- Renderer may interpolate but not be authoritative: already true, and A keeps it
  true.
