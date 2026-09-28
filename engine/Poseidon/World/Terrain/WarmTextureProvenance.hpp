#pragma once
#include <Poseidon/IO/Streams/ArchiveSourceBinding.hpp>
#include <Poseidon/IO/Streams/PatnikArchiveReason.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <array>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>
#include <string>
#include <string_view>

namespace Poseidon::Streaming
{
// Diagnostic only. Eight immutable source-member keys, 128 numeric event rows per process.
// No path/handle/source lease is retained or logged. Caller serializes Admit on owner;
// Find and Stamp also run on workers against published immutable records.
class WarmTextureProvenance
{
public:
    static constexpr uint32_t MaxRecords = 8, MaxRows = 128;
    enum class Event : uint8_t
    {
        Accepted, Submitted, SubmitRefused, WorkerStarted, EarlyWorkerStarted, WorkerReadFailed,
        WorkerPut, WorkerPutRefused, WorkerOutcomeUnknown, WorkerSkipped, WorkerSuppressed, WorkerCancelled,
        StorePut, StorePutRefused,
        OwnerExactAttempt, OwnerKeyOnlyAttempt, StoreTakeMissKeyOnly, StoreTakeWarm,
        StoreTakeCancelled, StoreTakeRefused, SourceValidated, SourceRefused, LayoutRefused,
        UploadSucceeded, UploadFailed, OrdinaryUpload,
        StorePromotedPublished,
        StoreRetiredCancelled, StoreRetiredExpired, StoreRetiredUploadedMark,
        StoreRetiredClear, StoreRetiredOther, Count
    };
    // Exact source-member retention outcome; no texture payload or model lease.
    enum class RetireReason : uint8_t { Consumed, Cancelled, Expired, UploadedMark, Clear, Other };
    struct Row
    {
        uint32_t ordinal = 0, token = 0;
        Event event = Event::Accepted;
        uint64_t a = 0, b = 0;
        bool truncated = false;
        explicit operator bool() const { return ordinal != 0 || truncated; }
    };
    struct Summary
    {
        uint32_t sources = 0, rows = 0;
        bool truncated = false;
    };
private:
    struct Record
    {
        char key[1024]{};
        uint16_t keyBytes = 0;
        BankReadMemberIdentity member;
        std::array<std::atomic<uint8_t>, static_cast<unsigned>(Event::Count)> occurrences{};
        std::atomic<uint8_t> storeLive{0};
        std::atomic<uint8_t> pendingPromotion{0};
        std::atomic<uint32_t> pendingRetireReasons{0};
    };
    const bool _targetOnly;
    std::shared_ptr<const ArchiveSourceBudget::Ticket> _charge;
    std::array<Record, MaxRecords> _records{};
    std::atomic<uint32_t> _sources{0}, _rows{0};
    std::atomic<bool> _truncated{false};
    static const char* Label(Event event) noexcept
    {
        switch(event)
        {
        case Event::Accepted: return "accepted";
        case Event::Submitted: return "submitted";
        case Event::SubmitRefused: return "submitRefused";
        case Event::WorkerStarted: return "workerStarted";
        case Event::EarlyWorkerStarted: return "earlyWorkerStarted";
        case Event::WorkerReadFailed: return "workerReadFailed";
        case Event::WorkerPut: return "workerPut";
        case Event::WorkerPutRefused: return "workerPutRefused";
        case Event::WorkerOutcomeUnknown: return "workerOutcomeUnknown";
        case Event::WorkerSkipped: return "workerSkipped";
        case Event::WorkerSuppressed: return "workerSuppressed";
        case Event::WorkerCancelled: return "workerCancelled";
        case Event::StorePut: return "storePut";
        case Event::StorePutRefused: return "storePutRefused";
        case Event::OwnerExactAttempt: return "ownerExactAttempt";
        case Event::OwnerKeyOnlyAttempt: return "ownerKeyOnlyAttempt";
        case Event::StoreTakeMissKeyOnly: return "storeTakeMissKeyOnly";
        case Event::StoreTakeWarm: return "storeTakeWarm";
        case Event::StoreTakeCancelled: return "storeTakeCancelled";
        case Event::StoreTakeRefused: return "storeTakeRefused";
        case Event::SourceValidated: return "sourceValidated";
        case Event::SourceRefused: return "sourceRefused";
        case Event::LayoutRefused: return "layoutRefused";
        case Event::UploadSucceeded: return "uploadSucceeded";
        case Event::UploadFailed: return "uploadFailed";
        case Event::OrdinaryUpload: return "ordinaryUpload";
        case Event::StorePromotedPublished: return "storePromotedPublished";
        case Event::StoreRetiredCancelled: return "storeRetiredCancelled";
        case Event::StoreRetiredExpired: return "storeRetiredExpired";
        case Event::StoreRetiredUploadedMark: return "storeRetiredUploadedMark";
        case Event::StoreRetiredClear: return "storeRetiredClear";
        case Event::StoreRetiredOther: return "storeRetiredOther";
        case Event::Count: break;
        }
        return "invalid";
    }
public:
    // One explicitly selected real renderer stage, diagnostic only. Filtering trace
    // records does not alter warm admission, source capture, or upload decisions.
    static std::string_view TargetKey() noexcept
    { return PatnikArchiveReason::TargetKey(); }
    static bool TargetOnly() noexcept
    {
        static const bool enabled = [] {
            const char* value = std::getenv("WGR_WARM_PATNIK_STAGE_PROBE");
            return value && std::strcmp(value, "1") == 0;
        }();
        return enabled;
    }
    explicit WarmTextureProvenance(std::shared_ptr<const ArchiveSourceBudget::Ticket> charge = {},
        bool targetOnly = TargetOnly()) : _targetOnly(targetOnly), _charge(std::move(charge)) {}
    WarmTextureProvenance(const WarmTextureProvenance&) = delete;
    WarmTextureProvenance& operator=(const WarmTextureProvenance&) = delete;
    static bool Enabled() noexcept
    {
        static const bool enabled = [] {
            for (const char* key : {"WGR_OBJECT_STREAM_WARM_TEXTURE_TRACE",
                "WGR_OBJECT_STREAM_WARM_TEXTURES", "WGR_OBJECT_STREAM_WARM_TEXTURE_JOBS"})
            { const char* value = std::getenv(key); if (!value || std::strcmp(value, "1")) return false; }
            return true;
        }();
        return enabled;
    }
    static WarmTextureProvenance* Active() noexcept
    {
        if (!Enabled()) return nullptr;
        static const std::unique_ptr<WarmTextureProvenance> state = [] {
            try
            {
                auto charge = ArchiveSourceBinding::ReserveTraceMetadata(sizeof(WarmTextureProvenance) + 256);
                if (!charge) return std::unique_ptr<WarmTextureProvenance>{};
                return std::unique_ptr<WarmTextureProvenance>(new WarmTextureProvenance(std::move(charge)));
            }
            catch (...) { return std::unique_ptr<WarmTextureProvenance>{}; }
        }();
        return state.get();
    }
    uint32_t Admit(std::string_view key, const BankReadMemberIdentity& member) noexcept
    {
        if (key.empty() || key.size() >= sizeof(Record::key) || (_targetOnly && key != TargetKey())) return 0;
        const uint32_t count = _sources.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < count; ++i)
            if (_records[i].keyBytes == key.size() &&
                !std::memcmp(_records[i].key, key.data(), key.size()) && _records[i].member == member)
                return i + 1; // same immutable content witness, not a distinct job claim
        if (count == MaxRecords)
        {
            _truncated.store(true, std::memory_order_release);
            return 0;
        }
        Record& record = _records[count];
        std::memcpy(record.key, key.data(), key.size());
        record.keyBytes = static_cast<uint16_t>(key.size());
        record.member = member;
        _sources.store(count + 1, std::memory_order_release); // publish last
        return count + 1;
    }
    uint32_t Find(std::string_view key, const BankReadMemberIdentity& member) const noexcept
    {
        const uint32_t count = _sources.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < count; ++i)
            if (_records[i].keyBytes == key.size() &&
                !std::memcmp(_records[i].key, key.data(), key.size()) && _records[i].member == member)
                return i + 1;
        return 0;
    }
    uint32_t KeyOnly(std::string_view key) const noexcept
    {
        uint32_t match = 0;
        const uint32_t count = _sources.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < count; ++i)
            if (_records[i].keyBytes == key.size() && !std::memcmp(_records[i].key, key.data(), key.size()))
            { if (match) return 0; match = i + 1; }
        return match; // potential key contact, NEVER a physical source match
    }
    // All Note* methods are bounded atomics; callers may hold the store mutex.
    // Emission occurs only at a later owner/harness cut outside that mutex.
    void NoteStored(uint32_t token) noexcept
    {
        if (token && token <= _sources.load(std::memory_order_acquire))
            _records[token - 1].storeLive.store(1, std::memory_order_release);
    }
    void NoteRetired(uint32_t token, RetireReason why) noexcept
    {
        if (!token || token > _sources.load(std::memory_order_acquire)) return;
        Record& record = _records[token - 1];
        if (!record.storeLive.exchange(0, std::memory_order_acq_rel) || why == RetireReason::Consumed) return;
        // A bit is an occurrence witness, not a count. Repeated same-cause
        // removals before a flush coalesce for this physical-member token.
        record.pendingRetireReasons.fetch_or(uint32_t(1) << static_cast<unsigned>(why), std::memory_order_release);
    }
    void NoteClearLive() noexcept
    {
        const uint32_t count = _sources.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < count; ++i) NoteRetired(i + 1, RetireReason::Clear);
    }
    void NotePublishedPromotion(uint32_t token) noexcept
    {
        if (token && token <= _sources.load(std::memory_order_acquire))
            _records[token - 1].pendingPromotion.store(1, std::memory_order_release);
    }
    void FlushPromotions() noexcept
    {
        const uint32_t count = _sources.load(std::memory_order_acquire);
        for (uint32_t token = 1; token <= count; ++token)
            if (_records[token - 1].pendingPromotion.exchange(0, std::memory_order_acq_rel))
                Emit(Stamp(token, Event::StorePromotedPublished));
    }
    uint32_t DrainRetirementReasons(uint32_t token) noexcept
    {
        return token && token <= _sources.load(std::memory_order_acquire) ?
            _records[token - 1].pendingRetireReasons.exchange(0, std::memory_order_acq_rel) : 0;
    }
    void FlushRetirements() noexcept
    {
        const uint32_t count = _sources.load(std::memory_order_acquire);
        constexpr RetireReason reasons[] = {RetireReason::Cancelled, RetireReason::Expired,
            RetireReason::UploadedMark, RetireReason::Clear, RetireReason::Other};
        constexpr Event events[] = {Event::StoreRetiredCancelled, Event::StoreRetiredExpired,
            Event::StoreRetiredUploadedMark, Event::StoreRetiredClear, Event::StoreRetiredOther};
        for (uint32_t token = 1; token <= count; ++token)
        {
            const uint32_t mask = DrainRetirementReasons(token);
            for (size_t i = 0; i < std::size(reasons); ++i)
                if (mask & (uint32_t(1) << static_cast<unsigned>(reasons[i]))) Emit(Stamp(token, events[i]));
        }
    }
    Row Stamp(uint32_t token, Event event, uint64_t a = 0, uint64_t b = 0) noexcept
    {
        if (!token || token > _sources.load(std::memory_order_acquire) || event >= Event::Count) return {};
        auto& occurrences = _records[token - 1].occurrences[static_cast<unsigned>(event)];
        uint8_t prior = occurrences.load(std::memory_order_relaxed);
        while (prior < 4 && !occurrences.compare_exchange_weak(prior, uint8_t(prior + 1),
            std::memory_order_relaxed)) {}
        if (prior >= 4) return {}; // four observations per source and event, never a full-route count
        const uint32_t ordinal = _rows.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ordinal > MaxRows)
            return {0, token, event, a, b, !_truncated.exchange(true, std::memory_order_acq_rel)};
        return {ordinal, token, event, a, b, false};
    }
    // Test/diagnostic only: capped at four stamps per token/event, not a total.
    uint8_t EventOccurrences(uint32_t token, Event event) const noexcept
    {
        if (!token || token > _sources.load(std::memory_order_acquire) || event >= Event::Count) return 0;
        return _records[token - 1].occurrences[static_cast<unsigned>(event)].load(std::memory_order_acquire);
    }
    Summary Observe() const noexcept
    {
        return {_sources.load(std::memory_order_acquire),
            std::min(_rows.load(std::memory_order_acquire), MaxRows), _truncated.load(std::memory_order_acquire)};
    }
    static void Emit(Row row) noexcept
    {
        try
        {
        if (row.truncated)
            LOG_INFO(World, "Warm provenance: truncated after {} numeric rows; no full-route/unused-work claim", MaxRows);
        else if (row.ordinal)
            LOG_INFO(World, "Warm provenance: row={} token={} event={} a={} b={} scope=exact-member-or-labelled-key-only",
                row.ordinal, row.token, Label(row.event), row.a, row.b);
        }
        catch (...) {} // Optional diagnostics must never change preparation or upload success.
    }
    static uint32_t Match(std::string_view key, const std::shared_ptr<const ArchiveSourceBinding>& source) noexcept
    {
        if (!source) return 0;
        auto* trace = Active(); if (!trace) return 0;
        BankReadMemberIdentity identity;
        return source->Request().CopyMemberIdentity(identity) ? trace->Find(key, identity) : 0;
    }
};
}
