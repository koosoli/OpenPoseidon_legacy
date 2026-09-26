// test_odol40_arma1.cpp - AST-013: the Arma 1 (ODOL revision 40) reader.
//
// Before this, nothing of revision 40 parsed: every one of the 893 models
// sara.wrp references failed at the container. The layout was solved against the
// whole local A1 corpus (2,580 models, 18,697 LOD bodies closing exactly on their
// declared boundary), and this file pins the parts of that result which can be
// stated from bytes alone, plus an opt-in corpus survey for the rest.
//
// Every model here is built byte by byte rather than shipped as a fixture: the A1
// data is licensed content that does not live in this repo, and the properties
// that matter -- which fields exist at which revision, and where a block ends --
// are exactly the ones a synthetic model can state.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Asset/Formats/P3D/ODOLLoader.hpp>
#include <Poseidon/Asset/Formats/P3D/Odol40.hpp>
#include <Poseidon/Asset/Formats/P3D/OdolRevision.hpp>
#include <Poseidon/World/Model/ModelBlob.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace P3D = Poseidon::Asset::Formats::P3D;
using Poseidon::Asset::Formats::BinaryReader;
using Poseidon::Asset::Formats::ODOLLoader;

namespace
{

// A little-endian byte builder. Deliberately explicit: the whole subject of these
// tests is which bytes are on the wire and in what order.
struct Bytes
{
    std::vector<char> data;

    void u8(uint8_t value) { data.push_back(static_cast<char>(value)); }
    void boolean(bool value) { u8(value ? 1 : 0); }
    void u16(uint16_t value) { for (int i = 0; i < 2; ++i) u8(static_cast<uint8_t>(value >> (8 * i))); }
    void u32(uint32_t value) { for (int i = 0; i < 4; ++i) u8(static_cast<uint8_t>(value >> (8 * i))); }
    void i32(int32_t value) { u32(static_cast<uint32_t>(value)); }
    void f32(float value) { uint32_t raw; std::memcpy(&raw, &value, 4); u32(raw); }
    void vec3(float x, float y, float z) { f32(x); f32(y); f32(z); }
    void asciiz(const char* text) { while (*text) u8(static_cast<uint8_t>(*text++)); u8(0); }
    void zeros(size_t count) { for (size_t i = 0; i < count; ++i) u8(0); }
    void raw(const std::vector<uint8_t>& bytes) { for (uint8_t byte : bytes) u8(byte); }

    size_t size() const { return data.size(); }
    void patch32(size_t at, uint32_t value)
    {
        for (int i = 0; i < 4; ++i) data[at + i] = static_cast<char>(value >> (8 * i));
    }
};

// An all-literal SSCompress (LZSS) payload: eight literals per flag byte, every
// flag bit set, then the trailing 32-bit sum of the decoded bytes that the
// decoder verifies. This is a valid encoding of `payload`, not a stub -- the
// format permits a stream with no back-references at all.
std::vector<uint8_t> lzssLiterals(const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> out;
    uint32_t             checksum = 0;
    for (size_t i = 0; i < payload.size(); ++i)
    {
        if (i % 8 == 0) out.push_back(0xff);
        out.push_back(payload[i]);
        checksum += payload[i];
    }
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(checksum >> (8 * i)));
    return out;
}

} // namespace

TEST_CASE("Revision 40 is claimed, not merely recognised", "[p3d][odol][ast-013]")
{
    const auto info = P3D::DescribeOdolRevision(40);
    REQUIRE(info.support == P3D::OdolSupport::NarrowSubset);
    REQUIRE(std::string(info.generation).find("Arma 1") != std::string::npos);
}

TEST_CASE("ODOL 40 short payloads are raw and preserve the next field", "[p3d][odol][ast-013]")
{
    // No per-payload compression flag at this revision: below the 1024-byte
    // threshold the bytes are simply there. Reading a flag would eat the first
    // byte of the payload and shift everything after it.
    const std::vector<char> data{'a', 'b', 'c', 0x7f};
    QIStream                stream(const_cast<char*>(data.data()), static_cast<int>(data.size()));
    BinaryReader            reader(stream);
    REQUIRE(P3D::ReadOdol40Compressed(reader, 3) == std::vector<uint8_t>{'a', 'b', 'c'});
    REQUIRE(reader.read<uint8_t>() == 0x7f);
}

