#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>

namespace Poseidon::render::procedural::admission
{

// A deliberately small, owner-only holding pen for already encoded procedural
// pixels. It is not a residency promise: only a completed owner registration can
// establish coverage. Process and admission limits both include each name and
// payload object, and remain charged until upload or texture destruction.
inline constexpr size_t MaxPendingCount = 16;
inline constexpr size_t MaxPendingBytes = 1024 * 1024;
inline constexpr size_t MaxBirthNameBytes = 256;

struct Snapshot
{
    uint64_t deferred = 0;
    uint64_t staged = 0;
    uint64_t escaped = 0;
    uint64_t immediateFallback = 0;
    uint64_t nameMismatch = 0;
    uint64_t failed = 0;
    size_t pendingCount = 0;
    size_t pendingBytes = 0;
    bool aborted = false;
};

class Ledger : public std::enable_shared_from_this<Ledger>
{
  public:
    class Charge
    {
        friend class Ledger;
        std::shared_ptr<Ledger> _owner;
        size_t _bytes;
        Charge(std::shared_ptr<Ledger> owner, size_t bytes) : _owner(std::move(owner)), _bytes(bytes) {}

      public:
        Charge(const Charge&) = delete;
        Charge& operator=(const Charge&) = delete;
        ~Charge() { _owner->Release(_bytes); }
    };

    // This is used only after procedural Parse/Encode, never for ordinary PAA.
    // Caller supplies the complete charge, including its retained name/record.
    std::unique_ptr<Charge> Reserve(size_t bytes)
    {
        if (!bytes || bytes > MaxPendingBytes)
        {
            NoteImmediateFallback();
            return {};
        }
        std::lock_guard lock(_mutex);
        if (_pendingCount >= MaxPendingCount || _pendingBytes > MaxPendingBytes - bytes ||
            _processCount >= MaxPendingCount || _processBytes > MaxPendingBytes - bytes)
        {
            _immediateFallback.fetch_add(1, std::memory_order_relaxed);
            _aborted.store(true, std::memory_order_relaxed);
            return {};
        }
        // Allocate before charging. A failed allocation leaves the source RGBA
        // vector intact and the caller uploads it by the ordinary path.
        std::unique_ptr<Charge> charge;
        try { charge.reset(new Charge(shared_from_this(), bytes)); }
        catch (...) {
            _immediateFallback.fetch_add(1, std::memory_order_relaxed);
            _aborted.store(true, std::memory_order_relaxed);
            return {};
        }
        ++_pendingCount;
        _pendingBytes += bytes;
        ++_processCount;
        _processBytes += bytes;
        return charge;
    }

    void NoteDeferred() { _deferred.fetch_add(1, std::memory_order_relaxed); }

    void NoteStaged(bool success)
    {
        _staged.fetch_add(1, std::memory_order_relaxed);
        if (!success) NoteFailed();
    }
    void NoteEscaped(bool success)
    {
        _escaped.fetch_add(1, std::memory_order_relaxed);
        _aborted.store(true, std::memory_order_relaxed);
        if (!success) NoteFailed();
    }
    void NoteImmediateFallback()
    {
        _immediateFallback.fetch_add(1, std::memory_order_relaxed);
        _aborted.store(true, std::memory_order_relaxed);
    }
    void NoteNameMismatch()
    {
        _nameMismatch.fetch_add(1, std::memory_order_relaxed);
        _aborted.store(true, std::memory_order_relaxed);
    }
    void NoteFailed()
    {
        _failed.fetch_add(1, std::memory_order_relaxed);
        _aborted.store(true, std::memory_order_relaxed);
    }
    Snapshot Read() const
    {
        std::lock_guard lock(_mutex);
        return {_deferred.load(std::memory_order_relaxed), _staged.load(std::memory_order_relaxed),
                _escaped.load(std::memory_order_relaxed), _immediateFallback.load(std::memory_order_relaxed),
                _nameMismatch.load(std::memory_order_relaxed), _failed.load(std::memory_order_relaxed),
                _pendingCount, _pendingBytes, _aborted.load(std::memory_order_relaxed)};
    }

  private:
    void Release(size_t bytes)
    {
        std::lock_guard lock(_mutex);
        --_pendingCount;
        _pendingBytes -= bytes;
        --_processCount;
        _processBytes -= bytes;
    }
    inline static std::mutex _mutex;
    inline static size_t _processCount = 0;
    inline static size_t _processBytes = 0;
    size_t _pendingCount = 0;
    size_t _pendingBytes = 0;
    std::atomic<uint64_t> _deferred{0}, _staged{0}, _escaped{0}, _immediateFallback{0}, _nameMismatch{0}, _failed{0};
    std::atomic<bool> _aborted{false};
};

inline bool Enabled()
{
    const char* value = std::getenv("WGR_OBJECT_STREAM_DAYZ_GENERATED_DEFER");
    return value && std::strcmp(value, "1") == 0;
}

inline thread_local std::shared_ptr<Ledger> CurrentCapture;
inline thread_local std::shared_ptr<Ledger> CurrentStage;

class CaptureScope
{
    std::shared_ptr<Ledger> _previous;

  public:
    CaptureScope(std::shared_ptr<Ledger> ledger, bool selected)
        : _previous(std::move(CurrentCapture))
    {
        CurrentCapture = selected && Enabled() ? std::move(ledger) : nullptr;
    }
    CaptureScope(const CaptureScope&) = delete;
    CaptureScope& operator=(const CaptureScope&) = delete;
    ~CaptureScope() { CurrentCapture = std::move(_previous); }
};

class StageScope
{
    std::shared_ptr<Ledger> _previous;

  public:
    explicit StageScope(std::shared_ptr<Ledger> ledger)
        : _previous(std::move(CurrentStage))
    {
        CurrentStage = Enabled() ? std::move(ledger) : nullptr;
    }
    StageScope(const StageScope&) = delete;
    StageScope& operator=(const StageScope&) = delete;
    ~StageScope() { CurrentStage = std::move(_previous); }
};

inline bool IsOwnerStage(const Ledger* ledger) { return ledger && CurrentStage.get() == ledger; }

inline bool SameBirthName(const char* current, const char* birth)
{
    return current && birth && std::strcmp(current, birth) == 0;
}

inline void NoteUploadAttempt(Ledger& ledger, bool success)
{
    if (IsOwnerStage(&ledger)) ledger.NoteStaged(success);
    else ledger.NoteEscaped(success);
}

} // namespace Poseidon::render::procedural::admission
