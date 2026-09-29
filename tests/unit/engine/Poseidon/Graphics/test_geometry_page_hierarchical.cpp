#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalCut.hpp>
#include <set>
using namespace Poseidon;
using namespace Poseidon::GeometryPages;
namespace
{
ClodBake CurvedHierarchy(bool seam=false)
{
    ShapeExport source;source.source.sourceSha256[0]=73;source.source.vertexLayout=sizeof(SVertex);
    source.source.materialMapping=1;source.source.coarseRepresentation=0;source.source.fineRepresentation=1;
    constexpr uint32_t side=17;
    for(uint32_t y=0;y<side;++y)for(uint32_t x=0;x<side;++x) {
        const float a=float(x)/16,b=float(y)/16,z=.9f*(1-a*a)*(1-b*b);SVertex v{};
        v.pos=Vector3P(a*4,b*4,z);v.norm=Vector3P(0,0,1);v.tangent=Vector3P(1,0,0);v.binormal=Vector3P(0,1,0);
        v.t0={a*a,b*b};v.t1=v.t0;source.fine.vertices.push_back(v);source.fine.positions.push_back({a*4,b*4,z});
    }
    for(uint32_t y=0;y<side-1;++y)for(uint32_t x=0;x<side-1;++x) {
        const auto a=y*side+x,b=a+1,c=a+side,d=c+1;
        source.fine.indices.insert(source.fine.indices.end(),{a,b,d,a,d,c});source.fine.materials.insert(source.fine.materials.end(),2,4);
    }
    if(seam){source.fine.vertices.push_back(source.fine.vertices[0]);source.fine.positions.push_back(source.fine.positions[0]);
        source.fine.vertices.back().t0={7,7};source.fine.indices[0]=uint32_t(source.fine.vertices.size()-1);}
    ClodBake bake;REQUIRE(BakeClodPilot(source,true,bake)==ClodBakeStatus::Baked);return bake;
}
HierarchicalPackage Pages(const ClodBake& bake,uint32_t pageBytes=65536)
{
    HierarchicalLimits limits;limits.pageBytes=pageBytes;HierarchicalPackage value;
    REQUIRE(BuildHierarchicalPackage(bake,value,limits)==HierarchicalBuildStatus::Built);return value;
}
std::vector<float> Thresholds(const ClodBake& bake)
{
    std::vector<float> result{0,1000000};
    for(const auto& g:bake.groups)if(g.simplified.error<FLT_MAX) {
        result.push_back(g.simplified.error);
        if(g.simplified.error>0)result.push_back(std::nextafter(g.simplified.error,0.f));
        result.push_back(std::nextafter(g.simplified.error,FLT_MAX));
    }
    return result;
}
}
TEST_CASE("Hierarchical pages preserve every actual baked DAG cluster and exact attributes", "[geometry-page-hierarchical]")
{
    const auto bake=CurvedHierarchy(true);const auto value=Pages(bake);const auto repeated=Pages(bake);
    REQUIRE(ValidHierarchicalMetadata(value));REQUIRE(value.identity==repeated.identity);
    REQUIRE(value.clusters.size()==bake.clusters.size());REQUIRE(value.groups.size()==bake.groups.size());
    REQUIRE(value.groups.size()>2);REQUIRE(value.pages.size()>2);REQUIRE_FALSE(value.rootPages.empty());
    REQUIRE(value.knownCapacityBytes==HierarchicalKnownBytes(value));REQUIRE(value.knownCapacityBytes<=1024*1024);
    bool seam=false;
    for(uint32_t id=0;id<value.clusters.size();++id) {
        const auto& d=value.clusters[id];const auto& p=value.pages[d.page];const auto& c=p.clusters[d.pageSlot];
        REQUIRE(d.group==bake.clusters[id].group);REQUIRE(d.refined==bake.clusters[id].refined);
        REQUIRE(p.group==d.group);REQUIRE(p.logicalBytes<=value.pageByteLimit);REQUIRE(p.uploadBytes<p.logicalBytes);
        REQUIRE(c.indices.size()==bake.clusters[id].indices.size());REQUIRE(c.material==4);
        for(size_t i=0;i<c.indices.size();++i)REQUIRE(c.originalVertexIds[c.indices[i]]==bake.clusters[id].indices[i]);
        for(size_t v=0;v<c.vertices.size();++v) {
            REQUIRE(std::memcmp(&c.vertices[v],&bake.original.vertices[c.originalVertexIds[v]],sizeof(SVertex))==0);
            if(c.vertices[v].t0.u==7&&c.vertices[v].t0.v==7)seam=true;
        }
    }
    REQUIRE(seam);
    auto changed=bake;changed.source.geometryOptions^=1;REQUIRE(Pages(changed).identity!=value.identity);
    REQUIRE(Pages(bake,16384).identity!=value.identity);
}
TEST_CASE("Hierarchical complete threshold cuts match real CLOD reference with page dependency sets", "[geometry-page-hierarchical]")
{
    const auto bake=CurvedHierarchy();const auto value=Pages(bake);
    std::vector<uint8_t> resident(value.pages.size(),1);std::set<std::vector<uint32_t>> distinct;
    for(float threshold:Thresholds(bake)) {
        ClodCut reference;REQUIRE(SelectClodCut(bake,threshold,reference));distinct.insert(reference.clusters);
        HierarchicalCutPlan plan;REQUIRE(PlanHierarchicalCut(value,threshold,{value.identity,resident},plan));
        REQUIRE(plan.state==HierarchicalCutState::RequestedCut);REQUIRE(plan.requestedClusters==reference.clusters);
        REQUIRE(plan.selectedClusters==reference.clusters);REQUIRE(plan.missingPages.empty());
        std::set<uint32_t> pages(value.rootPages.begin(),value.rootPages.end());
        for(auto c:reference.clusters)pages.insert(value.clusters[c].page);
        REQUIRE(std::set<uint32_t>(plan.requiredPages.begin(),plan.requiredPages.end())==pages);
        REQUIRE(plan.requiredPages.size()==pages.size());
    }
    REQUIRE(distinct.size()>2); // Intermediate actual bake cuts survive packaging.
}
TEST_CASE("Every partial hierarchical page mask preserves a complete pinned root fallback", "[geometry-page-hierarchical]")
{
    const auto bake=CurvedHierarchy();const auto value=Pages(bake);REQUIRE(value.pages.size()<16);
    ClodCut coarse;REQUIRE(SelectClodCut(bake,1000000,coarse));REQUIRE(coarse.clusters==value.rootClusters);
    ClodCut fine;REQUIRE(SelectClodCut(bake,0,fine));
    std::vector<uint8_t> resident(value.pages.size());
    for(uint32_t mask=0;mask<(1u<<value.pages.size());++mask) {
        for(size_t p=0;p<resident.size();++p)resident[p]=uint8_t((mask>>p)&1);
        HierarchicalCutPlan plan;REQUIRE(PlanHierarchicalCut(value,0,{value.identity,resident},plan));
        const bool roots=std::all_of(value.rootPages.begin(),value.rootPages.end(),[&](auto p){return resident[p]!=0;});
        const bool complete=std::all_of(plan.requiredPages.begin(),plan.requiredPages.end(),[&](auto p){return resident[p]!=0;});
        if(!roots){REQUIRE(plan.state==HierarchicalCutState::MissingRoots);REQUIRE(plan.selectedClusters.empty());}
        else if(!complete){REQUIRE(plan.state==HierarchicalCutState::RootFallback);REQUIRE(plan.selectedClusters==coarse.clusters);}
        else {REQUIRE(plan.state==HierarchicalCutState::RequestedCut);REQUIRE(plan.selectedClusters==fine.clusters);}
        std::set<uint32_t> missing;for(auto p:plan.requiredPages)if(!resident[p])missing.insert(p);
        REQUIRE(std::set<uint32_t>(plan.missingPages.begin(),plan.missingPages.end())==missing);
        REQUIRE(plan.missingPages.size()==missing.size());
    }
}
TEST_CASE("Hierarchical page request batches obey independent byte caps and in-flight deduplication", "[geometry-page-hierarchical]")
{
    const auto value=Pages(CurvedHierarchy());std::vector<uint8_t> resident(value.pages.size()),inFlight(value.pages.size());
    HierarchicalCutPlan plan;REQUIRE(PlanHierarchicalCut(value,0,{value.identity,resident},plan));
    REQUIRE(plan.missingPages.front()==value.rootPages.front());
    HierarchicalPageBatch batch;const auto root=value.rootPages.front();const auto& page=value.pages[root];
    REQUIRE(SelectHierarchicalPageBatch(value,plan,inFlight,1,page.logicalBytes,page.uploadBytes,batch));
    REQUIRE(batch.pages==std::vector<uint32_t>{root});REQUIRE(batch.logicalBytes==page.logicalBytes);REQUIRE(batch.uploadBytes==page.uploadBytes);
    inFlight[root]=1;
    REQUIRE(SelectHierarchicalPageBatch(value,plan,inFlight,64,UINT64_MAX,UINT64_MAX,batch));
    REQUIRE(std::find(batch.pages.begin(),batch.pages.end(),root)==batch.pages.end());REQUIRE(batch.pages.size()+1==plan.missingPages.size());
    REQUIRE(SelectHierarchicalPageBatch(value,plan,inFlight,64,0,UINT64_MAX,batch));REQUIRE(batch.pages.empty());
    REQUIRE(SelectHierarchicalPageBatch(value,plan,inFlight,64,UINT64_MAX,0,batch));REQUIRE(batch.pages.empty());
    const auto old=batch.pages;plan.missingPages.push_back(plan.missingPages.front());
    REQUIRE_FALSE(SelectHierarchicalPageBatch(value,plan,inFlight,64,UINT64_MAX,UINT64_MAX,batch));REQUIRE(batch.pages==old);
}
TEST_CASE("Hierarchical construction and demand fail closed without replacing published values", "[geometry-page-hierarchical]")
{
    auto bake=CurvedHierarchy();auto value=Pages(bake);const auto identity=value.identity;
    HierarchicalLimits limits;
    SECTION("foreign source") {bake.source.sourceSha256.fill(0);REQUIRE(BuildHierarchicalPackage(bake,value)==HierarchicalBuildStatus::Invalid);}
    SECTION("cycle") {bake.clusters.back().refined=int(bake.clusters.back().group);REQUIRE(BuildHierarchicalPackage(bake,value)==HierarchicalBuildStatus::Invalid);}
    SECTION("invalid payload count") {bake.clusters.front().indices.push_back(0);REQUIRE(BuildHierarchicalPackage(bake,value)==HierarchicalBuildStatus::Invalid);}
    SECTION("invalid payload ID") {bake.clusters.front().indices[0]=UINT32_MAX;REQUIRE(BuildHierarchicalPackage(bake,value)==HierarchicalBuildStatus::Invalid);}
    SECTION("page quota") {limits.pages=1;REQUIRE(BuildHierarchicalPackage(bake,value,limits)==HierarchicalBuildStatus::Capacity);}
    SECTION("known byte quota") {limits.knownCapacityBytes=1;REQUIRE(BuildHierarchicalPackage(bake,value,limits)==HierarchicalBuildStatus::Capacity);}
    SECTION("caps cannot be raised") {limits.clusters=257;REQUIRE(BuildHierarchicalPackage(bake,value,limits)==HierarchicalBuildStatus::Invalid);}
    REQUIRE(value.identity==identity);
    std::vector<uint8_t> resident(value.pages.size(),1);HierarchicalCutPlan plan;
    REQUIRE(PlanHierarchicalCut(value,0,{identity,resident},plan));const auto selected=plan.selectedClusters;
    auto foreign=identity;foreign.packageSha256[0]^=1;
    REQUIRE_FALSE(PlanHierarchicalCut(value,0,{foreign,resident},plan));REQUIRE(plan.selectedClusters==selected);
    REQUIRE_FALSE(PlanHierarchicalCut(value,FLT_MAX,{identity,resident},plan));REQUIRE(plan.selectedClusters==selected);
    resident[0]=2;REQUIRE_FALSE(PlanHierarchicalCut(value,0,{identity,resident},plan));resident[0]=1;
    value.clusters.back().page=UINT32_MAX;REQUIRE_FALSE(PlanHierarchicalCut(value,0,{identity,resident},plan));REQUIRE(plan.selectedClusters==selected);
}
