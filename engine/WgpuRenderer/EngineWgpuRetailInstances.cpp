#include "EngineWgpu.hpp"
#include <cstring>

namespace Poseidon
{
render::RetailPageInstanceObservation::InstanceEvidence EngineWgpu::GetRetailInstanceEvidence(
    const render::RetailPageInstanceObservation& receipt) const
{
    using Receipt = render::RetailPageInstanceObservation;
    Receipt::InstanceEvidence result;
    const auto& request = receipt.GetRequest();
    const auto producer = request.binding.producerInstance;
    result.mappingExists = producer < _instSlotOf.size() && _instSlotOf[producer] != UINT32_MAX;
    result.rendererHandle = result.mappingExists ? _instSlotOf[producer] : request.previousRenderer;
    WgrInstanceCpuFact fact{};
    const auto status = wgr_instance_cpu_fact(_renderer,result.rendererHandle,&fact,sizeof(fact),1);
    result.getterSucceeded = (status == 1 || status == 2) && fact.status == status &&
        fact.version == 1 && fact.bytes == sizeof(fact) && fact.queried_handle == result.rendererHandle &&
        !fact.reserved;
    if (!result.getterSucceeded) return result;
    result.instanceEpoch = fact.instance_epoch;
    result.state = status == 1 ? Receipt::FactState::Present : Receipt::FactState::Absent;
    result.actual = fact.row;
    if (status == 1)
    {
        const auto model = _modelIdOf.find(request.binding.producerModel);
        if (model == _modelIdOf.end() || model->second == WGR_INVALID_MODEL) { result.getterSucceeded = false; return result; }
        result.rendererModel = model->second;
    }
    return result;
}

void EngineWgpu::ObserveRetailInstanceDrain(const InstanceOp& op)
{
    using Receipt = render::RetailPageInstanceObservation;
    if (!op.retailReceipt) return;
    if (_retailInstanceReturn || _retailInstanceFrameAttempt >= UINT64_MAX-1)
    { op.retailReceipt->ConsumeRejected(Receipt::Refusal::Frame); return; }
    Receipt::DrainEvidence evidence;
    evidence.binding = op.retailReceipt->GetRequest().binding;
    evidence.frameAttempt = ++_retailInstanceFrameAttempt;
    evidence.instance = GetRetailInstanceEvidence(*op.retailReceipt);
    WgrMainCameraTuple before{};
    _retailInstanceCameraBefore = wgr_main_camera_tuple(_renderer,&before,sizeof(before),1) == 1 ? before.generation : 0;
    if (op.retailReceipt->Consume(evidence)) _retailInstanceReturn = op.retailReceipt;
}

void EngineWgpu::ObserveRetailInstanceReturned(const WgrFrame& frame, uint32_t renderStatus)
{
    using Receipt = render::RetailPageInstanceObservation;
    auto receipt = std::move(_retailInstanceReturn);
    if (!receipt) return;
    Receipt::ReturnedEvidence evidence;
    evidence.binding = receipt->GetRequest().binding;
    evidence.frameAttempt = _retailInstanceFrameAttempt;
    evidence.renderReturnStatus = static_cast<int32_t>(renderStatus);
    evidence.instance = GetRetailInstanceEvidence(*receipt);
    evidence.cameraGetterSucceeded = wgr_main_camera_tuple(_renderer,&evidence.camera,sizeof(evidence.camera),1) == 1 &&
        evidence.camera.generation > _retailInstanceCameraBefore;
    if (frame.cameras.data && evidence.camera.camera_index < frame.cameras.length)
    {
        evidence.packetCameraIndex = evidence.camera.camera_index;
        evidence.packetCamera = frame.cameras.data[evidence.packetCameraIndex];
    }
    receipt->PublishReturned(evidence);
    _retailInstanceCameraBefore = 0;
}
} // namespace Poseidon
