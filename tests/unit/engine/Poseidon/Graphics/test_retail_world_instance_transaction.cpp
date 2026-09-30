#include <catch2/catch_test_macros.hpp>
#include <WgpuRenderer/RetailWorldInstanceTransaction.hpp>
#include <limits>
#include <memory>
#include <thread>

using WorldTransaction=Poseidon::render::RetailWorldInstanceTransaction;
namespace
{
constexpr uint32_t OriginalHandle=0x03000001,PageHandle=0x05000002;
WorldTransaction::RowExpectation WorldRow(uint32_t producerModel,uint32_t rendererModel,
    uint64_t modelBirth,uint32_t flags)
{
    WorldTransaction::RowExpectation e;e.model={producerModel,rendererModel,modelBirth};
    e.row.model=producerModel;e.row.flags=flags;
    e.row.world.m[0]=e.row.world.m[5]=e.row.world.m[10]=e.row.world.m[15]=1;
    e.row.world.m[12]=1200;e.row.world.m[13]=15;e.row.world.m[14]=1240;
    e.row.center={1200,15,1240,1};return e;
}
WorldTransaction::Request WorldRequest(WorldTransaction::Action action=WorldTransaction::Action::Takeover)
{
    WorldTransaction::Request r;
    r.binding={7,8,9,10,11,12,13,14,15,16,17};r.action=action;
    r.originalRendererHandle=OriginalHandle;r.previousCameraGeneration=19;
    r.originalBefore=WorldRow(20,30,40,action==WorldTransaction::Action::Takeover?0:WGR_INSTANCE_OTHER_VIEWS_ONLY);
    r.originalAfter=WorldRow(20,30,40,action==WorldTransaction::Action::Restore?0:WGR_INSTANCE_OTHER_VIEWS_ONLY);
    if(action!=WorldTransaction::Action::Takeover)
    {r.pageBefore=WorldRow(21,31,41,WGR_INSTANCE_MAIN_CAMERA_ONLY);r.pageRendererHandle=PageHandle;}
    if(action!=WorldTransaction::Action::Restore)
        r.pageAfter=WorldRow(action==WorldTransaction::Action::UpdatePage?22:21,
            action==WorldTransaction::Action::UpdatePage?32:31,
            action==WorldTransaction::Action::UpdatePage?42:41,WGR_INSTANCE_MAIN_CAMERA_ONLY);
    return r;
}
WorldTransaction::InstanceEvidence WorldPresent(const WorldTransaction::RowExpectation& row,
    uint32_t producer,uint64_t birth,uint32_t handle,uint64_t epoch)
{
    WorldTransaction::InstanceEvidence e;e.producerState=WorldTransaction::ProducerState::Present;
    e.producerInstance=producer;e.producerModel=row.model.producerModel;
    e.consumerInstanceBirth=birth;e.consumerModelBirth=row.model.modelBirth;
    e.mappedRendererHandle=handle;e.mappedRendererModel=row.model.rendererModel;e.getterSucceeded=true;
    e.cpu.version=1;e.cpu.bytes=sizeof(e.cpu);e.cpu.status=WGR_INSTANCE_CPU_FACT_PRESENT;
    e.cpu.queried_handle=handle;e.cpu.instance_epoch=epoch;e.cpu.resolved_slot=(handle&0x00ffffffu)-1;
    e.cpu.row=row.row;e.cpu.row.model=row.model.rendererModel;return e;
}
WorldTransaction::InstanceEvidence WorldUnmapped(uint32_t producer)
{
    WorldTransaction::InstanceEvidence e;e.producerState=WorldTransaction::ProducerState::Unmapped;
    e.producerInstance=producer;return e;
}
WorldTransaction::BeforeEvidence WorldBefore(const WorldTransaction::Request& r)
{
    WorldTransaction::BeforeEvidence b;b.binding=r.binding;b.frameAttempt=50;b.instanceEpoch=51;
    b.original=WorldPresent(r.originalBefore,r.binding.originalProducerInstance,
        r.binding.originalInstanceBirth,r.originalRendererHandle,b.instanceEpoch);
    b.page=r.action==WorldTransaction::Action::Takeover?WorldUnmapped(r.binding.pageProducerInstance):
        WorldPresent(r.pageBefore,r.binding.pageProducerInstance,r.binding.pageInstanceBirth,r.pageRendererHandle,b.instanceEpoch);
    return b;
}
WorldTransaction::DrainEvidence WorldDrain(const WorldTransaction::Request& r)
{
    WorldTransaction::DrainEvidence d;d.binding=r.binding;d.before=WorldBefore(r);
    d.frameAttempt=d.before.frameAttempt;d.instanceEpoch=53;
    d.original=WorldPresent(r.originalAfter,r.binding.originalProducerInstance,
        r.binding.originalInstanceBirth,r.originalRendererHandle,d.instanceEpoch);
    if(r.action==WorldTransaction::Action::Restore)
    {
        d.page=WorldUnmapped(r.binding.pageProducerInstance);d.page.getterSucceeded=true;
        auto& c=d.page.cpu;c.version=1;c.bytes=sizeof(c);c.status=WGR_INSTANCE_CPU_FACT_ABSENT;
        c.queried_handle=r.pageRendererHandle;c.instance_epoch=d.instanceEpoch;c.resolved_slot=UINT32_MAX;
    }
    else d.page=WorldPresent(r.pageAfter,r.binding.pageProducerInstance,
        r.binding.pageInstanceBirth,r.action==WorldTransaction::Action::Takeover?PageHandle:r.pageRendererHandle,d.instanceEpoch);
    return d;
}
WorldTransaction::ReturnedEvidence WorldReturn(const WorldTransaction::DrainEvidence& d)
{
    WorldTransaction::ReturnedEvidence e;e.binding=d.binding;e.frameAttempt=d.frameAttempt;
    e.instanceEpoch=d.instanceEpoch;e.original=d.original;e.page=d.page;e.renderReturnStatus=0;
    e.cameraGetterSucceeded=true;e.cameraGenerationBeforeRender=23;
    auto& c=e.camera;c.version=1;c.struct_bytes=sizeof(c);c.status=1;c.source=1;c.generation=24;c.camera_index=2;
    c.render_width=c.output_width=1600;c.render_height=c.output_height=900;
    c.projection[0]=c.projection[5]=1;c.projection[10]=-1;c.projection[11]=-1;c.projection[14]=-0.2f;
    c.view[0]=c.view[5]=c.view[10]=c.view[15]=1;c.view[12]=-1210;
    c.camera_position[0]=1210;c.camera_position[1]=20;c.camera_position[2]=1260;
    e.packetCameraIndex=2;std::memcpy(e.packetCamera.proj.m,c.projection,sizeof(c.projection));
    std::memcpy(e.packetCamera.view.m,c.view,sizeof(c.view));e.packetCamera.cam_pos={1210,20,1260,4};return e;
}
}

