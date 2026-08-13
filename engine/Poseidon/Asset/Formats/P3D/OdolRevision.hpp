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
//   40  Armed Assault (Arma 1)   -- the *only* revision Arma 1 ships: 2,684
//                                   models across all 95 A1 PBOs in the corpus,
//                                   revision 40 without exception. The one
//                                   non-ODOL P3D found is an MLOD source.
//   48  Arma 2                   -- the base-game revision; every one of the 64
//                                   Takistan references parses through the same
//                                   A2-family reader as 49
//   49  Arma 2                   -- ~1,400 models, the widest
//   50  Arma 2 OA / BAF / PMC    -- ~90 models
//   52  Arma 2 OA / ACR          -- ~100 models
//   54  DayZ                     -- the only revision DayZ ships: 8,203 models
//                                   across all 133 PBOs of the owner's install,
//                                   revision 54 without exception. DayZ is an
//                                   Arma 2 descendant here, not an Arma 3 one.
//   73  Arma 3                   -- 162 PBOs, the newest
//   75  Arma 3 Tools Binarize     -- official Samples differential fixture
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
        // AST-013 implemented it. Odol40.hpp is a port of a reference parser that
        // closed 18,697 of 18,697 LOD bodies EXACTLY on their declared boundary
        // across the whole local A1 corpus (2,580 models, 95 PBOs), so the claim
        // here is measured at container level. It stays NarrowSubset rather than
        // Parsed for the same reason the A2 family does: what is read is the
        // static shape of a LOD, and anything the reader cannot close on its own
        // declared boundary comes back as a refusal naming the constraint.
        case 40: return {40, OdolSupport::NarrowSubset, "Armed Assault (Arma 1)"};
        case 48: return {48, OdolSupport::NarrowSubset, "Arma 2"};
        case 49: return {49, OdolSupport::NarrowSubset, "Arma 2 / Arma 2 Operation Arrowhead"};
        case 50: return {50, OdolSupport::NarrowSubset, "Arma 2 Operation Arrowhead"};
        case 52: return {52, OdolSupport::NarrowSubset, "Arma 2 Operation Arrowhead"};
        // DayZ. Not an Arma 3 derivative for models: DayZ forked the Arma 2 line
        // before revision 73's layout changes, so 54 reads through the A2-family
        // parser plus a six-byte widening of three fixed runs. Measured on the
        // owner's install -- 8,203 models across 133 PBOs, revision 54 without
        // exception, and no other P3D revision present anywhere in DayZ.
        case 54: return {54, OdolSupport::NarrowSubset, "DayZ"};
        // AST-012B reads exactly one shape of revision 73: a static LOD with no
        // skeleton, animation, proxy, named selection or skinning data, whose
        // faces are triangles and quads. That is a narrow subset of Arma 3
        // content, not Arma 3 support, and everything outside it still refuses.
        case 73: return {73, OdolSupport::NarrowSubset, "Arma 3"};
        // AST-012C observed this exact revision from BI Binarize 2.21 on an
        // official editable Arma 3 Samples source model. It is a distinct
        // layout family, not a fallback target for the revision-73 reader.
        case 75: return {75, OdolSupport::Recognised, "Arma 3 Tools Binarize"};
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
