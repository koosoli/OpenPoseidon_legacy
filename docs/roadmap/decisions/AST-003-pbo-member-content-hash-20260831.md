# AST-003 — Per-member content hashes for PBO archives: the producer half of the derived key

**Date:** 2026-08-31
**Branch:** `agent-pack-hashes` (from `asset-obs-001`)
**Roadmap item:** the producer-side gap named at the end of AST-002
**Scope:** audit of the PBO format and this reader, one design decision, `QFBank::GetContentHash`,
six tests, two ablations, and a cost measurement. **No consumer is wired to it in this pass**,
and the sky-bake cache is **not** ported. Both are deliberate.

---

## 1. The audit

AST-002 shipped `DerivedBlobStore` and named the remaining gap in one sentence: *`PackFiles`
exposes no per-member content hash today.* `DerivedAssetKey::Input` wants
`{role, canonical path, **sha256 of content**}`. Nothing in the engine could supply that third
field for an asset inside a PBO — which is nearly all of them.

### Where the code actually lives

`engine/Poseidon/IO/PackFiles.{hpp,cpp}` is the **writer** (`FileBankManager`, used by the `pbo`
tool). The **reader** is `QFBank` in `engine/Poseidon/IO/Streams/QBStream.{hpp,cpp}`. Both were
read; both were the same answer.

### What the format carries per member

Writer (`SaveFileInfo`, `PackFiles.cpp:175`) and reader (`LoadFileInfo`, `QBStream.cpp:259`)
agree exactly on the entry layout: asciiz name, then five int32 —
`compressedMagic`, `uncompressedSize`, `startOffset`, `time`, `length`.

**There is no checksum, CRC or digest of any kind at member granularity.** Not in the format,
not written by this writer, not read by this reader. `grep` for `sha1|crc|checksum` under
`engine/Poseidon/IO/` returns only `ParamFile`'s config CRC, which is a digest of parsed config
entries and has nothing to do with archive members.

The reader does not even trust the one offset field it is given: `QFBank::Load` recomputes
`startOffset` by accumulating `length`, ignoring the stored value entirely
(`QBStream.cpp:614`). So of the five stored ints only three are load-bearing.

### The trailer, and why it is a trap

Arma-2-era and later PBOs end with a 21-byte trailer: one `0x00` byte plus a 20-byte SHA-1.
That is the checksum somebody would reach for. **It is per-archive, of the whole file, and this
reader has never read a byte of it.** Measured across the local installs:

| Corpus | Archives | Members | Bytes in members | Archives with the 21-byte trailer |
|---|---|---|---|---|
| CWA retail (`AddOns` + `Dta` + `Res`) | 44 | 13,935 | 0.47 GiB | **0 / 44** |
| CWA Demo | 14 | 4,838 | 0.12 GiB | **0 / 14** |
| Arma 3 (all `Addons` trees) | 546 | 506,046 | 44.18 GiB | **546 / 546** |

So on the primary corpus the trailer does not exist at all, and where it does exist it names an
archive, never a member. Two archives containing the same `data\stone.paa` have different
trailers; one archive rebuilt with one texture changed has a different trailer for all 4,186 of
its members. It cannot key anything.

Other census facts that shaped the design:

- **Members per archive**: CWA retail median 43, mean 317, max 4,186 (`Dta\Data.pbo`). Arma 3
  median 43, mean 927, max 91,993. So "a typical PBO" is a few dozen members, and the tail is
  five orders of magnitude wide.
- **Compression**: 13,935 / 13,935 retail members and 4,838 / 4,838 Demo members are stored raw
  (`compressedMagic == 0`). Not one `Cprs` or `Encr` member in the CWA corpus. Arma 3: 506,046 /
  506,046 raw as well.
- **Timestamps**: 13,935 / 13,935 retail members carry a non-zero `time`; 1,619 / 506,046 Arma 3
  members carry zero. A timestamp exists almost everywhere, which is exactly what makes it
  tempting and wrong.

### How members are read, and what that costs

