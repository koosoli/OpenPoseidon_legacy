#pragma once

// The listener-surroundings -> SoundEnvironment decision, lifted out of
// World::PerformSound so it can be exercised without a landscape, a camera or a sound
// device (roadmap 1.4 item 2).
//
// This is a MOVE, not a redesign: the four outdoor branches are the ones the engine has
// used since 2001 and their sizes/densities are reproduced exactly, so a test that pins
// them is pinning shipped behaviour. The room branch is the 2026-08-30 addition and is
// still gated at the call site (POSEIDON_EXPERIMENTAL_EAX) -- see
// design notes
//
// Note what the engine can and cannot tell apart here. "Forest" and "city" are real
// classifications read out of the terrain's per-cell GeographyInfo, which the world
// builder computed offline; the engine is not inferring them from object density at
// runtime. "Mountains" is NOT a terrain classification at all -- it is a bare altitude
// threshold on the surface height under the listener, so a high plateau reads as
// mountains and a deep alpine valley reads as plain. Nothing here distinguishes a
// street from a courtyard, a clearing from a field, or a cliff face from open air.

#include <Poseidon/Audio/IAudioSystem.hpp>

#include <algorithm>

namespace Poseidon::Audio
{

// Everything the classifier is allowed to look at. Filled at the call site from the
// terrain cell under the camera plus (experimental) one interior query.
struct EnvironmentSurroundings
{
    // Inside a building's fire-geometry room. Strict: this is IndoorRoomAt, not the
    // cruder IsSheltered, which also reports true under a bridge or a tree canopy.
    bool insideRoom = false;
    // Room extent, already mapped into the "room" preset's own size scale by the
    // caller (NOT metres -- see ComputeEFXTarget in SoundSystemOAL.cpp).
    float roomSize = 2.0f;
    // GeographyInfo.forestInner || forestOuter for the cell under the listener.
    bool forest = false;
    // GeographyInfo.howManyHardObjects / howManyObjects, both 0..3 bitfields.
    int hardObjects = 0;
    int objects = 0;
    // Landscape surface height above sea level under the listener, in metres.
    float surfaceYAboveWater = 0.0f;
};

// Above this surface height (metres above sea level) the listener is called
// "mountains". A 2001 constant, kept because changing it changes shipped audio.
inline constexpr float kMountainsSurfaceY = 170.0f;

// Priority order is deliberate and matches World::PerformSound: an interior beats
// everything (you cannot hear the forest from inside a barn), forest beats built-up
// (a wooded village sounds like woods), built-up beats altitude.
inline SoundEnvironment ClassifyEnvironment(const EnvironmentSurroundings& s)
{
    SoundEnvironment env{SEPlain, 75.0f, 0.5f};

    if (s.insideRoom)
    {
        env.type = SERoom;
        env.size = s.roomSize;
        env.density = 0.5f;
    }
    else if (s.forest)
    {
        env.type = SEForest;
        env.size = 38.0f;
        env.density = 0.5f;
    }
    else if (s.hardObjects > 0)
    {
        env.type = SECity;
        // More hard objects means a TIGHTER space, hence a smaller environment.
        env.size = static_cast<float>(4 - s.hardObjects) * 15.0f;
        env.density = static_cast<float>(s.hardObjects) * (1.0f / 3.0f);
    }
    else if (s.surfaceYAboveWater > kMountainsSurfaceY)
    {
        env.type = SEMountains;
        env.size = std::clamp(s.surfaceYAboveWater - 120.0f, 50.0f, 100.0f);
        env.density = 0.5f;
    }
    else
    {
        env.type = SEPlain;
        // Can go negative for objects == 3 * something; DoSetEAXEnvironment clamps the
        // size to [2, 100] before it is used, exactly as it always has.
        env.size = 75.0f - static_cast<float>(s.objects) * 15.0f;
        env.density = 0.5f;
    }
    return env;
}

} // namespace Poseidon::Audio
