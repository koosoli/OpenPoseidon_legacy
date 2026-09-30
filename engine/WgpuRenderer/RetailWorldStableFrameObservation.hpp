#pragma once
#include <WgpuRenderer/RetailWorldInstanceTransaction.hpp>

namespace Poseidon::render
{
// CPU association for an immutable, already-ACKed original64/page32 pair.
// Arm is called AFTER the complete instance queue, immediately before render;
// PublishReturned joins the SAME unchanged rows/epoch to the actual successful
// returned main tuple and submitted packet. No mutation, source admission, GPU
// completion, geometric error, pixels or other-view coverage is certified here.
// The owner replaces this tracker before changing either expected row/lifetime.
class RetailWorldStableFrameObservation
{
public:
    using Pair=RetailWorldInstanceTransaction;
    using Binding=Pair::Binding;
    using RowExpectation=Pair::RowExpectation;
    using InstanceEvidence=Pair::InstanceEvidence;
    using ReturnedEvidence=Pair::ReturnedEvidence;
    enum class State : uint8_t { Unknown,Ready };
    enum class Refusal : uint8_t { None,InvalidRequest,Binding,Frame,Instance,Camera };
    struct Request {
        Binding binding;
        RowExpectation original,page;
        uint32_t originalRendererHandle=0,pageRendererHandle=0;
        uint64_t previousCameraGeneration=0;
    };
    struct DrainEvidence {
        Binding binding;
        uint64_t frameAttempt=0,instanceEpoch=0;
        InstanceEvidence original,page;
        bool cameraGetterSucceeded=false;
        uint64_t cameraGenerationBeforeRender=0;
    };
    struct Snapshot {
        State state=State::Unknown;
        Refusal refusal=Refusal::None;
        Binding binding;
        uint64_t frameAttempt=0,instanceEpoch=0,cameraGenerationBeforeRender=0;
        uint64_t acceptedFrames=0,refusedFrames=0;
        WgrMainCameraTuple camera{};
    };

