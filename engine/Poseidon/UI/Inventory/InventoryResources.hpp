// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace Poseidon
{
class ParamEntry;

// Stable, engine-owned fallback UI. Optional mod resources may replace it.
// The fallback requires no external icon pack or changes to retail BIN files.
const ParamEntry* InventoryDisplayResource();
} // namespace Poseidon
