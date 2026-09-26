#pragma once

#include <Poseidon/World/Terrain/StaticPlainSourceSummary.hpp>
#include <cstddef>

namespace Poseidon { class ParamEntry; }

namespace Poseidon::Streaming
{
enum class StaticPlainRouteObservation
{
    EmptyClassNow, Unsupported, Unknown, WrongThread, StaleSource
};

// The caller must supply the SAME immutable full ODOL IR from which evidence
// was built, and its current source generation. Evidence is not an IR identity
// hash. This function never scans vertices, loads a bank, or creates an Object.
//
// EmptyClassNow observes the live injected config only within this non-yielding
// owner operation. It is NOT CertifiedPlain/Ready, must not be cached or used
// after returning to another owner operation, and proves no adapter, placement,
// collision or constructor eligibility. No config pointer escapes. Inheritance
// and unsupported encodings are refused instead of approximated.
// ParamEntry exposes already-interned C strings, not original config bytes:
// bytes discarded by parsing/interning (including a trailing NUL suffix) cannot
// be audited here. The config observation is of the engine's visible strings.
StaticPlainRouteObservation ProbeStaticPlainRouteNow(
    const Model::Model& source, const StaticSourceEnvelope& evidence,
    uint64_t currentGeneration, const ParamEntry& liveConfigRoot);

// Equivalent immediate config observation without retaining the original IR.
// Caller supplies the exact case-sensitive inventory identity and generation
// associated with the original snapshot; aliases and stale tokens refuse.
// These tokens do NOT establish freshness of modified files or later IRs.
StaticPlainRouteObservation ProbeStaticPlainRouteNow(
    const StaticPlainSourceSummary& source, uint64_t currentGeneration,
    std::string_view exactModelIdentity, const ParamEntry& liveConfigRoot);
}
