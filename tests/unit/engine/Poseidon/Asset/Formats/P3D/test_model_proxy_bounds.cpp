// test_model_proxy_bounds.cpp -- per-model far-field proxy data from the P3D
// header alone.
//
// The property under test is as much about what is NOT read as about what is. A
// world names a few hundred distinct models and places them millions of times, so
// the far-field tier can afford a header read per model and nothing more. These
// cases pin the two halves of that: the header fields come out right for every
// revision whose ModelInfo is at the front of the file, and everything else --
// OFP-era ODOL 7 whose ModelInfo is at the BACK, MLOD, garbage, and every
// truncation of a good header -- comes back refused rather than parsed.
//
// The positive cases are built byte by byte rather than shipped as fixtures, for
// the reason test_odol40_arma1.cpp gives: the repository holds no revision-40,
// -49 or -73 model, because Arma 1/2/3 data is licensed content that does not
// live here. What a synthetic header can state is exactly what is at stake --
// which field sits at which offset, and where the reader stops. The real
// fixtures carry the refusals, which are the cases they can state.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <Poseidon/Asset/Formats/P3D/ModelProxyBounds.hpp>
#include <Poseidon/Asset/Formats/P3D/Odol40.hpp>
#include <Poseidon/Asset/Formats/P3D/Odol49.hpp>
#include <Poseidon/Asset/Formats/P3D/Odol73.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include "../../../test_fixtures.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace P3D = Poseidon::Asset::Formats::P3D;
using Poseidon::Asset::Formats::BinaryReader;

namespace
{

// A little-endian byte builder, as in the sibling ODOL tests: the subject here is
// which bytes are on the wire and in what order.
struct Bytes
{
    std::vector<char> data;

    void u8(uint8_t value) { data.push_back(static_cast<char>(value)); }
    void u32(uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
            u8(static_cast<uint8_t>(value >> (8 * i)));
    }
    void i32(int32_t value) { u32(static_cast<uint32_t>(value)); }
    void f32(float value)
    {
        uint32_t raw;
        std::memcpy(&raw, &value, 4);
        u32(raw);
    }
    void vec3(float x, float y, float z)
    {
        f32(x);
        f32(y);
        f32(z);
    }
    void asciiz(const char* text)
    {
        while (*text)
            u8(static_cast<uint8_t>(*text++));
        u8(0);
    }
    void tag(const char* text)
    {
        while (*text)
            u8(static_cast<uint8_t>(*text++));
    }
    int size() const { return static_cast<int>(data.size()); }
};

// The part of ModelInfo every supported revision shares, from the LOD-resolution
// table through bboxMax. Nothing after bboxMax is written: the reader must not
// need it, which is the whole point of stopping there.
void appendSharedModelInfo(Bytes& bytes)
{
    bytes.u32(2);    // LOD count
    bytes.f32(1.0f); // resolutions
    bytes.f32(10000.0f);
    bytes.i32(512);                  // special
    bytes.f32(12.5f);                // bounding sphere
    bytes.f32(9.0f);                 // geometry sphere
    bytes.i32(4);                    // remarks
    bytes.i32(5);                    // and hints
    bytes.i32(6);                    // or hints
    bytes.vec3(0.0f, 1.0f, 0.0f);    // aiming centre
    bytes.u32(0x11223344);           // colour
    bytes.u32(0x55667788);           // colour type
    bytes.f32(0.5f);                 // view density
    bytes.vec3(-2.0f, -1.0f, -3.0f); // bbox min
    bytes.vec3(2.0f, 4.0f, 3.0f);    // bbox max
}

std::vector<char> odol73Header()
{
    Bytes bytes;
    bytes.tag("ODOL");
    bytes.u32(73);
    bytes.u32(107410); // application id -- revision 73 only
    bytes.asciiz("");  // muzzle flash    -- revision 73 only
    appendSharedModelInfo(bytes);
    return bytes.data;
}

std::vector<char> odolFamilyHeader(uint32_t revision)
{
    Bytes bytes;
    bytes.tag("ODOL");
    bytes.u32(revision);
    // No application id and no muzzle-flash string at these revisions: the
    // resolution table follows the version word directly.
    appendSharedModelInfo(bytes);
    return bytes.data;
}

std::vector<char> readFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    REQUIRE(file.good());
    const auto size = file.tellg();
    std::vector<char> bytes(static_cast<size_t>(size));
    file.seekg(0);
    file.read(bytes.data(), size);
    return bytes;
}

} // namespace