TEST_CASE("ODOL 40 payloads at the threshold are LZSS", "[p3d][odol][ast-013]")
{
    // The single most consequential difference from the Arma 2 family: the bulk
    // arrays are SSCompress, not LZO. Decoding one as the other does not fail
    // cleanly, it desynchronises.
    std::vector<uint8_t> payload(1024);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i * 7 + 3);

    Bytes bytes;
    bytes.raw(lzssLiterals(payload));
    bytes.u8(0x7f); // sentinel: the checksum must be consumed, not left behind

    QIStream     stream(bytes.data.data(), static_cast<int>(bytes.size()));
    BinaryReader reader(stream);
    REQUIRE(P3D::ReadOdol40Compressed(reader, payload.size()) == payload);
    REQUIRE(reader.read<uint8_t>() == 0x7f);
}

TEST_CASE("ODOL 40 condensed arrays expand a single repeated element", "[p3d][odol][ast-013]")
{
    Bytes bytes;
    bytes.i32(4);        // count
    bytes.boolean(true); // every element is the one that follows
    bytes.i32(9);
    bytes.u8(0x7f);
    QIStream     stream(bytes.data.data(), static_cast<int>(bytes.size()));
    BinaryReader reader(stream);
    const auto   values = P3D::ReadOdol40CondensedArray<int32_t>(reader, "test", 16);
    REQUIRE(values == std::vector<int32_t>{9, 9, 9, 9});
    REQUIRE(reader.read<uint8_t>() == 0x7f);
}

