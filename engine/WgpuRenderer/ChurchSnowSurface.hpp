#pragma once

#include "ObjectSnowSurface.hpp"
#include <string_view>

namespace Poseidon::ChurchSnowSurface
{
// Church::Animate transforms exactly these selections, regardless of weights.
inline constexpr std::array<std::string_view, 8> ClockNames = {
    "hodinova1", "minutova1", "hodinova2", "minutova2",
    "hodinova3", "minutova3", "hodinova4", "minutova4"};
inline constexpr unsigned NoObjectSnow = 128u; // existing per-section flags, no layout change

struct ClockMask
{
    std::array<unsigned char, ObjectSnowSurface::MaxProofVertices> animated{};
    int vertices = 0;
    bool valid = false;
};

template<typename ReadSize, typename ReadIndex>
ClockMask BuildMask(int vertices, ReadSize size, ReadIndex index)
{
    ClockMask result;
    if (vertices <= 0 || vertices > ObjectSnowSurface::MaxProofVertices) return result;
    result.vertices = vertices;
    int budget = ObjectSnowSurface::MaxProofVertices;
    for (int selection = 0; selection < int(ClockNames.size()); ++selection)
    {
        const int count = size(selection);
        if (count < 0 || count > budget) return result;
        budget -= count;
        for (int member = 0; member < count; ++member)
        {
            const int vertex = index(selection, member);
            if (vertex < 0 || vertex >= vertices) return result;
            result.animated[vertex] = 1;
        }
    }
    result.valid = true;
    return result;
}

inline bool SectionDisjoint(const ClockMask& mask, int begin, int end)
{
    if (!mask.valid || begin < 0 || end <= begin || end > mask.vertices) return false;
    for (int vertex = begin; vertex < end; ++vertex)
        if (mask.animated[vertex]) return false;
    return true;
}

// Actual face corners, not the bounding vertex span: stock Church roof vertices
// are interleaved with clock hands in the same vertex array.
template<typename ReadIndex>
bool CornersDisjoint(const ClockMask& mask, int corners, ReadIndex read)
{
    if (!mask.valid || corners <= 0 || corners > ObjectSnowSurface::MaxProofVertices) return false;
    for (int corner = 0; corner < corners; ++corner)
    {
        const int vertex = read(corner);
        if (vertex < 0 || vertex >= mask.vertices || mask.animated[vertex]) return false;
    }
    return true;
}

template<typename ReadIndex, typename ReadPair>
bool FixedCornersRigid(const ClockMask& mask, int corners, bool originalsValid,
                       ReadIndex readIndex, ReadPair readPair)
{
    if (!originalsValid || !CornersDisjoint(mask, corners, readIndex)) return false;
    for (int corner = 0; corner < corners; ++corner)
    {
        const int vertex = readIndex(corner);
        if (!ObjectSnowSurface::CurrentSectionRigid(vertex, vertex + 1,
            mask.vertices, true, readPair)) return false;
    }
    return true;
}

// A stored tag cannot license even neutral clock hands. Their material veto
// persists when the clock moves; fixed geometry must independently match source.
template<typename ReadPair>
bool FixedVerticesRigid(const ClockMask& mask, bool originalsValid, ReadPair read)
{
    if (!mask.valid || !originalsValid) return false;
    bool fixed = false;
    for (int vertex = 0; vertex < mask.vertices; ++vertex)
    {
        if (mask.animated[vertex]) continue;
        fixed = true;
        if (!ObjectSnowSurface::CurrentSectionRigid(vertex, vertex + 1,
            mask.vertices, true, read)) return false;
    }
    return fixed;
}
} // namespace Poseidon::ChurchSnowSurface
