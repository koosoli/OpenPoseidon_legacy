#pragma once

// GraphicsConfig → live engine bridge.
//
// The Settings/ layer is allowed to know about engine globals
// (GScene + GEngine) here because this module is the boundary
// between "config value" and "running renderer".  Two callers:
//
//   - GameApplication::LoadAndApplyGraphicsConfig at boot.
//   - GraphicsPage's row handlers on every live row change.
//
// Tier→engine-value mappings live in this TU so both callers see the
// same translation table; changing a mapping touches one place.

#include <string>

#include <Poseidon/UI/Settings/GraphicsConfig.hpp>

namespace Poseidon
{
// File version of the DLSS runtime actually in play, e.g. "310.7.0" — read from the
// loaded nvngx_dlss.dll module (or the copy beside the exe before NGX loads it).
// Empty when no runtime is present (non-NVIDIA box, GPL build without the snippet).
// Cached after the first call.
std::string DlssRuntimeVersionString();
} // namespace Poseidon

// Push every cfg field into the live engine state.  Idempotent — safe
// to call repeatedly, e.g. after every row change.  Reads GScene +
// GEngine + writes to gUserFpsCap (the FPS-cap global defined in
// engine/Poseidon/Core/Game/GameLoop.cpp).

namespace Poseidon
{
float TerrainGridForTier(GraphicsConfig::Tier tier);
void ApplyGraphicsConfigToEngine(const GraphicsConfig& cfg);

} // namespace Poseidon
