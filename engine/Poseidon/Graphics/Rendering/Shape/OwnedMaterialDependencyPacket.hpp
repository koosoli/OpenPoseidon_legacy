#pragma once

#include <Poseidon/Graphics/Rendering/Shape/ResumableModelAdmission.hpp>
#include <Poseidon/Graphics/Rendering/Shape/RetainedMaterialRoleDeclarations.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Poseidon::render
{

// Holds actual owner-resolved Texture references and birth archive leases for
// bounded declaration rows. Capture performs NO lookup, file read, upload or
// material translation. The caller must already own each binding and take its
// physical member identity from that same birth lease. Completing this packet
// proves only the supplied rows; it NEVER proves all final resolver roles,
// sections, eligible proxies or whole-model GPU coverage.
template<class TextureRef, class Lease>
class OwnedMaterialDependencyPacket
{
  public:
    using Declarations = RetainedMaterialRoleDeclarations;
    using Member = ResumableModelAdmission::Member;
    static constexpr size_t MaxSections = 256;
    static constexpr size_t MaxRoleUses = 512;
    static constexpr size_t MaxUniqueTextures = ResumableModelAdmission::MaxTextures;

    enum class State : uint8_t { Capturing, Staging, DependenciesReady, Refused, Cancelled };
    enum class Refusal : uint8_t
    {
        None, IncompleteDeclarations, Capacity, MissingOwnerReference, NameMismatch,
        InvalidBirth, ConflictingAlias, StaleSource, EscapedGenerated,
        UploadFailed, UploadWitness, NotReady
    };
    struct Birth
    {
        uint64_t token = 0; // admission-local, never cross-admission pointer identity
        TextureRef texture; // strong owner ref, retained until packet dies/cancels
        std::string_view actualSourceName; // actual TextureWgpu::SourceName or generated Name
        std::shared_ptr<const Lease> physicalLease; // Init birth, not a fresh name lookup
        Member physicalMember; // copied from physicalLease->Request(), no I/O
        bool generatedDynamic = false; // actual TextureWgpu::IsDynamicTexture
        bool generatedDeferred = false; // actual HasDeferredGeneratedUpload at birth
    };
    struct Probe
    {
        const void* textureIdentity = nullptr; // must equal held Ref's current pointee
        std::string_view actualSourceName;
        Member physicalMember;
        uint64_t handle = 0;
        uint32_t slotLease = 0;
        bool mountedCurrent = false; // current mount/member/layout, not just birth lease
        bool generatedDynamic = false;
        bool generatedDeferred = false;
    };
    struct UploadWitness
    {
        uint64_t handle = 0;
        uint32_t slotLease = 0;
        uint32_t measuredImageCreates = 0; // callback-supplied witness, not independent proof
        bool ownerGeneratedStage = false; // exact generated ledger StageScope witness
        bool succeeded = false;
    };
    struct RoleBinding
    {
        uint64_t sectionToken = 0;
        uint16_t declarationIndex = 0;
        uint16_t dependencyIndex = 0;
        Declarations::Role role = Declarations::Role::Face;
        Declarations::Condition condition = Declarations::Condition::Always;
    };

    OwnedMaterialDependencyPacket() = default;
    OwnedMaterialDependencyPacket(const OwnedMaterialDependencyPacket&) = delete;
    OwnedMaterialDependencyPacket& operator=(const OwnedMaterialDependencyPacket&) = delete;
    OwnedMaterialDependencyPacket(OwnedMaterialDependencyPacket&&) = delete;
    OwnedMaterialDependencyPacket& operator=(OwnedMaterialDependencyPacket&&) = delete;

    bool BeginSection(uint64_t sectionToken, const Declarations& declarations)
    {
        if (_state != State::Capturing || _open || sectionToken == 0 || !declarations.DeclarationsComplete())
            return RefuseWith(Refusal::IncompleteDeclarations);
        if (_sectionCount == MaxSections || declarations.Count() > MaxRoleUses - _roleCount)
            return RefuseWith(Refusal::Capacity);
        for (size_t i = 0; i < _sectionCount; ++i)
            if (_sectionTokens[i] == sectionToken) return RefuseWith(Refusal::ConflictingAlias);
        // Copy the bounded declaration rows. A temporary caller plan must not
        // leave a borrowed pointer dangling between owner updates.
        _openPlan = declarations;
        _sectionTokens[_sectionCount++] = sectionToken;
        _open = &_openPlan;
        _openToken = sectionToken;
        _nextDeclaration = 0;
        return true;
    }

    // Sequential row admission makes omission/reordering within a captured
    // declaration visible. Distinct roles may alias the same exact texture
    // birth; each unique texture is staged only once.
    bool CaptureNext(Birth birth)
    {
        if (_state != State::Capturing || !_open || _nextDeclaration >= _open->Count())
            return RefuseWith(Refusal::IncompleteDeclarations);
        const auto& declared = _open->At(_nextDeclaration);
        if (!birth.token || !Identity(birth.texture)) return RefuseWith(Refusal::MissingOwnerReference);
        if (birth.actualSourceName.size() != declared.nameLength ||
            std::memcmp(birth.actualSourceName.data(), declared.name.data(), declared.nameLength) != 0)
            return RefuseWith(Refusal::NameMismatch);
        const bool generated = declared.sourceKind == Declarations::SourceKind::GeneratedColour;
        if (generated)
        {
            if (!birth.generatedDynamic || birth.physicalLease || birth.physicalMember != Member{} ||
                declared.generatedBytes == 0)
                return RefuseWith(Refusal::InvalidBirth);
        }
        else if (birth.generatedDynamic || birth.generatedDeferred || !birth.physicalLease ||
                 !birth.physicalMember.Bounded())
            return RefuseWith(Refusal::InvalidBirth);

        size_t dependency = _dependencyCount;
        for (size_t i = 0; i < _dependencyCount; ++i)
            if (_dependencies[i].token == birth.token || _dependencies[i].identity == Identity(birth.texture))
            {
                const auto& old = _dependencies[i];
                if (old.token != birth.token || old.identity != Identity(birth.texture) || old.generated != generated ||
                    old.generatedDeferred != birth.generatedDeferred ||
                    old.sourceLength != declared.nameLength ||
                    std::memcmp(old.source.data(), declared.name.data(), declared.nameLength) != 0 ||
                    old.member != birth.physicalMember ||
                    old.physicalLease.get() != birth.physicalLease.get())
                    return RefuseWith(Refusal::ConflictingAlias);
                dependency = i;
                break;
            }
        if (dependency == _dependencyCount)
        {
            if (_dependencyCount == MaxUniqueTextures) return RefuseWith(Refusal::Capacity);
            auto& row = _dependencies[_dependencyCount++];
            row.token = birth.token;
            row.identity = Identity(birth.texture);
            row.texture = std::move(birth.texture);
            row.physicalLease = std::move(birth.physicalLease);
            row.member = birth.physicalMember;
            row.generated = generated;
            row.generatedDeferred = birth.generatedDeferred;
            row.sourceLength = declared.nameLength;
            std::memcpy(row.source.data(), declared.name.data(), declared.nameLength);
            row.source[row.sourceLength] = '\0';
        }
        _roles[_roleCount++] = {_openToken, static_cast<uint16_t>(_nextDeclaration),
                                static_cast<uint16_t>(dependency), declared.role, declared.condition};
        ++_nextDeclaration;
        return true;
    }

    bool EndSection()
    {
        if (_state != State::Capturing || !_open || _nextDeclaration != _open->Count())
            return RefuseWith(Refusal::IncompleteDeclarations);
        _open = nullptr;
        _openToken = 0;
        _nextDeclaration = 0;
        return true;
    }
    bool SealCapturedRows()
    {
        if (_state != State::Capturing || _open || _sectionCount == 0)
            return RefuseWith(Refusal::IncompleteDeclarations);
        _state = _dependencyCount ? State::Staging : State::DependenciesReady;
        return true;
    }

    // Exactly one unique dependency per nonzero, increasing owner update.
    // Inspect must derive physical current-mount truth without opening pixels.
    // UploadOne must call EnsureUploaded on ONLY this retained TextureRef and
    // return independently observed counts/owner-generated-stage witness.
    template<class Inspect, class UploadOne>
    bool AdvanceOne(uint64_t update, Inspect&& inspect, UploadOne&& uploadOne)
    {
        if (_state != State::Staging || !update || update <= _lastUpdate)
            return RefuseWith(Refusal::NotReady);
        _lastUpdate = update;
        auto& row = _dependencies[_nextDependency];
        try
        {
            const Probe before = inspect(_nextDependency, row.texture, row.physicalLease);
            if (row.generated && row.generatedDeferred && !before.generatedDeferred &&
                SameIdentityAndName(row, before))
                return RefuseWith(Refusal::EscapedGenerated);
            if (!Matches(row, before, false)) return RefuseWith(Refusal::StaleSource);
            if (!row.generated && before.handle && !before.slotLease)
                return RefuseWith(Refusal::StaleSource);
            if (row.generated && !row.generatedDeferred && !before.handle)
                return RefuseWith(Refusal::StaleSource); // failed eager generated image has no file retry

            if (before.handle)
            {
                // Already resident: no callback and no new upload. A birth-held
                // deferred generated payload cannot have a handle yet.
                if (row.generatedDeferred) return RefuseWith(Refusal::EscapedGenerated);
                const Probe after = inspect(_nextDependency, row.texture, row.physicalLease);
                if (!Matches(row, after, true) || after.handle != before.handle ||
                    after.slotLease != before.slotLease) return RefuseWith(Refusal::StaleSource);
                row.handle = after.handle;
                row.slotLease = after.slotLease;
            }
            else
            {
                const UploadWitness result = uploadOne(_nextDependency, row.texture);
                if (result.measuredImageCreates != 1 ||
                    (row.generatedDeferred && !result.ownerGeneratedStage))
                    return RefuseWith(Refusal::UploadWitness);
                if (!result.succeeded || !result.handle || (!row.generated && !result.slotLease))
                    return RefuseWith(Refusal::UploadFailed);
                const Probe after = inspect(_nextDependency, row.texture, row.physicalLease);
                if (!Matches(row, after, true) || after.handle != result.handle ||
                    after.slotLease != result.slotLease ||
                    (!row.generated && before.slotLease && before.slotLease != result.slotLease))
                    return RefuseWith(Refusal::StaleSource);
                row.handle = result.handle;
                row.slotLease = result.slotLease;
            }
            ++_nextDependency;
            if (_nextDependency == _dependencyCount) _state = State::DependenciesReady;
            return true;
        }
        catch (...) { return RefuseWith(Refusal::NotReady); }
    }

    // Repeat before any later model queue/publish step; this is still only a
    // current-image check, not a proof that the final resolver/CPU complement was
    // exhaustively captured.
    template<class Inspect> bool AllStagedCurrent(Inspect&& inspect) const
    {
        if (_state != State::DependenciesReady) return false;
        try
        {
            for (size_t i = 0; i < _dependencyCount; ++i)
            {
                const auto& row = _dependencies[i];
                const Probe now = inspect(i, row.texture, row.physicalLease);
                if (!Matches(row, now, true) || now.handle != row.handle ||
                    now.slotLease != row.slotLease || !now.handle ||
                    (row.generated && now.generatedDeferred)) return false;
            }
            return true;
        }
        catch (...) { return false; }
    }

    std::optional<uint64_t> StagedHandle(uint64_t sectionToken, size_t declarationIndex) const
    {
        if (_state != State::DependenciesReady) return {};
        for (size_t i = 0; i < _roleCount; ++i)
            if (_roles[i].sectionToken == sectionToken && _roles[i].declarationIndex == declarationIndex)
                return _dependencies[_roles[i].dependencyIndex].handle;
        return {};
    }
    void Cancel()
    {
        if (_state == State::Cancelled) return;
        _state = State::Cancelled;
        for (size_t i = 0; i < _dependencyCount; ++i)
        {
            _dependencies[i].texture = TextureRef{};
            _dependencies[i].physicalLease.reset();
        }
    }
    // An explicit hard no: only the caller's final resolver can assert this
    // after inspecting every eligible parent/proxy section and hidden branch.
    bool FinalRoleClosureProven() const { return false; }
    State GetState() const { return _state; }
    Refusal RefusalReason() const { return _refusal; }
    size_t SectionsCaptured() const { return _sectionCount; }
    size_t RoleUsesCaptured() const { return _roleCount; }
    size_t UniqueDependencies() const { return _dependencyCount; }
    size_t DependenciesStaged() const { return _nextDependency; }
    const RoleBinding& RoleAt(size_t i) const { return _roles[i]; }

  private:
    struct Dependency
    {
        uint64_t token = 0, handle = 0;
        uint32_t slotLease = 0;
        const void* identity = nullptr;
        TextureRef texture;
        std::shared_ptr<const Lease> physicalLease;
        Member member;
        std::array<char, Declarations::MaxName + 1> source{};
        size_t sourceLength = 0;
        bool generated = false, generatedDeferred = false;
    };
    std::array<Dependency, MaxUniqueTextures> _dependencies{};
    std::array<RoleBinding, MaxRoleUses> _roles{};
    std::array<uint64_t, MaxSections> _sectionTokens{};
    Declarations _openPlan;
    const Declarations* _open = nullptr;
    uint64_t _openToken = 0, _lastUpdate = 0;
    size_t _sectionCount = 0, _roleCount = 0, _dependencyCount = 0;
    size_t _nextDeclaration = 0, _nextDependency = 0;
    State _state = State::Capturing;
    Refusal _refusal = Refusal::None;

    static const void* Identity(const TextureRef& ref)
    {
        if constexpr (requires { ref.GetRef(); }) return ref.GetRef();
        else return ref.get();
    }
    static bool SameIdentityAndName(const Dependency& row, const Probe& probe)
    {
        return probe.textureIdentity == row.identity && probe.actualSourceName.size() == row.sourceLength &&
            std::memcmp(probe.actualSourceName.data(), row.source.data(), row.sourceLength) == 0;
    }
    static bool Matches(const Dependency& row, const Probe& probe, bool afterUpload)
    {
        if (!SameIdentityAndName(row, probe)) return false;
        if (row.generated)
            return probe.generatedDynamic && probe.physicalMember == Member{} &&
                probe.generatedDeferred == (row.generatedDeferred && !afterUpload);
        return !probe.generatedDynamic && !probe.generatedDeferred && probe.mountedCurrent &&
            probe.physicalMember == row.member;
    }
    bool RefuseWith(Refusal reason)
    {
        _refusal = reason;
        _state = State::Refused;
        return false;
    }
};

} // namespace Poseidon::render
