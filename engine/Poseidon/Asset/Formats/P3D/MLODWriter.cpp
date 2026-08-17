#include <Poseidon/Asset/Formats/P3D/MLODWriter.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace Poseidon::Asset::Formats
{
namespace
{

// The reader's own caps, restated on the write side so a file this writer
// produces cannot be one MLODStructures.hpp refuses. readAsciiZ throws at 1024
// bytes accumulated, so 1023 is the longest string that survives; readHeader
// rejects a LOD count outside 1..100; readP3DMFaceTable rejects a vertex count
// outside 3..4.
constexpr size_t kMaxStringLength = 1023;
constexpr size_t kMaxLodCount = 100;
// #Property# name and value are fixed 64-byte fields, NUL-padded, so the payload
// is always 128 bytes regardless of what the strings actually hold.
constexpr size_t kPropertyFieldSize = 64;
constexpr int32_t kPropertyPayloadSize = 128;

struct ByteSink
{
    std::vector<char> bytes;

    void raw(const void* data, size_t size)
    {
        const char* first = static_cast<const char*>(data);
        bytes.insert(bytes.end(), first, first + size);
    }

    void u8(uint8_t value) { bytes.push_back(static_cast<char>(value)); }

    // Explicit byte-at-a-time little-endian, not a memcpy of the integer: the
    // wire order is a property of the format, not of whatever host this builds on.
    void u32(uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<char>((value >> shift) & 0xFF));
    }

    void i32(int32_t value) { u32(static_cast<uint32_t>(value)); }

    void f32(float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        u32(bits);
    }

    void asciiz(const std::string& text)
    {
        if (text.size() > kMaxStringLength)
            throw std::runtime_error("MLOD writer: string of " + std::to_string(text.size()) +
                                     " bytes exceeds the reader's " + std::to_string(kMaxStringLength) + "-byte limit");
        raw(text.data(), text.size());
        u8(0);
    }

    // A fixed-width NUL-padded field. Used only by #Property#, whose two fields the
    // reader reads as exactly 64 bytes each rather than as terminated strings.
    void fixedField(const std::string& text, size_t width)
    {
        if (text.size() > width)
            throw std::runtime_error("MLOD writer: property field '" + text + "' exceeds " + std::to_string(width) +
                                     " bytes");
        raw(text.data(), text.size());
        bytes.insert(bytes.end(), width - text.size(), '\0');
    }

    void signature(const char (&value)[5]) { raw(value, 4); }
};

// A P3DM TAGG entry: active byte, asciiz name, uint32 payload size. Not SP3X's
// fixed 64-byte name plus size -- readP3DMTAGGSection desynchronises on the first
// tag if the two are swapped.
void writeTagHeader(ByteSink& sink, const std::string& name, int32_t payloadSize)
{
    sink.u8(1);
    sink.asciiz(name);
    sink.i32(payloadSize);
}

int32_t totalFaceVertices(const MLOD::WriteLod& lod)
{
    int32_t total = 0;
    for (const auto& face : lod.faces)
        total += face.vertexCount;
    return total;
}

void validateLod(const MLOD::WriteLod& lod, size_t lodIndex)
{
    const std::string where = "MLOD writer: LOD " + std::to_string(lodIndex) + " ";
    const int32_t nPos = static_cast<int32_t>(lod.points.size());
    const int32_t nNorm = static_cast<int32_t>(lod.normals.size());

    for (size_t faceIndex = 0; faceIndex < lod.faces.size(); ++faceIndex)
    {
        const auto& face = lod.faces[faceIndex];
        if (face.vertexCount < 3 || face.vertexCount > 4)
            throw std::runtime_error(where + "face " + std::to_string(faceIndex) + " has " +
                                     std::to_string(face.vertexCount) +
                                     " vertices (MLOD stores triangles and quads only)");

        for (int32_t corner = 0; corner < face.vertexCount; ++corner)
        {
            const auto& vertex = face.vertices[corner];
            // An out-of-range point index does not fail on load -- convertGeometry
            // bounds-checks it and leaves the vertex at the origin -- so a mesh with
            // a bad index would load as a model with a stray vertex at (0,0,0)
            // instead of an error. Refuse it here, where it is still findable.
            if (vertex.point < 0 || vertex.point >= nPos)
                throw std::runtime_error(where + "face " + std::to_string(faceIndex) + " corner " +
                                         std::to_string(corner) + " references point " + std::to_string(vertex.point) +
                                         " of " + std::to_string(nPos));
            // -1 is the sanctioned "no normal"; any other out-of-range value is a
            // caller mistake that would read as an absent normal.
            if (vertex.normal < -1 || vertex.normal >= nNorm)
                throw std::runtime_error(where + "face " + std::to_string(faceIndex) + " corner " +
                                         std::to_string(corner) + " references normal " +
                                         std::to_string(vertex.normal) + " of " + std::to_string(nNorm));
        }
    }

    const int32_t nFace = static_cast<int32_t>(lod.faces.size());
    for (size_t selIndex = 0; selIndex < lod.selections.size(); ++selIndex)
    {
        const auto& selection = lod.selections[selIndex];
        const std::string who = where + "selection " + std::to_string(selIndex) + " ('" + selection.name + "') ";
        // An empty name is unaddressable; a '#'-prefixed one is a tagg keyword and
        // the reader would dispatch on it (readTAGGSection) instead of keeping it as
        // a selection. Both load without error and neither is what was asked for.
        if (selection.name.empty() || selection.name[0] == '#')
            throw std::runtime_error(who + "has a name the reader would not keep as a selection");
        if (selection.weight == 0)
            throw std::runtime_error(who + "has weight 0, which the reader reads as 'not selected'");
        if (!selection.pointWeights.empty() && selection.pointWeights.size() != selection.points.size())
            throw std::runtime_error(who + "has " + std::to_string(selection.pointWeights.size()) +
                                     " point weights for " + std::to_string(selection.points.size()) + " points");
        for (uint8_t pointWeight : selection.pointWeights)
            if (pointWeight == 0)
                throw std::runtime_error(who + "has a point weight of 0, which the reader reads as 'not selected'");
        for (int32_t point : selection.points)
            if (point < 0 || point >= nPos)
                throw std::runtime_error(who + "references point " + std::to_string(point) + " of " +
                                         std::to_string(nPos));
        for (int32_t face : selection.faces)
            if (face < 0 || face >= nFace)
                throw std::runtime_error(who + "references face " + std::to_string(face) + " of " +
                                         std::to_string(nFace));
    }

    if (!lod.mass.empty() && lod.mass.size() != lod.points.size())
        throw std::runtime_error(where + "has " + std::to_string(lod.mass.size()) + " mass entries for " +
                                 std::to_string(lod.points.size()) +
                                 " points (the reader sizes #Mass# from the point count)");
}

void writeLod(ByteSink& sink, const MLOD::WriteLod& lod, size_t lodIndex)
{
    validateLod(lod, lodIndex);

    const int32_t nPos = static_cast<int32_t>(lod.points.size());
    const int32_t nNorm = static_cast<int32_t>(lod.normals.size());
    const int32_t nFace = static_cast<int32_t>(lod.faces.size());

    sink.signature("P3DM");
    sink.i32(28);  // majorVersion -- the only pair readP3DMHeader accepts
    sink.i32(256); // minorVersion
    sink.i32(nPos);
    sink.i32(nNorm);
    sink.i32(nFace);
    sink.i32(0); // flags

    for (const auto& point : lod.points)
    {
        sink.f32(point.position.x);
        sink.f32(point.position.y);
        sink.f32(point.position.z);
        sink.i32(point.flags);
    }

    for (const auto& normal : lod.normals)
    {
        sink.f32(normal.x);
        sink.f32(normal.y);
        sink.f32(normal.z);
    }

    for (const auto& face : lod.faces)
    {
        sink.i32(face.vertexCount);
        // Four corner slots are always stored, even for a triangle: the reader
        // reads all four and discards the surplus rather than seeking past a
        // computed size, so a three-slot face record desynchronises the stream.
        for (int slot = 0; slot < 4; ++slot)
        {
            const bool used = slot < face.vertexCount;
            const auto& vertex = face.vertices[used ? slot : 0];
            sink.i32(used ? vertex.point : 0);
            sink.i32(used ? vertex.normal : 0);
            sink.f32(used ? vertex.u : 0.0f);
            sink.f32(used ? vertex.v : 0.0f);
        }
        sink.u32(face.flags);
        // Two separate variable-length strings, not one 32-byte field: that is the
        // whole difference between a P3DM face record and an SP3X one, and real
        // RVMAT paths run past 32 bytes routinely.
        sink.asciiz(face.texture);
        sink.asciiz(face.material);
    }

    sink.signature("TAGG");

    // Named selections first, then #Mass#, then #Property# -- the order the O2
    // tooling emits and the fixture generator reproduces. The reader dispatches on
    // the tag name, so any order loads; this one keeps a diff against an authored
    // file readable.
    for (const auto& selection : lod.selections)
    {
        // One byte per point, then one per face. Membership is by index, so a
        // selection listing the same point twice still writes one byte.
        std::vector<uint8_t> payload(static_cast<size_t>(nPos) + static_cast<size_t>(nFace), 0);
        for (size_t i = 0; i < selection.points.size(); ++i)
            payload[static_cast<size_t>(selection.points[i])] =
                selection.pointWeights.empty() ? selection.weight : selection.pointWeights[i];
        for (int32_t face : selection.faces)
            payload[static_cast<size_t>(nPos) + static_cast<size_t>(face)] = 1;
        writeTagHeader(sink, selection.name, static_cast<int32_t>(payload.size()));
        sink.raw(payload.data(), payload.size());
    }

    if (!lod.mass.empty())
    {
        writeTagHeader(sink, "#Mass#", 4 * nPos);
        for (float mass : lod.mass)
            sink.f32(mass);
    }

    for (const auto& property : lod.properties)
    {
        writeTagHeader(sink, "#Property#", kPropertyPayloadSize);
        sink.fixedField(property.name, kPropertyFieldSize);
        sink.fixedField(property.value, kPropertyFieldSize);
    }

    // A #UVSet# payload is `uint32 id` then one (u,v) pair per ACTUAL face corner --
    // face.n entries, not the four slots the face record reserves. The reader
    // checks the size against exactly that and skips any block that disagrees, so
    // a block sized off the four slots would be written, accepted by the container,
    // and then silently dropped.
    const int32_t faceVertexCount = totalFaceVertices(lod);
    if (lod.secondUVChannel && faceVertexCount > 0)
    {
        const int32_t payloadSize = 4 + 8 * faceVertexCount;
        for (int32_t setId = 0; setId < 2; ++setId)
        {
            writeTagHeader(sink, "#UVSet#", payloadSize);
            sink.i32(setId);
            for (const auto& face : lod.faces)
            {
                for (int32_t corner = 0; corner < face.vertexCount; ++corner)
                {
                    const auto& vertex = face.vertices[corner];
                    sink.f32(setId == 0 ? vertex.u : vertex.u1);
                    sink.f32(setId == 0 ? vertex.v : vertex.v1);
                }
            }
        }
    }

    writeTagHeader(sink, "#EndOfFile#", 0);
    sink.f32(lod.resolution);
}

} // namespace