`QFBank::Read(const char*)` (`QBStream.cpp:1019`) has three paths:

- `compressedMagic == 0` **and** the archive is memory-mapped → returns a `FileBufferSub` **view**
  onto the mapping. No copy, no read syscall, no allocation of the member's bytes.
- `Cprs` / `Encr` → allocate, read the stored bytes, decode into a `FileBufferMemory`.
- `compressedMagic == 0` without a mapping → allocate and read.

The dominant path on this corpus is therefore the mapped view. **A member's bytes are never
touched by the reader itself** — which is why hashing at index time is a genuinely new second
pass over the whole archive rather than a free ride on work already happening.

---

## 2. The decision

**Lazy per-member SHA-256 of the member's *logical* content, computed on first request through
the existing `Read()` path, memoised per open bank, exposed as
`QFBank::GetContentHash(const char* name) -> RString` ("" when absent or unreadable).**

Two sub-decisions worth stating.

**Logical, not stored, bytes.** What is hashed is what `Read()` hands back, after decompression
or decryption. Hashing the stored bytes would be cheaper (no decode) and would make the
*packer's choice* part of the asset's identity: the same file, raw in one archive and `Cprs` in
another, would key two different derived artefacts. That is exactly the kind of accidental input
`DerivedAssetKey` exists to exclude. On this corpus the distinction is currently moot (0 of
524,819 measured members are compressed), which is precisely why it had to be decided on
principle rather than on measurement — the first compressed addon would have found it silently.

**Lazy, not eager.** Hashing every member at `Load()` would cost a full pass over the archive:
0.47 GiB across the 44 retail archives, 44.18 GiB across the 546 Arma 3 ones, for members that
are mostly never opened. Section 4 puts numbers on that.

### Rejected

| Option | Why not |
|---|---|
| Reuse the PBO trailer SHA-1 | Per-**archive**, absent on 58/58 CWA + Demo archives, never read by this engine. Satisfies none of `DerivedAssetKey`'s requirements. The nearest thing to a checksum in the format, and it is the wrong one — worth naming explicitly so nobody reaches for it later. |
| Hash all members at index time | A second full pass over the archive for data mostly unused. Would turn a 47 ms index of `O.pbo` into a ~800 ms one (section 4). |
| Hash name + size + mtime | The thing `DerivedAssetKey`'s own header forbids in as many words. An addon rebuilt identically invalidates everything; two addons with the same path collide. |
| Hash the stored (compressed) bytes | Cheaper, and makes the packer's choice part of the asset's identity. |
| Persist a hash sidecar next to each archive | A second file with its own staleness problem, and unnecessary: the derived blob store is already the persistent layer, and a hash recomputed per session is cheap next to the artefact it keys. Reconsider only if a measurement shows session-start hashing is a real cost. |
| Store the hash in `FileInfoO` | `FileInfoO` is the on-disk index record, relocated in bulk by `MapStringToClass`. A separate memo keyed by canonical name keeps the index record exactly what the file says it is. |

### The memo, and the one correctness trap in it

The memo is keyed by the member's **canonical** name as stored in the index (lowercased,
native-slash), not by the caller's spelling, so `Data\x.paa` and `data/x.paa` are one entry and
not two independently-computed ones. It is cleared in `QFBank::Clear()`: it is keyed by name
only, so a bank unloaded and reopened over a rewritten archive would otherwise answer with the
*previous* file's hash. That is the bug this design could plausibly have shipped with, and it is
the reason `Clear()` is in the diff.

An absent member returns `""` rather than the SHA-256 of nothing, so "unknown" stays
distinguishable from "empty" — an empty member has a real, well-defined digest.

---

## 3. What was built

- `engine/Poseidon/IO/Streams/QBStream.hpp` — `GetContentHash`, `ContentHashCacheSize`, and the
  `_contentHashes` memo. +38 lines, mostly the rationale above.
