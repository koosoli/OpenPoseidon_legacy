#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageClodCutDecode.hpp>
using namespace Poseidon;
using namespace Poseidon::GeometryPages;
namespace
{
ShapeExport Shell()
{
    ShapeExport dto; dto.source.sourceSha256[0]=9; dto.source.vertexLayout=1;
    dto.source.materialMapping=1; dto.source.coarseRepresentation=0; dto.source.fineRepresentation=1;
    constexpr uint32_t side=9;
    for (uint32_t y=0;y<side;++y) for (uint32_t x=0;x<side;++x)
    {
        dto.fine.positions.push_back({float(x),float(y),0}); SVertex vertex{};
        vertex.pos=Vector3P(float(x),float(y),0); vertex.norm=Vector3P(0,0,1);
        vertex.tangent=Vector3P(1,0,0); vertex.binormal=Vector3P(0,1,0);
        vertex.t0={float(x)/8,float(y)/8}; vertex.t1=vertex.t0; vertex.conform=0;
        dto.fine.vertices.push_back(vertex);
    }
    for (uint32_t y=0;y<side-1;++y) for (uint32_t x=0;x<side-1;++x)
    {
        const uint32_t a=y*side+x,b=a+1,c=a+side,d=c+1;
        dto.fine.indices.insert(dto.fine.indices.end(),{a,b,d,a,d,c});
        dto.fine.materials.insert(dto.fine.materials.end(),2,4);
    }
    return dto;
}
void Check(const ClodBake& bake,const ClodCut& cut,const ClodDecodedCut& decoded)
{
    REQUIRE(decoded.clusters.size()==cut.clusters.size());
    for (size_t i=0;i<cut.clusters.size();++i)
    {
        const auto& c=decoded.clusters[i]; const auto& original=bake.clusters[cut.clusters[i]].indices;
        REQUIRE(c.bakedCluster==cut.clusters[i]); REQUIRE(c.mesh.material==4);
        REQUIRE(c.mesh.indices.size()==original.size());
        for (size_t v=0;v<c.mesh.vertices.size();++v)
        {
            const auto id=c.originalVertexIds[v]; REQUIRE(id<bake.original.vertices.size());
            REQUIRE(std::memcmp(&c.mesh.vertices[v],&bake.original.vertices[id],sizeof(SVertex))==0);
            const auto& p=c.mesh.vertices[v].pos; const std::array<float,3> xyz{p.X(),p.Y(),p.Z()};
            for (unsigned axis=0;axis<3;++axis)
            { REQUIRE(c.minimum[axis]<=xyz[axis]); REQUIRE(c.maximum[axis]>=xyz[axis]); }
        }
        for (size_t k=0;k<original.size();++k)
        { REQUIRE(c.mesh.indices[k]<c.originalVertexIds.size()); REQUIRE(c.originalVertexIds[c.mesh.indices[k]]==original[k]); }
    }
}
}
TEST_CASE("CLOD cut decoder emits exact conventional vertex and index buffers", "[geometry-clod-cut-decode]")
{
    auto dto=Shell();
    // Curvature and nonlinear attributes prevent a legitimate zero-error planar
    // collapse from making every threshold exercise the same representation.
    for (size_t i=0;i<dto.fine.vertices.size();++i)
    {
        const float x=dto.fine.positions[i].x,y=dto.fine.positions[i].y;
        const float z=.08f*x*x+.035f*y*y;
        dto.fine.positions[i].z=z; dto.fine.vertices[i].pos=Vector3P(x,y,z);
        dto.fine.vertices[i].t0={x*x/64.f,y*y/64.f}; dto.fine.vertices[i].t1=dto.fine.vertices[i].t0;
    }
    ClodBake bake;
    REQUIRE(BakeClodPilot(dto,true,bake)==ClodBakeStatus::Baked);
    float smallestPositive=FLT_MAX,largestPositive=0;
    for (const auto& group:bake.groups)
    {
        const float error=group.simplified.error;
        if (error>0 && error<FLT_MAX && std::isfinite(error))
        { smallestPositive=std::min(smallestPositive,error); largestPositive=std::max(largestPositive,error); }
    }
    REQUIRE(smallestPositive<FLT_MAX); REQUIRE(largestPositive>0);
    const float fineThreshold=std::nextafter(smallestPositive,0.f);
    const float coarseThreshold=std::nextafter(largestPositive,std::numeric_limits<float>::infinity());
    REQUIRE(std::isfinite(coarseThreshold)); REQUIRE(coarseThreshold<FLT_MAX);
    ClodCut fineCut,coarseCut;
    REQUIRE(SelectClodCut(bake,fineThreshold,fineCut));
    REQUIRE(SelectClodCut(bake,coarseThreshold,coarseCut));
    REQUIRE(fineCut.clusters!=coarseCut.clusters);
    auto triangles=[&](const ClodCut& cut)
    {
        size_t result=0;
        for (uint32_t id:cut.clusters) result+=bake.clusters[id].indices.size()/3;
        return result;
    };
    REQUIRE(triangles(fineCut)>triangles(coarseCut));
    for (float threshold:{fineThreshold,coarseThreshold})
    {
        ClodCut cut; REQUIRE(SelectClodCut(bake,threshold,cut)); ClodDecodedCut decoded;
        REQUIRE(DecodeClodCut(bake,threshold,cut,decoded)==ClodCutDecodeStatus::Decoded);
        REQUIRE(decoded.source==bake.source); REQUIRE(decoded.threshold==threshold);
        REQUIRE(decoded.knownPayloadBytes>0); Check(bake,cut,decoded);
    }
}
TEST_CASE("CLOD cut decoder preserves seam vertex IDs rather than welding", "[geometry-clod-cut-decode]")
{
    auto dto=Shell(); dto.fine.vertices.push_back(dto.fine.vertices[0]);
    dto.fine.vertices.back().t0={7,7}; dto.fine.positions.push_back(dto.fine.positions[0]);
    const uint32_t seam=uint32_t(dto.fine.vertices.size()-1); dto.fine.indices[0]=seam;
    ClodBake bake; REQUIRE(BakeClodPilot(dto,true,bake)==ClodBakeStatus::Baked);
    // Choose a threshold below every nonzero group error; zero-error valid
    // simplification may already replace planar interior triangles.
    ClodCut cut; REQUIRE(SelectClodCut(bake,0,cut)); ClodDecodedCut decoded;
    REQUIRE(DecodeClodCut(bake,0,cut,decoded)==ClodCutDecodeStatus::Decoded);
    Check(bake,cut,decoded);
    bool seamSeen=false;
    for (const auto& c:decoded.clusters) for (size_t v=0;v<c.originalVertexIds.size();++v)
        if (c.originalVertexIds[v]==seam) { seamSeen=true; REQUIRE(c.mesh.vertices[v].t0.u==7); }
    REQUIRE(seamSeen);
}
TEST_CASE("CLOD cut decoder refuses incomplete selections and bounded overflow transactionally", "[geometry-clod-cut-decode]")
{
    auto dto=Shell(); ClodBake bake; REQUIRE(BakeClodPilot(dto,true,bake)==ClodBakeStatus::Baked);
    ClodCut cut; REQUIRE(SelectClodCut(bake,0,cut)); ClodDecodedCut decoded;
    REQUIRE(DecodeClodCut(bake,0,cut,decoded)==ClodCutDecodeStatus::Decoded);
    const auto prior=decoded.knownPayloadBytes; const auto count=decoded.clusters.size();
    SECTION("missing selected cluster")
    { cut.clusters.pop_back(); REQUIRE(DecodeClodCut(bake,0,cut,decoded)==ClodCutDecodeStatus::Invalid); }
    SECTION("duplicate cluster")
    { cut.clusters.push_back(cut.clusters.front()); REQUIRE(DecodeClodCut(bake,0,cut,decoded)==ClodCutDecodeStatus::Invalid); }
    SECTION("bad local source index")
    { bake.clusters[cut.clusters.front()].indices[0]=9999; REQUIRE(DecodeClodCut(bake,0,cut,decoded)==ClodCutDecodeStatus::Invalid); }
    SECTION("small aggregate vertex cap")
    { ClodCutDecodeLimits l; l.vertexRecords=1; REQUIRE(DecodeClodCut(bake,0,cut,decoded,l)==ClodCutDecodeStatus::Capacity); }
    SECTION("small aggregate index cap")
    { ClodCutDecodeLimits l; l.indexEntries=1; REQUIRE(DecodeClodCut(bake,0,cut,decoded,l)==ClodCutDecodeStatus::Capacity); }
    SECTION("small byte cap")
    { ClodCutDecodeLimits l; l.knownPayloadBytes=1; REQUIRE(DecodeClodCut(bake,0,cut,decoded,l)==ClodCutDecodeStatus::Capacity); }
    SECTION("nonfinite threshold")
    { REQUIRE(DecodeClodCut(bake,std::numeric_limits<float>::infinity(),cut,decoded)==ClodCutDecodeStatus::Invalid); }
    REQUIRE(decoded.knownPayloadBytes==prior); REQUIRE(decoded.clusters.size()==count);
}
