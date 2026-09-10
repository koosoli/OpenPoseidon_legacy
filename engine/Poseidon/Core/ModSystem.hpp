#pragma once

#include <Poseidon/Core/ModCollection.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>

namespace Poseidon
{
typedef bool ModDirectoryCallback(RStringB dir, void* context);

/// The active mod set: an ordered collection of the mods currently mounted. Holds each
/// mod's folder name (its mod-path membership label / cross-machine identity) and its
/// absolute mount path, and exposes the two formatted views callers need — mount PATHS
/// (for the addon loader / re-mount) and folder NAMES (for the network advertise + the
/// server-browser display). The folder name is the mod's identity, not its content's VFS
/// prefix (those come from the PBO filenames inside, independent of the folder name).
class ModSystem
{
public:
	// Startup-only validated default packs. Kept across ordinary in-process mod
	// changes; explicit mods with the same identity take precedence.
	static void ConfigureDefaultContent(const RStringB& mountPath);
	// Replace the active set, parsed from a ';'-separated absolute mount-path string
	// (what AppConfig resolves --mod into, or "" for the base game).
	static void SetModPath(const RStringB& modPath);

	// ';'-joined absolute mount PATHS — for the addon-bank loader and in-process re-mount.
	static RStringB GetModList();

	// ';'-joined folder NAMES verbatim (`CSLA`, `@fixturemod`) — the mod's stable
	// cross-machine identity, what the server advertises to clients and the browser shows.
	// Use this, not GetModList(), for anything that crosses the wire or hits a screen.
	static RStringB GetModNames();

	// Directories are processed in reverse order (last to first), then empty string.
	// Returns true if any callback returns true (early exit), false otherwise.
	static bool EnumDirectories(ModDirectoryCallback callback, void* context);

private:
	static ModCollection s_active;
	static ModCollection s_defaultContent;
};
} // namespace Poseidon