- `engine/Poseidon/IO/Streams/QBStream.cpp` — the implementation and the `Clear()` line. +45.
- `tests/unit/engine/Poseidon/IO/Streams/test_qstream_pbo_content_hash.cpp` — 6 tests, 60
  assertions, plus one hidden benchmark. New file.
- Registered in `tests/unit/engine/Poseidon/CMakeLists.txt`. The PBO/QFBank tests live in
  **`PoseidonCoreTests`**, not `PoseidonTests`.

Nothing calls `GetContentHash`. It is opt-in by construction: a build that never asks pays for
one empty `std::map` per open bank and one `clear()` per `Clear()`.

The tests build their own PBOs in the shape all 44 retail archives use (entries, zero-name
terminator, concatenated data, no property block, no trailer), and one test runs over the
checked-in `tests/fixtures/pbo/addon_fixture.pbo` so the reader's real index handling is in the
loop.

---

## 4. The cost

Measured with the hidden `[ast003bench]` case against real retail archives, 5 runs each,
RelWithDebInfo, this workstation. `read` = `Read()` every member and touch one byte of each;
`hash-first` = `GetContentHash` every member, uncached; `hash-memo` = the same call again.

| Archive | Members | Bytes | index (ms) | read (ms), run 0 / runs 1-4 | hash-first (ms) | hash-memo (ms) |
|---|---|---|---|---|---|---|
| `AddOns\O.pbo` | 1,703 | 181.9 MB | 46.6–49.1 | 233.9 / 2.5–2.6 | 749.6–1100.5 | 0.77–0.86 |
| `Dta\Data.pbo` | 4,186 | 106.1 MB | 107.1–134.7 | 313.9 / 5.4–6.1 | 420.4–510.9 | 1.92–2.08 |
| `Dta\Data3D.pbo` | 878 | 52.4 MB | 20.8–37.5 | 242.1 / 106.1–115.9 | 295.9–360.6 | 0.37–0.63 |
| `Dta\Voice.pbo` | 3,758 | 32.5 MB | 114.4–138.8 | 140.6 / 5.0–5.3 | 130.5–146.1 | 1.89–2.05 |

**Index cost is unchanged, before and after.** Not "measured as unchanged" — `Load()` is not
touched by this diff; the numbers above are the reader's existing header-parse cost, quoted so
the hashing figures have something to sit next to. The index is bounded by member *count*
(4,186 members ≈ 120 ms; 1,703 members ≈ 47 ms), not by archive size.

**Read cost is unchanged.** Same reason. The run-0 / run-1 split shows what dominates it: cold
page faults. Once the file is in the OS cache, reading all 1,703 members of `O.pbo` costs 2.5 ms,
because they are mapped views and nothing is copied.

**Hashing costs SHA-256 throughput, ~210–245 MB/s here.** Warm, `O.pbo` is 181.9 MB in ~0.75 s
(243 MB/s); `Data.pbo` 106.1 MB in ~0.43 s (247 MB/s); `Voice.pbo` 32.5 MB in ~0.135 s
(241 MB/s). `Data3D.pbo` runs slower (52.4 MB in ~0.32 s, 164 MB/s) and is also the one archive
whose *read* stays expensive when warm (106 ms), so its members are not all coming from the
mapping; that is a property of that archive, not of the hash.

**Relative to a warm mapped read, hashing is ~100–300× the cost.** That is the honest headline
and the reason for every "lazy" in section 2. Extrapolating at 240 MB/s: hashing *every* member
of the CWA retail corpus once is ~2.1 s; of the Arma 3 corpus, ~190 s. Neither is a thing to do
at startup.

**The memo works**: ~0.5 µs per member on the second pass, i.e. a map lookup and an `RString`
construction, about 1,000× cheaper than recomputing.

**No opt-in flag was added**, because the feature is already opt-in: nothing calls it. If a
future consumer wants to hash a whole archive eagerly, the numbers above are what it must
budget, and *that* is the call site that will need a switch.

---

## 5. The ablations

Two, both by editing the one line that produces the digest, rebuilding, and re-running
`[ast003]`.

