# Independent hierarchical geometry pages

Work continues on `codex/streaming-residency`. The earlier `.gcd` codec stores
two selected flattened cuts; it cannot reconstruct the intermediate CLOD DAG.
The new typed RAM package preserves every group/refined edge, exact SVertex
attributes, original vertex IDs and local indices from the pinned bake. It
binds source/configuration/library/topology/payload into a package digest.
Groups, clusters, pages and known owned C++ capacity are bounded; terminal-root
pages are fallback pins. The pure planner supports complete object-space
threshold cuts and retains a complete root cut during partial residency.

An explicit `beginOriginalHierarchy` fixture consumes one authenticated,
immutable original-source snapshot. The authored coarse fallback remains
visible while `stepHierarchy` stages at most one RAM page. Pages become
resident only after owner-bound mesh Present observations. `rootHierarchy`
and `fineHierarchy` publish a complete model on the existing instance only
when every required page is resident. Returning to root reuses its model and
all uploaded page handles. Abort clears pending work and retires all owned
records through the existing queue and all-Absent observation.

The private fixture caps known retained/admission/staging metadata plus old
worker reservations at 1 MiB and mesh history at 64. These charges do not
describe allocator overhead, physical GPU slabs, queue-retirement memory or
frame-time bounds. Record absence is not device-memory reclamation. The source
bake runs once in the explicit fixture; it is not an automatic retail path.
The independent `Test-OriginalGeometryPages.ps1 -Hierarchy` runner checks
authored fallback, one-page increments, root/fine/root identity and retirement.
The paired desktop build passed after correcting one existing warm-worker
call for the added preparation parameter. Five focused hierarchical tests
passed with 3,921 assertions, including intermediate cuts, exact attributes,
all partial residency masks and corruption/refusal. Runtime verification is
pending at this checkpoint.

Next work: independently addressable disk pages with authenticated metadata,
asynchronous preparation and cancellation debt, arbitrary intermediate runtime
cuts, automatic projected demand across applicable views, supported static
opaque retail source admission and world epochs, release/refill with measured
byte convergence, and comparable-quality traversal measurements. No default
activation or Nanite-equivalence claim follows from the private fixture.
