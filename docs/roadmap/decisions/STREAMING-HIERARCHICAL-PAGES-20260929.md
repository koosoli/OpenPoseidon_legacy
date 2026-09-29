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
all partial residency masks and corruption/refusal.

Installed `232e2da7` run `hierarchy-ram-sep29-20260929-224244-2d91bf`
exercised the four-group/twelve-cluster/four-page package: authored two-triangle
fallback, complete 64-triangle root, 512-triangle Fine, then reuse of the same
root model on the fixed instance. Only two pages were needed, eight mesh
records were retained and peak known charge was 153,448 bytes. Abort observed
all eight records Absent; the game exited normally. The runner nevertheless
returned failure because its final one-shot rearm check required Busy whereas
the source correctly returned Invalid/HierarchyAdmissionUnavailable with no
active request. The validator now accepts that exact safe refusal. The original
failure artifact remains; these reviewed steps are not a retroactive passing
verdict, device reclamation or performance acceptance.

The next integrated source supports exact source-bound intermediate/local
demands and a three-cut retained cache. A fourth distinct cut refuses before
changing demand or loading pages. `geometry-page-mlod --hierarchy-pages`
exports the same pinned bake as independently addressed `hierarchy.ghp`, with
fresh external producer source/package/prefix/file authority. The explicit
disk begin authenticates the captured prefix before publishing fallback; it
does not rebake or decode all Fine geometry. Step uses the existing charged
worker for one page, validates the frozen ticket/authority and waits for mesh
Present before selection. Metadata stubs retain topology, while accepted pages
populate a bounded cluster index-count cache. Abort retains real cancellation
debt until the worker exits.

A pure preparation policy unions up to forty explicitly supplied enabled
views with exact source and view epochs. Perspective and actual orthographic
shadow matrices use outward projection arithmetic. Unknown/near/clipped views
request Fine; stale/incomplete authority refuses. Its projected baked-error
indicator is a selection heuristic, not a certified surface or pixel bound,
and automatic runtime view collection remains open.

The game/tests/tools build passed after removing a heavy codec include from
Harness and fixing an ambiguous test helper name. The first focused suite
failed two fixture expectations: a single ancestry chain cannot produce a
branched local frontier, and a clipped second camera must force Fine. Corrected
tests now cover those facts plus a genuinely branched synthetic frontier.
The repaired focused suite passed 29,545 assertions in 33 cases, including
independent codecs, worker debt/cancellation, producer sealing/rollback, local
cuts and seven projection cases. Installed verification remains pending.

A separate pure release/refill preflight passed 83 assertions in four cases.
It validates a complete returned root, exact tokens, zero worker debt,
per-handle expected presence, unused model identity and refill headroom before
planning nonroot retirement. It has no runtime retirement consumer yet and
does not prove allocator release or all-view quality.

## Installed independent-page validation

The exact committed `a525fef8` EXE/DLL and producer passed installed WGPU
startup. The first disk/local runner stopped at page residency while the old
root cut was still selected and failed its distinct-local-cut check. The
failure artifact `hierarchy-disk-local-sep29-20260929-233715-a58424` remains.
Waiting for returned `HierarchyCutResident` fixes that harness race.
`hierarchy-disk-local-returned-sep29-20260929-234025-af5c78` then passed and
shut down normally: authored2/root64/local256/Fine512/root64 on one instance,
three independent page reads, no worker debt, root model reuse, invalid
external metadata/demand rejection and fourth-cut capacity refusal before
mutation. Abort observed all eleven historical handles Absent. The maximum
reported retained known charge was99,926 bytes; physical allocator bytes and
frame-time improvement were not measured by this fixture.

The next source integrates explicit source-bound `releaseHierarchyNonroot`
after a returned root cut. It queues nonroot model retirement before mesh
destruction, pins the complete root, tracks expected state per historical
handle and allows one fresh-ID refill using the never-used fourth model slot.
Observe is Busy during queued retirement to prevent superseding its token.
The new `-HierarchyRefill` runner checks stale-token refusal, returned root
preservation, exact old-handle absence and fresh IDs during disk reload.
This is an explicit private record-lifetime test, not automatic all-view
quality, device memory reclamation or retail policy. Build/runtime pending.

## Completed-frame demand tuples and registration precursor

The next source has a default-off CPU snapshot (`DEMAND_VIEWS=1` plus the
fixture flag):40 fixed rows/36 actual view IDs,8096-byte additive ABI. Enabled
families come from the existing applicability policy before resource gates;
missing matrices remain required/Unknown. It copies actual main, solar/local,
prepared reflection, retained interior and planned GI matrices/viewports.
Completion is published only after successful outer render return; attempted
failure/panic invalidates old completion. Camera knownness is separate from
COUNT and image freshness. The C++ fixture joins main matrices/origin against
the same immutable frame packet before exposing summaries. `-DemandViews`
checks the frozen masks and all required rows; no automatic policy consumes
these tuples yet.

A pure combined light-VP helper supports RH perspective and signed affine
matrices with outward homogeneous clip half-spaces. Unknown/clipped/outside
views still force Fine; the baked error remains a heuristic. A separate unused
resumable-registration helper requires a complete final-role plan, bounds
duplicate geometry, checks physical source/mount/lease freshness, permits one
texture upload/mesh admission per update, and keeps uncertain queue debt until
ordered retirement settlement. It does not split the synchronous resolver.
In particular, missing generated textures upload during ordinary Load, so the
physical-only helper cannot yet admit the complete tenement role set.

Game/tests/tools build passed after renaming a test helper that conflicted with
the engine Package type. The first focused suite safely refused a mismatched
test binding; correcting the omitted-view fixture made281 assertions/16 cases
pass. Rust snapshot tests first failed to compile due a negative-zero literal;
correcting it produced6/6 passing tests. Failed logs are preserved. Installed
release/refill and tuple collection remain pending at this source checkpoint.

Next work: independently addressable disk pages with authenticated metadata,
asynchronous preparation and cancellation debt, arbitrary intermediate runtime
cuts, automatic projected demand across applicable views, supported static
opaque retail source admission and world epochs, release/refill with measured
byte convergence, and comparable-quality traversal measurements. No default
activation or Nanite-equivalence claim follows from the private fixture.