TEST_CASE("Atomic retail world pair joins takeover restore and page switch to actual returned CPU rows",
    "[geometry-page][retail-world-transaction]")
{
    for(const auto action:{WorldTransaction::Action::Takeover,WorldTransaction::Action::Restore,WorldTransaction::Action::UpdatePage})
    {
        const auto r=WorldRequest(action);const auto d=WorldDrain(r);const auto e=WorldReturn(d);
        auto receipt=std::make_shared<WorldTransaction>(r);REQUIRE(receipt->Valid());
        REQUIRE(receipt->CheckBefore(d.before)==WorldTransaction::Refusal::None);
        CHECK_FALSE(receipt->Observe().drained);REQUIRE(receipt->Consume(d));
        CHECK(receipt->Observe().state==WorldTransaction::State::Requested);CHECK(receipt->Observe().drained);
        CHECK_FALSE(receipt->Consume(d));bool published=false;
        std::thread consumer([receipt,e,&published]{published=receipt->PublishReturned(e);});consumer.join();
        REQUIRE(published);const auto snapshot=receipt->Observe();
        CHECK(snapshot.state==WorldTransaction::State::Consumed);CHECK(snapshot.binding==r.binding);
        CHECK(snapshot.action==action);CHECK(snapshot.originalRendererHandle==OriginalHandle);
        CHECK(snapshot.pageRendererHandle==PageHandle);CHECK(snapshot.beforeInstanceEpoch==51);
        CHECK(snapshot.instanceEpoch==53);CHECK(snapshot.frameAttempt==50);CHECK(snapshot.camera.generation==24);
        CHECK_FALSE(receipt->PublishReturned(e));receipt->ConsumeRejected();
        CHECK(receipt->Observe().state==WorldTransaction::State::Consumed);
    }
}