// ---------------------------------------------------------------------------
// A complete, minimal revision-40 model.
// ---------------------------------------------------------------------------
namespace
{

// One LOD: three vertices, one triangle, one texture, one embedded material.
// Small enough that every array stays under the compression threshold, which is
// what lets the expected bytes be written out by hand.
//
// COL-001: `withMass` fills the physical-body fields -- a three-entry mass
// array (raw, under the threshold), the four totals, and a non-empty class name
// -- so a test can state that the reader now KEEPS them. Everything else is
// byte-identical to the massless model, so the closure tests are unaffected.
std::vector<char> arma1Model(bool withMass = false, bool withSourceMotion = false)
{
    Bytes b;
    b.data.insert(b.data.end(), {'O', 'D', 'O', 'L'});
    b.u32(40);
    b.u32(1);      // one LOD
    b.f32(1.0f);   // its resolution
    b.i32(0);      // special
    b.f32(2.0f);   // bounding sphere
    b.f32(3.0f);   // geometry sphere
    b.i32(0);      // remarks
    b.i32(0);      // and hints
    b.i32(0);      // or hints
    b.vec3(0.0f, 0.0f, 0.0f); // aiming centre
    b.u32(0xff112233);        // colour
    b.u32(0xff445566);        // colour type
    b.f32(0.5f);              // view density
    b.vec3(-1.0f, -2.0f, -3.0f); // bbox min
    b.vec3(1.0f, 2.0f, 3.0f);    // bbox max
    b.vec3(0.0f, 0.0f, 0.0f); // bounding centre
    b.vec3(0.0f, 0.0f, 0.0f); // geometry centre
    if (withMass)
    {
        b.vec3(0.5f, 1.5f, 2.5f);  // centre of mass
        b.vec3(1.0f, 0.0f, 0.0f);  // inverse inertia rows: identity
        b.vec3(0.0f, 1.0f, 0.0f);
        b.vec3(0.0f, 0.0f, 1.0f);
    }
    else
    {
        for (int i = 0; i < 4; ++i) b.vec3(0.0f, 0.0f, 0.0f); // centre of mass and inverse inertia
    }
    b.zeros(11);   // the eleven flag bytes -- 39 typed bytes on the A2 layout
    b.asciiz(withSourceMotion ? "original_skeleton" : "");
    if (withSourceMotion)
    {
        b.boolean(false); b.u32(1);
        b.asciiz("original_bone"); b.asciiz("");
    }
    b.u8(0);       // map type
    if (withMass)
    {
        b.i32(3); b.f32(100.0f); b.f32(200.0f); b.f32(300.0f); // mass array, raw (12 bytes < 1024)
        b.f32(600.0f); b.f32(1.0f / 600.0f);                    // mass, inverse mass
        b.f32(50.0f); b.f32(1.0f / 50.0f);                      // armour, inverse armour
    }
    else
    {
        b.i32(0);      // mass array
        b.f32(0.0f); b.f32(0.0f); b.f32(0.0f); b.f32(0.0f); // mass, inverse, armour, inverse
    }
    for (int i = 0; i < 12; ++i) b.u8(0xff);            // special-LOD index table
    b.u32(0);
    b.boolean(false);
    b.asciiz("TestVehicle");
    b.asciiz("");
    b.boolean(false);
    b.u32(0);
    b.boolean(withSourceMotion);
    if (withSourceMotion)
    {
        b.u32(1); // one original hide animation, revision40 payload
        b.u32(9); b.asciiz("original_hide"); b.asciiz("original_source");
        b.f32(0); b.f32(1); b.f32(0); b.f32(1); b.u32(0); b.f32(.5f);
        b.u32(1); // one resolution
        b.u32(1); b.u32(1); b.u32(0); // one bone referencing animation0
        b.i32(-1); // reverse mapping has no axis payload
    }

    const size_t startAt = b.size();
    b.u32(0); // LOD start, patched below
    const size_t endAt = b.size();
    b.u32(0); // LOD end, patched below
    b.boolean(true); // permanent, so no 17-byte trailer

    const uint32_t lodStart = static_cast<uint32_t>(b.size());
    b.patch32(startAt, lodStart);

    // --- LOD header ---
    b.u32(0); // proxies
    b.u32(0); // sub-skeleton mappings
    b.u32(0); // sub-skeleton sets
    b.i32(3); b.boolean(true); b.i32(0);  // clip flags: three vertices, all zero
    b.i32(0); b.i32(0);                   // or/and hints
    b.vec3(-1.0f, -1.0f, 0.0f);           // bbox min
    b.vec3(1.0f, 1.0f, 0.0f);             // bbox max
    b.vec3(0.0f, 0.0f, 0.0f);             // bbox centre
    b.f32(1.5f);                          // bbox radius
    b.u32(1); b.asciiz("ca\\data\\hlava.paa");

    // --- embedded material, version 9 (the only version Arma 1 ships) ---
    b.u32(1);
    b.asciiz("ca\\data\\hlava.rvmat");
    b.u32(9);
    for (int i = 0; i < 24; ++i) b.f32(0.0f); // six four-component colours
    b.f32(1.0f);                              // specular power
    b.u32(0); b.u32(0);                       // pixel, vertex shader
    b.u32(0); b.u32(0);                       // main light, fog mode
    b.asciiz("");                             // surface file
    b.u32(0);                                 // nRenderFlags
    b.u32(0);                                 // renderFlags
    b.u32(1);                                 // stages
    b.u32(1);                                 // texGens, independent at version > 8
    b.u32(0); b.asciiz("ca\\data\\hlava_nohq.paa"); b.u32(0); // filter, texture, stage id
    b.u32(0);                                                 // texGen uv source
    for (int i = 0; i < 12; ++i) b.f32(0.0f);                 // the 3x4 stage transform

    b.i32(0); // point-to-vertex map
    b.i32(0); // vertex-to-point map

    // --- faces: one triangle. Its stream measures 2 + 2*3 = 8 bytes. ---
    b.i32(1);
    b.u32(8);
    b.u16(0);
    b.u8(3); b.u16(0); b.u16(1); b.u16(2);

    // --- one section covering the whole face stream ---
    b.u32(1);
    b.i32(0); b.i32(8);   // face-stream byte bounds
    b.i32(0); b.i32(0);   // min bone, bone count
    b.u32(0);             // common point flags
    b.u16(0);             // texture index
    b.u32(0);
    b.i32(0);             // material index
    b.u32(0);             // section areas

    b.u32(0); // named selections
    b.u32(1); b.asciiz("class"); b.asciiz("house"); // named properties
    b.u32(withSourceMotion ? 1 : 0);
    if (withSourceMotion)
    {
        b.f32(.25f); b.u32(1); b.vec3(500, 600, 700);
    }

    b.i32(static_cast<int32_t>(0xff000000)); // colour top
    b.i32(static_cast<int32_t>(0xff000000)); // colour
    b.i32(0);                                // special
    b.boolean(false);                        // bone refs are simple
    const size_t restSizeAt = b.size();
    b.u32(0); // rest-data size, patched below
    const size_t restStart = b.size();

    // --- rest data: full-float UVs and normals, unlike every later revision ---
    b.i32(3); b.boolean(false);                 // UV set 0, not condensed
    b.f32(0.0f); b.f32(0.0f);
    b.f32(1.0f); b.f32(0.0f);
    b.f32(0.0f); b.f32(1.0f);
    b.u32(1);                                   // one UV set in total
    // Positions are a plain compressed array: count then payload, with NO
    // condensed flag. Only the UV sets, the normals and the clip flags carry one.
    b.i32(3);
    b.vec3(0.0f, 0.0f, 0.0f);
    b.vec3(1.0f, 0.0f, 0.0f);
    b.vec3(0.0f, 1.0f, 0.0f);
    b.i32(3); b.boolean(true); b.vec3(0.0f, 0.0f, 1.0f); // normals, condensed
    b.i32(0);                                            // ST coordinates
    b.i32(withSourceMotion ? 1 : 0);
    if (withSourceMotion)
    {
        b.i32(1); b.u8(0); b.u8(255); b.zeros(6); // one raw bone-reference record
    }
    b.i32(0);                                            // neighbour bone references

    // Measured on the whole A1 corpus: declared rest size is always the consumed
    // bytes plus its own four-byte field, for all 18,697 LODs.
    b.patch32(restSizeAt, static_cast<uint32_t>(b.size() - restStart) + 4);
    b.patch32(endAt, static_cast<uint32_t>(b.size()));
    return b.data;
}

} // namespace