    explicit RetailWorldStableFrameObservation(const Request& request) noexcept :_request(request)
    {
        _snapshot.binding=request.binding;_valid=ValidRequest(request);
        _lastCameraGeneration=request.previousCameraGeneration;
        if(!_valid)Revoke(Refusal::InvalidRequest);
    }
    bool Valid() const noexcept {return _valid;}
    const Request& GetRequest() const noexcept {return _request;}
    bool Arm(const DrainEvidence& e) noexcept
    {
        std::lock_guard lock(_mutex);
        // A ready snapshot never survives into a new attempted render. Even a
        // failed/foreign/skipped arm revokes it instead of replaying an old view.
        _armed=false;Clear();_snapshot.frameAttempt=e.frameAttempt;
        if(!_valid){Revoke(Refusal::InvalidRequest);return false;}
        if(e.binding!=_request.binding){Revoke(Refusal::Binding);return false;}
        if(!Serial(e.frameAttempt)||e.frameAttempt<=_lastAttempt||!Serial(e.instanceEpoch))
        {Revoke(Refusal::Frame);return false;}
        _lastAttempt=e.frameAttempt;
        if(!Rows(e.original,e.page,e.instanceEpoch)){Revoke(Refusal::Instance);return false;}
        if(!e.cameraGetterSucceeded||!Serial(e.cameraGenerationBeforeRender)||
            e.cameraGenerationBeforeRender<_lastCameraGeneration)
        {Revoke(Refusal::Camera);return false;}
        _drain=e;_snapshot.instanceEpoch=e.instanceEpoch;
        _snapshot.cameraGenerationBeforeRender=e.cameraGenerationBeforeRender;_armed=true;return true;
    }
    bool PublishReturned(const ReturnedEvidence& e) noexcept
    {
        std::lock_guard lock(_mutex);
        const bool armed=_armed;_armed=false;
        const auto previousCamera=_lastCameraGeneration;
        // Fence known failed generations too: repairing the claimed facts cannot
        // turn the same returned tuple into another fresh observation.
        if(Serial(e.camera.generation)&&e.camera.generation>_lastCameraGeneration)
            _lastCameraGeneration=e.camera.generation;
        if(!_valid){Revoke(Refusal::InvalidRequest);return false;}
        if(e.binding!=_request.binding){Revoke(Refusal::Binding);return false;}
        if(!armed||e.frameAttempt!=_drain.frameAttempt||e.renderReturnStatus!=0)
        {Revoke(Refusal::Frame);return false;}
        if(e.instanceEpoch!=_drain.instanceEpoch||!Rows(e.original,e.page,e.instanceEpoch))
        {Revoke(Refusal::Instance);return false;}
        if(e.cameraGenerationBeforeRender!=_drain.cameraGenerationBeforeRender||
            !Camera(e,previousCamera)){Revoke(Refusal::Camera);return false;}
        _snapshot.state=State::Ready;_snapshot.refusal=Refusal::None;
        _snapshot.frameAttempt=e.frameAttempt;_snapshot.instanceEpoch=e.instanceEpoch;
        _snapshot.camera=e.camera;Increment(_snapshot.acceptedFrames);
        _state.store(State::Ready,std::memory_order_release);return true;
    }
    void Invalidate(Refusal reason=Refusal::Frame) noexcept
    {
        std::lock_guard lock(_mutex);_armed=false;
        Revoke(reason==Refusal::None?Refusal::Frame:reason);
    }
    Snapshot Observe() const noexcept
    {
        (void)_state.load(std::memory_order_acquire);
        std::lock_guard lock(_mutex);return _snapshot;
    }
private:
    static bool Serial(uint64_t v) noexcept {return v&&v!=UINT64_MAX;}
    static bool Handle(uint32_t v) noexcept {return (v&0x00ffffffu)!=0;}
    static void Increment(uint64_t& v) noexcept {if(v!=UINT64_MAX)++v;}
    static bool Row(const RowExpectation& e,uint32_t flags) noexcept
    {
        const auto& v=e.row;
        if(e.model.producerModel==UINT32_MAX||e.model.rendererModel==UINT32_MAX||
            !Serial(e.model.modelBirth)||v.model!=e.model.producerModel||(v.flags & ~WGR_INSTANCE_SURFACE_RECEIVERS)!=flags||
            v.cull_radius||v._pad||!std::isfinite(v.center.w)||v.center.w<=0||
            !std::isfinite(v.center.x)||!std::isfinite(v.center.y)||!std::isfinite(v.center.z))return false;
        for(float f:v.world.m)if(!std::isfinite(f))return false;
        for(const auto* p:{&v.conform0,&v.conform1,&v.conform2})
            if(p->x!=0||p->y!=0||p->z!=0||p->w!=0)return false;
        return true;
    }
    static bool ValidRequest(const Request& r) noexcept
    {
        const auto& b=r.binding;
        for(auto v:{b.ownerEpoch,b.worldObjectBirth,b.sourceAdmissionEpoch,b.shapeBirth,
            b.shapeQueryRevision,b.pageEpoch,b.requestId,b.originalInstanceBirth,b.pageInstanceBirth,
            r.previousCameraGeneration})if(!Serial(v))return false;
        if(b.originalProducerInstance==UINT32_MAX||b.pageProducerInstance==UINT32_MAX||
            b.originalProducerInstance==b.pageProducerInstance||!Handle(r.originalRendererHandle)||
            !Handle(r.pageRendererHandle)||r.originalRendererHandle==r.pageRendererHandle||
            !Row(r.original,WGR_INSTANCE_OTHER_VIEWS_ONLY)||!Row(r.page,WGR_INSTANCE_MAIN_CAMERA_ONLY))return false;
        auto a=r.original.row,c=r.page.row;
        if ((a.flags & WGR_INSTANCE_SURFACE_RECEIVERS) != (c.flags & WGR_INSTANCE_SURFACE_RECEIVERS)) return false;
        a.model=c.model=0;a.flags=c.flags=0;
        return std::memcmp(&a,&c,sizeof(a))==0;
    }
    static bool Present(const InstanceEvidence& e,const RowExpectation& row,uint32_t producer,
        uint64_t birth,uint32_t handle,uint64_t epoch) noexcept
    {
        const auto& c=e.cpu;
        if(e.producerState!=Pair::ProducerState::Present||e.producerInstance!=producer||
            e.producerModel!=row.model.producerModel||e.consumerInstanceBirth!=birth||
            e.consumerModelBirth!=row.model.modelBirth||e.mappedRendererHandle!=handle||
            e.mappedRendererModel!=row.model.rendererModel||!e.getterSucceeded||
            c.version!=1||c.bytes!=sizeof(c)||c.status!=WGR_INSTANCE_CPU_FACT_PRESENT||
            c.queried_handle!=handle||c.instance_epoch!=epoch||!Serial(epoch)||c.reserved||
            c.resolved_slot!=((handle&0x00ffffffu)-1))return false;
        auto expected=row.row;expected.model=row.model.rendererModel;
        return std::memcmp(&expected,&c.row,sizeof(expected))==0;
    }
    bool Rows(const InstanceEvidence& original,const InstanceEvidence& page,uint64_t epoch) const noexcept
    {
        const auto& b=_request.binding;
        return Present(original,_request.original,b.originalProducerInstance,b.originalInstanceBirth,
            _request.originalRendererHandle,epoch)&&Present(page,_request.page,b.pageProducerInstance,
            b.pageInstanceBirth,_request.pageRendererHandle,epoch);
    }
    static bool Camera(const ReturnedEvidence& e,uint64_t previous) noexcept
    {
        const auto& c=e.camera;
        if(!e.cameraGetterSucceeded||c.version!=1||c.struct_bytes!=sizeof(c)||c.status!=1||c.reserved||
            !Serial(c.generation)||c.generation<=previous||c.generation<=e.cameraGenerationBeforeRender||
            c.source<1||c.source>4||c.camera_index!=e.packetCameraIndex||e.packetCameraIndex==UINT32_MAX||
            !c.render_width||!c.render_height||c.render_width>16384||c.render_height>16384||
            c.render_width!=c.output_width||c.render_height!=c.output_height||c.camera_position[3]!=0)return false;
        for(float f:c.projection)if(!std::isfinite(f))return false;
        for(float f:c.view)if(!std::isfinite(f))return false;
        for(float f:c.camera_position)if(!std::isfinite(f))return false;
        return std::memcmp(c.projection,e.packetCamera.proj.m,sizeof(c.projection))==0&&
            std::memcmp(c.view,e.packetCamera.view.m,sizeof(c.view))==0&&
            std::memcmp(c.camera_position,&e.packetCamera.cam_pos,sizeof(float)*3)==0;
    }
    void Clear() noexcept
    {
        _snapshot.state=State::Unknown;_snapshot.refusal=Refusal::None;
        _snapshot.instanceEpoch=0;_snapshot.cameraGenerationBeforeRender=0;_snapshot.camera={};
        _state.store(State::Unknown,std::memory_order_release);
    }
    void Revoke(Refusal reason) noexcept
    {
        Clear();_snapshot.refusal=reason;Increment(_snapshot.refusedFrames);
        _state.store(State::Unknown,std::memory_order_release);
    }
    const Request _request;
    bool _valid=false,_armed=false;
    uint64_t _lastAttempt=0,_lastCameraGeneration=0;
    DrainEvidence _drain;
    mutable std::mutex _mutex;
    std::atomic<State> _state{State::Unknown};
    Snapshot _snapshot;
};
static_assert(sizeof(RetailWorldStableFrameObservation)<=4096);
} // namespace Poseidon::render
