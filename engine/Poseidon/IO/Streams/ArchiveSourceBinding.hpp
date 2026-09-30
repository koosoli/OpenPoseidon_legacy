#pragma once
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <mutex>
#include <memory>
#include <limits>

namespace Poseidon
{
// Optional source metadata only. No pixel buffer or engine Ref belongs here.
class ArchiveSourceBudget : public std::enable_shared_from_this<ArchiveSourceBudget>
{
public:
    struct Snapshot { size_t bindings = 0, bytes = 0, weakCells = 0, weakBytes = 0;
        size_t weakPeakReservationCells = 0, weakPeakReservationBytes = 0, traceBytes = 0; };
    class Ticket
    {
        friend class ArchiveSourceBudget;
        struct Key { private: Key() = default; friend class ArchiveSourceBudget; };
        std::shared_ptr<ArchiveSourceBudget> _budget;
        size_t _bytes;
        enum class Kind : uint8_t { Binding, Weak, Trace };
        Kind _kind;
    public:
        Ticket(Key, std::shared_ptr<ArchiveSourceBudget> budget, size_t bytes, Kind kind) noexcept
            : _budget(std::move(budget)), _bytes(bytes), _kind(kind) {}
        Ticket(const Ticket&) = delete;
        Ticket& operator=(const Ticket&) = delete;
        Ticket(Ticket&&) = delete;
        Ticket& operator=(Ticket&&) = delete;
        ~Ticket() { _budget->Release(_bytes, _kind); }
    };
    ArchiveSourceBudget(size_t bindings, size_t bytes, size_t weakCells = 1024)
        : _maxBindings(bindings), _maxBytes(bytes), _maxWeakCells(weakCells) {}
    std::shared_ptr<const Ticket> Acquire(size_t bytes, bool* capacityRefused = nullptr,
        size_t bindingLimit = std::numeric_limits<size_t>::max())
    { return AcquireImpl(bytes, Ticket::Kind::Binding, capacityRefused, bindingLimit); }
    std::shared_ptr<const Ticket> AcquireWeak(size_t bytes, bool* capacityRefused = nullptr)
    { return AcquireImpl(bytes, Ticket::Kind::Weak, capacityRefused); }
    std::shared_ptr<const Ticket> AcquireTrace(size_t bytes, bool* capacityRefused = nullptr)
    { return AcquireImpl(bytes, Ticket::Kind::Trace, capacityRefused); }
private:
    std::shared_ptr<const Ticket> AcquireImpl(size_t bytes, Ticket::Kind kind, bool* capacityRefused,
        size_t bindingLimit = std::numeric_limits<size_t>::max())
    {
        if (capacityRefused) *capacityRefused = false;
        std::lock_guard<std::mutex> lock(_mutex);
        if (!bytes || (kind == Ticket::Kind::Weak ? _live.weakCells >= _maxWeakCells :
             kind == Ticket::Kind::Binding &&
                 (_live.bindings >= _maxBindings || _live.bindings >= bindingLimit)) ||
            bytes > _maxBytes - _live.bytes)
        { if (capacityRefused) *capacityRefused = true; return {}; }
        try
        {
            // make_shared allocates BEFORE this noexcept constructor. Failed allocation
            // cannot destroy an uncharged Ticket or reenter our locked Release path.
            auto ticket = std::make_shared<const Ticket>(Ticket::Key{}, shared_from_this(), bytes, kind);
            if (kind == Ticket::Kind::Weak)
            {
                ++_live.weakCells; _live.weakBytes += bytes;
                if (_live.weakCells > _live.weakPeakReservationCells) _live.weakPeakReservationCells = _live.weakCells;
                if (_live.weakBytes > _live.weakPeakReservationBytes) _live.weakPeakReservationBytes = _live.weakBytes;
            }
            else if (kind == Ticket::Kind::Binding) ++_live.bindings;
            else _live.traceBytes += bytes;
            _live.bytes += bytes;
            return ticket;
        }
        catch (...) { return {}; }
    }
public:
    Snapshot Observe() const { std::lock_guard<std::mutex> lock(_mutex); return _live; }
private:
    void Release(size_t bytes, Ticket::Kind kind)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (kind == Ticket::Kind::Weak) { --_live.weakCells; _live.weakBytes -= bytes; }
        else if (kind == Ticket::Kind::Binding) --_live.bindings;
        else _live.traceBytes -= bytes;
        _live.bytes -= bytes;
    }
    mutable std::mutex _mutex;
    const size_t _maxBindings, _maxBytes, _maxWeakCells;
    Snapshot _live;
};

