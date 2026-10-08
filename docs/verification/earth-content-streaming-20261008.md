# Geographic content transport

The two presentation workers can now request pre-tiled building and feature
cells as the camera's resident terrain changes. This supplies a transport for
a separately hosted dataset; it does not supply a worldwide OSM dataset or
turn a public Overpass instance into a gameplay backend.

`Launch-WholeWorld.ps1 -ContentUrlTemplate 'https://your-host.example/{kind}/{lat}_{lon}.json'`
enables both consumers. The environment equivalent is
`POSEIDON_EARTH_CONTENT_URL` together with the existing building/feature gates.
`kind` is `buildings` or `features`; `lat` and `lon` are integer 0.02-degree
cell indices, with longitude wrapped at the antimeridian. Responses use the
same `elements` JSON format accepted by the preparation scripts, with
geometry for ways. A valid empty cell is `{"elements":[]}`. Source licensing,
OSM attribution and ODbL obligations still apply to the hosted dataset.

The default remains cached presentation. No content endpoint is configured or
contacted by default. Hand-prepared cells retain priority. Streamed cells use
the separate `content-stream` subtree and remain usable offline. Invalid
generated cache entries refetch; hand-prepared files are never evicted.
Features and buildings now use the same exact spherical projection as terrain
and imagery, including after a geographic origin shift.

Limits: one shared transfer at a time on background workers, at most a 3x3
window per request, a six-second window deadline, one-second connection and
three-second transfer timeout, 4 MiB per response, 60-second failed-cell
cooldown and 256 failed addresses in memory. Transfers cancel on superseding
terrain revisions and shutdown. Only HTTPS, no redirects or URL credentials.
Plain HTTP is available solely through the explicit C++ loopback test option;
the game never enables it. No per-frame network/render-thread waiting.

Only validated complete JSON without a server `remark` is published by atomic
rename. Element limits are 2,000 buildings or 6,000 general features. Generated
disk cache is limited to 128 cells and 256 MiB shared across both kinds; stale
partial writes are removed before admitting another file. Presentation mesh,
tree and source-point budgets remain unchanged. Missing/failed cells preserve
the existing terrain and available cached neighbors. Stationary failed cells
retry on a later terrain generation/window request, rather than polling.

`Test-EarthContent.ps1` uses synthetic loopback data to verify real HTTP byte
limits, rejection/redirect behavior, cache reuse/repair, cancellation, disk
eviction, prepared-cell preservation and network/disk failure cooldown.
`Test-EarthFeatures.ps1`, `Test-EarthBuildings.ps1` and
`Test-EarthCoordinates.ps1` verify the geometry consumers and spherical
addressing. Installed rendering acceptance is recorded separately in the
integrated ledger; a working transport is not proof of worldwide asset,
collision, traffic, hydrology or navigation completeness.
