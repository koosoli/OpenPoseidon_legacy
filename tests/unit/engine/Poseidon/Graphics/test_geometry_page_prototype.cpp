#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPagePrototype.hpp>
#include <cstring>

using namespace Poseidon::GeometryPages;
namespace
{
struct Pilot
{
    std::array<Position, 6> positions{{{0,0,0},{1,0,0},{1,1,0},{0,1,0},{0,0,0},{-1,0,0}}};
    // Vertex4 shares a position with vertex0 but has a different attribute record.
    std::array<uint8_t, 96> vertices{};
    std::array<uint32_t, 12> indices{{0,1,2,0,2,3,4,3,5,4,5,2}};
    std::array<uint32_t, 4> materials{{7,7,8,8}};
    Pilot()
    {
        for (size_t i=0; i<vertices.size(); ++i) vertices[i]=uint8_t(i);
        for (size_t v=0; v<positions.size(); ++v)
        {
            const std::array<float,3> position{positions[v].x,positions[v].y,positions[v].z};
            std::memcpy(vertices.data()+v*16,position.data(),12);
        }
    }
    MeshInput Fine() const { return {positions,vertices,16,indices,materials}; }
    MeshInput Coarse() const { return {positions,vertices,16,std::span(indices).first(3),std::span(materials).first(1)}; }
};
SourceIdentity Source()
{
    SourceIdentity s; s.sourceSha256[0]=42;
    s.coarseRepresentation=1; s.fineRepresentation=2;
    s.vertexLayout=1; s.materialMapping=1; return s;
}
uint32_t Read(std::span<const uint8_t> bytes, size_t at)
{
    REQUIRE(at+4 <= bytes.size());
    return uint32_t(bytes[at]) | uint32_t(bytes[at+1])<<8 |
           uint32_t(bytes[at+2])<<16 | uint32_t(bytes[at+3])<<24;
}
void CheckPagePayload(const Package& p, const MeshInput& input, size_t begin, size_t end)
{
    size_t triangles=0;
    for (size_t i=begin; i<end; ++i)
    {
        const auto& c=p.clusters[i]; const auto& bytes=p.pages[c.page].bytes;
        const size_t start=c.byteOffset;
        REQUIRE(Read(bytes,start)==c.material);
        REQUIRE(Read(bytes,start+4)==c.firstTriangle);
        REQUIRE(Read(bytes,start+8)==c.triangles);
        const uint32_t count=Read(bytes,start+16), indices=Read(bytes,start+20);
        REQUIRE(indices==c.triangles*3);
        const size_t ids=start+48, vertex=ids+count*4, local=vertex+count*input.stride;
        REQUIRE(local+indices*4==start+c.byteLength);
        for (uint32_t v=0; v<count; ++v)
        {
            const uint32_t id=Read(bytes,ids+v*4);
            REQUIRE(id < input.positions.size());
            REQUIRE(std::memcmp(bytes.data()+vertex+v*input.stride,
                input.vertices.data()+size_t(id)*input.stride,input.stride)==0);
            const auto pos=input.positions[id];
            std::array<float,3> decoded{};
            std::memcpy(decoded.data(),bytes.data()+vertex+v*input.stride,12);
            REQUIRE(decoded[0]==pos.x); REQUIRE(decoded[1]==pos.y); REQUIRE(decoded[2]==pos.z);
            for (unsigned axis=0; axis<3; ++axis)
            {
                const float value=std::array<float,3>{pos.x,pos.y,pos.z}[axis];
                REQUIRE(c.minimum[axis] <= value);
                REQUIRE(c.maximum[axis] >= value);
            }
        }
        for (uint32_t n=0; n<indices; ++n)
        {
            const uint32_t index=Read(bytes,local+n*4);
            REQUIRE(index<count);
            REQUIRE(Read(bytes,ids+index*4)==input.indices[size_t(c.firstTriangle)*3+n]);
        }
        for (uint32_t n=0; n<c.triangles; ++n)
            REQUIRE(input.triangleMaterials[c.firstTriangle+n]==c.material);
        triangles+=c.triangles;
    }
    REQUIRE(triangles==input.indices.size()/3);
}
}

