#pragma once

// PHY-010's acceptance test: register every object of the loaded world and report
// what would not convert.
//
// The plan's abort condition is "if stock CWA building Geometry LODs cannot be
// converted without per-model special cases, stop and report the evidence instead
// of working around individual models". Answering that needs the corpus, not a
// unit cube -- and it needs failures grouped by DISTINCT MODEL, because five
// hundred failures can be five hundred copies of one fence.
//
// Runs once per world load when POSEIDON_PHYSICS_CORPUS is set, and writes its
// report to the log. Off by default: it builds a physics world nothing else uses.

namespace Poseidon::Dev
{

/// True when POSEIDON_PHYSICS_CORPUS is set to anything but "0".
bool PhysicsCorpusRequested();

/// Walks the loaded landscape, registers terrain and every object, logs the
/// census. Safe to call with no world loaded -- it does nothing.
///
/// Also the way the world gets its colliders at all, so the dev panel calls it
/// from a button: requiring an environment variable before a ball can hit a wall
/// is a bad trade for anyone who just wants to start the game and try it.
void RunPhysicsCorpus();

/// True once RunPhysicsCorpus has registered a world, so the UI can say whether
/// the ball will hit buildings or only terrain.
bool PhysicsCorpusHasRun();

} // namespace Poseidon::Dev
