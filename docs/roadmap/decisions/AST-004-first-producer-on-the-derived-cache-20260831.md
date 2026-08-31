# AST-004 — The first producer on the derived data cache: the one where the arithmetic works

**Date:** 2026-08-31
**Branch:** `agent-ddc-wiring` (from `asset-obs-001`)
**Roadmap item:** the consumer AST-002 §5 and AST-003 §7 both named as "a separate decision"
**Scope:** measure four candidate producers, pick one on the numbers, write the serialisation it
needed, wire it end to end, prove it, ablate it, call `Prune()`. **Nothing was deployed and the
game was never launched** — another session owned the game folder throughout. §7 says what that
leaves unproven.

---

## Verdict first

**The producer is P3D → `Model::Model` IR, at `ModelCache`.** It is the only one of the four
whose derivation costs more than the SHA-256 that keys it, by 21x; the other three cost *less*
than their own key and caching them is a guaranteed loss at any quality of implementation.

**The win is real and modest: ~1.9x on model load** (448 retail models, cold 1.87–2.10 s vs warm
0.85–1.06 s), for 57.8 MiB of disk against 27.0 MiB of source. Not the 21x the derivation ratio
suggests, because the warm path pays two SHA-256 passes of its own; §4 accounts for the gap.
**The cache therefore ships DEFAULT OFF** (`POSEIDON_MODEL_DDC=1`), for the reason in §5.

---

## 1. Measuring before choosing

AST-002 built a store with no customer; AST-003 built a per-member content hash with no caller.
Both explicitly deferred "which producer". The temptation is to pick the one that is easiest to
wire. The four candidates were therefore run over a real archive first —
retail `AddOns\O.pbo`, 1,703 members, RelWithDebInfo, this workstation, warm page cache
(`[ast004cost]` in `tests/unit/engine/Poseidon/Asset/Cache/test_ast004_producer_cost.cpp`).

The column that decides it is not the derivation cost on its own. **It is the derivation cost
against the cost of the key**, because a cache hit cannot be cheaper than hashing the input that
names it. AST-003 measured SHA-256 here at 210–245 MB/s, so that number is in the table.

| Candidate | n | input | **per item** | vs the key | what a hit must store |
|---|---:|---:|---:|---:|---:|
| **sha256 of the member — the key's own cost** | 1,505 | 142.09 MiB | **0.4497 ms** | 1.00x | 64 B |
| A: PAA → GPU block chain (`ReadPAABlockChainBuffer`) | 887 | 106.28 MiB | 0.0185 ms | **0.04x** | 111,305,456 B |
| B: PAA → full RGBA8 decode (`DecodePAABuffer`) | 1,056 | 115.10 MiB | 0.2786 ms | **0.62x** | 709,518,592 B |
| C: PAA decode + alpha classification (`ClassifyAlpha`) | 1,056 | 115.10 MiB | 0.4679 ms | **1.04x** | 50,688 B |
| **D: P3D → `Model::Model` IR (`ModelCache::LoadLooseFile`)** | 449 | 26.99 MiB | **9.6164 ms** | **21.4x** | *no serialisation existed* |

### The three rejections, each on its own number

**A — PAA block chain. Rejected: 24x cheaper than its key, and the output is larger than the
input.** This is the streaming preparer's stage 2 and it is already close to a memcpy — the mip
chain is stored in DXT form and the only work is an LZO inflate on levels that have one. Caching
it would replace an 0.0185 ms decode with a 0.4497 ms hash. It is not a marginal call; it is
25x the wrong way round.

**B — full RGBA8 decode. Rejected: below the key cost, and a 6.2x size expansion.** 115 MiB of
PAA becomes 709 MiB of RGBA. Even if the derivation had been expensive, storing an uncompressed
decode of every texture is a cache that costs more disk than the game.

**C — alpha classification. Rejected on the closest margin, and it is the interesting one.**
The output is 48 bytes for a 109 KiB average input — a 2,300:1 compression, the best shape of
any candidate, and it is a derivation the renderer really does perform per texture at load
(`TextureWgpu::GetAlphaClass`). But decode-plus-histogram measures **0.4679 ms against a
0.4497 ms key**: the cache would spend 96% of the work it saves on deciding what to look up.
There is no implementation quality that fixes that. Worth recording because it is the candidate a
"which output is smallest and most stable" argument picks, and the arithmetic says no.

*(Caveat on C, stated because it is the one number here that could be argued: the engine's real
fast path is `ClassifyAlphaFromBlocks`, which expands only the alpha lane of the top mip and is
cheaper than the full decode measured above. That makes C **worse**, not better, as a cache
candidate — the measurement above is already the most favourable version of it.)*

