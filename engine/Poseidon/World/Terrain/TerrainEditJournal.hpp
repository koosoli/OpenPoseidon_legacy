#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace Poseidon
{
// First-write snapshot of edited samples only. Camera movement never evicts
// entries: resetting terrain must not depend on which island is currently visible.
class TerrainEditJournal
{
public:
    static constexpr int TileSize = 16;
    static constexpr std::size_t MaxTiles = 65536;

    bool Empty() const { return _samples == 0; }
    std::size_t Samples() const { return _samples; }
    std::size_t Tiles() const { return _tiles.size(); }
    void Clear()
    {
        decltype(_tiles){}.swap(_tiles);
        _samples = 0;
    }

    template<class ReadHeight>
    bool Remember(int range, int x, int z, int width, int height, ReadHeight read)
    {
        if (range < 2 || x < 0 || z < 0 || width < 1 || height < 1 ||
            width > range || height > range || x > range-width || z > range-height)
            return false;
        const int lastX = (x+width-1)/TileSize, lastZ = (z+height-1)/TileSize;
        std::size_t needed = 0;
        for (int tz = z/TileSize; tz <= lastZ; ++tz)
            for (int tx = x/TileSize; tx <= lastX; ++tx)
                if (_tiles.find(Key(tx,tz)) == _tiles.end() && ++needed > MaxTiles-_tiles.size())
                    return false;
        for (int iz = z; iz < z+height; ++iz)
            for (int ix = x; ix < x+width; ++ix)
            {
                auto& tile = _tiles[Key(ix/TileSize,iz/TileSize)];
                const int offset = (iz%TileSize)*TileSize+ix%TileSize;
                if (tile.saved.test(offset)) continue;
                tile.heights[offset] = read(ix,iz);
                tile.saved.set(offset);
                ++_samples;
            }
        return true;
    }

    template<class WriteHeight>
    void ForEach(WriteHeight write) const
    {
        for (const auto& [key,tile] : _tiles)
        {
            const int x = static_cast<int>(key >> 32)*TileSize;
            const int z = static_cast<int>(key & 0xffffffffu)*TileSize;
            for (int i = 0; i < TileSize*TileSize; ++i)
                if (tile.saved.test(i)) write(x+i%TileSize,z+i/TileSize,tile.heights[i]);
        }
    }

private:
    struct Tile
    {
        std::array<float,TileSize*TileSize> heights{};
        std::bitset<TileSize*TileSize> saved;
    };
    static std::uint64_t Key(int x, int z)
    {
        return (std::uint64_t(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(z);
    }
    std::unordered_map<std::uint64_t,Tile> _tiles;
    std::size_t _samples = 0;
};
}
