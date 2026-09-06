#include <Poseidon/Asset/Formats/Enfusion/XobModel.hpp>

#include <Poseidon/Foundation/Algorithms/Lz4Block.hpp>

#include <algorithm>
#include <cstring>
#include <utility>

namespace Poseidon::Asset::Formats::Enfusion
{

namespace
{

//! A bounds-checked forward cursor. Every read is checked because a `.xob` arrives
//! inside a downloaded addon: a declared count is attacker-controlled and the
//! classic failure of a count-driven reader is to size a vector from a field it
//! never validated against the buffer it is reading from.
class Cursor
{
  public:
    Cursor(const uint8_t* base, size_t begin, size_t end) : _base(base), _at(begin), _end(end) {}

    bool ok() const { return _ok; }
    size_t At() const { return _at; }
    size_t Remaining() const { return _at <= _end ? _end - _at : 0; }

    bool Skip(uint64_t bytes)
    {
        if (!_ok || bytes > Remaining())
            return MarkFailed();
        _at += static_cast<size_t>(bytes);
        return true;
    }

    uint8_t U8()
    {
        if (!_ok || Remaining() < 1)
            return MarkFailed(), 0;
        return _base[_at++];
    }

    uint16_t U16()
    {
        if (!_ok || Remaining() < 2)
            return MarkFailed(), 0;
        const uint16_t value = ReadLe16(_base + _at);
        _at += 2;
        return value;
    }

    uint32_t U32()
    {
        if (!_ok || Remaining() < 4)
            return MarkFailed(), 0;
        const uint32_t value = ReadLe32(_base + _at);
        _at += 4;
        return value;
    }

    float F32()
    {
        if (!_ok || Remaining() < 4)
            return MarkFailed(), 0.0f;
        const float value = ReadLeF32(_base + _at);
        _at += 4;
        return value;
    }

    XobVec3 Vec3()
    {
        XobVec3 out;
        out.x = F32();
        out.y = F32();
        out.z = F32();
        return out;
    }

    //! Copies `count` bytes out; the pointer form is deliberately not exposed so no
    //! caller can hold a span past a failed read.
    bool Bytes(void* out, uint64_t count)
    {
        if (!_ok || count > Remaining())
            return MarkFailed();
        std::memcpy(out, _base + _at, static_cast<size_t>(count));
        _at += static_cast<size_t>(count);
        return true;
    }

  private:
    bool MarkFailed()
    {
        _ok = false;
        return false;
    }