**D — P3D → Model IR. Chosen.** 9.62 ms per model, 21x its key. It was also the only candidate
with **no serialisation at all**, which is why it is not the one an easy-wiring argument would
have picked. Writing `ModelBlob` was the cost of choosing on the numbers.

### Two candidates the brief named that the corpus disposed of first

**ParamFile / the config preprocessor.** A census of the whole CWA retail corpus (44 archives,
13,935 members) by extension:

| ext | members | bytes |
|---|---:|---:|
| `.pac` | 4,208 | 183.8 MiB |
| `.wss` | 4,117 | 44.4 MiB |
| `.paa` | 2,895 | 75.4 MiB |
| `.p3d` | 1,400 | 66.1 MiB |
| `.rtm` | 739 | 16.6 MiB |
| … | | |
| `.bin` (rapified config) | **27** | 0.1 MiB |
| `.cpp` (text config) | **5** | 0.0 MiB |

**Five text configs in the entire primary corpus.** The expensive path (preprocess + parse +
evaluator) is the one that almost never runs; addons ship `config.bin` and take `ParseBin`.
`ParamFile` has the best in-tree round trip of the four (`SerializeBin` / `ParseBin` / `SaveBin`,
and `World.cpp:353` already writes a derived `config.bin`), and none of that matters against a
denominator of five. It also has the worst input closure: `#include` makes the key a transitive
set, not one member.

**RVMAT material compilation.** `ParseRvMaterialFile` → `TexMaterial` produces about twenty
floats from a few-KB file, and there are **zero `.rvmat` members in the CWA retail corpus** —
it is an Arma-2-and-later material form. Small output, cheap derivation, no serialisation, and
AST-002 had already parked material interpretation on the roadmap's own measurement (95% of a
28.7 ms frame is `swap`; all main-thread CPU work sums to ~1.5 ms). Nothing was measured here
that changes that.

---

## 2. What was built

| File | What |
|---|---|
| `engine/Poseidon/World/Model/ModelBlob.{hpp,cpp}` | The serialisation Model IR did not have. ~840 lines, almost all of it field-by-field read/write pairs. |
| `engine/Poseidon/World/Model/ModelDerivedCache.{hpp,cpp}` | Key construction, `Load`/`Store`/`Prune`, stats, the env switches. |
| `engine/Poseidon/World/Model/ModelCache.cpp` | The seam. Both entry points now route through one `DeriveOrHit`. |
| `engine/Poseidon/World/WorldInit.cpp` | `Prune()` at the top of `World::InitLandscape`. |
| `tests/.../Asset/Cache/test_ast004_model_ddc.cpp` | 11 cases, 3,470 assertions, plus a hidden benchmark. |
| `tests/.../Asset/Cache/test_ast004_producer_cost.cpp` | The hidden `[ast004cost]` case that produced §1's table. |

### The seam

`ModelCache` had two entry points that both ended in the same three steps — source bytes, a
loader, `compile()`. `LoadLooseFile` reached them via `ODOLLoader::load(path)`, which slurps the
file itself; `loadFromFileServer` via `loadFromBuffer` on the file server's buffer. They are now
one function, `DeriveOrHit(data, size, path, signature, contentSha, error)`, with the cache
around it. `LoadLooseFile` reads the file once and passes the bytes down — the same bytes by the
same route the loader used to fetch for itself, so the restructure also removes a duplicate read.

**Only a model that passed `compile()` is stored**, so a hit is never a shortcut past a validity
check the cold path applies.

### The key, input by input

| Part | Why it must be there |
|---|---|
| role `"model"` + canonical `VirtualPath` | The path is an **input**, not a label: the loaders write it into `Model::sourcePath`. Two identical files under two names are two different IRs. |
| the member's SHA-256 | AST-003's `QFBank::GetContentHash` on the bank path (through its memo), `Sha256::Of` on the bytes for a loose file. Content, so an identical rebuild does not invalidate and a real edit does. |
| option `modelProducer` | The version of the P3D loaders + `Model::compile`. Without it, fixing a loader bug leaves every machine that cached the broken IR still serving it. |
| option `blobFormat` | `ModelBlob::kFormatVersion`. Separate axis: the loaders and the envelope change independently, and collapsing them either wipes every entry for a field reorder or reinterprets old offsets as new ones. |

`DerivedAssetKey::kCompilerVersion` (engine-wide) and `DerivedBlobStore::kContainerVersion`
(the entry envelope) are the third and fourth axes and are unchanged.

