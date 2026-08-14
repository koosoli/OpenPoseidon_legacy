# WGPU renderer FFI contract

The public boundary is [`../include/wgpu_renderer.hpp`](../include/wgpu_renderer.hpp).
It uses C linkage for every exported symbol; C++ conveniences in the header do not
cross the boundary.

- `WGR_ABI_VERSION` is the compatibility gate. Any incompatible ABI change must
  increment it in the header and in Rust's `wgr_abi_version()`. Before
  `wgr_create`, `EngineWgpu` passes `WgrAbiCheck`; Rust verifies its own
  structure size plus `WgrSurfaceDesc`, `WgrLogCallbacks`, and `WgrFrame` sizes
  and refuses a stale pairing. `required_features` is a bit-set negotiation:
  an engine requiring an unsupported capability is refused before renderer
  creation. `wgr_build_id()` identifies the Rust artifact by source revision
  and build profile in the startup log.
- `WgrAbiCheck::layout_hash` is the gate that does **not** depend on anyone
  remembering to bump a version. It is an FNV-1a over the `sizeof` of every
  struct that crosses the boundary, computed independently by
  `WgrLayoutHash()` (header, bottom of file) and `wgr_layout_hash()`
  (`rust/src/ffi.rs`) from their own definitions. Any size change to any shared
  struct, on either side alone, changes the number and the handshake refuses the
  pair. **Adding a shared struct means appending it to both lists**, in the same
  position.

  This exists because bumping the version by hand was never reliable and the
  failure was silent: `WgrSkyRuntime` grew from 5 vec4 to 7 (80 -> 112 bytes)
  when the moon disc added `moon_params` + `moon_sun`, the version stayed at 4,
  and an engine binary from before that change kept shaking hands successfully
  while the renderer read the moon's angular radius and illuminated fraction off
  the end of an 80-byte stack object. The moon simply never drew. The size
  hash does not see a same-size field reorder — for the sky UBO that gap is
  covered by `sky/mod.rs`'s field-order test against the WGSL.
- Diagnostic exports that a *previous* DLL may not have (`wgr_abi_layout_hash`,
  `wgr_get_runtime_capabilities`) are deliberately absent from the header and
  fetched with `GetProcAddress`. Importing them would make an older DLL fail
  Windows module loading with a bare "Entry Point Not Found" instead of reaching
  the handshake's own diagnostic.
- C++ owns every input structure and slice passed to a call.  Rust borrows those
  inputs only for that call and never retains their pointer.
- Rust owns the opaque `WgrRenderer` returned by `wgr_create`; C++ releases it
  exactly once with `wgr_destroy`.  A null renderer is always accepted by the
  destruction and setter APIs where documented.
- Rust catches panics at exported entry points.  C++ does not let exceptions
  cross into Rust; its callbacks only log engine-owned text synchronously.
- Both sides keep compile-time size/alignment assertions for every shared data
  block.  Add or update assertions on both sides together with an ABI-version
  change.
- The executable build fingerprint and the staged DLL's SHA-256 are written by
  `scripts/write_preview0_manifest.py`.  That manifest is the build identity
  used when comparing a runtime log with the exact artifacts that produced it.

This contract deliberately does not provide transparent device recreation.
Preview-0 records device/surface failures and lets the engine use its normal
restart or backend-selection policy.
