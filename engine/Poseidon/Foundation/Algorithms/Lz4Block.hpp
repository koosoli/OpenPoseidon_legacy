#pragma once

#include <cstddef>
#include <cstdint>

namespace Poseidon::Foundation
{

// LZ4 block decompression, for the mip payloads of Enfusion .edds textures (DZ-002).
//
// Written from the published block format rather than adapted from the reference
// implementation, so no upstream licence attaches -- the same reason Lzo1x here is
// hand-written (CORE-007). The format is a sequence of tokens: the high nibble is a
// literal-run length, the low nibble a match length, either extended by trailing
// 0xFF bytes, with a little-endian 16-bit back distance between them. The final
// sequence is literals only and carries no distance.
//
// The one thing that is not obvious from the format description, and that DayZ
// actually depends on: .edds splits a mip into 64 KiB sub-blocks that are *linked*.
// A match in sub-block N may reference output produced by sub-block N-1, so the
// sub-blocks cannot be decoded independently -- they must all land in one buffer and
// each decode must be allowed to reach back before its own start. Decoding them
// independently gets the first 64 KiB right and then fails, which is exactly how
// this was found: 936 of 1,427 mips in the DayZ corpus decoded, all of the failures
// on textures larger than 64 KiB. With linking, 1,493 of 1,493 decode to exactly the
// byte count their dimensions and block format require.
//
// Every source read and every output write is bounds-checked. This decodes
// attacker-reachable data -- a texture inside a downloaded addon -- and the classic
// failure of an LZ decompressor is a back-reference pointing before the output
// buffer, which reads out of bounds long before it writes anything wrong.
class Lz4Block
{
  public:
    // Decompresses one LZ4 block into `dst + dstOffset`, returning the number of
    // bytes appended, or 0 on any malformed input.
    //
    // `dstOffset` is what makes linked sub-blocks work: bytes already present in
    // `dst[0, dstOffset)` are a valid match source, so a caller decoding a chain of
    // sub-blocks passes the running output length and gets the reference semantics
    // the format requires. Passing 0 decodes an independent block.
    //
    // `dst` is never written past `dstCapacity` and no match is ever read from
    // before `dst`.
    static size_t Decompress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstCapacity, size_t dstOffset = 0)
    {
        if (!src || !dst || srcSize == 0 || dstOffset > dstCapacity)
            return 0;

        size_t ip = 0;         // read position in src
        size_t op = dstOffset; // write position in dst, absolute

        // Reads a length extended by trailing 0xFF bytes. Bounded so a run of 0xFF
        // cannot spin past the end of input or overflow the accumulator.
        auto extend = [&](size_t base, size_t& out) -> bool
        {
            out = base;
            if (base != 15)
                return true;
            while (true)
            {
                if (ip >= srcSize)
                    return false;
                const uint8_t b = src[ip++];
                if (out > SIZE_MAX - b)
                    return false;
                out += b;
                if (b != 255)
                    return true;
            }
        };

        while (ip < srcSize)
        {
            const uint8_t token = src[ip++];

            size_t literals = 0;
            if (!extend(token >> 4, literals))
                return 0;
            if (literals > srcSize - ip || literals > dstCapacity - op)
                return 0;
            for (size_t i = 0; i < literals; ++i)
                dst[op + i] = src[ip + i];
            ip += literals;
            op += literals;

            // The last sequence in a block is literals only: it ends exactly at the
            // end of input with no distance following. Anything shorter than the
            // two distance bytes is malformed rather than terminal.
            if (ip == srcSize)
                break;
            if (srcSize - ip < 2)
                return 0;

            const size_t distance = static_cast<size_t>(src[ip]) | (static_cast<size_t>(src[ip + 1]) << 8);
            ip += 2;
            // A zero distance is invalid, and a distance past the start of the output
            // buffer would read memory this block never produced.
            if (distance == 0 || distance > op)
                return 0;

            size_t matchLen = 0;
            if (!extend(token & 0x0F, matchLen))
                return 0;
            if (matchLen > SIZE_MAX - 4)
                return 0;
            matchLen += 4; // the format stores length minus the 4-byte minimum
            if (matchLen > dstCapacity - op)
                return 0;

            // Byte at a time, deliberately: LZ4 matches routinely overlap their own
            // output (distance 1 is a run fill), so a block move would be wrong.
            size_t match = op - distance;
            for (size_t i = 0; i < matchLen; ++i)
                dst[op + i] = dst[match + i];
            op += matchLen;
        }

        return op - dstOffset;
    }
};

} // namespace Poseidon::Foundation
