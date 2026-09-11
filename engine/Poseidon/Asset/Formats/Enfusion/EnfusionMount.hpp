// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// RFG-007 — a mounted Enfusion install the engine can read files out of.
//
// The engine's own file layer cannot help here. `QFBank` is a concrete PBO class,
// not an interface with pluggable back ends (RFG-002 said so and it is still true),
// so mounting `.pak` into it is the wrong idea rather than a missing feature. What
// the native world path actually needs is much smaller: open the install's archives
// once, keep the directories in memory, and answer "give me the bytes of this
// virtual path".
//
// It is deliberately NOT a bank and deliberately not global to the file system. Only
// code that knows it is reading Enfusion data asks this object; nothing else can
// accidentally start resolving game paths through Reforger's archives.
//
// Lifetime: opened by the native world loader and kept for the session, because the
// same archives serve terrain, then surface textures, then models. Opening all 16 of
// a Reforger install costs about half a second and reads directories only — no
// payload is touched until Read().

#include <Poseidon/Asset/Formats/Enfusion/PakArchive.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Poseidon::Asset::Formats::Enfusion
{

class EnfusionMount
{
  public:
    //! Opens every `.pak` under `addonsDir`. Returns false when there are none.
    bool Open(const std::string& addonsDir);
    void Close();

    bool IsOpen() const { std::shared_lock lock(_mutex); return !_archives.empty(); }
    std::string Root() const { std::shared_lock lock(_mutex); return _root; }
    size_t ArchiveCount() const { std::shared_lock lock(_mutex); return _archives.size(); }
    size_t EntryCount() const { std::shared_lock lock(_mutex); return _index.size(); }
    uint64_t Generation() const { std::shared_lock lock(_mutex); return _generation; }

    //! Case-insensitive, '/'-separated, as stored in the archives.
    bool Read(const std::string& virtualPath, std::vector<uint8_t>& out) const;
    // The returned request owns its metadata after Close/remount. File reads and
    // decompression never hold the mount lock. Does not pin externally edited files.
    std::optional<PakReadRequest> MakeReadRequest(const std::string& virtualPath) const;
    bool Has(const std::string& virtualPath) const;
    // Canonical identity only for an actual mounted entry. No filesystem fallback
    // or interpretation of synthetic tint/coverage names.
    std::optional<std::string> CanonicalPath(const std::string& virtualPath) const;

    //! Every entry whose path contains `needle` (already lower-cased by the caller
    //! or not — the compare is case-insensitive). Used to find a world's tiles
    //! without knowing which archive holds them.
    std::vector<std::string> FindAll(const std::string& needle) const;

    //! The process-wide mount, empty until a native world load opens it. Returned by
    //! reference so callers can test IsOpen() rather than juggling a pointer.
    static EnfusionMount& Instance();

  private:
    void CloseLocked();
    mutable std::shared_mutex _mutex;
    uint64_t _generation = 0;
    std::string _root;
    // `deque`-like stability is not needed: the archives are opened once and never
    // added to afterwards, and the index stores an archive INDEX rather than a
    // pointer for exactly that reason.
    std::vector<PakArchive> _archives;
    //! lower-cased virtual path -> (archive index, entry index)
    std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> _index;
};

} // namespace Poseidon::Asset::Formats::Enfusion
