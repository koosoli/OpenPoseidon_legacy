#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalLocalCut.hpp>
#include <algorithm>
#include <limits>
#include <set>
#include <utility>
using namespace Poseidon;
using namespace Poseidon::GeometryPages;
namespace
{
HierarchicalPackage CurvedPages()
{
    ShapeExport source;source.source.sourceSha256[0]=73;source.source.producerVersion=1;
    source.source.vertexLayout=sizeof(SVertex);source.source.materialMapping=1;
    source.source.coarseRepresentation=0;source.source.fineRepresentation=1;
    constexpr uint32_t side=17;
    for(uint32_t y=0;y<side;++y)for(uint32_t x=0;x<side;++x) {
        const float a=float(x)/16,b=float(y)/16,z=.9f*(1-a*a)*(1-b*b);SVertex v{};
        v.pos=Vector3P(a*4,b*4,z);v.norm=Vector3P(0,0,1);
        v.tangent=Vector3P(1,0,0);v.binormal=Vector3P(0,1,0);
        v.t0={a*a,b*b};v.t1=v.t0;
        source.fine.vertices.push_back(v);source.fine.positions.push_back({a*4,b*4,z});
    }
    for(uint32_t y=0;y<side-1;++y)for(uint32_t x=0;x<side-1;++x) {
        const auto a=y*side+x,b=a+1,c=a+side,d=c+1;
        source.fine.indices.insert(source.fine.indices.end(),{a,b,d,a,d,c});
        source.fine.materials.insert(source.fine.materials.end(),2,4);
    }
    ClodBake bake;REQUIRE(BakeClodPilot(source,true,bake)==ClodBakeStatus::Baked);
    HierarchicalPackage package;
    REQUIRE(BuildHierarchicalPackage(bake,package)==HierarchicalBuildStatus::Built);
    return package;
}
float RootThreshold(const HierarchicalPackage& package)
{
    float greatest=0;
    for(const auto& group:package.groups)
        if(group.simplified.error<FLT_MAX)greatest=std::max(greatest,group.simplified.error);
    return std::nextafter(greatest,FLT_MAX);
}
HierarchicalPackage SharedChildTopology()
{
    // Two parent groups cover disjoint triangles; both refine into one shared
    // child group. This is a topology fixture for the planner, not a codec test.
    HierarchicalPackage package;auto& identity=package.identity;
    identity.source.sourceSha256[0]=9;identity.source.producerVersion=1;
    identity.source.vertexLayout=sizeof(SVertex);identity.source.materialMapping=1;
    identity.source.coarseRepresentation=0;identity.source.fineRepresentation=1;
    identity.packageSha256[0]=19;
    clodBounds leaf{},parent{},root{};
    leaf.radius=parent.radius=root.radius=1;
    leaf.error=.25f;parent.error=1;root.error=FLT_MAX;
    package.groups={{0,leaf,0,2},{1,parent,2,1},{1,parent,3,1},{2,root,4,2}};
    package.clusters={{0,0,0,-1,leaf},{0,0,1,-1,leaf},
        {1,1,0,0,parent},{2,2,0,0,parent},
        {3,3,0,1,root},{3,3,1,2,root}};
    for(uint32_t g=0;g<package.groups.size();++g) {
        HierarchicalPage page;page.group=g;
        const auto count=package.groups[g].count;
        for(uint32_t slot=0;slot<count;++slot) {
            HierarchicalPayload payload;payload.cluster=package.groups[g].first+slot;
            payload.originalVertexIds={0,1,2};payload.vertices.resize(3);
            payload.indices={0,1,2};page.clusters.push_back(std::move(payload));
            page.logicalBytes+=48+3*(sizeof(SVertex)+4)+3*4;
            page.uploadBytes+=3*sizeof(SVertex)+3*4;
        }
        package.pages.push_back(std::move(page));
    }
    package.rootClusters={4,5};package.rootPages={3};
    package.knownCapacityBytes=HierarchicalKnownBytes(package);
    REQUIRE(ValidHierarchicalMetadata(package));return package;
}
void CheckFrontier(const HierarchicalPackage& package,const LocalizedHierarchicalCutPlan& plan)
{
    std::set<uint32_t> active(plan.activeGroups.begin(),plan.activeGroups.end());
    std::set<uint32_t> selected(plan.cut.requestedClusters.begin(),plan.cut.requestedClusters.end());
    REQUIRE(active.size()==plan.activeGroups.size());
    REQUIRE(selected.size()==plan.cut.requestedClusters.size());
    for(uint32_t g=0;g<package.groups.size();++g)
        if(package.groups[g].simplified.error==FLT_MAX)REQUIRE(active.contains(g));
    for(uint32_t id=0;id<package.clusters.size();++id) {
        const auto& cluster=package.clusters[id];
        if(cluster.refined>=0 && active.contains(uint32_t(cluster.refined)))
            REQUIRE(active.contains(cluster.group)); // every parent of a shared child
        const bool parent=active.contains(cluster.group);
        const bool child=cluster.refined>=0 && active.contains(uint32_t(cluster.refined));
        REQUIRE(selected.contains(id)==(parent && !child));
    }
}
}
TEST_CASE("Localized hierarchy uniform demands exactly preserve the full threshold cut", "[geometry-page-hierarchical-local]")
{
    const auto package=CurvedPages();std::vector<uint8_t> resident(package.pages.size(),1);
    std::vector<float> thresholds(package.groups.size());
    std::set<std::vector<uint32_t>> distinct;
    for(const auto& group:package.groups) {
        if(group.simplified.error==FLT_MAX)continue;
        for(float threshold:{0.f,group.simplified.error,
            std::nextafter(group.simplified.error,0.f),
            std::nextafter(group.simplified.error,FLT_MAX),RootThreshold(package)}) {
            std::fill(thresholds.begin(),thresholds.end(),threshold);
            HierarchicalCutPlan global;
            REQUIRE(PlanHierarchicalCut(package,threshold,{package.identity,resident},global));
            LocalizedHierarchicalCutPlan local;
            REQUIRE(PlanLocalizedHierarchicalCut(package,{package.identity,thresholds},{package.identity,resident},local));
            REQUIRE(local.cut.identity==package.identity);
            REQUIRE(local.cut.requestedClusters==global.requestedClusters);
            REQUIRE(local.cut.selectedClusters==global.selectedClusters);
            REQUIRE(local.cut.requiredPages==global.requiredPages);
            REQUIRE(local.cut.state==HierarchicalCutState::RequestedCut);
            CheckFrontier(package,local);distinct.insert(local.cut.selectedClusters);
        }
    }
    REQUIRE(distinct.size()>2);
}
TEST_CASE("Actual curved bake is a chain whose binary local demands remain scalar cuts", "[geometry-page-hierarchical-local]")
{
    const auto package=CurvedPages();std::vector<uint8_t> resident(package.pages.size(),1);
    const auto high=RootThreshold(package);
    std::vector<float> thresholds(package.groups.size(),high);
    // This 17x17 bake currently has one four-group ancestry chain. A chain has
    // only prefix frontiers, so no local demand can select spatially different
    // branches. Keep this topology assertion explicit if the baker changes.
    REQUIRE(package.groups.size()==4);
    std::set<std::pair<uint32_t,uint32_t>> edges;
    for(const auto& cluster:package.clusters)if(cluster.refined>=0)
        edges.insert({cluster.group,uint32_t(cluster.refined)});
    REQUIRE(edges.size()+1==package.groups.size());
    std::vector<uint32_t> parentCount(package.groups.size()),childCount(package.groups.size());
    for(const auto& [parent,child]:edges) {
        REQUIRE(parent<package.groups.size());REQUIRE(child<package.groups.size());
        ++parentCount[child];++childCount[parent];
    }
    REQUIRE(std::count(parentCount.begin(),parentCount.end(),0u)==1);
    REQUIRE(std::count(childCount.begin(),childCount.end(),0u)==1);
    for(auto count:parentCount)REQUIRE(count<=1);
    for(auto count:childCount)REQUIRE(count<=1);
    std::set<std::vector<uint32_t>> uniform;
    const auto addUniform=[&](float threshold) {
        HierarchicalCutPlan cut;
        REQUIRE(PlanHierarchicalCut(package,threshold,{package.identity,resident},cut));
        uniform.insert(cut.requestedClusters);
    };
    addUniform(0);addUniform(high);
    for(const auto& group:package.groups)if(group.simplified.error<FLT_MAX) {
        addUniform(group.simplified.error);
        if(group.simplified.error>0)addUniform(std::nextafter(group.simplified.error,0.f));
        addUniform(std::nextafter(group.simplified.error,FLT_MAX));
    }
    for(uint32_t mask=0;mask<(1u<<package.groups.size());++mask) {
        for(uint32_t g=0;g<package.groups.size();++g)
            thresholds[g]=(mask&(1u<<g))?0.f:high;
        LocalizedHierarchicalCutPlan candidate;
        REQUIRE(PlanLocalizedHierarchicalCut(package,{package.identity,thresholds},{package.identity,resident},candidate));
        CheckFrontier(package,candidate);
        REQUIRE(uniform.contains(candidate.cut.requestedClusters));
    }
}
TEST_CASE("A shared child closes all parent groups and has no duplicate selected coverage", "[geometry-page-hierarchical-local]")
{
    const auto package=SharedChildTopology();std::vector<uint8_t> resident(package.pages.size(),1);
    std::vector<float> thresholds(package.groups.size(),2);
    thresholds[0]=0;LocalizedHierarchicalCutPlan cut;
    REQUIRE(PlanLocalizedHierarchicalCut(package,{package.identity,thresholds},{package.identity,resident},cut));
    CheckFrontier(package,cut);
    REQUIRE(cut.activeGroups==(std::vector<uint32_t>{0,1,2,3}));
    REQUIRE(cut.cut.requestedClusters==(std::vector<uint32_t>{0,1}));
    REQUIRE(cut.cut.selectedClusters==cut.cut.requestedClusters);
    // An independent demand on one parent retains the other root cluster.
    thresholds[0]=2;thresholds[1]=0;
    REQUIRE(PlanLocalizedHierarchicalCut(package,{package.identity,thresholds},{package.identity,resident},cut));
    CheckFrontier(package,cut);
    REQUIRE(cut.activeGroups==(std::vector<uint32_t>{1,3}));
    REQUIRE(cut.cut.requestedClusters==(std::vector<uint32_t>{2,5}));
    // This admissible branching topology demonstrates an actual localized
    // frontier that no scalar threshold can select: one root branch refines.
    for(float threshold:{0.f,.25f,1.f,2.f}) {
        HierarchicalCutPlan uniform;
        REQUIRE(PlanHierarchicalCut(package,threshold,{package.identity,resident},uniform));
        REQUIRE(uniform.requestedClusters!=cut.cut.requestedClusters);
    }
}
TEST_CASE("Every partial localized page mask has only the whole requested cut or pinned roots", "[geometry-page-hierarchical-local]")
{
    const auto package=CurvedPages();REQUIRE(package.pages.size()<16);
    std::vector<uint8_t> resident(package.pages.size());
    std::vector<float> thresholds(package.groups.size(),RootThreshold(package));
    bool found=false;
    for(uint32_t g=0;g<package.groups.size()&&!found;++g) {
        if(package.groups[g].simplified.error==0||package.groups[g].simplified.error==FLT_MAX)continue;
        thresholds[g]=0;LocalizedHierarchicalCutPlan candidate;
        std::fill(resident.begin(),resident.end(),1);
        REQUIRE(PlanLocalizedHierarchicalCut(package,{package.identity,thresholds},{package.identity,resident},candidate));
        found=candidate.cut.requestedClusters!=package.rootClusters;
        if(!found)thresholds[g]=RootThreshold(package);
    }
    REQUIRE(found);
    for(uint32_t mask=0;mask<(1u<<package.pages.size());++mask) {
        for(uint32_t p=0;p<package.pages.size();++p)resident[p]=uint8_t((mask>>p)&1);
        LocalizedHierarchicalCutPlan plan;
        REQUIRE(PlanLocalizedHierarchicalCut(package,{package.identity,thresholds},{package.identity,resident},plan));
        CheckFrontier(package,plan);
        const bool roots=std::all_of(package.rootPages.begin(),package.rootPages.end(),
            [&](uint32_t p){return resident[p]!=0;});
        const bool complete=std::all_of(plan.cut.requiredPages.begin(),plan.cut.requiredPages.end(),
            [&](uint32_t p){return resident[p]!=0;});
        std::set<uint32_t> missing;
        for(auto p:plan.cut.requiredPages)if(!resident[p])missing.insert(p);
        REQUIRE(std::set<uint32_t>(plan.cut.missingPages.begin(),plan.cut.missingPages.end())==missing);
        if(!roots) {
            REQUIRE(plan.cut.state==HierarchicalCutState::MissingRoots);
            REQUIRE(plan.cut.selectedClusters.empty());
        } else if(!complete) {
            REQUIRE(plan.cut.state==HierarchicalCutState::RootFallback);
            REQUIRE(plan.cut.selectedClusters==package.rootClusters);
        } else {
            REQUIRE(plan.cut.state==HierarchicalCutState::RequestedCut);
            REQUIRE(plan.cut.selectedClusters==plan.cut.requestedClusters);
        }
    }
}
TEST_CASE("Localized hierarchy batches and foreign demand fail closed", "[geometry-page-hierarchical-local]")
{
    const auto package=CurvedPages();std::vector<uint8_t> resident(package.pages.size());
    std::vector<float> thresholds(package.groups.size(),0);
    LocalizedHierarchicalCutPlan plan;
    REQUIRE(PlanLocalizedHierarchicalCut(package,{package.identity,thresholds},{package.identity,resident},plan));
    const auto old=plan.cut.requestedClusters;
    auto foreign=package.identity;foreign.packageSha256[0]^=1;
    REQUIRE_FALSE(PlanLocalizedHierarchicalCut(package,{package.identity,thresholds},{foreign,resident},plan));
    REQUIRE_FALSE(PlanLocalizedHierarchicalCut(package,{foreign,thresholds},{package.identity,resident},plan));
    REQUIRE(plan.cut.requestedClusters==old);
    thresholds[0]=FLT_MAX;
    REQUIRE_FALSE(PlanLocalizedHierarchicalCut(package,{package.identity,thresholds},{package.identity,resident},plan));
    thresholds[0]=-1;
    REQUIRE_FALSE(PlanLocalizedHierarchicalCut(package,{package.identity,thresholds},{package.identity,resident},plan));
    thresholds[0]=std::numeric_limits<float>::quiet_NaN();
    REQUIRE_FALSE(PlanLocalizedHierarchicalCut(package,{package.identity,thresholds},{package.identity,resident},plan));
    thresholds[0]=std::numeric_limits<float>::infinity();
    REQUIRE_FALSE(PlanLocalizedHierarchicalCut(package,{package.identity,thresholds},{package.identity,resident},plan));
    thresholds[0]=0;
    REQUIRE_FALSE(PlanLocalizedHierarchicalCut(package,
        {package.identity,std::span<const float>(thresholds.data(),thresholds.size()-1)},
        {package.identity,resident},plan));
    REQUIRE(plan.cut.requestedClusters==old);
    resident[0]=2;
    REQUIRE_FALSE(PlanLocalizedHierarchicalCut(package,{package.identity,thresholds},{package.identity,resident},plan));
    resident[0]=0;
    const auto root=package.rootPages.front();const auto& page=package.pages[root];
    std::vector<uint8_t> inFlight(package.pages.size());HierarchicalPageBatch batch;
    REQUIRE(SelectHierarchicalPageBatch(package,plan.cut,inFlight,1,page.logicalBytes,page.uploadBytes,batch));
    REQUIRE(batch.pages==std::vector<uint32_t>{root});
    inFlight[root]=1;
    REQUIRE(SelectHierarchicalPageBatch(package,plan.cut,inFlight,1,UINT64_MAX,UINT64_MAX,batch));
    REQUIRE(batch.pages.size()<=1);
    REQUIRE(std::find(batch.pages.begin(),batch.pages.end(),root)==batch.pages.end());
}
