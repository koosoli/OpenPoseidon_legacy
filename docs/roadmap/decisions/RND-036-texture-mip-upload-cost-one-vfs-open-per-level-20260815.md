# RND-036 — A texture upload cost one VFS open per mip level, and the transient was never split

**Date:** 2026-08-15
**Branch:** `feat/texture-streaming` (worktree `.claude/worktrees/texstream`, based on
`feat/farfield-tier` @ `2f3e325`)
**Status:** Redundancy **removed and unit-tested**. Its share of the streaming transient is
**NOT measured** — see "What this does not claim".
**Related:** [RND-035](RND-035-the-lod-governor-was-costing-40-percent-of-the-frame-20260815.md),
[RND-033](RND-033-cross-layer-frame-profile-objects-are-not-the-bottleneck-20260815.md)

## The defect

`TextureSourcePac::GetMipmapData` opened the texture through the VFS **on every call**:

```cpp
bool TextureSourcePac::GetMipmapData(void* mem, const PacLevelMem& mip, int level) const
{
    QIFStream in;
    GFileServer->Open(in, _name);   // <- once per LEVEL
    mip.SeekLevel(in);
    ...
}
```

`TextureWgpu::EnsureUploaded` wants the whole chain, so it called that once per level. A
texture therefore cost `_nMipmaps` opens where one would do — up to `MAX_MIPMAPS` (**7**),
and 3 even for the 64-square test fixture.

The prior handover estimated "~11 VFS opens" for a 2048-square PAA. That is wrong in the
engine's favour: `MAX_MIPMAPS` is 7, so 7 is the ceiling, not 11.

The bytes were never the redundant part — every level has to be LZO-decompressed either
way. The **opens** were: `FileServerST::Open` pumps the request queue, then does two linear
`strcmp` scans of the file cache plus an O(n) move-to-front, per call.

The same loop also grew its destination buffer *inside* the loop
(`blocks.resize(off + layout.dataSize)`), recopying the accumulated chain up to `_nMipmaps`
times for a chain whose final size is exactly computable up front.

## The fix

`ITextureSource::GetMipmapChain(const MipmapRead*, int)` — read a run of levels in one call.

- The **default implementation is the old loop**, so `TextureSourceDDS` and
  `TextureSourceJPEG` (which hold decoded data and open nothing per level) are byte-for-byte
  unchanged, and so is `TextureGL33`, which was deliberately not touched.
- `TextureSourcePac` overrides it: one open, then `SeekLevel` + `LoadPaa`/`LoadPac` per
  level. This is legal because `PacLevelMem::SeekLevel` is an **absolute** seek to that
  level's recorded start offset — the levels do not depend on the stream position or on each
  other.
- `TextureSourcePac::GetMipmapData` is now a one-element chain read, so there is one code
  path, not two that can drift.
- `TextureWgpu::EnsureUploaded` sizes the chain in one pass, allocates once, then issues one
  read.

## What is measured

`tests/unit/.../Graphics/test_texture_mip_chain.cpp`, against the real
`synthetic_dxt5.paa` fixture (3 levels):

| | opens |
|---|---|
| level-at-a-time (the old shape) | **3** — one per level |
| `GetMipmapChain` | **1** |

and the two results are compared **byte for byte over the whole packed chain**, so a level
read at the wrong offset fails the test rather than showing up as a corrupt mip in game.
The counter behind this is `TextureMipOpenCount()`, which is also readable at runtime.

## MEASURED IN GAME (added after the run)

Reforger Everon, settled, `WGR_TEXTURE_STREAM_STATS=1`, at 512 uploads:

```
uploads=512 mipOpens=506 blockMB=234.1 readMs=391.9 createMs=123.3
fallbacks=12 fallbackMs=23.5 alphaScans=112 alphaMs=357.4 totalMs=896.1
```

**The fix works: 506 opens for 512 uploads — one per texture.** Before it, each upload cost
one open per level, so the same 512 textures would have cost up to ~3,500. That is the
claim this document could not make when it was written, and it is now measured.

