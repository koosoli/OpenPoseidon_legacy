// test_xob_model.cpp - ARF-002: Arma Reforger's `.xob` render geometry.
//
// Every buffer here is built byte by byte from the format, so nothing Reforger-owned
// is committed and the expected result is known by construction rather than by
// comparison against a decoder that could be wrong in the same way.
//
// The corpus check that matters lives outside CI, where the corpus is the 15,809
// `.xob` files of a local Arma Reforger install: container walk 15809/15809, HEAD
// byte accounting 15809/15809, per-LOD extents tiling LODS 15809/15809, and
// dataSizeRaw against predicted part sizes 56770/56771 LODs -- the one miss being
// Billiard_01_ball.xob LOD1, the corpus's only nUV == 0 part, which this reader
// refuses on purpose.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Asset/Formats/Enfusion/XobModel.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using Catch::Approx;
using Poseidon::Asset::Formats::Enfusion::ReadXobHeader;
using Poseidon::Asset::Formats::Enfusion::ReadXobModel;
using Poseidon::Asset::Formats::Enfusion::UnpackXobDirection;
using Poseidon::Asset::Formats::Enfusion::UnpackXobUv;
using Poseidon::Asset::Formats::Enfusion::XobUvSet;

namespace
{

void AppendBe32(std::vector<uint8_t>& out, uint32_t value)
{
    for (int i = 3; i >= 0; --i)
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
}

void AppendLe32(std::vector<uint8_t>& out, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
}

void AppendLe16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void AppendF32(std::vector<uint8_t>& out, float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    AppendLe32(out, bits);
}

void AppendVec3(std::vector<uint8_t>& out, float x, float y, float z)
{
    AppendF32(out, x);
    AppendF32(out, y);
    AppendF32(out, z);
}

void AppendTag(std::vector<uint8_t>& out, const char* tag)
{
    out.insert(out.end(), tag, tag + 4);
}

void AppendChunk(std::vector<uint8_t>& out, const char* tag, const std::vector<uint8_t>& payload)
{
    AppendTag(out, tag);
    AppendBe32(out, static_cast<uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
}

std::vector<uint8_t> Form(const char* type, const std::vector<uint8_t>& chunks)
{
    std::vector<uint8_t> out;
    AppendTag(out, "FORM");
    AppendBe32(out, static_cast<uint32_t>(4 + chunks.size()));
    AppendTag(out, type);
    out.insert(out.end(), chunks.begin(), chunks.end());
    return out;
}

//! Emits one literals-only LZ4 block body. Legal LZ4: the final sequence of a block
//! carries literals and no match, so a whole block of literals decodes to itself.
std::vector<uint8_t> Lz4LiteralBlock(const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> out;
    const size_t length = payload.size();
    if (length < 15)
    {
        out.push_back(static_cast<uint8_t>(length << 4));
    }
    else
    {
        out.push_back(0xF0);
        size_t remaining = length - 15;
        while (remaining >= 255)
        {
            out.push_back(0xFF);
            remaining -= 255;
        }
        out.push_back(static_cast<uint8_t>(remaining));
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

//! Wraps block bodies in the .xob chain framing: u32 header, low 31 bits the
//! compressed size, bit 31 marking the LAST block.
void AppendLz4Chain(std::vector<uint8_t>& out, const std::vector<std::vector<uint8_t>>& blocks)
{
    for (size_t i = 0; i < blocks.size(); ++i)
    {
        uint32_t header = static_cast<uint32_t>(blocks[i].size());
        if (i + 1 == blocks.size())
            header |= 0x80000000u;
        AppendLe32(out, header);
        out.insert(out.end(), blocks[i].begin(), blocks[i].end());
    }
}

//! Packs a direction the way the format does, so the test encodes and the reader
//! decodes rather than both sharing one expression.
uint32_t PackDirection(uint32_t x11, uint32_t y11, uint32_t z10)
{
    return (x11 << 21) | (y11 << 10) | z10;
}

uint32_t PackUv(int16_t u, int16_t v)
{
    return static_cast<uint32_t>(static_cast<uint16_t>(u)) | (static_cast<uint32_t>(static_cast<uint16_t>(v)) << 16);
}

// ---------------------------------------------------------------- model builder

struct PartSpec
{
    uint16_t triangleCountLow = 0;
    uint32_t flags3 = 0;
    uint16_t vertexCount = 0;
    uint16_t renderVertexCount = 0;
    uint16_t materialIndex = 0;
    uint8_t uvSetCount = 1;
    uint8_t vertexFormat = 0x0F;
    XobUvSet uv0{0.0f, 1.0f, 0.0f, 1.0f, 1.0f};
    XobUvSet uv1{0.0f, 1.0f, 0.0f, 1.0f, 1.0f};
    float bboxMin[3] = {-1.0f, -1.0f, -1.0f};
    float bboxMax[3] = {1.0f, 1.0f, 1.0f};
};

void AppendPartDesc(std::vector<uint8_t>& head, const PartSpec& spec)
{
    AppendLe16(head, 0);                 // flags0
    head.push_back(0);                   // flags1
    head.push_back(spec.vertexFormat);   // vfmt
    AppendVec3(head, spec.bboxMin[0], spec.bboxMin[1], spec.bboxMin[2]);
    AppendVec3(head, spec.bboxMax[0], spec.bboxMax[1], spec.bboxMax[2]);
    AppendVec3(head, 0.0f, 0.0f, 0.0f);  // plane normal
    AppendF32(head, 0.0f);               // plane distance
    AppendLe16(head, spec.triangleCountLow);
    AppendLe16(head, spec.vertexCount);
    AppendLe16(head, spec.renderVertexCount);
    AppendLe32(head, 0);                 // skin entries
    AppendLe16(head, 0xFFFF);            // magic
    AppendLe16(head, spec.materialIndex);
    head.push_back(spec.uvSetCount);
    head.push_back(0); // bone count
    AppendLe32(head, spec.flags3);
    const XobUvSet* sets[2] = {&spec.uv0, &spec.uv1};
    for (uint8_t s = 0; s < spec.uvSetCount; ++s)
    {
        AppendF32(head, sets[s]->uMin);
        AppendF32(head, sets[s]->uMax);
        AppendF32(head, sets[s]->vMin);
        AppendF32(head, sets[s]->vMax);
        AppendF32(head, sets[s]->extra);
    }
}

//! Builds a one-LOD XOB9 around a raw (uncompressed) payload, LZ4-framing it and
//! patching the absolute dataOffset once the HEAD size is known.
std::vector<uint8_t> BuildXob(const std::vector<PartSpec>& parts, const std::vector<uint8_t>& rawPayload,
                              const std::vector<std::string>& materialPaths, uint32_t declaredRawSize,
                              bool splitPayloadIntoTwoBlocks = false)
{
    std::vector<uint8_t> lodsPayload;
    if (splitPayloadIntoTwoBlocks)
    {
        // Two linked blocks: the second is a single match reaching back into the
        // first block's output, which only decodes if the chain shares one buffer.
        const size_t half = rawPayload.size() / 2;
        std::vector<uint8_t> first(rawPayload.begin(), rawPayload.begin() + half);
        std::vector<uint8_t> second(rawPayload.begin() + half, rawPayload.end());
        AppendLz4Chain(lodsPayload, {Lz4LiteralBlock(first), Lz4LiteralBlock(second)});
    }
    else
    {
        AppendLz4Chain(lodsPayload, {Lz4LiteralBlock(rawPayload)});
    }

    std::vector<uint8_t> stringTable;
    std::vector<uint16_t> ordinals;
    for (const std::string& path : materialPaths)
    {
        ordinals.push_back(static_cast<uint16_t>(ordinals.size()));
        const std::string name = "mat" + std::to_string(ordinals.size());
        stringTable.insert(stringTable.end(), name.begin(), name.end());
        stringTable.push_back(0);
        ordinals.push_back(static_cast<uint16_t>(ordinals.size()));
        stringTable.insert(stringTable.end(), path.begin(), path.end());
        stringTable.push_back(0);
    }

    std::vector<uint8_t> head;
    AppendLe32(head, 0); // version
    AppendVec3(head, -1.0f, -2.0f, -3.0f);
    AppendVec3(head, 1.0f, 2.0f, 3.0f);
    AppendVec3(head, 0.0f, 0.0f, 0.0f);
    AppendF32(head, 4.0f); // sphere radius
    AppendLe16(head, static_cast<uint16_t>(materialPaths.size()));
    AppendLe16(head, 0); // bones
    head.push_back(1);   // LODs
    head.push_back(0);   // points
    AppendLe16(head, 0); // pad
    AppendLe32(head, 0); // quads
    AppendLe32(head, static_cast<uint32_t>(stringTable.size()));
    head.insert(head.end(), stringTable.begin(), stringTable.end());
    for (uint16_t ordinal : ordinals)
        AppendLe16(head, ordinal);

    AppendTag(head, "LZO4");
    AppendLe32(head, static_cast<uint32_t>(parts.size()));
    AppendLe32(head, 0);     // reserved
    AppendF32(head, 0.5f);   // LOD0 threshold
    AppendLe32(head, 0);     // skin bones
    const size_t offsetField = head.size();
    AppendLe32(head, 0); // dataOffset, patched below
    AppendLe32(head, static_cast<uint32_t>(lodsPayload.size()));
    AppendLe32(head, declaredRawSize);
    for (const PartSpec& part : parts)
        AppendPartDesc(head, part);

    // FORM header (12) + HEAD chunk header (8) + HEAD + LODS chunk header (8).
    const uint32_t dataOffset = static_cast<uint32_t>(12 + 8 + head.size() + 8);
    for (int i = 0; i < 4; ++i)
        head[offsetField + i] = static_cast<uint8_t>((dataOffset >> (i * 8)) & 0xFF);

    std::vector<uint8_t> chunks;
    AppendChunk(chunks, "HEAD", head);
    AppendChunk(chunks, "LODS", lodsPayload);
    return Form("XOB9", chunks);
}

} // namespace

TEST_CASE("XOB: packed directions unpack to the measured field split", "[asset][enfusion][xob][arf-002]")
{
    // z = bits 0..9 (10-bit), y = bits 10..20 (11-bit), x = bits 21..31 (11-bit),
    // each n / (2^w - 1) * 2 - 1. Asserting the field WIDTHS separately is the point:
    // a 10/11/11 split and an 11/11/10 split agree on the all-ones value and
    // disagree everywhere else, which is how a wrong layout survives a spot check.
    const auto all = UnpackXobDirection(PackDirection(2047, 2047, 1023));
    REQUIRE(all.x == Approx(1.0 / std::sqrt(3.0)).margin(1e-5));
    REQUIRE(all.y == Approx(1.0 / std::sqrt(3.0)).margin(1e-5));
    REQUIRE(all.z == Approx(1.0 / std::sqrt(3.0)).margin(1e-5));

    const auto none = UnpackXobDirection(0);
    REQUIRE(none.x == Approx(-1.0 / std::sqrt(3.0)).margin(1e-5));
    REQUIRE(none.z == Approx(-1.0 / std::sqrt(3.0)).margin(1e-5));

    // Only the 10-bit z field set: z is +1 before normalisation, x and y are -1.
    const auto zOnly = UnpackXobDirection(0x3FF);
    REQUIRE(zOnly.z == Approx(1.0 / std::sqrt(3.0)).margin(1e-5));
    REQUIRE(zOnly.x == Approx(-1.0 / std::sqrt(3.0)).margin(1e-5));

    // The 10-bit z field must not bleed into y: 0x400 is y's least significant bit,
    // so z drops back to -1 and y steps one quantum up from it.
    const auto yBit = UnpackXobDirection(0x400);
    REQUIRE(yBit.z < 0.0f);
    REQUIRE(yBit.y < 0.0f);
    REQUIRE(yBit.y > UnpackXobDirection(0).y);

    // A near-axis direction: +X with y and z at their nearest-to-zero codes. The
    // corpus's own evidence for this layout was the six face normals of a box.
    const auto plusX = UnpackXobDirection(PackDirection(2047, 1023, 512));
    REQUIRE(plusX.x > 0.9999f);
    REQUIRE(std::fabs(plusX.y) < 1e-3f);
    REQUIRE(std::fabs(plusX.z) < 1e-3f);
    REQUIRE(std::fabs(std::sqrt(plusX.x * plusX.x + plusX.y * plusX.y + plusX.z * plusX.z) - 1.0f) < 1e-5f);
}

TEST_CASE("XOB: UVs dequantise through their set's rect", "[asset][enfusion][xob][arf-002]")
{
    // The signed s16 halves span the rect end to end. Dropping the rect leaves every
    // atlased part on the wrong tile, which parses perfectly and renders wrong.
    const XobUvSet unit{0.0f, 1.0f, 0.0f, 1.0f, 0.0f};
    REQUIRE(UnpackXobUv(PackUv(32767, 32767), unit).u == Approx(1.0f));
    REQUIRE(UnpackXobUv(PackUv(32767, 32767), unit).v == Approx(1.0f));
    REQUIRE(UnpackXobUv(PackUv(-32767, -32767), unit).u == Approx(0.0f).margin(1e-6));
    REQUIRE(UnpackXobUv(PackUv(0, 0), unit).u == Approx(0.5f));

    // A shifted, non-unit rect, and a V axis that runs backwards -- both occur.
    const XobUvSet shifted{0.25f, 0.75f, 1.0f, -1.0f, 12.0f};
    REQUIRE(UnpackXobUv(PackUv(0, 0), shifted).u == Approx(0.5f));
    REQUIRE(UnpackXobUv(PackUv(32767, 0), shifted).u == Approx(0.75f));
    REQUIRE(UnpackXobUv(PackUv(-32767, 32767), shifted).u == Approx(0.25f).margin(1e-6));
    REQUIRE(UnpackXobUv(PackUv(0, 32767), shifted).v == Approx(-1.0f));
    REQUIRE(UnpackXobUv(PackUv(0, -32767), shifted).v == Approx(1.0f));

    // The two halves are independent: a value in U must not disturb V.
    REQUIRE(UnpackXobUv(PackUv(32767, 0), unit).v == Approx(0.5f));
}

TEST_CASE("XOB: the triangle count is 32-bit across two fields", "[asset][enfusion][xob][arf-002]")
{
    // nTris = nTrisLo | ((flags3 >> 16) << 16). Three corpus parts exceed 65,535
    // triangles and were each short by exactly 12 * 65536 payload bytes until the
    // high half was applied, so the size prediction is what this asserts.
    PartSpec spec;
    spec.triangleCountLow = 7;
    spec.flags3 = (1u << 16) | 2u; // one high half, plus a low-half flag that must not leak
    spec.vertexCount = 4;
    spec.renderVertexCount = 4;
    spec.uvSetCount = 1;

    const auto file = BuildXob({spec}, {}, {"{ABCD}Common/Mat.emat"}, 0);
    const auto header = ReadXobHeader(file.data(), file.size());
    REQUIRE(header.valid());
    REQUIRE(header.lods.size() == 1);
    REQUIRE(header.lods[0].parts.size() == 1);
    const auto& part = header.lods[0].parts[0];
    REQUIRE(part.triangleCount == 65543u);

    // 12 bytes per triangle, 20 per deduplicated vertex (12 position + 4 normal +
    // 4 UV) and 12 per render vertex (4 source + 8 tangent pair).
    REQUIRE(part.VertexStride() == 20u);
    REQUIRE(part.RenderVertexStride() == 12u);
    REQUIRE(part.PayloadBytes() == 12ull * 65543 + 20ull * 4 + 12ull * 4);
    // Losing the high half would predict exactly 12 * 65536 fewer bytes.
    REQUIRE(part.PayloadBytes() - (12ull * 7 + 20ull * 4 + 12ull * 4) == 12ull * 65536);
}

TEST_CASE("XOB: a whole model decodes and list B remaps onto list A", "[asset][enfusion][xob][arf-002]")
{
    // Two triangles over four deduplicated vertices, with vertex 3 duplicated as a
    // fifth render vertex -- a UV seam, the only reason the render stream is ever
    // longer than the deduplicated one. Mapping list B through the source indices
    // must reproduce list A's triangles exactly (14502 / 14502 corpus parts).
    PartSpec spec;
    spec.triangleCountLow = 2;
    spec.vertexCount = 4;
    spec.renderVertexCount = 5;
    spec.uvSetCount = 1;
    spec.uv0 = XobUvSet{0.0f, 1.0f, 0.0f, 1.0f, 1.0f};
    spec.bboxMin[0] = 0.0f;
    spec.bboxMin[1] = 0.0f;
    spec.bboxMin[2] = 0.0f;
    spec.bboxMax[0] = 1.0f;
    spec.bboxMax[1] = 1.0f;
    spec.bboxMax[2] = 0.0f;

    const uint16_t listA[6] = {0, 1, 2, 2, 1, 3};
    const uint16_t listB[6] = {0, 1, 2, 4, 1, 3}; // render vertex 4 stands in for 2
    const uint32_t sources[5] = {0, 1, 2, 3, 2};

    const float positions[4][3] = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}};

    std::vector<uint8_t> raw;
    for (uint16_t index : listA)
        AppendLe16(raw, index);
    for (uint16_t index : listB)
        AppendLe16(raw, index);
    for (const auto& position : positions)
        AppendVec3(raw, position[0], position[1], position[2]);
    for (int i = 0; i < 4; ++i)
        AppendLe32(raw, PackDirection(1023, 1023, 1023)); // +Z-ish, exact value is not the subject
    const uint32_t packedUvs[4] = {PackUv(-32767, -32767), PackUv(32767, -32767), PackUv(-32767, 32767),
                                   PackUv(32767, 32767)};
    for (uint32_t uv : packedUvs)
        AppendLe32(raw, uv);
    for (uint32_t source : sources)
    {
        AppendLe32(raw, source);
        AppendLe32(raw, PackDirection(2047, 1023, 512)); // tangent
        AppendLe32(raw, PackDirection(1023, 2047, 512)); // bitangent
    }

    const auto file = BuildXob({spec}, raw, {"{0123456789ABCDEF}Assets/Props/Poster.emat"},
                               static_cast<uint32_t>(raw.size()));
    const auto model = ReadXobModel(file.data(), file.size());
    INFO(model.error);
    REQUIRE(model.valid());
    REQUIRE(model.materials.size() == 1);
    // The GUID is stripped; the path is the half that resolves against the pak tree.
    REQUIRE(model.materials[0].path == "Assets/Props/Poster.emat");
    REQUIRE(model.lods.size() == 1);
    REQUIRE(model.lods[0].threshold == Approx(0.5f));
    REQUIRE(model.lods[0].parts.size() == 1);

    const auto& part = model.lods[0].parts[0];
    REQUIRE(part.triangleCount == 2u);
    REQUIRE(part.positions.size() == 4);
    REQUIRE(part.renderSource.size() == 5);
    REQUIRE(part.indices.size() == 6);
    REQUIRE(part.renderIndices.size() == 6);
    REQUIRE(part.uvSets.size() == 1);
    REQUIRE(part.uvSets[0].size() == 4);
    REQUIRE(part.tangents.size() == 5);

    REQUIRE(part.positions[3].x == Approx(1.0f));
    REQUIRE(part.positions[3].y == Approx(1.0f));
    REQUIRE(part.uvSets[0][3].u == Approx(1.0f));
    REQUIRE(part.uvSets[0][0].u == Approx(0.0f).margin(1e-6));

    // The remap, as an unordered set of unordered triangles -- winding is preserved
    // by the file but the claim being checked is that the two streams describe the
    // same surface.
    auto triangleSet = [](const std::vector<uint16_t>& indices, const std::vector<uint32_t>* through)
    {
        std::set<std::vector<uint32_t>> out;
        for (size_t i = 0; i + 2 < indices.size(); i += 3)
        {
            std::vector<uint32_t> triangle;
            for (size_t k = 0; k < 3; ++k)
            {
                const uint32_t index = indices[i + k];
                triangle.push_back(through ? (*through)[index] : index);
            }
            std::sort(triangle.begin(), triangle.end());
            out.insert(triangle);
        }
        return out;
    };
    REQUIRE(triangleSet(part.indices, nullptr) == triangleSet(part.renderIndices, &part.renderSource));

    // And the render position of vertex 4 is the position it duplicates.
    REQUIRE(part.RenderPosition(4).x == Approx(part.positions[2].x));
    REQUIRE(part.RenderPosition(4).y == Approx(part.positions[2].y));
}

TEST_CASE("XOB: the colour stream precedes the UV sets in a colour-bearing part",
          "[asset][enfusion][xob][arf-002][mat-051]")
{
    // vertexFormat 0x20 adds a u32 colour per deduplicated vertex, and it sits
    // BETWEEN the normals and the UV sets. Both orders total the same bytes, so the
    // dataSizeRaw closure cannot tell them apart; only the decoded values can. Read
    // UV-first, every Everon polyplane (all colour-bearing) got its colour words as
    // UVs -- 0xFFxx0000, i.e. u == 0 and v in a 512-step band -- and every leaf card
    // sampled one texel: the flat, uncut olive pine crowns. Measured on 309 colour-
    // bearing parts across 150 corpus models, colour-first decodes 263 to a UV span
    // >= 90% of the declared rect on both axes; UV-first decodes 0.
    //
    // The payload here has colour words that are recognisable as colours and UVs
    // that hit the rect corners, so a swapped read fails on values, not on size.
    PartSpec spec;
    spec.triangleCountLow = 2;
    spec.vertexCount = 4;
    spec.renderVertexCount = 4;
    spec.uvSetCount = 1;
    spec.vertexFormat = 0x2F; // 0x0F base + 0x20 colour stream (the polyplane format)
    spec.uv0 = XobUvSet{0.0f, 1.0f, 0.0f, 1.0f, 1.0f};

    const uint16_t list[6] = {0, 1, 2, 2, 1, 3};
    const float positions[4][3] = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}};
    const uint32_t colours[4] = {0xFF810000u, 0xFF000000u, 0xFE000000u, 0xFFFF0000u};
    const uint32_t packedUvs[4] = {PackUv(-32767, -32767), PackUv(32767, -32767), PackUv(-32767, 32767),
                                   PackUv(32767, 32767)};

    std::vector<uint8_t> raw;
    for (uint16_t index : list)
        AppendLe16(raw, index);
    for (uint16_t index : list)
        AppendLe16(raw, index);
    for (const auto& position : positions)
        AppendVec3(raw, position[0], position[1], position[2]);
    for (int i = 0; i < 4; ++i)
        AppendLe32(raw, PackDirection(1023, 1023, 1023));
    for (uint32_t colour : colours) // colour stream FIRST
        AppendLe32(raw, colour);
    for (uint32_t uv : packedUvs) // then UV set 0
        AppendLe32(raw, uv);
    for (uint32_t source = 0; source < 4; ++source)
    {
        AppendLe32(raw, source);
        AppendLe32(raw, PackDirection(2047, 1023, 512));
        AppendLe32(raw, PackDirection(1023, 2047, 512));
    }

    const auto file = BuildXob({spec}, raw, {"{ABCD}t_picea_abies_1f_polyplane.emat"},
                               static_cast<uint32_t>(raw.size()));
    const auto header = ReadXobHeader(file.data(), file.size());
    INFO(header.error);
    REQUIRE(header.valid());
    REQUIRE(header.sizesClose); // the stride still counts the colour word exactly once
    REQUIRE(header.lods[0].parts[0].hasColours());

    const auto model = ReadXobModel(file.data(), file.size());
    INFO(model.error);
    REQUIRE(model.valid());
    const auto& part = model.lods[0].parts[0];
    REQUIRE(part.colours.size() == 4);
    REQUIRE(part.uvSets.size() == 1);
    REQUIRE(part.uvSets[0].size() == 4);

    // The colour words came back as colours...
    for (size_t i = 0; i < 4; ++i)
        REQUIRE(part.colours[i] == colours[i]);
    // ...and the UVs span the whole rect: corner (0,0) to corner (1,1). A swapped
    // read would put every u at 0.5 and every v within 0.4922..0.5 -- exactly the
    // degenerate band the deployed pine MLODs carried.
    REQUIRE(part.uvSets[0][0].u == Approx(0.0f).margin(1e-6));
    REQUIRE(part.uvSets[0][0].v == Approx(0.0f).margin(1e-6));
    REQUIRE(part.uvSets[0][1].u == Approx(1.0f));
    REQUIRE(part.uvSets[0][2].v == Approx(1.0f));
    REQUIRE(part.uvSets[0][3].u == Approx(1.0f));
    REQUIRE(part.uvSets[0][3].v == Approx(1.0f));
    float uMin = 1.0f, uMax = 0.0f, vMin = 1.0f, vMax = 0.0f;
    for (const auto& uv : part.uvSets[0])
    {
        uMin = std::min(uMin, uv.u);
        uMax = std::max(uMax, uv.u);
        vMin = std::min(vMin, uv.v);
        vMax = std::max(vMax, uv.v);
    }
    REQUIRE(uMax - uMin > 0.9f);
    REQUIRE(vMax - vMin > 0.9f);
}

