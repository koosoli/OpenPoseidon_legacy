#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalOriginalDemand.hpp>
#include <Poseidon/Graphics/Shadow/ShadowMath.hpp>
#include <limits>
#include <vector>
using namespace Poseidon;
using namespace Poseidon::GeometryPages;
namespace D=Poseidon::GeometryPages::HierarchicalOriginalDemand;
namespace O=Poseidon::GeometryPages::HierarchicalOriginalOutside;
namespace H=Poseidon::GeometryPages::HierarchicalProjection;
namespace
{
std::array<float,16> Identity()
{
    std::array<float,16> m{};m[0]=m[5]=m[10]=m[15]=1;return m;
}
SVertex Vertex(float x,float y,float z)
{
    SVertex v{};v.pos=Vector3P(x,y,z);v.norm=Vector3P(0,0,1);
    v.tangent=Vector3P(1,0,0);v.binormal=Vector3P(0,1,0);return v;
}
struct Fixture
{
    ShapeExport original;HierarchicalPackage package;
    O::Token token;std::vector<O::View> views;
    Fixture()
    {
        auto& source=original.source;source.sourceSha256[0]=62;
        source.producerVersion=1;source.vertexLayout=sizeof(SVertex);source.materialMapping=1;
        source.coarseRepresentation=0;source.fineRepresentation=1;
        for(auto* mesh:{&original.coarse,&original.fine}) {
            mesh->vertices={Vertex(-.2f,-.2f,-.1f),Vertex(.2f,-.2f,.1f),Vertex(0,.2f,0)};
            for(const auto& v:mesh->vertices)mesh->positions.push_back({v.pos.X(),v.pos.Y(),v.pos.Z()});
            mesh->indices={0,1,2};mesh->materials={0};
        }
        package.identity.source=source;package.identity.packageSha256[0]=29;
        clodBounds leaf{},root{};leaf.radius=.2f;leaf.error=.1f;root.radius=1;root.error=FLT_MAX;
        package.groups={{0,leaf,0,1},{1,root,1,1}};
        package.clusters={{0,0,0,-1,leaf},{1,1,0,0,root}};
        for(uint32_t i=0;i<2;++i) {
            HierarchicalPage page;page.group=i;page.logicalBytes=256;
            page.uploadBytes=3*sizeof(SVertex)+3*sizeof(uint32_t);
            HierarchicalPayload payload;payload.cluster=i;payload.originalVertexIds={0,1,2};
            payload.vertices=original.fine.vertices;payload.indices={0,1,2};
            page.clusters.push_back(std::move(payload));package.pages.push_back(std::move(page));
        }
        package.rootClusters={1};package.rootPages={1};
        package.knownCapacityBytes=HierarchicalKnownBytes(package);
        REQUIRE(ValidHierarchicalMetadata(package));
        token.views.binding={package.identity,3,5,7,11,13,3};
        token.views.frameGeneration=19;token.pageEpoch=23;token.requestId=29;token.originalSourceBytes=4096;
        O::View main;main.id=1;main.kind=H::ViewKind::Main;main.joinedNonJittered=true;
        main.separateMainProjection=true;main.model=Identity();main.model[14]=8;
        main.view=Identity();main.projection[0]=1.25f;main.projection[5]=1.75f;
        main.projection[10]=main.projection[11]=1;main.projection[14]=-.25f;main.clipNear=.25f;
        main.viewportWidth=1024;main.viewportHeight=768;main.viewportOriginX=320;
        O::View local=main;local.id=2;local.kind=H::ViewKind::LocalLight;
        local.separateMainProjection=false;local.model=Identity();local.model[12]=100;
        local.combinedLightVP=shadow::Mul(
            shadow::Perspective(1.1f,1.3f,.25f,100),
            shadow::LookAt({0,0,8},{0,0,0},{0,1,0})).m;
        views={main,local};Sync();
    }
    void Sync()
    {
        token.views.binding.requiredViewMask=0;token.views.viewGenerations.fill(0);
        for(auto& view:views)if(view.enabled) {
            token.views.binding.requiredViewMask|=uint64_t(1)<<(view.id-1);
            token.views.viewGenerations[view.id-1]=41+view.id;
        }
        for(auto& view:views) {
            view.binding=token.views.binding;view.frameGeneration=token.views.frameGeneration;
            view.generation=token.views.viewGenerations[view.id-1];
        }
    }
    D::Input Input(bool verified,double allowance=10000) const
    {return {{token,views,true},allowance,verified};}
};
}
TEST_CASE("Source-bound outside light skips numeric demand only with page-position gate", "[geometry-page-hierarchical-original-demand]")
{
    Fixture fixture;D::Result guarded,unguarded;
    REQUIRE(D::Build(fixture.package,fixture.original,fixture.Input(true),fixture.token,guarded)==D::Status::Ready);
    REQUIRE(guarded.requiredViewMask==3);
    REQUIRE(guarded.outsideViewMask==2);
    REQUIRE(guarded.forcedFineViewMask==0);
    REQUIRE(guarded.outside.requiredViewMask==3);
    REQUIRE(guarded.outside.outsideViewMask==2);
    REQUIRE(guarded.demand.authority==fixture.token.views);
    REQUIRE(guarded.demand.forcedFineViewMask==0);
    REQUIRE(guarded.groupThresholds[0]==fixture.package.groups[0].simplified.error);
    REQUIRE(guarded.projectedBakedErrorIndicators[0]>0);
    REQUIRE(D::Build(fixture.package,fixture.original,fixture.Input(false),fixture.token,unguarded)==D::Status::ForcedFine);
    REQUIRE(unguarded.requiredViewMask==3);
    REQUIRE(unguarded.outsideViewMask==2); // proof reported, not used for filtering
    REQUIRE(unguarded.forcedFineViewMask==2);
    REQUIRE(unguarded.demand.forcedFineViewMask==2);
    for(uint32_t g=0;g<unguarded.groupCount;++g)REQUIRE(unguarded.groupThresholds[g]==0);
}
TEST_CASE("Visible main indicator still requests Fine at a tighter allowance", "[geometry-page-hierarchical-original-demand]")
{
    Fixture fixture;D::Result demand;
    REQUIRE(D::Build(fixture.package,fixture.original,fixture.Input(true,1e-10),fixture.token,demand)==D::Status::Ready);
    REQUIRE(demand.outsideViewMask==2);REQUIRE(demand.forcedFineViewMask==0);
    REQUIRE(demand.groupThresholds[0]==0);
    REQUIRE(demand.Localized().groupThresholds.size()==fixture.package.groups.size());
    std::vector<uint8_t> resident(fixture.package.pages.size(),1);
    LocalizedHierarchicalCutPlan plan;
    REQUIRE(PlanLocalizedHierarchicalCut(fixture.package,demand.Localized(),{fixture.package.identity,resident},plan));
    REQUIRE(plan.cut.requestedClusters==(std::vector<uint32_t>{0}));
}
TEST_CASE("Unknown clipped or unsupported required view forces complete Fine", "[geometry-page-hierarchical-original-demand]")
{
    Fixture fixture;auto& main=fixture.views[0];
    SECTION("near envelope") {main.model[14]=.25f;}
    SECTION("unjoined main") {main.joinedNonJittered=false;}
    SECTION("projection skew") {main.projection[8]=.01f;}
    SECTION("nonfinite view") {main.view[0]=std::numeric_limits<float>::quiet_NaN();}
    D::Result demand;
    REQUIRE(D::Build(fixture.package,fixture.original,fixture.Input(true),fixture.token,demand)==D::Status::ForcedFine);
    REQUIRE(demand.forcedFineViewMask==1);
    for(uint32_t g=0;g<demand.groupCount;++g)REQUIRE(demand.groupThresholds[g]==0);
}
TEST_CASE("Original demand rejects stale authority and invalid allowance transactionally", "[geometry-page-hierarchical-original-demand]")
{
    Fixture fixture;auto input=fixture.Input(true);D::Result kept;kept.groupCount=57;
    D::Status expected=D::Status::Invalid;
    SECTION("source admission") {input.views.token.views.binding.sourceAdmissionEpoch++;expected=D::Status::IdentityMismatch;}
    SECTION("page epoch") {input.views.token.pageEpoch++;expected=D::Status::IdentityMismatch;}
    SECTION("request") {input.views.token.requestId++;expected=D::Status::IdentityMismatch;}
    SECTION("missing required view") {fixture.views[1].enabled=false;}
    SECTION("nonfinite allowance") {input.pixelIndicatorAllowance=std::numeric_limits<double>::infinity();}
    REQUIRE(D::Build(fixture.package,fixture.original,input,fixture.token,kept)==expected);
    REQUIRE(kept.groupCount==57);
}
