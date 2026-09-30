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
// A bounded, immutable TWO-row consumer receipt, not source/world admission.
// The render owner must collect CheckBefore evidence BEFORE either mutation,
// execute the pair without interleaving another producer transaction, and collect
// Consume evidence AFTER the entire instance queue has drained. It must bind the
// producer table births to the actual operations, not infer births from a matching
// renderer row: renderer generations wrap after 256 slot recycles.
//
// Caller-owned source/current-mount, model-resource, object-lifetime and CPU shadow
// coverage checks remain separate. Consumed means exact CPU rows in a freshly
// returned main frame; it is neither pixels, GPU completion nor all-pass proof.
class RetailWorldInstanceTransaction
{
public:
    static constexpr uint32_t MaxInstances=2;
    enum class Action : uint8_t { Takeover=1, Restore=2, UpdatePage=3 };
    enum class ProducerState : uint8_t { Invalid=0, Present=1, Unmapped=2 };
    enum class State : uint8_t { Requested, Consumed, Rejected };
    enum class Refusal : uint8_t { None, InvalidRequest, Binding, Before, Instance, Frame, Camera };
    struct Binding {
        uint64_t ownerEpoch=0,worldObjectBirth=0,sourceAdmissionEpoch=0,shapeBirth=0;
        uint64_t shapeQueryRevision=0,pageEpoch=0,requestId=0;
        uint32_t originalProducerInstance=UINT32_MAX,pageProducerInstance=UINT32_MAX;
        uint64_t originalInstanceBirth=0,pageInstanceBirth=0;
        bool operator==(const Binding&) const=default;
    };
    struct ModelIdentity {
        uint32_t producerModel=UINT32_MAX,rendererModel=UINT32_MAX;
        uint64_t modelBirth=0;
        bool operator==(const ModelIdentity&) const=default;
    };
    struct RowExpectation {
        ModelIdentity model;
        WgrInstance row{}; // producer model; actual CPU row is compared after exact translation
    };
    struct Request {
        Binding binding;
        Action action=Action::Takeover;
        RowExpectation originalBefore,originalAfter,pageBefore,pageAfter;
        uint32_t originalRendererHandle=0,pageRendererHandle=0;
        uint64_t previousCameraGeneration=0;
    };
    // Literal provider facts from the actual consumer tables and CPU fact getter.
    // Unmapped is an affirmative table lookup, not a default Absent sentinel.
    // For a fresh Add before-state only, cpu is all zero and getterSucceeded=false:
    // there is no historical renderer handle to query. For a historical removal
    // cpu must be a successful exact-handle ABSENT getter with its actual epoch.
    struct InstanceEvidence {
        ProducerState producerState=ProducerState::Invalid;
        uint32_t producerInstance=UINT32_MAX,producerModel=UINT32_MAX;
        uint64_t consumerInstanceBirth=0,consumerModelBirth=0;
        uint32_t mappedRendererHandle=0,mappedRendererModel=UINT32_MAX;
        bool getterSucceeded=false;
        WgrInstanceCpuFact cpu{};
    };
    struct BeforeEvidence {
        Binding binding;
        uint64_t frameAttempt=0,instanceEpoch=0;
        InstanceEvidence original,page;
    };
    struct DrainEvidence {
        Binding binding;
        uint64_t frameAttempt=0,instanceEpoch=0;
        BeforeEvidence before;
        InstanceEvidence original,page;
    };
    struct ReturnedEvidence {
        Binding binding;
        uint64_t frameAttempt=0,instanceEpoch=0;
        int32_t renderReturnStatus=-1;
        InstanceEvidence original,page;
        bool cameraGetterSucceeded=false;
        uint64_t cameraGenerationBeforeRender=0; // actual completed tuple sampled immediately before this call
        WgrMainCameraTuple camera{};
        uint32_t packetCameraIndex=UINT32_MAX;
        WgrCamera packetCamera{}; // literal camera from this exact submitted WgrFrame
    };
    struct Snapshot {
        State state=State::Requested;
        Refusal refusal=Refusal::None;
        Binding binding;
        Action action=Action::Takeover;
        bool drained=false;
        uint64_t frameAttempt=0,beforeInstanceEpoch=0,instanceEpoch=0;
        uint32_t originalRendererHandle=0,pageRendererHandle=0;
        int32_t renderReturnStatus=-1;
        WgrMainCameraTuple camera{};
    };

