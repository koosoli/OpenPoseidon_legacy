# AST-002 — The Derived Data Cache: what already existed, and the store that did not

**Date:** 2026-08-31
**Branch:** `agent-derived-data-cache` (from `asset-obs-001`)
**Roadmap item:** critical-path step 10A, Phase 6 "Derived Data Cache"
**Scope:** audit, design, one slice (a keyed blob store), and its ablation. **No producer
is wired into it in this pass**, deliberately.

---

## 1. The audit

Seven roadmap items investigated in this repo this week turned out to be already built, so
the first question was not "how do I build a DDC" but "how much of one is here". The answer
is: **most of the key, none of the store.**

| What | Keyed on | Survives restart? | **What invalidates it** |
|---|---|---|---|
| `Asset/Cache/DerivedAssetKey.hpp` (AST-020) | SHA-256 of {role, canonical path, **content hash**} per input + **compiler version** + sorted options | n/a — it is a key, not a store | By construction: any input byte, the compiler version, or any option changes the digest |
| `Asset/Cache/AssetCache.hpp` | case-insensitive name → generation-tagged slot | **No** — in-memory | Nothing automatic. `Remove`/`Clear` only; the header says eviction is deliberately the caller's problem |
| `ShapeBank` (`Graphics/Rendering/Shape/Shape.hpp:1160`) | `(name, reversed, shadow)` | **No** | **Nothing. It never evicts** — the comment says so: shapes have no cheap byte size and are held by live objects |
| `PreparedTextureStore` (`Graphics/Textures/PreparedTextures.hpp`) | lower-cased texture name | **No** | Three ways, all in-process: consumed by a `Take`, a 30 s TTL sweep, or the world-change `Reset`. Plus a byte cap that *refuses* rather than evicts |
| sky-bake cache (`WgpuRenderer/rust/src/gfx3d/mod.rs:9600`) | **64-bit FNV-1a** of lowered model name + lod0 bbox bits + a hand-bumped `SKY_BAKE_CACHE_VERSION` | **Yes** — `%LOCALAPPDATA%/OpenPoseidon/skybake/*.skv` | The format-version constant, and the bbox as a *proxy* for geometry change. Not the model's bytes, and not the bake code's version |
| `resmap.tsv` (`apps/tools/Tools/commands/XobCommand.cpp:583`) | the file path the user passes to `--resmap-cache` | **Yes** — a plain TSV | **Nothing at all.** No version, no header, no hash of the `.pak` corpus it was scanned from. A changed Reforger install is served the old map silently |

### The finding that shaped the slice

**`DerivedAssetKey` is the roadmap's key, already implemented, already correct, and
completely unused.** `grep` finds it in exactly one place outside its own header: its unit
test. It hashes content rather than paths or mtimes, it carries the compiler version, it
sorts so discovery order cannot leak in — everything the Phase 6 diagram asks for. What it
does not have is anywhere to put a blob. **A key that invalidates perfectly, attached to no
storage, invalidates nothing.**

So the gap is not the key. It is the store.

### The sky-bake cache: is it the pattern?

**Its mechanism is the pattern; its key is not.** It is the only working, shipped,
restart-surviving derived-data cache in the tree, and three of its choices are right and
worth generalising:

- a magic + version header, so a format change is a miss rather than a misread;
- write-to-temp-then-`rename`, so a crash mid-write cannot publish a torn file;
- an env kill-switch (`WGR_SKY_BAKE_CACHE=0`) covering both read and write.

Four of its choices do not survive contact with a general DDC, and each is a concrete bug
rather than a stylistic difference:

1. **A 64-bit FNV-1a names the file and nothing verifies it.** The entry does not carry its
   own key, so a collision, a hand-copied file, or a stale entry left by a key scheme that
   has since changed reads back as a **hit of the wrong data** — silent and
   content-dependent. At sky-bake's scale (a few thousand models) FNV-64 collisions are
   unlikely; at DDC scale, across every mesh, texture and material of a foreign world, that
   is not a bet worth taking, and the cost of not taking it is 64 bytes per entry.
2. **The key is the model *name plus its bounding box*, not its content, and it contains no
   producer version — only a format version.** The bbox is an honest proxy for "the
   geometry moved", but a repack that changes interior geometry inside the same bbox hits.
   More importantly, **fixing a bug in `sky_bake` does not invalidate anything**: every
   machine that already cached the broken volumes keeps serving them until a human
   remembers to bump `SKY_BAKE_CACHE_VERSION`. That is precisely the failure the roadmap's
   "importer version" line exists to prevent.
3. **The temp file name is derived from the key alone** — `path.with_extension("tmp")`.
   Every process and every thread writing the same key writes to the *same* temp path. Two
   writers interleave into one file and the rename publishes the interleaving. If the
   interleaving happens to land at the right length, the length check passes and the entry
   is trusted. Write-then-rename only buys atomicity if the temp is private to the writer.
4. **Validation is magic + exact length, and there is no size bound.** A file damaged in
   place at the same length reads as a hit; the directory grows forever.

Conclusion: **generalise the mechanism, replace the key with `DerivedAssetKey`, and fix all
four.** Not reinvent, and not extend sky-bake in place — see §5.

