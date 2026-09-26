#pragma once

#include <Poseidon/World/Terrain/StaticSourceEnvelope.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Poseidon::Streaming
{
namespace StaticPlainRouteLimits
{
inline constexpr size_t PathBytes = 4096;
// ShapeBank rewrites sourcePath through lowName[128] before CfgModels lookup.
// Longer originals are unsupported instead of interpreted under another name.
inline constexpr size_t ShapeBankPathBytes = 128;
inline constexpr size_t StringBytes = 256;
inline constexpr size_t SourceProperties = 4096;
inline constexpr int RootEntries = 4096;
inline constexpr int ModelEntries = 4096;
inline constexpr int OverrideEntries = 128;
inline constexpr int PropertyElements = 256;
}

enum class StaticPlainSourceSummaryState : uint8_t { Unknown, Unsupported, SourceFacts };

// Compact immutable SOURCE facts only. No live config verdict, radius, Ready,
// constructor eligibility or disk freshness is certified. No IR/config pointer
// or heap payload is retained; string views borrow only this value's own arrays.
class StaticPlainSourceSummary
{
    uint64_t _generation = 0;
    StaticPlainSourceSummaryState _state = StaticPlainSourceSummaryState::Unknown;
    std::array<char, StaticPlainRouteLimits::ShapeBankPathBytes> _identity{};
    std::array<char, StaticPlainRouteLimits::ShapeBankPathBytes> _modelName{};
    uint8_t _identityBytes = 0;
    uint8_t _modelNameBytes = 0;
    friend StaticPlainSourceSummary BuildStaticPlainSourceSummary(const Model::Model&, const StaticSourceEnvelope&);
public:
    StaticPlainSourceSummaryState State() const { return _state; }
    uint64_t SourceGeneration() const { return _generation; }
    std::string_view ModelIdentity() const { return {_identity.data(), _identityBytes}; }
    std::string_view ConfigModelName() const { return {_modelName.data(), _modelNameBytes}; }
};

// Caller supplies an exclusively owned, immutable full audited ODOL IR and the
// envelope built from THAT snapshot. Generation is inventory/world identity,
// not a per-parse content revision. Do not pair a retained envelope with a later
// mutated/reparsed IR. Worker-safe: no live config, banks, renderer or vertices
// are scanned. Extract before publishing/sharing the IR with an owner.
StaticPlainSourceSummary BuildStaticPlainSourceSummary(
    const Model::Model& source, const StaticSourceEnvelope& evidence);
}
