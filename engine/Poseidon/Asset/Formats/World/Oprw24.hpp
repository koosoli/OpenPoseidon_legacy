#pragma once

#include <Poseidon/Asset/Formats/World/Oprw25.hpp>

namespace Poseidon::Asset::Formats::World
{

// Arma 2 / Operation Arrowhead profile. Revision 24 shares every payload field
// with revision 25; it simply predates the latter's app-id header field. Exposing
// a named entry point prevents callers from accidentally accepting a nearby
// revision just because its data happens to parse.
using Oprw24Header = Oprw25Header;
using Oprw24Object = Oprw25Object;
using Oprw24StaticEntity = Oprw25StaticEntity;
using Oprw24RoadLink = Oprw25RoadLink;
using Oprw24World = Oprw25World;
using Oprw24Geography = Oprw25Geography;

inline Oprw24World ReadOprw24(BinaryReader& reader)
{
    return ReadOprwModern(reader, 24);
}

inline bool IsOprw24(BinaryReader& reader)
{
    return IsOprwRevision(reader, 24);
}

} // namespace Poseidon::Asset::Formats::World
