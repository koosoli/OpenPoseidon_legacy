#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalProjectedDemand.hpp>
#include <Poseidon/Graphics/Core/MatrixConversion.hpp>
#include <Poseidon/Graphics/Shadow/ShadowMath.hpp>
#include <cstring>
using namespace Poseidon;
using namespace Poseidon::GeometryPages;
namespace HP=Poseidon::GeometryPages::HierarchicalProjection;
namespace {
struct Baked {
    ClodBake bake;HierarchicalPackage package;
    Baked() {
        ShapeExport source;source.source.sourceSha256[0]=83;source.source.producerVersion=1;
        source.source.vertexLayout=sizeof(SVertex);source.source.materialMapping=1;source.source.fineRepresentation=1;
        constexpr uint32_t side=17;
        for(uint32_t y=0;y<side;++y)for(uint32_t x=0;x<side;++x) {
            const float a=float(x)/16,b=float(y)/16,z=.9f*(1-a*a)*(1-b*b);SVertex v{};
            v.pos=Vector3P(a*4,b*4,z);v.norm=Vector3P(0,0,1);v.t0={a*a,b*b};v.t1=v.t0;
            source.fine.vertices.push_back(v);source.fine.positions.push_back({a*4,b*4,z});
        }
        for(uint32_t y=0;y<side-1;++y)for(uint32_t x=0;x<side-1;++x) {
            const auto a=y*side+x,b=a+1,c=a+side,d=c+1;
            source.fine.indices.insert(source.fine.indices.end(),{a,b,d,a,d,c});source.fine.materials.insert(source.fine.materials.end(),2,4);
        }
        REQUIRE(BakeClodPilot(source,true,bake)==ClodBakeStatus::Baked);
        REQUIRE(BuildHierarchicalPackage(bake,package)==HierarchicalBuildStatus::Built);
    }
};
void Copy(std::array<float,16>& out,const GfxMatrix& matrix){std::memcpy(out.data(),&matrix,sizeof(matrix));}
HP::View ProjectedView(float z=30) {
    HP::View view;view.id=1;view.kind=HP::ViewKind::Main;view.joinedNonJittered=true;
    view.viewportWidth=1280;view.viewportHeight=720;view.clipNear=.25f;
    Matrix4 model=MIdentity;model.SetPosition(Vector3(0,0,z));GfxMatrix matrix;
    ConvertMatrix(matrix,model);Copy(view.model,matrix);
    ConvertMatrix(matrix,MIdentity);matrix._41=matrix._42=matrix._43=0;Copy(view.view,matrix);
    Matrix4 projection=MZero;projection(0,0)=1.25f;projection(1,1)=1.75f;projection(2,2)=1.01f;
    projection.SetPosition(Vector3(0,0,-1.01f*view.clipNear));ConvertProjectionMatrix(matrix,projection,0);
    matrix._33=1;matrix._43=-view.clipNear;Copy(view.projection,matrix);return view;
}
HP::View OrthographicView() {
    auto view=ProjectedView(-30);view.kind=HP::ViewKind::SolarShadow;
    // Execute the actual solar-shadow builder, including its negative RH depth
    // scale and zero-to-one WGPU clip depth. Raw column-memory uploads unchanged.
    view.projection=Poseidon::shadow::Ortho(-20,20,-20,20,.25f,100).m;
    view.viewportWidth=view.viewportHeight=2048;return view;
}
struct Joined {
    HP::Authority authority;std::vector<HP::View> views;
    explicit Joined(const HierarchicalPackage& package) {
        authority.binding={package.identity,3,5,7,11,13,1};authority.frameGeneration=19;
        views.push_back(ProjectedView());Sync();
    }
    void Sync() {
        authority.binding.requiredViewMask=0;authority.viewGenerations.fill(0);
        for(auto& view:views)if(view.enabled) {
            authority.binding.requiredViewMask|=uint64_t(1)<<(view.id-1);
            authority.viewGenerations[view.id-1]=29+view.id;
        }
        for(auto& view:views) {view.binding=authority.binding;view.frameGeneration=authority.frameGeneration;
            view.generation=authority.viewGenerations[view.id-1];}
    }
    HP::Input Input(double allowance=1)const{return {authority.binding,views,true,allowance};}
};
std::array<long double,2> Project(const HP::View& view,std::array<long double,3> point) {
    std::array<long double,4> p{point[0],point[1],point[2],1};
    for(const auto* matrix:{&view.model,&view.view,&view.projection}) {
        std::array<long double,4> next{};
        for(unsigned j=0;j<4;++j)for(unsigned i=0;i<4;++i)next[j]+=p[i]*(*matrix)[i*4+j];p=next;
    }
    REQUIRE(p[3]>0);return {p[0]/p[3]*view.viewportWidth/2,p[1]/p[3]*view.viewportHeight/2};
}
}
TEST_CASE("Localized projected baked-error indicator uses actual engine tuple math without claiming surface certification", "[geometry-page-hierarchical-projected-demand]") {
    const Baked asset;Joined joined(asset.package);HP::Demand demand;
    REQUIRE(HP::Build(asset.package,joined.Input(),joined.authority,demand)==HP::Status::Ready);
    REQUIRE(demand.authority==joined.authority);REQUIRE(demand.groupCount==asset.package.groups.size());
    bool nonzero=false;
    for(uint32_t g=0;g<demand.groupCount;++g) {
        const auto& bounds=asset.package.groups[g].simplified;if(bounds.error==FLT_MAX) {REQUIRE(demand.groupThresholds[g]==0);continue;}
        REQUIRE(demand.projectedBakedErrorIndicators[g]>=0);nonzero|=demand.projectedBakedErrorIndicators[g]>0;
        // Independent numerical pair, not a claim that the baked error bounds
        // actual surface discrepancies. Assumed displacement is exactly E/2.
        std::array<long double,3> p{bounds.center[0],bounds.center[1],bounds.center[2]};
        for(unsigned axis=0;axis<3;++axis) {auto q=p;q[axis]+=bounds.error/2;
            const auto a=Project(joined.views[0],p),b=Project(joined.views[0],q);
            REQUIRE(std::abs(a[0]-b[0])+std::abs(a[1]-b[1])<=demand.projectedBakedErrorIndicators[g]);}
    }
    REQUIRE(nonzero);
    auto doubled=joined;doubled.views[0].viewportWidth*=2;doubled.views[0].viewportHeight*=2;HP::Demand larger;
    REQUIRE(HP::Build(asset.package,doubled.Input(),doubled.authority,larger)==HP::Status::Ready);
    for(uint32_t g=0;g<demand.groupCount;++g)
        REQUIRE(larger.projectedBakedErrorIndicators[g]>=demand.projectedBakedErrorIndicators[g]*2);
}
TEST_CASE("Localized demand unions every supplied enabled view then feeds complete hierarchy fallback", "[geometry-page-hierarchical-projected-demand]") {
    const Baked asset;Joined joined(asset.package);HP::Demand relaxed,fine;
    REQUIRE(HP::Build(asset.package,joined.Input(10000),joined.authority,relaxed)==HP::Status::Ready);
    REQUIRE(HP::Build(asset.package,joined.Input(1e-10),joined.authority,fine)==HP::Status::Ready);
    std::vector<uint8_t> resident(asset.package.pages.size(),1);LocalizedHierarchicalCutPlan coarsePlan,finePlan;
    REQUIRE(PlanLocalizedHierarchicalCut(asset.package,relaxed.Localized(),{asset.package.identity,resident},coarsePlan));
    REQUIRE(coarsePlan.cut.requestedClusters==asset.package.rootClusters);
    REQUIRE(PlanLocalizedHierarchicalCut(asset.package,fine.Localized(),{asset.package.identity,resident},finePlan));
    ClodCut upstream;REQUIRE(SelectClodCut(asset.bake,0,upstream));REQUIRE(finePlan.cut.selectedClusters==upstream.clusters);
    auto second=ProjectedView(20);second.id=2;second.kind=HP::ViewKind::SolarShadow;joined.views.push_back(second);joined.Sync();
    HP::Demand united;REQUIRE(HP::Build(asset.package,joined.Input(),joined.authority,united)==HP::Status::Ready);
    for(uint32_t g=0;g<united.groupCount;++g) {
        double secondIndicator=0;
        if(asset.package.groups[g].simplified.error!=FLT_MAX) {
            REQUIRE(HP::Detail::Indicator(second,asset.package.groups[g].simplified,secondIndicator));
            REQUIRE(united.projectedBakedErrorIndicators[g]>=secondIndicator);
            if(secondIndicator>1)REQUIRE(united.groupThresholds[g]==0);
        }
    }
    std::fill(resident.begin(),resident.end(),0);for(auto p:asset.package.rootPages)resident[p]=1;
    REQUIRE(PlanLocalizedHierarchicalCut(asset.package,fine.Localized(),{asset.package.identity,resident},finePlan));
    REQUIRE(finePlan.cut.state==HierarchicalCutState::RootFallback);REQUIRE(finePlan.cut.selectedClusters==asset.package.rootClusters);
    resident[asset.package.rootPages[0]]=0;
    REQUIRE(PlanLocalizedHierarchicalCut(asset.package,fine.Localized(),{asset.package.identity,resident},finePlan));
    REQUIRE(finePlan.cut.state==HierarchicalCutState::MissingRoots);REQUIRE(finePlan.cut.selectedClusters.empty());
}
TEST_CASE("Unknown clipped near or unsupported active projection conservatively requests Fine", "[geometry-page-hierarchical-projected-demand]") {
    const Baked asset;Joined joined(asset.package);auto& view=joined.views[0];
    SECTION("unknown pass") {view.kind=HP::ViewKind::Unknown;}
    SECTION("unjoined or jittered cut") {view.joinedNonJittered=false;}
    SECTION("projection jitter") {view.projection[8]=.001f;}
    SECTION("degenerate constant-w projection") {view.projection[11]=0;}
    SECTION("near intersection") {view.model[14]=.25f;}
    SECTION("partially clipped") {view.model[12]=100;}
    SECTION("nonuniform model scale") {view.model[0]=2;view.model[5]=.5f;}
    SECTION("sheared model") {view.model[1]=.3f;}
    SECTION("translated view") {view.view[12]=1;}
    SECTION("invalid finite domain") {view.model[0]=std::numeric_limits<float>::quiet_NaN();}
    HP::Demand demand;REQUIRE(HP::Build(asset.package,joined.Input(10000),joined.authority,demand)==HP::Status::ForcedFine);
    REQUIRE(demand.forcedFineViewMask==1);
    for(uint32_t g=0;g<demand.groupCount;++g)REQUIRE(demand.groupThresholds[g]==0);
    std::vector<uint8_t> resident(asset.package.pages.size(),1);LocalizedHierarchicalCutPlan plan;
    REQUIRE(PlanLocalizedHierarchicalCut(asset.package,demand.Localized(),{asset.package.identity,resident},plan));
    ClodCut upstream;REQUIRE(SelectClodCut(asset.bake,0,upstream));REQUIRE(plan.cut.selectedClusters==upstream.clusters);
}
TEST_CASE("Projected hierarchy source view epochs completeness and finite capacities refuse transactionally", "[geometry-page-hierarchical-projected-demand]") {
    const Baked asset;Joined joined(asset.package);auto input=joined.Input();HP::Demand kept;kept.groupCount=57;
    HP::Status status=HP::Status::Invalid;
    SECTION("different package") {input.binding.identity.packageSha256[0]^=1;status=HP::Status::IdentityMismatch;}
    SECTION("source admission epoch") {input.binding.sourceAdmissionEpoch++;status=HP::Status::IdentityMismatch;}
    SECTION("world epoch") {joined.views[0].binding.worldEpoch++;status=HP::Status::IdentityMismatch;}
    SECTION("mount epoch") {joined.views[0].binding.mountEpoch++;status=HP::Status::IdentityMismatch;}
    SECTION("model birth") {joined.views[0].binding.modelBirth++;status=HP::Status::IdentityMismatch;}
    SECTION("view set epoch") {joined.views[0].binding.viewSetEpoch++;status=HP::Status::IdentityMismatch;}
    SECTION("stale frame") {joined.views[0].frameGeneration--;status=HP::Status::IdentityMismatch;}
    SECTION("stale view") {joined.views[0].generation--;status=HP::Status::IdentityMismatch;}
    SECTION("incomplete list") {input.completeEnabledSet=false;}
    SECTION("missing required enabled view") {joined.views[0].enabled=false;}
    SECTION("unknown view does not conceal later stale view") {
        auto second=ProjectedView();second.id=2;joined.views.push_back(second);joined.Sync();
        joined.views[0].kind=HP::ViewKind::Unknown;joined.views[1].generation--;input=joined.Input();status=HP::Status::IdentityMismatch;
    }
    SECTION("duplicate id") {joined.views.push_back(joined.views.front());input=joined.Input();}
    SECTION("over forty supplied rows") {joined.views.resize(41,joined.views.front());input=joined.Input();status=HP::Status::Capacity;}
    SECTION("unprovided expected generation") {joined.authority.viewGenerations[5]=8;}
    SECTION("zero epoch") {joined.authority.binding.modelBirth=0;}
    SECTION("nonfinite allowance") {input.pixelIndicatorAllowance=std::numeric_limits<double>::infinity();}
    REQUIRE(HP::Build(asset.package,input,joined.authority,kept)==status);REQUIRE(kept.groupCount==57);
}
TEST_CASE("Forty explicit views and disabled unknown rows have finite union without inferring all-pass completeness", "[geometry-page-hierarchical-projected-demand]") {
    const Baked asset;Joined joined(asset.package);
    for(uint32_t id=2;id<=HP::MaxViews;++id) {auto view=ProjectedView();view.id=id;view.kind=HP::ViewKind::Auxiliary;joined.views.push_back(view);}
    joined.Sync();HP::Demand demand;
    REQUIRE(HP::Build(asset.package,joined.Input(),joined.authority,demand)==HP::Status::Ready);
    REQUIRE(demand.authority.binding.requiredViewMask==(uint64_t(1)<<40)-1);
    joined.views.back().enabled=false;joined.views.back().kind=HP::ViewKind::Unknown;joined.views.back().joinedNonJittered=false;joined.Sync();
    REQUIRE(HP::Build(asset.package,joined.Input(),joined.authority,demand)==HP::Status::Ready);
    auto scaled=Joined(asset.package);scaled.views[0].model[0]=scaled.views[0].model[5]=scaled.views[0].model[10]=2;
    HP::Demand larger;REQUIRE(HP::Build(asset.package,scaled.Input(),scaled.authority,larger)==HP::Status::Ready);
    bool amplified=false;
    for(uint32_t g=0;g<demand.groupCount;++g)amplified|=larger.projectedBakedErrorIndicators[g]>demand.projectedBakedErrorIndicators[g];
    REQUIRE(amplified);
}