    explicit RetailWorldInstanceTransaction(const Request& request) noexcept :_request(request)
    {
        _snapshot.binding=request.binding;_snapshot.action=request.action;
        _valid=ValidRequest(request);
        if(!_valid)Reject(Refusal::InvalidRequest);
    }
    bool Valid() const noexcept {return _valid;}
    const Request& GetRequest() const noexcept {return _request;}
    // Pure preflight, intended before issuing either FFI mutation. Calling this
    // does not publish or manufacture an authoritative "beforeVerified" token.
    Refusal CheckBefore(const BeforeEvidence& e) const noexcept
    {
        if(!_valid)return Refusal::InvalidRequest;
        if(e.binding!=_request.binding)return Refusal::Binding;
        if(!ValidSerial(e.frameAttempt)||!ValidSerial(e.instanceEpoch))return Refusal::Frame;
        if(!Present(e.original,_request.originalBefore,_request.binding.originalProducerInstance,
            _request.binding.originalInstanceBirth,_request.originalRendererHandle,e.instanceEpoch))
            return Refusal::Before;
        if(_request.action==Action::Takeover)
            return FreshUnmapped(e.page,_request.binding.pageProducerInstance)?Refusal::None:Refusal::Before;
        return Present(e.page,_request.pageBefore,_request.binding.pageProducerInstance,
            _request.binding.pageInstanceBirth,_request.pageRendererHandle,e.instanceEpoch)?
            Refusal::None:Refusal::Before;
    }
    bool Consume(const DrainEvidence& e) noexcept
    {
        std::lock_guard lock(_mutex);
        if(_state.load(std::memory_order_relaxed)!=State::Requested||_snapshot.drained)return false;
        if(e.binding!=_request.binding){Reject(Refusal::Binding);return false;}
        const auto before=CheckBefore(e.before);
        if(before!=Refusal::None){Reject(before);return false;}
        if(!ValidSerial(e.frameAttempt)||e.frameAttempt!=e.before.frameAttempt||
            !ValidSerial(e.instanceEpoch)||e.instanceEpoch<=e.before.instanceEpoch)
        {Reject(Refusal::Frame);return false;}
        const uint32_t pageHandle=_request.action==Action::Takeover?
            e.page.mappedRendererHandle:_request.pageRendererHandle;
        if(!ValidHandle(pageHandle)||pageHandle==_request.originalRendererHandle||
            !After(e.original,e.page,pageHandle,e.instanceEpoch))
        {Reject(Refusal::Instance);return false;}
        _snapshot.drained=true;_snapshot.frameAttempt=e.frameAttempt;
        _snapshot.beforeInstanceEpoch=e.before.instanceEpoch;_snapshot.instanceEpoch=e.instanceEpoch;
        _snapshot.originalRendererHandle=_request.originalRendererHandle;_snapshot.pageRendererHandle=pageHandle;
        return true;
    }
    bool PublishReturned(const ReturnedEvidence& e) noexcept
    {
        std::lock_guard lock(_mutex);
        if(_state.load(std::memory_order_relaxed)!=State::Requested)return false;
        if(e.binding!=_request.binding){Reject(Refusal::Binding);return false;}
        _snapshot.renderReturnStatus=e.renderReturnStatus;
        if(!_snapshot.drained||e.frameAttempt!=_snapshot.frameAttempt||e.renderReturnStatus!=0)
        {Reject(Refusal::Frame);return false;}
        if(e.instanceEpoch!=_snapshot.instanceEpoch||
            !After(e.original,e.page,_snapshot.pageRendererHandle,e.instanceEpoch))
        {Reject(Refusal::Instance);return false;}
        if(!ValidCamera(e)){Reject(Refusal::Camera);return false;}
        _snapshot.camera=e.camera;_snapshot.state=State::Consumed;
        _state.store(State::Consumed,std::memory_order_release);return true;
    }
    void ConsumeRejected(Refusal reason=Refusal::Instance) noexcept
    {
        std::lock_guard lock(_mutex);
        if(_state.load(std::memory_order_relaxed)==State::Requested)
            Reject(reason==Refusal::None?Refusal::Instance:reason);
    }
    Snapshot Observe() const noexcept
    {
        (void)_state.load(std::memory_order_acquire);
        std::lock_guard lock(_mutex);return _snapshot;
    }
private:
    static bool ValidSerial(uint64_t v) noexcept {return v&&v!=UINT64_MAX;}
    static bool ValidHandle(uint32_t v) noexcept {return (v&0x00ffffffu)!=0;}
    static bool ZeroRow(const WgrInstance& v) noexcept
    {const WgrInstance zero{};return std::memcmp(&v,&zero,sizeof(v))==0;}
    static bool Empty(const RowExpectation& v) noexcept
    {return v.model==ModelIdentity{}&&ZeroRow(v.row);}
    static bool Rigid(const WgrInstance& v,uint32_t flags) noexcept
    {
        if((v.flags & ~WGR_INSTANCE_SURFACE_RECEIVERS)!=flags||v.cull_radius||v._pad||v.center.w<=0||
            !std::isfinite(v.center.x)||!std::isfinite(v.center.y)||
            !std::isfinite(v.center.z)||!std::isfinite(v.center.w))return false;
        for(float f:v.world.m)if(!std::isfinite(f))return false;
        for(const auto* plane:{&v.conform0,&v.conform1,&v.conform2})
            if(plane->x!=0||plane->y!=0||plane->z!=0||plane->w!=0)return false;
        return true;
    }
    static bool Row(const RowExpectation& v,uint32_t flags) noexcept
    {
        return v.model.producerModel!=UINT32_MAX&&v.model.rendererModel!=UINT32_MAX&&
            ValidSerial(v.model.modelBirth)&&v.row.model==v.model.producerModel&&Rigid(v.row,flags);
    }
    static bool SamePlacement(WgrInstance a,WgrInstance b) noexcept
    {
        if ((a.flags & WGR_INSTANCE_SURFACE_RECEIVERS) != (b.flags & WGR_INSTANCE_SURFACE_RECEIVERS)) return false;
        a.model=b.model=0;a.flags=b.flags=0;
        return std::memcmp(&a,&b,sizeof(a))==0;
    }
    static bool ValidRequest(const Request& r) noexcept
    {
        const auto& b=r.binding;
        if(!ValidSerial(b.ownerEpoch)||!ValidSerial(b.worldObjectBirth)||!ValidSerial(b.sourceAdmissionEpoch)||
            !ValidSerial(b.shapeBirth)||!ValidSerial(b.shapeQueryRevision)||!ValidSerial(b.pageEpoch)||
            !ValidSerial(b.requestId)||!ValidSerial(b.originalInstanceBirth)||!ValidSerial(b.pageInstanceBirth)||
            b.originalProducerInstance==UINT32_MAX||b.pageProducerInstance==UINT32_MAX||
            b.originalProducerInstance==b.pageProducerInstance||
            !ValidHandle(r.originalRendererHandle)||r.previousCameraGeneration==UINT64_MAX||
            r.originalBefore.model!=r.originalAfter.model||
            !SamePlacement(r.originalBefore.row,r.originalAfter.row))return false;
        const auto main=WGR_INSTANCE_MAIN_CAMERA_ONLY,other=WGR_INSTANCE_OTHER_VIEWS_ONLY;
        if(r.action==Action::Takeover)
            return Row(r.originalBefore,0)&&Row(r.originalAfter,other)&&Empty(r.pageBefore)&&
                Row(r.pageAfter,main)&&!r.pageRendererHandle&&
                SamePlacement(r.originalAfter.row,r.pageAfter.row);
        if(!ValidHandle(r.pageRendererHandle)||r.pageRendererHandle==r.originalRendererHandle||
            !Row(r.originalBefore,other)||!Row(r.pageBefore,main)||
            !SamePlacement(r.originalBefore.row,r.pageBefore.row))return false;
        if(r.action==Action::Restore)return Row(r.originalAfter,0)&&Empty(r.pageAfter);
        return r.action==Action::UpdatePage&&Row(r.originalAfter,other)&&Row(r.pageAfter,main)&&
            SamePlacement(r.originalAfter.row,r.pageAfter.row)&&
            r.pageBefore.model.producerModel!=r.pageAfter.model.producerModel&&
            r.pageBefore.model.rendererModel!=r.pageAfter.model.rendererModel;
    }
    static bool Cpu(const InstanceEvidence& e,uint32_t handle,uint64_t epoch,uint32_t status) noexcept
    {
        const auto& c=e.cpu;
        return e.getterSucceeded&&ValidHandle(handle)&&c.version==1&&c.bytes==sizeof(c)&&
            c.status==status&&c.queried_handle==handle&&c.instance_epoch==epoch&&ValidSerial(epoch)&&!c.reserved&&
            (status==WGR_INSTANCE_CPU_FACT_PRESENT?
                c.resolved_slot==((handle&0x00ffffffu)-1):c.resolved_slot==UINT32_MAX&&ZeroRow(c.row));
    }
    static bool Present(const InstanceEvidence& e,const RowExpectation& r,uint32_t producer,
        uint64_t birth,uint32_t handle,uint64_t epoch) noexcept
    {
        if(e.producerState!=ProducerState::Present||e.producerInstance!=producer||
            e.producerModel!=r.model.producerModel||e.consumerInstanceBirth!=birth||
            e.consumerModelBirth!=r.model.modelBirth||e.mappedRendererHandle!=handle||
            e.mappedRendererModel!=r.model.rendererModel||!Cpu(e,handle,epoch,WGR_INSTANCE_CPU_FACT_PRESENT))return false;
        auto expected=r.row;expected.model=r.model.rendererModel;
        return std::memcmp(&expected,&e.cpu.row,sizeof(expected))==0;
    }
    static bool Unmapped(const InstanceEvidence& e,uint32_t producer) noexcept
    {
        return e.producerState==ProducerState::Unmapped&&e.producerInstance==producer&&
            e.producerModel==UINT32_MAX&&!e.consumerInstanceBirth&&!e.consumerModelBirth&&
            !e.mappedRendererHandle&&e.mappedRendererModel==UINT32_MAX;
    }
    static bool FreshUnmapped(const InstanceEvidence& e,uint32_t producer) noexcept
    {
        const WgrInstanceCpuFact zero{};
        return Unmapped(e,producer)&&!e.getterSucceeded&&std::memcmp(&e.cpu,&zero,sizeof(zero))==0;
    }
    bool After(const InstanceEvidence& original,const InstanceEvidence& page,uint32_t handle,uint64_t epoch) const noexcept
    {
        const auto& b=_request.binding;
        if(!Present(original,_request.originalAfter,b.originalProducerInstance,b.originalInstanceBirth,
            _request.originalRendererHandle,epoch))return false;
        if(_request.action==Action::Restore)
            return Unmapped(page,b.pageProducerInstance)&&Cpu(page,handle,epoch,WGR_INSTANCE_CPU_FACT_ABSENT);
        return Present(page,_request.pageAfter,b.pageProducerInstance,b.pageInstanceBirth,handle,epoch);
    }
    bool ValidCamera(const ReturnedEvidence& e) const noexcept
    {
        const auto& c=e.camera;
        if(!e.cameraGetterSucceeded||c.version!=1||c.struct_bytes!=sizeof(c)||c.status!=1||c.reserved||
            !ValidSerial(c.generation)||c.generation<=_request.previousCameraGeneration||
            e.cameraGenerationBeforeRender==UINT64_MAX||
            e.cameraGenerationBeforeRender<_request.previousCameraGeneration||
            c.generation<=e.cameraGenerationBeforeRender||
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
static_assert(sizeof(RetailWorldInstanceTransaction)<=2048);
} // namespace Poseidon::render
