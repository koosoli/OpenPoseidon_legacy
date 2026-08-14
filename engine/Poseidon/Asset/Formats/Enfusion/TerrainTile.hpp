#pragma once

#include <Poseidon/Asset/Formats/Enfusion/EnfusionIff.hpp>

#include <cstdint>
#include <string>
#include <vector>

// Enfusion's terrain: the `.terr` world descriptor and the `.ttile` height tiles.
//
// Both are the IFF dialect in EnfusionIff.hpp and both use the form type `TERR`.
//
// `.terr` -- one per terrain, at `worlds/<W>/<T>/<T>.terr`
//   VERS 4    u32 version (9 on every shipped world; 5 and 7 on editor ones)
//   HEAD 32   u32 gridW, u32 gridH, u32 (33 on all 11 local terrains),
//             u32 subPerTileEdge, f32 cellSize, f32 heightScale, f32 heightOffset,
//             u32 (same value as VERS)
//   TEXS 6    three u16 texture sizes
//   MATS n    [u16 on version 9] then repeated <u32 len><len bytes> of
//             "{GUID}Terrains/Common/Surfaces/X.emat", NUL included in len.
//             LRS2 and TMAT index this list 0-based.
//
// `.ttile` -- one per tile, at `<T>/.Data/<T>_<index>.ttile`
//   VERS 4        u32 9
//   HGHT 2*n*n    n*n u16 little-endian, ROW-MAJOR with X fastest, n = tileEdge+1.
//   BERR s*s*36   per sub-cell min/max in raw units plus 7 LOD error metrics.
//   LRS2, TMAT    per-sub-cell material palette and assignment.
//
// heightScale and heightOffset are NOT per-world: they are byte-identical on all 11
// terrains in the local install (1/32 and -204.78125), so sea level is exactly raw
// 6553. They are still read from the file rather than hard-coded, because "constant
// across the corpus I have" is not the same as "constant".
//
// The row order is measured, not assumed, and the evidence is strong: a tile's last
// row and column are the shared edge with its neighbour and are byte-identical
// there. Over the whole install that is 10,308 of 10,308 shared edges matching,
// while the transposed interpretation matches 0 of 4,900 on Everon alone with a
// maximum disagreement of 6,502 raw units. Tile index i sits at
// (row, col) = (i / tilesX, i % tilesX).

namespace Poseidon::Asset::Formats::Enfusion
{

struct TerrainDescriptor
{
    uint32_t version = 0;
    uint32_t gridWidth = 0;  //!< height samples across, == tilesX * tileEdge + 1
    uint32_t gridHeight = 0;
    uint32_t subPerTileEdge = 0;
    float cellSize = 0.0f;     //!< metres between height samples
    float heightScale = 0.0f;  //!< metres per raw unit
    float heightOffset = 0.0f; //!< metres added after scaling
    //! Terrain surface materials, GUID stripped, indexed 0-based by LRS2/TMAT.
    std::vector<std::string> materials;
    std::string error;

    bool valid() const { return error.empty() && gridWidth > 0 && gridHeight > 0; }

    float HeightAt(uint16_t raw) const { return static_cast<float>(raw) * heightScale + heightOffset; }
    //! World extent in metres along X.
    float WorldWidth() const { return static_cast<float>(gridWidth - 1) * cellSize; }
    float WorldHeight() const { return static_cast<float>(gridHeight - 1) * cellSize; }
};

//! One tile's heightfield, still in raw units. Kept raw because the scale lives in
//! the descriptor and because the raw values are what the edge-continuity check
//! compares -- converting first would make that test depend on float equality.
struct TerrainTile
{
    uint32_t samples = 0; //!< per edge, i.e. tileEdge + 1
    std::vector<uint16_t> heights;
    std::string error;