TEST_CASE("ODOL40 source motion survives audit even when static IR discards it", "[p3d][odol][source-audit]")
{
    const auto data = arma1Model(false, true);
    QIStream stream(const_cast<char*>(data.data()), static_cast<int>(data.size()));
    BinaryReader reader(stream);
    const auto raw = P3D::ReadOdol40StaticModel(reader, static_cast<int>(data.size()));
    REQUIRE(raw.declaredLodBodiesDecoded);
    REQUIRE(raw.lods[0].keyframesObserved);
    REQUIRE(raw.lods[0].observedKeyframes == 1);
    const auto model = ODOLLoader::loadFromBuffer(data.data(), static_cast<int>(data.size()), "original_audit40.p3d");
    const auto& audit = model.sourceAudit;
    REQUIRE(audit.producerVersion == 1);
    REQUIRE(audit.sourceRevision == 40);
    REQUIRE(audit.geometryCoverage == Poseidon::Model::SourceGeometryCoverage::DeclaredLodsDecoded);
    REQUIRE(audit.declaredLods == 1);
    REQUIRE(audit.decodedLods == 1);
    REQUIRE(audit.observations == Poseidon::Model::SourceAuditAllObservations);
    REQUIRE(audit.skeletonDeclared);
    REQUIRE(audit.skeletonBones == 1);
    REQUIRE(audit.directoryHasAnimations);
    REQUIRE(audit.directoryAnimationClasses == 1);
    REQUIRE(audit.keyframeCount == 1);
    REQUIRE(audit.keyframePayloadDiscarded);
    REQUIRE(audit.vertexBoneReferenceCount == 1);
    // Audit fields must not enable animation/change the old converter behavior.
    REQUIRE(model.allowAnimation == 0);
    REQUIRE(model.lodLevels[0].mesh.frames.empty());
    auto blob = Poseidon::ModelBlob::Serialize(model);
    Poseidon::Model::Model restored;
    REQUIRE(Poseidon::ModelBlob::Deserialize(blob.data(), blob.size(), restored));
    REQUIRE(restored.sourceAudit.keyframeCount == 1);
    REQUIRE(restored.sourceAudit.keyframePayloadDiscarded);
    REQUIRE(restored.sourceAudit.skeletonBones == 1);
    REQUIRE(restored.sourceAudit.directoryAnimationClasses == 1);
    REQUIRE(restored.sourceAudit.vertexBoneReferenceCount == 1);
    REQUIRE(restored.allowAnimation == 0);
    REQUIRE(restored.lodLevels[0].mesh.frames.empty());
    blob.resize(blob.size() - Poseidon::ModelBlob::kSourceAuditFooterBytes);
    REQUIRE(Poseidon::ModelBlob::Deserialize(blob.data(), blob.size(), restored));
    REQUIRE(restored.sourceAudit.producerVersion == 0);
    REQUIRE(restored.sourceAudit.observations == 0);
    REQUIRE(restored.sourceAudit.geometryCoverage == Poseidon::Model::SourceGeometryCoverage::Unknown);
}

