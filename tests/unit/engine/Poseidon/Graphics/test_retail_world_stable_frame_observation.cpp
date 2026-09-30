#include <catch2/catch_test_macros.hpp>
#include <WgpuRenderer/RetailWorldStableFrameObservation.hpp>
#include <atomic>
#include <memory>
#include <thread>

using StableFrame=Poseidon::render::RetailWorldStableFrameObservation;
namespace
{
constexpr uint32_t StableOriginalHandle=0x03000001,StablePageHandle=0x05000002;
StableFrame::RowExpectation StableRow(uint32_t producer,uint32_t renderer,uint32_t flags)
{
    StableFrame::RowExpectation r;r.model={producer,renderer,uint64_t(producer)+1};
    r.row.model=producer;r.row.flags=flags;
    r.row.world.m[0]=r.row.world.m[5]=r.row.world.m[10]=r.row.world.m[15]=1;
    r.row.world.m[12]=1200;r.row.world.m[13]=15;r.row.world.m[14]=1240;
    r.row.center={1200,15,1240,1};return r;
}
StableFrame::Request StableRequest()
{
    StableFrame::Request r;r.binding={7,8,9,10,11,12,13,14,15,16,17};
    r.original=StableRow(20,30,WGR_INSTANCE_OTHER_VIEWS_ONLY);
    r.page=StableRow(21,31,WGR_INSTANCE_MAIN_CAMERA_ONLY);
    r.originalRendererHandle=StableOriginalHandle;r.pageRendererHandle=StablePageHandle;
    r.previousCameraGeneration=19;return r;
}
StableFrame::InstanceEvidence StablePresent(const StableFrame::RowExpectation& row,
    uint32_t producer,uint64_t birth,uint32_t handle,uint64_t epoch)
{
    StableFrame::InstanceEvidence e;e.producerState=StableFrame::Pair::ProducerState::Present;
    e.producerInstance=producer;e.producerModel=row.model.producerModel;
    e.consumerInstanceBirth=birth;e.consumerModelBirth=row.model.modelBirth;
    e.mappedRendererHandle=handle;e.mappedRendererModel=row.model.rendererModel;e.getterSucceeded=true;
    e.cpu.version=1;e.cpu.bytes=sizeof(e.cpu);e.cpu.status=WGR_INSTANCE_CPU_FACT_PRESENT;
    e.cpu.queried_handle=handle;e.cpu.instance_epoch=epoch;e.cpu.resolved_slot=(handle&0x00ffffffu)-1;
    e.cpu.row=row.row;e.cpu.row.model=row.model.rendererModel;return e;
}
StableFrame::DrainEvidence StableDrain(const StableFrame::Request& r,uint64_t attempt=50,
    uint64_t cameraBefore=19,uint64_t epoch=60)
{
    StableFrame::DrainEvidence d;d.binding=r.binding;d.frameAttempt=attempt;d.instanceEpoch=epoch;
    d.original=StablePresent(r.original,r.binding.originalProducerInstance,
        r.binding.originalInstanceBirth,r.originalRendererHandle,epoch);
    d.page=StablePresent(r.page,r.binding.pageProducerInstance,r.binding.pageInstanceBirth,r.pageRendererHandle,epoch);
    d.cameraGetterSucceeded=true;d.cameraGenerationBeforeRender=cameraBefore;return d;
}
StableFrame::ReturnedEvidence StableReturn(const StableFrame::DrainEvidence& d,uint64_t generation=20)
{
    StableFrame::ReturnedEvidence e;e.binding=d.binding;e.frameAttempt=d.frameAttempt;e.instanceEpoch=d.instanceEpoch;
    e.original=d.original;e.page=d.page;e.renderReturnStatus=0;e.cameraGetterSucceeded=true;
    e.cameraGenerationBeforeRender=d.cameraGenerationBeforeRender;
    auto& c=e.camera;c.version=1;c.struct_bytes=sizeof(c);c.status=1;c.source=1;c.generation=generation;c.camera_index=2;
    c.render_width=c.output_width=1600;c.render_height=c.output_height=900;
    c.projection[0]=c.projection[5]=1;c.projection[10]=-1;c.projection[11]=-1;c.projection[14]=-0.2f;
    c.view[0]=c.view[5]=c.view[10]=c.view[15]=1;
    c.camera_position[0]=1210;c.camera_position[1]=20;c.camera_position[2]=1260;
    e.packetCameraIndex=2;std::memcpy(e.packetCamera.proj.m,c.projection,sizeof(c.projection));
    std::memcpy(e.packetCamera.view.m,c.view,sizeof(c.view));e.packetCamera.cam_pos={1210,20,1260,1};return e;
}
}

