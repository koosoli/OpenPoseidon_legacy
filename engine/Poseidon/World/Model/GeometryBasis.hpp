#pragma once

#include <cstdint>
#include <string>

namespace Poseidon
{
namespace Model
{

// AST-018 -- the explicit source-geometry basis.
//
// A loader knows things about the geometry it just read that no consumer can
// recover afterwards: which way faces wind, what the normals are relative to,
// where the origin is. Until now those facts lived in free-text strings on the
// LOD and in comments, which meant the interesting part -- how much any of it is
// actually known -- was not represented at all.
//
// Every axis therefore carries its own confidence. "Unknown" is the honest and
// common answer, and it is worth recording: a consumer that needs a tangent
// frame should be able to find out that this build cannot supply one, rather
// than reading a default and believing it.
enum class BasisConfidence : uint8_t
{
    Unknown,     // nothing here has established it
    Assumed,     // taken on faith, usually by analogy with a sibling format
    Established, // the original engine's own documented load behaviour
    Measured,    // checked against the source data in this repo
};

enum class SourceWinding : uint8_t
{
    Unknown,
    // The loader reverses index order as it reads, so the IR is the opposite
    // hand from the file.
    ClockwiseReversedOnLoad,
    // Index order is clockwise about the model's own stored vertex normals.
    // Says nothing about which way the face points on screen.
    ClockwiseRelativeToStoredNormals,
};

enum class NormalOrientation : uint8_t
{
    Unknown,
    OutwardFromSurface,
    StoredPerVertex, // present in the file, orientation convention not established
};

enum class TangentHandedness : uint8_t
{
    Unknown,
    NotSupplied, // the format carries no tangent frame this build decodes
};

enum class SourceOrigin : uint8_t
{
    Unknown,
    ModelSpace, // positions are relative to the model's own origin
};

struct BasisAxis
{
    BasisConfidence confidence = BasisConfidence::Unknown;
};

struct GeometryBasis
{
    SourceWinding     winding     = SourceWinding::Unknown;
    NormalOrientation normals     = NormalOrientation::Unknown;
    TangentHandedness tangents    = TangentHandedness::Unknown;
    SourceOrigin      origin      = SourceOrigin::Unknown;

    BasisConfidence windingConfidence  = BasisConfidence::Unknown;
    BasisConfidence normalsConfidence  = BasisConfidence::Unknown;
    BasisConfidence tangentsConfidence = BasisConfidence::Unknown;
    BasisConfidence originConfidence   = BasisConfidence::Unknown;

    // True when the centre of mass is known to coincide with the source origin.
    // Left false and Unknown until something establishes it, because assuming it
    // silently moves every model that does not.
    bool            centreOfMassAtOrigin           = false;
    BasisConfidence centreOfMassConfidence         = BasisConfidence::Unknown;
};

// The legacy free-text winding label, derived rather than written out again.
// The three strings below are the ones already recorded on the SP3X, P3DM and
// ODOL 73 paths; deriving them here is what stops the label and the typed basis
// from drifting apart the first time one of them is edited.
inline std::string DescribeWinding(SourceWinding winding, BasisConfidence confidence)
{
    switch (winding)
    {
        case SourceWinding::ClockwiseReversedOnLoad:
            return confidence == BasisConfidence::Assumed ? "CW_REVERSED_ON_LOAD_ASSUMED"
                                                          : "CW_REVERSED_ON_LOAD";
        case SourceWinding::ClockwiseRelativeToStoredNormals:
            return confidence == BasisConfidence::Measured ? "CW_RELATIVE_TO_STORED_NORMALS_MEASURED"
                                                           : "CW_RELATIVE_TO_STORED_NORMALS";
        case SourceWinding::Unknown:
            break;
    }
    return std::string();
}

// True when every axis is still Unknown -- i.e. the loader recorded nothing.
// Useful to a conformance report, which should be able to say which formats
// describe their basis and which do not.
inline bool IsBasisUnstated(const GeometryBasis& basis)
{
    return basis.windingConfidence == BasisConfidence::Unknown &&
           basis.normalsConfidence == BasisConfidence::Unknown &&
           basis.tangentsConfidence == BasisConfidence::Unknown &&
           basis.originConfidence == BasisConfidence::Unknown &&
           basis.centreOfMassConfidence == BasisConfidence::Unknown;
}

} // namespace Model
} // namespace Poseidon
