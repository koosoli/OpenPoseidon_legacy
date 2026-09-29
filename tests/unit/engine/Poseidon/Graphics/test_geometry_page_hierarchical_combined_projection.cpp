#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalCombinedProjection.hpp>
#include <Poseidon/Graphics/Shadow/ShadowMath.hpp>
#include <cmath>
#include <limits>
using namespace Poseidon;
using namespace Poseidon::GeometryPages;
namespace HC=Poseidon::GeometryPages::HierarchicalCombinedProjection;
namespace HP=Poseidon::GeometryPages::HierarchicalProjection;
namespace
{
HierarchicalPackage MakeCombinedPackage()
{
    HierarchicalPackage package;auto& source=package.identity.source;
    source.sourceSha256[0]=91;source.producerVersion=1;
    source.vertexLayout=sizeof(SVertex);source.materialMapping=1;
    source.coarseRepresentation=0;source.fineRepresentation=1;
    package.identity.packageSha256[0]=37;
    clodBounds leaf{},root{};leaf.radius=.2f;leaf.error=.1f;
    root.radius=1;root.error=FLT_MAX;
    package.groups={{0,leaf,0,1},{1,root,1,1}};
    package.clusters={{0,0,0,-1,leaf},{1,1,0,0,root}};
    for(uint32_t i=0;i<2;++i) {
        HierarchicalPage page;page.group=i;page.logicalBytes=256;
        page.uploadBytes=3*sizeof(SVertex)+3*sizeof(uint32_t);
        HierarchicalPayload payload;payload.cluster=i;payload.originalVertexIds={0,1,2};
        payload.vertices.resize(3);payload.indices={0,1,2};
        page.clusters.push_back(std::move(payload));package.pages.push_back(std::move(page));
    }
    package.rootClusters={1};package.rootPages={1};
    package.knownCapacityBytes=HierarchicalKnownBytes(package);
    REQUIRE(ValidHierarchicalMetadata(package));return package;
}
std::array<float,16> Identity()
{
    std::array<float,16> value{};
    value[0]=value[5]=value[10]=value[15]=1;return value;
}
struct Joined
{
    HP::Authority authority;
    std::vector<HC::View> views;
    explicit Joined(const HierarchicalPackage& package)
    {
        authority.binding={package.identity,3,5,7,11,13,1};
        authority.frameGeneration=19;
        HC::View view;view.id=1;view.kind=HP::ViewKind::LocalLight;
        view.joinedNonJittered=true;view.model=Identity();
        view.combinedLightVP=shadow::Mul(
            shadow::Perspective(1.1f,1.3f,.25f,100),
            shadow::LookAt({0,0,8},{0,0,0},{0,1,0})).m;
        view.viewportWidth=1024;view.viewportHeight=768;
        view.viewportOriginX=512;view.viewportOriginY=256;
        views.push_back(view);Sync();
    }
    void Sync()
    {
        authority.binding.requiredViewMask=0;authority.viewGenerations.fill(0);
        for(auto& view:views)if(view.enabled) {
            authority.binding.requiredViewMask|=uint64_t(1)<<(view.id-1);
            authority.viewGenerations[view.id-1]=29+view.id;
        }
        for(auto& view:views) {
            view.binding=authority.binding;view.frameGeneration=authority.frameGeneration;
            view.generation=authority.viewGenerations[view.id-1];
        }
    }
    HC::Input Input(double allowance=10000) const
    {return {authority.binding,views,true,allowance};}
};
std::array<long double,2> Project(const HC::View& view,std::array<long double,3> point)
{
    std::array<long double,4> p{point[0],point[1],point[2],1};
    for(const auto* matrix:{&view.model,&view.combinedLightVP}) {
        std::array<long double,4> next{};
        for(unsigned j=0;j<4;++j)for(unsigned i=0;i<4;++i)
            next[j]+=p[i]*(*matrix)[i*4+j];
        p=next;
    }
    REQUIRE(p[3]>0);
    return {p[0]/p[3]*view.viewportWidth/2+view.viewportOriginX+view.viewportWidth/2,
        p[1]/p[3]*view.viewportHeight/2+view.viewportOriginY+view.viewportHeight/2};
}
void CheckPairBound(const HC::View& view,const clodBounds& bounds,double indicator)
{
    std::array<long double,3> p{bounds.center[0],bounds.center[1],bounds.center[2]};
    for(unsigned axis=0;axis<3;++axis) {
        auto q=p;q[axis]+=bounds.error/2;
        const auto a=Project(view,p),b=Project(view,q);
        REQUIRE(std::abs(a[0]-b[0])+std::abs(a[1]-b[1])<=indicator);
    }
}
}
TEST_CASE("Actual RH perspective light VP bounds a camera-relative baked-error indicator", "[geometry-page-hierarchical-combined]")
{
    const auto package=MakeCombinedPackage();Joined joined(package);HP::Demand demand;
    REQUIRE(HC::Build(package,joined.Input(),joined.authority,demand)==HP::Status::Ready);
    REQUIRE(demand.forcedFineViewMask==0);
    REQUIRE(demand.groupCount==package.groups.size());
    REQUIRE(demand.projectedBakedErrorIndicators[0]>0);
    CheckPairBound(joined.views[0],package.groups[0].simplified,demand.projectedBakedErrorIndicators[0]);
    REQUIRE(demand.groupThresholds[0]==package.groups[0].simplified.error);
    REQUIRE(demand.groupThresholds[1]==0);
    auto shifted=joined;shifted.views[0].viewportOriginX=2048;shifted.views[0].viewportOriginY=1024;
    HP::Demand same;
    REQUIRE(HC::Build(package,shifted.Input(),shifted.authority,same)==HP::Status::Ready);
    REQUIRE(same.projectedBakedErrorIndicators[0]==demand.projectedBakedErrorIndicators[0]);
}
TEST_CASE("Rotated translated perspective and signed affine light VP preserve numeric bounds", "[geometry-page-hierarchical-combined]")
{
    const auto package=MakeCombinedPackage();Joined joined(package);auto& view=joined.views[0];
    const float c=std::cos(.4f),s=std::sin(.4f);
    view.model=Identity();view.model[0]=c;view.model[1]=s;
    view.model[4]=-s;view.model[5]=c;
    view.model[12]=.5f;view.model[13]=-.3f;view.model[14]=.2f;
    view.combinedLightVP=shadow::Mul(
        shadow::Perspective(1.1f,1.3f,.25f,100),
        shadow::LookAt({4,2,10},{0,0,0},{0,1,0})).m;
    HP::Demand rotated;
    REQUIRE(HC::Build(package,joined.Input(),joined.authority,rotated)==HP::Status::Ready);
    CheckPairBound(view,package.groups[0].simplified,rotated.projectedBakedErrorIndicators[0]);
    view.combinedLightVP=shadow::Mul(
        shadow::Ortho(5,-5,-5,5,.25f,50),
        shadow::LookAt({4,2,10},{0,0,0},{0,1,0})).m;
    HP::Demand affine;
    REQUIRE(HC::Build(package,joined.Input(),joined.authority,affine)==HP::Status::Ready);
    CheckPairBound(view,package.groups[0].simplified,affine.projectedBakedErrorIndicators[0]);
}
TEST_CASE("Complete local and affine light set unions indicators and conservatively propagates clipping", "[geometry-page-hierarchical-combined]")
{
    const auto package=MakeCombinedPackage();Joined joined(package);HP::Demand local;
    REQUIRE(HC::Build(package,joined.Input(),joined.authority,local)==HP::Status::Ready);
    auto affine=joined.views[0];affine.id=2;affine.kind=HP::ViewKind::SolarShadow;
    affine.combinedLightVP=shadow::Mul(
        shadow::Ortho(-5,5,-5,5,.25f,50),
        shadow::LookAt({0,0,8},{0,0,0},{0,1,0})).m;
    joined.views.push_back(affine);joined.Sync();
    HP::Demand unionDemand;
    REQUIRE(HC::Build(package,joined.Input(),joined.authority,unionDemand)==HP::Status::Ready);
    REQUIRE(unionDemand.authority.binding.requiredViewMask==3);
    REQUIRE(unionDemand.projectedBakedErrorIndicators[0]>=local.projectedBakedErrorIndicators[0]);
    joined.views[1].model[12]=100;
    REQUIRE(HC::Build(package,joined.Input(),joined.authority,unionDemand)==HP::Status::ForcedFine);
    REQUIRE(unionDemand.forcedFineViewMask==2);
    for(uint32_t g=0;g<unionDemand.groupCount;++g)REQUIRE(unionDemand.groupThresholds[g]==0);
}
TEST_CASE("Combined projection forces Fine for near W side far and unsupported matrices", "[geometry-page-hierarchical-combined]")
{
    const auto package=MakeCombinedPackage();Joined joined(package);auto& view=joined.views[0];
    SECTION("near and W intersection") {view.model[14]=7.9f;}
    SECTION("outside side plane") {view.model[12]=100;}
    SECTION("error-expanded envelope crosses side") {view.model[12]=6.3f;}
    SECTION("past far plane") {view.model[14]=-200;}
    SECTION("skew") {view.combinedLightVP[4]+=.2f;}
    SECTION("nonfinite") {view.combinedLightVP[0]=std::numeric_limits<float>::quiet_NaN();}
    SECTION("subnormal") {view.combinedLightVP[0]=std::numeric_limits<float>::denorm_min();}
    HP::Demand demand;
    REQUIRE(HC::Build(package,joined.Input(),joined.authority,demand)==HP::Status::ForcedFine);
    REQUIRE(demand.forcedFineViewMask==1);
    for(uint32_t g=0;g<demand.groupCount;++g)REQUIRE(demand.groupThresholds[g]==0);
}
TEST_CASE("Combined view authority refuses stale or incomplete tuples transactionally", "[geometry-page-hierarchical-combined]")
{
    const auto package=MakeCombinedPackage();Joined joined(package);HP::Demand kept;kept.groupCount=57;
    auto input=joined.Input();HP::Status expected=HP::Status::Invalid;
    SECTION("source") {input.binding.identity.source.sourceSha256[0]^=1;expected=HP::Status::IdentityMismatch;}
    SECTION("page admission") {input.binding.sourceAdmissionEpoch++;expected=HP::Status::IdentityMismatch;}
    SECTION("frame") {joined.views[0].frameGeneration--;expected=HP::Status::IdentityMismatch;}
    SECTION("view generation") {joined.views[0].generation--;expected=HP::Status::IdentityMismatch;}
    SECTION("incomplete set") {input.completeEnabledSet=false;}
    SECTION("duplicate view") {joined.views.push_back(joined.views[0]);input=joined.Input();}
    SECTION("unprovided required view") {
        joined.authority.binding.requiredViewMask|=2;
        input.binding=joined.authority.binding;joined.views[0].binding=joined.authority.binding;
    }
    SECTION("invalid allowance") {input.pixelIndicatorAllowance=std::numeric_limits<double>::infinity();}
    REQUIRE(HC::Build(package,input,joined.authority,kept)==expected);
    REQUIRE(kept.groupCount==57);
}