TEST_CASE("Atomic world before evidence rejects changed original and page rows before either mutation",
    "[geometry-page][retail-world-transaction]")
{
    const auto r=WorldRequest(WorldTransaction::Action::UpdatePage);auto d=WorldDrain(r);
    SECTION("actual moved original"){d.before.original.cpu.row.world.m[12]+=1;}
    SECTION("original shape/model mapping swapped"){++d.before.original.cpu.row.model;}
    SECTION("original flags already restored"){d.before.original.cpu.row.flags=0;}
    SECTION("original getter unavailable"){d.before.original.getterSucceeded=false;}
    SECTION("original renderer generation changed"){d.before.original.cpu.queried_handle=0x04000001;}
    SECTION("recycled exact renderer handle still matching row but new world birth")
        {++d.before.original.consumerInstanceBirth;}
    SECTION("same source model handle reused with different birth"){++d.before.original.consumerModelBirth;}
    SECTION("different producer index despite identical actual row"){++d.before.original.producerInstance;}
    SECTION("before page moved"){d.before.page.cpu.row.center.x+=1;}
    SECTION("before page cut changed"){++d.before.page.producerModel;}
    SECTION("page producer alias changed birth"){++d.before.page.consumerInstanceBirth;}
    SECTION("different before row epoch"){++d.before.page.cpu.instance_epoch;}
    SECTION("resolved raw slot lies"){++d.before.original.cpu.resolved_slot;}
    SECTION("before fact reserved field"){d.before.original.cpu.reserved=1;}
    SECTION("before epoch absent"){d.before.instanceEpoch=0;}
    WorldTransaction receipt(r);REQUIRE(receipt.Valid());
    CHECK(receipt.CheckBefore(d.before)!=WorldTransaction::Refusal::None);
    CHECK(receipt.Observe().state==WorldTransaction::State::Requested); // pure preflight has no side effect
    CHECK_FALSE(receipt.Consume(d));CHECK(receipt.Observe().state==WorldTransaction::State::Rejected);
}