std::vector<char> MLODWriter::writeToBuffer(const MLOD::WriteModel& model)
{
    if (model.lods.empty() || model.lods.size() > kMaxLodCount)
        throw std::runtime_error("MLOD writer: " + std::to_string(model.lods.size()) + " LODs (readHeader accepts 1-" +
                                 std::to_string(kMaxLodCount) + ")");

    ByteSink sink;
    sink.signature("MLOD");
    sink.u8(1); // versionMajor
    sink.u8(1); // versionMinor -- MLOD 1.1, what every Real Virtuality model carries
    sink.u8(0);
    sink.u8(0); // padding
    sink.u32(static_cast<uint32_t>(model.lods.size()));

    for (size_t lodIndex = 0; lodIndex < model.lods.size(); ++lodIndex)
        writeLod(sink, model.lods[lodIndex], lodIndex);

    return std::move(sink.bytes);
}

void MLODWriter::write(const MLOD::WriteModel& model, const std::string& filePath)
{
    // Serialise first, then open: a validation failure must not leave a truncated
    // file behind where a previous good one was.
    const std::vector<char> bytes = writeToBuffer(model);

    std::ofstream file(filePath, std::ios::binary | std::ios::trunc);
    if (!file)
        throw std::runtime_error("MLOD writer: failed to open " + filePath + " for writing");
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!file)
        throw std::runtime_error("MLOD writer: failed to write " + filePath);
}

