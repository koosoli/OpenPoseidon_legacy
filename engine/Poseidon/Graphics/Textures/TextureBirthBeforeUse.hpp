#pragma once

#include <Poseidon/Graphics/Rendering/Shape/ResumableModelAdmission.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Poseidon::render
{

// Owner-only witness at TextureBankWgpu::NoteTextureUse, which EnsureUploaded
// calls BEFORE it can retire a texture's initialized archive proof. A scoped
// caller selects one exact target; ordinary paths see one TLS pointer check.
// Capture owns refs and birth leases, but does not enumerate material roles,
// read files/pixels, upload, or establish whole-model completeness.
template<class TextureRef, class Lease>
class TextureBirthBeforeUse
{
  public:
    static_assert(std::is_nothrow_move_assignable_v<TextureRef>,
                  "captured strong refs must move without leaving an uncharged partial row");
    using Member = ResumableModelAdmission::Member;
    static constexpr size_t MaxRows = 128;
    static constexpr size_t MaxName = 255;

    struct Facts
    {
        TextureRef texture; // actual strong ref made by the owner callback
        std::string_view sourceName; // SourceName for physical, Name for generated
        std::shared_ptr<const Lease> currentBirthLease;
        Member member; // exact identity copied from currentBirthLease, no I/O
        bool dynamic = false;
        bool generatedDeferred = false;
        bool mountedCurrent = false;
        uint64_t handleBeforeUse = 0;
        uint32_t slotLeaseBeforeUse = 0;
    };
    enum class Kind : uint8_t { Physical, Generated };
    struct Entry
    {
        uint64_t token = 0; // admission-local only
        const void* identity = nullptr;
        TextureRef texture;
        std::shared_ptr<const Lease> birthLease;
        Member member;
        std::array<char, MaxName + 1> source{};
        size_t sourceLength = 0;
        Kind kind = Kind::Physical;
        bool generatedDeferred = false;
        uint64_t handleBeforeUse = 0;
        uint32_t slotLeaseBeforeUse = 0;
        std::string_view SourceName() const { return {source.data(), sourceLength}; }
    };
    struct Report
    {
        uint64_t observedCalls = 0, duplicateCalls = 0;
        size_t captured = 0, physical = 0, generated = 0;
        uint64_t missingSource = 0, unsupportedDynamic = 0, invalidName = 0, capacityRefused = 0;
        bool partial = false;
        bool cancelled = false;
    };

    class Scope
    {
        TextureBirthBeforeUse* _previous = nullptr;

      public:
        Scope(TextureBirthBeforeUse& observer, bool exactSelected, bool ownerThread)
            : _previous(_active)
        {
            _active = exactSelected && ownerThread && Enabled() ? &observer : nullptr;
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        ~Scope() { _active = _previous; }
    };

    TextureBirthBeforeUse() = default;
    TextureBirthBeforeUse(const TextureBirthBeforeUse&) = delete;
    TextureBirthBeforeUse& operator=(const TextureBirthBeforeUse&) = delete;
    TextureBirthBeforeUse(TextureBirthBeforeUse&&) = delete;
    TextureBirthBeforeUse& operator=(TextureBirthBeforeUse&&) = delete;
    ~TextureBirthBeforeUse() { Cancel(); }

    static TextureBirthBeforeUse* Active() { return _active; }
    static bool Enabled()
    {
        const char* value = std::getenv("WGR_OBJECT_STREAM_DAYZ_BIRTH_OBSERVER");
        return value && std::strcmp(value, "1") == 0;
    }

    template<class Gather>
    bool Observe(const void* identity, Gather&& gather)
    {
        if (_report.cancelled || !identity) return Refuse(_report.invalidName);
        ++_report.observedCalls;
        for (size_t i = 0; i < _report.captured; ++i)
            if (_rows[i].identity == identity)
            { ++_report.duplicateCalls; return true; }
        if (_report.captured == MaxRows || !ReserveProcessRow())
            return Refuse(_report.capacityRefused);

        // The bank supplies a no-load callback so existing getters are called
        // only for a new, in-cap texture. Failure never blocks ordinary use.
        try
        {
            Facts facts = gather();
            const void* held = RefIdentity(facts.texture);
            if (held != identity)
            { ReleaseProcessRow(); return Refuse(_report.invalidName); }
            const size_t length = BoundedLength(facts.sourceName);
            if (!length)
            { ReleaseProcessRow(); return Refuse(_report.invalidName); }
            const bool generated = facts.dynamic && facts.sourceName[0] == '#';
            if (facts.dynamic && !generated)
            { ReleaseProcessRow(); return Refuse(_report.unsupportedDynamic); }
            if (generated)
            {
                if (facts.currentBirthLease || facts.member != Member{})
                { ReleaseProcessRow(); return Refuse(_report.unsupportedDynamic); }
            }
            else if (!facts.currentBirthLease || !facts.mountedCurrent || !facts.member.Bounded())
            { ReleaseProcessRow(); return Refuse(_report.missingSource); }

            Entry& row = _rows[_report.captured];
            row.token = _report.captured + 1;
            row.identity = identity;
            row.texture = std::move(facts.texture);
            row.birthLease = std::move(facts.currentBirthLease);
            row.member = facts.member;
            row.sourceLength = length;
            std::memcpy(row.source.data(), facts.sourceName.data(), length);
            row.source[length] = '\0';
            row.kind = generated ? Kind::Generated : Kind::Physical;
            row.generatedDeferred = facts.generatedDeferred;
            row.handleBeforeUse = facts.handleBeforeUse;
            row.slotLeaseBeforeUse = facts.slotLeaseBeforeUse;
            ++_report.captured;
            if (generated) ++_report.generated;
            else ++_report.physical;
            return true;
        }
        catch (...)
        {
            ReleaseProcessRow();
            return Refuse(_report.missingSource);
        }
    }

    const Entry& At(size_t index) const { return _rows[index]; }
    Report Read() const { return _report; }
    bool FinalRoleClosureProven() const { return false; }
    void Cancel()
    {
        if (_report.cancelled) return;
        _report.cancelled = true;
        for (size_t i = 0; i < _report.captured; ++i)
        {
            _rows[i].texture = TextureRef{};
            _rows[i].birthLease.reset();
            ReleaseProcessRow();
        }
    }

  private:
    inline static thread_local TextureBirthBeforeUse* _active = nullptr;
    inline static std::atomic<size_t> _processRows{0};
    std::array<Entry, MaxRows> _rows{};
    Report _report;

    static const void* RefIdentity(const TextureRef& ref)
    {
        if constexpr (requires { ref.GetRef(); }) return ref.GetRef();
        else return ref.get();
    }
    static size_t BoundedLength(std::string_view name)
    {
        return !name.empty() && name.size() <= MaxName ? name.size() : 0;
    }
    static bool ReserveProcessRow()
    {
        size_t count = _processRows.load(std::memory_order_relaxed);
        while (count < MaxRows)
            if (_processRows.compare_exchange_weak(count, count + 1, std::memory_order_acq_rel,
                                                   std::memory_order_relaxed)) return true;
        return false;
    }
    static void ReleaseProcessRow() { _processRows.fetch_sub(1, std::memory_order_acq_rel); }
    bool Refuse(uint64_t& counter)
    {
        ++counter;
        _report.partial = true;
        return false;
    }
};

} // namespace Poseidon::render
