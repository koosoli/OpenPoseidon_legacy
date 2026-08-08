#pragma once

#include <cstdint>

namespace Poseidon
{
namespace Model
{

// AST-018 -- what a LOD is for, decided once at load instead of by float
// comparison at every consumer.
//
// Real Virtuality encodes a LOD's purpose in its resolution: ordinary visual
// LODs carry a viewing distance in metres, and every special LOD carries a
// sentinel far outside that range. Consumers have been re-deriving that with
// IsSpec(resolution, SOME_SPEC) against a table of macros, which means each one
// carries its own copy of the table and its own idea of which sentinels exist.
// A LOD that nobody's table covers silently reads as an ordinary visual LOD at
// an absurd draw distance.
//
// Unknown is a real answer here, not a failure: a sentinel outside the visual
// range that this table does not name must stay distinguishable from a visual
// LOD, or a consumer will draw it.
enum class LodPurpose : uint8_t
{
    Visual,           // ordinary resolution LOD; resolution is a viewing distance
    ViewGunner,       // 1000 -- a visual LOD with a fixed role, not a sentinel
    ViewPilot,        // 1100
    ViewCargo,        // 1200
    Geometry,         // 1e13
    Memory,           // 1e15 * 1
    LandContact,      // 1e15 * 2
    Roadway,          // 1e15 * 3
    Paths,            // 1e15 * 4
    HitPoints,        // 1e15 * 5
    ViewGeometry,     // 1e15 * 6
    FireGeometry,     // 1e15 * 7
    ViewCargoGeometry,     // 1e15 * 8
    ViewCommander,         // 1e15 * 10
    ViewCommanderGeometry, // 1e15 * 11
    ViewPilotGeometry,     // 1e15 * 13
    ViewGunnerGeometry,    // 1e15 * 15
    FireGunnerGeometry,    // 1e15 * 16
    Unknown,          // outside the visual range and not a sentinel this build names
};

// The sentinels are exact values in the source, but they arrive through a float
// and through whatever rounding the writing tool applied, so they are matched
// with the engine's existing relative tolerance rather than by equality.
inline bool IsLodResolution(float resolution, double sentinel)
{
    const double difference = static_cast<double>(resolution) - sentinel;
    return (difference < 0 ? -difference : difference) < sentinel * 1e-3;
}

inline LodPurpose ClassifyLodResolution(float resolution)
{
    constexpr double kGeometry = 1e13;
    constexpr double kSpec     = 1e15;

    if (IsLodResolution(resolution, kGeometry)) return LodPurpose::Geometry;
    if (IsLodResolution(resolution, kSpec * 1)) return LodPurpose::Memory;
    if (IsLodResolution(resolution, kSpec * 2)) return LodPurpose::LandContact;
    if (IsLodResolution(resolution, kSpec * 3)) return LodPurpose::Roadway;
    if (IsLodResolution(resolution, kSpec * 4)) return LodPurpose::Paths;
    if (IsLodResolution(resolution, kSpec * 5)) return LodPurpose::HitPoints;
    if (IsLodResolution(resolution, kSpec * 6)) return LodPurpose::ViewGeometry;
    if (IsLodResolution(resolution, kSpec * 7)) return LodPurpose::FireGeometry;
    if (IsLodResolution(resolution, kSpec * 8)) return LodPurpose::ViewCargoGeometry;
    if (IsLodResolution(resolution, kSpec * 10)) return LodPurpose::ViewCommander;
    if (IsLodResolution(resolution, kSpec * 11)) return LodPurpose::ViewCommanderGeometry;
    if (IsLodResolution(resolution, kSpec * 13)) return LodPurpose::ViewPilotGeometry;
    if (IsLodResolution(resolution, kSpec * 15)) return LodPurpose::ViewGunnerGeometry;
    if (IsLodResolution(resolution, kSpec * 16)) return LodPurpose::FireGunnerGeometry;

    // The crewed view LODs are ordinary visual LODs at reserved distances, so
    // they are matched on the half-metre band the engine already used and not
    // with the relative tolerance the sentinels need.
    if (resolution >= 999.5f && resolution <= 1000.5f) return LodPurpose::ViewGunner;
    if (resolution >= 1099.5f && resolution <= 1100.5f) return LodPurpose::ViewPilot;
    if (resolution >= 1199.5f && resolution <= 1200.5f) return LodPurpose::ViewCargo;

    // Anything beyond a plausible draw distance is a sentinel of some kind. Do
    // not call it Visual: a consumer would then treat it as drawable geometry.
    if (resolution >= 1e5f) return LodPurpose::Unknown;
    return LodPurpose::Visual;
}

// A purpose is geometry-only when it must never be drawn. Its previous form,
// ResolGeometryOnly, took a float and had to be kept in step with the table by
// hand; here the compiler is what keeps them in step.
inline bool IsGeometryOnlyLod(LodPurpose purpose)
{
    switch (purpose)
    {
        case LodPurpose::Geometry:
        case LodPurpose::Memory:
        case LodPurpose::LandContact:
        case LodPurpose::Roadway:
        case LodPurpose::Paths:
        case LodPurpose::HitPoints:
        case LodPurpose::ViewGeometry:
        case LodPurpose::FireGeometry:
        case LodPurpose::ViewCargoGeometry:
        case LodPurpose::ViewCommanderGeometry:
        case LodPurpose::ViewPilotGeometry:
        case LodPurpose::ViewGunnerGeometry:
        case LodPurpose::FireGunnerGeometry:
            return true;
        default:
            return false;
    }
}

inline const char* LodPurposeName(LodPurpose purpose)
{
    switch (purpose)
    {
        case LodPurpose::Visual:                return "Visual";
        case LodPurpose::ViewGunner:            return "View (Gunner)";
        case LodPurpose::ViewPilot:             return "View (Pilot)";
        case LodPurpose::ViewCargo:             return "View (Cargo)";
        case LodPurpose::Geometry:              return "Geometry";
        case LodPurpose::Memory:                return "Memory";
        case LodPurpose::LandContact:           return "LandContact";
        case LodPurpose::Roadway:               return "Roadway";
        case LodPurpose::Paths:                 return "Paths";
        case LodPurpose::HitPoints:             return "HitPoints";
        case LodPurpose::ViewGeometry:          return "Geometry (View)";
        case LodPurpose::FireGeometry:          return "Geometry (Fire)";
        case LodPurpose::ViewCargoGeometry:     return "Geometry (Cargo)";
        case LodPurpose::ViewCommander:         return "View (Commander)";
        case LodPurpose::ViewCommanderGeometry: return "Geometry (Commander)";
        case LodPurpose::ViewPilotGeometry:     return "Geometry (Pilot)";
        case LodPurpose::ViewGunnerGeometry:    return "Geometry (Gunner)";
        case LodPurpose::FireGunnerGeometry:    return "Geometry (Gunner Fire)";
        case LodPurpose::Unknown:               return "Unknown special LOD";
    }
    return "Unknown special LOD";
}

} // namespace Model
} // namespace Poseidon