TEST_CASE("Geometry pages retain original attributes, seams, triangles and materials", "[geometry-page-prototype]")
{
    Pilot pilot; Package p; Limits limits; limits.pageBytes=170;
    REQUIRE(std::memcmp(pilot.vertices.data(),pilot.vertices.data()+4*16,12)==0);
    REQUIRE(std::memcmp(pilot.vertices.data()+12,pilot.vertices.data()+4*16+12,4)!=0);
    REQUIRE(Build(pilot.Coarse(),pilot.Fine(),Source(),true,p,limits)==Status::Built);
    REQUIRE(p.coarsePages==1); REQUIRE(p.coarseClusters==1);
    REQUIRE(p.pages.size()==3); REQUIRE(p.clusters.size()==3);
    CheckPagePayload(p,pilot.Coarse(),0,p.coarseClusters);
    CheckPagePayload(p,pilot.Fine(),p.coarseClusters,p.clusters.size());
    uint64_t sum=0;
    for (const auto& page:p.pages)
    {
        REQUIRE(page.bytes.size()<=limits.pageBytes);
        REQUIRE(Read(page.bytes,0)==0x31504743);
        REQUIRE(Read(page.bytes,4)==Package::FormatVersion);
        REQUIRE(Read(page.bytes,12)==page.bytes.size()); sum+=page.bytes.size();
    }
    REQUIRE(sum==p.payloadBytes);
    Package again;
    REQUIRE(Build(pilot.Coarse(),pilot.Fine(),Source(),true,again,limits)==Status::Built);
    REQUIRE(again.pages.size()==p.pages.size());
    for (size_t i=0;i<p.pages.size();++i) REQUIRE(again.pages[i].bytes==p.pages[i].bytes);
}

TEST_CASE("Geometry page frontier uses complete resident coarse or complete fine only", "[geometry-page-prototype]")
{
    Pilot pilot; Package p; Limits l; l.pageBytes=170;
    REQUIRE(Build(pilot.Coarse(),pilot.Fine(),Source(),true,p,l)==Status::Built);
    std::array<uint8_t,3> resident{1,0,0};
    REQUIRE(SelectFrontier(p,{p.Identity(),resident})==Frontier::Coarse);
    resident[1]=1; REQUIRE(SelectFrontier(p,{p.Identity(),resident})==Frontier::Coarse);
    resident[2]=1; REQUIRE(SelectFrontier(p,{p.Identity(),resident})==Frontier::Fine);
    resident[0]=0; REQUIRE(SelectFrontier(p,{p.Identity(),resident})==Frontier::Unavailable);
    REQUIRE(SelectFrontier(p,{p.Identity(),std::span(resident).first(2)})==Frontier::Unavailable);
    resident.fill(1);
    auto stale=p.Identity(); stale.source.materialOptions=77;
    REQUIRE(SelectFrontier(p,{stale,resident})==Frontier::Unavailable);
    stale=p.Identity(); ++stale.packing.pageBytes;
    REQUIRE(SelectFrontier(p,{stale,resident})==Frontier::Unavailable);
    stale=p.Identity(); ++stale.algorithmVersion;
    REQUIRE(SelectFrontier(p,{stale,resident})==Frontier::Unavailable);
}

