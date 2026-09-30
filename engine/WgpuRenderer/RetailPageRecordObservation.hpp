#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <span>

namespace Poseidon::render
{
// Optional record-only receipt for the private retail page pilot. The packet
// freezes numeric producer identities before the queue crosses threads. A
// successful observation says only that the drain saw these model mappings and
// generational mesh records after preceding resource operations. It does not
// establish source freshness, an instance, a rendered frame, GPU completion, or
// physical allocation release. The owner must recheck its source authority.
class RetailPageRecordObservation
{
public:
    static constexpr size_t MaxMeshes = 16, MaxModels = 4;
    static constexpr uint32_t InvalidModel = std::numeric_limits<uint32_t>::max();
    enum class ExpectedMesh : uint8_t { Present = 1, Absent = 2 };
    enum class ExpectedModel : uint8_t { MappedValid = 1, Absent = 2 };
    enum class State : uint8_t { Requested, Observed, Rejected };
    enum class Refusal : uint8_t { None, InvalidPacket, Getter, Summary, Mesh, Model };

    struct MeshExpectation
    {
        uint64_t producer = 0;
        ExpectedMesh expected = ExpectedMesh::Present;
        // Required for Absent: the exact renderer handle observed while this
        // producer still mapped. An erased map entry alone cannot prove death.
        uint64_t previousRenderer = 0;
    };
    struct ModelExpectation
    {
        uint32_t producer = InvalidModel;
        ExpectedModel expected = ExpectedModel::MappedValid;
    };
    struct MeshEvidence
    {
        bool mappingExists = false;
        uint64_t mappedRenderer = 0;
        uint64_t factRenderer = 0, vertexBytes = 0, indexBytes = 0;
        uint32_t factState = 0, factReserved = 0;
    };
    struct ModelEvidence
    {
        bool mappingExists = false;
        uint32_t mappedRenderer = InvalidModel;
    };
    struct FactSummary
    {
        uint32_t requested = 0, inspected = 0, present = 0, absent = 0;
        uint32_t invalid = 0, duplicateHandles = 0, complete = 0, flags = 0;
        uint32_t recordScopeValid = 0, reserved = 0;
    };
    struct DrainEvidence
    {
        bool getterSucceeded = false;
        FactSummary summary;
        std::array<MeshEvidence, MaxMeshes> meshes{};
        std::array<ModelEvidence, MaxModels> models{};
    };
    struct Snapshot
    {
        State state = State::Requested;
        Refusal refusal = Refusal::None;
        uint64_t ownerEpoch = 0, sourceAdmissionEpoch = 0, requestId = 0;
        uint8_t meshCount = 0, modelCount = 0, presentMeshes = 0, absentMeshes = 0;
        uint8_t mappedModels = 0, absentModels = 0;
        std::array<uint64_t, MaxMeshes> rendererMeshes{};
        std::array<uint8_t, MaxMeshes> meshStates{};
        std::array<uint32_t, MaxModels> rendererModels{};
    };

    RetailPageRecordObservation(uint64_t ownerEpoch, uint64_t sourceAdmissionEpoch,
        uint64_t requestId, std::span<const MeshExpectation> meshes,
        std::span<const ModelExpectation> models) noexcept
    {
        _snapshot.ownerEpoch = ownerEpoch;
        _snapshot.sourceAdmissionEpoch = sourceAdmissionEpoch;
        _snapshot.requestId = requestId;
        _snapshot.rendererModels.fill(InvalidModel);
        if (!ownerEpoch || ownerEpoch == UINT64_MAX || !sourceAdmissionEpoch ||
            sourceAdmissionEpoch == UINT64_MAX || !requestId || requestId == UINT64_MAX ||
            meshes.empty() || meshes.size() > MaxMeshes || models.size() > MaxModels)
        { RejectInvalid(); return; }
        for (size_t i = 0; i < meshes.size(); ++i)
        {
            const auto& row = meshes[i];
            if (!row.producer || row.producer == UINT64_MAX ||
                (row.expected != ExpectedMesh::Present && row.expected != ExpectedMesh::Absent) ||
                (row.expected == ExpectedMesh::Present && row.previousRenderer) ||
                (row.expected == ExpectedMesh::Absent &&
                    (!row.previousRenderer || row.previousRenderer == UINT64_MAX)))
            { RejectInvalid(); return; }
            for (size_t j = 0; j < i; ++j)
                if (meshes[j].producer == row.producer ||
                    (row.expected == ExpectedMesh::Absent &&
                     meshes[j].expected == ExpectedMesh::Absent &&
                     meshes[j].previousRenderer == row.previousRenderer))
                { RejectInvalid(); return; }
            _meshes[i] = row;
        }
        for (size_t i = 0; i < models.size(); ++i)
        {
            const auto& row = models[i];
            if (row.producer == InvalidModel ||
                (row.expected != ExpectedModel::MappedValid && row.expected != ExpectedModel::Absent))
            { RejectInvalid(); return; }
            for (size_t j = 0; j < i; ++j)
                if (models[j].producer == row.producer) { RejectInvalid(); return; }
            _models[i] = row;
        }
        _meshCount = static_cast<uint8_t>(meshes.size());
        _modelCount = static_cast<uint8_t>(models.size());
        _snapshot.meshCount = _meshCount;
        _snapshot.modelCount = _modelCount;
        _valid = true;
    }