MLOD::WriteModel MLODWriter::describe(const Poseidon::Model::Model& model)
{
    MLOD::WriteModel out;
    out.lods.reserve(model.lodLevels.size());

    for (const auto& lodLevel : model.lodLevels)
    {
        const auto& mesh = lodLevel.mesh;
        MLOD::WriteLod lod;
        lod.resolution = lodLevel.resolution;

        lod.points.reserve(mesh.vertices.size());
        lod.normals.reserve(mesh.vertices.size());
        bool anySecondUV = false;
        for (const auto& vertex : mesh.vertices)
        {
            MLOD::WritePoint point;
            point.position = Vector3{vertex.position.x, vertex.position.y, vertex.position.z};
            // Deliberately 0, not vertex.flags. What the IR holds is ClipFlags, the
            // output of convertPointFlagsToClipFlags; the POINT_* bits that produced
            // it are not recoverable from it (the mapping is many-to-one and zeroes
            // any input carrying bits outside its table), so writing the ClipFlags
            // back would encode a different point than the source had.
            point.flags = 0;
            lod.points.push_back(point);
            lod.normals.push_back(Vector3{vertex.normal.x, vertex.normal.y, vertex.normal.z});
            anySecondUV = anySecondUV || vertex.uv1.u != 0.0f || vertex.uv1.v != 0.0f;
        }
        lod.secondUVChannel = anySecondUV;

        // Source face order, rebuilt. The loader hands back two vectors and the
        // original interleaving only survives in originalIndex; writing triangles
        // then quads would renumber every face, which moves the per-face UV blocks
        // and the material assignment with it.
        struct FaceRef
        {
            uint32_t originalIndex;
            bool isQuad;
            size_t index;
        };
        std::vector<FaceRef> order;
        order.reserve(mesh.triangles.size() + mesh.quads.size());
        for (size_t i = 0; i < mesh.triangles.size(); ++i)
            order.push_back({mesh.triangles[i].originalIndex, false, i});
        for (size_t i = 0; i < mesh.quads.size(); ++i)
            order.push_back({mesh.quads[i].originalIndex, true, i});
        std::stable_sort(order.begin(), order.end(),
                         [](const FaceRef& a, const FaceRef& b) { return a.originalIndex < b.originalIndex; });

        auto materialOf = [&mesh](uint32_t materialIndex, std::string& texture, std::string& material)
        {
            if (materialIndex < mesh.materials.size())
            {
                texture = mesh.materials[materialIndex].texturePath;
                material = mesh.materials[materialIndex].materialPath;
            }
        };

        lod.faces.reserve(order.size());
        for (const auto& ref : order)
        {
            MLOD::WriteFace face;
            uint32_t indices[4] = {0, 0, 0, 0};
            uint32_t materialIndex = 0;

            if (ref.isQuad)
            {
                const auto& quad = mesh.quads[ref.index];
                face.vertexCount = 4;
                face.flags = static_cast<uint32_t>(quad.flags);
                materialIndex = quad.materialIndex;
                for (int i = 0; i < 4; ++i)
                    indices[i] = quad.indices[i];
                // Undo the loader's quad reversal (0<->1, 2<->3).
                std::swap(indices[0], indices[1]);
                std::swap(indices[2], indices[3]);
            }
            else
            {
                const auto& tri = mesh.triangles[ref.index];
                face.vertexCount = 3;
                face.flags = static_cast<uint32_t>(tri.flags);
                materialIndex = tri.materialIndex;
                for (int i = 0; i < 3; ++i)
                    indices[i] = tri.indices[i];
                // Undo the loader's triangle reversal (0<->1).
                std::swap(indices[0], indices[1]);
            }

            materialOf(materialIndex, face.texture, face.material);

            for (int32_t corner = 0; corner < face.vertexCount; ++corner)
            {
                const uint32_t vertexIndex = indices[corner];
                if (vertexIndex >= mesh.vertices.size())
                    throw std::runtime_error("MLOD writer: face references vertex " + std::to_string(vertexIndex) +
                                             " of " + std::to_string(mesh.vertices.size()));
                const auto& vertex = mesh.vertices[vertexIndex];
                face.vertices[corner].point = static_cast<int32_t>(vertexIndex);
                face.vertices[corner].normal = static_cast<int32_t>(vertexIndex);
                face.vertices[corner].u = vertex.uv.u;
                face.vertices[corner].v = vertex.uv.v;
                face.vertices[corner].u1 = vertex.uv1.u;
                face.vertices[corner].v1 = vertex.uv1.v;
            }

            lod.faces.push_back(std::move(face));
        }

        lod.properties.reserve(mesh.properties.size());
        for (const auto& property : mesh.properties)
            lod.properties.push_back({property.name, property.value});

        // Selections and #Mass# (COL-001). One vertex is one point, so the vertex
        // lists are the point lists; the face lists are source face indices, which
        // are positions in the re-interleaved order built above -- look them up
        // rather than assuming originalIndex is dense.
        std::vector<int32_t> positionOfOriginal;
        for (size_t position = 0; position < order.size(); ++position)
        {
            const uint32_t original = order[position].originalIndex;
            if (original >= positionOfOriginal.size())
                positionOfOriginal.resize(static_cast<size_t>(original) + 1, -1);
            positionOfOriginal[original] = static_cast<int32_t>(position);
        }
        for (const auto& selection : mesh.selections)
        {
            MLOD::WriteNamedSelection written;
            written.name = selection.name;
            const bool hasSourceWeights = selection.sourceVertexWeights.size() == selection.vertexIndices.size();
            for (size_t i = 0; i < selection.vertexIndices.size(); ++i)
            {
                const uint32_t vertexIndex = selection.vertexIndices[i];
                if (vertexIndex >= mesh.vertices.size())
                    continue;
                written.points.push_back(static_cast<int32_t>(vertexIndex));
                // A source byte of 0 cannot have produced membership; treat it as 1.
                const uint8_t sourceWeight = hasSourceWeights ? selection.sourceVertexWeights[i] : 1;
                written.pointWeights.push_back(sourceWeight == 0 ? 1 : sourceWeight);
            }
            for (uint32_t sourceFace : selection.triangleIndices)
                if (sourceFace < positionOfOriginal.size() && positionOfOriginal[sourceFace] >= 0)
                    written.faces.push_back(positionOfOriginal[sourceFace]);
            if (!written.name.empty() && written.name[0] != '#')
                lod.selections.push_back(std::move(written));
        }
        if (mesh.vertexMass.size() == mesh.vertices.size())
            lod.mass = mesh.vertexMass;

        out.lods.push_back(std::move(lod));
    }

    return out;
}

} // namespace Poseidon::Asset::Formats
