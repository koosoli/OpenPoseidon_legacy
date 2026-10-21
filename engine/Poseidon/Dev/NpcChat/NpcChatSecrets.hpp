#pragma once

// API keys for the NPC chat feature.
//
// Rules (owner's requirement: keys must never leave the player's computer, and must never end
// up in a repository, not even during development):
//   * Keys live in memory for the session by default.
//   * "Remember keys" writes them to KeyStorePath(): a per-user application-data directory
//     (%APPDATA%\OpenPoseidon\npc-chat on Windows, $XDG_CONFIG_HOME/openposeidon/npc-chat
//     elsewhere). It is never the game folder, the profile folder or a build output.
//   * On Windows the file is encrypted with DPAPI (CryptProtectData, current-user scope), so
//     a copied file is useless on another machine or account. Elsewhere it is mode 0600.
//   * The store refuses to write anywhere inside a git work tree (a .git entry in any parent).
//   * Keys are never logged; RedactSecrets() scrubs them from any text that is logged or shown.
//   * Unticking "Remember keys" deletes the file.

#include <string>

namespace Poseidon::NpcChat
{
/// Key for a slot ("openai", "groq", ...): the panel value, else the slot's environment
/// variable, else empty.
std::string GetKey(const std::string& slot);
/// Panel value only (never the environment variable), for showing "set / not set".
bool HasPanelKey(const std::string& slot);
bool HasEnvKey(const std::string& slot);
void SetKey(const std::string& slot, const std::string& key);
/// "sk-or-v1...1a2b (73 chars)": enough to compare with the provider's key page, never the key.
std::string KeyFingerprint(const std::string& slot);
/// What SetKey stores: printable ASCII only, quotes and a "Bearer" prefix removed (pure).
std::string SanitizeKey(const std::string& key);
void ClearAllKeys();

/// Load remembered keys once (no-op when the file is absent).
void LoadRememberedKeys();
/// Write (remember=true) or delete (remember=false) the key file. Returns false and fills
/// `error` when writing was refused or failed.
bool PersistKeys(bool remember, std::string& error);

std::string KeyStorePath();

/// Replace every known key (and anything that looks like a bearer token) in `text`.
std::string RedactSecrets(const std::string& text);

/// True when `path` lies inside a git work tree (pure path walk; unit-tested).
bool IsInsideGitWorkTree(const std::string& path);
} // namespace Poseidon::NpcChat