TEST_CASE("ODOL40 explicitly observes source motion absence without claiming final static eligibility", "[p3d][odol][source-audit]")
{
    const auto data = arma1Model();
    const auto model = ODOLLoader::loadFromBuffer(data.data(), static_cast<int>(data.size()), "original_audit40_static.p3d");
    REQUIRE(model.sourceAudit.observations == Poseidon::Model::SourceAuditAllObservations);
    REQUIRE_FALSE(model.sourceAudit.skeletonDeclared);
    REQUIRE_FALSE(model.sourceAudit.directoryHasAnimations);
    REQUIRE(model.sourceAudit.keyframeCount == 0);
    REQUIRE_FALSE(model.sourceAudit.keyframePayloadDiscarded);
    REQUIRE(model.sourceAudit.vertexBoneReferenceCount == 0);
}

TEST_CASE("Raw ODOL conversion cannot invent decoder coverage or relabel its revision", "[p3d][odol][source-audit]")
{
    const auto data = arma1Model();
    QIStream stream(const_cast<char*>(data.data()), int(data.size()));
    BinaryReader reader(stream);
    auto raw = P3D::ReadOdol40StaticModel(reader, int(data.size()));
    uint32_t targetRevision = 40;
    SECTION("caller assembled data lacks declared-body proof") { raw.declaredLodBodiesDecoded = false; }
    SECTION("decoded40 cannot become audited54") { targetRevision = 54; }
    const auto converted = ODOLLoader::convertStaticModel(raw, "original_raw_audit.p3d", targetRevision);
    REQUIRE(converted.sourceAudit.producerVersion == 0);
    REQUIRE(converted.sourceAudit.observations == 0);
    REQUIRE(converted.sourceAudit.geometryCoverage == Poseidon::Model::SourceGeometryCoverage::Unknown);
}

TEST_CASE("An ODOL 40 model closes on its declared boundary", "[p3d][odol][ast-013]")
{
    const auto   data = arma1Model();
    QIStream     stream(const_cast<char*>(data.data()), static_cast<int>(data.size()));
    BinaryReader reader(stream);
    const auto   model = P3D::ReadOdol40StaticModel(reader, static_cast<int>(data.size()));

    REQUIRE(model.directory.model.resolutions == std::vector<float>{1.0f});
    REQUIRE(model.directory.model.boundingSphere == 2.0f);
    REQUIRE(model.directory.model.bboxMax.z == 3.0f);
    REQUIRE(model.lods.size() == 1);

    const auto& lod = model.lods[0];
    REQUIRE(lod.header.vertexCount == 3);
    REQUIRE(lod.header.textures == std::vector<std::string>{"ca\\data\\hlava.paa"});
    REQUIRE(lod.materials.size() == 1);
    REQUIRE(lod.materials[0].version == 9);
    REQUIRE(lod.materials[0].name == "ca\\data\\hlava.rvmat");
    REQUIRE(lod.materials[0].stageTextures == std::vector<std::string>{"ca\\data\\hlava_nohq.paa"});
    REQUIRE(lod.materials[0].texGens.size() == 1);
    REQUIRE(lod.polygons.faces.size() == 1);
    REQUIRE(lod.polygons.faces[0] == std::vector<uint32_t>{0, 1, 2});
    // The face stream rebuilt in the section bounds' own units closes on the
    // LOD's declared total, which is what licenses reading those bounds as face
    // ranges at all.
    REQUIRE(P3D::Odol73FaceStreamOffsets(lod.polygons).back() == lod.polygons.faceDataSize);
    REQUIRE(lod.sections.size() == 1);
    REQUIRE(lod.sections[0].faceUpper == 8);
    REQUIRE(lod.namedProperties.size() == 1);
    REQUIRE(lod.namedProperties[0].first == "class");

    // Full floats, exactly as stored: no quantised UV range to expand and no
    // packed-normal scale to apply. A 16-bit UV reading would land nowhere near
    // these values, and the packed-normal decoder would invert the normal.
    REQUIRE(lod.rest.uv0.uv.size() == 3);
    REQUIRE(lod.rest.uv0.uv[1][0] == 1.0f);
    REQUIRE(lod.rest.uv0.uv[2][1] == 1.0f);
    REQUIRE(lod.rest.positions.size() == 3);
    REQUIRE(lod.rest.positions[2].y == 1.0f);
    REQUIRE(lod.rest.normals.size() == 3);
    REQUIRE(lod.rest.normals[0].z == 1.0f);
    // Clip flags come from the LOD header at this revision, not the rest data.
    REQUIRE(lod.rest.clip.size() == 3);
}

