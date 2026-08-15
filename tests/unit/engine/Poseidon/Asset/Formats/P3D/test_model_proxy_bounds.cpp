// test_model_proxy_bounds.cpp -- per-model far-field proxy data from the P3D
// header alone.
//
// The property under test is as much about what is NOT read as about what is. A
// world names a few hundred distinct models and places them millions of times, so
// the far-field tier can afford a header read per model and nothing more. These
// cases pin the two halves of that: the header fields come out right for every
// revision whose ModelInfo is at the front of the file, and everything else --
// OFP-era ODOL 7 whose ModelInfo is at the BACK, garbage, and every truncation of
// a good header -- comes back refused rather than parsed.
//
// MLOD is the exception the far tier could not afford. It carries no ModelInfo,
// no bounding sphere and no colour anywhere on the wire, so bounds must come from
// geometry -- and refusing it refused every imported Reforger world, whose models
// are MLOD 1.1 / P3DM 28.256 without exception. The MLOD cases below pin the
// FIRST-LOD vertex walk and its boundary: it reads one vertex block, it derives
// the same quantities ODOL states, it does not claim a colour it never saw, and a
// truncated or empty MLOD refuses instead of reading past the input.
//
// The ODOL positive cases are built byte by byte rather than shipped as fixtures,
// for the reason test_odol40_arma1.cpp gives: the repository holds no revision-40,
// -49 or -73 model, because Arma 1/2/3 data is licensed content that does not
// live here. What a synthetic header can state is exactly what is at stake --
// which field sits at which offset, and where the reader stops. MLOD is the other
// way round: the repository's own fixtures ARE MLOD, so the SP3X encoding is
// tested against real files and only P3DM -- which no fixture uses, and which
// every model in the corpus that motivated this uses -- is synthesised.

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
#include <cmath>
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

// An MLOD file header: "MLOD", major.minor as two bytes, a padding word, and the
// LOD count. Twelve bytes, and then a LOD body starts -- there is no ModelInfo to
// follow it, which is the whole reason the MLOD path has to read geometry.
void appendMlodFileHeader(Bytes& bytes, uint32_t lodCount)
{
    bytes.tag("MLOD");
    bytes.u8(1); // version major
    bytes.u8(1); // version minor
    bytes.u8(0); // padding
    bytes.u8(0);
    bytes.u32(lodCount);
}

struct Point
{
    float x, y, z;
};

// A P3DM LOD carrying nothing but its header and its vertex block.
//
// Synthesised because no fixture in the repository is P3DM and every model in the
// corpus that motivated this is: the Reforger imports are P3DM 28.256 without
// exception. What the bytes state is the layout claim -- 28 fixed header bytes
// with no headSize, the version gate at major 28 / minor 256, and vertex records
// of three floats plus a flags word immediately after.
std::vector<char> p3dmModel(const std::vector<Point>& points, int32_t majorVersion = 28, int32_t minorVersion = 256)
{
    Bytes bytes;
    appendMlodFileHeader(bytes, 1);
    bytes.tag("P3DM");
    bytes.i32(majorVersion);
    bytes.i32(minorVersion);
    bytes.i32(static_cast<int32_t>(points.size())); // nPos
    bytes.i32(7);                                   // nNorm -- independent of nPos, and never read here
    bytes.i32(3);                                   // nFace -- never read here either
    bytes.i32(0);                                   // flags
    for (const Point& point : points)
    {
        bytes.vec3(point.x, point.y, point.z);
        bytes.i32(0x10); // point clip flags
    }
    // Whatever a real file puts after the vertex block -- normals, faces, TAGGs, a
    // second LOD. None of it may be needed, so it is deliberately not written.
    return bytes.data;
}

