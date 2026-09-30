#include <catch2/catch_test_macros.hpp>
#include <WgpuRenderer/RetailPageInstanceObservation.hpp>
#include <limits>
#include <memory>
#include <thread>

using InstanceRecord=Poseidon::render::RetailPageInstanceObservation;
namespace
{
InstanceRecord::Request VisibleRequest()
{
    InstanceRecord::Request r;r.binding={7,8,9,10,11,12,13};
    r.expected.model=13;r.expected.flags=WGR_INSTANCE_MAIN_CAMERA_ONLY;
    r.expected.world.m[0]=r.expected.world.m[5]=r.expected.world.m[10]=r.expected.world.m[15]=1;
    r.expected.world.m[12]=1200;r.expected.world.m[13]=15;r.expected.world.m[14]=1240;
    r.expected.center={1200,15,1240,1};r.previousCameraGeneration=19;return r;
}
InstanceRecord::DrainEvidence VisibleDrain(const InstanceRecord::Request& r)
{
    InstanceRecord::DrainEvidence e;e.binding=r.binding;e.frameAttempt=20;
    e.instance.getterSucceeded=e.instance.mappingExists=true;
    e.instance.rendererHandle=0x03000001;e.instance.rendererModel=33;
    e.instance.instanceEpoch=21;e.instance.state=InstanceRecord::FactState::Present;
    e.instance.actual=r.expected;e.instance.actual.model=33;return e;
}
InstanceRecord::ReturnedEvidence VisibleReturn(const InstanceRecord::DrainEvidence& d)
{
    InstanceRecord::ReturnedEvidence e;e.binding=d.binding;e.frameAttempt=d.frameAttempt;
    e.renderReturnStatus=0;e.instance=d.instance;e.cameraGetterSucceeded=true;
    auto& c=e.camera;c.version=1;c.struct_bytes=sizeof(c);c.status=1;c.camera_index=2;c.generation=20;
    c.source=1;c.render_width=c.output_width=1600;c.render_height=c.output_height=900;
    c.camera_position[0]=1210;c.camera_position[1]=20;c.camera_position[2]=1260;
    c.projection[0]=c.projection[5]=1;c.projection[10]=-1;c.projection[11]=-1;c.projection[14]=-0.2f;
    c.view[0]=c.view[5]=c.view[10]=c.view[15]=1;c.view[12]=-1210;
    e.packetCameraIndex=2;
    std::memcpy(e.packetCamera.proj.m,c.projection,sizeof(c.projection));
    std::memcpy(e.packetCamera.view.m,c.view,sizeof(c.view));
    e.packetCamera.cam_pos={1210,20,1260,4};return e;
}
}

TEST_CASE("Retail visible instance receipt joins actual consumption to one fresh returned main camera",
    "[geometry-page][retail-instance]")
{
    const auto request=VisibleRequest();const auto drain=VisibleDrain(request);const auto returned=VisibleReturn(drain);
    auto packet=std::make_shared<InstanceRecord>(request);REQUIRE(packet->Valid());
    REQUIRE(packet->Consume(drain));CHECK(packet->Observe().state==InstanceRecord::State::Requested);
    CHECK(packet->Observe().drained);CHECK_FALSE(packet->Consume(drain));
    bool published=false;
    std::thread consumer([packet,returned,&published]{published=packet->PublishReturned(returned);});consumer.join();
    REQUIRE(published);const auto observed=packet->Observe();
    CHECK(observed.state==InstanceRecord::State::Consumed);CHECK(observed.binding==request.binding);
    CHECK(observed.rendererHandle==drain.instance.rendererHandle);CHECK(observed.rendererModel==33);
    CHECK(observed.instanceEpoch==21);CHECK(observed.frameAttempt==20);CHECK(observed.camera.generation==20);
    CHECK_FALSE(packet->PublishReturned(returned));packet->ConsumeRejected();
    CHECK(packet->Observe().state==InstanceRecord::State::Consumed);
}