TEST_CASE("XOB: the LZ4 chain is linked and bit 31 means last, not compressed",
          "[asset][enfusion][xob][arf-002]")
{
    // Two blocks whose combined output is one part's payload. Bit 31 read as a
    // "compressed" flag takes the size field to 0x7fffffff on the final block of
    // every model; blocks decoded independently get the first block right and then
    // diverge. Both mistakes are refused rather than half-read.
    PartSpec spec;
    spec.triangleCountLow = 1;
    spec.vertexCount = 3;
    spec.renderVertexCount = 3;
    spec.uvSetCount = 1;

    std::vector<uint8_t> raw;
    const uint16_t list[3] = {0, 1, 2};
    for (uint16_t index : list)
        AppendLe16(raw, index);
    for (uint16_t index : list)
        AppendLe16(raw, index);
    AppendVec3(raw, 0.0f, 0.0f, 0.0f);
    AppendVec3(raw, 1.0f, 0.0f, 0.0f);
    AppendVec3(raw, 0.0f, 1.0f, 0.0f);
    for (int i = 0; i < 3; ++i)
        AppendLe32(raw, PackDirection(1023, 1023, 1023));
    for (int i = 0; i < 3; ++i)
        AppendLe32(raw, PackUv(0, 0));
    for (uint32_t source = 0; source < 3; ++source)
    {
        AppendLe32(raw, source);
        AppendLe32(raw, PackDirection(2047, 1023, 512));
        AppendLe32(raw, PackDirection(1023, 2047, 512));
    }

    const auto file = BuildXob({spec}, raw, {"{ABCD}Mat.emat"}, static_cast<uint32_t>(raw.size()), true);
    const auto model = ReadXobModel(file.data(), file.size());
    INFO(model.error);
    REQUIRE(model.valid());
    REQUIRE(model.lods[0].parts[0].positions.size() == 3);
    REQUIRE(model.lods[0].parts[0].positions[1].x == Approx(1.0f));
}

