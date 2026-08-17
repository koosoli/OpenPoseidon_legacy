#include <Poseidon/Asset/Formats/Enfusion/XobCollision.hpp>

#include <cctype>
#include <cstring>
#include <utility>
#include <vector>

namespace Poseidon::Asset::Formats::Enfusion
{
namespace
{

//! Bounds-checked little-endian cursor over the COLL payload. Every read that would
//! run past the end sets `overrun` and returns zero; the walker checks the flag once
//! per record, so a truncated record is reported at the record and not as a crash.
struct CollCursor
{
    const uint8_t* base;
    size_t size;
    size_t at = 0;
    bool overrun = false;

    bool Has(size_t bytes) const { return at + bytes <= size; }

    uint8_t U8()
    {
        if (!Has(1))
        {
            overrun = true;
            return 0;
        }
        return base[at++];
    }
    uint16_t U16()
    {
        if (!Has(2))
        {
            overrun = true;
            return 0;
        }
        const uint16_t v = ReadLe16(base, at);
        at += 2;
        return v;
    }
    uint32_t U32()
    {
        if (!Has(4))
        {
            overrun = true;
            return 0;
        }
        const uint32_t v = ReadLe32(base, at);
        at += 4;
        return v;
    }
    float F32()
    {
        if (!Has(4))
        {
            overrun = true;
            return 0.0f;
        }
        const float v = ReadLeF32(base, at);
        at += 4;
        return v;
    }
    XobVec3 Vec3()
    {
        XobVec3 v;
        v.x = F32();
        v.y = F32();
        v.z = F32();
        return v;
    }
};

std::string StripGuid(std::string reference)
{
    if (!reference.empty() && reference.front() == '{')
    {
        const size_t close = reference.find('}');
        if (close != std::string::npos)
            reference = reference.substr(close + 1);
    }
    return reference;
}

std::string StringAt(const XobHeader& header, size_t ordinal)
{
    return ordinal < header.strings.size() ? header.strings[ordinal] : std::string();
}

XobCollisionKind KindOf(uint8_t typeCode)
{
    switch (typeCode)
    {
        case 1:
            return XobCollisionKind::Sphere;
        case 2:
            return XobCollisionKind::Capsule;
        case 3:
            return XobCollisionKind::Box;
        case 4:
            return XobCollisionKind::ConvexHull;
        case 5:
            return XobCollisionKind::TriMesh;
        case 6:
            return XobCollisionKind::TriMeshGrouped;
        case 7:
            return XobCollisionKind::Cylinder;
        default:
            return XobCollisionKind::Unknown;
    }
}

bool StartsWithNoCase(const std::string& text, const char* prefix)
{
    size_t i = 0;
    for (; prefix[i] != '\0'; ++i)
    {
        if (i >= text.size())
            return false;
        if (std::tolower(static_cast<unsigned char>(text[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    }
    return true;
}

} // namespace

const char* ToString(XobCollisionKind kind)
{
    switch (kind)
    {
        case XobCollisionKind::Sphere:
            return "sphere";
        case XobCollisionKind::Capsule:
            return "capsule";
        case XobCollisionKind::Box:
            return "box";
        case XobCollisionKind::ConvexHull:
            return "hull";
        case XobCollisionKind::TriMesh:
            return "mesh";
        case XobCollisionKind::TriMeshGrouped:
            return "mesh+groups";
        case XobCollisionKind::Cylinder:
            return "cylinder";
        default:
            return "unknown";
    }
}

XobVec3 XobCollisionShape::ToModel(const XobVec3& local) const
{
    // Row-vector convention: model = local * M + position, rows are the local axes.
    XobVec3 out;
    out.x = local.x * rotation[0] + local.y * rotation[3] + local.z * rotation[6] + position.x;
    out.y = local.x * rotation[1] + local.y * rotation[4] + local.z * rotation[7] + position.y;
    out.z = local.x * rotation[2] + local.y * rotation[5] + local.z * rotation[8] + position.z;
    return out;
}

XobCollision ReadXobCollision(const void* data, size_t size, const XobHeader& header)
{
    XobCollision out;
    const IffFile iff = ReadIff(data, size);
    if (!iff.valid())
    {
        out.error = iff.error;
        return out;
    }
    const IffChunk* coll = iff.Find(FourCC("COLL"));
    if (!coll)
        return out; // present == false, no error: most decals and grass have none
    out.present = true;

    CollCursor cursor{static_cast<const uint8_t*>(data) + coll->offset, coll->size};

    while (cursor.at < cursor.size)
    {
        const size_t recordStart = cursor.at;
        XobCollisionShape shape;
        shape.typeCode = cursor.U8();
        shape.kind = KindOf(shape.typeCode);
        cursor.U8(); // 0xFF on every record measured; not interpreted
        shape.layer = StringAt(header, cursor.U16());
        for (float& m : shape.rotation)
            m = cursor.F32();
        shape.position = cursor.Vec3();
        shape.extra = cursor.F32();
        shape.name = StringAt(header, cursor.U16());
        shape.material = StripGuid(StringAt(header, cursor.U16()));
        cursor.U32(); // zero on every record measured

        switch (shape.kind)
        {
            case XobCollisionKind::Sphere:
                shape.radius = cursor.F32();
                break;
            case XobCollisionKind::Capsule:
            case XobCollisionKind::Cylinder:
                shape.radius = cursor.F32();
                shape.halfHeight = cursor.F32();
                break;
            case XobCollisionKind::Box:
                shape.halfExtents = cursor.Vec3();
                break;
            case XobCollisionKind::ConvexHull:
            {
                const uint16_t nVerts = cursor.U16();
                const uint16_t nFaces = cursor.U16();
                const uint16_t nEdges = cursor.U16();
                const uint16_t nIndices = cursor.U16();
                // Size the whole record before reading any of it, so a wrong count
                // fails as "truncated record" and not as a multi-gigabyte allocation.
                if (!cursor.Has(12ull * nVerts + 8ull * nEdges + 2ull * nIndices + 4ull * nFaces))
                {
                    cursor.overrun = true;
                    break;
                }
                shape.vertices.resize(nVerts);
                for (XobVec3& v : shape.vertices)
                    v = cursor.Vec3();
                shape.edges.resize(4ull * nEdges);
                for (uint32_t& e : shape.edges)
                    e = cursor.U16();
                // The index list is EDGE indices -- each face is a run of edges, and
                // every edge appears in exactly two faces' runs. Measured, not assumed:
                // on the first hull read as vertex indices the maximum was nEdges - 1
                // (101 of 36 vertices), and every face's edges chain into a closed
                // ring 68 / 68 times. The vertex rings the consumer wants are rebuilt
                // from the edges below.
                std::vector<uint32_t> faceEdges(nIndices);
                for (uint32_t& i : faceEdges)
                    i = cursor.U16();
                std::vector<std::pair<uint32_t, uint32_t>> edgeRuns(nFaces);
                for (auto& run : edgeRuns)
                {
                    run.first = cursor.U16();
                    run.second = cursor.U16();
                }
                for (uint32_t e = 0; e < nEdges; ++e)
                    if (shape.edges[4ull * e] >= nVerts || shape.edges[4ull * e + 1] >= nVerts)
                    {
                        out.error = "hull '" + shape.name + "' at " + std::to_string(recordStart) + " edge " +
                                    std::to_string(e) + " indexes a vertex past " + std::to_string(nVerts);
                        return out;
                    }
                for (uint32_t i : faceEdges)
                    if (i >= nEdges)
                    {
                        out.error = "hull '" + shape.name + "' at " + std::to_string(recordStart) + " indexes edge " +
                                    std::to_string(i) + " of " + std::to_string(nEdges);
                        return out;
                    }
                shape.polygons.reserve(nFaces);
                std::vector<bool> used;
                for (size_t f = 0; f < edgeRuns.size(); ++f)
                {
                    const auto& run = edgeRuns[f];
                    if (static_cast<uint64_t>(run.first) + run.second > nIndices || run.second < 3)
                    {
                        out.error = "hull '" + shape.name + "' at " + std::to_string(recordStart) + " face " +
                                    std::to_string(f) + " has a bad edge run";
                        return out;
                    }
                    // Chain the face's edges into a vertex ring: start on the first
                    // edge, then repeatedly take the unused edge that continues from
                    // the current vertex. The winding this yields is arbitrary; the
                    // geometry builder re-orients every face anyway.
                    const uint32_t n = run.second;
                    auto edgeV = [&](uint32_t k, int end)
                    { return shape.edges[4ull * faceEdges[run.first + k] + static_cast<uint32_t>(end)]; };
                    used.assign(n, false);
                    const uint32_t start = shape.indices.size();
                    shape.indices.push_back(edgeV(0, 0));
                    uint32_t current = edgeV(0, 1);
                    shape.indices.push_back(current);
                    used[0] = true;
                    bool chained = true;
                    for (uint32_t step = 2; step < n && chained; ++step)
                    {
                        chained = false;
                        for (uint32_t k = 1; k < n; ++k)
                        {
                            if (used[k])
                                continue;
                            const uint32_t v0 = edgeV(k, 0), v1 = edgeV(k, 1);
                            if (v0 != current && v1 != current)
                                continue;
                            current = v0 == current ? v1 : v0;
                            shape.indices.push_back(current);
                            used[k] = true;
                            chained = true;
                            break;
                        }
                    }
                    if (!chained)
                    {
                        out.error = "hull '" + shape.name + "' at " + std::to_string(recordStart) + " face " +
                                    std::to_string(f) + " edges do not chain into a ring";
                        return out;
                    }
                    shape.polygons.emplace_back(start, n);
                }
                break;
            }
            case XobCollisionKind::TriMesh:
            case XobCollisionKind::TriMeshGrouped:
            {
                const uint16_t nVerts = cursor.U16();
                const uint16_t nTris = cursor.U16();
                if (shape.kind == XobCollisionKind::TriMeshGrouped)
                {
                    const uint32_t nGroups = cursor.U32();
                    if (!cursor.Has(4ull * nGroups))
                    {
                        cursor.overrun = true;
                        break;
                    }
                    shape.materialGroups.resize(nGroups);
                    for (auto& group : shape.materialGroups)
                    {
                        group.first = cursor.U16();
                        group.second = cursor.U16();
                    }
                }
                if (!cursor.Has(12ull * nVerts + 6ull * nTris))
                {
                    cursor.overrun = true;
                    break;
                }
                shape.vertices.resize(nVerts);
                for (XobVec3& v : shape.vertices)
                    v = cursor.Vec3();
                shape.indices.resize(3ull * nTris);
                for (uint32_t& i : shape.indices)
                    i = cursor.U16();
                shape.polygons.resize(nTris);
                for (uint32_t t = 0; t < nTris; ++t)
                    shape.polygons[t] = {3u * t, 3u};
                for (uint32_t i : shape.indices)
                    if (i >= nVerts)
                    {
                        out.error = "mesh '" + shape.name + "' at " + std::to_string(recordStart) + " indexes vertex " +
                                    std::to_string(i) + " of " + std::to_string(nVerts);
                        return out;
                    }
                break;
            }
            default:
                out.error = "unknown record type " + std::to_string(shape.typeCode) + " at " +
                            std::to_string(recordStart) + " of " + std::to_string(cursor.size);
                return out;
        }

        if (cursor.overrun)
        {
            out.error = ToString(shape.kind) + std::string(" '") + shape.name + "' at " + std::to_string(recordStart) +
                        " runs past the end of COLL (" + std::to_string(cursor.size) + " bytes)";
            return out;
        }
        out.shapes.push_back(std::move(shape));
    }

    out.closes = cursor.at == cursor.size;
    return out;
}

bool XobLayerBlocksCharacters(const std::string& layer)
{
    static const char* const kBlocking[] = {"building", "prop",    "tree", "rock",    "door",  "ladder",
                                            "debris",   "terrain", "item", "vehicle", "weapon"};
    // "CharNoCollide", "CharSpecialCollisionNoCollide" -- and any future
    // "<Blocking>NoCollide" -- are explicit opt-outs whatever the prefix says.
    std::string lower;
    lower.reserve(layer.size());
    for (char c : layer)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (lower.find("nocollide") != std::string::npos)
        return false;
    for (const char* prefix : kBlocking)
        if (StartsWithNoCase(lower, prefix))
            return true;
    return false;
}

} // namespace Poseidon::Asset::Formats::Enfusion
