#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageResidentDecode.hpp>
using namespace Poseidon;
using namespace Poseidon::GeometryPages;
namespace
{
ShapeExport Original()
{
    ShapeExport dto; dto.source.sourceSha256[0]=1; dto.source.vertexLayout=1;
    dto.source.materialMapping=1; dto.source.coarseRepresentation=0; dto.source.fineRepresentation=1;
    for (auto* mesh : {&dto.coarse,&dto.fine})
    {
        mesh->positions={{0,0,0},{1,0,0},{0,1,0}};
        mesh->vertices.resize(3);
        for (size_t v=0; v<3; ++v)
        {
            auto p=mesh->positions[v]; mesh->vertices[v].pos=Vector3P(p.x,p.y,p.z);
            mesh->vertices[v].norm=Vector3P(0,0,1);
            mesh->vertices[v].tangent=Vector3P(1,0,0);
            mesh->vertices[v].binormal=Vector3P(0,1,0);
            mesh->vertices[v].t0={float(v),0}; mesh->vertices[v].t1={0,float(v)};
            mesh->vertices[v].conform=0;
        }
        mesh->indices={0,1,2}; mesh->materials={7};
    }
    return dto;
}
void Patch(Page& p,size_t at,uint32_t value)
{ for (unsigned i=0;i<4;++i) p.bytes[at+i]=uint8_t(value>>(i*8)); }
}
TEST_CASE("Geometry resident decoder preserves original attributes and local index topology", "[geometry-page-decode]")
{
    auto dto=Original(); Package package;
    REQUIRE(Build(dto.coarse.Input(),dto.fine.Input(),dto.source,true,package)==Status::Built);
    REQUIRE(package.pages.size()==2);
    for (uint32_t page=0;page<2;++page)
    {
        ResidentPage decoded;
        REQUIRE(DecodeResidentPage(package,package.Identity(),page,dto,decoded)==DecodeStatus::Decoded);
        REQUIRE(decoded.page==page); REQUIRE(decoded.clusters.size()==1);
        REQUIRE(decoded.clusters[0].material==7);
        REQUIRE(decoded.clusters[0].indices==std::vector<uint32_t>{0,1,2});
        REQUIRE(std::memcmp(decoded.clusters[0].vertices.data(),dto.fine.vertices.data(),3*sizeof(SVertex))==0);
    }
}
TEST_CASE("Geometry resident decoder refuses corrupt payload transactionally", "[geometry-page-decode]")
{
    auto dto=Original(); Package package;
    REQUIRE(Build(dto.coarse.Input(),dto.fine.Input(),dto.source,true,package)==Status::Built);
    ResidentPage decoded;
    REQUIRE(DecodeResidentPage(package,package.Identity(),0,dto,decoded)==DecodeStatus::Decoded);
    const auto oldIdentity=decoded.identity;
    SECTION("short header") { package.pages[0].bytes.resize(15); }
    SECTION("bad magic") { Patch(package.pages[0],0,0); }
    SECTION("unsupported stride") { Patch(package.pages[0],16+12,sizeof(SVertex)+4); }
    SECTION("bad material") { Patch(package.pages[0],16,8); }
    SECTION("wrong length") { Patch(package.pages[0],12,17); }
    SECTION("bad local index") { Patch(package.pages[0],16+48+12+3*sizeof(SVertex),3); }
    SECTION("changed original attribute") { package.pages[0].bytes[16+48+12+sizeof(Vector3P)]^=1; }
    SECTION("bounds too small") { Patch(package.pages[0],16+36,0); }
    SECTION("bad original ID") { Patch(package.pages[0],16+48,99); }
    SECTION("descriptor disagrees") { ++package.clusters[0].byteLength; }
    SECTION("wrong source") { dto.source.sourceSha256[0]=2; }
    SECTION("different original topology") { dto.coarse.indices={0,2,1}; }
    REQUIRE(DecodeResidentPage(package,package.Identity(),0,dto,decoded)==DecodeStatus::Invalid);
    REQUIRE(decoded.identity==oldIdentity); REQUIRE(decoded.clusters[0].material==7);
}
TEST_CASE("Geometry resident decoder enforces identity and bounded page counts", "[geometry-page-decode]")
{
    auto dto=Original(); Package package;
    REQUIRE(Build(dto.coarse.Input(),dto.fine.Input(),dto.source,true,package)==Status::Built);
    ResidentPage decoded; auto identity=package.Identity(); ++identity.packing.pageBytes;
    REQUIRE(DecodeResidentPage(package,identity,0,dto,decoded)==DecodeStatus::Invalid);
    REQUIRE(DecodeResidentPage(package,package.Identity(),2,dto,decoded)==DecodeStatus::Invalid);
    Patch(package.pages[0],8,257);
    REQUIRE(DecodeResidentPage(package,package.Identity(),0,dto,decoded)==DecodeStatus::Capacity);
    REQUIRE(decoded.clusters.empty());
}

TEST_CASE("Every rolled-over authored page remains resident-decodable", "[geometry-page-decode]")
{
    auto dto=Original(); dto.fine.indices.assign(257*3,0); dto.fine.materials.assign(257,7);
    Package package; Limits limits; limits.clusterTriangles=1;
    REQUIRE(Build(dto.coarse.Input(),dto.fine.Input(),dto.source,true,package,limits)==Status::Built);
    REQUIRE(package.pages.size()==3);
    size_t triangles=0;
    for (uint32_t page=package.coarsePages;page<package.pages.size();++page)
    {
        ResidentPage decoded;
        REQUIRE(DecodeResidentPage(package,package.Identity(),page,dto,decoded)==DecodeStatus::Decoded);
        REQUIRE(decoded.clusters.size()<=256);
        for (const auto& cluster:decoded.clusters) triangles+=cluster.indices.size()/3;
    }
    REQUIRE(triangles==257);
}