---

## 2. The design

### The key

`DerivedAssetKey`, unchanged. It already answers the Phase 6 diagram:

| Roadmap term | Where it lives |
|---|---|
| canonical source identity / addon identity | `VirtualPath::canonical()` per input |
| source-file hash | `Input::sha256` — **content, not path or mtime** |
| importer version | `kCompilerVersion`, in the digest **and** in the file name |
| compatibility-layer version | an option pair, or a further version input |
| derived-format version | `kContainerVersion` in the store's own header (independent axis) |
| platform/GPU-format capability | an option pair (`AddOption("bcFormat", "bc7")`) |

**Why the producing code's version must be in the hash, stated as the failure it prevents:**
a converter bug is found and fixed. Without the version in the key, the fix ships and
*nothing changes on any machine that ever ran the broken build* — they all keep serving the
cached broken output, and the bug reproduces only on machines that never had it. That is
the worst possible support-report shape. `kCompilerVersion` is a plain integer a human
edits, deliberately not a build timestamp: a rebuild that changes nothing must not
invalidate every cache in existence.

**Two independent version axes, on purpose.** `kCompilerVersion` says "the meaning of the
output changed"; `kContainerVersion` says "the store's own envelope changed". Collapsing
them would force a full cache wipe for a header tweak, or leave a header change silently
reinterpreting old field offsets.

### Storage layout

```
<root>/<first two hex of sha>/v<compilerVersion>-<sha256>.pdc
```

`<root>` is `%LOCALAPPDATA%/OpenPoseidon/derived` (`$XDG_CACHE_HOME`/`~/.cache` elsewhere) —
outside the source assets, which the roadmap requires and sky-bake already does. Sharded on
two hex characters because `Prune()` enumerates the tree and a flat directory of a hundred
thousand entries is slow to walk on NTFS.

**The version is in the file name, not only inside the digest.** It is already in the digest,
so this is redundant for correctness — it is there so two compiler versions visibly coexist
as separate files, and a rollback to v7 cannot read v8's output even if something went wrong
upstream in key construction.

Entry format:

```
magic "PDC1" (4) | containerVersion u32 | idLen u32 | payloadLen u64
| payloadSha256Hex (64) | id (idLen) | payload (payloadLen)
```

The two fields sky-bake lacks, and what each one buys:

- **`id` — the full key echoed inside the entry.** Read verifies it against the requested
  key. A hash collision, a file copied to the wrong name, or a leftover from an older
  naming scheme is now a **miss**, not wrong data.
- **`payloadSha256Hex` — the payload's own digest.** Catches damage that preserves the
  length, which a magic-plus-length check structurally cannot see.

### Atomicity

Write to `<shard>/.tmp-<pid>-<counter>-<sha16>`, then `rename` onto the final path.

The temp name carries the **process id and a per-process atomic counter**, so no two writers
anywhere ever share a temp file. This is the sky-bake bug, fixed by construction, and it is
the entire concurrency story: there is no lock. Entries are immutable once published, and a
key names one content forever, so two writers racing on a key are writing *identical bytes*
and whichever `rename` lands last is correct either way.

A `rename` that fails — on Windows a concurrent reader can hold the target open — leaves the
existing valid entry in place, deletes the temp, counts a `writeFailure`, and returns false.
**A cache is an optimisation; failing to have one is never fatal.** Nothing in `Put` or `Get`
throws.

### Corrupt entries

Every rejection path — short file, wrong magic, wrong container version, absurd length field,
key mismatch, payload digest mismatch — is a **miss with `out` cleared**, so a caller cannot
use a partial fill. The offending file is then **unlinked**: a poisoned entry that stays costs
every subsequent run a read and a hash before reaching the same conclusion. `rejectedCorrupt`
is counted separately from `misses` so a machine with failing storage is visible rather than
merely slow.

The length check is computed in 64-bit (`kFixedHeader + idLen + payloadLen`) so a damaged
length field cannot wrap past it and be used to size an allocation.

### Size bound and eviction

An explicit byte budget (512 MB default) and `Prune()`, which evicts oldest-`mtime`-first
until the tree fits.

**Explicit rather than automatic on every `Put`**: eviction is a whole-directory walk, and
the moment to pay for that is a level change or shutdown, not a model load in the middle of
a frame. Sky-bake has no bound at all.

`mtime` is really "least recently *written*", not least recently used. True LRU means
touching a file on every hit — a write per read. Not obviously worth it; measurable later if
the hit distribution turns out to matter.

---

## 3. What was built

**`engine/Poseidon/Asset/Cache/DerivedBlobStore.hpp`** — header-only, matching
`DerivedAssetKey.hpp` next to it. `Get`/`Put`/`Contains`/`Prune`/`TotalBytes`/`EntryCount`,
plus a `Stats` block (`hits`, `misses`, `rejectedCorrupt`, `writes`, `writeFailures`,
`evictions`, `bytesRead`, `bytesWritten`) so a cache that is quietly missing everything is
observable rather than merely slow.

