#pragma once
// What the simulation needs to know of the whole world's moon frame (EarthMoonFrame.hpp), as plain globals
// so the base game pays one load for each, nothing more. The main thread writes them on entering and
// leaving the moon frame (CelestialState).
namespace Poseidon::EarthStreaming
{
constexpr float EarthGravity = 9.8066f;
// Surface gravity the simulation uses (G_CONST, Vehicle.hpp): Earth's, or the moon's in the moon frame.
inline float Gravity = EarthGravity;
// In the moon frame: the ground is the moon's sphere (EarthStreamingMode.hpp) and there is no sea.
inline bool OnMoon = false;
} // namespace Poseidon::EarthStreaming
