// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <functional>
#include <vector>
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

// The planner map's roads: the OpenStreetMap ways in the given bounds (degrees), in the mission frame
// about (originLatitude, originLongitude) -- the map's own metres. Fetches the cells it needs (the
// renderer's feature cache, the same data the 3D roads are built from). Blocking, for a worker thread.
struct Road
{
    enum Kind : int
    {
        Major, // motorway .. secondary
        Minor, // tertiary, residential, service, ...
        Track  // track, path, footway, cycleway
    };
    Kind kind = Minor;
    float width = 6;        // metres
    std::vector<float> xz;  // x, z pairs
};
using RoadsBuilder = bool (*)(const std::filesystem::path& cache, double originLatitude, double originLongitude,
                              double south, double west, double north, double east, std::vector<Road>& out,
                              std::string& message, const std::function<bool()>& cancelled);

inline RoadsBuilder& RoadsHook()
{
    static RoadsBuilder builder = nullptr;
    return builder;
}
} // namespace Poseidon::EarthPlannerMap