class ArchiveSourceBinding
{
    friend class QFBank;
    BankReadRequest _request;
    std::shared_ptr<const ArchiveSourceBudget::Ticket> _charge;
    ArchiveSourceBinding(BankReadRequest request, std::shared_ptr<const ArchiveSourceBudget::Ticket> charge)
        : _request(std::move(request)), _charge(std::move(charge)) {}
    static std::shared_ptr<const ArchiveSourceBinding> Create(BankReadRequest request);
    static void NoteWrappedRead();
    static void NoteWeakWrapperPublished();
public:
    // Optional model primary/material-stage Init purpose, NOT source identity or worker access.
    // Needed when BOTH warm-source and warm-job flags are exactly 1, or the
    // dedicated cold-handoff experiment is exactly 1; both use existing model Init.
    // source-only diagnostic capture keeps its original all-PAA behaviour.
    class ModelReadScope
    {
        bool _entered = false;
        bool _materialPreflightEntered = false;
    public:
        enum class Intent : uint8_t { Model, MaterialPreflight };
        explicit ModelReadScope(Intent intent = Intent::Model);
        ~ModelReadScope();
        ModelReadScope(const ModelReadScope&) = delete;
        ModelReadScope& operator=(const ModelReadScope&) = delete;
        ModelReadScope(ModelReadScope&&) = delete;
        ModelReadScope& operator=(ModelReadScope&&) = delete;
        static bool PurposeRequired();
        static bool IsActive(); // owner thread only; never grants worker bank access
        static bool MaterialPreflightReservationEnabled();
        static bool IsMaterialPreflightActive();
        static size_t BindingLimit(); // 254 ordinary/256 stage when explicitly enabled; 256 otherwise
    };
    // Exact retail rigid-model PAC experiment. The selected model may enter
    // this owner-only purpose while it performs its original Shape/texture Init.
    // Only the matching raw data.pbo member may obtain a binding; this scope
    // never resolves a path, opens a bank, or certifies a later mount by name.
    class RetailPacReadScope
    {
        bool _entered = false;
    public:
        explicit RetailPacReadScope(const char* exactModelPath);
        ~RetailPacReadScope();
        RetailPacReadScope(const RetailPacReadScope&) = delete;
        RetailPacReadScope& operator=(const RetailPacReadScope&) = delete;
        RetailPacReadScope(RetailPacReadScope&&) = delete;
        RetailPacReadScope& operator=(RetailPacReadScope&&) = delete;
        static bool Enabled();
        static bool Active();
        static bool MatchesBankMember(const char* bankPath, const char* prefix, const char* member);
        static bool MatchesLogicalName(const char* name);
    };
    // Original raw mapped member only; names/headers are not source identity.
    // The process admission cap follows shared bindings retained by buffers/sources.
    // Subsequent worker copies must additionally be charged to their own bounded job.
    struct Stats
    {
        bool enabled = false;
        size_t liveBindings = 0, knownCppBytes = 0, capBindings = 256, capBytes = 2 * 1024 * 1024;
        bool cacheHandoffEnabled = false;
        size_t weakCells = 0, weakKnownCppBytes = 0, capWeakCells = 1024;
        uint64_t weakCapacityRefused = 0, weakAllocationRefused = 0;
        // Cumulative wrapper publication and FIRST bootstrap completion only.
        uint64_t weakWrappersPublished = 0, weakInitializedHandoffs = 0, weakFailedInitCompletions = 0;
        // Charged reservation high-water; includes allocation overlap/failure, not RSS.
        size_t weakPeakReservationCells = 0, weakPeakReservationBytes = 0, traceKnownCppBytes = 0;
        uint64_t wrappedReads = 0, initRetains = 0, captureRefused = 0,
            capacityRefused = 0, allocationRefused = 0;
    };
    // Separate non-lease cells share the unchanged byte budget; no source-cap raise.
    static bool CacheHandoffEnabled();
    static std::shared_ptr<const ArchiveSourceBudget::Ticket> ReserveWeakCell(size_t bytes);
    static std::shared_ptr<const ArchiveSourceBudget::Ticket> ReserveTraceMetadata(size_t bytes);
    static Stats SnapshotStats();
    static void NoteCacheInitCompletion(bool initialized); // Owner-only weak cell first completion, after dropping bootstrap.
    static void NoteInitRetained(); // Called only after successful Init retaining buffer evidence.
    const BankReadRequest& Request() const { return _request; }
    // Capacity accounting includes guards for shared controls, not kernel/RSS/heap proof.
    size_t KnownCppBytes() const;
};
}