**Checked, not assumed, for what is NOT in the key:** `grep getenv` over
`engine/Poseidon/Asset/Formats/P3D/` and `engine/Poseidon/World/Model/` finds nothing outside
`ShapeAdapter.cpp`, which runs *after* this seam and is not cached. The P3D loaders are a pure
function of their bytes and their path today. If that stops being true, the new switch is an
option or the cache is wrong.

**An empty content hash is "do not cache", not a key.** `HashBankMember` returns `""` for a path
the banks do not name; `DeriveOrHit` reads that as a reason to derive without touching the store,
because a key missing an input is exactly the wrong-data hit this design exists to prevent.

### `ModelBlob`, and the one rule that mattered

Every field is written **explicitly**, never by `memcpy` of a struct. `Vertex`, `Triangle` and
friends contain padding, and padding is whatever the allocator last left there — a memcpy
serialisation puts uninitialised memory in the payload, so two runs over identical input produce
different bytes and the "cold and warm are byte-identical" test ends up testing the allocator.
It also makes the format independent of the compiler's layout.

The reader believes no count until it has checked the smallest possible element still fits in the
bytes remaining, and a blob that decodes but does not **end** where the writer stopped is
rejected. Trailing bytes are as much a sign of a wrong reading as missing ones.

`LODLevel::sourceWinding` is written out rather than recomputed from the typed basis via
`DescribeWinding`, even though it is derivable. Recomputing would let the cache quietly *repair*
an IR whose loader set it to something else — a difference between a cold load and a warm one,
which is the one thing a cache must never have.

---

## 3. The proof

11 cases, `[ast004]`, `PoseidonCoreTests`.

| Group | Case | What it pins |
|---|---|---|
| Format | serialise → deserialise → re-serialise is byte-identical | the round trip is a fixed point |
| Format | **the serialised form of two fixtures is pinned by digest** | no field can be added, dropped, reordered or rewidened silently |
| Format | a truncated blob is refused **at every length** | no count is believed before it is checked |
| Format | wrong magic / wrong format version / trailing junk are refused | |
| Cache | **a cold run and a warm run produce byte-identical output** | plus miss-then-hit, and a hit does not rewrite |
| Cache | changing the input content changes the key and misses | |
| Cache | the same bytes under two names are two entries | the path is an input |
| Cache | changing an option changes the key and misses | and `MakeKey`'s option set is pinned |
| Cache | **a corrupted entry is a miss, not a wrong answer** | payload damaged in place, at the same length |
| Cache | it is off unless it is switched on | a disabled cache creates nothing and counts nothing |
| Cache | `Prune` evicts to the budget, and a disabled cache prunes nothing | |

The byte-identity assertions are literal: the model returned from a cache hit is re-serialised
and required to equal the blob the cold derivation produced. Comparing a handful of fields would
pass with any number of fields silently dropped.

---

## 4. The ablations, including the one that did not fail

Four, each by breaking exactly one thing, rebuilding, running `[ast004]`, and reverting.

**A — drop the path from the key** (`VirtualPath::Parse("")`).
**1 case fails:** *the same bytes under two names are two entries* — `REQUIRE(hits == 0)`.
That is the correct and only failure: with the path gone, the second model is served the first's
`sourcePath`. A wrong answer, not a stale one.

**B — drop the content hash** (a constant in its place).
**1 case fails:** *changing the input content changes the key and misses*. The edited file is
served the unedited model.

**C — drop the `blobFormat` option. THIS ONE DID NOT FAIL, and that was the finding.**
All 11 cases still passed. The option test as first written built two keys by hand and compared
them to each other, so it proved `DerivedAssetKey` honours options — which AST-020 already
proves — and proved nothing about what `MakeKey` puts in. **An option silently missing from the
real key is precisely the failure mode this design exists to prevent, and the test could not see
it.** Fixed by comparing the real `MakeKey` output against a written-out expectation of its whole
input and option set. Re-ablated: **1 case fails**, *changing an option changes the key and
misses*. This is why the instruction to ablate is worth following even when the tests are green.

**D — drop a field (`Vertex::binormal`) from BOTH the writer and the reader.**
**1 case fails:** *the serialised form of two fixtures is pinned by digest*.
Note which case did **not** fail: the byte-identical round trip. A field dropped from both halves
is still a fixed point, so the round-trip test is structurally blind to it. That hole is the
entire reason the digest pin exists, and this ablation is the measurement that shows the pin is
carrying weight rather than restating the round trip. It is deliberately brittle: when it fails
for an intended change, the fix is to bump `kFormatVersion` **and then** paste the new digest.