TEST_CASE("Fresh world takeover requires affirmative unmapped producer table evidence and complete pair",
    "[geometry-page][retail-world-transaction]")
{
    const auto r=WorldRequest();auto d=WorldDrain(r);
    SECTION("default absent is unknown"){d.before.page={};}
    SECTION("wrong queried producer"){++d.before.page.producerInstance;}
    SECTION("a reused producer birth still exists"){d.before.page.consumerInstanceBirth=17;}
    SECTION("a renderer mapping still exists"){d.before.page.mappedRendererHandle=PageHandle;}
    SECTION("a model mapping still exists"){d.before.page.mappedRendererModel=31;}
    SECTION("fresh before incorrectly pretends historical getter")
        {d.before.page.getterSucceeded=true;d.before.page.cpu.status=WGR_INSTANCE_CPU_FACT_ABSENT;}
    SECTION("only hide original consumed"){d.page=WorldUnmapped(r.binding.pageProducerInstance);}
    SECTION("only page Add consumed"){d.original=WorldPresent(r.originalBefore,r.binding.originalProducerInstance,
        r.binding.originalInstanceBirth,OriginalHandle,d.instanceEpoch);}
    SECTION("after page binding translated differently"){++d.page.mappedRendererModel;}
    SECTION("after page uses original handle"){d.page.mappedRendererHandle=OriginalHandle;}
    SECTION("same after row epoch not shared"){++d.page.cpu.instance_epoch;}
    SECTION("after contains a conflicting visibility flag"){d.page.cpu.row.flags|=WGR_INSTANCE_OTHER_VIEWS_ONLY;}
    SECTION("unchanged epoch is not a consumed transition"){d.instanceEpoch=d.before.instanceEpoch;
        d.original.cpu.instance_epoch=d.page.cpu.instance_epoch=d.instanceEpoch;}
    SECTION("before belongs to another frame"){++d.before.frameAttempt;}
    WorldTransaction receipt(r);REQUIRE(receipt.Valid());CHECK_FALSE(receipt.Consume(d));
    CHECK(receipt.Observe().state==WorldTransaction::State::Rejected);
}

TEST_CASE("World restore requires known-present historical page and exact actual absence without resurrection",
    "[geometry-page][retail-world-transaction]")
{
    const auto r=WorldRequest(WorldTransaction::Action::Restore);auto d=WorldDrain(r);
    SECTION("unknown prior page cannot certify remove"){d.before.page=WorldUnmapped(r.binding.pageProducerInstance);}
    SECTION("another historical handle absent"){d.page.cpu.queried_handle=0x06000002;}
    SECTION("recycled page producer still mapped"){d.page.producerState=WorldTransaction::ProducerState::Present;
        d.page.consumerInstanceBirth=123;d.page.mappedRendererHandle=PageHandle;}
    SECTION("actual absence getter missing"){d.page.getterSucceeded=false;}
    SECTION("absence status unsupported"){d.page.cpu.status=WGR_INSTANCE_CPU_FACT_UNSUPPORTED;}
    SECTION("absence row not zero"){d.page.cpu.row.world.m[0]=1;}
    SECTION("absence raw slot not sentinel"){d.page.cpu.resolved_slot=1;}
    SECTION("original still hidden"){d.original.cpu.row.flags=WGR_INSTANCE_OTHER_VIEWS_ONLY;}
    SECTION("replacement original cannot be restored"){++d.original.consumerInstanceBirth;}
    SECTION("original model retirement/reuse"){++d.original.consumerModelBirth;}
    WorldTransaction receipt(r);REQUIRE(receipt.Valid());CHECK_FALSE(receipt.Consume(d));
    CHECK(receipt.Observe().state==WorldTransaction::State::Rejected);
}

TEST_CASE("Every immutable world binding stamp is checked at before drain and returned publication",
    "[geometry-page][retail-world-transaction]")
{
    const auto r=WorldRequest();auto changed=r.binding;
    SECTION("owner"){++changed.ownerEpoch;}
    SECTION("world object birth"){++changed.worldObjectBirth;}
    SECTION("source admission"){++changed.sourceAdmissionEpoch;}
    SECTION("shape birth"){++changed.shapeBirth;}
    SECTION("shape query revision"){++changed.shapeQueryRevision;}
    SECTION("page epoch"){++changed.pageEpoch;}
    SECTION("request"){++changed.requestId;}
    SECTION("original producer"){++changed.originalProducerInstance;}
    SECTION("page producer"){++changed.pageProducerInstance;}
    SECTION("original consumer birth"){++changed.originalInstanceBirth;}
    SECTION("page consumer birth"){++changed.pageInstanceBirth;}
    auto d=WorldDrain(r);d.before.binding=changed;WorldTransaction before(r);
    CHECK(before.CheckBefore(d.before)==WorldTransaction::Refusal::Binding);CHECK_FALSE(before.Consume(d));
    d=WorldDrain(r);d.binding=changed;WorldTransaction drain(r);CHECK_FALSE(drain.Consume(d));
    CHECK(drain.Observe().refusal==WorldTransaction::Refusal::Binding);
    d=WorldDrain(r);auto e=WorldReturn(d);e.binding=changed;WorldTransaction returned(r);REQUIRE(returned.Consume(d));
    CHECK_FALSE(returned.PublishReturned(e));CHECK(returned.Observe().refusal==WorldTransaction::Refusal::Binding);
}

