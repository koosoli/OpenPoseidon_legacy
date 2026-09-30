#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <utility>

namespace Poseidon::render
{

// A final-binding guard, not a material plan or a source-freshness certificate.
// The owner supplies strong references and the exact resident handle/slot facts
// it already measured. In the scoped EnsureUploaded path, only those unchanged
// images may be reused. A refusal returns zero before any upload/retry mutation;
// the caller must abandon this registration and keep CPU coverage.
//
// The final resolver must use the captured references without bank Load, and
// must separately validate material/source freshness and role completeness.
// In particular, this does not intercept eager InitDynamic during Load.
template<class TextureRef>
class TextureReuseOnlyBinding
{
  public:
    static_assert(std::is_nothrow_move_assignable_v<TextureRef>);
    static constexpr size_t MaxRows = 128;

    struct Snapshot
    {
        uint64_t handle = 0;
        uint32_t slotLease = 0;
        bool dynamic = false;
        bool deferredGenerated = false;
    };
    struct Report
    {
        size_t rows = 0;
        uint64_t duplicateAdds = 0, allowedChecks = 0;
        uint64_t freezeRefused = 0, capacityRefused = 0;
        uint64_t deniedNotSealed = 0, deniedPoisoned = 0;
        uint64_t deniedUnexpected = 0, deniedCold = 0;
        uint64_t deniedChanged = 0, deniedDeferred = 0;
        bool sealed = false, failed = false, cancelled = false;
    };

    class Scope
    {
        TextureReuseOnlyBinding* _previous = nullptr;

      public:
        Scope(TextureReuseOnlyBinding& binding, bool exactSelected, bool ownerThread)
            : _previous(_active)
        {
            // An incomplete/failed plan must still intercept EnsureUploaded.
            _active = exactSelected && ownerThread && Enabled() ? &binding : nullptr;
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        ~Scope() { _active = _previous; }
    };

    TextureReuseOnlyBinding() = default;
    TextureReuseOnlyBinding(const TextureReuseOnlyBinding&) = delete;
    TextureReuseOnlyBinding& operator=(const TextureReuseOnlyBinding&) = delete;
    TextureReuseOnlyBinding(TextureReuseOnlyBinding&&) = delete;
    TextureReuseOnlyBinding& operator=(TextureReuseOnlyBinding&&) = delete;
    ~TextureReuseOnlyBinding() { Cancel(); }

    static TextureReuseOnlyBinding* Active() { return _active; }
    static bool Enabled()
    {
        const char* value = std::getenv("WGR_OBJECT_STREAM_DAYZ_REUSE_ONLY");
        return value && std::strcmp(value, "1") == 0;
    }

    bool Add(TextureRef held, Snapshot frozen)
    {
        if (_report.sealed || _report.failed || _report.cancelled)
            return Refuse(_report.freezeRefused);
        const void* identity = RefIdentity(held);
        if (!identity || !frozen.handle || frozen.deferredGenerated ||
            (!frozen.dynamic && !frozen.slotLease))
            return Refuse(_report.freezeRefused);
        for (size_t i = 0; i < _report.rows; ++i)
        {
            if (_rows[i].identity != identity) continue;
            if (_rows[i].frozen.handle != frozen.handle ||
                _rows[i].frozen.slotLease != frozen.slotLease ||
                _rows[i].frozen.dynamic != frozen.dynamic)
                return Refuse(_report.freezeRefused);
            ++_report.duplicateAdds;
            return true;
        }
        if (_report.rows == MaxRows || !ReserveProcessRow())
            return Refuse(_report.capacityRefused);
        Entry& row = _rows[_report.rows++];
        row.identity = identity;
        row.held = std::move(held);
        row.frozen = frozen;
        return true;
    }

    bool Seal()
    {
        if (_report.cancelled || _report.failed) return false;
        _report.sealed = true;
        return true;
    }

    // Called after NoteTextureUse and before EnsureUploaded's deferred-generated,
    // delegate, adaptive, retry, source-read or create branches. No IO here.
    uint64_t Check(const void* identity, Snapshot current)
    {
        if (!_report.sealed)
            return Deny(_report.deniedNotSealed);
        if (_report.failed || _report.cancelled)
            return Deny(_report.deniedPoisoned);
        for (size_t i = 0; i < _report.rows; ++i)
        {
            const Entry& row = _rows[i];
            if (row.identity != identity) continue;
            if (current.deferredGenerated) return Deny(_report.deniedDeferred);
            if (!current.handle) return Deny(_report.deniedCold);
            if (current.handle != row.frozen.handle ||
                current.slotLease != row.frozen.slotLease ||
                current.dynamic != row.frozen.dynamic)
                return Deny(_report.deniedChanged);
            ++_report.allowedChecks;
            return current.handle;
        }
        return Deny(_report.deniedUnexpected);
    }

    Report Read() const { return _report; }
    bool FinalRoleClosureProven() const { return false; }
    void Cancel()
    {
        if (_report.cancelled) return;
        _report.cancelled = true;
        for (size_t i = 0; i < _report.rows; ++i)
        {
            _rows[i].held = TextureRef{};
            ReleaseProcessRow();
        }
    }

  private:
    struct Entry
    {
        const void* identity = nullptr;
        TextureRef held;
        Snapshot frozen;
    };
    inline static thread_local TextureReuseOnlyBinding* _active = nullptr;
    inline static std::atomic<size_t> _processRows{0};
    std::array<Entry, MaxRows> _rows{};
    Report _report;

    static const void* RefIdentity(const TextureRef& ref)
    {
        if constexpr (requires { ref.GetRef(); }) return ref.GetRef();
        else return ref.get();
    }
    static bool ReserveProcessRow()
    {
        size_t count = _processRows.load(std::memory_order_relaxed);
        while (count < MaxRows)
            if (_processRows.compare_exchange_weak(count, count + 1,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) return true;
        return false;
    }
    static void ReleaseProcessRow() { _processRows.fetch_sub(1, std::memory_order_acq_rel); }
    bool Refuse(uint64_t& counter)
    {
        ++counter;
        _report.failed = true;
        return false;
    }
    uint64_t Deny(uint64_t& counter)
    {
        ++counter;
        _report.failed = true;
        return 0;
    }
};

} // namespace Poseidon::render
