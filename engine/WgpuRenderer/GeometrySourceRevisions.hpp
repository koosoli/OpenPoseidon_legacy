#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Poseidon::render {

// Opt-in, drain-owned identity for immutable registered LOD/section/material
// payloads and their live mesh dependencies. A retired producer can be reused
// only with a new revision; a duplicate live registration poisons the old one.
class GeometrySourceRevisions final {
public:
    explicit GeometrySourceRevisions(uint64_t incarnation, uint64_t firstRevision=1)
        : _incarnation(incarnation), _nextRevision(firstRevision) {}

    uint64_t Incarnation() const { return _incarnation; }

    static uint32_t TakeModelHandle(uint32_t& next)
    {
        return next == UINT32_MAX ? UINT32_MAX : next++;
    }
    static uint64_t TakeMeshHandle(uint64_t& next)
    {
        return next == UINT64_MAX || next == 0 ? 0 : next++;
    }

    uint64_t Register(uint32_t producer, uint32_t renderer, std::vector<uint64_t> meshes,
        bool meshesLive)
    {
        if (_records.find(producer) != _records.end()) { Duplicate(producer); return 0; }
        if (producer == UINT32_MAX || renderer == UINT32_MAX) return 0;
        const uint64_t revision = NextRevision();
        if (!revision) return 0;
        const bool valid = meshesLive && !meshes.empty();
        try { _records.emplace(producer, Record{revision, renderer, valid, std::move(meshes)}); }
        catch (const std::bad_alloc&) { _records.clear(); return 0; }
        return valid ? revision : 0;
    }

    void Duplicate(uint32_t producer)
    {
        if (auto it = _records.find(producer); it != _records.end())
        { it->second.revision = NextRevision(); it->second.valid = false; }
    }

    void Retire(uint32_t producer) { _records.erase(producer); }

    void MeshChanged(uint64_t mesh)
    {
        for (auto& entry : _records)
            if (std::find(entry.second.meshes.begin(), entry.second.meshes.end(), mesh) !=
                entry.second.meshes.end())
            { entry.second.revision = NextRevision(); entry.second.valid = false; }
    }

    uint64_t Revision(uint32_t producer, uint32_t renderer) const
    {
        const auto it = _records.find(producer);
        return it != _records.end() && it->second.valid &&
            it->second.renderer == renderer ? it->second.revision : 0;
    }

    bool Fresh(uint32_t producer, uint32_t renderer, uint64_t incarnation,
        uint64_t revision) const
    {
        return revision && incarnation && incarnation == _incarnation &&
            Revision(producer, renderer) == revision;
    }

private:
    struct Record {
        uint64_t revision;
        uint32_t renderer;
        bool valid;
        std::vector<uint64_t> meshes;
    };
    uint64_t NextRevision()
    {
        // Never issue zero or wrap to a previously issued revision.
        return !_incarnation || !_nextRevision || _nextRevision == UINT64_MAX ?
            0 : _nextRevision++;
    }
    uint64_t _incarnation;
    uint64_t _nextRevision;
    std::unordered_map<uint32_t, Record> _records;
};

} // namespace Poseidon::render
