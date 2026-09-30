#pragma once
#include <WgpuRenderer/include/wgpu_renderer.hpp>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <mutex>

namespace Poseidon::render
{
// One immutable private-instance request. The caller collects actual renderer
// records; this helper validates their association, not their provenance. No
// sourceCurrent callback, renderer call, allocation or GPU readback occurs here.
// Consumed means one exact instance operation plus a joined returned main frame,
// not pixels, an all-pass cut, queue completion or physical allocation release.
class RetailPageInstanceObservation
{
public:
    enum class Expected : uint8_t { Present=1, Absent=2 };
    enum class Action : uint8_t { Add=1, Update=2, Remove=3 };
    enum class FactState : uint8_t { Invalid=0, Present=1, Absent=2 };
    enum class State : uint8_t { Requested, Consumed, Rejected };
    enum class Refusal : uint8_t { None, InvalidRequest, Binding, Getter, Instance, Frame, Camera };
    struct Binding {
        uint64_t ownerEpoch=0,sourceAdmissionEpoch=0,pageEpoch=0,requestId=0,privateBirth=0;
        uint32_t producerInstance=UINT32_MAX,producerModel=UINT32_MAX;
        bool operator==(const Binding&) const=default;
    };
    struct Request {
        Binding binding;
        Action action=Action::Add;
        Expected expectedState=Expected::Present;
        WgrInstance expected{}; // immutable producer model; actual renderer model is joined separately
        uint32_t previousRenderer=0; // mandatory for removal; optional same-handle constraint for update
        uint64_t previousCameraGeneration=0;
    };
    struct InstanceEvidence {
        bool getterSucceeded=false,mappingExists=false;
        uint32_t rendererHandle=0,rendererModel=UINT32_MAX,reserved=0;
        uint64_t instanceEpoch=0;
        FactState state=FactState::Invalid;
        WgrInstance actual{}; // collected actual CPU row, not the enqueued request
    };
    struct DrainEvidence {
        Binding binding;
        uint64_t frameAttempt=0;
        InstanceEvidence instance;
    };
    struct ReturnedEvidence {
        Binding binding;
        uint64_t frameAttempt=0;
        int32_t renderReturnStatus=-1;
        InstanceEvidence instance; // reobserved after the actual render call
        bool cameraGetterSucceeded=false;
        WgrMainCameraTuple camera{};
        uint32_t packetCameraIndex=UINT32_MAX;
        WgrCamera packetCamera{}; // exact camera from that same submitted frame packet
    };
    struct Snapshot {
        State state=State::Requested;
        Refusal refusal=Refusal::None;
        Binding binding;
        Expected expectedState=Expected::Present;
        bool drained=false;
        uint64_t frameAttempt=0,instanceEpoch=0;
        uint32_t rendererHandle=0,rendererModel=UINT32_MAX;
        int32_t renderReturnStatus=-1;
        WgrMainCameraTuple camera{};
    };