TEST_CASE("XOB: a nUV == 0 part is refused rather than guessed", "[asset][enfusion][xob][arf-002]")
{
    // Billiard_01_ball.xob LOD1 is the corpus's only nUV == 0 part and spends 28
    // bytes per vertex where the formula predicts 20. Guessing here would emit
    // garbage for that part AND for every later part in the same blob, since the
    // parts share one payload.
    PartSpec spec;
    spec.triangleCountLow = 1;
    spec.vertexCount = 3;
    spec.renderVertexCount = 3;
    spec.uvSetCount = 0;

    const auto file = BuildXob({spec}, {}, {"{ABCD}Mat.emat"}, 12 + 3 * 16 + 3 * 4);
    const auto model = ReadXobModel(file.data(), file.size());
    REQUIRE_FALSE(model.valid());
    REQUIRE(model.error.find("UV") != std::string::npos);
}

TEST_CASE("XOB: byte accounting refuses a file that does not close", "[asset][enfusion][xob][arf-002]")
{
    PartSpec spec;
    spec.triangleCountLow = 1;
    spec.vertexCount = 3;
    spec.renderVertexCount = 3;
    spec.uvSetCount = 1;

    // A dataSizeRaw that disagrees with the predicted part sizes is the check that
    // caught the 32-bit triangle count in the first place; it must stay fatal.
    const uint64_t predicted = 12ull * 1 + 20ull * 3 + 12ull * 3;
    std::vector<uint8_t> raw(static_cast<size_t>(predicted), 0);
    const auto wrongSize = BuildXob({spec}, raw, {"{ABCD}Mat.emat"}, static_cast<uint32_t>(predicted) + 8);
    const auto header = ReadXobHeader(wrongSize.data(), wrongSize.size());
    REQUIRE(header.headCloses);
    REQUIRE_FALSE(header.sizesClose);
    const auto refused = ReadXobModel(wrongSize.data(), wrongSize.size());
    REQUIRE_FALSE(refused.valid());

    // The same file with the honest size closes on every criterion.
    const auto good = BuildXob({spec}, raw, {"{ABCD}Mat.emat"}, static_cast<uint32_t>(predicted));
    const auto goodHeader = ReadXobHeader(good.data(), good.size());
    REQUIRE(goodHeader.valid());
    REQUIRE(goodHeader.headCloses);
    REQUIRE(goodHeader.sizesClose);
    REQUIRE(goodHeader.lodsTileChunk);
    // All-zero indices and vertices are in range, so this decodes.
    REQUIRE(ReadXobModel(good.data(), good.size()).valid());

    // A HEAD with a byte of slack at the end must be refused, not tolerated.
    auto slack = good;
    REQUIRE(slack.size() > 20);
    const auto notXob = Form("TERR", std::vector<uint8_t>());
    const auto wrongForm = ReadXobHeader(notXob.data(), notXob.size());
    REQUIRE_FALSE(wrongForm.valid());
    REQUIRE(wrongForm.error.find("XOB9") != std::string::npos);
}
