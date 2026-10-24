#pragma once

// Arma 3 title resources as scripts see them: onLoad / onUnload of a cutRsc'd class and a
// stand-in display that reads as null once closed (GameStateExtA3Rsc.cpp). No-ops outside
// --arma3.

#include <Evaluator/express.hpp>

#include <string>

namespace Poseidon::Arma3Rsc
{
std::string LayerKey(GameValuePar layer);
void Open(const std::string& layer, GameValuePar resource);
void CloseLayer(const std::string& layer);
bool IsClosedDisplay(float value);
void Register();

// Arma 3's safe zone (safezoneX/Y/W/H) for this screen: 0..1 is a 4:3 box, as tall as the
// interface size allows, in the middle of the screen.
struct SafeZone
{
    float x, y, w, h;
};
SafeZone UiSafeZone();
// While on, safeZone* and pixelW/H answer in Arma 3's coordinates (Arma3Menu loading one of
// Arma 3's screens); otherwise this engine's (0..1 is the screen). Calls nest.
void SetUiCoordinates(bool on);
bool UiCoordinates();
} // namespace Poseidon::Arma3Rsc
