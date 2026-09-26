#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Asset/Formats/P3D/ODOLLoader.hpp>
#include <Poseidon/World/Model/ModelBlob.hpp>
#include <cstring>
#include <vector>

namespace
{
struct OriginalBytes
{
    std::vector<char> data;
    void U8(uint8_t v) { data.push_back(char(v)); }
    void U16(uint16_t v) { U8(uint8_t(v)); U8(uint8_t(v >> 8)); }
    void U32(uint32_t v) { for (int i = 0; i < 4; ++i) U8(uint8_t(v >> (8 * i))); }
    void F32(float v) { uint32_t bits; std::memcpy(&bits, &v, 4); U32(bits); }
    void Point(float x, float y, float z) { F32(x); F32(y); F32(z); }
};

// Authored here: one original ODOL7 triangle and optional distant frame poses.
// No retail bytes, exporter, model cache, banks or game session are needed.
std::vector<char> OriginalOdol7(bool motion)
{
    OriginalBytes b;
    b.data = {'O', 'D', 'O', 'L'};
    b.U32(7); b.U32(1);
    b.U32(3); b.U32(0); b.U32(0); b.U32(0); // clip flags
    b.U32(3); for (int i = 0; i < 6; ++i) b.F32(0); // UV
    b.U32(3); b.Point(0, 0, 0); b.Point(1, 0, 0); b.Point(0, 1, 0);
    b.U32(3); for (int i = 0; i < 3; ++i) b.Point(0, 0, 1); // normals
    b.U32(0); b.U32(0); // LOD hints
    b.Point(0, 0, 0); b.Point(1, 1, 0); b.Point(.5f, .5f, 0); b.F32(1);
    b.U32(0); // texture paths
    b.U32(0); b.U32(0); // edge maps
    b.U32(1); b.U32(0); // face count, section offset
    b.U32(0); b.U16(0xffff); b.U8(3); b.U16(0); b.U16(1); b.U16(2);
    b.U32(0); b.U32(0); b.U32(0); // sections, selections, properties
    b.U32(motion ? 1 : 0);
    if (motion)
    {
        b.F32(.25f); b.U32(3);
        b.Point(100, 200, 300); b.Point(101, 200, 300); b.Point(100, 201, 300);
    }
    b.U32(0); b.U32(0); b.U32(0); // LOD end colors/special
    b.U32(0); // proxies
    b.F32(1); // resolution
    b.U32(0); b.F32(1); b.F32(1); // model special/spheres
    b.U32(0); b.U32(0); b.U32(0); // remarks/hints
    b.Point(0, 0, 0); b.U32(0); b.U32(0); b.F32(0); // aiming/colors/view density
    b.Point(0, 0, 0); b.Point(1, 1, 0); // model bounds
    b.Point(0, 0, 0); b.Point(0, 0, 0); b.Point(0, 0, 0); // centers
    for (int i = 0; i < 9; ++i) b.F32(0); // inverse inertia
    b.U8(0); b.U8(0); b.U8(0); b.U8(0); b.U8(motion ? 1 : 0); b.U8(0);
    b.U32(0); // mass array
    for (int i = 0; i < 4; ++i) b.F32(0); // totals
    for (int i = 0; i < 12; ++i) b.U8(0xff); // role indices
    return b.data;
}

Poseidon::Model::Model OriginalModel(bool motion)
{
    const auto bytes = OriginalOdol7(motion);
    return Poseidon::Asset::Formats::ODOLLoader::loadFromBuffer(bytes.data(), int(bytes.size()), "original_source_audit7.p3d");
}
void PatchWord(std::vector<uint8_t>& bytes, size_t offset, uint32_t value)
{ for (int i = 0; i < 4; ++i) bytes[offset + size_t(i)] = uint8_t(value >> (8 * i)); }
}

TEST_CASE("Original ODOL7 source audit distinguishes retained frames from format-defined absent skeleton fields", "[source-audit]")
{
    const auto model = OriginalModel(true);
    const auto& audit = model.sourceAudit;
    REQUIRE(audit.producerVersion == 1);
    REQUIRE(audit.sourceRevision == 7);
    REQUIRE(audit.geometryCoverage == Poseidon::Model::SourceGeometryCoverage::DeclaredLodsDecoded);
    REQUIRE(audit.declaredLods == 1);
    REQUIRE(audit.decodedLods == 1);
    REQUIRE(audit.observations == Poseidon::Model::SourceAuditAllObservations);
    REQUIRE(audit.directoryHasAnimations);
    REQUIRE(audit.keyframeCount == 1);
    REQUIRE_FALSE(audit.keyframePayloadDiscarded);
    REQUIRE_FALSE(audit.skeletonDeclared);
    REQUIRE(audit.skeletonBones == 0);
    REQUIRE(audit.vertexBoneReferenceCount == 0);
    REQUIRE(model.allowAnimation == 1);
    REQUIRE(model.lodLevels[0].mesh.frames.size() == 1);
    REQUIRE(model.lodLevels[0].mesh.frames[0].positions[0].x == 100);
    const auto staticModel = OriginalModel(false);
    REQUIRE(staticModel.sourceAudit.observations == Poseidon::Model::SourceAuditAllObservations);
    REQUIRE(staticModel.sourceAudit.keyframeCount == 0);
    REQUIRE_FALSE(staticModel.sourceAudit.directoryHasAnimations);
}

