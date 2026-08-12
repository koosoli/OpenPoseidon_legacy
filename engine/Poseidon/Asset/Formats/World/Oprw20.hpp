#pragma once

#include <Poseidon/Asset/Formats/World/Oprw25.hpp>

namespace Poseidon::Asset::Formats::World
{

// Armed Assault (Arma 1) profile: OPRW revisions 18 and 20.
//
// Arma 1 sits between the OFP container the legacy WrpReader handles and the
// Arma 2 container Oprw24 handles, and reading it says where each of the later
// changes actually happened. Measured against the three shipped Sahrani worlds:
//
//   sara.wrp       rev 20   512 land / 2048 terrain @ 40 m   20,480 m
//   sara_dbe1.wrp  rev 18   512 land / 2048 terrain @ 40 m   20,480 m
//   saralite.wrp   rev 18   256 land / 1024 terrain @ 40 m   10,240 m
//
// Revisions 18 and 20 are byte-identical in layout on this corpus -- the same
// reader consumes all three files exactly -- so they share one code path and
// differ only in which value the header is allowed to carry.
//
// Against revision 24 there are exactly four differences, all gated inside
// ReadOprwModern:
//
//   1. Bulk arrays are LZSS (SSCompress), not LZO. Revision 23 made that switch.
//   2. A dense per-land-cell 16-bit randomization array sits between the material
//      index quadtree and the terrain grids -- the same array, in the same place
//      in the field order, that the OFP-era Landscape::Serialize wrote as
//      `_random`. Revision 24 dropped it and regenerates it at load time.
//   3. There is one per-terrain-cell byte array before the elevation grid where
//      revision 24 has two.
//   4. Road links have no per-connection type byte.
//
// Everything else -- the two-resolution header, all four quadtrees, the terrain
// material table with its trailing "major" byte, the model table, the static
// entity records, the object/map-info offset quadtrees and their byte counts, the
// persistent and subdivision arrays, and the 60-byte object record -- is already
// exactly the revision-24 layout in revision 18. Arma 1 is where this container
// arrived, not a halfway house.
//
// Confirmation is the same standard WLD-010 held revision 25 to, and it is not
// "it did not crash". All three worlds consume to the byte: the road network's
// declared size matches what walking its per-cell lists consumes (sara 2,191,865;
// sara_dbe1 2,260,476; saralite 835,019), what remains after the object table
// matches the declared map-info size, the object table divides exactly by 60, and
// no placement references a model index outside the model table. On top of that
// every LZSS payload verifies its own trailing checksum, so a wrong array size is
// caught at the array rather than several fields downstream.
using Oprw20Header = Oprw25Header;
using Oprw20Object = Oprw25Object;
using Oprw20StaticEntity = Oprw25StaticEntity;
using Oprw20RoadLink = Oprw25RoadLink;
using Oprw20World = Oprw25World;
using Oprw20Geography = Oprw25Geography;

inline Oprw20World ReadOprw20(BinaryReader& reader)
{
    return ReadOprwModern(reader, 20);
}

inline Oprw20World ReadOprw18(BinaryReader& reader)
{
    return ReadOprwModern(reader, 18);
}

inline bool IsOprw20(BinaryReader& reader)
{
    return IsOprwRevision(reader, 20);
}

inline bool IsOprw18(BinaryReader& reader)
{
    return IsOprwRevision(reader, 18);
}

// Either Arma 1 revision. Callers that dispatch on generation rather than on an
// exact revision want this; callers that mean one revision exactly want the pair
// above, which refuse the other one.
inline bool IsOprwArma1(BinaryReader& reader)
{
    const int32_t revision = PeekOprwRevision(reader);
    return revision == 18 || revision == 20;
}

} // namespace Poseidon::Asset::Formats::World
