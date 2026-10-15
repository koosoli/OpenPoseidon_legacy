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
} // namespace Poseidon::Arma3Rsc