TEST_CASE("Far-field proxy bounds read an ODOL 73 header", "[p3d][odol][farfield]")
{
    const auto data = odol73Header();
    const auto bounds = P3D::ReadModelProxyBounds(data.data(), static_cast<int>(data.size()));

    REQUIRE(bounds.valid);
    REQUIRE(bounds.radius == Catch::Approx(12.5f));
    REQUIRE(bounds.radius > 0.0f);
    REQUIRE(bounds.minY == Catch::Approx(-1.0f));
    REQUIRE(bounds.maxY == Catch::Approx(4.0f));
    REQUIRE(bounds.maxY > bounds.minY);
    // max(x extent 4, z extent 6). Taking the x extent alone would under-size a
    // billboard for every model that is longer than it is wide.
    REQUIRE(bounds.horizontalExtent == Catch::Approx(6.0f));
    REQUIRE(bounds.color == 0x11223344u);
}

TEST_CASE("Far-field proxy bounds read every A1/A2-family revision", "[p3d][odol][farfield]")
{
    // Revisions 40, 48-52 and 54 share this prefix byte for byte; the layouts
    // diverge only after bboxMax. One reader covers the family, and this states
    // that claim revision by revision rather than on the one that was tried.
    const uint32_t revision =
        GENERATE(uint32_t{40}, uint32_t{48}, uint32_t{49}, uint32_t{50}, uint32_t{52}, uint32_t{54});
    CAPTURE(revision);

    const auto data = odolFamilyHeader(revision);
    const auto bounds = P3D::ReadModelProxyBounds(data.data(), static_cast<int>(data.size()));

    REQUIRE(bounds.valid);
    REQUIRE(bounds.radius == Catch::Approx(12.5f));
    REQUIRE(bounds.minY == Catch::Approx(-1.0f));
    REQUIRE(bounds.maxY == Catch::Approx(4.0f));
    REQUIRE(bounds.horizontalExtent == Catch::Approx(6.0f));
    REQUIRE(bounds.color == 0x11223344u);
}

TEST_CASE("An A1/A2 header is not read as revision 73", "[p3d][odol][farfield]")
{
    // Revision 73's extra application id and muzzle-flash string are what makes
    // this a real distinction: feeding a family header to the v73 reader would
    // shift every field by five bytes and still produce numbers.
    auto data = odolFamilyHeader(49);
    // Same bytes, relabelled. The resolution count now lands where v73 expects an
    // application id, so nothing after it can line up.
    const uint32_t seventyThree = 73;
    std::memcpy(data.data() + 4, &seventyThree, 4);
    const auto bounds = P3D::ReadModelProxyBounds(data.data(), static_cast<int>(data.size()));
    REQUIRE_FALSE(bounds.valid);
}

TEST_CASE("OFP-era ODOL 7 is refused, not parsed", "[p3d][odol][farfield]")
{
    // Revision 7 keeps ModelInfo at the END of the file, behind every LOD body.
    // Reading it is a full parse, not a header read, so the cheap path must say
    // so instead of quietly paying for one.
    const auto data = readFile(GET_FIXTURE("p3d/animated_morph_odol.p3d"));
    const auto bounds = P3D::ReadModelProxyBounds(data.data(), static_cast<int>(data.size()));
    REQUIRE_FALSE(bounds.valid);
    REQUIRE(bounds.radius == 0.0f);
}

TEST_CASE("An MLOD model is refused", "[p3d][odol][farfield]")
{
    const auto data = readFile(GET_FIXTURE("p3d/simple_tree.p3d"));
    const auto bounds = P3D::ReadModelProxyBounds(data.data(), static_cast<int>(data.size()));
    REQUIRE_FALSE(bounds.valid);
}

