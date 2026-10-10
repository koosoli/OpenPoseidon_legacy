// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace Poseidon::EarthPlannerMap
{
// Builds the Whole World mission planner's map: provider imagery for the given bounds (degrees),
// resampled to a square power-of-two PNG at `out`, row 0 = north. The imagery client lives in the
// renderer library, which registers itself here; the menu only sees this hook. Blocking, so call it
// from a worker thread. Returns false with `message` set when no image could be made.
using Builder = bool (*)(const std::filesystem::path& cache, double south, double west, double north, double east,
                         int size, const std::filesystem::path& out, std::string& message,
                         const std::function<bool()>& cancelled);

inline Builder& Hook()
{
    static Builder builder = nullptr;
    return builder;
}
} // namespace Poseidon::EarthPlannerMap
