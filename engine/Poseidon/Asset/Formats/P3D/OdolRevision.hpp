#pragma once

#include <Poseidon/Asset/Formats/BISBinaryStream.hpp>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace Poseidon::Asset::Formats::P3D
{

// AST-012A -- versioned ODOL dispatch.
//
// This is architecture only. It reads the exact revision, routes to the exact
// parser that handles it, preserves the existing OFP/CWA path untouched, and
// refuses everything else in a way a caller can act on. It is explicitly NOT a
// claim of support for any later revision: implementing one is AST-012B, and
// naming a revision here means only that the corpus contains it.
//
// The revisions below were found by AST-007 reading the entry tables of 545 PBOs.
// They are not a guess at what exists in the wild -- they are what is present in
// the owner's reference corpora, which is the only population any claim here can
// honestly cover:
//
//   7   OFP / Cold War Assault   -- the one this build parses
//   48  Arma 2                   -- 17 models
//   49  Arma 2                   -- ~1,400 models, the widest
//   50  Arma 2 OA / BAF / PMC    -- ~90 models
//   52  Arma 2 OA / ACR          -- ~100 models
//   73  Arma 3                   -- 162 PBOs, the newest
enum class OdolSupport
{
    Parsed,       // this build reads it
    NarrowSubset, // AST-012B: one verified shape reads; everything else is refused
    Recognised,   // present in the corpora, deliberately not implemented yet
    Unknown,      // never observed; refuse without speculating
};

struct OdolRevisionInfo
{
    uint32_t     version = 0;
    OdolSupport  support = OdolSupport::Unknown;
    const char*  generation = "unknown";
};

inline OdolRevisionInfo DescribeOdolRevision(uint32_t version)
{
    switch (version)
    {
        case 7:  return {7,  OdolSupport::Parsed,     "Operation Flashpoint / Cold War Assault"};
        case 48: return {48, OdolSupport::Recognised, "Arma 2"};
        case 49: return {49, OdolSupport::Recognised, "Arma 2"};
        case 50: return {50, OdolSupport::Recognised, "Arma 2 Operation Arrowhead"};
        case 52: return {52, OdolSupport::Recognised, "Arma 2 Operation Arrowhead"};
        // AST-012B reads exactly one shape of revision 73: a static LOD with no
        // skeleton, animation, proxy, named selection or skinning data, whose
        // faces are triangles and quads. That is a narrow subset of Arma 3
        // content, not Arma 3 support, and everything outside it still refuses.
        case 73: return {73, OdolSupport::NarrowSubset, "Arma 3"};
        default: return {version, OdolSupport::Unknown, "unknown"};
    }
}

// Recognised container, unimplemented revision.
//
// Separate from the malformed-input runtime_errors the readers throw, for the same
// reason UnsupportedLodFormat is: "this is a valid model this build cannot read"
// and "these bytes are not a model" call for different handling, and a caller that
// cannot tell them apart is left matching on message text.
class UnsupportedOdolRevision : public std::runtime_error
{
  public:
    explicit UnsupportedOdolRevision(const OdolRevisionInfo& info)
        : std::runtime_error(BuildMessage(info, std::string())), info_(info)
    {
    }

    // The reason a narrow-subset reader gave up. Without it the caller is told
    // the revision is unsupported when in fact one shape of it reads, and has no
    // way to learn which constraint this particular file broke.
    UnsupportedOdolRevision(const OdolRevisionInfo& info, const std::string& reason)
        : std::runtime_error(BuildMessage(info, reason)), info_(info)
    {
    }

    const OdolRevisionInfo& info() const { return info_; }
    uint32_t                version() const { return info_.version; }
    bool                    recognised() const
    {
        return info_.support == OdolSupport::Recognised || info_.support == OdolSupport::NarrowSubset;
    }

  private:
    static std::string BuildMessage(const OdolRevisionInfo& info, const std::string& reason)
    {
        std::string message = "ODOL revision " + std::to_string(info.version) + " is not supported by this build";
        if (info.support == OdolSupport::NarrowSubset)
        {
            message += " for this model (recognised: ";
            message += info.generation;
            message += "; AST-012B reads only static LODs of this revision";
            if (!reason.empty()) message += ": " + reason;
            message += ")";
        }
        else if (info.support == OdolSupport::Recognised)
        {
            // Naming the generation is the difference between a report that leads
            // somewhere and one that sends the reader to a hex editor.
            message += " (recognised: ";
            message += info.generation;
            message += "; implementing it is AST-012B)";
        }
        else
        {
            message += " (unrecognised revision; not present in the indexed reference corpora)";
        }
        return message;
    }

    OdolRevisionInfo info_;
};

// Read the revision without consuming it, so the chosen parser still sees its own
// header. Mirrors MLOD::peekLodSignature, for the same reason: dispatch must not
// disturb the path it dispatches to.
inline OdolRevisionInfo PeekOdolRevision(BinaryReader& reader)
{
    if (reader.remaining() < 8)
        throw std::runtime_error("ODOL header truncated");

    char     signature[4] = {0};
    uint32_t raw          = reader.read<uint32_t>();
    std::memcpy(signature, &raw, 4);
    const uint32_t version = reader.read<uint32_t>();
    reader.seekRelative(-8);

    if (std::memcmp(signature, "ODOL", 4) != 0)
        throw std::runtime_error("Invalid P3D signature: expected 'ODOL', got '" + std::string(signature, 4) + "'");
    return DescribeOdolRevision(version);
}

} // namespace Poseidon::Asset::Formats::P3D