    const uint8_t* _base;
    size_t _at;
    size_t _end;
    bool _ok = true;
};

//! "{GUID}path" -- the engine has no GUID registry, and the path is the half that
//! resolves. Same treatment TerrainDescriptor gives its surface materials.
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

constexpr uint64_t kBoneRecordSize = 36;
constexpr uint64_t kSkinBoneBoxSize = 28;
constexpr uint64_t kQuadFixedSize = 56;

} // namespace

XobHeader ReadXobHeader(const void* data, size_t size)
{
    XobHeader out;
    const IffFile iff = ReadIff(data, size);
    if (!iff.valid())
    {
        out.error = iff.error;
        return out;
    }
    if (iff.formType != FourCC("XOB9"))
    {
        out.error = "form type is '" + iff.formType.ToString() + "', not 'XOB9'";
        return out;
    }
    const IffChunk* head = iff.Find(FourCC("HEAD"));
    if (!head)
    {
        out.error = "no HEAD chunk";
        return out;
    }

    const auto* base = static_cast<const uint8_t*>(data);
    const size_t headEnd = head->offset + head->size;
    Cursor cursor(base, head->offset, headEnd);

    out.version = cursor.U32();
    out.bboxMin = cursor.Vec3();
    out.bboxMax = cursor.Vec3();
    out.sphereCenter = cursor.Vec3();
    out.sphereRadius = cursor.F32();
    const uint16_t materialCount = cursor.U16();
    out.boneCount = cursor.U16();
    const uint8_t lodCount = cursor.U8();
    const uint8_t pointCount = cursor.U8();
    cursor.U16(); // pad, zero on the whole corpus
    out.quadCount = cursor.U32();
    const uint32_t stringTableSize = cursor.U32();

    if (!cursor.ok())
    {
        out.error = "HEAD is shorter than its fixed prefix";
        return out;
    }
    if (stringTableSize > cursor.Remaining())
    {
        out.error = "string table of " + std::to_string(stringTableSize) + " bytes overruns HEAD";
        return out;
    }
    {
        // NUL-separated, with a terminating NUL on the last entry, so the split
        // leaves a trailing empty piece that is not a string.
        const uint8_t* table = base + cursor.At();
        size_t start = 0;
        for (size_t i = 0; i < stringTableSize; ++i)
        {
            if (table[i] == 0)
            {
                out.strings.emplace_back(reinterpret_cast<const char*>(table + start), i - start);
                start = i + 1;
            }
        }
        cursor.Skip(stringTableSize);
    }

    // Two of the 15,809 corpus files declare no LODs and stop HEAD right here, with
    // no material array at all. Treating that as truncation would refuse a file the
    // format allows.
    if (cursor.At() >= headEnd)
    {
        out.headCloses = (cursor.At() == headEnd);
        out.lodsTileChunk = out.lods.empty();
        out.sizesClose = true;
        if (!out.headCloses)
            out.error = "HEAD walk ended at " + std::to_string(cursor.At()) + " of " + std::to_string(headEnd);
        return out;
    }

    out.materials.resize(materialCount);
    for (XobMaterial& material : out.materials)
    {
        const uint16_t nameOrdinal = cursor.U16();
        const uint16_t pathOrdinal = cursor.U16();
        if (nameOrdinal < out.strings.size())
            material.name = out.strings[nameOrdinal];
        if (pathOrdinal < out.strings.size())
            material.path = StripGuid(out.strings[pathOrdinal]);
    }

    out.points.resize(pointCount);
    for (XobVec3& point : out.points)
        point = cursor.Vec3();

    // RFG-064: the bone array is DECODED now, not stepped over. It used to be
    // skipped on the grounds that only render geometry was wanted; that is what
    // left Everon's houses with holes where their doors and windows belong,
    // because Enfusion attaches those as separate prefabs bound to these bones.
    //
    // The layout was read off the corpus, not guessed: on FarmHouse_E_1L01.xob the
    // name ordinals run 27..52 against a 26-bone header and name `Scene_Root` plus
    // 25 sockets; the trailing pair is 0xffff / first-child on the root and
    // next-sibling / 0xffff on the rest, which is what identified it as hierarchy
    // links rather than a transform tail. The record stays 36 bytes either way, so
    // a misread here cannot shift the offsets that follow.
    if (kBoneRecordSize * out.boneCount > cursor.Remaining())
    {
        out.error = "bone array of " + std::to_string(out.boneCount) + " records overruns HEAD";
        return out;
    }
    out.bones.resize(out.boneCount);
    for (XobBone& bone : out.bones)
    {
        const uint16_t nameOrdinal = cursor.U16();
        bone.parent = cursor.U16();
        if (nameOrdinal < out.strings.size())
            bone.name = out.strings[nameOrdinal];
        bone.translation = cursor.Vec3();
        bone.rotation[0] = cursor.F32();
        bone.rotation[1] = cursor.F32();
        bone.rotation[2] = cursor.F32();
        bone.rotation[3] = cursor.F32();
        cursor.U16(); // next sibling, 0xffff for none
        cursor.U16(); // first child, 0xffff for none
    }
    if (!cursor.ok())
    {
        out.error = "bone array overruns HEAD";
        return out;
    }

    out.lods.resize(lodCount);
    for (XobLodDesc& lod : out.lods)
    {
        uint8_t tag[4] = {0, 0, 0, 0};
        cursor.Bytes(tag, 4);
        lod.codec = FourCC(static_cast<uint32_t>(tag[0]) << 24 | static_cast<uint32_t>(tag[1]) << 16 |
                           static_cast<uint32_t>(tag[2]) << 8 | static_cast<uint32_t>(tag[3]));
        lod.partCount = cursor.U32();
        cursor.U32(); // reserved, zero on the whole corpus
        lod.threshold = cursor.F32();
        lod.skinBoneCount = cursor.U32();
        lod.dataOffset = cursor.U32();
        lod.dataSize = cursor.U32();
        lod.dataSizeRaw = cursor.U32();
        if (!cursor.ok())
        {
            out.error = "LOD descriptor overruns HEAD";
            return out;
        }
        // A part is 64 bytes minimum, so a partCount that cannot fit in what is left
        // of HEAD is a wrong field width rather than a big model.
        if (static_cast<uint64_t>(lod.partCount) * 64ull > cursor.Remaining())
        {
            out.error = "LOD declares " + std::to_string(lod.partCount) + " parts, more than HEAD can hold";
            return out;
        }

        lod.parts.resize(lod.partCount);
        for (XobPartDesc& part : lod.parts)
        {
            part.flags0 = cursor.U16();
            part.flags1 = cursor.U8();
            part.vertexFormat = cursor.U8();
            part.bboxMin = cursor.Vec3();
            part.bboxMax = cursor.Vec3();
            part.planeNormal = cursor.Vec3();
            part.planeDistance = cursor.F32();
            const uint16_t triangleCountLow = cursor.U16();
            part.vertexCount = cursor.U16();
            part.renderVertexCount = cursor.U16();
            part.skinEntryCount = cursor.U32();
            cursor.U16(); // 0xffff on 93598 / 93598 parts
            part.materialIndex = cursor.U16();
            part.uvSetCount = cursor.U8();
            const uint8_t boneCount = cursor.U8();
            part.flags3 = cursor.U32();
            // The triangle count is 32-bit: low half in the u16 above, high half in
            // the top 16 bits of flags3. Measured on the 3 corpus parts that exceed
            // 65535 triangles -- each was short by exactly 12 * 65536 bytes until
            // this was applied.
            part.triangleCount = static_cast<uint32_t>(triangleCountLow) | ((part.flags3 >> 16) << 16);

            part.uvSets.resize(part.uvSetCount);
            for (XobUvSet& set : part.uvSets)
            {
                set.uMin = cursor.F32();
                set.uMax = cursor.F32();
                set.vMin = cursor.F32();
                set.vMax = cursor.F32();
                set.extra = cursor.F32();
            }
            part.boneIds.resize(boneCount);
            if (boneCount != 0)
                cursor.Bytes(part.boneIds.data(), boneCount);
            if (!cursor.ok())
            {
                out.error = "part descriptor overruns HEAD";
                return out;
            }
        }
        cursor.Skip(kSkinBoneBoxSize * lod.skinBoneCount);
        if (!cursor.ok())
        {
            out.error = "LOD skin-bone bounds overrun HEAD";
            return out;
        }
    }

    for (uint32_t i = 0; i < out.quadCount; ++i)
    {
        // f32[3] min, f32[3] max, 28 zero bytes, u8, u8 n, u16, then f32[3] * n.
        // n == 4 in every observed record, giving 104 bytes, but it is read rather
        // than assumed because nothing in the format fixes it.
        if (kQuadFixedSize > cursor.Remaining())
        {
            out.error = "quad record overruns HEAD";
            return out;
        }
        const uint8_t pointsInQuad = base[cursor.At() + 53];
        cursor.Skip(kQuadFixedSize);
        cursor.Skip(12ull * pointsInQuad);
        if (!cursor.ok())
        {
            out.error = "quad points overrun HEAD";
            return out;
        }
    }

    out.headCloses = cursor.ok() && cursor.At() == headEnd;
    if (!out.headCloses)
    {
        out.error = "HEAD walk ended at " + std::to_string(cursor.At()) + " of " + std::to_string(headEnd);
        return out;
    }

    // Do the per-LOD extents tile the LODS chunk exactly? This is the check that
    // catches a wrong descriptor width the HEAD closure tolerates, because a
    // descriptor read one field short still lands on the chunk end when the error
    // cancels -- but its offsets then point nowhere.
    const IffChunk* lodsChunk = iff.Find(FourCC("LODS"));
    if (out.lods.empty())
    {
        out.lodsTileChunk = (lodsChunk == nullptr);
    }
    else if (lodsChunk != nullptr)
    {
        std::vector<std::pair<uint32_t, uint32_t>> extents;
        extents.reserve(out.lods.size());
        for (const XobLodDesc& lod : out.lods)
            extents.emplace_back(lod.dataOffset, lod.dataSize);
        std::sort(extents.begin(), extents.end());
        size_t cursorAt = lodsChunk->offset;
        out.lodsTileChunk = true;
        for (const auto& extent : extents)
        {
            if (extent.first != cursorAt)
            {
                out.lodsTileChunk = false;
                break;
            }
            cursorAt = extent.first + extent.second;
        }
        if (out.lodsTileChunk && cursorAt != lodsChunk->offset + lodsChunk->size)
            out.lodsTileChunk = false;
    }

    out.sizesClose = true;
    for (const XobLodDesc& lod : out.lods)
    {
        if (lod.PredictedRawBytes() != lod.dataSizeRaw)
            out.sizesClose = false;
        if (static_cast<uint64_t>(lod.dataOffset) + lod.dataSize > size)
        {
            out.error = "LOD payload runs past the end of the file";
            return out;
        }
    }
    return out;
}

bool DecodeXobLodPayload(const void* data, size_t size, const XobLodDesc& lod, std::vector<uint8_t>& raw,
                         std::string& error)
{
    raw.clear();
    if (static_cast<uint64_t>(lod.dataOffset) + lod.dataSize > size)
    {
        error = "LOD payload runs past the end of the file";
        return false;
    }
    const auto* blob = static_cast<const uint8_t*>(data) + lod.dataOffset;

    raw.resize(lod.dataSizeRaw);
    size_t produced = 0;
    size_t at = 0;
    bool sawLast = false;
    while (at < lod.dataSize)
    {
        if (lod.dataSize - at < 4)
        {
            error = "LZ4 chain ends mid-header";
            return false;
        }
        const uint32_t header = ReadLe32(blob + at);
        at += 4;
        // Bit 31 marks the LAST block. It is NOT a "compressed" flag: reading it as
        // one takes the size field to 0x7fffffff on the final block of every model.
        const uint32_t blockSize = header & 0x7FFFFFFFu;
        const bool last = (header & 0x80000000u) != 0;
        if (blockSize == 0 || blockSize > lod.dataSize - at)
        {
            error = "LZ4 block of " + std::to_string(blockSize) + " bytes overruns the payload";
            return false;
        }
        // The blocks are LINKED, so each decode is handed the running output length
        // and is allowed to match back into what earlier blocks produced. Decoding
        // them independently gets the first 64 KiB of a LOD right and then diverges.
        const size_t appended = Foundation::Lz4Block::Decompress(blob + at, blockSize, raw.data(), raw.size(), produced);
        if (appended == 0)
        {
            error = "LZ4 block failed to decode";
            return false;
        }
        produced += appended;
        at += blockSize;
        if (last)
        {
            sawLast = true;
            break;
        }
    }
    if (!sawLast || at != lod.dataSize)
    {
        error = "LZ4 chain left " + std::to_string(lod.dataSize - at) + " of " + std::to_string(lod.dataSize) +
                " compressed bytes";
        return false;
    }
    if (produced != lod.dataSizeRaw)
    {
        error = "LZ4 produced " + std::to_string(produced) + " bytes against a declared " +
                std::to_string(lod.dataSizeRaw);
        return false;
    }
    return true;
}

bool ReadXobLod(const void* data, size_t size, const XobHeader& header, size_t lodIndex, XobLod& out,
                std::string& error)
{
    out = XobLod();
    if (lodIndex >= header.lods.size())
    {
        error = "LOD index out of range";
        return false;
    }
    const XobLodDesc& lod = header.lods[lodIndex];
    out.threshold = lod.threshold;

    for (const XobPartDesc& part : lod.parts)
    {
        // Billiard_01_ball.xob LOD1 is the corpus's only nUV == 0 part and it spends
        // 28 bytes per vertex where the formula predicts 20. Its layout was never
        // established, so it is refused rather than guessed at -- a guess here
        // silently produces garbage geometry for every later part in the blob.
        if (part.uvSetCount == 0)
        {
            error = "part declares no UV set; that layout was never established";
            return false;
        }
        if (part.materialIndex >= header.materials.size())
        {
            error = "part references material " + std::to_string(part.materialIndex) + " of " +
                    std::to_string(header.materials.size());
            return false;
        }
    }
    if (lod.PredictedRawBytes() != lod.dataSizeRaw)
    {
        error = "declared dataSizeRaw " + std::to_string(lod.dataSizeRaw) + " against predicted " +
                std::to_string(lod.PredictedRawBytes());
        return false;
    }

    std::vector<uint8_t> raw;
    if (!DecodeXobLodPayload(data, size, lod, raw, error))
        return false;

    Cursor cursor(raw.data(), 0, raw.size());
    out.parts.resize(lod.parts.size());
    for (size_t p = 0; p < lod.parts.size(); ++p)
    {
        const XobPartDesc& desc = lod.parts[p];
        XobPart& part = out.parts[p];
        part.materialIndex = desc.materialIndex;
        part.uvSetCount = desc.uvSetCount;
        part.skinned = desc.skinned();
        part.bboxMin = desc.bboxMin;
        part.bboxMax = desc.bboxMax;
        part.triangleCount = desc.triangleCount;

        const size_t indexCount = static_cast<size_t>(desc.triangleCount) * 3;
        part.indices.resize(indexCount);
        part.renderIndices.resize(indexCount);
        if (indexCount != 0)
        {
            if (!cursor.Bytes(part.indices.data(), indexCount * 2) ||
                !cursor.Bytes(part.renderIndices.data(), indexCount * 2))
            {
                error = "index lists overrun the payload";
                return false;
            }
        }

        part.positions.resize(desc.vertexCount);
        for (XobVec3& position : part.positions)
        {
            position = cursor.Vec3();
            // Skinned vertices append a u32 index into the skin table, which is what
            // makes their stride 16 rather than 12.
            if (desc.skinned())
                cursor.U32();
        }

        part.normals.resize(desc.vertexCount);
        for (XobVec3& normal : part.normals)
            normal = UnpackXobDirection(cursor.U32());

        // The colour stream comes BEFORE the UV sets, not after. The byte total is
        // the same either way, so every size closure held with the order reversed --
        // and every colour-bearing part (vertexFormat 0x20: all the vegetation
        // polyplanes, most tree bark) decoded its colour words as UVs. Measured on
        // 309 colour-bearing LOD0 parts across 150 corpus models: read colour-first,
        // 263 parts span >= 90% of their declared UV rect in both axes (mean 0.95 /
        // 0.97); read UV-first, 0 do (mean 0.23 / 0.002) -- the "UVs" were
        // 0xFFxx0000 colour words, which is why every Everon pine crown rendered as
        // flat single-texel cards.
        if (desc.hasColours())
        {
            part.colours.resize(desc.vertexCount);
            for (uint32_t& colour : part.colours)
                colour = cursor.U32();
        }

        part.uvSets.resize(desc.uvSetCount);
        part.uvSetDescs = desc.uvSets;
        for (uint8_t s = 0; s < desc.uvSetCount; ++s)
            part.uvSets[s].resize(desc.vertexCount);
        if (XobUvPlanar())
        {
            for (uint8_t s = 0; s < desc.uvSetCount; ++s)
                for (XobVec2& uv : part.uvSets[s])
                    uv = UnpackXobUv(cursor.U32(), desc.uvSets[s]);
        }
        else
        {
            for (size_t v = 0; v < desc.vertexCount; ++v)
                for (uint8_t s = 0; s < desc.uvSetCount; ++s)
                    part.uvSets[s][v] = UnpackXobUv(cursor.U32(), desc.uvSets[s]);
        }

        part.renderSource.resize(desc.renderVertexCount);
        part.tangents.resize(desc.renderVertexCount);
        part.bitangents.resize(desc.renderVertexCount);
        const int uvWord = XobRenderUvWord();
        if (uvWord > 0)
        {
            part.renderUvSets.resize(desc.uvSetCount);
            for (uint8_t s = 0; s < desc.uvSetCount; ++s)
                part.renderUvSets[s].resize(desc.renderVertexCount);
        }
        for (size_t v = 0; v < desc.renderVertexCount; ++v)
        {
            part.renderSource[v] = cursor.U32();
            for (uint8_t s = 0; s < desc.uvSetCount; ++s)
            {
                // RFG-102: one of the two per-set words is the packed UV (POSEIDON_XOB_UV_RENDER
                // picks which); the other is the tangent. Without the switch: tangent, bitangent.
                const uint32_t word1 = cursor.U32();
                const uint32_t word2 = cursor.U32();
                if (uvWord > 0)
                    part.renderUvSets[s][v] = UnpackXobUv(uvWord == 1 ? word1 : word2, desc.uvSets[s]);
                const XobVec3 tangent = UnpackXobDirection(uvWord == 1 ? word2 : word1);
                const XobVec3 bitangent = uvWord > 0 ? tangent : UnpackXobDirection(word2);
                // Only set 0's frame is kept: set 1 is a lightmap/detail channel and
                // nothing downstream consumes a second tangent basis.
                if (s == 0)
                {
                    part.tangents[v] = tangent;
                    part.bitangents[v] = bitangent;
                }
            }
        }

        // The skin table is stepped over: bone weights are not render geometry and
        // the bone hierarchy is not decoded, but the bytes still have to be counted.
        if (desc.skinned())
            cursor.Skip(16ull * desc.skinEntryCount);

        if (!cursor.ok())
        {
            error = "part " + std::to_string(p) + " overruns the decompressed payload";
            return false;
        }
        for (uint16_t index : part.indices)
        {
            if (index >= part.positions.size())
            {
                error = "list A index out of range";
                return false;
            }
        }
        for (uint16_t index : part.renderIndices)
        {
            if (index >= part.renderSource.size())
            {
                error = "list B index out of range";
                return false;
            }
        }
        for (uint32_t source : part.renderSource)
        {
            if (source >= part.positions.size())
            {
                error = "render vertex source index out of range";
                return false;
            }
        }
    }

    if (cursor.At() != raw.size())
    {
        error = "payload walk left " + std::to_string(raw.size() - cursor.At()) + " of " +
                std::to_string(raw.size()) + " bytes";
        return false;
    }
    return true;
}

XobModel ReadXobModel(const void* data, size_t size, bool decodeGeometry)
{
    XobModel out;
    const XobHeader header = ReadXobHeader(data, size);
    if (!header.valid())
    {
        out.error = header.error;
        return out;
    }
    out.bboxMin = header.bboxMin;
    out.bboxMax = header.bboxMax;
    out.sphereCenter = header.sphereCenter;
    out.sphereRadius = header.sphereRadius;
    out.materials = header.materials;

    if (!decodeGeometry)
    {
        out.lods.resize(header.lods.size());
        for (size_t i = 0; i < header.lods.size(); ++i)
            out.lods[i].threshold = header.lods[i].threshold;
        return out;
    }

    out.lods.resize(header.lods.size());
    for (size_t i = 0; i < header.lods.size(); ++i)
    {
        std::string error;
        if (!ReadXobLod(data, size, header, i, out.lods[i], error))
        {
            out.error = "LOD " + std::to_string(i) + ": " + error;
            out.lods.clear();
            return out;
        }
    }
    return out;
}

} // namespace Poseidon::Asset::Formats::Enfusion
