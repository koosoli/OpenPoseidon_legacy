#pragma once
#include <vector>

namespace Poseidon
{
class Landscape;
// Mask the existing near-field snow upload; no new GPU texture or per-pixel rays.
void MaskSnowShelters(const Landscape& land, std::vector<float>& snapshot);
}
