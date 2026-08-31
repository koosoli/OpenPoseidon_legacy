#pragma once

// SIM-814. `SimStageGraph.hpp` says of its resource declarations:
//
//     "Honest limit: the reads/writes are hand-audited from the call sites, not derived by
//      the compiler and not enforced at runtime."
//
// A declaration nobody verifies is a comment. This file is what turns the six stages'
// declarations into something a test can FAIL on.
//
// The mechanism is deliberately not a runtime recorder. No unit test in this repo can
// construct a `World` -- there is no game data on a build machine -- so an instrumented
// build would report "no violations" in CI when it meant "did not run", which is the
// documented failure mode this project has hit four times in one night. Instead the
// evidence is a CALL PATH, checked against the source text.
//
// Each entry says: stage S reads (or writes) resource R, and here is the chain of calls
// from S's entry point in `World::StepSimulation` to the statement that touches R, hop by
// hop, as file + enclosing symbol + the text that must appear inside it. A test walks
// every hop. When somebody deletes, renames or moves a call in that chain, the hop stops
// matching and the test says which stage, which resource, and which hop broke.
//
// Two things this buys that reading the call sites again does not:
//
//   * The DEPTH is recorded. Three of the six stages reach a resource through five or more
//     layers -- the AI's visibility read and its vehicle write are nine hops each -- and it
//     is the deep reach that a hand-written declaration loses track of. Every bit SIM-814
//     found missing was at hop 2 or deeper; every bit at hop 0 or 1 was already correct.
//   * Widening is now a written decision. SIM-811 deliberately over-declared where a
//     stage's reach was uncertain; `SimStageWidenings()` keeps that policy but requires a
//     reason next to each widened bit, so "wider to be safe" and "nobody has looked" stop
//     looking the same.
//
// What it does NOT do is notice a NEW touch nobody recorded. For three resources whose
// access points are rare enough to enumerate, `SimResourceCensus()` closes that gap by
// requiring every occurrence in `engine/Poseidon` to be accounted for. For Clock,
// VehicleState and AIState the witnesses (`Glob.time`, `SetPosition`, ...) run to
// thousands of sites and no census is affordable; that is stated rather than papered over.

#include "SimStageGraph.hpp"

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

namespace Poseidon::Sim
{

/// One link in a call chain.
///
/// Read as: inside `file`, in the body of the definition whose line contains `symbol`,
/// the text `contains` appears.
struct SimEvidenceHop
{
    /// Path under `engine/Poseidon`, forward slashes.
    std::string_view file;
    /// A distinctive slice of the enclosing definition's line, e.g.
    /// `"void World::SimulateLandscape("`. Must appear at the start of a line so a
    /// declaration in a header-style listing cannot be mistaken for the definition.
    std::string_view symbol;
    /// Text that must appear inside that definition's body.
    std::string_view contains;
    /// True when the call this hop makes is resolved at run time -- a member function
    /// pointer, or the SQF command table -- so the NEXT hop's symbol cannot be checked
    /// against this hop's `contains`. The linkage check skips exactly these, and only
    /// these, and they are counted so a table that silently became all-dynamic is visible.
    bool indirect = false;
};

inline constexpr std::size_t kMaxEvidenceHops = 12;

/// One (stage, resource, direction) bit, with the path that justifies it.
struct SimEvidence
{
    SimStage stage{};
    SimResource resource{};
    /// False = the stage reads the resource, true = it writes it.
    bool write = false;

    std::size_t hopCount = 0;
    std::array<SimEvidenceHop, kMaxEvidenceHops> hops{};

    /// Why this path matters, in one line. Shown when a hop fails.
    std::string_view note;
};

/// Every evidenced bit. The declarations in `SimStageGraph.cpp` must be a superset of the
/// bits named here, and every bit they add beyond it must appear in `SimStageWidenings()`.
[[nodiscard]] const std::vector<SimEvidence>& SimStageEvidence();

/// A declared bit that no evidence path supports, kept on purpose.
///
/// SIM-811's rule: "an over-declared read produces a spurious edge, which somebody
/// investigates and removes, whereas an under-declared read produces a missing edge, which
/// nobody ever notices." Widening stays allowed. What is no longer allowed is widening
/// without a reason anybody can read.
struct SimDeclWidening
{
    SimStage stage{};
    SimResource resource{};
    bool write = false;
    std::string_view reason;
};

[[nodiscard]] const std::vector<SimDeclWidening>& SimStageWidenings();

/// One accounted-for occurrence of a census witness.
struct SimCensusSite
{
    /// Path under `engine/Poseidon`, forward slashes.
    std::string_view file;
    /// A slice of the line the witness appears on. Empty means "every occurrence in this
    /// file", which is used only for files that are wholly outside a tick.
    std::string_view line;
    /// The stage that reaches this site during `World::StepSimulation`.
    /// `inTick == false` means no stage does, and `reason` says why.
    bool inTick = false;
    SimStage stage{};
    bool write = false;
    std::string_view reason;
};

/// A resource whose access points are few enough to enumerate exhaustively.
///
/// For these the gate is two-sided. Every non-comment occurrence of `witness` anywhere
/// under `engine/Poseidon` must match a listed site; an unlisted one fails the test with
/// its file, line number and text. A site marked `inTick` additionally requires its stage
/// to declare `resource`, so a NEW call added inside a stage's reach cannot be classified
/// without the declaration widening to match.
struct SimResourceCensusEntry
{
    SimResource resource{};
    /// The exact text searched for. Chosen to be the chokepoint, not a common accessor.
    std::string_view witness;
    std::vector<SimCensusSite> sites;
};

[[nodiscard]] const std::vector<SimResourceCensusEntry>& SimResourceCensus();

/// The resources the census covers. Anything not here is declared on evidence alone, and
/// a new touch of it will not be caught.
[[nodiscard]] std::uint32_t CensusedResources();

} // namespace Poseidon::Sim