TEST_CASE("Actual shadow orthographic signed scales and combined light VP yield bounded localized indicators", "[geometry-page-hierarchical-projected-demand]") {
    const Baked asset;Joined joined(asset.package);joined.views[0]=OrthographicView();joined.Sync();
    HP::Demand demand;REQUIRE(HP::Build(asset.package,joined.Input(),joined.authority,demand)==HP::Status::Ready);
    REQUIRE(demand.forcedFineViewMask==0);REQUIRE(joined.views[0].projection[10]<0);
    for(uint32_t g=0;g<demand.groupCount;++g) {
        const auto& bounds=asset.package.groups[g].simplified;if(bounds.error==FLT_MAX)continue;
        std::array<long double,3> p{bounds.center[0],bounds.center[1],bounds.center[2]};
        for(unsigned axis=0;axis<3;++axis) {auto q=p;q[axis]+=bounds.error/2;
            const auto a=Project(joined.views[0],p),b=Project(joined.views[0],q);
            REQUIRE(std::abs(a[0]-b[0])+std::abs(a[1]-b[1])<=demand.projectedBakedErrorIndicators[g]);}
    }
    auto mirrored=joined;
    mirrored.views[0].projection=Poseidon::shadow::Ortho(20,-20,20,-20,.25f,100).m;
    HP::Demand signedDemand;REQUIRE(HP::Build(asset.package,mirrored.Input(),mirrored.authority,signedDemand)==HP::Status::Ready);
    for(uint32_t g=0;g<demand.groupCount;++g)
        REQUIRE(signedDemand.projectedBakedErrorIndicators[g]==demand.projectedBakedErrorIndicators[g]);
    auto doubled=joined;doubled.views[0].viewportWidth*=2;doubled.views[0].viewportHeight*=2;HP::Demand resized;
    REQUIRE(HP::Build(asset.package,doubled.Input(),doubled.authority,resized)==HP::Status::Ready);
    for(uint32_t g=0;g<demand.groupCount;++g)
        REQUIRE(resized.projectedBakedErrorIndicators[g]>=demand.projectedBakedErrorIndicators[g]*2);
    // Actual solar/interior tuples expose combined affine VP. Identity view
    // avoids applying its rotation/translation twice; model remains relative.
    const auto light=Poseidon::shadow::LookAt({10,10,10},{0,0,-30},{0,1,0});
    joined.views[0].projection=Poseidon::shadow::Mul(Poseidon::shadow::Ortho(-20,20,-20,20,.25f,100),light).m;
    REQUIRE(HP::Build(asset.package,joined.Input(),joined.authority,demand)==HP::Status::Ready);
    for(uint32_t g=0;g<demand.groupCount;++g) {
        const auto& bounds=asset.package.groups[g].simplified;if(bounds.error==FLT_MAX)continue;
        std::array<long double,3> p{bounds.center[0],bounds.center[1],bounds.center[2]},q=p;q[0]+=bounds.error/2;
        const auto a=Project(joined.views[0],p),b=Project(joined.views[0],q);
        REQUIRE(std::abs(a[0]-b[0])+std::abs(a[1]-b[1])<=demand.projectedBakedErrorIndicators[g]);
    }
    // Join an independent ordinary main view: union preserves BOTH indications.
    auto main=ProjectedView();main.id=2;joined.views.push_back(main);joined.Sync();
    HP::Demand united;REQUIRE(HP::Build(asset.package,joined.Input(),joined.authority,united)==HP::Status::Ready);
    for(uint32_t g=0;g<demand.groupCount;++g)
        REQUIRE(united.projectedBakedErrorIndicators[g]>=demand.projectedBakedErrorIndicators[g]);
}
TEST_CASE("Orthographic whole-envelope side and zero-to-one depth clipping conservatively requests Fine", "[geometry-page-hierarchical-projected-demand]") {
    const Baked asset;Joined joined(asset.package);joined.views[0]=OrthographicView();joined.Sync();
    auto& view=joined.views[0];
    SECTION("left or right plane") {view.model[12]=25;}
    SECTION("near light plane") {view.model[14]=-.25f;}
    SECTION("far light plane") {view.model[14]=-100;}
    SECTION("degenerate depth row") {view.projection[10]=0;view.projection[14]=.5f;}
    SECTION("invalid homogeneous affine scale") {view.projection[15]=2;}
    HP::Demand demand;REQUIRE(HP::Build(asset.package,joined.Input(10000),joined.authority,demand)==HP::Status::ForcedFine);
    REQUIRE(demand.forcedFineViewMask==1);
    for(uint32_t g=0;g<demand.groupCount;++g)REQUIRE(demand.groupThresholds[g]==0);
}
