# Production mud GPU sampling and geometry checks

This test slice targets root's signed mud geometry implementation `96a0a811`. It changes no production shaders, terrain/collision code, weather or public layout. Root owns the serial GPU run, combined renderer tests, game build, installation and runtime acceptance.

`shaders/mud_geometry_tests.rs` imports actual production conform helpers and evaluates the terrain shader's actual helper declarations extracted verbatim from its source. This extraction makes an entry-point terrain shader testable with a small compute fixture; it does not substitute an independently rewritten sampler. The full terrain shader is also composed, both production helper bodies must match, and vertex/colour/prepass call sites are checked. Only conform's bind-group number is remapped for the default headless device; production function bodies are unchanged. Explicit non-filterable R32Float texture layouts match the heightmap's actual resource type.

The GPU fixture uses a flat 10m heightmap and the real storage contract: four snow header floats, 512² original snow deficits, four appended mud header floats, then 512² signed mud heights. An interior asymmetric four-cell depression has values −.01, −.02, −.04 and −.06m. At its midpoint the expected bilinear offset is −.0325m, its finite-difference gradient is (−.03, −.07), and sampled support is 9.9675m. These simple independent expectations falsify sign, array offset, source spacing and interpolation mistakes while requiring actual geometry and normals to consume the sampler.

Eight cases are run through both production paths:

1. Legacy snow-only buffer: exact zero mud, original nonzero snow retained.
2. Full signed appended mud view: expected depression, gradient and nontrivial upward unit normal.
3. Disabled mud header: exact zero deformation.
4. Unsupported cell size: exact zero deformation.
5. Excess negative source heights: bounded at the declared .06m depth limit.
6. Positive source heights: no terrain hills from a depression-only field.
7. Changed snow deficit only: snow changes while mud geometry remains identical.
8. Incomplete appended view: exact zero mud without out-of-bounds reads.

Every case additionally requires exact zero outside both sides of the mud window, finite outputs, normalized normals and terrain/conform agreement. Legacy/disabled/partial buffers must produce the unchanged up normal and flat support. Appending mud must preserve the first-half snow deficit and depth exactly.

Verification here: production helper extraction/parity/caller assertions passed in a fast source check; `rustfmt` parsed/formatted only the new module; `git diff --check` passed. The Rust composition and actual GPU tests were authored but **not run by this agent**, per root's test ownership. Root should run `cargo test -p wgpu_renderer mud_geometry -- --test-threads=1` (using the workspace's established package/target arguments if different) and the combined terrain/renderer suite before rebuilding.

Passing these tests establishes renderer helper/geometry behavior on synthetic storage, not actual foot-contact admission, installed collision parity, roads/ballistic triangle coverage or appearance. Installed walking contacts and scene captures must still establish those separately; do not claim a rendered geometric footprint from a helper readback alone.