// The same, in the OFP-era encoding, with a headSize larger than the 28 bytes the
// struct covers. No shipped fixture has one (all are exactly 28), so this is the
// only statement that the trailing-header skip lands on the vertex block.
std::vector<char> sp3xModel(const std::vector<Point>& points, int32_t headSize = 40)
{
    Bytes bytes;
    appendMlodFileHeader(bytes, 1);
    bytes.tag("SP3X");
    bytes.i32(headSize);
    bytes.i32(1); // version
    bytes.i32(static_cast<int32_t>(points.size()));
    bytes.i32(0); // nNorm
    bytes.i32(0); // nFace
    bytes.i32(0); // flags
    for (int32_t i = 28; i < headSize; ++i)
        bytes.u8(0xCD); // header bytes past the struct, which must be skipped not read
    for (const Point& point : points)
    {
        bytes.vec3(point.x, point.y, point.z);
        bytes.i32(0);
    }
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
    // ODOL states a colour, so this one is a reading and not a default.
    REQUIRE(bounds.colorKnown);
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

TEST_CASE("A real MLOD fixture yields bounds from its first LOD", "[p3d][mlod][farfield]")
{
    // simple_tree.p3d is MLOD 1.1 / SP3X, four points spanning x [-1, 1],
    // y [-1, 1], z [0, 0]. Those numbers are read off the file, not chosen: this
    // states that the vertex walk reaches the right block, not merely that some
    // floats came out.
    const auto data = readFile(GET_FIXTURE("p3d/simple_tree.p3d"));
    const auto bounds = P3D::ReadModelProxyBounds(data.data(), static_cast<int>(data.size()));

    REQUIRE(bounds.valid);
    REQUIRE(bounds.radius > 0.0f);
    REQUIRE(bounds.maxY > bounds.minY);
    REQUIRE(bounds.minY == Catch::Approx(-1.0f));
    REQUIRE(bounds.maxY == Catch::Approx(1.0f));
    // max(x extent 2, z extent 0).
    REQUIRE(bounds.horizontalExtent == Catch::Approx(2.0f));
    // The bounding sphere about the model origin, which is what ODOL's
    // boundingSphere means: the farthest point is a corner at (+-1, +-1, 0).
    REQUIRE(bounds.radius == Catch::Approx(std::sqrt(2.0f)));
    // MLOD has no average colour anywhere. The default is kept and flagged as a
    // default, so a caller cannot mistake it for a model that is really white.
    REQUIRE_FALSE(bounds.colorKnown);
    REQUIRE(bounds.color == 0xFFFFFFFFu);
}

TEST_CASE("An MLOD horizontal extent takes the larger of the two ground axes", "[p3d][mlod][farfield]")
{
    // multi_lod_vehicle.p3d spans x [-1, 1] but z [-1, 1.8], so a reader that took
    // the x extent alone would under-size this proxy by 40%. It also has three
    // LODs, which is the case that says only the first one is read: the later two
    // are never reached, and a walk that ran into them would not stop cleanly.
    const auto data = readFile(GET_FIXTURE("p3d/multi_lod_vehicle.p3d"));
    const auto bounds = P3D::ReadModelProxyBounds(data.data(), static_cast<int>(data.size()));

    REQUIRE(bounds.valid);
    REQUIRE(bounds.minY == Catch::Approx(-1.0f));
    REQUIRE(bounds.maxY == Catch::Approx(1.0f));
    REQUIRE(bounds.horizontalExtent == Catch::Approx(2.8f));
    REQUIRE(bounds.radius == Catch::Approx(1.8f));
}

TEST_CASE("Every MLOD fixture in the repository resolves", "[p3d][mlod][farfield]")
{
    // The denominator, not a sample. Refusing MLOD cost the far tier every model
    // of an imported world at once; a per-file spot check is exactly the shape of
    // test that would not have noticed.
    const char* names[] = {"p3d/animated_actor.p3d",  "p3d/complex_vehicle.p3d", "p3d/crew_proxy.p3d",
                           "p3d/flat_quad.p3d",       "p3d/light_disc.p3d",      "p3d/multi_lod_vehicle.p3d",
                           "p3d/proxy_structure.p3d", "p3d/simple_proxy.p3d",    "p3d/simple_tree.p3d",
                           "p3d/sky_plane.p3d"};
    for (const char* name : names)
    {
        CAPTURE(name);
        const auto data = readFile(GET_FIXTURE(name));
        const auto bounds = P3D::ReadModelProxyBounds(data.data(), static_cast<int>(data.size()));
        REQUIRE(bounds.valid);
        REQUIRE(bounds.radius > 0.0f);
        REQUIRE(bounds.maxY >= bounds.minY);
        REQUIRE_FALSE(bounds.colorKnown);
    }
}

TEST_CASE("A P3DM MLOD LOD is read", "[p3d][mlod][farfield]")
{
    // The encoding no fixture in this repository uses and every model in the
    // corpus that motivated this does.
    const auto data = p3dmModel({{-3.0f, 0.5f, -1.0f}, {4.0f, 6.5f, 2.0f}, {0.0f, 2.0f, 9.0f}});
    const auto bounds = P3D::ReadModelProxyBounds(data.data(), static_cast<int>(data.size()));

    REQUIRE(bounds.valid);
    REQUIRE(bounds.minY == Catch::Approx(0.5f));
    REQUIRE(bounds.maxY == Catch::Approx(6.5f));
    // max(x extent 7, z extent 10).
    REQUIRE(bounds.horizontalExtent == Catch::Approx(10.0f));
    REQUIRE(bounds.radius == Catch::Approx(std::sqrt(0.0f + 4.0f + 81.0f)));
    REQUIRE_FALSE(bounds.colorKnown);
}

TEST_CASE("An SP3X headSize past the struct is skipped, not read as vertices", "[p3d][mlod][farfield]")
{
    // headSize covers header bytes the struct does not name. Reading them as the
    // start of the vertex block would put 0xCDCDCDCD -- a large negative float --
    // into the bounds and still produce a plausible-looking number.
    const auto data = sp3xModel({{-1.0f, 0.0f, -1.0f}, {1.0f, 3.0f, 1.0f}}, 40);
    const auto bounds = P3D::ReadModelProxyBounds(data.data(), static_cast<int>(data.size()));

    REQUIRE(bounds.valid);
    REQUIRE(bounds.minY == Catch::Approx(0.0f));
    REQUIRE(bounds.maxY == Catch::Approx(3.0f));
    REQUIRE(bounds.horizontalExtent == Catch::Approx(2.0f));
}

TEST_CASE("Malformed MLOD is refused without reading past the input", "[p3d][mlod][farfield]")
{
    SECTION("an MLOD whose first LOD has no vertices")
    {
        // empty_shape.p3d is a real MLOD with nPos == 0. There is no geometry to
        // derive bounds from, and a zero-sized proxy is not an answer.
        const auto data = readFile(GET_FIXTURE("p3d/empty_shape.p3d"));
        REQUIRE_FALSE(P3D::ReadModelProxyBounds(data.data(), static_cast<int>(data.size())).valid);
    }

    SECTION("every truncation into a real MLOD fixture's vertex block")
    {
        // Copied into an exactly-sized heap buffer at each length, as the ODOL
        // truncation case is, so an over-read is a real over-read and a sanitizer
        // build has something to catch.
        //
        // The boundary is 104 bytes: a 12-byte MLOD file header, a 28-byte SP3X
        // header, and simple_tree.p3d's four 16-byte points. Everything after that
        // -- normals, faces, TAGGs, 581 further bytes of it -- the reader must
        // never need, so the prefix is asserted to be ENOUGH as well as the
        // shortfalls to be refused. Requiring refusal at every length would only
        // have passed by accident of the reader being greedier than it claims.
        const auto full = readFile(GET_FIXTURE("p3d/simple_tree.p3d"));
        constexpr int needed = 12 + 28 + 4 * 16;
        REQUIRE(static_cast<int>(full.size()) > needed);
        for (int length = 0; length < needed; ++length)
        {
            CAPTURE(length);
            std::vector<char> truncated(full.begin(), full.begin() + length);
            REQUIRE_FALSE(P3D::ReadModelProxyBounds(truncated.data(), length).valid);
        }
        std::vector<char> exact(full.begin(), full.begin() + needed);
        const auto bounds = P3D::ReadModelProxyBounds(exact.data(), needed);
        REQUIRE(bounds.valid);
        REQUIRE(bounds.horizontalExtent == Catch::Approx(2.0f));
    }

    SECTION("every truncation of a synthetic P3DM model")
    {
        const auto full = p3dmModel({{-3.0f, 0.5f, -1.0f}, {4.0f, 6.5f, 2.0f}, {0.0f, 2.0f, 9.0f}});
        for (int length = 0; length < static_cast<int>(full.size()); ++length)
        {
            CAPTURE(length);
            std::vector<char> truncated(full.begin(), full.begin() + length);
            REQUIRE_FALSE(P3D::ReadModelProxyBounds(truncated.data(), length).valid);
        }
    }

    SECTION("an MLOD signature over garbage")
    {
        // The signature alone must not buy a parse: everything after it here is
        // noise, and a vertex count read out of noise is what a bounds check is
        // for.
        std::vector<char> junk(512);
        for (size_t i = 0; i < junk.size(); ++i)
            junk[i] = static_cast<char>(i * 37 + 11);
        std::memcpy(junk.data(), "MLOD", 4);
        REQUIRE_FALSE(P3D::ReadModelProxyBounds(junk.data(), static_cast<int>(junk.size())).valid);
    }

    SECTION("a vertex count larger than the file can hold")
    {
        // nPos overwritten with a count no input could back. Without the stride
        // check this reserves and reads until the stream gives out.
        auto data = p3dmModel({{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}});
        const int32_t huge = 0x0FFFFFFF;
        std::memcpy(data.data() + 24, &huge, 4); // nPos: 12 file header + 12 into the P3DM header
        REQUIRE_FALSE(P3D::ReadModelProxyBounds(data.data(), static_cast<int>(data.size())).valid);
    }

    SECTION("a P3DM version the layout is not fixed for")
    {
        // There is no headSize in P3DM, so a different version is not a header to
        // skip past -- it is a layout this reader cannot locate the vertex block in.
        const auto data = p3dmModel({{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}}, 28, 255);
        REQUIRE_FALSE(P3D::ReadModelProxyBounds(data.data(), static_cast<int>(data.size())).valid);
    }

    SECTION("vertices that all decode to non-numbers")
    {
        auto data = p3dmModel({{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}});
        const uint32_t quietNaN = 0x7fc00000u;
        for (int offset = 40; offset + 4 <= static_cast<int>(data.size()); offset += 4)
            if (((offset - 40) % 16) != 12) // every coordinate, leaving the flags word alone
                std::memcpy(data.data() + offset, &quietNaN, 4);
        REQUIRE_FALSE(P3D::ReadModelProxyBounds(data.data(), static_cast<int>(data.size())).valid);
    }
}

TEST_CASE("One unusable vertex does not cost a model its proxy", "[p3d][mlod][farfield]")
{
    // The other half of the NaN rule. ODOL can refuse on a single bad component
    // because it has seven of them; a 3,000-vertex mesh with one bad point still
    // has a silhouette, and dropping the model would be the larger error.
    auto data = p3dmModel({{-2.0f, 0.0f, -2.0f}, {0.0f, 0.0f, 0.0f}, {2.0f, 5.0f, 2.0f}});
    const uint32_t quietNaN = 0x7fc00000u;
    std::memcpy(data.data() + 40 + 16, &quietNaN, 4); // the middle point's x
    const auto bounds = P3D::ReadModelProxyBounds(data.data(), static_cast<int>(data.size()));

    REQUIRE(bounds.valid);
    REQUIRE(bounds.minY == Catch::Approx(0.0f));
    REQUIRE(bounds.maxY == Catch::Approx(5.0f));
    REQUIRE(bounds.horizontalExtent == Catch::Approx(4.0f));
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
    //
    // All four routes are walked, because they leave the cursor in four different
    // places: the ODOL header read, the MLOD vertex walk (which travels furthest
    // by far), the ODOL 7 refusal and an MLOD refusal.
    auto acceptedOdol = odol73Header();
    auto acceptedMlod = readFile(GET_FIXTURE("p3d/simple_tree.p3d"));
    auto refusedOdol = readFile(GET_FIXTURE("p3d/animated_morph_odol.p3d"));
    auto refusedMlod = readFile(GET_FIXTURE("p3d/empty_shape.p3d"));

    for (auto* data : {&acceptedOdol, &acceptedMlod, &refusedOdol, &refusedMlod})
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
