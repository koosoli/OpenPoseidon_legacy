# Pure projected selected-surface bound prerequisite

Source24283a81d84e3126caed01bd6cd374776a0d7754 adds a fixed-value mathematical helper and focused checks. It has no Game, Tools CLI, live camera capture, runtime policy or renderer invocation. The installed binary remains16ff6aaa; no deployment or new game smoke is warranted for this unused CPU prerequisite. The warm preflight failed-positive history remains unchanged.

## Bound and caller authority

The caller must supply a successful immutable surface certificate, complete original/derived keys, packed hashes, selected IDs, threshold bits and directional/maximum error bits. It must separately prove the supplied local AABB encloses BOTH complete actual selected triangle unions. A single joined cut must provide actual camera-relative model transform, translation-free view, canonical WGPU projection, viewport and positive near plane, with matching model/camera/certificate generations. The helper compares the expected full values and copies the successful tuple; it does not authenticate a descriptor, examine geometry, pair live getters or establish the caller's envelope.

For a matching pair with local separation at most E, each camera component change is bounded by E times the corresponding composed affine row's L1 norm. Interval range evaluation establishes positive minimum depth z throughout the box. Per axis, quotient discrepancy is bounded by deltaX/z + maxAbsX*deltaZ/z². Actual projection scale and half viewport extent convert this to a coordinate discrepancy; the sum of the two axis bounds conservatively bounds Euclidean projected-set distance. This explicitly includes off-axis depth sensitivity and nonuniform, mirrored or singular affine transforms. It avoids assuming rotation-only distance preservation.

The entire box must be strictly beyond the near plane and fully inside the viewport. Intersection, equality, behind-camera or lateral clipping refuses rather than inheriting an invalid clipped-set distance. The theorem concerns ideal mathematical projection of selected packed geometry under the supplied affine tuple. Wind, animation, per-vertex deformation, shader floating-point, visibility, rasterization, occlusion, UVs, normals, topology, original-source fidelity, all rendering passes and pixel coverage remain outside scope.

## Numerical and work limits

Fixed arrays and bounded arithmetic compose the transforms, evaluate three box ranges and two quotient bounds. No metadata scan, source I/O or dynamic allocation is introduced. Float inputs must be finite normal-or-zero with magnitude at most10,000; viewport dimensions/origins are bounded to16,384 and dimensions positive. Error is finite normal-or-zero binary64 at most10,000. Exact IEEE bit classification precedes floating comparisons so DAZ cannot turn an accepted subnormal into zero. Volatile materializes individual operations and nextafter moves bounds outward. Uncertain zero, underflow, overflow or subnormal endpoints refuse; a nonzero starting result whose adjacent endpoint is flushed to zero also refuses. Fast-math or unsupported arithmetic formats refuse. This is not a captured global floating-environment or hard timing theorem.

Actual ConvertMatrix/ConvertProjectionMatrix row-memory conventions match the helper's indexing. Tests call those functions and then apply the existing manual WGPU infinite-far depth override. The certificate inputs in these projected tests are synthetic policy authority, not an actual producer certificate presented as live geometry. Independent long-double equation samples test a separate projection formulation; long double may equal double on Windows and is not a guaranteed higher-precision oracle.

## Executed evidence and open gates

The all-five build passed; its displayed schedule changed during reconfiguration, so no fixed step count is asserted. Focused projected/offline-certificate/CLI checks passed13,900 assertions in eleven cases without skips. Coverage includes actual conversions, viewport scaling, off-axis depth sensitivity, nonuniform transforms and rotations, transactional full-binding/generation/near/lateral/refusal, zero discrepancy and uncertain arithmetic. These checks establish the pure declared contract, not runtime rendering.

The earlier actual offline certificate's symmetric bound4.0000000000000027 is deliberately loose. It is not connected here to a captured actual camera tuple, and cannot be labelled an accepted screen-space error or policy. Startup sidecar work remains an unbuilt draft excluded from this checkpoint. Whole plan OPEN; defaultReady=false and performanceAccepted=false. No normal default changes or geometry residency/eviction/paging acceptance follow from this helper.

## Artifact manifest

| Artifact | Bytes | SHA256 |
|---|---:|---|
| build/build-projected-surface-bound-20260927.log | 25060 | `F912740536A1521C2A34FFFF895573CCAA88D31AB1EFA06A435EA42897E8890F` |
| build/test-projected-surface-bound-20260927.log | 192 | `46FA591DF531A4ABF62A81AE5E8A4B52EDE1C76FA73B4A0847610018518B2C63` |
| build/projected-bound-source-24283a81/engine/Poseidon/Graphics/Rendering/GeometryPageProjectedSurfaceBound.hpp | 11680 | `41D68F1BA58F02DD6BA56A745AC7756A953D6B428E6C3D5E29A14CCB7EC09D0B` |
| build/projected-bound-source-24283a81/tests/unit/engine/Poseidon/Graphics/Rendering/test_geometry_page_projected_surface_bound.cpp | 10749 | `C535FF53CEEFFA57359B81FD2A603600130AB7AED4725812D9CDF6AE44C803AC` |
| build/projected-bound-source-24283a81/tests/unit/engine/Poseidon/CMakeLists.txt | 35030 | `DC1C7FC00910AF37FC4A3CE23C624C5B927E72DBD37EF14B4CB1274EC2B5D2C4` |
