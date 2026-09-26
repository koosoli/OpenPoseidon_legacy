#pragma once

#include <cstddef>
#include <cstdint>

namespace Poseidon::Streaming
{
inline constexpr uint64_t SimulationMaxMetadataPlacements = 4000000;
inline constexpr uint64_t SimulationMaxMetadataModels = 8192;
inline constexpr uint64_t SimulationMaxMetadataRecordBytes = 4 * 1024 * 1024;
enum class SimulationMetadataRefusal { None, PlacementCount, ModelCount, RecordBytes };
struct SimulationMetadataAllocation
{
    SimulationMetadataRefusal refusal = SimulationMetadataRefusal::None;
    uint64_t recordBytes = 0;
};

// Fixed record storage only: excludes allocator overhead, existing landscape
// placement/cell storage, sparse map nodes and all active shape/source payload.
// Division precedes multiplication, including untrusted size/overflow cases.
inline SimulationMetadataAllocation PreflightSimulationMetadata(uint64_t placements, uint64_t models, uint64_t recordSize)
{
    if (placements > SimulationMaxMetadataPlacements) return {SimulationMetadataRefusal::PlacementCount, 0};
    if (models > SimulationMaxMetadataModels) return {SimulationMetadataRefusal::ModelCount, 0};
    if (!recordSize || models > SimulationMaxMetadataRecordBytes / recordSize)
        return {SimulationMetadataRefusal::RecordBytes, 0};
    return {SimulationMetadataRefusal::None, models * recordSize};
}
} // namespace Poseidon::Streaming
