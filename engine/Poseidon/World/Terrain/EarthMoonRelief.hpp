#pragma once
// The moon's topography for the simulation: NASA LRO LOLA heights (assets/earth/moon/moon_height.pmoh,
// tools/sky/make_moon_relief.py), the same map the sky shader ray-marches the moon against, so the ground
// the soldier stands on is the ground that is drawn.
namespace Poseidon::EarthStreaming
{
// Height in metres above the moon's reference sphere (MoonRadius) at a selenographic latitude/longitude in
// degrees (longitude 0 = the near side's centre, east positive). 0 when the map is not installed.
double MoonReliefHeight(double latitude, double longitude);
} // namespace Poseidon::EarthStreaming
