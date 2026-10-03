# Read-only sand publication diagnostic

This change completes the unfinished trace/snapshot work left by the previous
`sand-upload-audit-20261001` agent. It changes neither sand contacts, terrain
geometry, shader sampling, public ABI, source eligibility nor normal defaults.
The root agent remains the sole combined build, device-test and deployment owner.

`WGR_SAND_UPLOAD_TRACE=1` records the exact queued C++ snapshot and its Rust
admission: four header floats, total float count, signed-cell counts, extrema and
FNV-1a64 over every little-endian f32 byte. Rust also reports the actual buffer
allocation and write offset. The four snow header floats live outside the WGSL
`snow.deficits` array; the sand header therefore starts at deficits[524292], with
cells at524296. The byte write starts at2097184 in a3145776-byte buffer.

Only with the trace enabled, `WGR_SAND_SNAPSHOT_FILE` and
`WGR_SAND_SNAPSHOT_ROOT` can save the actual camera-resident snapshot. The root
must be an existing absolute fixture directory and the file's canonical parent
must match that directory. Relative paths, other extensions, directories,
symlink targets, outside paths, invalid counts and invalid signed data refuse
the write. One1048592-byte file is overwritten; no frame history accumulates in
the production producer. All filesystem and receipt work is opt-in.

`Test-OriginalOFPWeatherPlayerRuntime.ps1 -SandSnapshotTrace` supplies those
paths inside its current per-map fixture directory. For each sand capture, it
copies the current source snapshot and requires matching queued, successful dump
and accepted renderer receipts. The reader validates every cell against the
actual enabled/disabled signed contract and computes FNV and SHA256 from the same
byte read. Source/Rust header and extrema must agree within their log formatting
rounding intervals; hashes and integer counts/offsets agree exactly. A copy race
or missing receipt fails the diagnostic. The helper and runner hashes are saved
with the existing actual-world/player/body/contact provenance.

## CPU evidence

On2026-10-03, `Test-SandUploadReceipt.ps1` passed against the complete production
`SandField`, production C++ receipt/dump helper and extracted actual Rust
`valid_sand_view`/receipt declarations. A replay of four saved Everon contact
positions uses an explicitly controlled heading, not an invented claim about
the original unlogged actor orientation. Two different camera snapshots preserve
the exact field samples. The representative snapshot contains43negative cells
and79positive cells, minimum−0.09920236m and maximum+0.01394663m. Its C++ and Rust
FNV matches at`e5dd41cad4cfbc71`, including exact extrema f32 bits.

Tests reject malformed signed payloads, disabled nonzero geometry, nonfinite
data, unbounded cells/rims, unsafe dump destinations and altered queued/accepted
receipt hashes, counts, flags, header, extrema and buffer offsets. The snapshot
SHA matches an independent completed-file hash. The original-map runtime-runner
self-test also passes. Full combined build and installed trace collection remain
pending with root.

These receipts prove source publication and renderer acceptance, **not GPU
completion or visible sand depressions**. The source audit found consistent
signed shader indexing, vertex-height sampling and filtered fragment normals.
The installed contact-union source refines near feet to4m patches with32quads,
matching the125mm field spacing. Actual GPU sampling and inspected low/top views
are still required to resolve the reported invisible sand relief.
