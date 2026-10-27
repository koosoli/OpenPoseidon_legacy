// SPDX-License-Identifier: GPL-3.0-or-later
// Production, the commander view's side of it (ConquestReferee.hpp): included by ConquestReferee.cpp inside
// namespace Poseidon::Conquest, after the anonymous namespace (ConquestRefereeBuild.inc holds the rules).

std::vector<StructureType> StructureTypes()
{
    std::vector<StructureType> out;
    for (const StructureDef& d : StructureDefs)
        out.push_back({d.name, d.cost, d.seconds, d.unlocks});
    return out;
}

float SquadBuildSeconds(int type)
{
    const Referee& r = R();
    if (type < 0 || type >= static_cast<int>(r.config.squads.size()))
        return 0.0f;
    Build b;
    b.type = type;
    return BuildSeconds(r, b);
}

std::string SquadMissing(int base, int type)
{
    const Referee& r = R();
    if (base < 0 || base >= static_cast<int>(r.war.defs.size()))
        return std::string();
    return MissingFor(r, base, type);
}

std::string QueueAt(int base, int type, bool structure)
{
    Referee& r = R();
    if (!IsMirror())
        return QueueBuild(r, r.playerSide, base, type, structure, true);
    // A client: checked here as far as it can be, then asked of the server.
    if (std::string why = QueueRefusal(r, r.playerSide, base, type, structure); !why.empty())
        return why;
    GameValue args = NetArray();
    GameArrayType& a = args;
    a.Add(GameValue(static_cast<float>(base)));
    a.Add(GameValue(static_cast<float>(type)));
    a.Add(GameValue(structure));
    Request("buy", args);
    return std::string();
}

bool CancelAt(int base, int type, bool structure)
{
    Referee& r = R();
    if (!IsMirror())
        return CancelBuild(r, r.playerSide, base, type, structure);
    GameValue args = NetArray();
    GameArrayType& a = args;
    a.Add(GameValue(static_cast<float>(base)));
    a.Add(GameValue(static_cast<float>(type)));
    a.Add(GameValue(structure));
    Request("cancel", args);
    return true;
}

std::string PlaceAt(int base, int kind, float x, float y)
{
    Referee& r = R();
    if (base < 0 || base >= static_cast<int>(r.war.defs.size()))
        return "no such base";
    if (!IsMirror())
        return PlaceBuilt(r, r.playerSide, base, kind, x, y);
    if (std::string why = PlacementRefusal(r, base, x, y); !why.empty())
        return why;
    GameValue args = NetArray();
    GameArrayType& a = args;
    a.Add(GameValue(static_cast<float>(base)));
    a.Add(GameValue(static_cast<float>(kind)));
    a.Add(GameValue(x));
    a.Add(GameValue(y));
    Request("place", args);
    return std::string();
}
