#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace Poseidon::render
{
// Optional producer-to-drain receipt for one selected ModelRegister operation.
// Only bounded numeric identity and an exact producer-mesh set cross the queue.
// ConsumedAccepted means the FFI returned a model and all expected meshes mapped
// at the drain; it is neither source freshness, GPU coverage, nor retirement ACK.
class ModelAdmissionReceipt
{
public:
    static constexpr size_t MaxMeshes = 16;
    static constexpr size_t MaxSections = 256;
    static constexpr size_t MaxLods = 64;
    static constexpr uint32_t InvalidModel = std::numeric_limits<uint32_t>::max();
    enum class State : uint8_t { Pending, ConsumedRejected, ConsumedAccepted };
    struct Observation
    {
        State state = State::Pending;
        uint32_t rendererModel = InvalidModel;
    };

    ModelAdmissionReceipt(uint32_t producerModel, uint64_t ownerEpoch,
        uint64_t sourceAdmissionEpoch, uint64_t requestId,
        std::span<const uint64_t> expectedMeshes)
        : _producerModel(producerModel), _ownerEpoch(ownerEpoch),
          _sourceAdmissionEpoch(sourceAdmissionEpoch), _requestId(requestId)
    {
        if (_producerModel == InvalidModel || !_ownerEpoch || !_sourceAdmissionEpoch || !_requestId ||
            _ownerEpoch == UINT64_MAX || _sourceAdmissionEpoch == UINT64_MAX || _requestId == UINT64_MAX ||
            expectedMeshes.empty() || expectedMeshes.size() > MaxMeshes) return;
        for (size_t i = 0; i < expectedMeshes.size(); ++i)
        {
            if (!expectedMeshes[i] || expectedMeshes[i] == UINT64_MAX) return;
            for (size_t j = 0; j < i; ++j)
                if (expectedMeshes[i] == expectedMeshes[j]) return;
            _expectedMeshes[i] = expectedMeshes[i];
        }
        _meshCount = static_cast<uint8_t>(expectedMeshes.size());
        _valid = true;
    }

    bool ValidFor(uint32_t producerModel) const noexcept
    { return _valid && producerModel == _producerModel; }
    uint32_t ProducerModel() const noexcept { return _producerModel; }
    uint64_t OwnerEpoch() const noexcept { return _ownerEpoch; }
    uint64_t SourceAdmissionEpoch() const noexcept { return _sourceAdmissionEpoch; }
    uint64_t RequestId() const noexcept { return _requestId; }
    size_t ExpectedMeshCount() const noexcept { return _meshCount; }

    // Check the FULL section table before the producer->renderer translation.
    // Every section must belong to exactly one LOD, reference one expected mesh,
    // and have a nonzero current renderer mapping. Every expected mesh must occur.
    // The drain's normal map lookup/translation follows immediately on one thread.
    template<class Sections, class Lods, class RendererMesh>
    bool MatchesBeforeTranslation(uint32_t producerModel, const Sections& sections,
        const Lods& lods, RendererMesh&& rendererMesh) const
    {
        if (!ValidFor(producerModel) || _state.load(std::memory_order_acquire) != State::Pending ||
            sections.empty() || sections.size() > MaxSections ||
            lods.empty() || lods.size() > MaxLods) return false;
        std::array<uint8_t, MaxSections> coverage{};
        for (const auto& lod : lods)
        {
            const uint64_t begin = lod.section_base;
            const uint64_t count = lod.section_count;
            if (!count || begin > sections.size() || count > sections.size() - begin) return false;
            for (uint64_t section = begin; section < begin + count; ++section)
                if (++coverage[static_cast<size_t>(section)] != 1) return false;
        }
        uint32_t seen = 0;
        for (size_t section = 0; section < sections.size(); ++section)
        {
            if (coverage[section] != 1) return false;
            const uint64_t mesh = sections[section].mesh;
            size_t index = 0;
            while (index < _meshCount && _expectedMeshes[index] != mesh) ++index;
            if (index == _meshCount || !rendererMesh(mesh)) return false;
            seen |= uint32_t(1) << index;
        }
        return seen == ((uint32_t(1) << _meshCount) - 1);
    }

    // Drain-only writes. The release store publishes the renderer ID to an
    // owner-thread acquire snapshot. A cancelled owner still waits for consumed
    // state before ordering retirement; this receipt does not settle that debt.
    void ConsumeRejected() noexcept
    {
        State pending = State::Pending;
        _state.compare_exchange_strong(pending, State::ConsumedRejected,
            std::memory_order_release, std::memory_order_relaxed);
    }
    bool ConsumeAccepted(uint32_t rendererModel) noexcept
    {
        if (rendererModel == InvalidModel || !_valid) { ConsumeRejected(); return false; }
        if (_state.load(std::memory_order_relaxed) != State::Pending) return false;
        _rendererModel.store(rendererModel, std::memory_order_relaxed);
        State pending = State::Pending;
        return _state.compare_exchange_strong(pending, State::ConsumedAccepted,
            std::memory_order_release, std::memory_order_relaxed);
    }
    Observation Observe() const noexcept
    {
        const State state = _state.load(std::memory_order_acquire);
        return {state, state == State::ConsumedAccepted ?
            _rendererModel.load(std::memory_order_relaxed) : InvalidModel};
    }

private:
    const uint32_t _producerModel;
    const uint64_t _ownerEpoch, _sourceAdmissionEpoch, _requestId;
    std::array<uint64_t, MaxMeshes> _expectedMeshes{};
    uint8_t _meshCount = 0;
    bool _valid = false;
    std::atomic<State> _state{State::Pending};
    std::atomic<uint32_t> _rendererModel{InvalidModel};
};
} // namespace Poseidon::render