It knows about **bytes, not about meshes**. A producer serialises its own artefact and hands
it over. That is what makes it reusable across the six candidate products the roadmap lists,
and it is why no producer is wired in here: serialisation format is a per-producer decision
and mixing it into the store's first pass would bake one producer's assumptions into all of
them.

**`tests/unit/engine/Poseidon/Asset/Cache/test_derived_blob_store.cpp`** — 21 cases:

| Group | Cases |
|---|---|
| Basic | miss on empty; byte-exact round trip (embedded NUL and high bytes); empty payload is a hit not a miss; hit survives a fresh store object over the same root; a different key does not read another key's entry |
| Invalidation | changed source content invalidates; **bumping the compiler version invalidates every entry it wrote, and the two versions coexist**; the version is in the file name |
| Damaged entries | truncated; zero-length; **corrupted in place at the same length**; wrong magic; **wrong container version**; **carrying another key's id**; absurd payload length |
| Concurrency | 8 threads × 8 writes of one 64 KB key never publish a torn entry; a reader racing a writer sees a whole entry or none; no temp files left behind |
| Bounds | `Prune` evicts to budget and stops; `Prune` keeps the newest; a root that cannot be created fails the write instead of throwing |

**Test counts** (`PoseidonTests.exe`, `win-x64-clang-rwdi`):

| | test cases | assertions | failures |
|---|---:|---:|---:|
| before | 3061 | 242,709 | 0 |
| after | **3082** | **242,827** | **0** |

---

## 4. The ablation

A cache whose invalidation cannot fail is not tested. Both invalidation axes were broken in
turn, rebuilt, and re-run.

**Ablation A — remove the producer version's contribution to the key.** Deleted
`hash.Update(std::to_string(compilerVersion_))` from `DerivedAssetKey::Compute()` and
dropped the `v<N>-` prefix from `FileName()`. Both mechanisms had to go together, because
either one alone still separates the versions — which is the point of having two.

```
3 failed:
  Derived key: the compiler version participates                            (AST-020)
  Blob store: bumping the compiler version invalidates every entry it wrote (AST-002)
  Blob store: the version is in the file name, not only in the hash         (AST-002)

  REQUIRE( DerivedBlobStore::EntryId(sample(7)) != DerivedBlobStore::EntryId(sample(8)) )
  with expansion:
    "9813c023...213a0845.pdc" != "9813c023...213a0845.pdc"
```

The two versions land on one path and the buggy converter's output is served to a machine
running the fixed converter — exactly the failure §2 names.

**Ablation B — remove the store's two content checks.** Deleted the payload-digest
comparison and the container-version comparison from `Decode`.

```
2 failed:
  Blob store: a payload corrupted in place at the SAME length is rejected
  Blob store: an entry written by another container version is rejected
```

Note which cases *did not* fail under B: truncation, wrong magic, and the absurd-length case
still pass, because the length and magic checks catch those. That is the measurement — it
shows the digest is earning its 64 bytes on the one class of corruption the cheaper checks
structurally cannot see, rather than being belt-and-braces over ground already covered.

Both ablations reverted; full suite re-run green at 242,827 assertions.

---

## 5. What a second producer would need

Nothing in the store. The remaining work is entirely on the producer side, and it is the
same three items for each of them:

1. **Content hashes for the inputs.** `DerivedAssetKey::AddInput` wants a SHA-256 of the
   source *bytes*. For a loose file that is a read; for a PBO/PAA member it should come from
   the archive entry rather than a re-read, and `PackFiles` does not expose one today. This
   is the largest single piece of remaining work and it is shared by every producer.
2. **A serialisation for the artefact**, plus its own version discipline — the producer's
   `kCompilerVersion` must be bumped whenever the serialised meaning changes, not only when
   the algorithm does.
3. **A decision about where `Prune()` is called.** Level change is the obvious candidate;
   nothing calls it yet.

**The most concrete customer is RFG-002** (loading Reforger's Everon with no pre-export),
which is **parked by the owner** — this store was designed against it as a workload, not
implemented for it. That workload is the right one to design against because it is the case
where conversion genuinely moves to load time: Everon is 1,230,897 placements over 110,577
resource GUIDs, and a cache is what makes converting geometry at load affordable rather than
a 35-minute wait. The store's shape follows from that: content-addressed so a re-run of a
partially-changed corpus reuses everything unchanged, sharded so a million-entry tree walks,
bounded so it cannot fill the user's disk with derived Everon.

**The sky-bake cache is deliberately left alone.** It is Rust, it works, and porting it means
either exposing the store across the C ABI or mirroring it in Rust — a real decision with a
real cost, and not one to fold into the pass that first proves the store works. When it is
done, the win is not the mechanism (sky-bake already has that) but the key: keying the bake
on the model's *content* and on the bake code's version, instead of on a name, a bounding box
and a constant someone has to remember to bump.

**Slice 3 (material interpretation) remains off the table**, per the roadmap's measurement:
95% of a 28.7 ms frame is `swap`, and every piece of main-thread CPU work sums to ~1.5 ms.
Caching interpreted materials is a candidate *product* for this store, but it is not the
producer that justifies wiring one in first.