TEST_CASE("Garbage and truncation are refused without reading past the input", "[p3d][odol][farfield]")
{
    SECTION("an empty or null input")
    {
        REQUIRE_FALSE(P3D::ReadModelProxyBounds(nullptr, 0).valid);
        REQUIRE_FALSE(P3D::ReadModelProxyBounds(nullptr, 64).valid);
        const char nothing = 0;
        REQUIRE_FALSE(P3D::ReadModelProxyBounds(&nothing, 0).valid);
    }

    SECTION("bytes that are not a P3D at all")
    {
        std::vector<char> junk(256);
        for (size_t i = 0; i < junk.size(); ++i)
            junk[i] = static_cast<char>(i * 37 + 11);
        REQUIRE_FALSE(P3D::ReadModelProxyBounds(junk.data(), static_cast<int>(junk.size())).valid);
    }

    SECTION("every truncation of a good header")
    {
        // The header is copied into an exactly-sized heap buffer at each length so
        // an over-read is a real over-read, not a walk into slack the test itself
        // supplied. A sanitizer build has something to catch.
        for (const auto& full : {odol73Header(), odolFamilyHeader(49)})
            for (int length = 0; length < static_cast<int>(full.size()); ++length)
            {
                CAPTURE(length);
                std::vector<char> truncated(full.begin(), full.begin() + length);
                REQUIRE_FALSE(P3D::ReadModelProxyBounds(truncated.data(), length).valid);
            }
    }

    SECTION("a header whose bbox decodes to a non-number")
    {
        // A run of plausible bytes can decode to NaN. An impostor sized from NaN
        // is worse than none at all, so a header that parsed is still refused if
        // what it says is not a number.
        auto data = odol73Header();
        const uint32_t quietNaN = 0x7fc00000u;
        std::memcpy(data.data() + data.size() - 4, &quietNaN, 4); // bbox max z
        REQUIRE_FALSE(P3D::ReadModelProxyBounds(data.data(), static_cast<int>(data.size())).valid);
    }

    SECTION("a header whose bounding box is inverted")
    {
        auto data = odol73Header();
        const float low = -9.0f;
        std::memcpy(data.data() + data.size() - 8, &low, 4); // bbox max y, below bbox min y
        REQUIRE_FALSE(P3D::ReadModelProxyBounds(data.data(), static_cast<int>(data.size())).valid);
    }
}

TEST_CASE("Peeking proxy bounds leaves the reader where it was found", "[p3d][odol][farfield]")
{
    // The reader overload exists so a caller can take proxy bounds off a stream it
    // is about to parse properly. That only holds if both the cursor and the fail
    // bit survive -- including on the refusal path, which is reached by throwing.
    auto accepted = odol73Header();
    auto refused = readFile(GET_FIXTURE("p3d/simple_tree.p3d"));

    for (auto* data : {&accepted, &refused})
    {
        QIStream stream(data->data(), static_cast<int>(data->size()));
        BinaryReader reader(stream);
        reader.seek(0);
        const int before = reader.tell();
        (void)P3D::ReadModelProxyBounds(reader);
        REQUIRE(reader.tell() == before);
        REQUIRE_FALSE(reader.fail());
        // Usable afterwards, not merely repositioned.
        char signature[4] = {};
        reader.readBytes(signature, sizeof(signature));
        REQUIRE(std::string(signature, 4) == std::string(data->data(), 4));
    }
}

TEST_CASE("Proxy bounds off a real corpus model", "[p3d][odol][farfield][corpus]")
{
    // Opt-in, for the same reason the sibling revision-73 corpus case is: the
    // model is licensed content that cannot live in the repository. Point
    // POSEIDON_ODOL73_FIXTURE at any extracted ODOL 40/48-54/73 model.
    const char* path = std::getenv("POSEIDON_ODOL73_FIXTURE");
    if (path == nullptr || *path == '\0')
        SKIP("set POSEIDON_ODOL73_FIXTURE to an extracted later-revision ODOL model");

    const auto data = readFile(path);
    const auto bounds = P3D::ReadModelProxyBounds(data.data(), static_cast<int>(data.size()));
    CAPTURE(bounds.radius, bounds.minY, bounds.maxY, bounds.horizontalExtent);
    REQUIRE(bounds.valid);
    REQUIRE(bounds.radius > 0.0f);
    REQUIRE(bounds.maxY > bounds.minY);

    // A second witness, and the only thing that makes the above more than "some
    // floats came out". The header readers in Odol40/49/73 are the ones whose
    // layout was fitted against whole corpora, and they reach the same fields by
    // continuing all the way to the LOD byte-offset table -- so if the cheap path
    // has any field off by a byte, these disagree. Plausible numbers alone would
    // not have caught it: an ODOL header is dense enough that a shifted read
    // still yields finite, ordinary-looking extents.
    QIStream stream(data.data(), static_cast<int>(data.size()));
    BinaryReader reader(stream);
    const uint32_t revision = P3D::PeekOdolRevision(reader).version;
    const int size = static_cast<int>(data.size());
    P3D::Odol73Preamble witness;
    CAPTURE(revision);
    if (revision == 73)
        witness = P3D::ReadOdol73StaticDirectory(reader, size).model;
    else if (revision == 40)
        witness = P3D::ReadOdol40StaticDirectory(reader, size).model;
    else
        witness = P3D::ReadOdol49StaticDirectory(reader, size, revision).model;

    REQUIRE(bounds.radius == witness.boundingSphere);
    REQUIRE(bounds.minY == witness.bboxMin.y);
    REQUIRE(bounds.maxY == witness.bboxMax.y);
    REQUIRE(bounds.color == witness.color);
    REQUIRE(bounds.horizontalExtent ==
            std::max(witness.bboxMax.x - witness.bboxMin.x, witness.bboxMax.z - witness.bboxMin.z));
}
