#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

namespace Poseidon::render
{
// Owner-only accounting for a future exact-model registration path. This does
// not split RegisterGpuModel's synchronous material resolver: the caller must
// first produce a COMPLETE list of individually uploadable, final-resolver
// texture roles. A callback may operate on exactly its one supplied texture;
// it must report the measured upload-counter delta. Texture residency itself
// remains owned by the existing texture cache; queued meshes/models acquire
// explicit cancellation debt here. No role plan, no pilot.
class ResumableModelAdmission
{
public:
    static constexpr size_t MaxTextures = 128;
    static constexpr size_t MaxMeshes = 16;
    static constexpr size_t MaxModels = 64; // parent plus distinct proxy shapes
    static constexpr uint64_t MaxDuplicateGeometryBytes = 32ull * 1024 * 1024;
    static constexpr size_t MaxSourceName = 255;

    struct Member
    {
        uint64_t volume = 0, archiveBytes = 0, offset = 0, bytes = 0;
        std::array<uint8_t, 16> fileId{};
        bool operator==(const Member&) const = default;
        bool Bounded() const
        { return bytes != 0 && archiveBytes != 0 && offset <= archiveBytes && bytes <= archiveBytes - offset; }
    };
    // textureToken is a job-local identity of an owner-held Ref<Texture>. It is
    // never a cross-admission pointer identity. The owner retains the birth
    // archive lease separately and derives member/mountedCurrent from the same
    // final PAA source at every check, even if TextureWgpu drops its Init proof.
    struct TextureProof
    {
        uint64_t textureToken = 0;
        std::string_view sourceName;
        Member member;
        uint64_t handle = 0;
        uint32_t slotLease = 0;
        bool birthLeaseHeld = false;
        bool mountedCurrent = false;
    };
    struct UploadResult
    {
        uint64_t handle = 0;
        uint32_t slotLease = 0;
        uint32_t measuredUploads = 0;
        bool succeeded = false;
    };
    struct MeshResult
    {
        uint64_t admittedDuplicateBytes = 0;
        bool queued = false;
    };
    struct Limits
    {
        size_t textures = 0, meshes = 0, models = 0;
        uint64_t duplicateGeometryBytes = 0;
        bool finalRolesComplete = false;
    };
    enum class State : uint8_t
    { Empty, Planning, Staging, ReadyToRegister, AwaitingAck, ReadyToPublish, Published,
      Refused, Cancelling, Cancelled };
    enum class Refusal : uint8_t
    { None, IncompleteResolver, Capacity, InvalidSource, StaleSource, UploadFailed,
      QuotaViolation, MeshFailed, ModelFailed, AckFailed, NotReady };
    enum class DebtKind : uint8_t { ModelRetire, MeshDestroy };
    struct Debt { DebtKind kind; uint64_t producer; size_t index; };

    bool Begin(Limits limits)
    {
        if (_state != State::Empty) return false;
        if (!limits.finalRolesComplete) return Refuse(Refusal::IncompleteResolver);
        if (limits.textures > MaxTextures || limits.meshes > MaxMeshes ||
            limits.models == 0 || limits.models > MaxModels ||
            limits.duplicateGeometryBytes > MaxDuplicateGeometryBytes)
            return Refuse(Refusal::Capacity);
        _limits = limits;
        _state = State::Planning;
        return true;
    }
    bool AddTexture(const TextureProof& birth)
    {
        if (_state != State::Planning || _textureCount == _limits.textures) return Refuse(Refusal::Capacity);
        if (!ValidSource(birth))
            return Refuse(Refusal::InvalidSource);
        for (size_t i = 0; i < _textureCount; ++i)
            if (_textures[i].token == birth.textureToken) return Refuse(Refusal::IncompleteResolver);
        auto& row = _textures[_textureCount++];
        row.token = birth.textureToken;
        row.member = birth.member;
        row.sourceLength = birth.sourceName.size();
        std::memcpy(row.source.data(), birth.sourceName.data(), row.sourceLength);
        row.source[row.sourceLength] = '\0';
        return true;
    }
    bool AddMesh(uint64_t producer, uint64_t duplicateBytes)
    {
        if (_state != State::Planning || _meshCount == _limits.meshes || producer == 0 ||
            duplicateBytes == 0 || duplicateBytes > _limits.duplicateGeometryBytes - _meshBytes)
            return Refuse(Refusal::Capacity);
        for (size_t i = 0; i < _meshCount; ++i)
            if (_meshes[i].producer == producer) return Refuse(Refusal::Capacity);
        auto& row = _meshes[_meshCount++];
        row.producer = producer;
        row.duplicateBytes = duplicateBytes;
        _meshBytes += duplicateBytes;
        return true;
    }
    bool AddModel(uint64_t producer)
    {
        if (_state != State::Planning || _modelCount == _limits.models || producer == 0)
            return Refuse(Refusal::Capacity);
        for (size_t i = 0; i < _modelCount; ++i)
            if (_models[i].producer == producer) return Refuse(Refusal::Capacity);
        _models[_modelCount++].producer = producer;
        return true;
    }
    bool Seal()
    {
        if (_state != State::Planning || _textureCount != _limits.textures ||
            _meshCount != _limits.meshes || _modelCount != _limits.models ||
            _meshBytes != _limits.duplicateGeometryBytes)
            return Refuse(Refusal::IncompleteResolver);
        _state = State::Staging;
        return true;
    }