TEST_CASE("World pair returned receipt rejects skipped stale and unrelated camera completions",
    "[geometry-page][retail-world-transaction]")
{
    const auto r=WorldRequest();const auto d=WorldDrain(r);auto e=WorldReturn(d);
    SECTION("acquire skipped no completed candidate"){e.cameraGetterSucceeded=false;}
    SECTION("acquire skipped reuses newer than request but already completed camera")
        {e.camera.generation=e.cameraGenerationBeforeRender;}
    SECTION("precall generation absent despite known prior tuple"){e.cameraGenerationBeforeRender=0;}
    SECTION("precall counter exhausted"){e.cameraGenerationBeforeRender=UINT64_MAX;}
    SECTION("old completed generation"){e.camera.generation=r.previousCameraGeneration;}
    SECTION("another frame"){++e.frameAttempt;}
    SECTION("render failed"){e.renderReturnStatus=-1;}
    SECTION("unsupported source"){e.camera.source=0;}
    SECTION("camera not ready"){e.camera.status=2;}
    SECTION("another packet camera"){++e.packetCameraIndex;}
    SECTION("packet matrix changed"){e.packetCamera.proj.m[0]+=1;}
    SECTION("packet position changed"){e.packetCamera.cam_pos.x+=1;}
    SECTION("new after observation epoch"){++e.instanceEpoch;}
    SECTION("original row changed after drain"){e.original.cpu.row.center.y+=1;}
    SECTION("page row changed after drain"){e.page.cpu.row.world.m[14]+=1;}
    SECTION("page renderer generation changed after drain"){e.page.mappedRendererHandle=0x06000002;}
    SECTION("original birth changed after drain"){++e.original.consumerInstanceBirth;}
    SECTION("page model birth changed after drain"){++e.page.consumerModelBirth;}
    SECTION("nonfinite camera"){e.camera.view[1]=std::numeric_limits<float>::quiet_NaN();}
    SECTION("unsupported scaled output"){e.camera.output_width=800;}
    WorldTransaction receipt(r);REQUIRE(receipt.Consume(d));CHECK_FALSE(receipt.PublishReturned(e));
    CHECK(receipt.Observe().state==WorldTransaction::State::Rejected);CHECK(receipt.Observe().camera.generation==0);
    CHECK_FALSE(receipt.PublishReturned(WorldReturn(d))); // terminal failure cannot be thawed by unrelated later frame
}

