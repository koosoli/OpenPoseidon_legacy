#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace Poseidon::GeometryPages
{
// Offline visual-only prototype. No engine pointers, GPU ownership or simulation
// geometry. Eligibility and the authored coarse representation are caller facts.
struct Position { float x, y, z; };
struct MeshInput
{
    // Mandatory caller proof: position[i] is exactly the rendered position in
    // vertex record i, in the same coordinate basis. Opaque bytes are not decoded
    // here; bounds cannot certify a mismatched position table or vertex layout.
    std::span<const Position> positions;
    std::span<const uint8_t> vertices; // Entire original interleaved vertex records.
    uint32_t stride = 0;
    std::span<const uint32_t> indices;
    std::span<const uint32_t> triangleMaterials;
};
struct SourceIdentity
{
    std::array<uint8_t, 32> sourceSha256{};
    uint64_t geometryOptions = 0, materialOptions = 0;
    uint32_t producerVersion = 1;
    // Together with both option hashes, these must identify the exact authored
    // coarse/fine selection, vertex layout and material mapping of both inputs.
    uint32_t coarseRepresentation = 0, fineRepresentation = 0;
    uint32_t vertexLayout = 0, materialMapping = 0;
    bool operator==(const SourceIdentity&) const = default;
};
struct Limits
{
    uint32_t maxVertices = 1'000'000, maxTriangles = 2'000'000;
    uint32_t clusterVertices = 64, clusterTriangles = 124;
    uint32_t pageBytes = 65536, maxPages = 4096, maxClusters = 65536;
    uint64_t payloadBytes = 128ull * 1024 * 1024;
};
enum class Status { Built, Unsupported, InvalidInput, Capacity, AllocationFailed };
struct PackingDescriptor
{
    uint32_t clusterVertices = 0, clusterTriangles = 0, pageBytes = 0;
    bool operator==(const PackingDescriptor&) const = default;
};
struct CacheIdentity
{
    SourceIdentity source;
    PackingDescriptor packing;
    uint32_t formatVersion = 1, algorithmVersion = 2;
    bool operator==(const CacheIdentity&) const = default;
};
struct Cluster
{
    uint32_t page = 0, byteOffset = 0, byteLength = 0;
    uint32_t material = 0, firstTriangle = 0, triangles = 0;
    std::array<float, 3> minimum{}, maximum{};
};
struct Page
{
    // Little-endian control fields: magic/version/clusterCount/byteLength (4*u32), then
    // clusters: material/firstTriangle/triangles/stride/vertexCount/indexCount,
    // conservative min/max (6*f32), original vertex IDs, unchanged vertex bytes,
    // local u32 indices. Vertex bytes retain their ORIGINAL opaque encoding.
    // Uncompressed, disposable derived data format v1.
    std::vector<uint8_t> bytes;
};
struct Package
{
    static constexpr uint32_t FormatVersion = 1;
    SourceIdentity source;
    PackingDescriptor packing;
    std::vector<Page> pages;
    std::vector<Cluster> clusters;
    uint32_t coarsePages = 0, coarseClusters = 0;
    uint64_t payloadBytes = 0; // Actual serialized bytes, not allocator/RSS.
    // Algorithm2 bounds every page to256 clusters, matching resident decoding.
    CacheIdentity Identity() const { return {source, packing, FormatVersion, 2}; }
};
struct ResidentPages
{
    CacheIdentity identity;
    std::span<const uint8_t> pages;
};
enum class Frontier { Unavailable, Coarse, Fine };
// The snapshot belongs to this immutable Build-produced package. Identity checks
// reject different source/options/packing; asynchronous runtime request epochs
// are not implemented by this offline helper.
inline Frontier SelectFrontier(const Package& p, const ResidentPages& resident)
{
    if (!(resident.identity == p.Identity()) || !p.coarsePages ||
        p.coarsePages >= p.pages.size() || resident.pages.size() != p.pages.size())
        return Frontier::Unavailable;
    for (uint32_t i = 0; i < p.coarsePages; ++i)
        if (!resident.pages[i]) return Frontier::Unavailable;
    for (size_t i = p.coarsePages; i < p.pages.size(); ++i)
        if (!resident.pages[i]) return Frontier::Coarse;
    // Atomic whole authored-LOD replacement: never partial children or both LODs.
    return Frontier::Fine;
}

namespace Detail
{
inline void U32(std::vector<uint8_t>& dst, uint32_t value)
{ for (unsigned shift = 0; shift < 32; shift += 8) dst.push_back(uint8_t(value >> shift)); }
inline void Patch(std::vector<uint8_t>& dst, size_t at, uint32_t value)
{ for (unsigned shift = 0; shift < 32; shift += 8) dst[at++] = uint8_t(value >> shift); }
inline bool Valid(const MeshInput& m, const Limits& l)
{
    if (!m.stride || m.stride > 4096 || m.positions.empty() || m.indices.empty() ||
        !m.positions.data() || !m.vertices.data() || !m.indices.data() || !m.triangleMaterials.data() ||
        m.positions.size() > l.maxVertices || m.indices.size() % 3 ||
        m.indices.size() / 3 > l.maxTriangles ||
        m.triangleMaterials.size() != m.indices.size() / 3 ||
        m.positions.size() > std::numeric_limits<size_t>::max() / m.stride ||
        m.vertices.size() != m.positions.size() * m.stride ||
        m.vertices.size() > 128ull * 1024 * 1024) return false;
    for (const auto& p : m.positions)
        for (float v : {p.x, p.y, p.z}) if (!std::isfinite(v)) return false;
    for (uint32_t i : m.indices) if (i >= m.positions.size()) return false;
    return true;
}
inline Status Append(const MeshInput& m, const Limits& l, Package& out)
{
    const size_t firstPage = out.pages.size(); // Coarse and fine never share pages.
    uint32_t pageClusters = 0;
    size_t tri = 0;
    while (tri < m.indices.size() / 3)
    {
        if (out.clusters.size() >= l.maxClusters) return Status::Capacity;
        const uint32_t material = m.triangleMaterials[tri];
        const size_t first = tri;
        std::array<uint32_t, 256> ids{};
        std::vector<uint32_t> local;
        local.reserve(size_t(l.clusterTriangles) * 3);
        uint32_t count = 0;
        while (tri < m.indices.size() / 3 && tri - first < l.clusterTriangles &&
               m.triangleMaterials[tri] == material)
        {
            uint32_t added = 0;
            for (unsigned k = 0; k < 3; ++k)
            {
                const uint32_t id = m.indices[tri * 3 + k];
                bool found = std::find(ids.begin(), ids.begin() + count, id) != ids.begin() + count;
                for (unsigned prev = 0; prev < k; ++prev) found |= m.indices[tri * 3 + prev] == id;
                if (!found) ++added;
            }
            if (count + added > l.clusterVertices) break;
            for (unsigned k = 0; k < 3; ++k)
            {
                const uint32_t id = m.indices[tri * 3 + k];
                auto it = std::find(ids.begin(), ids.begin() + count, id);
                uint32_t index = uint32_t(it - ids.begin());
                if (index == count) ids[count++] = id;
                local.push_back(index);
            }
            ++tri;
        }
        if (tri == first) return Status::Capacity;
        const uint64_t bytes = 48ull + uint64_t(count) * (4ull + m.stride) + local.size() * 4ull;
        if (bytes + 16 > l.pageBytes || bytes > l.payloadBytes - std::min(out.payloadBytes, l.payloadBytes))
            return Status::Capacity;
        if (out.pages.size() == firstPage || pageClusters == 256 ||
            out.pages.back().bytes.size() + bytes > l.pageBytes)
        {
            if (out.pages.size() >= l.maxPages ||
                bytes + 16 > l.payloadBytes - std::min(out.payloadBytes, l.payloadBytes)) return Status::Capacity;
            // Reserve actual first-cluster payload, never pageBytes*maxPages.
            Page page; page.bytes.reserve(size_t(bytes) + 16);
            U32(page.bytes, 0x31504743); U32(page.bytes, Package::FormatVersion);
            U32(page.bytes, 0); U32(page.bytes, 16);
            out.pages.push_back(std::move(page)); out.payloadBytes += 16; pageClusters = 0;
        }
        Cluster cluster;
        cluster.page = uint32_t(out.pages.size() - 1);
        cluster.byteOffset = uint32_t(out.pages.back().bytes.size());
        cluster.byteLength = uint32_t(bytes); cluster.material = material;
        cluster.firstTriangle = uint32_t(first); cluster.triangles = uint32_t(tri - first);
        cluster.minimum.fill(std::numeric_limits<float>::max());
        cluster.maximum.fill(std::numeric_limits<float>::lowest());
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto& pos = m.positions[ids[i]];
            const std::array<float, 3> values{pos.x, pos.y, pos.z};
            for (unsigned axis = 0; axis < 3; ++axis)
            {
                cluster.minimum[axis] = std::min(cluster.minimum[axis], values[axis]);
                cluster.maximum[axis] = std::max(cluster.maximum[axis], values[axis]);
            }
        }
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            cluster.minimum[axis] = std::nextafter(cluster.minimum[axis], -std::numeric_limits<float>::infinity());
            cluster.maximum[axis] = std::nextafter(cluster.maximum[axis], std::numeric_limits<float>::infinity());
            if (!std::isfinite(cluster.minimum[axis]) || !std::isfinite(cluster.maximum[axis]))
                return Status::InvalidInput;
        }
        auto& dst = out.pages.back().bytes;
        U32(dst, material); U32(dst, cluster.firstTriangle); U32(dst, cluster.triangles);
        U32(dst, m.stride); U32(dst, count); U32(dst, uint32_t(local.size()));
        for (float v : cluster.minimum) U32(dst, std::bit_cast<uint32_t>(v));
        for (float v : cluster.maximum) U32(dst, std::bit_cast<uint32_t>(v));
        for (uint32_t i = 0; i < count; ++i) U32(dst, ids[i]);
        for (uint32_t i = 0; i < count; ++i)
        {
            const size_t at = size_t(ids[i]) * m.stride;
            dst.insert(dst.end(), m.vertices.begin() + at, m.vertices.begin() + at + m.stride);
        }
        for (uint32_t index : local) U32(dst, index);
        Patch(dst, 8, ++pageClusters); Patch(dst, 12, uint32_t(dst.size()));
        out.payloadBytes += bytes; out.clusters.push_back(cluster);
    }
    return Status::Built;
}
}

