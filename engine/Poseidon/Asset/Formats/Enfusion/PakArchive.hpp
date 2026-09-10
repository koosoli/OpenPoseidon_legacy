#pragma once

#include <Poseidon/Asset/Formats/Enfusion/EnfusionIff.hpp>

#include <cstdint>
#include <string>
#include <vector>

// Enfusion's `.pak` archive -- Arma Reforger's equivalent of the PBO, and not a PBO
// in any respect.
//
//   "FORM" <u32be size>       size + 8 == file size
//   "PAC1"                    form type
//   "HEAD" <u32be 28> ...
//   "DATA" <u32be n>  ...     the payload blob; every file's bytes live here
//   "FILE" <u32be n>  ...     the directory tree
//
// The directory is a pre-order walk of a single NAMELESS root:
//
//   directory  0x00 <u8 nameLen> <name> <u32le childCount>
//   file       0x01 <u8 nameLen> <name> <24-byte record>
//
// and the record is offset / storedSize / realSize as u32le, six reserved bytes
// that are zero across all 222,566 entries in the local corpus, then a flags byte,
// a method byte and a u32le stamp. Offsets are ABSOLUTE file offsets, and the first
// entry sits at 56 -- the DATA chunk's own data start.
//
// Only two (flags, method) pairs exist in the whole corpus: (0,0) stored and (1,6)
// zlib. There is no third codec to discover, so an unknown method is a real error
// rather than a gap to paper over.
//
// Closure, on all 16 archives of the local install (ARF-001): the FORM size
// accounts for the file, the chunk walk lands on EOF, the directory tree is
// consumed exactly, and the entries' extents tile the DATA chunk with zero gaps and
// zero overlaps. That last one is the check that catches a wrong field width the
// other three tolerate, which is why Validate() reports it separately.

namespace Poseidon::Asset::Formats::Enfusion
{

struct PakEntry
{
    std::string path;      //!< '/'-separated, from the nameless root
    uint64_t offset = 0;   //!< absolute offset into the archive
    uint32_t storedSize = 0;
    uint32_t realSize = 0;
    uint8_t flags = 0;  //!< 1 = compressed, 0 = stored
    uint8_t method = 0; //!< 6 = zlib when compressed, 0 when stored
    uint32_t stamp = 0;

    bool compressed() const { return flags != 0; }
};

// Owns its directory metadata; reading never mutates a mount/archive or a shared
// error string. The caller owns output/error, and must handle stale/cancelled
// requests before publishing results. This does not pin a file against edits.
struct PakReadRequest
{
    std::string archivePath;
    PakEntry entry;
    bool Read(std::vector<uint8_t>& out, std::string& error) const;
};

//! How completely a `.pak` accounts for its own bytes. Every field must hold; they
//! are reported separately because each catches a different class of mistake.
struct PakClosure
{
    bool formSizeCloses = false;   //!< FORM size + 8 == file size
    bool chunkWalkCloses = false;  //!< the chunk walk lands exactly on EOF
    bool directoryCloses = false;  //!< the tree consumed the FILE chunk exactly
    bool dataTiles = false;        //!< entries tile DATA with no gap and no overlap
    size_t gapBytes = 0;
    size_t overlaps = 0;
    size_t nonZeroReserved = 0; //!< entries whose reserved fields were not zero

    bool complete() const { return formSizeCloses && chunkWalkCloses && directoryCloses && dataTiles; }
};

class PakArchive
{
  public:
    //! Opens and parses the directory. The payload is NOT read here; Read() seeks.
    bool Open(const std::string& path);
    void Close();

    bool IsOpen() const { return !_path.empty(); }
    const std::string& Path() const { return _path; }
    const std::string& Error() const { return _error; }
    const std::vector<PakEntry>& Entries() const { return _entries; }
    size_t DirectoryCount() const { return _directories; }
    const PakClosure& Closure() const { return _closure; }

    //! Case-insensitive lookup on the '/'-separated path.
    const PakEntry* Find(const std::string& path) const;

    //! Decompresses (or copies) one entry. Returns false and sets Error() when the
    //! decode fails or produces a size other than the declared realSize -- a short
    //! decode that is silently accepted is how a corrupt archive becomes a corrupt
    //! render.
    //! Legacy single-caller diagnostics: workers should use PakReadRequest instead.
    bool Read(const PakEntry& entry, std::vector<uint8_t>& out) const;
    bool Read(const std::string& path, std::vector<uint8_t>& out) const;

  private:
    bool ParseDirectory(const std::vector<uint8_t>& blob, size_t dataStart, size_t dataSize);

    std::string _path;
    std::string _error;
    std::vector<PakEntry> _entries;
    size_t _directories = 0;
    PakClosure _closure;
};

} // namespace Poseidon::Asset::Formats::Enfusion