TEST_CASE("An ODOL 40 model reaches the common Poseidon model IR", "[p3d][odol][ast-013]")
{
    // The point of the work: revision 40 has to arrive in the same IR every other
    // format lands in, through the same loader entry point.
    const auto data  = arma1Model();
    const auto model = ODOLLoader::loadFromBuffer(data.data(), static_cast<int>(data.size()), "a1.p3d");

    REQUIRE(model.sourceFormat == "ODOL");
    REQUIRE(model.sourceVersion == 40);
    REQUIRE(model.lodLevels.size() == 1);

    const auto& lod = model.lodLevels[0];
    REQUIRE(lod.sourceEncoding == "ODOL40");
    REQUIRE(lod.resolution == 1.0f);
    REQUIRE(lod.mesh.vertices.size() == 3);
    REQUIRE(lod.mesh.triangles.size() == 1);
    REQUIRE(lod.mesh.quads.empty());
    REQUIRE(lod.mesh.triangles[0].materialIndex == 0);
    REQUIRE(lod.mesh.vertices[1].position.x == 1.0f);
    REQUIRE(lod.mesh.vertices[1].uv.u == 1.0f);
    REQUIRE(lod.mesh.vertices[0].normal.z == 1.0f);
    REQUIRE(lod.mesh.materials.size() == 1);
    REQUIRE(lod.mesh.materials[0].texturePath == "ca\\data\\hlava.paa");
    REQUIRE(lod.mesh.materials[0].materialPath == "ca\\data\\hlava.rvmat");
    REQUIRE(lod.mesh.sections.size() == 1);
    REQUIRE(lod.mesh.sections[0].faceRangeKnown);
    REQUIRE(lod.mesh.sections[0].faceCount == 1);
    REQUIRE(lod.mesh.properties.size() == 1);
    // The winding basis is deliberately NOT claimed here: AST-018's measurement
    // was made on revision 73 and rests on that revision's packed-normal scale
    // being negative, which revision 40 does not have.
    REQUIRE(lod.basis.windingConfidence == Poseidon::Model::BasisConfidence::Unknown);
}

TEST_CASE("An ODOL 40 model keeps its mass, armour and class name", "[p3d][odol][ast-013][COL-001]")
{
    // COL-001. The reader used to consume these fields and throw them away, so
    // every Arma 1 model arrived at mass 0 -- and Object::IsPassable() is
    // `GetMass() < 10`, so a soldier walked through it. The bytes are at exactly
    // the offsets the massless model has always closed on; only their
    // destination changed.
    const auto   data = arma1Model(/*withMass=*/true);
    QIStream     stream(const_cast<char*>(data.data()), static_cast<int>(data.size()));
    BinaryReader reader(stream);
    const auto   raw = P3D::ReadOdol40StaticModel(reader, static_cast<int>(data.size()));

    const auto& info = raw.directory.model;
    REQUIRE(info.massArray == std::vector<float>{100.0f, 200.0f, 300.0f});
    REQUIRE(info.mass == 600.0f);
    REQUIRE(info.invMass == Catch::Approx(1.0f / 600.0f));
    REQUIRE(info.armor == 50.0f);
    REQUIRE(info.invArmor == Catch::Approx(1.0f / 50.0f));
    REQUIRE(info.centerOfMass.y == 1.5f);
    REQUIRE(info.invInertia[0].x == 1.0f);
    REQUIRE(info.invInertia[1].y == 1.0f);
    REQUIRE(info.invInertia[2].z == 1.0f);
    REQUIRE(info.lodIndexTable.size() == 12);
    REQUIRE(info.propertyClass == "TestVehicle");
    REQUIRE(info.propertyDamage.empty());
    // The model still closes on its boundary: the LOD after the preamble is intact.
    REQUIRE(raw.lods.size() == 1);
    REQUIRE(raw.lods[0].polygons.faces.size() == 1);

    // ...and all of it reaches the IR through the shared entry point.
    const auto model = ODOLLoader::loadFromBuffer(data.data(), static_cast<int>(data.size()), "a1.p3d");
    REQUIRE(model.mass == 600.0f);
    REQUIRE(model.invMass == Catch::Approx(1.0f / 600.0f));
    REQUIRE(model.armor == 50.0f);
    REQUIRE(model.massArray.size() == 3);
    REQUIRE(model.centerOfMass.y == 1.5f);
    REQUIRE(model.invInertia[4] == 1.0f);
    REQUIRE(model.metadata.at("odolPropertyClass") == "TestVehicle");
    // One visual LOD and nothing else: no geometry level to point at.
    REQUIRE(model.geometryIdx == -1);
    REQUIRE(model.landContactIdx == -1);
}