    explicit RetailPageInstanceObservation(const Request& request) noexcept :_request(request)
    {
        _snapshot.binding=request.binding;_snapshot.expectedState=request.expectedState;
        _valid=ValidRequest(request);
        if(!_valid)Reject(Refusal::InvalidRequest);
    }
    bool Valid() const noexcept {return _valid;}
    const Request& GetRequest() const noexcept {return _request;}
    // Render-owner only: called AFTER instance drain. Consumption alone remains
    // pending until a fresh completed main tuple is joined below.
    bool Consume(const DrainEvidence& evidence) noexcept
    {
        std::lock_guard lock(_mutex);
        if(_state.load(std::memory_order_relaxed)!=State::Requested||_snapshot.drained)return false;
        if(evidence.binding!=_request.binding){Reject(Refusal::Binding);return false;}
        if(!ValidSerial(evidence.frameAttempt)){Reject(Refusal::Frame);return false;}
        const auto refusal=ValidateInstance(evidence.instance);
        if(refusal!=Refusal::None){Reject(refusal);return false;}
        _snapshot.drained=true;_snapshot.frameAttempt=evidence.frameAttempt;
        _snapshot.instanceEpoch=evidence.instance.instanceEpoch;
        _snapshot.rendererHandle=evidence.instance.rendererHandle;
        _snapshot.rendererModel=evidence.instance.rendererModel;
        return true;
    }
    bool PublishReturned(const ReturnedEvidence& evidence) noexcept
    {
        std::lock_guard lock(_mutex);
        if(_state.load(std::memory_order_relaxed)!=State::Requested)return false;
        if(evidence.binding!=_request.binding){Reject(Refusal::Binding);return false;}
        _snapshot.renderReturnStatus=evidence.renderReturnStatus;
        if(!_snapshot.drained||evidence.frameAttempt!=_snapshot.frameAttempt||evidence.renderReturnStatus!=0)
        {Reject(Refusal::Frame);return false;}
        const auto refusal=ValidateInstance(evidence.instance);
        if(refusal!=Refusal::None){Reject(refusal);return false;}
        if(evidence.instance.rendererHandle!=_snapshot.rendererHandle||
           evidence.instance.rendererModel!=_snapshot.rendererModel||
           evidence.instance.instanceEpoch!=_snapshot.instanceEpoch)
        {Reject(Refusal::Instance);return false;}
        if(!ValidCamera(evidence)){Reject(Refusal::Camera);return false;}
        _snapshot.camera=evidence.camera;_snapshot.state=State::Consumed;
        _state.store(State::Consumed,std::memory_order_release);return true;
    }
    void ConsumeRejected(Refusal reason=Refusal::Getter) noexcept
    {
        std::lock_guard lock(_mutex);
        if(_state.load(std::memory_order_relaxed)==State::Requested)
            Reject(reason==Refusal::None?Refusal::Getter:reason);
    }
    Snapshot Observe() const noexcept
    {
        (void)_state.load(std::memory_order_acquire);
        std::lock_guard lock(_mutex);return _snapshot;
    }
private:
    static bool ValidSerial(uint64_t value) noexcept {return value&&value!=UINT64_MAX;}
    static bool ValidRigid(const WgrInstance& v) noexcept
    {
        if((v.flags & ~WGR_INSTANCE_SURFACE_RECEIVERS)!=WGR_INSTANCE_MAIN_CAMERA_ONLY||v.cull_radius||v._pad||
           v.center.w<=0||!std::isfinite(v.center.x)||!std::isfinite(v.center.y)||
           !std::isfinite(v.center.z)||!std::isfinite(v.center.w))return false;
        for(float f:v.world.m)if(!std::isfinite(f))return false;
        for(const auto* plane:{&v.conform0,&v.conform1,&v.conform2})
            if(plane->x!=0||plane->y!=0||plane->z!=0||plane->w!=0)return false;
        return true;
    }
    static bool ValidRequest(const Request& r) noexcept
    {
        const auto& b=r.binding;
        return ValidSerial(b.ownerEpoch)&&ValidSerial(b.sourceAdmissionEpoch)&&ValidSerial(b.pageEpoch)&&
            ValidSerial(b.requestId)&&ValidSerial(b.privateBirth)&&b.producerInstance!=UINT32_MAX&&
            b.producerModel!=UINT32_MAX&&r.expected.model==b.producerModel&&ValidRigid(r.expected)&&
            (r.expectedState==Expected::Present||r.expectedState==Expected::Absent)&&
            ((r.action==Action::Add&&r.expectedState==Expected::Present&&!r.previousRenderer)||
             (r.action==Action::Update&&r.expectedState==Expected::Present&&r.previousRenderer)||
             (r.action==Action::Remove&&r.expectedState==Expected::Absent&&r.previousRenderer))&&
            r.previousCameraGeneration!=UINT64_MAX;
    }
    Refusal ValidateInstance(const InstanceEvidence& e) const noexcept
    {
        if(!_valid)return Refusal::InvalidRequest;
        if(!e.getterSucceeded)return Refusal::Getter;
        if(e.reserved||!e.rendererHandle||!ValidSerial(e.instanceEpoch))return Refusal::Instance;
        if(_request.previousRenderer&&e.rendererHandle!=_request.previousRenderer)return Refusal::Instance;
        if(_request.expectedState==Expected::Absent)
            return !e.mappingExists&&e.state==FactState::Absent&&e.rendererModel==UINT32_MAX?
                Refusal::None:Refusal::Instance;
        if(!e.mappingExists||e.state!=FactState::Present||e.rendererModel==UINT32_MAX||
           e.actual.model!=e.rendererModel||!ValidRigid(e.actual))return Refusal::Instance;
        auto expected=_request.expected;expected.model=e.rendererModel;
        return std::memcmp(&expected,&e.actual,sizeof(expected))==0?Refusal::None:Refusal::Instance;
    }
    bool ValidCamera(const ReturnedEvidence& e) const noexcept
    {
        const auto& c=e.camera;
        if(!e.cameraGetterSucceeded||c.version!=1||c.struct_bytes!=sizeof(c)||c.status!=1||c.reserved||
           !ValidSerial(c.generation)||c.generation<=_request.previousCameraGeneration||
           c.source<1||c.source>4||c.camera_index!=e.packetCameraIndex||
           e.packetCameraIndex==UINT32_MAX||!c.render_width||!c.render_height||
           c.render_width>16384||c.render_height>16384||c.render_width!=c.output_width||
           c.render_height!=c.output_height||c.camera_position[3]!=0)return false;
        for(float f:c.projection)if(!std::isfinite(f))return false;
        for(float f:c.view)if(!std::isfinite(f))return false;
        for(float f:c.camera_position)if(!std::isfinite(f))return false;
        return std::memcmp(c.projection,e.packetCamera.proj.m,sizeof(c.projection))==0&&
            std::memcmp(c.view,e.packetCamera.view.m,sizeof(c.view))==0&&
            std::memcmp(c.camera_position,&e.packetCamera.cam_pos,sizeof(float)*3)==0;
    }
    void Reject(Refusal reason) noexcept
    {
        _snapshot.state=State::Rejected;_snapshot.refusal=reason;_snapshot.camera={};
        _state.store(State::Rejected,std::memory_order_release);
    }
    const Request _request;
    bool _valid=false;
    mutable std::mutex _mutex;
    std::atomic<State> _state{State::Requested};
    Snapshot _snapshot;
};
static_assert(sizeof(RetailPageInstanceObservation)<=1024);
}
