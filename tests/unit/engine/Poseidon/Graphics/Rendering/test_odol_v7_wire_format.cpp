// ODOL v7 is a FIXED on-disk format: BI shipped it, we only read it. Every
// vertex index in it is two bytes.
//
// `VertexIndex` is a runtime type and was widened from 16 to 32 bits so imported
// later-generation models past 32,767 vertices in a LOD stop losing their
// geometry. The two are unrelated, but the reader used to tie them together --
// TransferBinaryArray()'s stride is sizeof(*data.Data()) -- so widening the
// typedef changed how many bytes were consumed from a file whose layout had not
// moved. The stream desynchronised at the first such array and every model in
// the shipped game rendered as melted geometry.
//
// It passed review at the time because instance counts, section counts and the
// LOD histogram were all byte-identical: the same instances still drew, with
// scrambled geometry inside them. So this test reads the BYTES of a committed
// ODOL v7 file and asserts the values that come out, which is the one check that
// cannot pass on counts alone. A save/load round trip would NOT catch it -- both
// halves move together and stay self-consistent.
#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>

#include "../../test_fixtures.hpp"

#include <cstdio>
#include <memory>
#include <vector>

namespace
{
std::vector<char> ReadWholeFile(const char* path)
{
    std::vector<char> data;
    std::FILE* f = std::fopen(path, "rb");
    REQUIRE(f != nullptr);
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    REQUIRE(size > 0);
    data.resize(static_cast<size_t>(size));
    REQUIRE(std::fread(data.data(), 1, data.size(), f) == data.size());
    std::fclose(f);
    return data;
}
} // namespace

TEST_CASE("ODOL v7: vertex indices are 16-bit on the wire whatever VertexIndex is", "[Shape][odol][wire]")
{
    // Written by tests/fixtures/p3d/generate_animated_morph_odol.py, which lays the
    // file out byte for byte: pointToVertex and vertexToPoint are u16 arrays of
    // [0,1,2,3], the single face is a quad over vertices 0..3, and there is one
    // named selection over the same four points.
    const std::vector<char> bytes = ReadWholeFile(GET_FIXTURE("p3d/animated_morph_odol.p3d"));

    QIStream in(bytes.data(), static_cast<int>(bytes.size()));
    std::unique_ptr<LODShapeWithShadow> shape(new LODShapeWithShadow());
    REQUIRE(shape->LoadOptimized(in));
    REQUIRE(shape->NLevels() == 1);

    const Shape* lod = shape->Level(0);
    REQUIRE(lod != nullptr);

    // The point/vertex maps come BEFORE the face table in the stream, so a stride
    // mismatch here is what melts everything downstream of it.
    REQUIRE(lod->NPoints() == 4);
    for (int i = 0; i < 4; i++)
    {
        REQUIRE(lod->PointToVertex(i) == i);
        REQUIRE(lod->VertexToPoint(i) == i);
    }

    REQUIRE(lod->NFaces() == 1);
    lod->BuildFaceIndexToOffset(); // the index->offset table is built lazily
    const Poly& face = lod->Face(lod->FaceIndexToOffset(0));
    REQUIRE(face.N() == 4);
    for (int i = 0; i < 4; i++)
    {
        REQUIRE(face.GetVertex(i) == i);
    }

    // Selection::_sel is the third VertexIndex-typed array on this wire.
    REQUIRE(lod->NNamedSel() == 1);
    const Poseidon::NamedSelection& sel = lod->NamedSel(0);
    REQUIRE(sel.Size() == 4);
    for (int i = 0; i < 4; i++)
    {
        REQUIRE(sel[i] == i);
    }
}
