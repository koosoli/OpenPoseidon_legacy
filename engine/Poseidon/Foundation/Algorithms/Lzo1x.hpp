#pragma once

#include <cstddef>
#include <cstdint>

namespace Poseidon::Foundation
{

// LZO1X decompression, for PAA mips whose stored width has bit 15 set (AST-013).
//
// Which compression PAA uses was determined, not assumed: the engine's existing
// LZSS decoder (SSCompress, used for PBO entries and BIS binary arrays) produces
// nothing usable on these payloads -- 0 of 40 sampled mips decoded to their
// expected size -- while this reads all 98 compressed mips in the Arma 3 sample
// corpus to exactly the byte count their DXT dimensions require.
//
// Written from the published format rather than adapted from liblzo, so no
// upstream licence attaches (CORE-007). The format is a byte-oriented LZ77:
// opcodes carry a literal-run length or a match length plus a back distance, with
// the low two bits of the previous opcode byte carrying a short trailing literal
// run.
//
// Every source read and every output write is bounds-checked. This decodes
// attacker-reachable data -- a texture inside a downloaded addon -- and the
// classic failure of an LZ decompressor is a back-reference pointing before the
// output buffer, which reads out of bounds long before it writes anything wrong.
class Lzo1x
{
  public:
    // Returns the number of bytes written, or 0 on any malformed input. `dst` is
    // never written past `dstCapacity`.  When `consumed` is supplied it receives
    // the exact byte position after an LZO end marker (or zero if no complete
    // marker was encountered).  ODOL stores compressed arrays back-to-back, so
    // callers must not have to guess where one payload ends.
    static size_t Decompress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstCapacity,
                             size_t* consumed = nullptr)
    {
        if (consumed)
            *consumed = 0;
        if (!src || !dst || srcSize == 0)
            return 0;

        size_t ip = 0, op = 0;
        size_t t  = 0;

        // Reads a length that continues across zero bytes, as the format encodes
        // long runs. Bounded so a run of zeroes cannot spin to the end of input.
        auto extend = [&](size_t base, size_t bias) -> bool {
            t = base;
            while (ip < srcSize && src[ip] == 0)
            {
                if (t > (SIZE_MAX - 255))
                    return false;
                t += 255;
                ++ip;
            }
            if (ip >= srcSize)
                return false;
            t += bias + src[ip];
            ++ip;
            return true;
        };
        auto literals = [&](size_t count) -> bool {
            if (count > srcSize - ip || count > dstCapacity - op)
                return false;
            for (size_t k = 0; k < count; ++k)
                dst[op + k] = src[ip + k];
            ip += count;
            op += count;
            return true;
        };
        // Byte-at-a-time on purpose: an LZO match may overlap its own output (a
        // run-length fill is encoded as distance 1), so a bulk copy is wrong.
        auto copyMatch = [&](size_t from, size_t count) -> bool {
            if (from > op || count > dstCapacity - op)
                return false;
            for (size_t k = 0; k < count; ++k)
                dst[op + k] = dst[from + k];
            op += count;
            return true;
        };
        auto need = [&](size_t count) { return srcSize - ip >= count; };

        enum Label { Start, Loop, FirstLiteralRun, Match, MatchDone, MatchNext, Done };
        Label label = Start;
        size_t m = 0;

        while (label != Done)
        {
            switch (label)
            {
                case Start:
                    if (src[ip] > 17)
                    {
                        t = static_cast<size_t>(src[ip]) - 17;
                        ++ip;
                        if (t < 4)
                        {
                            label = MatchNext;
                            break;
                        }
                        if (!literals(t))
                            return 0;
                        label = FirstLiteralRun;
                        break;
                    }
                    label = Loop;
                    break;

                case Loop:
                    if (ip >= srcSize)
                        return op;
                    t = src[ip++];
                    if (t >= 16)
                    {
                        label = Match;
                        break;
                    }
                    if (t == 0 && !extend(0, 15))
                        return 0;
                    if (!literals(t + 3))
                        return 0;
                    label = FirstLiteralRun;
                    break;

                case FirstLiteralRun:
                    if (ip >= srcSize)
                        return op;
                    t = src[ip++];
                    if (t >= 16)
                    {
                        label = Match;
                        break;
                    }
                    if (!need(1))
                        return 0;
                    {
                        const size_t back = (1 + 0x0800) + (t >> 2) + (static_cast<size_t>(src[ip]) << 2);
                        ++ip;
                        if (back > op)
                            return 0;
                        m = op - back;
                    }
                    if (!copyMatch(m, 3))
                        return 0;
                    label = MatchDone;
                    break;

                case Match:
                    if (t >= 64)
                    {
                        if (!need(1))
                            return 0;
                        const size_t back = 1 + ((t >> 2) & 7) + (static_cast<size_t>(src[ip]) << 3);
                        ++ip;
                        if (back > op)
                            return 0;
                        m = op - back;
                        t = (t >> 5) - 1;
                    }
                    else if (t >= 32)
                    {
                        t &= 31;
                        if (t == 0 && !extend(0, 31))
                            return 0;
                        if (!need(2))
                            return 0;
                        const size_t back =
                            1 + ((static_cast<size_t>(src[ip]) + (static_cast<size_t>(src[ip + 1]) << 8)) >> 2);
                        ip += 2;
                        if (back > op)
                            return 0;
                        m = op - back;
                    }
                    else if (t >= 16)
                    {
                        size_t base = (t & 8) << 11;
                        t &= 7;
                        if (t == 0 && !extend(0, 7))
                            return 0;
                        if (!need(2))
                            return 0;
                        const size_t off =
                            (static_cast<size_t>(src[ip]) + (static_cast<size_t>(src[ip + 1]) << 8)) >> 2;
                        ip += 2;
                        // A zero distance in this opcode is the end-of-stream
                        // marker, not a self-referencing match.
                        if (base + off == 0)
                        {
                            if (consumed)
                                *consumed = ip;
                            return op;
                        }
                        const size_t back = base + off + 0x4000;
                        if (back > op)
                            return 0;
                        m = op - back;
                    }
                    else
                    {
                        if (!need(1))
                            return 0;
                        const size_t back = 1 + (t >> 2) + (static_cast<size_t>(src[ip]) << 2);
                        ++ip;
                        if (back > op)
                            return 0;
                        m = op - back;
                        if (!copyMatch(m, 2))
                            return 0;
                        label = MatchDone;
                        break;
                    }
                    if (!copyMatch(m, t + 2))
                        return 0;
                    label = MatchDone;
                    break;

                case MatchDone:
                    if (ip < 2)
                        return 0;
                    t = src[ip - 2] & 3;
                    label = (t == 0) ? Loop : MatchNext;
                    break;

                case MatchNext:
                    if (!literals(t))
                        return 0;
                    if (ip >= srcSize)
                        return op;
                    t = src[ip++];
                    label = Match;
                    break;

                case Done:
                    break;
            }
        }
        return op;
    }
};

} // namespace Poseidon::Foundation
