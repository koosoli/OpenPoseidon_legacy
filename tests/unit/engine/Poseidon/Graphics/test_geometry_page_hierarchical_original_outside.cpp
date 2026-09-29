#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalOriginalOutside.hpp>
#include <Poseidon/Graphics/Shadow/ShadowMath.hpp>
#include <limits>
#include <vector>
using namespace Poseidon;
using namespace Poseidon::GeometryPages;
namespace HO=Poseidon::GeometryPages::HierarchicalOriginalOutside;
namespace HP=Poseidon::GeometryPages::HierarchicalProjection;
namespace
{
SVertex Vertex(float x,float y,float z)
{
    SVertex value{};value.pos=Vector3P(x,y,z);
    value.norm=Vector3P(0,0,1);value.tangent=Vector3P(1,0,0);
    value.binormal=Vector3P(0,1,0);value.t0={x,y};value.t1={y,x};return value;
}
void Triangle(ExportedMesh& mesh,float extent)
{
    mesh.vertices={Vertex(-extent,-extent,-.2f),Vertex(extent,-extent,.2f),Vertex(0,extent,0)};
    for(const auto& v:mesh.vertices)
        mesh.positions.push_back({v.pos.X(),v.pos.Y(),v.pos.Z()});
    mesh.indices={0,1,2};mesh.materials={0};
}
struct OriginalAssetFixture
{
    ShapeExport original;HierarchicalPackage package;
    OriginalAssetFixture()
    {
        auto& source=original.source;source.sourceSha256[0]=81;
        source.producerVersion=1;source.vertexLayout=sizeof(SVertex);
        source.materialMapping=1;source.coarseRepresentation=0;source.fineRepresentation=1;
        Triangle(original.coarse,.4f);Triangle(original.fine,.2f);
        package.identity.source=source;package.identity.packageSha256[0]=17;
        clodBounds leaf{},root{};leaf.radius=.2f;leaf.error=.1f;
        root.radius=1;root.error=FLT_MAX;
        package.groups={{0,leaf,0,1},{1,root,1,1}};
        package.clusters={{0,0,0,-1,leaf},{1,1,0,0,root}};
        for(uint32_t c=0;c<2;++c) {
            HierarchicalPage page;page.group=c;page.logicalBytes=256;
            page.uploadBytes=3*sizeof(SVertex)+3*sizeof(uint32_t);
            HierarchicalPayload payload;payload.cluster=c;
            payload.originalVertexIds={0,1,2};payload.vertices=original.fine.vertices;
            payload.indices={0,1,2};page.clusters.push_back(std::move(payload));
            package.pages.push_back(std::move(page));
        }
        package.rootClusters={1};package.rootPages={1};
        package.knownCapacityBytes=HierarchicalKnownBytes(package);
        REQUIRE(ValidHierarchicalMetadata(package));
    }
};
std::array<float,16> Identity()
{
    std::array<float,16> result{};result[0]=result[5]=result[10]=result[15]=1;return result;
}
std::array<float,16> MainProjection()
{
    std::array<float,16> result{};
    result[0]=1.25f;result[5]=1.75f;result[10]=result[11]=1;
    result[14]=-.25f;return result;
}
std::array<float,16> FlattenForTest(const std::array<float,16>& view,
    const std::array<float,16>& projection)
{
    std::array<float,16> result{};
    for(unsigned k=0;k<4;++k)for(unsigned j=0;j<4;++j)
        for(unsigned i=0;i<4;++i)result[k*4+j]+=view[k*4+i]*projection[i*4+j];
    return result;
}
struct Joined
{
    HO::Token token;std::vector<HO::View> views;
    explicit Joined(const OriginalAssetFixture& asset)
    {
        token.views.binding={asset.package.identity,3,5,7,11,13,1};
        token.views.frameGeneration=19;token.pageEpoch=23;
        token.requestId=29;token.originalSourceBytes=4096;
        HO::View view;view.id=1;view.kind=HP::ViewKind::LocalLight;
        view.joinedNonJittered=true;view.model=Identity();
        view.combinedLightVP=shadow::Mul(
            shadow::Perspective(1.1f,1.3f,.25f,100),
            shadow::LookAt({0,0,8},{0,0,0},{0,1,0})).m;
        view.viewportWidth=1024;view.viewportHeight=768;
        view.viewportOriginX=320;view.viewportOriginY=100;
        views.push_back(view);Sync();
    }
    void Sync()
    {
        token.views.binding.requiredViewMask=0;token.views.viewGenerations.fill(0);
        for(auto& view:views)if(view.enabled) {
            token.views.binding.requiredViewMask|=uint64_t(1)<<(view.id-1);
            token.views.viewGenerations[view.id-1]=41+view.id;
        }
        for(auto& view:views) {
            view.binding=token.views.binding;
            view.frameGeneration=token.views.frameGeneration;
            view.generation=token.views.viewGenerations[view.id-1];
        }
    }
    HO::Input Input() const{return {token,views,true};}
};
}
TEST_CASE("Original coarse and Fine vertices form a source-bound whole-object box", "[geometry-page-hierarchical-original-outside]")
{
    OriginalAssetFixture asset;Joined joined(asset);HO::Result result;
    REQUIRE(HO::Build(asset.package,asset.original,joined.Input(),joined.token,result)==HO::Status::Ready);
    REQUIRE(result.originalMinimum[0]==-.4f);REQUIRE(result.originalMaximum[0]==.4f);
    REQUIRE(result.originalMinimum[2]==-.2f);REQUIRE(result.originalMaximum[2]==.2f);
    REQUIRE(result.requiredViewMask==1);REQUIRE(result.outsideViewMask==0);
    REQUIRE(result.unknownViewMask==1);
    // Neither the CLOD error nor the root sphere is used as an original bound.
    asset.package.groups[0].simplified.center[0]=1000;
    asset.package.groups[0].simplified.radius=1000;
    asset.package.groups[0].simplified.error=900;
    REQUIRE(ValidHierarchicalMetadata(asset.package));
    HO::Result same;
    REQUIRE(HO::Build(asset.package,asset.original,joined.Input(),joined.token,same)==HO::Status::Ready);
    REQUIRE(same.originalMinimum==result.originalMinimum);
    REQUIRE(same.originalMaximum==result.originalMaximum);
}
TEST_CASE("Whole original box strictly outside one light plane preserves required view identity", "[geometry-page-hierarchical-original-outside]")
{
    OriginalAssetFixture asset;Joined joined(asset);joined.views[0].model[12]=100;
    HO::Result result;
    REQUIRE(HO::Build(asset.package,asset.original,joined.Input(),joined.token,result)==HO::Status::Ready);
    REQUIRE(result.requiredViewMask==1);
    REQUIRE(result.outsideViewMask==1);REQUIRE(result.unknownViewMask==0);
    // A second enabled view that sees the source is never silently omitted.
    auto main=joined.views[0];main.id=2;main.kind=HP::ViewKind::Main;
    main.model=Identity();joined.views.push_back(main);joined.Sync();
    REQUIRE(HO::Build(asset.package,asset.original,joined.Input(),joined.token,result)==HO::Status::Ready);
    REQUIRE(result.requiredViewMask==3);
    REQUIRE(result.outsideViewMask==1);REQUIRE(result.unknownViewMask==2);
}
TEST_CASE("Separate joined main and reflection matrices prove outside without float VP flattening", "[geometry-page-hierarchical-original-outside]")
{
    OriginalAssetFixture asset;Joined joined(asset);auto& view=joined.views[0];
    view.kind=HP::ViewKind::Main;view.separateMainProjection=true;
    view.view=Identity();view.projection=MainProjection();view.clipNear=.25f;
    view.model[12]=100;view.model[14]=8;
    HO::Result separate,flattened;
    REQUIRE(HO::Build(asset.package,asset.original,joined.Input(),joined.token,separate)==HO::Status::Ready);
    REQUIRE(separate.outsideViewMask==1);REQUIRE(separate.unknownViewMask==0);
    view.separateMainProjection=false;
    view.combinedLightVP=FlattenForTest(view.view,view.projection);
    REQUIRE(HO::Build(asset.package,asset.original,joined.Input(),joined.token,flattened)==HO::Status::Ready);
    REQUIRE(flattened.outsideViewMask==separate.outsideViewMask);
    // Actual translation-free camera rotation: x world becomes positive view Z,
    // while z world becomes negative view X. No view translation is fabricated.
    view.separateMainProjection=true;view.kind=HP::ViewKind::Reflection;
    view.view=Identity();view.view[0]=view.view[10]=0;
    view.view[2]=1;view.view[8]=-1;
    view.model=Identity();view.model[12]=8;view.model[14]=100;
    REQUIRE(HO::Build(asset.package,asset.original,joined.Input(),joined.token,separate)==HO::Status::Ready);
    REQUIRE(separate.outsideViewMask==1);
    view.combinedLightVP=FlattenForTest(view.view,view.projection);
    view.separateMainProjection=false;
    REQUIRE(HO::Build(asset.package,asset.original,joined.Input(),joined.token,flattened)==HO::Status::Ready);
    REQUIRE(flattened.outsideViewMask==separate.outsideViewMask);
    view.separateMainProjection=true;view.model[14]=0;
    REQUIRE(HO::Build(asset.package,asset.original,joined.Input(),joined.token,separate)==HO::Status::Ready);
    REQUIRE(separate.outsideViewMask==0);REQUIRE(separate.unknownViewMask==1);
}
TEST_CASE("Separate main rejects translated view and unsupported projection as unknown", "[geometry-page-hierarchical-original-outside]")
{
    OriginalAssetFixture asset;Joined joined(asset);auto& view=joined.views[0];
    view.kind=HP::ViewKind::Main;view.separateMainProjection=true;
    view.view=Identity();view.projection=MainProjection();view.clipNear=.25f;
    view.model[12]=100;view.model[14]=8;
    SECTION("view translation") {view.view[12]=1;}
    SECTION("projection skew") {view.projection[8]=.01f;}
    SECTION("wrong clip near") {view.clipNear=.5f;}
    HO::Result result;
    REQUIRE(HO::Build(asset.package,asset.original,joined.Input(),joined.token,result)==HO::Status::Ready);
    REQUIRE(result.requiredViewMask==1);REQUIRE(result.outsideViewMask==0);
    REQUIRE(result.unknownViewMask==1);
}
TEST_CASE("Behind-eye original is outside but partial clip and unsupported tuple remain unknown", "[geometry-page-hierarchical-original-outside]")
{
    OriginalAssetFixture asset;Joined joined(asset);auto& view=joined.views[0];HO::Result result;
    SECTION("strictly behind W plane") {view.model[14]=100;}
    SECTION("side plane straddled by original box") {view.model[12]=6.3f;}
    SECTION("near plane straddled by original box") {view.model[14]=7.7f;}
    SECTION("skew projection") {view.combinedLightVP[4]+=.2f;}
    SECTION("nonfinite matrix") {view.combinedLightVP[0]=std::numeric_limits<float>::quiet_NaN();}
    SECTION("unjoined tuple") {view.joinedNonJittered=false;}
    REQUIRE(HO::Build(asset.package,asset.original,joined.Input(),joined.token,result)==HO::Status::Ready);
    if(view.model[14]==100) {REQUIRE(result.outsideViewMask==1);REQUIRE(result.unknownViewMask==0);}
    else {REQUIRE(result.outsideViewMask==0);REQUIRE(result.unknownViewMask==1);}
}
TEST_CASE("Original outside authority and malformed captured geometry refuse transactionally", "[geometry-page-hierarchical-original-outside]")
{
    OriginalAssetFixture asset;Joined joined(asset);auto input=joined.Input();
    HO::Result kept;kept.outsideViewMask=0xdead;
    HO::Status expected=HO::Status::Invalid;
    SECTION("source identity") {asset.original.source.sourceSha256[0]^=1;expected=HO::Status::IdentityMismatch;}
    SECTION("package identity") {input.token.views.binding.identity.packageSha256[0]^=1;expected=HO::Status::IdentityMismatch;}
    SECTION("source admission epoch") {input.token.views.binding.sourceAdmissionEpoch++;expected=HO::Status::IdentityMismatch;}
    SECTION("page epoch") {input.token.pageEpoch++;expected=HO::Status::IdentityMismatch;}
    SECTION("request id") {input.token.requestId++;expected=HO::Status::IdentityMismatch;}
    SECTION("view generation") {joined.views[0].generation--;expected=HO::Status::IdentityMismatch;}
    SECTION("missing required view") {joined.views[0].enabled=false;}
    SECTION("incomplete set") {input.completeEnabledSet=false;}
    SECTION("position differs from packed vertex") {asset.original.fine.positions[0].x+=1;}
    SECTION("index outside mesh") {asset.original.coarse.indices[0]=99;}
    SECTION("nonfinite original position") {asset.original.fine.vertices[0].pos=Vector3P(
        std::numeric_limits<float>::quiet_NaN(),0,0);}
    REQUIRE(HO::Build(asset.package,asset.original,input,joined.token,kept)==expected);
    REQUIRE(kept.outsideViewMask==0xdead);
}
TEST_CASE("Decoded page vertices must equal admitted Fine IDs and serialized attributes", "[geometry-page-hierarchical-original-outside]")
{
    OriginalAssetFixture asset;auto page=asset.package.pages[0];
    REQUIRE(HO::ExactOriginalFinePageVertices(page,asset.original.fine.vertices));
    SECTION("wrong source vertex id") {page.clusters[0].originalVertexIds[0]=9;}
    SECTION("wrong source position") {page.clusters[0].vertices[0].pos=Vector3P(-.1f,-.2f,-.2f);}
    SECTION("wrong normal") {page.clusters[0].vertices[0].norm=Vector3P(0,1,0);}
    SECTION("wrong UV") {page.clusters[0].vertices[0].t0.u+=.1f;}
    SECTION("wrong tangent") {page.clusters[0].vertices[0].tangent=Vector3P(0,1,0);}
    SECTION("wrong conform") {page.clusters[0].vertices[0].conform=1;}
    REQUIRE_FALSE(HO::ExactOriginalFinePageVertices(page,asset.original.fine.vertices));
}
