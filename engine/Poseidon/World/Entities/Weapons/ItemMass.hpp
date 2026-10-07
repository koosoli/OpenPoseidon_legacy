// Adapted from the author-supplied CWR-Physical Inventory source donation (2026).
// Distributed under this project's GPL-3.0-or-later licence and Section 7 terms; see LICENSE.
#pragma once

namespace Poseidon
{

class WeaponType;
class MagazineType;
class Magazine;
class EntityAI;

namespace ItemMass
{

constexpr float MaxCarryWeight = 32.7f;
constexpr float FightingLoadWeight = 22.0f;

float Of(const WeaponType *weapon);
float Of(const MagazineType *type);
float Of(const Magazine *magazine);
float TotalCarried(const EntityAI *unit);
void InvalidateCache();

// --- Volume (litres) --------------------------------------------------------
// Packed volume per item (bounding box x class fill factor; see ITEM_WEIGHTS.md).
// Used by the inventory's per-side volume cap. Purely internal (never shown).
float VolumeOf(const WeaponType *weapon);
float VolumeOf(const MagazineType *type);
float VolumeOf(const Magazine *magazine);
float TotalVolumeCarried(const EntityAI *unit);

} // namespace ItemMass
} // namespace Poseidon
