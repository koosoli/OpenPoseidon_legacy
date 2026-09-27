#pragma once
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <algorithm>
#include <fstream>
#include <limits>
#include <utility>

namespace Poseidon::Streaming
{
// Optional worker capture only: no VFS/archive identity or pathname freshness proof.
// Source bytes die before return. Decoder output/scratch retains its existing limits;
// this is not an allocator/RSS or I/O-time bound.
struct LoosePaaPreparation
{
    enum class Kind { Unavailable, Blocks, Rgba };
    Kind kind = Kind::Unavailable;
    PAABlockChain chain;
    DecodedImage image;
    size_t openAttempts = 0, capturedSources = 0, capturedBytes = 0;
};

inline bool LoosePaaTopPayloadPresent(const std::vector<uint8_t>& bytes)
{
    if (bytes.size() < 2) return false;
    const auto word = [&](size_t p, unsigned n) {
        uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i) v |= uint32_t(bytes[p + i]) << (8 * i);
        return v;
    };
    const uint32_t magic = word(0, 2);
    // These decoder branches allocate declared source size without checking read failure.
    // Other legacy formats retain their existing checked LoadPaa/LoadPac policy.
    if (magic != 0x8888 && !(magic >= 0xff01 && magic <= 0xff05)) return true;
    size_t at = 2, top = SIZE_MAX;
    const auto fits = [&](size_t p, size_t n) { return p <= bytes.size() && n <= bytes.size() - p; };
    while (fits(at, 4) && word(at, 4) == 0x54414747u)
    {
        if (!fits(at, 12)) return false;
        const uint32_t tag = word(at + 4, 4), size = word(at + 8, 4);
        at += 12;
        if (!fits(at, size)) return false;
        // PacPalette reads exactly one word for AVGC/FLAG and whole words for OFFS.
        // Reject layouts whose declared cursor differs from that unchanged reader.
        if ((tag == 0x41564743u || tag == 0x464c4147u) && size != 4) return false;
        if (tag == 0x4f464653u && size % 4 != 0) return false;
        if (tag == 0x4f464653u && size >= 4)
        {
            const uint32_t offset = word(at, 4);
            if (offset > INT32_MAX) return false;
            top = offset;
        }
        at += size;
    }
    if (!fits(at, 2)) return false;
    const size_t paletteBytes = size_t(word(at, 2)) * 3;
    at += 2;
    if (!fits(at, paletteBytes)) return false;
    at += paletteBytes;
    // DecodePAABuffer initializes dimensions at the natural cursor, then seeks the
    // source OFFS position when decoding. Both must describe this same top header.
    if (top != SIZE_MAX && top != at) return false;
    if (!fits(at, 4)) return false;
    // The fallback decoder does not re-skip this exact legacy marker after SeekLevel.
    // Successful BC-chain parsing above retains its existing marker support.
    if (word(at, 2) == 1234 && word(at + 2, 2) == 8765) return false;
    if (!fits(at, 7)) return false;
    const uint32_t w = word(at, 2) & 0x7fff, h = word(at + 2, 2);
    return w && h && fits(at + 7, word(at + 4, 3));
}

inline LoosePaaPreparation PrepareLoosePaa(const std::string& key, size_t maxSourceBytes = 16 * 1024 * 1024)
{
    LoosePaaPreparation result;
    result.openAttempts = 1; // Actual native stream construction, not an OS syscall count.
    std::ifstream file(key, std::ios::binary | std::ios::ate);
    if (!file) return result;
    const std::streamoff length = file.tellg();
    const size_t limit = maxSourceBytes; // SIZE_MAX explicitly preserves the legacy worker policy.
    if (length <= 0 || static_cast<uint64_t>(length) > limit ||
        static_cast<uint64_t>(length) > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max())) return result;
    std::vector<uint8_t> bytes(static_cast<size_t>(length), 0);
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (file.gcount() != static_cast<std::streamsize>(bytes.size()) || !file) return result;
    // Detect short reads and growth during capture; this is not an atomic disk snapshot.
    if (file.peek() != std::char_traits<char>::eof() || !file.eof() || file.bad()) return result;
    result.capturedSources = 1;
    result.capturedBytes = bytes.size();
    std::string error;
    if (ReadPAABlockChainBuffer(bytes.data(), bytes.size(), result.chain,
                               limit == SIZE_MAX ? nullptr : &error, limit))
    {
        result.kind = LoosePaaPreparation::Kind::Blocks;
        return result;
    }
    // A newly imposed bounded-mode refusal must not inflate a large unused RGBA fallback.
    if (limit != SIZE_MAX && (error == "block chain exceeds preparation byte limit" ||
                             error == "too many prepared mip levels")) return result;
    if (!LoosePaaTopPayloadPresent(bytes)) return result;
    const bool isPaa = key.size() >= 4 && key.compare(key.size() - 4, 4, ".paa") == 0;
    result.image = DecodePAABuffer(bytes.data(), bytes.size(), isPaa);
    if (result.image.valid()) result.kind = LoosePaaPreparation::Kind::Rgba;
    return result;
}
} // namespace Poseidon::Streaming