**But the fix is not where the time is.** Of 896 ms of texture work:

| | ms | per call | share |
|---|---|---|---|
| mip chain read (VFS + LZO) | 391.9 | 0.77 ms | 44% |
| **alpha classification** | **357.4** | **3.19 ms** | **40%** |
| `wgr_texture_create` | 123.3 | 0.24 ms | 14% |
| whole-file fallback decode | 23.5 | 1.96 ms | 3% |

`ScanTopMipAlphaStats` ran for **112 of 512 textures (22%)** and cost **40% of all texture
time**, at **3.19 ms each** — four times the cost of actually reading and uploading the
texture. It reads the file a second time, decodes the full-resolution mip to RGBA8, and walks
every pixel's alpha, purely to decide opaque/cutout/blend. It buys no pixels on screen.

This was flagged in the original write-up as "the most likely thing to be larger than
expected". It is. **It is now the top texture-streaming target**, ahead of anything else here
— and it matters most on DayZ Chernarus, which is 62.7% textures by VRAM (3.58 GiB) where
Everon is 84% geometry.

Care is needed: the top mip is used deliberately (a smaller mip blurs a cutout's crisp 0/255
holes into false partial-alpha), and the PAA TAGG path is MAT-047's territory on another
branch. The cheap wins are to cache the classification per texture *file* rather than per
`TextureWgpu`, and to short-circuit formats whose header already answers the question.

## What this does not claim

**The share of the streaming transient this removes is unknown.** The transient is ~81 s on
Everon at ~1.1 ms of CPU per object created, and that number is still unattributed *within
itself* — object construction, the synchronous ODOL parse and texture upload are all inside
it. Removing `n-1` of `n` opens per texture is structurally less work; "structurally less
work" is not a measurement, and this was not benchmarked in game because another agent's
Stratis/Chernarus benchmark owned the deployed binary for the whole session.

## The attribution that was missing

`WGR_TEXTURE_STREAM_STATS=1` now splits the texture side of the transient into the three
things it actually does, cumulatively (the transient is 81 s long — a per-frame region
cannot answer "of the whole fill-in, how much was texture work"):

```
Wgpu texture stream stats: uploads=N mipOpens=N blockMB=… readMs=… createMs=…
                           fallbacks=N fallbackMs=… alphaScans=N alphaMs=… totalMs=…
```

- `readMs` — reading the mip chain (VFS + LZO), the part this change targets.
- `createMs` — `wgr_texture_create`.
- `alphaMs` / `alphaScans` — `ScanTopMipAlphaStats`, which for a multi-bit-alpha format
  reads the file *again*, decodes the **full-resolution** mip to RGBA8, and walks every
  pixel. This buys no pixels on screen; it only classifies opaque/cutout/blend. It is
  counted separately because it is the most likely thing to be larger than expected.
- `mipOpens` — `TextureMipOpenCount()`, so the fix can be credited or dismissed on a number.

Logged on power-of-two upload counts: dense while the world fills in, near-silent once
settled, with no frame hook. Off by default; when off it costs one cached bool test per
upload and reads no clock (`getenv` is 2.1 µs a call on this machine, so it is read once).

## What to do next

1. Run Everon with `WGR_TEXTURE_STREAM_STATS=1` to plateau and read the split. That answers
   whether texture upload is 5% or 50% of the transient — and therefore whether the two
   remaining suspects (the synchronous `ShapeBank::New` ODOL parse; moving decode off the
   main thread, which is legal where I/O is not) are the real targets.
2. If `alphaMs` is large, the cheapest remaining win is caching the alpha class per texture
   *file* rather than per `TextureWgpu`, or deriving it from the PAA TAGG header where one
   is present, instead of a full-resolution decode.
3. Only then consider capping mip levels during streaming (the previous handover's
   "cheapest first"). It is not cheap: it needs an upgrade path to re-upload the full chain
   later, and it changes what is on screen, so it needs visual verification the other two do
   not.