TEST_CASE("Retail instance source and private birth bindings cannot be substituted at either publication",
    "[geometry-page][retail-instance]")
{
    const auto request=VisibleRequest();auto drain=VisibleDrain(request);auto returned=VisibleReturn(drain);
    SECTION("owner"){++drain.binding.ownerEpoch;}
    SECTION("source"){++drain.binding.sourceAdmissionEpoch;}
    SECTION("page"){++drain.binding.pageEpoch;}
    SECTION("request"){++drain.binding.requestId;}
    SECTION("private birth"){++drain.binding.privateBirth;}
    SECTION("producer instance"){++drain.binding.producerInstance;}
    SECTION("producer model"){++drain.binding.producerModel;}
    InstanceRecord wrongDrain(request);CHECK_FALSE(wrongDrain.Consume(drain));
    CHECK(wrongDrain.Observe().refusal==InstanceRecord::Refusal::Binding);
    InstanceRecord wrongReturn(request);REQUIRE(wrongReturn.Consume(VisibleDrain(request)));
    returned.binding=drain.binding;CHECK_FALSE(wrongReturn.PublishReturned(returned));
    CHECK(wrongReturn.Observe().refusal==InstanceRecord::Refusal::Binding);
    CHECK(wrongReturn.Observe().camera.generation==0);
}

TEST_CASE("Retail consumed instance evidence requires exact actual row and historical generation for update",
    "[geometry-page][retail-instance]")
{
    auto request=VisibleRequest();request.action=InstanceRecord::Action::Update;request.previousRenderer=0x03000001;
    auto drain=VisibleDrain(request);
    SECTION("missing actual getter"){drain.instance.getterSucceeded=false;}
    SECTION("missing mapping"){drain.instance.mappingExists=false;}
    SECTION("stale generation"){drain.instance.rendererHandle=0x04000001;}
    SECTION("foreign actual model"){++drain.instance.actual.model;}
    SECTION("changed translation"){drain.instance.actual.world.m[12]+=1;}
    SECTION("non main flags"){drain.instance.actual.flags=0;}
    SECTION("rigid conform changed"){drain.instance.actual.conform2.z=1;}
    SECTION("reserved evidence"){drain.instance.reserved=1;}
    SECTION("nonfinite actual row"){drain.instance.actual.center.x=std::numeric_limits<float>::quiet_NaN();}
    SECTION("unknown state"){drain.instance.state=InstanceRecord::FactState::Invalid;}
    InstanceRecord packet(request);REQUIRE(packet.Valid());CHECK_FALSE(packet.Consume(drain));
    CHECK(packet.Observe().state==InstanceRecord::State::Rejected);
}

TEST_CASE("Retail removal requires absence of the exact previously consumed renderer instance",
    "[geometry-page][retail-instance]")
{
    auto request=VisibleRequest();request.action=InstanceRecord::Action::Remove;
    request.expectedState=InstanceRecord::Expected::Absent;request.previousRenderer=0x03000001;
    auto drain=VisibleDrain(request);drain.instance.mappingExists=false;drain.instance.rendererModel=UINT32_MAX;
    drain.instance.state=InstanceRecord::FactState::Absent;drain.instance.actual={};
    InstanceRecord accepted(request);REQUIRE(accepted.Consume(drain));
    REQUIRE(accepted.PublishReturned(VisibleReturn(drain)));
    CHECK(accepted.Observe().state==InstanceRecord::State::Consumed);
    CHECK(accepted.Observe().rendererHandle==request.previousRenderer);
    auto unknown=request;unknown.previousRenderer=0;InstanceRecord noHistory(unknown);CHECK_FALSE(noHistory.Valid());
    auto reused=drain;reused.instance.mappingExists=true;reused.instance.state=InstanceRecord::FactState::Present;
    InstanceRecord stillLive(request);CHECK_FALSE(stillLive.Consume(reused));
    auto other=drain;other.instance.rendererHandle=0x04000001;
    InstanceRecord wrongGeneration(request);CHECK_FALSE(wrongGeneration.Consume(other));
}

