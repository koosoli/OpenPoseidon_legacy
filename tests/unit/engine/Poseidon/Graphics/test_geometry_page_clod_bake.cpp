#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageClodBake.hpp>
using namespace Poseidon;
using namespace Poseidon::GeometryPages;
namespace
{
ShapeExport Grid()
{
    ShapeExport dto; dto.source.sourceSha256[0]=7; dto.source.vertexLayout=1;
    dto.source.materialMapping=1; dto.source.coarseRepresentation=0; dto.source.fineRepresentation=1;
    constexpr unsigned side=17;
    for (unsigned y=0;y<side;++y) for (unsigned x=0;x<side;++x)
    {
        dto.fine.positions.push_back({float(x),float(y),0}); SVertex v{};
        v.pos=Vector3P(float(x),float(y),0); v.norm=Vector3P(0,0,1);
        v.tangent=Vector3P(1,0,0); v.binormal=Vector3P(0,1,0);
        v.t0={float(x)/16,float(y)/16}; v.t1=v.t0; v.conform=0;
        dto.fine.vertices.push_back(v);
    }
    for (unsigned y=0;y<side-1;++y) for (unsigned x=0;x<side-1;++x)
    {
        unsigned a=y*side+x,b=a+1,c=a+side,d=c+1;
        dto.fine.indices.insert(dto.fine.indices.end(),{a,b,d,a,d,c});
        dto.fine.materials.insert(dto.fine.materials.end(),2,0);
    }
    return dto;
}
}
TEST_CASE("CLOD pilot bakes original indexed shell and paired DAG cuts", "[geometry-clod-bake]")
{
    auto dto=Grid(); const auto indices=dto.fine.indices;
    const auto vertices=dto.fine.vertices; ClodBake bake;
    REQUIRE(BakeClodPilot(dto,true,bake)==ClodBakeStatus::Baked);
    REQUIRE(ClodDetail::OriginalCoverage(bake)); REQUIRE(bake.groups.size()>1);
    REQUIRE(std::memcmp(vertices.data(),dto.fine.vertices.data(),vertices.size()*sizeof(SVertex))==0);
    REQUIRE(std::memcmp(vertices.data(),bake.original.vertices.data(),vertices.size()*sizeof(SVertex))==0);
    REQUIRE(dto.fine.indices==indices);
    size_t refined=0; for (const auto& c:bake.clusters) if (c.refined>=0) ++refined;
    REQUIRE(refined>0);
    for (float threshold : {0.f,.001f,.1f,1.f,1000000.f})
    {
        ClodCut cut; REQUIRE(SelectClodCut(bake,threshold,cut)); REQUIRE_FALSE(cut.clusters.empty());
        std::vector<uint32_t> expected;
        for (uint32_t i=0;i<bake.clusters.size();++i)
        {
            const auto& c=bake.clusters[i];
            if (bake.groups[c.group].simplified.error>threshold &&
                (c.refined<0 || bake.groups[c.refined].simplified.error<=threshold)) expected.push_back(i);
        }
        REQUIRE(cut.clusters==expected);
    }
}
TEST_CASE("CLOD pilot keeps original seam IDs and refuses unsupported data", "[geometry-clod-bake]")
{
    auto dto=Grid(); dto.fine.vertices.push_back(dto.fine.vertices[0]);
    dto.fine.vertices.back().t0={9,9}; dto.fine.positions.push_back(dto.fine.positions[0]);
    dto.fine.indices[0]=uint32_t(dto.fine.vertices.size()-1);
    ClodBake bake; REQUIRE(BakeClodPilot(dto,true,bake)==ClodBakeStatus::Baked);
    REQUIRE(ClodDetail::OriginalCoverage(bake));
    REQUIRE(bake.original.vertices.back().t0.u==9);
    const size_t oldGroups=bake.groups.size();
    SECTION("multi-material") { dto.fine.materials[0]=1; REQUIRE(BakeClodPilot(dto,true,bake)==ClodBakeStatus::Unsupported); }
    SECTION("invalid index") { dto.fine.indices[0]=99999; REQUIRE(BakeClodPilot(dto,true,bake)==ClodBakeStatus::Invalid); }
    SECTION("nonfinite input") { dto.fine.positions[0].x=std::numeric_limits<float>::infinity(); REQUIRE(BakeClodPilot(dto,true,bake)==ClodBakeStatus::Invalid); }
    SECTION("output quota") { ClodBakeLimits limits; limits.groups=1; REQUIRE(BakeClodPilot(dto,true,bake,limits)==ClodBakeStatus::Capacity); }
    SECTION("input quota") { ClodBakeLimits limits; limits.vertices=1; REQUIRE(BakeClodPilot(dto,true,bake,limits)==ClodBakeStatus::Capacity); }
    SECTION("absent eligibility") { REQUIRE(BakeClodPilot(dto,false,bake)==ClodBakeStatus::Unsupported); }
    REQUIRE(bake.groups.size()==oldGroups);
}
TEST_CASE("CLOD full cut refuses stale or malformed references transactionally", "[geometry-clod-bake]")
{
    auto dto=Grid(); ClodBake bake; REQUIRE(BakeClodPilot(dto,true,bake)==ClodBakeStatus::Baked);
    ClodCut cut; REQUIRE(SelectClodCut(bake,0,cut)); const auto expected=cut.clusters;
    SECTION("bad refined group") { bake.clusters[0].refined=0; }
    SECTION("bad group range") { ++bake.groups[0].first; }
    SECTION("nonfinite group error") { bake.groups[0].simplified.error=std::numeric_limits<float>::quiet_NaN(); }
    REQUIRE_FALSE(SelectClodCut(bake,.1f,cut)); REQUIRE(cut.clusters==expected);
    REQUIRE_FALSE(SelectClodCut(bake,FLT_MAX,cut));
}
TEST_CASE("CLOD cut uses paired group errors instead of nonmonotonic cluster errors", "[geometry-clod-bake]")
{
    ClodBake bake;
    clodBounds fine{{0,0,0},1,2},terminal{{0,0,0},1,FLT_MAX};
    bake.groups={{0,fine,0,2},{1,terminal,2,1}};
    bake.clusters.resize(3);
    bake.clusters[0].group=0; bake.clusters[1].group=0;
    bake.clusters[0].bounds.error=1000; bake.clusters[1].bounds.error=0;
    bake.clusters[2].group=1; bake.clusters[2].refined=0; bake.clusters[2].bounds.error=0;
    ClodCut cut;
    REQUIRE(SelectClodCut(bake,1,cut)); REQUIRE(cut.clusters==std::vector<uint32_t>{0,1});
    REQUIRE(SelectClodCut(bake,3,cut)); REQUIRE(cut.clusters==std::vector<uint32_t>{2});
}
