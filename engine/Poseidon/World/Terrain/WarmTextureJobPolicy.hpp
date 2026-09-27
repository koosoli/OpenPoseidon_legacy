#pragma once
#include <cstddef>
#include <cstdint>
#include <initializer_list>
namespace Poseidon::Streaming
{
// Pure bounds used by the optional serial-admission/worker queue. Requested C++
// metadata and scratch estimates are not allocator/kernel or process RSS limits.
struct WarmTextureJobPolicy
{
    static constexpr size_t JobLimit = 8, MemberLimit = 64, ActiveLimit = 2;
    static bool ActiveRoom(size_t active) { return active < ActiveLimit; }
    static constexpr uint64_t MetadataLimit = 2ull * 1024 * 1024;
    // Same 64 MiB source/decode/alpha allowance as cold PAA preparation, plus a
    // bounded guard for one copied request/key and alpha-facts metadata.
    static constexpr uint64_t ScratchBytes = 64ull * 1024 * 1024 + 64 * 1024;
    static bool AddCharge(uint64_t& charge, uint64_t bytes)
    {
        if (bytes > MetadataLimit || charge > MetadataLimit - bytes) return false;
        charge += bytes; return true;
    }
    static bool Admit(size_t jobs, size_t members, uint64_t retained, uint64_t charge)
    {
        return jobs < JobLimit && members && members <= MemberLimit && charge &&
            charge <= MetadataLimit && retained <= MetadataLimit - charge;
    }
    static bool ScratchFits(uint64_t budget, uint64_t ready, uint64_t parse,
        uint64_t conversion, uint64_t encode, uint64_t warm, uint64_t metadata = 0)
    {
        if (!budget) return true;
        uint64_t remaining = budget;
        for (uint64_t debt : {ready, parse, conversion, encode, warm, metadata})
        { if (debt > remaining) return false; remaining -= debt; }
        return ScratchBytes <= remaining;
    }
};
}