    bool valid() const { return error.empty() && samples > 0; }
    uint16_t At(uint32_t x, uint32_t z) const { return heights[static_cast<size_t>(z) * samples + x]; }
};

inline TerrainDescriptor ReadTerrainDescriptor(const void* data, size_t size)
{
    TerrainDescriptor out;
    const IffFile iff = ReadIff(data, size);
    if (!iff.valid())
    {
        out.error = iff.error;
        return out;
    }
    if (iff.formType != FourCC("TERR"))
    {
        out.error = "form type is '" + iff.formType.ToString() + "', not 'TERR'";
        return out;
    }
    const auto* base = static_cast<const uint8_t*>(data);

    if (const IffChunk* vers = iff.Find(FourCC("VERS")); vers && vers->size >= 4)
        out.version = ReadLe32(base + vers->offset);

    const IffChunk* head = iff.Find(FourCC("HEAD"));
    if (!head || head->size < 32)
    {
        out.error = "no 32-byte HEAD chunk";
        return out;
    }
    const uint8_t* h = base + head->offset;
    out.gridWidth = ReadLe32(h, 0);
    out.gridHeight = ReadLe32(h, 4);
    out.subPerTileEdge = ReadLe32(h, 12);
    out.cellSize = ReadLeF32(h, 16);
    out.heightScale = ReadLeF32(h, 20);
    out.heightOffset = ReadLeF32(h, 24);
    if (out.gridWidth == 0 || out.gridHeight == 0 || out.cellSize <= 0.0f || out.heightScale <= 0.0f)
    {
        out.error = "HEAD declares a degenerate grid";
        return out;
    }

    if (const IffChunk* mats = iff.Find(FourCC("MATS")))
    {
        size_t at = mats->offset;
        const size_t end = mats->offset + mats->size;
        // Version 9 prefixes the list with a u16; earlier revisions do not.
        if (out.version >= 9 && at + 2 <= end)
            at += 2;
        while (at + 4 <= end)
        {
            const uint32_t length = ReadLe32(base, at);
            at += 4;
            if (length == 0 || at + length > end)
                break;
            // The stored length includes the terminating NUL.
            std::string reference(reinterpret_cast<const char*>(base + at), length);
            while (!reference.empty() && reference.back() == '\0')
                reference.pop_back();
            at += length;
            // "{GUID}path" -- the engine has no GUID registry, and the path is the
            // half that resolves.
            if (!reference.empty() && reference.front() == '{')
            {
                const size_t close = reference.find('}');
                if (close != std::string::npos)
                    reference = reference.substr(close + 1);
            }
            out.materials.push_back(std::move(reference));
        }
    }
    return out;
}

inline TerrainTile ReadTerrainTile(const void* data, size_t size)
{
    TerrainTile out;
    const IffFile iff = ReadIff(data, size);
    if (!iff.valid())
    {
        out.error = iff.error;
        return out;
    }
    if (iff.formType != FourCC("TERR"))
    {
        out.error = "form type is '" + iff.formType.ToString() + "', not 'TERR'";
        return out;
    }
    const IffChunk* hght = iff.Find(FourCC("HGHT"));
    if (!hght)
    {
        out.error = "no HGHT chunk";
        return out;
    }
    // The tile edge is not stored anywhere: it is derived from the chunk size, and
    // it varies across the corpus (128 cells on the shipped worlds, 256 and 64 on
    // editor ones). Requiring a perfect square is what keeps a wrong derivation
    // from silently producing a skewed tile.
    const size_t count = hght->size / 2;
    uint32_t samples = 1;
    while (static_cast<size_t>(samples) * samples < count)
        ++samples;
    if (static_cast<size_t>(samples) * samples != count || hght->size % 2 != 0)
    {
        out.error = "HGHT of " + std::to_string(hght->size) + " bytes is not a square of u16 samples";
        return out;
    }
    out.samples = samples;
    out.heights.resize(count);
    std::memcpy(out.heights.data(), static_cast<const uint8_t*>(data) + hght->offset, hght->size);
    return out;
}

} // namespace Poseidon::Asset::Formats::Enfusion