**Ablation A — hash the path instead of the content** (`Sha256::Of(key)`):
**3 of 6 cases fail, 3 of 51 assertions.** The first failure is the load-bearing one:

```
REQUIRE( strcmp((const char*)a, (const char*)b) == 0 )   // same bytes, two names
with expansion: -1 == 0
```

**Ablation B — hash nothing** (`Sha256::Of("constant")`): **4 of 6 cases fail.**

Both reverted; the suite is back to 6/6, 60 assertions.

**What the ablations also revealed, and it is worth writing down:** under the path ablation, the
"ignores the member timestamp", "stable across two independent opens" and "memoises" tests all
still **passed**. Those three properties are necessary and nowhere near sufficient — a path-keyed
hash is perfectly stable and perfectly timestamp-independent. The identity tests ("same bytes
under two names agree", "the digest equals `Sha256::Of` of the bytes the read returned") are the
ones carrying the weight, and a future edit that weakens them has removed the guard even if the
case count stays at six.

---

## 6. Suites

Run from this worktree's own build directory, `build/win-x64-clang-rwdi`.

| Suite | Before | After |
|---|---|---|
| `PoseidonCoreTests` | 821 cases / 6,281 assertions / 0 failures | **827 / 6,341 / 0** |
| `PoseidonTests` | 3,089 / 389,159 / 0 | **3,089 / 389,159 / 0** (untouched) |
| `PoseidonFoundationTests` | 491 cases / 0 failures | **491 / 0** |

The "before" for `PoseidonCoreTests` is the same binary run with `~[ast003]`, so before and after
differ only by the new cases. The brief quoted a `PoseidonTests` baseline of 389,164 assertions;
this machine measures 389,159 both before and after the change, and the delta is not this diff.
`PoseidonFoundationTests` reports a different assertion total on each run (168,024 and 163,804
observed) because some of its cases are randomness-seeded; the case count is stable and it is the
case count quoted.

The hidden `[.][ast003bench]` case does not run in the default suite and does not move any count.

---

## 7. What this does NOT prove

- **That any derived artefact is now correctly invalidated.** Nothing calls `GetContentHash`.
  This closes the gap between `DerivedAssetKey` and real inputs; it does not use it. The first
  consumer is a separate decision, and it will have to choose *which* inputs go in the key
  (a model's own bytes, or its materials and textures too) — that choice, not this function, is
  where a wrong answer would come from.
- **That the sky-bake cache is fixed.** It is not ported and not touched. It still keys on a
  64-bit FNV-1a of a lowered model name plus bbox bits. AST-002 documented why that is wrong;
  this change gives a future port the content hash it would need, and nothing more.
- **Anything about loose files on disk.** `GetContentHash` is a `QFBank` method. Assets served
  from a loose directory, from a mod override (`ResolveModOverride`), or through the
  `_ENABLE_PATCHING` path have no hash here. A consumer that mixes the two will need a second
  producer, and until then a mixed-source key would be silently incomplete.
- **Anything about compressed or encrypted members in practice.** The logical-bytes decision is
  reasoned, and covered by the code path, but **0 of the 524,819 members measured across CWA
  retail, CWA Demo and Arma 3 are `Cprs` or `Encr`**, so no test in this repo exercises a
  compressed member's hash against a real archive. The first Arma 2 / DayZ-era corpus to be
  wired in is where that gets its first real witness.
- **Anything about threads.** `QFBank` has no locking (`MT_SAFE 0`) and this adds none. Two
  threads calling `GetContentHash` on one bank race on the memo exactly as they already race on
  everything else in `QFBank`. That is consistent, not safe, and it is the pre-existing state of
  this class rather than something this change introduced.
- **Anything about collisions.** SHA-256 is assumed, not demonstrated. The reason it is SHA-256
  rather than the engine's existing CRC32 is written down in `Sha256.hpp` and unchanged here.
- **Anything in the running game.** Nothing was deployed; the game folder was in use by another
  session for the whole of this work, and no binary was written to it.
