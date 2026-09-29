#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalBinaryDemand.hpp>
#include <initializer_list>
#include <limits>
namespace B=Poseidon::GeometryPages::HierarchicalBinaryDemand;
namespace {
B::Binding Authority()
{
    B::Binding binding;
    binding.identity.source.sourceSha256[0]=53;
    binding.identity.packageSha256[0]=79;
    binding.identity.source.vertexLayout=68;binding.identity.source.materialMapping=1;
    binding.identity.source.fineRepresentation=1;
    binding.privateOwnerEpoch=2;binding.sourceAdmissionEpoch=3;
    binding.pageEpoch=5;binding.modelBirth=7;return binding;
}
B::Observation MakeObservation(uint64_t generation,bool root=true,bool wantsFine=false)
{
    B::Observation result;result.binding=Authority();result.frameGeneration=generation;
    result.requiredViewMask=31;result.currentlyRoot=root;result.wantsFine=wantsFine;
    result.allowance=root?B::Policy::RefineAllowance:B::Policy::CoarsenAllowance;
    return result;
}
}
TEST_CASE("Hierarchy binary demand immediately refines from the complete union key rather than its maximum indicator", "[geometry-page-hierarchical-binary-demand]")
{
    B::Policy policy(Authority());auto fine=MakeObservation(1,true,true);
    fine.maximumIndicator=0;REQUIRE(policy.Observe(fine)==B::Decision::Refine);
    auto root=MakeObservation(2);root.maximumIndicator=500;
    REQUIRE(policy.Observe(root)==B::Decision::Hold);
    REQUIRE(policy.Stats().refine==1);REQUIRE(policy.Stats().observations==2);
    REQUIRE(policy.SafeRootObservations()==0);
}
TEST_CASE("Hierarchy coarsening requires three fresh known lower-allowance observations", "[geometry-page-hierarchical-binary-demand]")
{
    B::Policy policy(Authority());
    REQUIRE(policy.Observe(MakeObservation(10,false))==B::Decision::Hold);
    REQUIRE(policy.SafeRootObservations()==1);
    REQUIRE(policy.Observe(MakeObservation(11,false))==B::Decision::Hold);
    REQUIRE(policy.SafeRootObservations()==2);
    REQUIRE(policy.Observe(MakeObservation(12,false))==B::Decision::Coarsen);
    REQUIRE(policy.SafeRootObservations()==0);REQUIRE(policy.Stats().coarsen==1);
    REQUIRE(policy.Observe(MakeObservation(13))==B::Decision::Hold);
    REQUIRE(policy.Observe(MakeObservation(14,true,true))==B::Decision::Refine);
    REQUIRE(policy.Observe(MakeObservation(15,false))==B::Decision::Hold);
    REQUIRE(policy.SafeRootObservations()==1);
}
TEST_CASE("Unknown required views force Fine and interrupt a coarsening streak", "[geometry-page-hierarchical-binary-demand]")
{
    B::Policy policy(Authority());auto unknown=MakeObservation(1);unknown.forcedFine=true;
    REQUIRE(policy.Observe(unknown)==B::Decision::Refine);
    REQUIRE(policy.Observe(MakeObservation(2,false))==B::Decision::Hold);
    REQUIRE(policy.Observe(MakeObservation(3,false))==B::Decision::Hold);
    unknown=MakeObservation(4,false);unknown.forcedFine=true;
    REQUIRE(policy.Observe(unknown)==B::Decision::Hold);
    REQUIRE(policy.SafeRootObservations()==0);
    for(uint64_t generation=5;generation<7;++generation)
        REQUIRE(policy.Observe(MakeObservation(generation,false))==B::Decision::Hold);
    REQUIRE(policy.Observe(MakeObservation(7,false))==B::Decision::Coarsen);
    REQUIRE(policy.Stats().forcedFine==2);
}
TEST_CASE("Pending hierarchy work consumes no camera generation or policy counters", "[geometry-page-hierarchical-binary-demand]")
{
    B::Policy policy(Authority());REQUIRE(policy.Observe(MakeObservation(1,false))==B::Decision::Hold);
    const auto stats=policy.Stats();auto pending=MakeObservation(2,false);pending.pending=true;
    for(unsigned i=0;i<20;++i)REQUIRE(policy.Observe(pending)==B::Decision::Hold);
    REQUIRE(policy.LastGeneration()==1);REQUIRE(policy.SafeRootObservations()==1);
    REQUIRE(policy.Stats()==stats);
    pending.pending=false;REQUIRE(policy.Observe(pending)==B::Decision::Hold);
    REQUIRE(policy.SafeRootObservations()==2);
    REQUIRE(policy.Observe(MakeObservation(3,false))==B::Decision::Coarsen);
}
TEST_CASE("Hierarchy authority changes and repaired replay cannot authorize either direction", "[geometry-page-hierarchical-binary-demand]")
{
    for(unsigned field=0;field<15;++field) {
        B::Policy policy(Authority());REQUIRE(policy.Observe(MakeObservation(1,false))==B::Decision::Hold);
        auto changed=MakeObservation(2,false);auto& key=changed.binding;
        switch(field) {
            case 0:++key.privateOwnerEpoch;break;
            case 1:++key.sourceAdmissionEpoch;break;
            case 2:++key.pageEpoch;break;
            case 3:++key.modelBirth;break;
            case 4:++key.identity.packageSha256[0];break;
            case 5:++key.identity.adapterVersion;break;
            case 6:++key.identity.source.sourceSha256[0];break;
            case 7:++key.identity.source.geometryOptions;break;
            case 8:++key.identity.source.materialOptions;break;
            case 9:++key.identity.source.producerVersion;break;
            case 10:++key.identity.source.coarseRepresentation;break;
            case 11:++key.identity.source.fineRepresentation;break;
            case 12:++key.identity.source.vertexLayout;break;
            case 13:++key.identity.source.materialMapping;break;
            case 14:key.privateOwnerEpoch=0;break;
        }
        REQUIRE(policy.Observe(changed)==B::Decision::Refused);
        REQUIRE(policy.SafeRootObservations()==0);REQUIRE(policy.LastGeneration()==2);
        REQUIRE(policy.Stats().observations==1);
        REQUIRE(policy.Observe(MakeObservation(2,true,true))==B::Decision::Refused);
        REQUIRE(policy.Observe(MakeObservation(3,false))==B::Decision::Hold);
        REQUIRE(policy.Observe(MakeObservation(4,false))==B::Decision::Hold);
        REQUIRE(policy.Observe(MakeObservation(5,false))==B::Decision::Coarsen);
    }
}
TEST_CASE("Replayed older and sentinel frame generations reset safe-root evidence", "[geometry-page-hierarchical-binary-demand]")
{
    for(uint64_t generation:{uint64_t(0),uint64_t(1),uint64_t(2),UINT64_MAX}) {
        B::Policy policy(Authority());policy.Observe(MakeObservation(1,false));policy.Observe(MakeObservation(2,false));
        REQUIRE(policy.Observe(MakeObservation(generation,false))==B::Decision::Refused);
        REQUIRE(policy.SafeRootObservations()==0);REQUIRE(policy.LastGeneration()==2);
        REQUIRE(policy.Observe(MakeObservation(3,false))==B::Decision::Hold);
        REQUIRE(policy.Observe(MakeObservation(4,false))==B::Decision::Hold);
        REQUIRE(policy.Observe(MakeObservation(5,false))==B::Decision::Coarsen);
    }
}
TEST_CASE("Changed enabled-view policy cannot inherit prior safe-root observations", "[geometry-page-hierarchical-binary-demand]")
{
    B::Policy policy(Authority());policy.Observe(MakeObservation(1,false));policy.Observe(MakeObservation(2,false));
    auto view=MakeObservation(3,false);view.requiredViewMask=1;
    REQUIRE(policy.Observe(view)==B::Decision::Hold);REQUIRE(policy.SafeRootObservations()==1);
    view.frameGeneration=4;REQUIRE(policy.Observe(view)==B::Decision::Hold);
    view.frameGeneration=5;REQUIRE(policy.Observe(view)==B::Decision::Coarsen);
    view=MakeObservation(6,false,true);view.requiredViewMask=uint64_t(1)|(uint64_t(1)<<39);
    REQUIRE(policy.Observe(view)==B::Decision::Hold);REQUIRE(policy.SafeRootObservations()==0);
}
TEST_CASE("Malformed numeric mode or incomplete view observations fail closed transactionally", "[geometry-page-hierarchical-binary-demand]")
{
    for(unsigned invalid=0;invalid<9;++invalid) {
        B::Policy policy(Authority());policy.Observe(MakeObservation(1,false));policy.Observe(MakeObservation(2,false));
        auto bad=MakeObservation(3,false);
        switch(invalid) {
            case 0:bad.maximumIndicator=-1;break;
            case 1:bad.maximumIndicator=std::numeric_limits<double>::infinity();break;
            case 2:bad.maximumIndicator=std::numeric_limits<double>::quiet_NaN();break;
            case 3:bad.allowance=4;break; // Fine must be re-evaluated at the lower allowance.
            case 4:bad.allowance=std::numeric_limits<double>::quiet_NaN();break;
            case 5:bad.requiredViewMask=0;break;
            case 6:bad.requiredViewMask=2;break; // Main omitted.
            case 7:bad.requiredViewMask=uint64_t(1)|(uint64_t(1)<<40);break;
            case 8:bad.currentlyRoot=true;break; // Root needs allowance4, not3.
        }
        REQUIRE(policy.Observe(bad)==B::Decision::Refused);
        REQUIRE(policy.LastGeneration()==3);REQUIRE(policy.SafeRootObservations()==0);
        REQUIRE(policy.Stats().observations==2);REQUIRE(policy.Stats().coarsen==0);
        REQUIRE(policy.Stats().refine==0);REQUIRE(policy.Stats().refused==1);
        REQUIRE(policy.Observe(MakeObservation(3,false))==B::Decision::Refused);
    }
}
TEST_CASE("An unauthenticated hierarchy policy binding has no decision authority", "[geometry-page-hierarchical-binary-demand]")
{
    for(unsigned field=0;field<14;++field) {
        auto binding=Authority();
        switch(field) {
            case 0:binding.identity.source.sourceSha256.fill(0);break;
            case 1:binding.identity.packageSha256.fill(0);break;
            case 2:binding.identity.source.producerVersion=0;break;
            case 3:binding.identity.source.vertexLayout=0;break;
            case 4:binding.identity.source.materialMapping=0;break;
            case 5:binding.identity.adapterVersion=0;break;
            case 6:binding.privateOwnerEpoch=0;break;
            case 7:binding.sourceAdmissionEpoch=0;break;
            case 8:binding.modelBirth=0;break;
            case 9:binding.pageEpoch=0;break;
            case 10:binding.privateOwnerEpoch=UINT64_MAX;break;
            case 11:binding.sourceAdmissionEpoch=UINT64_MAX;break;
            case 12:binding.pageEpoch=UINT64_MAX;break;
            case 13:binding.modelBirth=UINT64_MAX;break;
        }
        B::Policy policy(binding);auto observation=MakeObservation(1,true,true);observation.binding=binding;
        REQUIRE(policy.Observe(observation)==B::Decision::Refused);
        REQUIRE(policy.Stats().refine==0);
    }
}
