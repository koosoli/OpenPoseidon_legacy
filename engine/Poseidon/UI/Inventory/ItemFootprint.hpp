// Adapted from the author-supplied CWR-Physical Inventory source donation (2026).
// Distributed under this project's GPL-3.0-or-later licence and Section 7 terms; see LICENSE.
#pragma once

namespace Poseidon
{

struct GridSize
{
    int w = 1;
    int h = 1;
};

class WeaponType;
class MagazineType;

namespace ItemFootprint
{

GridSize Of(const WeaponType *weapon);
GridSize Of(const MagazineType *type);

} // namespace ItemFootprint
} // namespace Poseidon