All four reverted; suite green.

---

## 5. The win, measured, and why it is 1.9x and not 21x

`[ast004bench]`, five independent process runs over the 448 loadable `.p3d` members of retail
`AddOns\O.pbo`, spilled to loose files. Each run is a fresh cache root: pass 0 derives and
stores, passes 1 and 2 hit.

| run | cold (ms) | warm 1 (ms) | warm 2 (ms) | speedup |
|---|---:|---:|---:|---:|
| 1 | 7894.2 | 876.1 | 810.3 | 9.01x |
| 2 | 2095.2 | 1146.5 | 1006.1 | 1.83x |
| 3 | 1869.6 | 1011.4 | 897.5 | 1.85x |
| 4 | 2036.8 | 1001.8 | 851.7 | 2.03x |
| 5 | 1927.4 | 1055.8 | 967.0 | 1.83x |

**Run 1 is discarded and named rather than averaged in.** Its cold pass is 3.8x every other
run's over identical work; the files had just been written to disk and the plausible explanation
is first-touch (page cache, on-access scanning). Quoting the 9.01x would have been the single
most flattering number available and it is not reproducible. **The honest headline is the
1.83–2.03x that four consecutive runs agree on.**

- **cold:** 4.17–4.68 ms per model
- **warm:** 1.81–2.36 ms per model
- **saved:** ~1.0 s per 448 models, i.e. ~2.2 ms per model
- **stored:** 57.76 MiB of entries for 26.96 MiB of source — **2.14x expansion**

### Where the other 19x went

The derivation is 21x the key. The end-to-end win is 1.9x. The gap is not mysterious and it is
not implementation slop — it is the two hashes a verified cache is *made of*:

| warm-pass component | bytes | at ~210 MB/s | share of ~950 ms |
|---|---:|---:|---:|
| SHA-256 of each source `.p3d` (to build the key) | 26.96 MiB | ~128 ms | 13% |
| SHA-256 of each entry's payload (the store's own verify) | 57.76 MiB | ~275 ms | 29% |
| reading 57.76 MiB of entries + `Deserialize` allocations | | remainder | 58% |

**Roughly 42% of the warm path is hashing**, and it is hashing more bytes than the source
contains, because the blob is 2.14x the `.p3d`. That is the structural cost of AST-002's two
correctness properties — a key built from content, and a payload that cannot be trusted without
its digest — and it is the correct trade: the alternative is sky-bake's, where an entry is
believed because it is at the right path. It is also why the §1 table's "vs the key" column is
the right screen and not a pedantic one.

**Is it worth it?** On CWA retail, marginally: ~1 s off a 448-model load, at 58 MiB. The case it
was designed for is the one AST-002 named — a foreign corpus where the derivation is far more
than reading an already-binarised ODOL. That workload was not run here and this document does
not claim it.

---

## 6. `Prune()`, and the budget

`ModelDerivedCache::Instance().Prune()` is called at the **top of `World::InitLandscape`** —
once per level change, before the new world's models are loaded.

**Before the fill, not after**, deliberately: an eviction pass at the end of a load would be
sorting entries whose mtimes are all seconds old and would throw away exactly what is about to be
asked for again. At the start, the coldest entries are the previous world's, which is what
oldest-mtime-first should be evicting.

**Not on every `Put`**, because `Prune` walks and sorts the whole tree, and a directory
enumeration in the middle of a model load is the wrong place to pay for it. That is AST-002's own
argument, now with a call site.

**Budget: 512 MiB**, sized on the measurement rather than picked round. At the measured 2.14x
expansion the entire CWA retail model corpus (1,400 members, 66.1 MiB) caches to ~142 MiB, so
512 MiB holds the primary corpus more than three times over and only begins evicting on a
foreign, Arma-scale one — which is exactly the case that must not be allowed to fill a disk.
`POSEIDON_MODEL_DDC_BUDGET_MB` overrides it.

**The expansion is the reason there is a budget at all.** The store is *bigger* than the assets
it derives from; an unbounded one grows past the game install.

---

## 7. What this does NOT prove

- **Nothing about the running game.** No binary was deployed and the game was never launched:
  another session owned `D:\SteamLibrary\...\ARMA Cold War Assault` for the whole of this work.
  Every number here is from `PoseidonCoreTests` against retail archives and loose `.p3d` files.
  **No world has been loaded with this cache enabled.** That is the reason it ships default off
  (`POSEIDON_MODEL_DDC=1` to enable): the switch turns a decision that currently needs a rebuild
  into one someone can make after a capture.
