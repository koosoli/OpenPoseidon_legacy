// Adapted from the author-supplied CWR-Physical Inventory source donation (2026).
// Distributed under this project's GPL-3.0-or-later licence and Section 7 terms; see LICENSE.
#include "ItemFootprint.hpp"

#include <Poseidon/World/Entities/Weapons/Weapons.hpp>

#include <cstring>

namespace Poseidon::ItemFootprint
{

GridSize Of(const WeaponType *weapon)
{
    if (!weapon) return {1, 1};

    // Rough sizing by slot type: rifles/launchers 4x1, pistols/binoculars/NVGs 1x1.
    if (weapon->_weaponType & MaskSlotPrimary)   return {4, 1};
    if (weapon->_weaponType & MaskSlotSecondary) return {4, 1};
    if (weapon->_weaponType & MaskSlotHandGun)   return {1, 1};   // pistols: one cell
    if (weapon->_weaponType & MaskSlotBinocular) return {1, 1};   // binoculars / NVGs: one cell
    if (weapon->_weaponType & MaskSlotItem)      return {1, 1};
    return {3, 1};
}

GridSize Of(const MagazineType *type)
{
    if (!type) return {1, 1};

    // Identify by class name AND picture, lowercased - the icon can resolve via the
    // class name while the picture is an unrelated path, so we check both.
    char buf[160];
    int j = 0;
    for (const char *p = (const char *)type->GetName(); p && *p && j < 78; ++p)
    {
        char c = *p; if (c >= 'A' && c <= 'Z') c += 32; buf[j++] = c;
    }
    buf[j++] = ' ';
    for (const char *p = (const char *)type->GetPictureName(); p && *p && j < 158; ++p)
    {
        char c = *p; if (c >= 'A' && c <= 'Z') c += 32; buf[j++] = c;
    }
    buf[j] = 0;

    // Satchel charges occupy a 2x2 block.
    if (strstr(buf, "pipebomb") || strstr(buf, "timebomb") || strstr(buf, "satchel"))
    {
        return {2, 2};
    }
    // Strela rockets: six cells wide in the storage grid.
    if (strstr(buf, "9k32") || strstr(buf, "strela"))
    {
        return {6, 1};
    }
    // Carl Gustav / AA rockets: four cells wide.
    if (strstr(buf, "carlgustav") || strstr(buf, "aalaunch"))
    {
        return {4, 1};
    }
    if (type->_maxAmmo >= 100)
    {
        return {2, 1};   // MG belt (M60/PK etc.)
    }
    // LAW / AT4 / RPG / Mortar rounds (and other launcher rounds) take two cells.
    if (strstr(buf, "law") || strstr(buf, "rpg") || strstr(buf, "at4") ||
        strstr(buf, "mortar") || strstr(buf, "launcher"))
    {
        return {2, 1};
    }
    return {1, 1};
}

} // namespace Poseidon::ItemFootprint