    bool Valid() const noexcept { return _valid; }
    size_t MeshCount() const noexcept { return _meshCount; }
    size_t ModelCount() const noexcept { return _modelCount; }
    MeshExpectation MeshAt(size_t i) const noexcept { return i < _meshCount ? _meshes[i] : MeshExpectation{}; }
    ModelExpectation ModelAt(size_t i) const noexcept { return i < _modelCount ? _models[i] : ModelExpectation{}; }

    // Drain-only. The caller collects every map lookup and ONE complete mesh
    // fact result, then publishes all rows together. Any mismatch is rejected
    // without exposing a partly successful record cut.
    bool Publish(const DrainEvidence& evidence) noexcept
    {
        std::lock_guard lock(_mutex);
        if (_state.load(std::memory_order_relaxed) != State::Requested) return false;
        const Refusal refusal = Validate(evidence);
        if (refusal != Refusal::None)
        {
            _snapshot.refusal = refusal;
            _snapshot.state = State::Rejected;
            _state.store(State::Rejected, std::memory_order_release);
            return false;
        }
        for (size_t i = 0; i < _meshCount; ++i)
        {
            const auto& row = evidence.meshes[i];
            _snapshot.rendererMeshes[i] = row.factRenderer;
            _snapshot.meshStates[i] = static_cast<uint8_t>(row.factState);
            if (row.factState == 1) ++_snapshot.presentMeshes;
            else ++_snapshot.absentMeshes;
        }
        for (size_t i = 0; i < _modelCount; ++i)
        {
            const auto& row = evidence.models[i];
            if (row.mappingExists)
            { _snapshot.rendererModels[i] = row.mappedRenderer; ++_snapshot.mappedModels; }
            else ++_snapshot.absentModels;
        }
        _snapshot.state = State::Observed;
        _state.store(State::Observed, std::memory_order_release);
        return true;
    }
    void ConsumeRejected() noexcept
    {
        std::lock_guard lock(_mutex);
        if (_state.load(std::memory_order_relaxed) != State::Requested) return;
        _snapshot.state = State::Rejected;
        _snapshot.refusal = Refusal::Getter;
        _state.store(State::Rejected, std::memory_order_release);
    }
    Snapshot Observe() const noexcept
    {
        (void)_state.load(std::memory_order_acquire);
        std::lock_guard lock(_mutex);
        return _snapshot;
    }

private:
    void RejectInvalid() noexcept
    {
        _snapshot.state = State::Rejected;
        _snapshot.refusal = Refusal::InvalidPacket;
        _state.store(State::Rejected, std::memory_order_release);
    }
    Refusal Validate(const DrainEvidence& e) const noexcept
    {
        if (!_valid) return Refusal::InvalidPacket;
        if (!e.getterSucceeded) return Refusal::Getter;
        const auto& s = e.summary;
        if (s.requested != _meshCount || s.inspected != _meshCount ||
            s.present + s.absent != _meshCount || s.invalid || s.duplicateHandles ||
            s.complete != 1 || s.flags || s.recordScopeValid != 1 || s.reserved)
            return Refusal::Summary;
        uint32_t present = 0, absent = 0;
        for (size_t i = 0; i < _meshCount; ++i)
        {
            const auto& expected = _meshes[i];
            const auto& row = e.meshes[i];
            if (row.factReserved || !row.factRenderer || row.factRenderer == UINT64_MAX)
                return Refusal::Mesh;
            for (size_t j = 0; j < i; ++j)
                if (e.meshes[j].factRenderer == row.factRenderer) return Refusal::Mesh;
            if (expected.expected == ExpectedMesh::Present)
            {
                if (!row.mappingExists || !row.mappedRenderer ||
                    row.mappedRenderer == UINT64_MAX ||
                    row.mappedRenderer != row.factRenderer || row.factState != 1 ||
                    !row.vertexBytes || !row.indexBytes) return Refusal::Mesh;
                ++present;
            }
            else
            {
                if (row.mappingExists || row.mappedRenderer ||
                    row.factRenderer != expected.previousRenderer || row.factState != 2 ||
                    row.vertexBytes || row.indexBytes) return Refusal::Mesh;
                ++absent;
            }
        }
        if (present != s.present || absent != s.absent) return Refusal::Summary;
        for (size_t i = 0; i < _modelCount; ++i)
        {
            const auto& expected = _models[i];
            const auto& row = e.models[i];
            if (expected.expected == ExpectedModel::MappedValid)
            {
                if (!row.mappingExists || row.mappedRenderer == InvalidModel)
                    return Refusal::Model;
                for (size_t j = 0; j < i; ++j)
                    if (e.models[j].mappingExists &&
                        e.models[j].mappedRenderer == row.mappedRenderer)
                        return Refusal::Model;
            }
            else if (row.mappingExists || row.mappedRenderer != InvalidModel)
                return Refusal::Model;
        }
        return Refusal::None;
    }

    bool _valid = false;
    uint8_t _meshCount = 0, _modelCount = 0;
    std::array<MeshExpectation, MaxMeshes> _meshes{};
    std::array<ModelExpectation, MaxModels> _models{};
    mutable std::mutex _mutex;
    std::atomic<State> _state{State::Requested};
    Snapshot _snapshot;
};
} // namespace Poseidon::render
