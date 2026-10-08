# One cached Luxembourg surface cell

This optional Earth-preview slice draws a locally prepared ACT 2023 orthophoto
on the existing elevation mesh around Luxembourg city (49.61, 6.13). It adds
local imagery detail, not new elevation precision, collision, roads, vegetation
or a worldwide playable landscape. Geographic relocation remains an explicit
teleport; seamless circumnavigation is not implemented.

## Prepare and enable

Run `scripts/Prepare-EarthSurface.ps1` once before launching. Its only network
operation is one explicit 2048x2048 JPEG WMS request, capped at 8 MiB with a
60-second header timeout and a 60-second body cancellation deadline, without
retries. Existing image/receipt pairs are checked and reused. Data and its
SHA256/source/bounds receipt stay outside Git, under
`%LOCALAPPDATA%/OpenPoseidon/world-stream/surface/`.

Start the existing Earth preview with `POSEIDON_EARTH_SURFACE=1` in addition to
its existing `POSEIDON_EARTH_CACHE` and `POSEIDON_EARTH_IMAGERY` settings. The
NASA file remains `blue-marble-june-5400.jpg` in the Earth cache. Go to
latitude 49.61, longitude 6.13, altitude about 2000–4500 m using Earth navigation.
Use the same cached buildings option if desired. The surface option is off by
default and never changes original OFP rendering/gameplay.

The worker performs no network requests. Missing, malformed, oversized or
unverified files preserve the existing NASA presentation and log the reason.
Source files are loaded once: if preparing or repairing a cache after launching,
restart the preview. Moving elsewhere preserves NASA imagery; this is one cell,
not automatic imagery downloads around every destination.

## Source and orientation

Source: **Administration du cadastre et de la topographie (ACT), Luxembourg,
ortho_2023**, licensed **CC0 1.0** in the official
[2023 orthophoto dataset](https://data.public.lu/fr/datasets/orthophoto-officielle-du-grand-duche-de-luxembourg-edition-2023/)
and [WMS/WMTS service dataset](https://data.public.lu/fr/datasets/bd-l-ortho-webservices-wms-et-wmts/).
[Official service capabilities](https://wmts1.geoportail.lu/opendata/service?REQUEST=GetCapabilities&version=1.3.0)
advertise JPEG support and no fees/access constraints. No numeric request quota
or SLA was found; this prototype therefore uses one manual request, no retries
and persistent local reuse.

Bounds are south=49.60, west=6.12, north=49.62, east=6.14 (WGS84).
WMS 1.3 EPSG:4326 requests use latitude/longitude axis order; image row zero
is north. The source's native resolution is finer than this bounded response:
the delivered JPEG covers roughly 1.44 by 2.23 km, about 0.7 by 1.1 m per texel.
The 2048 atlas spans the current 6.4 km near window at low altitude, so its
output sampling is about 3.125 m per texel (12.5 m at the 25.6 km high band).
NASA remains a coarse global overview; no high-resolution worldwide imagery
is included.

Prepared local sample: 1,637,387 bytes, SHA256
`9a6c5fbae7bf4683a9485c51b152f2346c0dba4c09194406cca34ed656ce4c3b`.
This receipt describes the external sample, which is not committed.

## Rendering and bounds

The optional path splits the existing near/far indices into two draws. The
257x257 near vertices, heights, normals and triangle topology stay identical.
Far skirts remain unchanged. There is no coplanar overlay, extra terrain
triangle, new fullscreen pass, shader-global change or material ABI change.
The combined near/far/skirt budget remains 256,000 triangles for a fully valid
patch. The off path retains the existing combined mesh and global NASA UVs.

The near draw initially uses NASA. A cache-only worker composites a 2048 RGBA
atlas, feathering ACT imagery 75 m inside its cell boundary and NASA 200 m
inside the near-window boundary. Its NASA source uses the same stb linear
4096x2048 resize as the globe. Local atlas UVs and texture are installed together
only for the current height-patch generation and geographic-origin revision.
New requests discard older ready output; relocation cancels pending output;
composition checks cancellation every row. Remote cells skip atlas allocation.

Pixel memory is bounded explicitly, in addition to the existing Earth renderer:

- JPEG metadata is inspected before decode. NASA is limited to 5400x2700,
  compressed 16 MiB; ACT must be exactly 2048x2048, compressed 8 MiB; receipt
  is limited to 16 KiB and verifies SHA256, source identity and bounds.
- Worker resident decoded sources: NASA 4096x2048 RGBA **32 MiB**, ACT RGBA
  **16 MiB**. One working/ready atlas is **16 MiB**; allowing for temporary
  renderer handoff/replacement gives a conservative **80 MiB** pixel budget.
- The initial NASA decode is at most **55.62 MiB**. Together with its 32 MiB
  resized copy and the 16 MiB compressed cap, initial source preparation reaches
  about **103.62 MiB** in these buffers. JPEG decoder scratch and allocator
  overhead are additional; this is a pixel/file-buffer bound, not process RAM.
  The original decode and compressed bytes are released before ACT loads.
- The extra mipmapped atlas costs about **21.33 MiB GPU**; replacement can
  briefly retain two atlases (**42.67 MiB**) until resource retirement. The
  existing globe NASA texture and renderer upload/retirement buffers are
  separate baseline costs. Near/far geometry is split, not duplicated.

## Verification

`scripts/Test-EarthSurface.ps1` compiles focused CPU checks using the existing
vcpkg dependencies; it does not install dependencies or fetch data. Passing
`-CacheDirectory` additionally decodes/verifies the real local ACT/NASA pair,
composes an atlas after a superseded relocation, and verifies invalid receipt
fallback. Tests cover image north orientation, smooth cell/window feathering,
near/far seams at both altitude bands, dateline repeat, cancellation, latest
publication and invalid windows/cache. Runtime acceptance is performed by the
parent installed-game test after integration; CPU checks alone do not establish
visual acceptance.
