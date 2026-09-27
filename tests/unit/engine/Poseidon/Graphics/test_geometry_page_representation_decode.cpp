#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageRepresentationDecode.hpp>
using namespace Poseidon;
using namespace Poseidon::GeometryPages;
namespace
{
ShapeExport Authored()
{
    ShapeExport dto; dto.source.sourceSha256[0]=17; dto.source.vertexLayout=1;
    dto.source.materialMapping=1; dto.source.coarseRepresentation=0; dto.source.fineRepresentation=1;
    for (auto* mesh:{&dto.coarse,&dto.fine})
    {
        mesh->positions={{0,0,0},{1,0,0},{0,1,0},{0,0,0}};
        mesh->vertices.resize(4);
        for (size_t v=0;v<4;++v)
        {
            const auto p=mesh->positions[v]; mesh->vertices[v].pos=Vector3P(p.x,p.y,p.z);
            mesh->vertices[v].norm=Vector3P(0,0,1); mesh->vertices[v].t0={float(v),0};
            mesh->vertices[v].t1={0,float(v)}; mesh->vertices[v].conform=0;
            mesh->vertices[v].tangent=Vector3P(1,0,0); mesh->vertices[v].binormal=Vector3P(0,1,0);
        }
    }
    dto.coarse.indices={0,1,2}; dto.coarse.materials={7};
    dto.fine.indices={0,1,2,3,2,1,0,1,2}; dto.fine.materials={7,8,7};
    return dto;
}
void PatchControl(Page& page,size_t at,uint32_t value)
{ for (unsigned i=0;i<4;++i) page.bytes[at+i]=uint8_t(value>>(8*i)); }
void CheckReconstruction(const ResidentRepresentation& result,const ExportedMesh& source)
{
    size_t triangle=0; uint64_t vertices=0,indices=0,bytes=0,clusters=0;
    for (const auto& page:result.pages) for (const auto& cluster:page.clusters)
    {
        REQUIRE(cluster.firstTriangle==triangle);
        for (size_t i=0;i<cluster.indices.size();++i)
        {
            REQUIRE(cluster.indices[i]<cluster.vertices.size());
            const auto& actual=cluster.vertices[cluster.indices[i]];
            const auto& original=source.vertices[source.indices[triangle*3+i]];
            REQUIRE(std::memcmp(&actual,&original,sizeof(SVertex))==0);
        }
        for (size_t t=0;t<cluster.indices.size()/3;++t) REQUIRE(cluster.material==source.materials[triangle+t]);
        triangle+=cluster.indices.size()/3; vertices+=cluster.vertices.size(); indices+=cluster.indices.size();
        bytes+=cluster.vertices.size()*sizeof(SVertex)+cluster.indices.size()*4; ++clusters;
    }
    REQUIRE(triangle==source.materials.size()); REQUIRE(result.vertexRecords==vertices);
    REQUIRE(result.indices==indices); REQUIRE(result.decodedBytes==bytes); REQUIRE(result.clusters==clusters);
}
}
TEST_CASE("Representation decode publishes one complete authored coarse or fine cut", "[geometry-page-representation]")
{
    auto dto=Authored(); Package package; Limits packing;
    packing.pageBytes=uint32_t(16+48+3*(4+sizeof(SVertex))+3*4);
    REQUIRE(Build(dto.coarse.Input(),dto.fine.Input(),dto.source,true,package,packing)==Status::Built);
    REQUIRE(package.pages.size()==4);
    for (auto selected:{Frontier::Coarse,Frontier::Fine})
    {
        ResidentRepresentation result;
        REQUIRE(DecodeResidentRepresentation(package,package.Identity(),selected,dto,result)==DecodeStatus::Decoded);
        REQUIRE(result.identity==package.Identity()); REQUIRE(result.representation==selected);
        REQUIRE(result.pages.size()==(selected==Frontier::Coarse?1:3));
        CheckReconstruction(result,selected==Frontier::Coarse?dto.coarse:dto.fine);
    }
}
TEST_CASE("Representation decode rejects locally valid overlapping triangle ranges", "[geometry-page-representation]")
{
    auto dto=Authored(); dto.fine.indices={0,1,2,0,1,2}; dto.fine.materials={7,7};
    Package package; Limits packing; packing.clusterTriangles=1;
    REQUIRE(Build(dto.coarse.Input(),dto.fine.Input(),dto.source,true,package,packing)==Status::Built);
    const auto descriptor=package.coarseClusters+1;
    auto& repeated=package.clusters[descriptor]; repeated.firstTriangle=0;
    PatchControl(package.pages[repeated.page],repeated.byteOffset+4,0);
    ResidentPage individual;
    REQUIRE(DecodeResidentPage(package,package.Identity(),repeated.page,dto,individual)==DecodeStatus::Decoded);
    ResidentRepresentation old; old.indices=991; old.representation=Frontier::Coarse;
    REQUIRE(DecodeResidentRepresentation(package,package.Identity(),Frontier::Fine,dto,old)==DecodeStatus::Invalid);
    REQUIRE(old.indices==991); REQUIRE(old.representation==Frontier::Coarse); REQUIRE(old.pages.empty());
}
TEST_CASE("Representation decode refuses missing detail and bounded aggregate debt transactionally", "[geometry-page-representation]")
{
    auto dto=Authored(); Package package;
    REQUIRE(Build(dto.coarse.Input(),dto.fine.Input(),dto.source,true,package)==Status::Built);
    ResidentRepresentation result; result.indices=77;
    RepresentationDecodeLimits limits; DecodeStatus expected=DecodeStatus::Capacity;
    SECTION("vertex sum") { limits.vertexRecords=8; }
    SECTION("index sum") { limits.indices=8; }
    SECTION("cluster sum") { limits.clusters=2; }
    SECTION("page sum") { limits.pages=0; }
    SECTION("serialized bytes") { limits.serializedBytes=16; }
    SECTION("decoded bytes") { limits.decodedBytes=1; }
    SECTION("hard cap cannot be raised") { ++limits.vertexRecords; }
    SECTION("wrong material") { dto.fine.materials[1]=99; expected=DecodeStatus::Invalid; }
    SECTION("missing original final triangle") { dto.fine.indices.insert(dto.fine.indices.end(),{0,1,2}); dto.fine.materials.push_back(7); expected=DecodeStatus::Invalid; }
    SECTION("late attribute corruption") { const auto& d=package.clusters.back(); package.pages[d.page].bytes[d.byteOffset+48+12+sizeof(Vector3P)]^=1; expected=DecodeStatus::Invalid; }
    REQUIRE(DecodeResidentRepresentation(package,package.Identity(),Frontier::Fine,dto,result,limits)==expected);
    REQUIRE(result.indices==77); REQUIRE(result.pages.empty());
}
TEST_CASE("Representation decode handles the 256 cluster page boundary without losing triangles", "[geometry-page-representation]")
{
    auto dto=Authored(); dto.fine.indices.assign(257*3,0); dto.fine.materials.assign(257,7);
    Package package; Limits packing; packing.clusterTriangles=1;
    REQUIRE(Build(dto.coarse.Input(),dto.fine.Input(),dto.source,true,package,packing)==Status::Built);
    ResidentRepresentation result;
    REQUIRE(DecodeResidentRepresentation(package,package.Identity(),Frontier::Fine,dto,result)==DecodeStatus::Decoded);
    REQUIRE(result.pages.size()==2); REQUIRE(result.clusters==257); CheckReconstruction(result,dto.fine);
    auto stale=package.Identity(); ++stale.source.geometryOptions;
    REQUIRE(DecodeResidentRepresentation(package,stale,Frontier::Fine,dto,result)==DecodeStatus::Invalid);
    REQUIRE(result.clusters==257);
    REQUIRE(DecodeResidentRepresentation(package,package.Identity(),Frontier::Unavailable,dto,result)==DecodeStatus::Invalid);
    REQUIRE(result.clusters==257);
}
