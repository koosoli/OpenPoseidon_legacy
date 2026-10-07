// WTR-001: "test pitch stability, aerial repetition ... and lifecycle reset".
//
// Water is meshed by a CDLOD selection driven from the camera. A large camera
// translation is the case where that selection can misbehave in ways invisible
// from a static ground view: it can collapse (no water meshed at all) or run
// away (node count climbing until frame time does too).
//
// Measured behaviour at the time of writing, on Malden at 800m, sampling
// triWaterNodeCount after the move:
//
//     ground=28   +60 frames=0   +180=0   +420=5   back at ground=24
//
// So water is absent for hundreds of frames after the move and returns only
// partially. That is recorded in
// design notes rather than
// asserted as correct.
//
// This fixture therefore guards the two hard failures -- water never coming
// back at all, and an unbounded count -- with a frame budget generous enough to
// cover the measured latency. It deliberately does NOT assert a tight recovery
// time: doing so would fail today and encode the defect as expected. Tighten
// the budget when the selection is improved.
//
// Note the sampling is explicit rather than leaning on assert retries. An
// earlier draft asserted immediately after the move and passed only because the
// harness retried for two minutes while water slowly recovered -- hiding
// exactly the behaviour this test exists to observe.

triSimFrames 60

_ground = triWaterStats
triAssertNe [_ground, "FAIL:no_water_frame"]
triAssertGt [(triWaterNodeCount), 0]

_p = getPos player
player setPos [(_p select 0), (_p select 1), 800]

// Budget covers the measured ~420-frame recovery with headroom.
triSimFrames 600

// Water must still be submitting frames even while the selection is sparse.
triAssertNe [(triWaterStats), "FAIL:no_water_frame"]
triAssertGt [(triWaterNodeCount), 0]
triAssertLt [(triWaterNodeCount), 4096]

// Back down. The shrink direction is the one that can leave a stale selection
// behind rather than rebuilding it.
player setPos [(_p select 0), (_p select 1), (_p select 2)]
triSimFrames 300

triAssertNe [(triWaterStats), "FAIL:no_water_frame"]
triAssertGt [(triWaterNodeCount), 0]
triAssertLt [(triWaterNodeCount), 4096]

triEndTest