TEST_CASE("Retail returned frame rejects acquire skips, stale cameras and unrelated frame evidence",
    "[geometry-page][retail-instance]")
{
    const auto request=VisibleRequest();const auto drain=VisibleDrain(request);auto returned=VisibleReturn(drain);
    SECTION("acquire skip without completed candidate"){returned.cameraGetterSucceeded=false;}
    SECTION("prior completion"){returned.camera.generation=request.previousCameraGeneration;}
    SECTION("different frame attempt"){++returned.frameAttempt;}
    SECTION("render failed"){returned.renderReturnStatus=-1;}
    SECTION("camera source fallback"){returned.camera.source=0;}
    SECTION("camera tuple unavailable"){returned.camera.status=2;}
    SECTION("camera index differs"){++returned.packetCameraIndex;}
    SECTION("actual packet projection differs"){returned.packetCamera.proj.m[0]+=1;}
    SECTION("actual packet origin differs"){returned.packetCamera.cam_pos.x+=1;}
    SECTION("changed actual instance epoch"){++returned.instance.instanceEpoch;}
    SECTION("changed actual generation"){++returned.instance.rendererHandle;}
    SECTION("nonfinite camera"){returned.camera.view[0]=std::numeric_limits<float>::infinity();}
    SECTION("resolution ratio unsupported"){returned.camera.output_width=800;}
    InstanceRecord packet(request);REQUIRE(packet.Consume(drain));CHECK_FALSE(packet.PublishReturned(returned));
    CHECK(packet.Observe().state==InstanceRecord::State::Rejected);CHECK(packet.Observe().camera.generation==0);
    CHECK_FALSE(packet.PublishReturned(VisibleReturn(drain))); // terminal refusal cannot be overwritten
}

TEST_CASE("Retail instance malformed requests and frame-before-consumption fail closed",
    "[geometry-page][retail-instance]")
{
    auto request=VisibleRequest();
    SECTION("invalid owner"){request.binding.ownerEpoch=0;}
    SECTION("exhausted birth"){request.binding.privateBirth=UINT64_MAX;}
    SECTION("unknown producer"){request.binding.producerInstance=UINT32_MAX;}
    SECTION("model differs"){++request.expected.model;}
    SECTION("add cannot bind historical handle"){request.previousRenderer=1;}
    SECTION("update requires historical handle"){request.action=InstanceRecord::Action::Update;}
    SECTION("non main instance"){request.expected.flags=0;}
    InstanceRecord invalid(request);CHECK_FALSE(invalid.Valid());
    CHECK(invalid.Observe().refusal==InstanceRecord::Refusal::InvalidRequest);
    const auto validRequest=VisibleRequest();InstanceRecord notConsumed(validRequest);
    CHECK_FALSE(notConsumed.PublishReturned(VisibleReturn(VisibleDrain(validRequest))));
    CHECK(notConsumed.Observe().refusal==InstanceRecord::Refusal::Frame);
}

TEST_CASE("Visible page receipts accept the exact copied snow bit but reject other unsupported flags", "[geometry-page][retail-instance][object-snow]")
{
    auto r=VisibleRequest();r.expected.flags |= WGR_INSTANCE_SNOW_RECEIVER;
    InstanceRecord accepted(r);REQUIRE(accepted.Valid());
    auto drain=VisibleDrain(r);REQUIRE(accepted.Consume(drain));REQUIRE(accepted.PublishReturned(VisibleReturn(drain)));
    auto bad=r;bad.expected.flags |= WGR_INSTANCE_FAR_AUTHORED;CHECK_FALSE(InstanceRecord(bad).Valid());
}