TEST_CASE("World pair malformed requests cannot introduce extra instances move source or conflicting flags",
    "[geometry-page][retail-world-transaction]")
{
    auto r=WorldRequest();
    SECTION("invalid owner"){r.binding.ownerEpoch=0;}
    SECTION("exhausted world birth"){r.binding.worldObjectBirth=UINT64_MAX;}
    SECTION("same producer for both rows"){r.binding.pageProducerInstance=r.binding.originalProducerInstance;}
    SECTION("missing original handle"){r.originalRendererHandle=0;}
    SECTION("generation bits alone no slot"){r.originalRendererHandle=0x03000000;}
    SECTION("takeover cannot claim historical page"){r.pageRendererHandle=PageHandle;}
    SECTION("original model swapped in takeover"){++r.originalAfter.model.producerModel;++r.originalAfter.row.model;}
    SECTION("page placed somewhere different"){r.pageAfter.row.world.m[12]+=1;}
    SECTION("original transformed during split"){r.originalAfter.row.center.y+=1;}
    SECTION("source flags conflict"){r.originalAfter.row.flags|=WGR_INSTANCE_MAIN_CAMERA_ONLY;}
    SECTION("page flags conflict"){r.pageAfter.row.flags|=WGR_INSTANCE_OTHER_VIEWS_ONLY;}
    SECTION("conform not rigid"){r.originalBefore.row.conform0.x=1;}
    SECTION("unknown model birth"){r.pageAfter.model.modelBirth=0;}
    SECTION("row model disagrees with producer mapping"){++r.pageAfter.row.model;}
    SECTION("missing actual renderer model"){r.pageAfter.model.rendererModel=UINT32_MAX;}
    SECTION("nonfinite matrix"){r.pageAfter.row.world.m[4]=std::numeric_limits<float>::infinity();}
    SECTION("invalid camera prior"){r.previousCameraGeneration=UINT64_MAX;}
    SECTION("unknown action"){r.action=static_cast<WorldTransaction::Action>(4);}
    WorldTransaction invalid(r);CHECK_FALSE(invalid.Valid());
    CHECK(invalid.Observe().refusal==WorldTransaction::Refusal::InvalidRequest);
    WorldTransaction notDrained{WorldRequest()};CHECK_FALSE(notDrained.PublishReturned(WorldReturn(WorldDrain(WorldRequest()))));
    CHECK(notDrained.Observe().refusal==WorldTransaction::Refusal::Frame);
}

TEST_CASE("World page switch only changes page model and restore never accepts an absent prior source",
    "[geometry-page][retail-world-transaction]")
{
    auto r=WorldRequest(WorldTransaction::Action::UpdatePage);
    SECTION("switch missing historical page"){r.pageRendererHandle=0;}
    SECTION("switch aliases original renderer instance"){r.pageRendererHandle=OriginalHandle;}
    SECTION("switch has no model change"){r.pageAfter=r.pageBefore;}
    SECTION("switch changes page placement"){r.pageAfter.row.center.z+=1;}
    SECTION("switch hides source another way"){r.originalAfter.row.flags=0;}
    WorldTransaction invalid(r);CHECK_FALSE(invalid.Valid());
    const auto restore=WorldRequest(WorldTransaction::Action::Restore);auto d=WorldDrain(restore);
    d.before.original.producerState=WorldTransaction::ProducerState::Unmapped;
    WorldTransaction noSource(restore);CHECK_FALSE(noSource.Consume(d));
}

TEST_CASE("World page transactions preserve only explicit snow receiver authority", "[geometry-page][retail-world][object-snow]")
{
    for (auto action : {WorldTransaction::Action::Takeover, WorldTransaction::Action::UpdatePage, WorldTransaction::Action::Restore}) {
        auto r=WorldRequest(action);
        for (auto* row : {&r.originalBefore,&r.originalAfter,&r.pageBefore,&r.pageAfter})
            if (row->model.producerModel != UINT32_MAX) row->row.flags |= WGR_INSTANCE_SNOW_RECEIVER;
        WorldTransaction accepted(r);REQUIRE(accepted.Valid());
        auto wrong=r;wrong.originalAfter.row.flags ^= WGR_INSTANCE_SNOW_RECEIVER;
        CHECK_FALSE(WorldTransaction(wrong).Valid());
        for (auto forbidden : {uint32_t(WGR_INSTANCE_CANOPY_BUSH),uint32_t(WGR_INSTANCE_FAR_AUTHORED),uint32_t(256)}) {
            auto bad=r;bad.originalBefore.row.flags |= forbidden;
            CHECK_FALSE(WorldTransaction(bad).Valid());
        }
    }
}