TEST_CASE("A truncated ODOL 40 model is refused, naming the constraint", "[p3d][odol][ast-013]")
{
    std::vector<char> data(64, 0);
    std::memcpy(data.data(), "ODOL", 4);
    const uint32_t revision = 40;
    std::memcpy(data.data() + 4, &revision, 4);
    try
    {
        ODOLLoader::loadFromBuffer(data.data(), static_cast<int>(data.size()), "a1.p3d");
        FAIL("expected a truncated ODOL 40 model to be refused");
    }
    catch (const P3D::UnsupportedOdolRevision& error)
    {
        REQUIRE(error.version() == 40);
        REQUIRE(error.recognised());
        REQUIRE(std::string(error.what()).find("Arma 1") != std::string::npos);
    }
}

TEST_CASE("Reading an ODOL 40 model as Arma 2 desynchronises it", "[p3d][odol][ast-013]")
{
    // The guard on the whole file: the two layouts are genuinely different, so
    // routing revision 40 to the A2 reader has to fail rather than quietly
    // produce plausible geometry. Both readers are asked for the same bytes.
    const auto   data = arma1Model();
    QIStream     stream(const_cast<char*>(data.data()), static_cast<int>(data.size()));
    BinaryReader reader(stream);
    bool         desynchronised = false;
    try
    {
        const auto wrong = P3D::ReadOdolA2StaticModel(reader, static_cast<int>(data.size()), 40);
        desynchronised = wrong.lods.empty();
    }
    catch (const std::exception&)
    {
        desynchronised = true;
    }
    REQUIRE(desynchronised);
}

// --- the latent material-version bug AST-013 found in passing -----------------

namespace
{
// One embedded material at `version`, followed by a sentinel. Written the way the
// BIS reference writes it: below version 8 each stage is [transform, texture] and
// carries no stage id; from 8 it is all textures then all transforms.
std::vector<char> embeddedMaterial(uint32_t version)
{
    Bytes b;
    b.u32(1); // one material
    b.asciiz("test.rvmat");
    b.u32(version);
    for (int i = 0; i < 24; ++i) b.f32(0.0f);
    b.f32(1.0f);
    b.u32(0); b.u32(0); b.u32(0); b.u32(0);
    b.asciiz("");
    b.u32(0); b.u32(0);
    // Version 6 declares no stage count at all, so it has one stage fewer than
    // the others: zero.
    const uint32_t stages = version > 6 ? 1u : 0u;
    if (version > 6) b.u32(stages);   // stages
    if (version > 8) b.u32(stages);   // texGens, an independent count only above 8
    for (uint32_t stage = 0; stage < stages; ++stage)
    {
        if (version < 8)
        {
            b.u32(7);                                 // transform uv source
            for (int i = 0; i < 12; ++i) b.f32(0.0f); // its 3x4 matrix
            b.u32(0); b.asciiz("stage.paa");          // filter, texture -- and no stage id
        }
        else
        {
            b.u32(0); b.asciiz("stage.paa"); b.u32(0);
            if (version >= 11) b.boolean(false);
        }
    }
    if (version >= 8)
        for (uint32_t stage = 0; stage < stages; ++stage)
        {
            b.u32(7);
            for (int i = 0; i < 12; ++i) b.f32(0.0f);
        }
    if (version >= 10)
    {
        // The thermal stage texture, itself a StageTexture at the same version.
        b.u32(0); b.asciiz("ti.paa"); b.u32(0);
        if (version >= 11) b.boolean(false);
    }
    b.u8(0x7f);
    return b.data;
}
} // namespace