- **Nothing about `ShapeBank` or `LODShape`.** The cache stops at the Model IR.
  `ShapeAdapter::convertToLODShape`, proxy resolution, material and texture binding and
  `OptimizeOneShape` all still run on every load, hit or miss. A hit skips the parse, not the
  conversion — which is also why the input closure stays at one file and the key stays honest.
- **Nothing about threads.** `ModelDerivedCache` has no lock and `QFBank` has none either
  (AST-003 §7). `LoadLooseFile` is called from the object-stream workers; the store's own
  guarantee (immutable entries, per-writer temp, atomic rename) covers concurrent *writers of the
  same key*, but the singleton's `Stats` counters are plain `uint64_t` and will race. They are
  diagnostics, not a mechanism, and no correctness result above depends on them.
- **Nothing about a corpus other than CWA retail.** The 2.14x expansion, the 4.2 ms cold and the
  1.9x speedup are all `AddOns\O.pbo`. An Arma 2/3 or Reforger corpus has different model sizes,
  different ODOL revisions and a different derivation cost, and could move any of the three.
- **Nothing about the sky-bake cache.** Untouched, still keyed on a 64-bit FNV-1a of a lowered
  model name plus bbox bits, still in Rust, still explicitly out of scope.
- **Nothing about collisions.** SHA-256 is assumed, as in AST-002 and AST-003.
- **That candidate C is dead.** Alpha classification measured *at* the key cost, not below it. If
  its derivation ever gets more expensive — or if the classification is ever wanted without a
  decode already happening — the arithmetic changes. It was rejected on a 4% margin, and that is
  a much weaker rejection than A's 96%.

---

## 8. Suites

Run from this worktree's own build directory, `build/win-x64-clang-rwdi`.

| Suite | Before | After |
|---|---|---|
| `PoseidonTests` | 3,093 cases / 389,191 assertions / 0 failures | **3,093 / 389,191 / 0** (untouched) |
| `PoseidonCoreTests` | 827 / 6,341 / 0 | **838 / 9,811 / 0** |
| `PoseidonFoundationTests` | 491 cases / 0 failures | **491 / 0** |

`PoseidonCoreTests` gains 11 cases; the assertion jump is mostly the truncation case, which
refuses the blob at every one of its ~3,200 possible prefix lengths.
`PoseidonFoundationTests` reports a different assertion total per run (randomness-seeded cases);
the case count is the stable one and is what is quoted, as in AST-003.

The two hidden cases (`[.][ast004cost]`, `[.][ast004bench]`) do not run in the default suite and
move no count.

---

## Addendum — measured in the running game, and it does not pay on Abel

AST-004 shipped default-off because nothing had ever loaded a world with the cache on; the
document said so and asked for exactly this. It has now been run, on the `perf_abel`
mission, by the session that owns the game folder.

**The cache works.** With `POSEIDON_MODEL_DDC=1` a cold run populates
`%LOCALAPPDATA%\OpenPoseidon\derived` with **329 entries, 18 MB**, and the world is
identical either way: **98,684 registered instances in every arm**, cache on or off, cold or
warm. Nothing is lost or wrongly reconstructed.

**It also does not make the mission load faster.** Time from process start to the first
frame of scene content, six runs:

| | run 1 | run 2 |
|---|---:|---:|
| cache ON, cold store | 2.99 s | 2.26 s |
| cache ON, warm store | 2.35 s | 2.26 s |
| cache OFF | 2.52 s | 2.28 s |

Every arm lands in the same 2.26–2.99 s band and the three second runs are within 0.09 s of
each other. **There is no measurable win**, and the one figure that looks like one — the
2.99 s cold run — is the OS file cache being cold on the first launch after a rebuild. That
is the same trap this document already records for its own bench run 1, which is worth
noting: the flattering number reappeared in a different arm and had to be discarded twice.

Why, given the 1.9x measured over 448 models: Abel's mission does not load enough distinct
models for the P3D parse to be the thing the load is waiting on. The 21x figure that
selected this producer is a per-model ratio and remains true; it simply is not multiplied by
a large enough count here to show through the rest of the load.

**Default-off stays, and is now the measured position rather than the cautious one.** What
would change it is a world where the model count is large — Everon or an Arma 2/3 corpus,
where AST-004's own "left undone" list already says the expansion ratio and the speedup are
unmeasured. This addendum is one mission on one world and does not speak for those.
