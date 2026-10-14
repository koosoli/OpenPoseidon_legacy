// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Arma 3 mission.sqm -> the mission layout this engine reads.
//
// Arma 3 writes missions in two layouts. Missions made before the Eden editor (2016) use
// the Arma 2 layout ("version=12": Groups / Vehicles / Markers / Sensors). Eden missions
// ("version=52..54") keep one tree of Entities -- groups, objects, waypoints, markers,
// triggers, logics, layers -- plus Connections (synchronisation, waypoint activation,
// trigger owners, random start markers) and per-group CrewLinks (who sits where).
//
// Both are turned into one text in the Arma 2 layout:
//  - an Eden mission is rebuilt entity by entity (see Arma3Eden.cpp for every rule);
//  - in either layout the synchronisations are rewritten. Arma 2 and 3 store them as
//    pairs (an item's syncId plus the syncIds of its partners); this engine stores
//    groups (every item that lists the same number is synchronised). Each pair becomes a
//    group of its own, so an item linked to two others is linked to exactly those two.
//
// Only the C++ standard library is used, so the conversion can be checked on its own.

#include <functional>
#include <string>

namespace Poseidon::Arma1Compat
{
// What kind of thing a mission class is, asked of the Arma 3 config while converting.
enum class EdenKind
{
    Unknown,
    Man,
    Land,
    Air,
    Ship,
    Static,
    Thing,
};

struct EdenEnvironment
{
    // The kind of a CfgVehicles class (Unknown when not known).
    std::function<EdenKind(const std::string& className)> kind;
    // True when this engine's CfgMarkers / CfgMarkerColors define the name. When not
    // given, every name is taken as defined.
    std::function<bool(const std::string& name)> markerExists;
    std::function<bool(const std::string& name)> colorExists;
};

struct EdenReport
{
    bool eden = false;    // the input was an Eden mission
    bool changed = false; // the output differs from the input
    int groups = 0, units = 0, vehicles = 0, waypoints = 0, markers = 0, triggers = 0, logics = 0, links = 0;
    int skipped = 0; // entities with no counterpart (comments, polylines, ...)
};

// Converts the text of an Arma 3 mission.sqm. Returns false when the text is not an
// Arma 3 mission this code knows how to read (then `out` is left empty).
bool ConvertArma3MissionText(const std::string& in, std::string& out, EdenReport& report,
                             const EdenEnvironment& env = {});
} // namespace Poseidon::Arma1Compat
