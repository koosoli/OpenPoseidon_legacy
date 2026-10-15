// SPDX-License-Identifier: GPL-3.0-or-later
// RFG-157: test commands for the native Enfusion doors (World/Terrain/EnfusionDoors.hpp).
//
//   triEnfusionDoor [x, z, radius]   -> "index leafId open phase angle hx hy hz ax ay az members"
//                                        of the door nearest (x, z), or "NONE"
//   triEnfusionDoorSet [index, open] -> "OK" / "REFUSED"; opens (1) or closes (0) one door
//   triEnfusionDoorCensus            -> "doors N parts M"

#include <Evaluator/express.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace Poseidon;

GameValue TriEnfusionDoor(const GameState*, GameValuePar arg)
{
    const GameArrayType& args = arg;
    if (!GLandscape || args.Size() < 2 || args[0].GetType() != GameScalar || args[1].GetType() != GameScalar)
        return GameValue("REFUSED");
    const float x = args[0];
    const float z = args[1];
    const float radius = args.Size() >= 3 && args[2].GetType() == GameScalar ? static_cast<float>(args[2]) : 30.0f;
    const EnfusionDoorRegistry& doors = GLandscape->EnfusionDoors();
    const int door = doors.FindNearest(Vector3(x, 0.0f, z), std::max(radius, 1.0f));
    if (door < 0)
        return GameValue("NONE");
    const Vector3 hinge = doors.Hinge(door);
    const Vector3 action = doors.ActionPoint(door);
    char text[320];
    std::snprintf(text, sizeof(text), "%d %d %d %.3f %.1f %.3f %.3f %.3f %.3f %.3f %.3f", door, doors.LeafId(door),
                  doors.IsOpen(door) ? 1 : 0, doors.Phase(door), doors.AngleDeg(door), hinge.X(), hinge.Y(), hinge.Z(),
                  action.X(), action.Y(), action.Z());
    return GameValue(text);
}

GameValue TriEnfusionDoorSet(const GameState*, GameValuePar arg)
{
    const GameArrayType& args = arg;
    if (!GLandscape || args.Size() < 2 || args[0].GetType() != GameScalar || args[1].GetType() != GameScalar)
        return GameValue("REFUSED");
    const int door = static_cast<int>(static_cast<float>(args[0]));
    if (door < 0 || static_cast<size_t>(door) >= GLandscape->EnfusionDoors().Count())
        return GameValue("REFUSED");
    GLandscape->EnfusionDoors().RequestState(door, static_cast<float>(args[1]) > 0.5f);
    return GameValue("OK");
}

GameValue TriEnfusionDoorCensus(const GameState*)
{
    char text[96];
    std::snprintf(text, sizeof(text), "doors %zu parts %zu", GLandscape ? GLandscape->EnfusionDoors().Count() : size_t(0),
                  GLandscape ? GLandscape->EnfusionDoors().MemberCount() : size_t(0));
    return GameValue(text);
}

// RFG-158: test commands for the native Enfusion window panes (World/Terrain/EnfusionGlass.hpp).
//
//   triEnfusionGlass [x, z, y, radius] -> "index id broken x y z" of the pane nearest, or "NONE"
//   triEnfusionGlassBreak index        -> "OK" / "REFUSED"
//   triEnfusionGlassCensus             -> "panes N broken M"
GameValue TriEnfusionGlass(const GameState*, GameValuePar arg)
{
    const GameArrayType& args = arg;
    if (!GLandscape || args.Size() < 3 || args[0].GetType() != GameScalar || args[1].GetType() != GameScalar ||
        args[2].GetType() != GameScalar)
        return GameValue("REFUSED");
    const float radius = args.Size() >= 4 && args[3].GetType() == GameScalar ? static_cast<float>(args[3]) : 20.0f;
    const Vector3 pos(static_cast<float>(args[0]), static_cast<float>(args[2]), static_cast<float>(args[1]));
    const EnfusionGlassRegistry& glass = GLandscape->EnfusionGlass();
    const int index = glass.Nearest(pos, std::max(radius, 1.0f));
    if (index < 0)
        return GameValue("NONE");
    const EnfusionGlassRegistry::Pane& pane = glass.Get(static_cast<uint32_t>(index));
    char text[200];
    std::snprintf(text, sizeof(text), "%d %d %d %.3f %.3f %.3f", index, pane.id, pane.broken ? 1 : 0, pane.position[0],
                  pane.position[1], pane.position[2]);
    return GameValue(text);
}

GameValue TriEnfusionGlassBreak(const GameState*, GameValuePar arg)
{
    if (!GLandscape || arg.GetType() != GameScalar)
        return GameValue("REFUSED");
    const int index = static_cast<int>(static_cast<float>(arg));
    if (index < 0)
        return GameValue("REFUSED");
    return GameValue(GLandscape->BreakNativeGlassPane(static_cast<uint32_t>(index), nullptr) ? "OK" : "REFUSED");
}

GameValue TriEnfusionGlassCensus(const GameState*)
{
    char text[96];
    std::snprintf(text, sizeof(text), "panes %zu broken %zu", GLandscape ? GLandscape->EnfusionGlass().Count() : size_t(0),
                  GLandscape ? GLandscape->EnfusionGlass().BrokenCount() : size_t(0));
    return GameValue(text);
}
