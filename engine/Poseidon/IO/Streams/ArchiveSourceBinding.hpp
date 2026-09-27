#pragma once
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <mutex>
#include <memory>

namespace Poseidon
{
// Optional source metadata only. No pixel buffer or engine Ref belongs here.
class ArchiveSourceBudget : public std::enable_shared_from_this<ArchiveSourceBudget>
{
public:
    struct Snapshot { size_t bindings = 0, bytes = 0; };
    class Ticket
    {
        friend class ArchiveSourceBudget;
        struct Key { private: Key() = default; friend class ArchiveSourceBudget; };
        std::shared_ptr<ArchiveSourceBudget> _budget;
        size_t _bytes;
    public:
        Ticket(Key, std::shared_ptr<ArchiveSourceBudget> budget, size_t bytes) noexcept
            : _budget(std::move(budget)), _bytes(bytes) {}
        Ticket(const Ticket&) = delete;
        Ticket& operator=(const Ticket&) = delete;
        Ticket(Ticket&&) = delete;
        Ticket& operator=(Ticket&&) = delete;
        ~Ticket() { _budget->Release(_bytes); }
    };
    ArchiveSourceBudget(size_t bindings, size_t bytes) : _maxBindings(bindings), _maxBytes(bytes) {}
    std::shared_ptr<const Ticket> Acquire(size_t bytes, bool* capacityRefused = nullptr)
    {
        if (capacityRefused) *capacityRefused = false;
        std::lock_guard<std::mutex> lock(_mutex);
        if (!bytes || _live.bindings >= _maxBindings || bytes > _maxBytes - _live.bytes)
        { if (capacityRefused) *capacityRefused = true; return {}; }
        try
        {
            // make_shared allocates BEFORE this noexcept constructor. Failed allocation
            // cannot destroy an uncharged Ticket or reenter our locked Release path.
            auto ticket = std::make_shared<const Ticket>(Ticket::Key{}, shared_from_this(), bytes);
            ++_live.bindings; _live.bytes += bytes;
            return ticket;
        }
        catch (...) { return {}; }
    }
    Snapshot Observe() const { std::lock_guard<std::mutex> lock(_mutex); return _live; }
private:
    void Release(size_t bytes)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        --_live.bindings; _live.bytes -= bytes;
    }
    mutable std::mutex _mutex;
    const size_t _maxBindings, _maxBytes;
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
public:
    // Optional model-primary Init purpose, NOT source identity or worker access.
    // Needed only when BOTH warm-source and warm-job flags are exactly 1;
    // source-only diagnostic capture keeps its original all-PAA behaviour.
    class ModelReadScope
    {
        bool _entered = false;
    public:
        ModelReadScope();
        ~ModelReadScope();
        ModelReadScope(const ModelReadScope&) = delete;
        ModelReadScope& operator=(const ModelReadScope&) = delete;
        ModelReadScope(ModelReadScope&&) = delete;
        ModelReadScope& operator=(ModelReadScope&&) = delete;
        static bool PurposeRequired();
        static bool IsActive(); // owner thread only; never grants worker bank access
    };
    // Original raw mapped member only; names/headers are not source identity.
    // The process admission cap follows shared bindings retained by buffers/sources.
    // Subsequent worker copies must additionally be charged to their own bounded job.
    struct Stats
    {
        bool enabled = false;
        size_t liveBindings = 0, knownCppBytes = 0, capBindings = 256, capBytes = 2 * 1024 * 1024;
        uint64_t wrappedReads = 0, initRetains = 0, captureRefused = 0,
            capacityRefused = 0, allocationRefused = 0;
    };
    static Stats SnapshotStats();
    static void NoteInitRetained(); // Called only after successful Init retaining buffer evidence.
    const BankReadRequest& Request() const { return _request; }
    // Capacity accounting includes guards for shared controls, not kernel/RSS/heap proof.
    size_t KnownCppBytes() const;
};
}
