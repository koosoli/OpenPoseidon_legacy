# Rain-water body direct-light visibility

Executed follow-up: body helper2 device tests and the actual complete-pool
visibility/admission device test pass. The21step combined game build exits0;
matching0a353f97 pair deployed15:01. Actual land fb33ab and occupied flight
163ebd pass normal stable exits. Focused retained-water shadow/open pixel
review is still required; the soil diagnostic will identify its actual layer.

Source candidate based on `1b002b92c8f5021f5a6df0c55ae20dc7ecf9d2bf`.
Shader composition, GPU probes, full build and installed runtime are pending
the main agent's serialized verification. No visual acceptance is claimed.

The existing fragment shaded its separate GGX solar interface using actual
CSM, terrain shadow and cloud transmittance. It evaluated body illumination
before those values were calculated, so direct body light survived beneath
the corresponding cast/cloud shadow.

The candidate computes that existing visibility once and passes the same
scalar to both components:

```
visibility = (1 - max(csm, terrainShadow)) * cloudTransmittance
bodyDirect = max(sunDiffuse, 0) * clamp(visibility, 0, 1)
interfaceDirect = sunDiffuse * PI * visibility
```

This is the same shadow/fog/cloud contract as the prior solar interface, with
the same actual world position and geometric-up receiver. The samples are
shared, not duplicated. When the physical-sky body has zero N.L and no solar
interface energy, the shadow path is skipped; zero sun radiance also skips it.
Legacy body lighting retains its existing 0.2 direct coefficient and therefore
still requests visibility when its direct source is present.

Physical sky irradiance/scale, legacy ambient, body reflectance
`(0.045,0.060,0.052)`, N.L, legacy coefficient, Beer attenuation, surface-depth
handoff, reflection/SSR, alpha, native bed/raster support, snow, source and roof
admission are unchanged. In particular, a shadow does not remove ambient,
sky or reflected energy, and clear visibility 1 reproduces the old body law.
The grey-green appearance of an optically deep two-metre pool is not regraded.

## Verification prepared

The existing resource-free actual body helper/composition test now requires
the production body call and GGX interface to use the shared visibility, with
only one CSM and one terrain-shadow callsite.

The existing small actual helper GPU grid expands from 12 to 20 cases. The
original captured/independent clear-light expectations are retained. Added
controls require:

- Zero visibility removes direct physical body light but preserves exact sky.
- Visibility 0.25 retains one quarter of the original direct contribution.
- Legacy ambient survives zero visibility, and legacy direct light scales by
  the same fraction.
- With no ambient/sky, full shadow makes body radiance zero while reflected
  composite energy remains positive; the clear counterpart has positive body.
- Bounded out-of-range visibility cannot add energy or create negative body.
- All added lighting controls leave physical optical opacity identical.

The existing actual full-fragment pool GPU fixture already provides a real
cloud-transmittance zero/clear comparison, checks that cloud shadow removes
direct energy, retains non-solar energy and leaves admission/alpha unchanged.
No additional large device fixture or resources are introduced.

Executed source checks: `git diff --check` passes, `rustfmt --emit stdout` parses
the edited Rust test with exit 0 without changing the file, and a source search
confirms both existing body-helper callsites supply the new argument.
No compiler or device was run during the main agent's current runtime window.

Required focused checks after integration:

```powershell
cargo test --manifest-path engine/WgpuRenderer/rust/Cargo.toml rain_water::body_tests:: -- --nocapture
cargo test --manifest-path engine/WgpuRenderer/rust/Cargo.toml actual_gpu_complete_pool_draw_visibility_and_admission -- --nocapture
```

The main agent must also run normal installed-game startup and native pool
captures in a shadowed/open scene. This correction does not itself establish
reflection quality or fine-water performance.
