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
    // Serial worker-queue state. Only constructed when the existing warm job flags
    // are enabled; one early first-member slice must be followed by a completed
    // current-generation cold parse before another early slice can start.
    struct FirstMemberFairness
    {
        bool enabled = false, used = false;
        explicit FirstMemberFairness(bool on = false) : enabled(on) {}
        bool TryStart(bool coldRunnable, size_t nextMember, size_t activeWarm)
        {
            if (!enabled || used || !coldRunnable || nextMember != 0 || activeWarm != 0)
                return false;
            used = true;
            return true;
        }
        void ColdCompleted() { used = false; }
        void Reset() { used = false; }
    };
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
    // At an early warm dequeue, reserve enough room for the already-runnable
    // next cold parse as well. This is a selection-time soft-budget check,
    // not a promise that later independent requests cannot use that room.
    static bool ScratchAndNextParseFit(uint64_t budget, uint64_t ready, uint64_t parse,
        uint64_t nextParse, uint64_t conversion, uint64_t encode, uint64_t warm,
        uint64_t metadata = 0)
    {
        if (nextParse > UINT64_MAX - parse) return false;
        return ScratchFits(budget, ready, parse + nextParse, conversion, encode, warm, metadata);
    }
};
}
