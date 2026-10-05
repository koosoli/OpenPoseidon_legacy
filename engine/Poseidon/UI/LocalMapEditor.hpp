// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
namespace Poseidon
{
struct LocalMap;
bool RequestLocalMapEditor(const LocalMap& map, std::string& error);
bool ServiceLocalMapEditor();
bool CloseLocalMapEditor();
} // namespace Poseidon