inline Status Build(const MeshInput& coarse, const MeshInput& fine, SourceIdentity source,
                    bool staticOpaquePilot, Package& destination, Limits limits = {})
{
    if (!staticOpaquePilot) return Status::Unsupported;
    if (!source.producerVersion || !source.vertexLayout || !source.materialMapping ||
        source.coarseRepresentation == source.fineRepresentation ||
        std::all_of(source.sourceSha256.begin(), source.sourceSha256.end(),
        [](uint8_t b) { return b == 0; }) || limits.clusterVertices < 3 || limits.clusterVertices > 256 ||
        !limits.clusterTriangles || limits.clusterTriangles > 256 || limits.pageBytes < 64 ||
        limits.pageBytes > 1024 * 1024 || !limits.maxPages || limits.maxPages > 4096 ||
        !limits.maxClusters || limits.maxClusters > 65536 || limits.maxVertices > 1'000'000 ||
        limits.maxTriangles > 2'000'000 || limits.payloadBytes > 128ull * 1024 * 1024 ||
        !Detail::Valid(coarse, limits) || !Detail::Valid(fine, limits)) return Status::InvalidInput;
    // Transactional publication: failed allocations cannot replace fallback.
    try
    {
        Package built; built.source = source;
        built.packing = {limits.clusterVertices, limits.clusterTriangles, limits.pageBytes};
        auto status = Detail::Append(coarse, limits, built);
        if (status != Status::Built) return status;
        built.coarsePages = uint32_t(built.pages.size());
        built.coarseClusters = uint32_t(built.clusters.size());
        status = Detail::Append(fine, limits, built);
        if (status != Status::Built) return status;
        destination = std::move(built); return Status::Built;
    }
    catch (...) { return Status::AllocationFailed; }
}
}
