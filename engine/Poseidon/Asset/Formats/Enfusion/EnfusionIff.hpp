#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Enfusion's IFF container, shared by every binary format the engine ships.
//
// One walker covers all of them, which is the point of putting it here rather than
// in any one reader. Measured across the local Arma Reforger corpus (ARF-001):
//
//   .pak    PAC1   HEAD DATA FILE           16 / 16 archives close
//   .xob    XOB9   HEAD LODS VOLM COLL      938 / 938 sampled close
//   .ttile  TERR   VERS HGHT TMAT LRS2 BERR 262 / 262 sampled close
//   .ntile  NAVD   TIHE DATA                480 / 480 sampled close
//   .terr   TERR   VERS HEAD TEXS MATS      11 / 11 close
//   .smd    SMDF   VERS HEAD TMAP           closes
//
// DayZ's `FORM`/`ANIMSET5` animation container (DZ-001) is the same family, so this
// is not a Reforger-specific shape.
//
// The size fields are BIG-endian -- IFF's own convention -- while every payload
// inside them is little-endian. Reading the chunk size the same way as the payload
// is the one mistake that makes a walk appear to work on a small file and then
// diverge on a large one, so the two accessors are named apart here.

namespace Poseidon::Asset::Formats::Enfusion
{

//! A four-character chunk tag, comparable and printable.
struct FourCC
{
    uint32_t value = 0;

    FourCC() = default;
    explicit FourCC(uint32_t v) : value(v) {}
    //! Builds from a literal in reading order, e.g. FourCC("HGHT").
    explicit FourCC(const char (&tag)[5])
        : value(static_cast<uint32_t>(static_cast<uint8_t>(tag[0])) << 24 |
                static_cast<uint32_t>(static_cast<uint8_t>(tag[1])) << 16 |
                static_cast<uint32_t>(static_cast<uint8_t>(tag[2])) << 8 |
                static_cast<uint32_t>(static_cast<uint8_t>(tag[3])))
    {
    }

    bool operator==(const FourCC& other) const { return value == other.value; }
    bool operator!=(const FourCC& other) const { return value != other.value; }

    std::string ToString() const
    {
        char text[5] = {static_cast<char>((value >> 24) & 0xFF), static_cast<char>((value >> 16) & 0xFF),
                        static_cast<char>((value >> 8) & 0xFF), static_cast<char>(value & 0xFF), 0};
        return std::string(text);
    }
};

//! One chunk: its tag, and the extent of its payload within the buffer.
struct IffChunk
{
    FourCC tag;
    size_t offset = 0; //!< payload start, i.e. just past the 8-byte chunk header
    size_t size = 0;   //!< payload size, as declared
};

inline uint32_t ReadBe32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 |
           static_cast<uint32_t>(p[3]);
}

inline uint32_t ReadLe32(const uint8_t* p, size_t at = 0)
{
    uint32_t v = 0;
    std::memcpy(&v, p + at, sizeof(v));
    return v;
}

inline uint16_t ReadLe16(const uint8_t* p, size_t at = 0)
{
    uint16_t v = 0;
    std::memcpy(&v, p + at, sizeof(v));
    return v;
}

inline float ReadLeF32(const uint8_t* p, size_t at = 0)
{
    float v = 0.0f;
    std::memcpy(&v, p + at, sizeof(v));
    return v;
}

//! The top-level FORM and its chunks.
struct IffFile
{
    FourCC formType;
    std::vector<IffChunk> chunks;
    std::string error; //!< empty on success; names the constraint that failed

    bool valid() const { return error.empty(); }

    const IffChunk* Find(const FourCC& tag) const
    {
        for (const IffChunk& chunk : chunks)
            if (chunk.tag == tag)
                return &chunk;
        return nullptr;
    }
};

//! Walks a FORM. Two closures are required and neither is optional:
//! the FORM's declared size must account for the whole buffer, and walking the
//! chunks must land exactly on the end. A reader that only checks the first will
//! accept a file whose interior is misaligned.
inline IffFile ReadIff(const void* data, size_t size)
{
    IffFile file;
    if (!data || size < 12)
    {
        file.error = "buffer shorter than a FORM header";
        return file;
    }
    const auto* base = static_cast<const uint8_t*>(data);
    if (ReadBe32(base) != FourCC("FORM").value)
    {
        file.error = "not a FORM container";
        return file;
    }
    const uint32_t declared = ReadBe32(base + 4);
    // FORM's size covers everything after its own 8-byte header.
    if (static_cast<size_t>(declared) + 8 != size)
    {
        file.error = "FORM declares " + std::to_string(declared) + " bytes, which with its header is " +
                     std::to_string(static_cast<size_t>(declared) + 8) + " against a file of " + std::to_string(size);
        return file;
    }
    file.formType = FourCC(ReadBe32(base + 8));

    size_t at = 12;
    while (at + 8 <= size)
    {
        IffChunk chunk;
        chunk.tag = FourCC(ReadBe32(base + at));
        chunk.size = ReadBe32(base + at + 4);
        chunk.offset = at + 8;
        if (chunk.offset + chunk.size > size)
        {
            file.error = "chunk '" + chunk.tag.ToString() + "' runs past the end of the file";
            return file;
        }
        file.chunks.push_back(chunk);
        at = chunk.offset + chunk.size;
    }
    if (at != size)
    {
        file.error = "chunk walk ended at " + std::to_string(at) + " of " + std::to_string(size);
        return file;
    }
    return file;
}

} // namespace Poseidon::Asset::Formats::Enfusion