TEST_CASE("A material below version 8 interleaves its stages and has no stage id",
          "[p3d][odol][ast-013][latent]")
{
    // Latent, not live: no file in any local corpus is below version 9. It is
    // fixed to match the reference because a reader that silently disagrees with
    // it is a trap for whoever meets the first such file -- and the failure mode
    // would be the one AST-013 already paid for once, a plausible read four bytes
    // out of place rather than an error.
    for (const uint32_t version : {6u, 7u})
    {
        const auto   data = embeddedMaterial(version);
        QIStream     stream(const_cast<char*>(data.data()), static_cast<int>(data.size()));
        BinaryReader reader(stream);
        const auto   materials = P3D::ReadOdol73EmbeddedMaterials(reader);
        REQUIRE(materials.size() == 1);
        REQUIRE(materials[0].version == version);
        if (version == 7)
        {
            REQUIRE(materials[0].stageTextures == std::vector<std::string>{"stage.paa"});
            REQUIRE(materials[0].texGens.size() == 1);
            REQUIRE(materials[0].texGens[0].uvSource == 7);
        }
        else
        {
            // Version 6 declares no stage count at all, so it has neither.
            REQUIRE(materials[0].stageTextures.empty());
        }
        // The decisive part: the material ended exactly where it ends.
        REQUIRE(reader.read<uint8_t>() == 0x7f);
    }
}

TEST_CASE("A material at version 8 and above keeps the layout Arma 1-3 use",
          "[p3d][odol][ast-013][latent]")
{
    for (const uint32_t version : {8u, 9u, 11u})
    {
        const auto   data = embeddedMaterial(version);
        QIStream     stream(const_cast<char*>(data.data()), static_cast<int>(data.size()));
        BinaryReader reader(stream);
        const auto   materials = P3D::ReadOdol73EmbeddedMaterials(reader);
        REQUIRE(materials.size() == 1);
        REQUIRE(materials[0].stageTextures == std::vector<std::string>{"stage.paa"});
        REQUIRE(materials[0].texGens.size() == 1);
        REQUIRE(materials[0].texGens[0].uvSource == 7);
        REQUIRE(reader.read<uint8_t>() == 0x7f);
    }
}

// --- opt-in survey against real Arma 1 content --------------------------------

// The claim this file cannot make from synthetic bytes: that the reader closes on
// real Arma 1 models. Point POSEIDON_ODOL40_CORPUS at a directory of extracted
// revision-40 models and this reports the closure rate the C++ reader achieves,
// which is the figure to compare against the reference parser's 2,580 of 2,580.
TEST_CASE("ODOL 40 corpus survey reports what the reader takes", "[p3d][odol][ast-013][corpus][survey]")
{
    namespace fs = std::filesystem;
    const char* root = std::getenv("POSEIDON_ODOL40_CORPUS");
    if (!root || !*root)
        SKIP("set POSEIDON_ODOL40_CORPUS to a directory of extracted revision-40 models");
    if (!fs::exists(root))
        SKIP("POSEIDON_ODOL40_CORPUS does not exist");

    std::vector<std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (entry.is_regular_file() && entry.path().extension() == ".p3d")
            files.push_back(entry.path().string());
    std::sort(files.begin(), files.end());
    REQUIRE_FALSE(files.empty());

    size_t      loaded = 0, refused = 0, other = 0, vertices = 0;
    std::string report;
    for (const auto& file : files)
    {
        const std::string name = fs::path(file).filename().string();
        try
        {
            const auto model = ODOLLoader::load(file);
            if (model.sourceVersion != 40) { ++other; continue; }
            ++loaded;
            REQUIRE_FALSE(model.lodLevels.empty());
            for (const auto& lod : model.lodLevels) vertices += lod.mesh.vertices.size();
        }
        catch (const std::exception& error)
        {
            ++refused;
            if (report.size() < 8000) report += "  REFUSED  " + name + ": " + error.what() + "\n";
        }
    }
    WARN("ODOL 40 corpus survey (" << files.size() << " files, " << loaded << " loaded, " << refused
                                   << " refused, " << other << " other revisions, " << vertices
                                   << " vertices):\n"
                                   << report);
    REQUIRE(loaded + refused + other == files.size());
    REQUIRE(loaded > 0);
    REQUIRE(vertices > 0);
}