    // One call/update, one explicit texture role and one mesh at most. The
    // inspector must check the retained birth lease against the CURRENT bank
    // mount. If any source is unknown or changed, callbacks are not called.
    // The mesh callback must queue only its one precharged immutable mesh and
    // no texture work. A single EnsureUploaded can still be expensive; this
    // caps calls, not ms.
    template<class Inspect, class UploadOne, class CreateOne>
    bool Advance(uint64_t update, Inspect&& inspect, UploadOne&& uploadOne, CreateOne&& createOne)
    {
        if (_state != State::Staging || !FreshUpdate(update)) return Refuse(Refusal::NotReady);
        try
        {
        if (_nextTexture < _textureCount)
        {
            auto& row = _textures[_nextTexture];
            const TextureProof before = inspect(_nextTexture);
            if (!Matches(row, before, false)) return Refuse(Refusal::StaleSource);
            const UploadResult result = uploadOne(_nextTexture);
            if (result.measuredUploads > 1) return Refuse(Refusal::QuotaViolation);
            if (!result.succeeded || result.handle == 0 || result.slotLease == 0)
                return Refuse(Refusal::UploadFailed);
            const TextureProof after = inspect(_nextTexture);
            if (!Matches(row, after, false) || after.handle != result.handle ||
                after.slotLease != result.slotLease) return Refuse(Refusal::StaleSource);
            row.handle = result.handle;
            row.slotLease = result.slotLease;
            ++_nextTexture;
        }
        if (_nextMesh < _meshCount)
        {
            auto& row = _meshes[_nextMesh];
            // Record cancellation debt BEFORE the queue callback. If it throws
            // after enqueueing, FIFO MeshDestroy is still owed; if it never
            // enqueued, the destroy is a harmless missing-producer no-op.
            row.mayHaveQueued = true;
            // The caller must derive this from immutable geometry and reserve
            // exactly this many duplicate bytes before queueing the mesh.
            const MeshResult result = createOne(_nextMesh, row.producer, row.duplicateBytes);
            if (!result.queued) return Refuse(Refusal::MeshFailed);
            if (result.admittedDuplicateBytes != row.duplicateBytes)
                return Refuse(Refusal::QuotaViolation);
            ++_nextMesh;
        }
        if (_nextTexture == _textureCount && _nextMesh == _meshCount)
            _state = State::ReadyToRegister;
        return true;
        }
        catch (...) { return Refuse(Refusal::NotReady); } // mesh debt was recorded before its callback
    }
    // Same final resolver/source checks just before queueing each model. Model
    // registration is deliberately separate from mesh/texture steps. QueueOne
    // must consume staged handles/meshes only, never run the old resolver.
    template<class Inspect, class QueueOne>
    bool QueueOneModel(uint64_t update, Inspect&& inspect, QueueOne&& queueOne)
    {
        if ((_state != State::ReadyToRegister && _state != State::AwaitingAck) ||
            _nextModel == _modelCount || !FreshUpdate(update)) return Refuse(Refusal::NotReady);
        try
        {
        if (!AllFresh(inspect)) return Refuse(Refusal::StaleSource);
        auto& row = _models[_nextModel];
        row.mayHaveQueued = true; // debt precedes external enqueue
        if (!queueOne(_nextModel, row.producer)) return Refuse(Refusal::ModelFailed);
        ++_nextModel;
        _state = State::AwaitingAck;
        return true;
        }
        catch (...) { return Refuse(Refusal::NotReady); } // model debt was recorded before its callback
    }
    bool AcknowledgeModel(uint64_t producer, bool rendererModelValid, bool allMeshesLive)
    {
        if (_state != State::AwaitingAck || !rendererModelValid || !allMeshesLive)
            return Refuse(Refusal::AckFailed);
        for (size_t i = 0; i < _nextModel; ++i)
            if (_models[i].producer == producer && !_models[i].acknowledged)
            {
                _models[i].acknowledged = true;
                ++_acks;
                if (_acks == _modelCount && _nextModel == _modelCount)
                    _state = State::ReadyToPublish;
                return true;
            }
        return Refuse(Refusal::AckFailed);
    }
    template<class Inspect>
    bool PublishIfCurrent(Inspect&& inspect, bool liveObjects)
    {
        if (_state != State::ReadyToPublish || !liveObjects) return Refuse(Refusal::NotReady);
        try { if (!AllFresh(inspect)) return Refuse(Refusal::StaleSource); }
        catch (...) { return Refuse(Refusal::NotReady); }
        _state = State::Published;
        return true; // only now may the owner enqueue instances/change coverage
    }
    void Cancel()
    {
        if (_state == State::Cancelled || _state == State::Published) return;
        _state = HasDebt() ? State::Cancelling : State::Cancelled;
    }
    // Debt is not erased when an op is queued. The owner calls SettleDebt only
    // after the renderer drain acknowledges it, so a lost queue operation cannot
    // silently discard an allocation. ModelRetire queues precede MeshDestroy.
    bool NextDebt(Debt& out) const
    {
        if (_state != State::Cancelling) return false;
        for (size_t i = 0; i < _modelCount; ++i)
            if (_models[i].mayHaveQueued && !_models[i].retirementQueued)
            { out = {DebtKind::ModelRetire, _models[i].producer, i}; return true; }
        for (size_t i = 0; i < _meshCount; ++i)
            if (_meshes[i].mayHaveQueued && !_meshes[i].retirementQueued)
            { out = {DebtKind::MeshDestroy, _meshes[i].producer, i}; return true; }
        return false;
    }
    bool MarkDebtQueued(Debt debt)
    {
        if (_state != State::Cancelling) return false;
        ProducerRow* row = nullptr;
        if (debt.kind == DebtKind::ModelRetire)
        {
            if (debt.index >= _modelCount) return false;
            row = &_models[debt.index];
        }
        else
        {
            for (const auto& model : _models)
                if (model.mayHaveQueued && !model.retirementQueued) return false;
            if (debt.index >= _meshCount) return false;
            row = &_meshes[debt.index];
        }
        if (row->producer != debt.producer || !row->mayHaveQueued || row->retirementQueued)
            return false;
        row->retirementQueued = true;
        return true;
    }
    bool SettleDebt(Debt debt)
    {
        if (_state != State::Cancelling) return false;
        if (debt.kind == DebtKind::ModelRetire)
        {
            if (debt.index >= _modelCount || _models[debt.index].producer != debt.producer ||
                !_models[debt.index].retirementQueued || _models[debt.index].settled) return false;
            _models[debt.index].settled = true;
        }
        else
        {
            for (const auto& model : _models)
                if (model.mayHaveQueued && !model.retirementQueued) return false;
            if (debt.index >= _meshCount || _meshes[debt.index].producer != debt.producer ||
                !_meshes[debt.index].retirementQueued || _meshes[debt.index].settled) return false;
            _meshes[debt.index].settled = true;
        }
        if (!HasDebt()) _state = State::Cancelled;
        return true;
    }
    State GetState() const { return _state; }
    Refusal RefusalReason() const { return _refusal; }
    size_t TexturesDone() const { return _nextTexture; }
    size_t MeshesDone() const { return _nextMesh; }
    size_t ModelsAcknowledged() const { return _acks; }
    uint64_t DuplicateGeometryBytes() const { return _meshBytes; }

private:
    struct TextureRow
    {
        std::array<char, MaxSourceName + 1> source{};
        size_t sourceLength = 0;
        uint64_t token = 0, handle = 0;
        uint32_t slotLease = 0;
        Member member;
    };
    struct ProducerRow { uint64_t producer = 0, duplicateBytes = 0; bool mayHaveQueued = false, acknowledged = false,
        retirementQueued = false, settled = false; };
    std::array<TextureRow, MaxTextures> _textures{};
    std::array<ProducerRow, MaxMeshes> _meshes{};
    std::array<ProducerRow, MaxModels> _models{};
    Limits _limits{};
    State _state = State::Empty;
    Refusal _refusal = Refusal::None;
    size_t _textureCount = 0, _meshCount = 0, _modelCount = 0;
    size_t _nextTexture = 0, _nextMesh = 0, _nextModel = 0, _acks = 0;
    uint64_t _meshBytes = 0, _lastUpdate = 0;
    bool Refuse(Refusal why) { _refusal = why; _state = State::Refused; return false; }
    bool FreshUpdate(uint64_t update)
    { if (update == 0 || update <= _lastUpdate) return false; _lastUpdate = update; return true; }
    static bool ValidSource(const TextureProof& proof)
    { return proof.textureToken != 0 && !proof.sourceName.empty() &&
        proof.sourceName.size() <= MaxSourceName && proof.member.Bounded() &&
        proof.birthLeaseHeld && proof.mountedCurrent; }
    static bool Matches(const TextureRow& row, const TextureProof& proof, bool committed)
    { return ValidSource(proof) && proof.textureToken == row.token &&
        proof.sourceName.size() == row.sourceLength &&
        std::memcmp(proof.sourceName.data(), row.source.data(), row.sourceLength) == 0 &&
        proof.member == row.member &&
        (!committed || (proof.handle == row.handle && proof.slotLease == row.slotLease)); }
    template<class Inspect> bool AllFresh(Inspect&& inspect) const
    {
        if (_nextTexture != _textureCount) return false;
        for (size_t i = 0; i < _textureCount; ++i)
            if (!Matches(_textures[i], inspect(i), true)) return false;
        return true;
    }
    bool HasDebt() const
    {
        for (const auto& row : _models) if (row.mayHaveQueued && !row.settled) return true;
        for (const auto& row : _meshes) if (row.mayHaveQueued && !row.settled) return true;
        return false;
    }
};
} // namespace Poseidon::render
