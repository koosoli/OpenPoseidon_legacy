#include "EngineWgpu.hpp"
#include <cstring>

namespace Poseidon
{
namespace
{
using WorldPair=render::RetailWorldInstanceTransaction;
constexpr uint32_t MissingInstance=UINT32_MAX;
bool WorldSerial(uint64_t value) {return value&&value!=UINT64_MAX;}
bool ExactWorldRow(const WorldPair::InstanceEvidence& e,const WorldPair::RowExpectation& row,
    uint32_t producer,uint64_t birth,uint32_t handle)
{
    if(e.producerState!=WorldPair::ProducerState::Present||e.producerInstance!=producer||
        e.producerModel!=row.model.producerModel||e.consumerInstanceBirth!=birth||
        e.consumerModelBirth!=row.model.modelBirth||e.mappedRendererHandle!=handle||
        e.mappedRendererModel!=row.model.rendererModel||!e.getterSucceeded||
        e.cpu.status!=WGR_INSTANCE_CPU_FACT_PRESENT||e.cpu.queried_handle!=handle||
        !WorldSerial(e.cpu.instance_epoch)||e.cpu.resolved_slot!=((handle&0x00ffffffu)-1))return false;
    auto expected=row.row;expected.model=row.model.rendererModel;
    return std::memcmp(&expected,&e.cpu.row,sizeof(expected))==0;
}
}

void EngineWgpu::SetRetailWorldConsumerBirth(uint32_t producer,uint64_t birth)
{
    if(producer==UINT32_MAX||birth==UINT64_MAX)return;
    for(auto& slot:_retailWorldConsumerBirths)if(slot.producer==producer)
    {if(birth)slot.birth=birth;else slot={};return;}
    if(!birth)return;
    for(auto& slot:_retailWorldConsumerBirths)if(!slot.birth)
    {slot.producer=producer;slot.birth=birth;return;}
    // No allocation, overwrite or eviction of somebody else's selected birth.
    // The caller preflights capacity and verifies installation before mutation.
}

uint64_t EngineWgpu::GetRetailWorldConsumerBirth(uint32_t producer) const
{
    for(const auto& slot:_retailWorldConsumerBirths)if(slot.producer==producer)return slot.birth;
    return 0;
}

WorldPair::InstanceEvidence EngineWgpu::GetRetailWorldInstanceEvidence(uint32_t producer,
    const WorldPair::ModelIdentity& model,uint32_t historicalHandle) const
{
    WorldPair::InstanceEvidence result;result.producerInstance=producer;
    const bool mapped=producer<_instSlotOf.size()&&_instSlotOf[producer]!=MissingInstance;
    result.producerState=mapped?WorldPair::ProducerState::Present:WorldPair::ProducerState::Unmapped;
    result.consumerInstanceBirth=GetRetailWorldConsumerBirth(producer);
    uint32_t handle=historicalHandle;
    if(mapped)
    {
        handle=result.mappedRendererHandle=_instSlotOf[producer];
        const auto found=_modelIdOf.find(model.producerModel);
        if(found!=_modelIdOf.end()&&found->second!=WGR_INVALID_MODEL)
        {
            result.producerModel=model.producerModel;result.mappedRendererModel=found->second;
            // Producer model handles never recycle in this owner. This is the
            // actual current translation's lifetime discriminator, not a source
            // hash or an opaque material/physical upload claim.
            result.consumerModelBirth=uint64_t(model.producerModel)+1;
        }
    }
    if(!_renderer||!handle)return result; // fresh unmapped reservation has no historical CPU row
    const auto status=wgr_instance_cpu_fact(_renderer,handle,&result.cpu,sizeof(result.cpu),1);
    result.getterSucceeded=(status==WGR_INSTANCE_CPU_FACT_PRESENT||status==WGR_INSTANCE_CPU_FACT_ABSENT)&&
        result.cpu.status==status&&result.cpu.version==1&&result.cpu.bytes==sizeof(result.cpu)&&
        result.cpu.queried_handle==handle&&!result.cpu.reserved;
    return result;
}

WorldPair::BeforeEvidence EngineWgpu::GetRetailWorldBefore(const WorldPair::Request& request,uint64_t attempt) const
{
    WorldPair::BeforeEvidence before;before.binding=request.binding;before.frameAttempt=attempt;
    before.original=GetRetailWorldInstanceEvidence(request.binding.originalProducerInstance,
        request.originalBefore.model,request.originalRendererHandle);
    before.page=GetRetailWorldInstanceEvidence(request.binding.pageProducerInstance,
        request.pageBefore.model,request.pageRendererHandle);
    before.instanceEpoch=before.original.getterSucceeded?before.original.cpu.instance_epoch:0;
    return before;
}

// Failure recovery is intentionally narrower than admission. It uses exact
// historical bindings, never Object*, mount validity or a new source placement.
// A removed/recycled/moved original must not be resurrected by cleanup.
void EngineWgpu::RollbackRetailWorldPair(const WorldPair::Request& request)
{
    if(!_renderer)return;
    const auto retainedActive=_retailWorldActivePair; // keep request storage alive if the cleanup clears its last consumer hold
    const auto& b=request.binding;
    if(!WorldSerial(b.originalInstanceBirth)||!WorldSerial(b.pageInstanceBirth)||
        b.originalProducerInstance==UINT32_MAX||b.pageProducerInstance==UINT32_MAX)return;
    auto source=GetRetailWorldInstanceEvidence(b.originalProducerInstance,request.originalBefore.model,
        request.originalRendererHandle);
    bool originalSafe=false;
    if(source.consumerInstanceBirth==b.originalInstanceBirth&&
        source.mappedRendererHandle==request.originalRendererHandle)
    {
        auto baseline=request.originalBefore;baseline.row.flags &= WGR_INSTANCE_SURFACE_RECEIVERS;
        auto hidden=baseline;hidden.row.flags |= WGR_INSTANCE_OTHER_VIEWS_ONLY;
        if(ExactWorldRow(source,baseline,b.originalProducerInstance,b.originalInstanceBirth,request.originalRendererHandle))
            originalSafe=true;
        else if(ExactWorldRow(source,hidden,b.originalProducerInstance,b.originalInstanceBirth,request.originalRendererHandle))
        {
            auto row=baseline.row;row.model=baseline.model.rendererModel;
            wgr_instance_update(_renderer,request.originalRendererHandle,&row);
            source=GetRetailWorldInstanceEvidence(b.originalProducerInstance,baseline.model,request.originalRendererHandle);
            originalSafe=ExactWorldRow(source,baseline,b.originalProducerInstance,b.originalInstanceBirth,request.originalRendererHandle);
        }
        // A normal move can already have restored its NEW conventional row to
        // flags0. Do not write the old placement over it; only remove our page.
        else if(source.getterSucceeded&&source.cpu.status==WGR_INSTANCE_CPU_FACT_PRESENT&&(source.cpu.row.flags & ~WGR_INSTANCE_SURFACE_RECEIVERS)==0)
            originalSafe=true;
    }
    else if(source.consumerInstanceBirth!=b.originalInstanceBirth)
    {
        // An ordinary lifetime op has cleared/replaced this selected birth. Cleanup may
        // remove ONLY the quarantined page; never touch the replacement source.
        originalSafe=true;
    }
    if(!originalSafe)return; // unknown still-hidden source: retain the page and all resources for owner recovery

    const auto pageProducer=b.pageProducerInstance;
    const auto clearActiveIfUnmapped=[this,&b,pageProducer]()
    {
        if(_retailWorldActivePair&&_retailWorldActivePair->GetRequest().binding==b&&
            !GetRetailWorldConsumerBirth(pageProducer)&&
            (pageProducer>=_instSlotOf.size()||_instSlotOf[pageProducer]==MissingInstance))
            _retailWorldActivePair.reset();
    };
    if(GetRetailWorldConsumerBirth(pageProducer)!=b.pageInstanceBirth||pageProducer>=_instSlotOf.size()||
        _instSlotOf[pageProducer]==MissingInstance)
    {clearActiveIfUnmapped();return;}
    const uint32_t handle=_instSlotOf[pageProducer];
    if(request.pageRendererHandle&&handle!=request.pageRendererHandle)return;
    bool exact=false;
    for(const auto* expectation:{&request.pageAfter,&request.pageBefore})
    {
        if(expectation->model.producerModel==UINT32_MAX)continue;
        const auto page=GetRetailWorldInstanceEvidence(pageProducer,expectation->model,handle);
        if(ExactWorldRow(page,*expectation,pageProducer,b.pageInstanceBirth,handle))
        {exact=true;break;}
        // A previous removal can have succeeded before its getter failed. The
        // still-quarantined birth+literal handle permit an absence reobservation.
        if(page.getterSucceeded&&page.cpu.status==WGR_INSTANCE_CPU_FACT_ABSENT&&page.cpu.resolved_slot==UINT32_MAX)
        {
            const WgrInstance zero{};
            if(std::memcmp(&page.cpu.row,&zero,sizeof(zero))==0)
            {_instSlotOf[pageProducer]=MissingInstance;SetRetailWorldConsumerBirth(pageProducer,0);
                clearActiveIfUnmapped();return;}
        }
    }
    if(!exact)return;
    wgr_instance_remove(_renderer,handle);
    const auto model=request.pageAfter.model.producerModel!=UINT32_MAX?request.pageAfter.model:request.pageBefore.model;
    const auto removed=GetRetailWorldInstanceEvidence(pageProducer,model,handle);
    const WgrInstance zero{};
    if(removed.getterSucceeded&&removed.cpu.status==WGR_INSTANCE_CPU_FACT_ABSENT&&
        removed.cpu.resolved_slot==UINT32_MAX&&std::memcmp(&removed.cpu.row,&zero,sizeof(zero))==0)
    {_instSlotOf[pageProducer]=MissingInstance;SetRetailWorldConsumerBirth(pageProducer,0);clearActiveIfUnmapped();}
}

// Called BEFORE any ordinary mutation of either bound producer, including a
// move in the SAME queue as Takeover. Restore/remove our exact old rows first;
// the ordinary op can then publish its new conventional placement/lifetime.
void EngineWgpu::InvalidateRetailWorldConsumer(uint32_t producer)
{
    if(const auto stable=_retailWorldStableFrame)
    {
        const auto& b=stable->GetRequest().binding;
        if(producer==b.originalProducerInstance||producer==b.pageProducerInstance)
        {
            stable->Invalidate(render::RetailWorldStableFrameObservation::Refusal::Instance);
            if(_retailWorldStableArmed==stable)_retailWorldStableArmed.reset();
        }
    }
    const auto active=_retailWorldActivePair;if(!active)return;
    const auto& r=active->GetRequest();
    if(producer!=r.binding.originalProducerInstance&&producer!=r.binding.pageProducerInstance)return;
    RollbackRetailWorldPair(r);
    if(_retailWorldPairReturn)
    {
        _retailWorldPairReturn->ConsumeRejected(WorldPair::Refusal::Before);
        _retailWorldPairReturn.reset();
    }
    active->ConsumeRejected(WorldPair::Refusal::Before);
    _retailWorldCameraBefore=0;
    // If a getter/row is Unknown, the active request deliberately survives and
    // resources stay held for recovery. Rejection alone is never absence ACK.
}

void EngineWgpu::ExecuteRetailWorldPair(const std::shared_ptr<WorldPair>& receipt)
{
    if(!receipt)return;
    const auto& r=receipt->GetRequest();const auto& b=r.binding;
    const auto prior=receipt->Observe();
    if(prior.state!=WorldPair::State::Requested||prior.drained)return;
    if(!_renderer||!receipt->Valid()||b.ownerEpoch!=_geometryOwnerEpoch||_retailWorldPairReturn||
        _retailInstanceReturn||_retailWorldFrameAttempt>=UINT64_MAX-1)
    {receipt->ConsumeRejected(WorldPair::Refusal::Frame);return;}
    const auto modelCurrent=[this](const WorldPair::RowExpectation& row)
    {
        if(row.model.producerModel==UINT32_MAX)return true; // absent side has no model mapping
        const auto found=_modelIdOf.find(row.model.producerModel);
        return found!=_modelIdOf.end()&&found->second!=WGR_INVALID_MODEL&&found->second==row.model.rendererModel&&
            row.model.modelBirth==uint64_t(row.model.producerModel)+1;
    };
    if(!modelCurrent(r.originalBefore)||!modelCurrent(r.originalAfter)||
        !modelCurrent(r.pageBefore)||!modelCurrent(r.pageAfter))
    {receipt->ConsumeRejected(WorldPair::Refusal::Instance);return;}
    _retailWorldPairBefore=GetRetailWorldBefore(r,++_retailWorldFrameAttempt);
    const auto preflight=receipt->CheckBefore(_retailWorldPairBefore);
    if(preflight!=WorldPair::Refusal::None){receipt->ConsumeRejected(preflight);return;}
    // A stable tracker never crosses a changing cut or Restore transaction.
    // This is revocation only; pair preflight/mutation still uses its own proof.
    if(const auto stable=_retailWorldStableFrame)
    {
        const auto& sb=stable->GetRequest().binding;
        if(sb.originalProducerInstance==b.originalProducerInstance||sb.pageProducerInstance==b.pageProducerInstance)
        {
            stable->Invalidate(render::RetailWorldStableFrameObservation::Refusal::Instance);
            if(_retailWorldStableArmed==stable)_retailWorldStableArmed.reset();
        }
    }
    if(r.action==WorldPair::Action::Takeover)
    {
        for(const auto& slot:_retailWorldConsumerBirths)
            if(slot.birth&&slot.producer!=b.originalProducerInstance&&slot.producer!=b.pageProducerInstance)
            {receipt->ConsumeRejected(WorldPair::Refusal::Instance);return;}
        // A reservation can reuse an existing cell or append ONE cell. It cannot
        // turn an untrusted sparse producer number into a huge vector allocation.
        if(b.pageProducerInstance>_instSlotOf.size())
        {receipt->ConsumeRejected(WorldPair::Refusal::Instance);return;}
        if(b.pageProducerInstance==_instSlotOf.size())
        {
            try {_instSlotOf.resize(_instSlotOf.size()+1,MissingInstance);}
            catch(...) {receipt->ConsumeRejected(WorldPair::Refusal::Instance);return;}
        }
        auto page=r.pageAfter.row;page.model=r.pageAfter.model.rendererModel;
        _retailWorldActivePair=receipt;
        const auto handle=wgr_instance_add(_renderer,&page);
        if(!handle){RollbackRetailWorldPair(r);receipt->ConsumeRejected(WorldPair::Refusal::Instance);return;}
        _instSlotOf[b.pageProducerInstance]=handle;
        SetRetailWorldConsumerBirth(b.pageProducerInstance,b.pageInstanceBirth);
        const auto added=GetRetailWorldInstanceEvidence(b.pageProducerInstance,r.pageAfter.model,handle);
        if(!ExactWorldRow(added,r.pageAfter,b.pageProducerInstance,b.pageInstanceBirth,handle))
        {RollbackRetailWorldPair(r);receipt->ConsumeRejected(WorldPair::Refusal::Instance);return;}
        // The original stays fully visible until actual page Add+row succeeds.
        auto original=r.originalAfter.row;original.model=r.originalAfter.model.rendererModel;
        wgr_instance_update(_renderer,r.originalRendererHandle,&original);
    }
    else if(r.action==WorldPair::Action::Restore)
    {
        _retailWorldActivePair=receipt;
        auto original=r.originalAfter.row;original.model=r.originalAfter.model.rendererModel;
        wgr_instance_update(_renderer,r.originalRendererHandle,&original);
        const auto restored=GetRetailWorldInstanceEvidence(b.originalProducerInstance,r.originalAfter.model,r.originalRendererHandle);
        if(!ExactWorldRow(restored,r.originalAfter,b.originalProducerInstance,b.originalInstanceBirth,r.originalRendererHandle))
        {RollbackRetailWorldPair(r);receipt->ConsumeRejected(WorldPair::Refusal::Instance);return;}
        wgr_instance_remove(_renderer,r.pageRendererHandle);
        const auto removed=GetRetailWorldInstanceEvidence(b.pageProducerInstance,r.pageBefore.model,r.pageRendererHandle);
        const WgrInstance zero{};
        if(!removed.getterSucceeded||removed.cpu.status!=WGR_INSTANCE_CPU_FACT_ABSENT||
            removed.cpu.resolved_slot!=UINT32_MAX||std::memcmp(&removed.cpu.row,&zero,sizeof(zero))!=0)
        {RollbackRetailWorldPair(r);receipt->ConsumeRejected(WorldPair::Refusal::Instance);return;}
        _instSlotOf[b.pageProducerInstance]=MissingInstance;
        SetRetailWorldConsumerBirth(b.pageProducerInstance,0); // immutable Request retains historical birth until owner ACK
    }
    else
    {
        _retailWorldActivePair=receipt;
        auto page=r.pageAfter.row;page.model=r.pageAfter.model.rendererModel;
        wgr_instance_update(_renderer,r.pageRendererHandle,&page);
    }
    _retailWorldPairReturn=receipt;
}

void EngineWgpu::ObserveRetailWorldPairDrain()
{
    const auto receipt=_retailWorldPairReturn;if(!receipt)return;
    const auto& r=receipt->GetRequest();WorldPair::DrainEvidence d;
    d.binding=r.binding;d.frameAttempt=_retailWorldFrameAttempt;d.before=_retailWorldPairBefore;
    d.original=GetRetailWorldInstanceEvidence(r.binding.originalProducerInstance,r.originalAfter.model,r.originalRendererHandle);
    const auto model=r.action==WorldPair::Action::Restore?r.pageBefore.model:r.pageAfter.model;
    d.page=GetRetailWorldInstanceEvidence(r.binding.pageProducerInstance,model,r.pageRendererHandle);
    d.instanceEpoch=d.original.getterSucceeded?d.original.cpu.instance_epoch:0;
    if(r.binding.ownerEpoch!=_geometryOwnerEpoch||!receipt->Consume(d))
    {
        RollbackRetailWorldPair(r);receipt->ConsumeRejected(WorldPair::Refusal::Binding);
        _retailWorldPairReturn.reset();return;
    }
    WgrMainCameraTuple before{};
    _retailWorldCameraBefore=wgr_main_camera_tuple(_renderer,&before,sizeof(before),1)==1?before.generation:0;
}

void EngineWgpu::ObserveRetailWorldPairReturned(const WgrFrame& frame,uint32_t renderStatus)
{
    auto receipt=std::move(_retailWorldPairReturn);if(!receipt)return;
    const auto& r=receipt->GetRequest();const auto snapshot=receipt->Observe();WorldPair::ReturnedEvidence e;
    e.binding=r.binding;e.frameAttempt=_retailWorldFrameAttempt;e.renderReturnStatus=static_cast<int32_t>(renderStatus);
    e.original=GetRetailWorldInstanceEvidence(r.binding.originalProducerInstance,r.originalAfter.model,r.originalRendererHandle);
    const auto model=r.action==WorldPair::Action::Restore?r.pageBefore.model:r.pageAfter.model;
    e.page=GetRetailWorldInstanceEvidence(r.binding.pageProducerInstance,model,snapshot.pageRendererHandle);
    e.instanceEpoch=e.original.getterSucceeded?e.original.cpu.instance_epoch:0;
    e.cameraGenerationBeforeRender=_retailWorldCameraBefore;
    e.cameraGetterSucceeded=wgr_main_camera_tuple(_renderer,&e.camera,sizeof(e.camera),1)==1;
    if(frame.cameras.data&&e.camera.camera_index<frame.cameras.length)
    {e.packetCameraIndex=e.camera.camera_index;e.packetCamera=frame.cameras.data[e.packetCameraIndex];}
    if(r.binding.ownerEpoch!=_geometryOwnerEpoch)
    {receipt->ConsumeRejected(WorldPair::Refusal::Binding);RollbackRetailWorldPair(r);}
    else if(!receipt->PublishReturned(e))RollbackRetailWorldPair(r);
    else if(r.action==WorldPair::Action::Restore&&_retailWorldActivePair==receipt&&
        !GetRetailWorldConsumerBirth(r.binding.pageProducerInstance)&&
        (r.binding.pageProducerInstance>=_instSlotOf.size()||_instSlotOf[r.binding.pageProducerInstance]==MissingInstance))
        _retailWorldActivePair.reset();
    _retailWorldCameraBefore=0;
}

void EngineWgpu::ArmRetailWorldStableFrame()
{
    using Stable=render::RetailWorldStableFrameObservation;
    if(_retailWorldStableArmed)
    {
        // A previous call without its matching return cannot leave ready data.
        _retailWorldStableArmed->Invalidate(Stable::Refusal::Frame);
        _retailWorldStableArmed.reset();
    }
    const auto tracker=_retailWorldStableFrame;if(!tracker)return;
    const auto& r=tracker->GetRequest();
    if(!_renderer||!tracker->Valid()||r.binding.ownerEpoch!=_geometryOwnerEpoch||
        _retailWorldPairReturn||_retailInstanceReturn||_retailWorldStableFrameAttempt>=UINT64_MAX-1)
    {tracker->Invalidate(Stable::Refusal::Binding);return;}
    Stable::DrainEvidence e;e.binding=r.binding;e.frameAttempt=++_retailWorldStableFrameAttempt;
    e.original=GetRetailWorldInstanceEvidence(r.binding.originalProducerInstance,r.original.model,
        r.originalRendererHandle);
    e.page=GetRetailWorldInstanceEvidence(r.binding.pageProducerInstance,r.page.model,r.pageRendererHandle);
    e.instanceEpoch=e.original.getterSucceeded?e.original.cpu.instance_epoch:0;
    WgrMainCameraTuple before{};
    e.cameraGetterSucceeded=wgr_main_camera_tuple(_renderer,&before,sizeof(before),1)==1&&
        before.version==1&&before.struct_bytes==sizeof(before)&&before.status==1&&!before.reserved;
    e.cameraGenerationBeforeRender=e.cameraGetterSucceeded?before.generation:0;
    if(tracker->Arm(e))_retailWorldStableArmed=tracker;
}

void EngineWgpu::ObserveRetailWorldStableReturned(const WgrFrame& frame,uint32_t renderStatus)
{
    using Stable=render::RetailWorldStableFrameObservation;
    auto tracker=std::move(_retailWorldStableArmed);if(!tracker)return;
    const auto& r=tracker->GetRequest();
    if(tracker!=_retailWorldStableFrame||r.binding.ownerEpoch!=_geometryOwnerEpoch)
    {tracker->Invalidate(Stable::Refusal::Binding);return;}
    const auto armed=tracker->Observe();
    Stable::ReturnedEvidence e;e.binding=r.binding;e.frameAttempt=armed.frameAttempt;
    e.original=GetRetailWorldInstanceEvidence(r.binding.originalProducerInstance,r.original.model,
        r.originalRendererHandle);
    e.page=GetRetailWorldInstanceEvidence(r.binding.pageProducerInstance,r.page.model,r.pageRendererHandle);
    e.instanceEpoch=e.original.getterSucceeded?e.original.cpu.instance_epoch:0;
    e.renderReturnStatus=static_cast<int32_t>(renderStatus);
    // Arm retained the actual pre-call generation. The tracker compares it to
    // this returned value as part of its immutable attempt, never a later getter.
    e.cameraGenerationBeforeRender=armed.cameraGenerationBeforeRender;
    e.cameraGetterSucceeded=wgr_main_camera_tuple(_renderer,&e.camera,sizeof(e.camera),1)==1;
    if(frame.cameras.data&&e.camera.camera_index<frame.cameras.length)
    {e.packetCameraIndex=e.camera.camera_index;e.packetCamera=frame.cameras.data[e.packetCameraIndex];}
    tracker->PublishReturned(e);
}
} // namespace Poseidon
