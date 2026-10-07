#pragma once

namespace Poseidon
{
// Optional local single-player aids adapted from the donated Physical Inventory fork.
bool BearingCalloutsEnabled();
void SetBearingCalloutsEnabled(bool enabled);
bool BearingReadoutEnabled();
void SetBearingReadoutEnabled(bool enabled);
bool GrenadeRangeEnabled();
void SetGrenadeRangeEnabled(bool enabled);
void ResetCombatAssistSettings();
}