TEST_CASE("New model blobs retain source audit while exact legacy base blobs remain unaudited", "[source-audit][model-blob]")
{
    const auto model = OriginalModel(true);
    auto bytes = Poseidon::ModelBlob::Serialize(model);
    Poseidon::Model::Model restored;
    REQUIRE(Poseidon::ModelBlob::Deserialize(bytes.data(), bytes.size(), restored));
    REQUIRE(restored.sourceAudit.sourceRevision == 7);
    REQUIRE(restored.sourceAudit.observations == Poseidon::Model::SourceAuditAllObservations);
    REQUIRE(restored.sourceAudit.keyframeCount == 1);
    REQUIRE(restored.sourceAudit.directoryHasAnimations);
    REQUIRE(restored.lodLevels[0].mesh.frames[0].positions[0].x == 100);
    // This is the unchanged exact v1 base, as written before the optional audit.
    // Its existing IR animation data remains readable but supplies no audit.
    bytes.resize(bytes.size() - Poseidon::ModelBlob::kSourceAuditFooterBytes);
    REQUIRE(Poseidon::ModelBlob::Deserialize(bytes.data(), bytes.size(), restored));
    REQUIRE(restored.sourceAudit.producerVersion == 0);
    REQUIRE(restored.sourceAudit.observations == 0);
    REQUIRE(restored.sourceAudit.geometryCoverage == Poseidon::Model::SourceGeometryCoverage::Unknown);
    REQUIRE(restored.allowAnimation == 1);
    REQUIRE(restored.lodLevels[0].mesh.frames[0].positions[0].x == 100);
}

TEST_CASE("Model blob audit rejects invalid provenance, enums and contradictory source counts", "[source-audit][model-blob]")
{
    auto model = OriginalModel(true);
    SECTION("future producer") { model.sourceAudit.producerVersion = 2; }
    SECTION("source revision differs from IR") { model.sourceAudit.sourceRevision = 54; }
    SECTION("wrong container cannot reuse ODOL audit") { model.sourceFormat = "MLOD"; }
    SECTION("unknown coverage enum") { model.sourceAudit.geometryCoverage = Poseidon::Model::SourceGeometryCoverage(9); }
    SECTION("unknown observation bits") { model.sourceAudit.observations |= 0x80; }
    SECTION("LOD count exceeds decoder domain") { model.sourceAudit.declaredLods = 101; }
    SECTION("decoded count differs from geometry") { model.sourceAudit.decodedLods = 0; }
    SECTION("motion count lacks its observed fact") { model.sourceAudit.observations &= ~uint32_t(Poseidon::Model::SourceAuditObservation::Keyframes); }
    SECTION("skeleton count without declaration") { model.sourceAudit.skeletonBones = 1; }
    SECTION("discard flag without source frames") { model.sourceAudit.keyframeCount = 0; model.sourceAudit.keyframePayloadDiscarded = true; }
    SECTION("unknown producer cannot carry known facts") { model.sourceAudit.producerVersion = 0; }
    const auto bytes = Poseidon::ModelBlob::Serialize(model);
    Poseidon::Model::Model restored;
    REQUIRE_FALSE(Poseidon::ModelBlob::Deserialize(bytes.data(), bytes.size(), restored));
}

TEST_CASE("Model blob audit footer rejects malformed lengths and trailing data instead of treating it as old absence", "[source-audit][model-blob]")
{
    auto bytes = Poseidon::ModelBlob::Serialize(OriginalModel(false));
    const auto footer = bytes.size() - Poseidon::ModelBlob::kSourceAuditFooterBytes;
    SECTION("tag") { PatchWord(bytes, footer, 0); }
    SECTION("future footer version") { PatchWord(bytes, footer + 4, 2); }
    SECTION("declared payload length") { PatchWord(bytes, footer + 8, 0xffffffff); }
    SECTION("unknown flag bits") { PatchWord(bytes, bytes.size() - 4, 0x80); }
    SECTION("truncated footer") { bytes.pop_back(); }
    SECTION("trailing garbage") { bytes.push_back(0); }
    SECTION("old base with incomplete audit prefix") { bytes.resize(footer + 4); }
    Poseidon::Model::Model restored;
    REQUIRE_FALSE(Poseidon::ModelBlob::Deserialize(bytes.data(), bytes.size(), restored));
}

TEST_CASE("A2 keyframe source counts remain observed despite intentional position skipping", "[source-audit][p3d]")
{
    OriginalBytes b;
    b.U32(1); b.F32(.75f); b.U32(2); b.Point(10, 20, 30); b.Point(40, 50, 60);
    QIStream stream(b.data.data(), int(b.data.size()));
    Poseidon::Asset::Formats::BinaryReader reader(stream);
    REQUIRE(Poseidon::Asset::Formats::P3D::ReadOdol49Keyframes(reader, uint32_t(b.data.size())) == 1);
    REQUIRE(reader.tell() == int(b.data.size()));
}

TEST_CASE("Revision73 continues to refuse authored keyframes rather than claim their absence", "[source-audit][p3d]")
{
    OriginalBytes b;
    b.U32(1);
    QIStream stream(b.data.data(), int(b.data.size()));
    Poseidon::Asset::Formats::BinaryReader reader(stream);
    REQUIRE_THROWS(Poseidon::Asset::Formats::P3D::ReadOdol73StaticPreRestData(reader, uint32_t(b.data.size())));
}
