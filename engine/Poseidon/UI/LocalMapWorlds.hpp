// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Poseidon/UI/LocalMapsCatalog.hpp>
#include <string>

namespace Poseidon
{
// Main-thread APIs. Descriptors contain owned metadata only; no foreign config is parsed.
// Saves use the stable OP_Local_<hash> island identifier and a logical world sentinel.
// Successful registration persists under GamePaths::UserDir()/LocalMapWorlds/.
std::string RegisterLocalMapWorld(const LocalMap& map, std::string& error);

// Replay after Glob_Init, when addon configs have finished, before creating the world.
// Keep unavailable descriptors: their saved missions must never fall back to an OFP island.
void RestoreLocalMapWorldClasses();

// Accepts an alias or worlds\\OP_Local_<hash>.wrp, case/separator-insensitively.
// Returned pointers survive insertion, but not RestoreLocalMapWorldClasses().
const LocalMap* FindLocalMapWorld(const std::string& worldOrSentinel);
bool IsLocalMapWorldAvailable(const std::string& worldOrSentinel);

// Recognizes reserved identifiers even when their descriptor is missing or corrupt.
bool IsLocalMapWorldIdentifier(const std::string& worldOrSentinel);
} // namespace Poseidon
