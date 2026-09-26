#pragma once

namespace Poseidon::Streaming
{

// The placement table owns authored IDs even when no Object is currently resident.
// Scan only at cold save/reset/rebuild boundaries, never for each NewObjectID call.
template<class Placements>
int AuthoredObjectIdFloor(bool streaming, const Placements& placements)
{
    int maximum = -1;
    if (streaming)
        for (const auto& placement : placements)
            if (placement.id > maximum)
                maximum = placement.id;
    return maximum;
}

} // namespace Poseidon::Streaming
