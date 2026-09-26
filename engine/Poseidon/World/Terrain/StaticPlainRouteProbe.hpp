#pragma once

#include <Poseidon/World/Terrain/StaticSourceEnvelope.hpp>
#include <cstddef>

namespace Poseidon { class ParamEntry; }

namespace Poseidon::Streaming
{
enum class StaticPlainRouteObservation
{
    EmptyClassNow, Unsupported, Unknown, WrongThread, StaleSource
};

namespace StaticPlainRouteLimits
{
inline constexpr size_t PathBytes = 4096;
// ShapeBank rewrites sourcePath through lowName[128] before CfgModels lookup.
// Longer originals are unsupported rather than interpreted under another name.
inline constexpr size_t ShapeBankPathBytes = 128;
inline constexpr size_t StringBytes = 256;
inline constexpr size_t SourceProperties = 4096;
inline constexpr int RootEntries = 4096;
inline constexpr int ModelEntries = 4096;
inline constexpr int OverrideEntries = 128;
inline constexpr int PropertyElements = 256;
}

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
}