TEST_CASE("Geometry page failures do not publish partial fallback or detail", "[geometry-page-prototype]")
{
    Pilot pilot; Package p; p.source.geometryOptions=99; p.payloadBytes=123;
    auto fine=pilot.Fine();
    SECTION("unsupported categories") { REQUIRE(Build(pilot.Coarse(),fine,Source(),false,p)==Status::Unsupported); }
    SECTION("bad reference") { pilot.indices[11]=6; REQUIRE(Build(pilot.Coarse(),fine,Source(),true,p)==Status::InvalidInput); }
    SECTION("nonfinite position") { pilot.positions[5].x=std::numeric_limits<float>::quiet_NaN(); REQUIRE(Build(pilot.Coarse(),fine,Source(),true,p)==Status::InvalidInput); }
    SECTION("bounds expansion overflow") { pilot.positions[5].x=std::numeric_limits<float>::max(); REQUIRE(Build(pilot.Coarse(),fine,Source(),true,p)==Status::InvalidInput); }
    SECTION("partial triangle") { fine.indices=fine.indices.first(11); REQUIRE(Build(pilot.Coarse(),fine,Source(),true,p)==Status::InvalidInput); }
    SECTION("missing material") { fine.triangleMaterials=fine.triangleMaterials.first(3); REQUIRE(Build(pilot.Coarse(),fine,Source(),true,p)==Status::InvalidInput); }
    SECTION("stride mismatch") { fine.stride=15; REQUIRE(Build(pilot.Coarse(),fine,Source(),true,p)==Status::InvalidInput); }
    SECTION("source identity absent") { REQUIRE(Build(pilot.Coarse(),fine,{},true,p)==Status::InvalidInput); }
    SECTION("layout absent") { auto s=Source(); s.vertexLayout=0; REQUIRE(Build(pilot.Coarse(),fine,s,true,p)==Status::InvalidInput); }
    SECTION("coarse fine identity ambiguous") { auto s=Source(); s.fineRepresentation=s.coarseRepresentation; REQUIRE(Build(pilot.Coarse(),fine,s,true,p)==Status::InvalidInput); }
    SECTION("one page total") { Limits l; l.maxPages=1; REQUIRE(Build(pilot.Coarse(),fine,Source(),true,p,l)==Status::Capacity); }
    SECTION("payload cap") { Limits l; l.payloadBytes=136; REQUIRE(Build(pilot.Coarse(),fine,Source(),true,p,l)==Status::Capacity); }
    SECTION("cluster cap") { Limits l; l.maxClusters=1; REQUIRE(Build(pilot.Coarse(),fine,Source(),true,p,l)==Status::Capacity); }
    SECTION("page too small") { Limits l; l.pageBytes=64; REQUIRE(Build(pilot.Coarse(),fine,Source(),true,p,l)==Status::Capacity); }
    REQUIRE(p.source.geometryOptions==99); REQUIRE(p.payloadBytes==123);
    REQUIRE(p.pages.empty()); REQUIRE(p.clusters.empty());
}

TEST_CASE("Geometry page cache identity contains effective packing and representation options", "[geometry-page-prototype]")
{
    Pilot pilot; Package first, other; Limits l;
    REQUIRE(Build(pilot.Coarse(),pilot.Fine(),Source(),true,first,l)==Status::Built);
    l.clusterTriangles=1;
    REQUIRE(Build(pilot.Coarse(),pilot.Fine(),Source(),true,other,l)==Status::Built);
    REQUIRE(first.Identity()!=other.Identity());
    l=Limits{}; l.maxPages=100; // Refusal ceilings do not change successful packing.
    REQUIRE(Build(pilot.Coarse(),pilot.Fine(),Source(),true,other,l)==Status::Built);
    REQUIRE(first.Identity()==other.Identity());
    auto different=Source(); different.coarseRepresentation=9;
    REQUIRE(Build(pilot.Coarse(),pilot.Fine(),different,true,other,l)==Status::Built);
    REQUIRE(first.Identity()!=other.Identity());
}

TEST_CASE("Geometry cluster vertex cap splits without changing original triangles", "[geometry-page-prototype]")
{
    Pilot pilot; Package p; Limits limits; limits.clusterVertices=3;
    auto source=Source(); source.geometryOptions=7; source.materialOptions=11;
    REQUIRE(Build(pilot.Coarse(),pilot.Fine(),source,true,p,limits)==Status::Built);
    REQUIRE(p.source==source); REQUIRE(p.clusters.size()==5);
    CheckPagePayload(p,pilot.Fine(),p.coarseClusters,p.clusters.size());
    pilot.indices.fill(0); pilot.materials.fill(9);
    REQUIRE(Build(pilot.Coarse(),pilot.Fine(),source,true,p,limits)==Status::Built);
    CheckPagePayload(p,pilot.Fine(),p.coarseClusters,p.clusters.size());
}

TEST_CASE("Geometry pages roll over at256 clusters even below byte capacity", "[geometry-page-prototype]")
{
    Pilot pilot; std::vector<uint32_t> indices(257*3,0),materials(257,7);
    auto fine=pilot.Fine(); fine.indices=indices; fine.triangleMaterials=materials;
    Limits limits; limits.clusterTriangles=1; Package p;
    REQUIRE(Build(pilot.Coarse(),fine,Source(),true,p,limits)==Status::Built);
    REQUIRE(p.Identity().algorithmVersion==2);
    REQUIRE(p.coarsePages==1); REQUIRE(p.pages.size()==3);
    REQUIRE(Read(p.pages[1].bytes,8)==256); REQUIRE(Read(p.pages[2].bytes,8)==1);
    CheckPagePayload(p,fine,p.coarseClusters,p.clusters.size());
}