TEST_CASE("Stable world observations join repeated empty-queue frames without artificial instance writes",
    "[geometry-page][retail-world-stable-frame]")
{
    const auto r=StableRequest();StableFrame tracker(r);REQUIRE(tracker.Valid());
    const auto first=StableDrain(r);REQUIRE(tracker.Arm(first));
    CHECK(tracker.Observe().state==StableFrame::State::Unknown);
    REQUIRE(tracker.PublishReturned(StableReturn(first)));
    const auto accepted=tracker.Observe();CHECK(accepted.state==StableFrame::State::Ready);
    CHECK(accepted.binding==r.binding);CHECK(accepted.instanceEpoch==60);
    CHECK(accepted.cameraGenerationBeforeRender==19);CHECK(accepted.camera.generation==20);
    // Same actual CPU epoch is allowed across completed frames with no mutations.
    const auto second=StableDrain(r,51,20,60);REQUIRE(tracker.Arm(second));
    CHECK(tracker.Observe().state==StableFrame::State::Unknown);
    CHECK(tracker.Observe().camera.generation==0);
    REQUIRE(tracker.PublishReturned(StableReturn(second,21)));
    CHECK(tracker.Observe().acceptedFrames==2);CHECK(tracker.Observe().camera.generation==21);
}

TEST_CASE("Stable world request requires exact rigid same-placement other-view and main-only rows",
    "[geometry-page][retail-world-stable-frame]")
{
    auto r=StableRequest();
    SECTION("original conventional flags"){r.original.row.flags=0;}
    SECTION("page also in other views"){r.page.row.flags|=WGR_INSTANCE_OTHER_VIEWS_ONLY;}
    SECTION("page placement differs"){r.page.row.world.m[12]+=1;}
    SECTION("page conforming geometry"){r.page.row.conform2.z=1;}
    SECTION("zero scale"){r.page.row.center.w=0;}
    SECTION("producer-model mismatch"){++r.page.row.model;}
    SECTION("renderer-handle alias"){r.pageRendererHandle=r.originalRendererHandle;}
    SECTION("unknown previous returned camera"){r.previousCameraGeneration=0;}
    SECTION("world lifetime missing"){r.binding.worldObjectBirth=0;}
    SECTION("model birth absent"){r.original.model.modelBirth=0;}
    SECTION("unknown source admission"){r.binding.sourceAdmissionEpoch=UINT64_MAX;}
    StableFrame tracker(r);CHECK_FALSE(tracker.Valid());
    CHECK(tracker.Observe().refusal==StableFrame::Refusal::InvalidRequest);
    CHECK_FALSE(tracker.Arm(StableDrain(r)));
}

TEST_CASE("Whole-drain world facts refuse changed mappings epochs and wrap-alias births",
    "[geometry-page][retail-world-stable-frame]")
{
    const auto r=StableRequest();auto d=StableDrain(r);StableFrame tracker(r);
    SECTION("producer binding stale"){++d.binding.pageEpoch;}
    SECTION("whole drain changed source placement"){d.original.cpu.row.world.m[12]+=1;}
    SECTION("normal source restored"){d.original.cpu.row.flags=0;}
    SECTION("same renderer handle recycled after 256 generations"){++d.original.consumerInstanceBirth;}
    SECTION("same model handle replaced"){++d.page.consumerModelBirth;}
    SECTION("producer mapping wrong"){++d.page.mappedRendererHandle;}
    SECTION("model mapping wrong"){++d.original.mappedRendererModel;}
    SECTION("CPU getter unavailable"){d.page.getterSucceeded=false;}
    SECTION("page absent"){d.page.cpu.status=WGR_INSTANCE_CPU_FACT_ABSENT;}
    SECTION("not a shared whole-drain epoch"){++d.page.cpu.instance_epoch;}
    SECTION("literal CPU layout wrong"){d.original.cpu.bytes=0;}
    SECTION("literal CPU reserved field"){d.page.cpu.reserved=1;}
    SECTION("raw pre-call tuple unavailable"){d.cameraGetterSucceeded=false;}
    SECTION("raw pre-call generation older than transition ACK"){d.cameraGenerationBeforeRender=18;}
    CHECK_FALSE(tracker.Arm(d));CHECK(tracker.Observe().state==StableFrame::State::Unknown);
    CHECK(tracker.Observe().camera.generation==0);
}

TEST_CASE("Stable returned frame rejects post-drain mutation skipped render and foreign camera packet",
    "[geometry-page][retail-world-stable-frame]")
{
    const auto r=StableRequest();const auto d=StableDrain(r);auto e=StableReturn(d);
    StableFrame tracker(r);REQUIRE(tracker.Arm(d));
    SECTION("render acquired no frame"){e.renderReturnStatus=1;}
    SECTION("render error"){e.renderReturnStatus=-1;}
    SECTION("foreign attempt"){++e.frameAttempt;}
    SECTION("source changed after drain"){++e.binding.shapeQueryRevision;}
    SECTION("actual table mutation after drain"){++e.instanceEpoch;}
    SECTION("page row changed"){e.page.cpu.row.center.z+=1;}
    SECTION("source lifetime changed"){++e.original.consumerInstanceBirth;}
    SECTION("stale returned tuple"){e.camera.generation=d.cameraGenerationBeforeRender;}
    SECTION("getter unavailable"){e.cameraGetterSucceeded=false;}
    SECTION("unknown returned status"){e.camera.status=6;}
    SECTION("wrong actual pre-call generation"){++e.cameraGenerationBeforeRender;}
    SECTION("camera matrix from different frame"){e.packetCamera.view.m[0]+=1;}
    SECTION("camera origin from different frame"){e.packetCamera.cam_pos.x+=1;}
    SECTION("different submitted camera index"){++e.packetCameraIndex;}
    SECTION("non 1-to-1 viewport"){e.camera.output_width=800;}
    CHECK_FALSE(tracker.PublishReturned(e));CHECK(tracker.Observe().state==StableFrame::State::Unknown);
    CHECK(tracker.Observe().camera.generation==0);CHECK(tracker.Observe().acceptedFrames==0);
}

TEST_CASE("Failed stable frames revoke previous observations and recover only on a fresh completed tuple",
    "[geometry-page][retail-world-stable-frame]")
{
    const auto r=StableRequest();StableFrame tracker(r);const auto d=StableDrain(r);
    REQUIRE(tracker.Arm(d));REQUIRE(tracker.PublishReturned(StableReturn(d)));
    auto second=StableDrain(r,51,20);REQUIRE(tracker.Arm(second));auto failed=StableReturn(second,21);
    failed.renderReturnStatus=1;CHECK_FALSE(tracker.PublishReturned(failed));
    CHECK(tracker.Observe().state==StableFrame::State::Unknown);CHECK(tracker.Observe().camera.generation==0);
    CHECK_FALSE(tracker.PublishReturned(StableReturn(second,21))); // no pending arm, not repairable replay
    CHECK_FALSE(tracker.Arm(second)); // a second arm cannot reuse this frame attempt
    auto third=StableDrain(r,52,21);REQUIRE(tracker.Arm(third));
    CHECK_FALSE(tracker.PublishReturned(StableReturn(third,21))); // failed generation fenced
    auto fourth=StableDrain(r,53,21);REQUIRE(tracker.Arm(fourth));
    REQUIRE(tracker.PublishReturned(StableReturn(fourth,22)));
    CHECK(tracker.Observe().acceptedFrames==2);CHECK(tracker.Observe().camera.generation==22);
    tracker.Invalidate(StableFrame::Refusal::Binding);
    CHECK(tracker.Observe().state==StableFrame::State::Unknown);CHECK(tracker.Observe().camera.generation==0);
}

TEST_CASE("Stable observation publication remains a coherent bounded snapshot across reader threads",
    "[geometry-page][retail-world-stable-frame]")
{
    const auto r=StableRequest();auto tracker=std::make_shared<StableFrame>(r);
    std::atomic<bool> done=false,bad=false;
    std::thread reader([&]{while(!done.load(std::memory_order_acquire)) {
        const auto s=tracker->Observe();
        if(s.binding!=r.binding||(s.state==StableFrame::State::Ready&&
           (!s.camera.generation||s.camera.generation<=s.cameraGenerationBeforeRender||s.instanceEpoch!=60)))bad=true;
    }});
    bool allPublished=true;
    for(uint64_t i=0;i<32;++i) {
        const auto d=StableDrain(r,50+i,19+i);
        allPublished=tracker->Arm(d)&&tracker->PublishReturned(StableReturn(d,20+i))&&allPublished;
    }
    done.store(true,std::memory_order_release);reader.join();
    REQUIRE(allPublished);CHECK_FALSE(bad.load());CHECK(tracker->Observe().acceptedFrames==32);
}

TEST_CASE("Stable world page frames retain matching snow proof without widening view authority", "[geometry-page][retail-world-stable-frame][object-snow]")
{
    auto r=StableRequest();r.original.row.flags |= WGR_INSTANCE_SNOW_RECEIVER;r.page.row.flags |= WGR_INSTANCE_SNOW_RECEIVER;
    StableFrame accepted(r);REQUIRE(accepted.Valid());
    auto drain=StableDrain(r);REQUIRE(accepted.Arm(drain));REQUIRE(accepted.PublishReturned(StableReturn(drain)));
    auto lost=r;lost.page.row.flags &= ~WGR_INSTANCE_SNOW_RECEIVER;CHECK_FALSE(StableFrame(lost).Valid());
    auto unsupported=r;unsupported.page.row.flags |= WGR_INSTANCE_CANOPY_BUSH;CHECK_FALSE(StableFrame(unsupported).Valid());
}
