#include "EngineWgpu.hpp"
#include "RetailPagePilotAdmission.hpp"
#include <Poseidon/Graphics/Rendering/AuthoredGeometryPageWorker.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalOriginalOutside.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cfloat>
#include <filesystem>
#include <limits>
#include <numeric>
#include <string>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace Poseidon
{
namespace
{
constexpr uint64_t RetailLimit = 1024 * 1024;
using Record = render::RetailPageRecordObservation;
std::string RetailHex(const std::array<uint8_t,32>& hash)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string out(64, '0');
    for (size_t i = 0; i < hash.size(); ++i)
    { out[i * 2] = digits[hash[i] >> 4]; out[i * 2 + 1] = digits[hash[i] & 15]; }
    return out;
}
bool RetailNonzero(const std::array<uint8_t,32>& hash)
{ return std::any_of(hash.begin(), hash.end(), [](uint8_t b) { return b != 0; }); }
bool RetailEnclosed(const SVertex& vertex, float radius)
{
    const double x = vertex.pos.X(), y = vertex.pos.Y(), z = vertex.pos.Z();
    const double r = radius;
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
        std::isfinite(r) && r > 0 && x*x + y*y + z*z <= r*r;
}
#ifdef _WIN32
bool RemoveRetailFile(HANDLE& held, std::filesystem::path& path)
{
    if(held==INVALID_HANDLE_VALUE)return path.empty();
    FILE_DISPOSITION_INFO disposition{TRUE};
    if(!SetFileInformationByHandle(held,FileDispositionInfo,&disposition,sizeof(disposition)))return false;
    CloseHandle(held);held=INVALID_HANDLE_VALUE;path.clear();return true;
}
bool CreateRetailFile(const std::vector<uint8_t>& bytes, std::filesystem::path& path, HANDLE& held)
{
    if (bytes.empty() || bytes.size() > RetailLimit) return false;
    wchar_t temp[MAX_PATH + 1]{};
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    if (!length || length >= MAX_PATH) return false;
    static std::atomic<uint64_t> nonce{1};
    for (unsigned attempt = 0; attempt < 16; ++attempt)
    {
        const auto id = nonce.fetch_add(1, std::memory_order_relaxed);
        if (!id || id == UINT64_MAX) return false;
        auto candidate = std::filesystem::path(temp) /
            (L"wgr-retail-page-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(id) + L".ghp");
        if (candidate.native().size() > 1024) return false;
        HANDLE file = CreateFileW(candidate.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
            FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        { if (GetLastError() == ERROR_FILE_EXISTS) continue; return false; }
        DWORD written = 0;
        const bool okay = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
            written == bytes.size() && FlushFileBuffers(file);
        if (okay) { path = std::move(candidate); held = file; return true; }
        if(!RemoveRetailFile(file,candidate))CloseHandle(file); // never delete by a replaceable path
        return false;
    }
    return false;
}
#endif
}

struct EngineWgpu::RetailPagePilotState
{
    enum class Phase : uint8_t {
        AwaitFallback, Advance, AwaitWorker, StagePage, AwaitMesh, AwaitModel, AwaitReadyHistory,
        RootOnlyReady, FineReady, AwaitRelease, Released, AwaitAbort, Aborted, Failed
    };
    enum class Target : uint8_t { Root, Fine, Refill };
    struct MeshRow {
        uint64_t producer = 0, renderer = 0;
        uint32_t cluster = UINT32_MAX, page = UINT32_MAX;
        bool present = true;
    };
    render::RetailPagePilotAdmission admission;
    GeometryPageFixtureReport report;
    std::unique_ptr<GeometryPages::AuthoredPageWorker> worker;
    std::shared_ptr<const GeometryPages::HierarchicalDiskPageInput> diskBase;
    std::filesystem::path path;
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE;
#endif
    std::array<uint64_t,64> clusterMesh{};
    std::array<uint32_t,64> clusterIndices{};
    std::array<uint8_t,64> resident{};
    std::array<uint32_t,4> models{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    std::array<bool,4> modelReady{};
    std::array<std::shared_ptr<render::ModelAdmissionReceipt>,4> receipts{};
    std::vector<MeshRow> history;
    std::vector<uint32_t> rootClusters, fineClusters;
    std::vector<uint32_t> rootPages, finePages, nonrootPages;
    GeometryPages::HierarchicalPage decodedPage;
    uint64_t decodedPageCharge = 0;
    size_t clusterCursor = 0;
    uint32_t pendingPage = UINT32_MAX, pendingCluster = UINT32_MAX, pendingModelSlot = UINT32_MAX,
        readyModelSlot = UINT32_MAX;
    uint64_t workerRequest = 0, pendingRequest = 0;
    uint32_t lastMeshFrame = UINT32_MAX;
    float rootThreshold = 0;
    bool abortRequested = false, abortRecordSettled = false, refilled = false;
    Phase phase = Phase::AwaitFallback;
    Target target = Target::Root;
    std::shared_ptr<Record> observation;

    void RefreshWorkerReport()
    {
        if (!worker)
        {
            report.pageQueued = report.pageActive = report.pageReady = report.pageLiveJobs = 0;
            report.pageReservedBytes = 0;
            return; // retain the last completed/cancelled counters after the worker joins
        }
        const auto stats = worker->Snapshot();
        report.pageQueued = stats.queued; report.pageActive = stats.active;
        report.pageReady = stats.ready; report.pageLiveJobs = stats.liveJobs;
        report.pageReservedBytes = stats.reservedBytes;
        report.pageCompleted = stats.completed; report.pageCancelled = stats.cancelled;
    }

    ~RetailPagePilotState()
    {
        if (worker) { worker->Cancel(report.pageEpoch); worker.reset(); } // joins before owned-file removal
#ifdef _WIN32
        if (file != INVALID_HANDLE_VALUE && !RemoveRetailFile(file,path))
        { CloseHandle(file); file = INVALID_HANDLE_VALUE; } // a failed cleanup may leak our file; never remove another
#endif
    }
    const GeometryPages::HierarchicalPackage& Package() const { return admission.product->manifest->metadata; }
    bool SourceCurrent() const
    { return admission.sourceCurrent && admission.sourceCurrent(); }
    bool Plan(GeometryPages::HierarchicalCutPlan& out) const
    {
        const auto& p = Package();
        return GeometryPages::PlanHierarchicalCut(p,
            target == Target::Root ? rootThreshold : 0.0f,
            {p.identity, std::span<const uint8_t>(resident.data(), p.pages.size())}, out);
    }
    MeshRow* Find(uint64_t producer)
    {
        for (auto& row : history) if (row.producer == producer) return &row;
        return nullptr;
    }
    uint32_t Slot() const { return target == Target::Root ? 1 : target == Target::Fine ? 2 : 3; }
};

Engine::GeometryPageFixtureReport EngineWgpu::SnapshotRetailPageRecordPilot() const
{
    GeometryPageFixtureReport result;
    if (!Foundation::IsMainThread()) { result.status = GeometryReportStatus::Invalid; return result; }
    if (_retailPagePilot)
    {
        _retailPagePilot->RefreshWorkerReport();
        return _retailPagePilot->report;
    }
    return result;
}

Engine::GeometryPageFixtureReport EngineWgpu::BeginRetailPageRecordPilot(render::RetailPagePilotAdmission&& input)
{
    using namespace GeometryPages;
    constexpr uint64_t abortReserve = (Record::MaxMeshes + Record::MaxModels + 1) *
        sizeof(ResourceOp) + sizeof(Record) + 256;
    static_assert(abortReserve < RetailLimit);
    constexpr uint64_t workingLimit = RetailLimit - abortReserve;
    GeometryPageFixtureReport failure;
    failure.status = GeometryReportStatus::Invalid;
    failure.retailRecordOnly = true;
    failure.pageWorkState = "RetailRecordAdmissionRefused";
    if (!Foundation::IsMainThread() || !_renderer || !_gpuDriven || _retailPagePilot ||
        _geometryPageFixture || !_nextGeometryPageEpoch || _nextGeometryPageEpoch == UINT64_MAX ||
        !_nextGeometryPageRequest || _nextGeometryPageRequest == UINT64_MAX) return failure;
    if (_geometryPageWorker)
    {
        const auto debt = _geometryPageWorker->Snapshot();
        if (debt.liveJobs || debt.ready || debt.reservedBytes || debt.queued || debt.active) return failure;
    }
    try
    {
        const auto actual = input.actual;
        const auto product = input.product;
        if (!actual || !product || !product->image || !product->manifest || !input.modelLease ||
            !input.modelLease->Encoded().HasArchiveIdentity() || !input.texture || !input.textureBirth ||
            !input.pixelSourceVerified || !RetailNonzero(input.pixelSourceSha256) ||
            !input.sourceCurrent || !input.sourceCurrent() || !std::isfinite(input.sphereRadius) ||
            input.sphereRadius <= 0 || !input.textureHandle || !input.textureSlotLease ||
            input.material.texture_id != input.textureHandle || input.material.alpha_ref != 0 ||
            input.section.variant != 0 || input.section.flags != 0 ||
            !input.knownAuthorityBytes || input.knownAuthorityBytes > RetailLimit ||
            !actual->requiresOriginalOtherPasses || !product->requiresOriginalOtherPasses ||
            actual->actualNoShadow != product->actualNoShadow ||
            actual->sourceAdmissionEpoch != product->sourceAdmissionEpoch ||
            actual->ownerEpoch != product->ownerEpoch || actual->ownerEpoch != _geometryOwnerEpoch ||
            actual->modelBirth != product->modelBirth ||
            actual->shapeQueryRevision != product->shapeQueryRevision ||
            !(actual->actual.source == product->expectedSource) ||
            !(product->manifest->metadata.identity.source == product->expectedSource) ||
            !(product->image->identity == product->manifest->metadata.identity) ||
            product->manifest->metadata.identity.packageSha256 != product->image->identity.packageSha256 ||
            product->manifest->metadata.pages.empty() || product->manifest->metadata.pages.size() > 64 ||
            product->manifest->fileBytes != product->image->bytes.size() ||
            product->manifest->metadataBytes > HierarchicalDiskDetail::MaxMetadataBytes ||
            product->manifest->metadataBytes > product->image->bytes.size() ||
            product->manifest->metadataSha256 != product->image->metadataSha256 ||
            HierarchicalDiskDetail::Hash(product->image->bytes) != product->fileSha256 ||
            HierarchicalDiskDetail::Hash(std::span<const uint8_t>(product->image->bytes).first(
                size_t(product->manifest->metadataBytes))) != product->image->metadataSha256 ||
            input.modelLease->DecodedBytes() != 53786 ||
            actual->actual.coarse.vertices.empty() || actual->actual.fine.vertices.empty() ||
            actual->actual.coarse.indices.empty() || actual->actual.fine.indices.empty()) return failure;
        for (const auto* mesh : {&actual->actual.coarse, &actual->actual.fine})
            for (const auto& vertex : mesh->vertices)
                if (!RetailEnclosed(vertex, input.sphereRadius)) return failure;
        const auto& p = product->manifest->metadata;
        std::array<uint8_t,64> allResident{}; allResident.fill(1);
        HierarchicalCutPlan root, fine;
        float threshold = 0;
        for (const auto& group : p.groups)
            if (group.simplified.error != FLT_MAX)
                threshold = std::max(threshold, group.simplified.error);
        threshold = std::nextafter(threshold, std::numeric_limits<float>::infinity());
        if (!std::isfinite(threshold) || threshold == FLT_MAX ||
            !PlanHierarchicalCut(p, threshold,
                {p.identity, std::span<const uint8_t>(allResident.data(), p.pages.size())}, root) ||
            !PlanHierarchicalCut(p, 0,
                {p.identity, std::span<const uint8_t>(allResident.data(), p.pages.size())}, fine) ||
            root.state != HierarchicalCutState::RequestedCut || fine.state != HierarchicalCutState::RequestedCut ||
            root.selectedClusters.empty() || fine.selectedClusters.empty() ||
            root.selectedClusters.size() > Record::MaxMeshes || fine.selectedClusters.size() > Record::MaxMeshes)
            return failure;
        const bool distinct = root.selectedClusters != fine.selectedClusters;
        uint32_t initialMeshes = 1, refillMeshes = 0;
        for(auto page:root.requiredPages) {
            if(page>=p.pages.size())return failure;
            initialMeshes+=uint32_t(p.pages[page].clusters.size());
        }
        for (auto page : fine.requiredPages)
        {
            if (page >= p.pages.size()) return failure;
            if (std::find(root.requiredPages.begin(), root.requiredPages.end(), page) == root.requiredPages.end())
            { initialMeshes += uint32_t(p.pages[page].clusters.size());
              refillMeshes += uint32_t(p.pages[page].clusters.size()); }
        }
        if (initialMeshes + (distinct ? refillMeshes : 0) > Record::MaxMeshes ||
            product->knownSourceCapacityBytes > RetailLimit ||
            product->knownRetainedCapacityBytes > RetailLimit - product->knownSourceCapacityBytes ||
            input.knownAuthorityBytes > RetailLimit - product->knownSourceCapacityBytes -
                product->knownRetainedCapacityBytes) return failure;
        auto state = std::make_shared<RetailPagePilotState>();
        state->admission = std::move(input);
        state->rootClusters = root.selectedClusters; state->fineClusters = fine.selectedClusters;
        state->rootPages = root.requiredPages; state->finePages = fine.requiredPages;
        for (auto page : state->finePages)
            if (std::find(state->rootPages.begin(), state->rootPages.end(), page) == state->rootPages.end())
                state->nonrootPages.push_back(page);
        state->rootThreshold = threshold;
        state->history.reserve(Record::MaxMeshes);
        state->worker = std::make_unique<AuthoredPageWorker>();
#ifdef _WIN32
        if (!CreateRetailFile(product->image->bytes, state->path, state->file)) return failure;
#else
        return failure;
#endif
        auto disk = std::make_shared<HierarchicalDiskPageInput>();
        disk->path = state->path;
        disk->metadataBytes.assign(product->image->bytes.begin(),
            product->image->bytes.begin() + size_t(product->manifest->metadataBytes));
        disk->expectedIdentity = p.identity;
        disk->expectedMetadataSha256 = product->image->metadataSha256;
        disk->fileBytes = product->image->bytes.size(); disk->pageId = 0;
        if (!ValidHierarchicalDiskPageInput(*disk)) return failure;
        state->diskBase = std::move(disk);
        uint64_t known = product->knownSourceCapacityBytes + product->knownRetainedCapacityBytes +
            state->admission.knownAuthorityBytes + sizeof(RetailPagePilotState) +
            HierarchicalDiskPageInputKnownBytes(*state->diskBase) +
            state->history.capacity() * sizeof(RetailPagePilotState::MeshRow) + 4096;
        if (known >= workingLimit) return failure;
        state->report.status = GeometryReportStatus::Pending;
        state->report.retailRecordOnly = true;
        state->report.retailPixelSourceVerified = true;
        state->report.retailPixelSourceSha256 = RetailHex(state->admission.pixelSourceSha256);
        state->report.retailSourceProbe = true;
        state->report.retailSourceExported = true;
        state->report.retailSourceOtherPassesRequired = true;
        state->report.retailHierarchyProductStatus = uint32_t(RigidFinalHierarchyStatus::Produced);
        state->report.retailHierarchyPages = uint32_t(p.pages.size());
        state->report.retailRecordDistinctCuts = distinct;
        state->report.hierarchicalPilot = true;
        state->report.hierarchicalDiskPilot = true;
        state->report.hierarchyPages = uint32_t(p.pages.size());
        state->report.hierarchyRootPages = uint32_t(p.rootPages.size());
        state->report.sourceAdmissionEpoch = actual->sourceAdmissionEpoch;
        state->report.originalSourceBytes = state->admission.modelLease->DecodedBytes();
        state->report.sourceSha256 = RetailHex(actual->actual.source.sourceSha256);
        state->report.hierarchyPackageSha256 = RetailHex(p.identity.packageSha256);
        state->report.coarseTriangles = uint32_t(actual->actual.coarse.indices.size() / 3);
        state->report.fineTriangles = uint32_t(actual->actual.fine.indices.size() / 3);
        state->report.knownPayloadBytes = known;
        state->report.active = true;
        state->report.pageEpoch = _nextGeometryPageEpoch++;
        state->report.pageWorkState = "RetailFallbackQueued";
        if (_frameCounter == UINT32_MAX) return failure;
        ResourceOp mesh; mesh.kind = ResourceOp::MeshCreate;
        mesh.verts = actual->actual.coarse.vertices;
        mesh.indices.reserve(actual->actual.coarse.indices.size());
        for (auto index : actual->actual.coarse.indices)
        { if (index > INT32_MAX) return failure; mesh.indices.push_back(VertexIndex(index)); }
        const uint64_t meshCharge = mesh.verts.capacity() * sizeof(SVertex) +
            mesh.indices.capacity() * sizeof(VertexIndex);
        const uint64_t opCharge = 3 * sizeof(ResourceOp) + sizeof(Record) +
            sizeof(render::ModelAdmissionReceipt) + 256;
        if (meshCharge + opCharge > workingLimit - known) return failure;
        const uint64_t initialTicket = _nextGeometryPageRequest++;
        mesh.mesh = AllocateProducerMesh();
        const uint32_t modelId = AllocateProducerModel();
        if (!mesh.mesh || modelId == WGR_INVALID_MODEL) return failure;
        const uint64_t producerMesh = mesh.mesh;
        ResourceOp model; model.kind = ResourceOp::ModelRegister; model.model = modelId;
        model.sphereRadius = state->admission.sphereRadius;
        model.name = "private-retail-fallback-record";
        auto section = state->admission.section;
        section.mesh = producerMesh; section.index_begin = 0;
        section.index_count = uint32_t(mesh.indices.size());
        model.sections.push_back(section);
        model.materials.push_back(state->admission.material);
        model.lods.push_back(WgrModelLod{1,0,1,0});
        const std::array<uint64_t,1> expectedMeshes{producerMesh};
        model.modelAdmissionReceipt = std::make_shared<render::ModelAdmissionReceipt>(modelId,
            _geometryOwnerEpoch, actual->sourceAdmissionEpoch, initialTicket, expectedMeshes);
        const std::array<Record::MeshExpectation,1> meshExpected{{
            {producerMesh, Record::ExpectedMesh::Present, 0}}};
        const std::array<Record::ModelExpectation,1> modelExpected{{
            {modelId, Record::ExpectedModel::MappedValid}}};
        auto observation = std::make_shared<Record>(_geometryOwnerEpoch, actual->sourceAdmissionEpoch,
            initialTicket, meshExpected, modelExpected);
        if (!model.modelAdmissionReceipt->ValidFor(modelId) || !observation->Valid()) return failure;
        ResourceOp observe; observe.kind = ResourceOp::RetailPageRecordObserve;
        observe.retailPageRecordObservation = observation;
        _resourceOps.reserve(_resourceOps.size() + 3);
        state->models[0] = modelId;
        state->receipts[0] = model.modelAdmissionReceipt;
        state->observation = std::move(observation);
        state->pendingModelSlot = 0; state->pendingRequest = initialTicket;
        state->history.push_back({producerMesh,0,UINT32_MAX,UINT32_MAX,true});
        state->report.meshCount = 1;
        state->report.producerModel = modelId;
        state->report.requestId = state->report.retailRecordRequestId = initialTicket;
        state->report.retailRecordFreshIds = 2;
        state->report.knownPayloadBytes += meshCharge + opCharge;
        state->lastMeshFrame = _frameCounter;
        _resourceOps.push_back(std::move(mesh));
        _resourceOps.push_back(std::move(model));
        _resourceOps.push_back(std::move(observe));
        _retailPagePilot = std::move(state);
        return _retailPagePilot->report;
    }
    catch (...) { failure.status = GeometryReportStatus::Failed; failure.pageWorkState = "RetailRecordAdmissionFailed"; return failure; }
}

Engine::GeometryPageFixtureReport EngineWgpu::StepRetailPageRecordPilot(GeometryPageFixtureAction action)
{
    using namespace GeometryPages;
    GeometryPageFixtureReport unavailable;
    unavailable.status = GeometryReportStatus::Invalid;
    unavailable.retailRecordOnly = true;
    unavailable.pageWorkState = "RetailRecordUnavailable";
    if (!Foundation::IsMainThread() || !_retailPagePilot) return unavailable;
    auto state = _retailPagePilot;
    auto& s = *state;
    auto& r = s.report;
    constexpr uint64_t abortReserve = (Record::MaxMeshes + Record::MaxModels + 1) *
        sizeof(ResourceOp) + sizeof(Record) + 256;
    constexpr uint64_t workingLimit = RetailLimit - abortReserve;
    auto current = [&]() -> GeometryPageFixtureReport { s.RefreshWorkerReport(); return r; };
    if (action == GeometryPageFixtureAction::SnapshotRetailRecords) return current();
    auto refusal = [&](const char* reason) {
        auto copy = current(); copy.status = GeometryReportStatus::Invalid; copy.pageWorkState = reason; return copy;
    };
    auto failed = [&](const char* reason) {
        s.phase = RetailPagePilotState::Phase::Failed;
        r.status = GeometryReportStatus::Failed; r.pageWorkState = reason; return current();
    };
    auto nextRequest = [&]() -> uint64_t {
        if (!_nextGeometryPageRequest || _nextGeometryPageRequest == UINT64_MAX) return 0;
        return _nextGeometryPageRequest++;
    };
    auto updateResident = [&]() {
        r.hierarchyResidentMask = 0; r.hierarchyResidentPages = 0;
        for (uint32_t p = 0; p < s.Package().pages.size(); ++p)
            if (s.resident[p]) { r.hierarchyResidentMask |= uint64_t(1) << p; ++r.hierarchyResidentPages; }
        r.meshCount = uint32_t(s.history.size());
        r.meshesPresent = r.meshesAbsent = 0;
        for (const auto& row : s.history)
            if (row.present) ++r.meshesPresent; else ++r.meshesAbsent;
        r.retailRecordModelsPresent = 0; r.retailRecordModelsAbsent = 0;
        for (size_t i = 0; i < s.models.size(); ++i) if (s.models[i] != UINT32_MAX)
        { if (s.modelReady[i]) ++r.retailRecordModelsPresent;
          else ++r.retailRecordModelsAbsent; }
    };
    auto recordComplete = [&]() -> int {
        if (!s.observation) return -1;
        const auto observed = s.observation->Observe();
        if (observed.state == Record::State::Requested) return 0;
        if (observed.state != Record::State::Observed ||
            observed.ownerEpoch != _geometryOwnerEpoch ||
            observed.sourceAdmissionEpoch != r.sourceAdmissionEpoch ||
            observed.requestId != s.pendingRequest) return -1;
        if (!s.SourceCurrent() && !s.abortRequested) return -1;
        if (s.pendingModelSlot < s.models.size())
        {
            const auto receipt = s.receipts[s.pendingModelSlot];
            if (!receipt) return -1;
            const auto ack = receipt->Observe();
            if (ack.state == render::ModelAdmissionReceipt::State::Pending) return 0;
            if (ack.state != render::ModelAdmissionReceipt::State::ConsumedAccepted ||
                observed.modelCount != 1 || observed.rendererModels[0] != ack.rendererModel ||
                s.models[s.pendingModelSlot] != receipt->ProducerModel()) return -1;
            r.rendererModel = ack.rendererModel;
            if (r.modelAdmissionAccepted != UINT32_MAX) ++r.modelAdmissionAccepted;
        }
        for (size_t i = 0; i < s.observation->MeshCount(); ++i)
        {
            const auto expected = s.observation->MeshAt(i);
            auto* history = s.Find(expected.producer);
            if (!history || observed.rendererMeshes[i] == 0) return -1;
            if (expected.expected == Record::ExpectedMesh::Present)
            {
                if (history->renderer && history->renderer != observed.rendererMeshes[i]) return -1;
                history->renderer = observed.rendererMeshes[i];
            }
            else if (history->renderer != observed.rendererMeshes[i]) return -1;
        }
        r.retailRecordRequestId = r.observedRequestId = observed.requestId;
        r.retailRecordMeshesPresent = observed.presentMeshes;
        r.retailRecordMeshesAbsent = observed.absentMeshes;
        r.retailRecordModelsPresent = observed.mappedModels;
        r.retailRecordModelsAbsent = observed.absentModels;
        s.observation.reset(); s.pendingRequest = 0;
        return 1;
    };
    auto enqueueRecord = [&](std::span<const Record::MeshExpectation> meshes,
        std::span<const Record::ModelExpectation> models, uint64_t ticket,
        std::vector<ResourceOp>& staged) -> bool {
        auto packet = std::make_shared<Record>(_geometryOwnerEpoch, r.sourceAdmissionEpoch,
            ticket, meshes, models);
        if (!packet->Valid()) return false;
        ResourceOp op; op.kind = ResourceOp::RetailPageRecordObserve;
        op.retailPageRecordObservation = packet;
        staged.push_back(std::move(op));
        _resourceOps.reserve(_resourceOps.size() + staged.size());
        s.observation = std::move(packet);
        s.pendingRequest = ticket;
        r.requestId = r.retailRecordRequestId = ticket;
        for (auto& item : staged) _resourceOps.push_back(std::move(item));
        return true;
    };
    auto enqueueReadyHistory = [&](uint32_t slot) -> bool {
        if (slot >= s.models.size() || !s.modelReady[slot] || !s.SourceCurrent() ||
            s.history.empty() || s.history.size() > Record::MaxMeshes) return false;
        std::array<Record::MeshExpectation, Record::MaxMeshes> meshes{};
        std::array<Record::ModelExpectation, Record::MaxModels> models{};
        for (size_t i = 0; i < s.history.size(); ++i)
        {
            const auto& row = s.history[i];
            if (!row.producer || !row.renderer) return false;
            meshes[i] = {row.producer, row.present ? Record::ExpectedMesh::Present :
                Record::ExpectedMesh::Absent, row.present ? 0 : row.renderer};
        }
        size_t modelCount = 0;
        for (size_t i = 0; i < s.models.size(); ++i)
            if (s.models[i] != UINT32_MAX)
                models[modelCount++] = {s.models[i], s.modelReady[i] ?
                    Record::ExpectedModel::MappedValid : Record::ExpectedModel::Absent};
        std::vector<ResourceOp> staged; staged.reserve(1);
        const uint64_t extra = staged.capacity() * sizeof(ResourceOp) + sizeof(Record) + 256;
        if (r.knownPayloadBytes > workingLimit || extra > workingLimit - r.knownPayloadBytes)
            return false;
        const uint64_t ticket = nextRequest();
        if (!ticket || !enqueueRecord(std::span(meshes.data(),s.history.size()),
            std::span(models.data(),modelCount),ticket,staged)) return false;
        s.readyModelSlot = slot;
        s.pendingModelSlot = UINT32_MAX; // the model receipt was accepted by the preceding packet
        s.phase = RetailPagePilotState::Phase::AwaitReadyHistory;
        r.knownPayloadBytes += extra;
        r.status = GeometryReportStatus::Pending;
        r.pageWorkState = "RetailFullHistoryQueued";
        return true;
    };
    auto abortNow = [&]() -> GeometryPageFixtureReport {
        if (s.phase == RetailPagePilotState::Phase::Aborted) return current();
        if (s.phase == RetailPagePilotState::Phase::AwaitAbort) return current();
        s.abortRequested = true;
        if (s.worker) s.worker->Cancel(r.pageEpoch);
        if (s.observation)
        { r.status = GeometryReportStatus::Pending; r.pageWorkState = "RetailAbortWaitingRecord"; return current(); }
        if (s.worker)
        {
            const auto debt = s.worker->Snapshot();
            if (debt.liveJobs || debt.queued || debt.active || debt.ready || debt.reservedBytes)
            { r.status = GeometryReportStatus::Pending; r.pageWorkState = "RetailAbortWaitingWorker"; return current(); }
        }
        s.decodedPage = {}; s.decodedPageCharge = 0; s.workerRequest = 0;
        if (s.history.empty()) return failed("RetailAbortMissingHistory");
        std::array<Record::MeshExpectation, Record::MaxMeshes> meshes{};
        std::array<Record::ModelExpectation, Record::MaxModels> models{};
        std::vector<ResourceOp> staged;
        staged.reserve(s.history.size() + s.models.size() + 1);
        size_t meshCount = 0, modelCount = 0;
        for (auto model : s.models) if (model != UINT32_MAX)
        {
            ResourceOp retire; retire.kind = ResourceOp::ModelRetire; retire.model = model;
            staged.push_back(std::move(retire));
            models[modelCount++] = {model, Record::ExpectedModel::Absent};
        }
        for (const auto& row : s.history)
        {
            if (!row.renderer || meshCount >= meshes.size()) return failed("RetailAbortUnknownMeshRecord");
            if (row.present)
            { ResourceOp destroy; destroy.kind = ResourceOp::MeshDestroy; destroy.mesh = row.producer;
              staged.push_back(std::move(destroy)); }
            meshes[meshCount++] = {row.producer, Record::ExpectedMesh::Absent, row.renderer};
        }
        const uint64_t ticket = nextRequest();
        if (!ticket) return failed("RetailAbortRequestExhausted");
        const uint64_t debt = s.worker ? s.worker->Snapshot().reservedBytes : 0;
        const uint64_t extra = staged.capacity() * sizeof(ResourceOp) + sizeof(Record) + 256;
        if (r.knownPayloadBytes > RetailLimit || debt > RetailLimit - r.knownPayloadBytes ||
            extra > RetailLimit - r.knownPayloadBytes - debt) return failed("RetailAbortCapacity");
        if (!enqueueRecord(std::span(meshes.data(),meshCount), std::span(models.data(),modelCount),ticket,staged))
            return failed("RetailAbortPacketInvalid");
        s.phase = RetailPagePilotState::Phase::AwaitAbort;
        s.pendingModelSlot = UINT32_MAX;
        r.status = GeometryReportStatus::Pending;
        r.pageWorkState = "RetailAbortRecordsQueued";
        r.knownPayloadBytes += extra;
        return current();
    };
    try
    {
        if (action == GeometryPageFixtureAction::AbortRetailRecords) return abortNow();
        if (s.phase == RetailPagePilotState::Phase::Aborted ||
            (s.phase == RetailPagePilotState::Phase::Failed && !s.abortRequested)) return current();
        if (action == GeometryPageFixtureAction::ReleaseRetailRecords)
        {
            if (s.phase != RetailPagePilotState::Phase::FineReady || !r.retailRecordDistinctCuts ||
                s.refilled || !s.SourceCurrent() || !s.worker ||
                s.worker->Snapshot().reservedBytes || s.observation ||
                s.models[2] == UINT32_MAX || !s.modelReady[1] || !s.modelReady[2])
                return refusal("RetailReleaseUnavailable");
            std::array<Record::MeshExpectation, Record::MaxMeshes> meshes{};
            std::array<Record::ModelExpectation, Record::MaxModels> models{};
            std::vector<ResourceOp> staged; staged.reserve(s.history.size() + 2);
            ResourceOp retire; retire.kind = ResourceOp::ModelRetire; retire.model = s.models[2];
            staged.push_back(std::move(retire));
            for (size_t i = 0; i < s.history.size(); ++i)
            {
                const auto& row = s.history[i];
                if (!row.renderer) return refusal("RetailReleaseUnknownRecord");
                const bool nonroot = row.page != UINT32_MAX &&
                    std::find(s.nonrootPages.begin(),s.nonrootPages.end(),row.page) != s.nonrootPages.end();
                if (nonroot && row.present)
                {
                    ResourceOp destroy; destroy.kind = ResourceOp::MeshDestroy; destroy.mesh = row.producer;
                    staged.push_back(std::move(destroy));
                }
                meshes[i] = {row.producer, nonroot ? Record::ExpectedMesh::Absent :
                    row.present ? Record::ExpectedMesh::Present : Record::ExpectedMesh::Absent,
                    nonroot || !row.present ? row.renderer : 0};
            }
            models[0] = {s.models[0], Record::ExpectedModel::MappedValid};
            models[1] = {s.models[1], Record::ExpectedModel::MappedValid};
            models[2] = {s.models[2], Record::ExpectedModel::Absent};
            const uint64_t ticket = nextRequest();
            if (!ticket) return refusal("RetailReleaseRequestExhausted");
            const uint64_t extra = staged.capacity()*sizeof(ResourceOp) + sizeof(Record) + 256;
            if (r.knownPayloadBytes > workingLimit || extra > workingLimit - r.knownPayloadBytes ||
                !enqueueRecord(std::span(meshes.data(),s.history.size()),
                    std::span(models.data(),3),ticket,staged)) return refusal("RetailReleaseCapacity");
            s.phase = RetailPagePilotState::Phase::AwaitRelease;
            s.pendingModelSlot = UINT32_MAX;
            r.knownPayloadBytes += extra; r.status = GeometryReportStatus::Pending;
            r.pageWorkState = "RetailNonrootRetireQueued";
            return current();
        }
        if (action == GeometryPageFixtureAction::RefillRetailRecords)
        {
            if (s.phase != RetailPagePilotState::Phase::Released || !r.retailRecordDistinctCuts ||
                s.refilled || !s.SourceCurrent() || s.history.size() >= Record::MaxMeshes ||
                !s.worker || s.worker->Snapshot().reservedBytes)
                return refusal("RetailRefillUnavailable");
            s.refilled = true; s.target = RetailPagePilotState::Target::Refill;
            s.phase = RetailPagePilotState::Phase::Advance;
            r.status = GeometryReportStatus::Pending; r.pageWorkState = "RetailRefillRequested";
            return current();
        }
        if (action != GeometryPageFixtureAction::PollRetailRecords)
            return refusal("RetailActionUnsupported");
        if (s.abortRequested && s.phase == RetailPagePilotState::Phase::Failed && s.observation)
        {
            const int complete = recordComplete();
            if (complete == 0) return current();
            if (complete < 0) return failed("RetailAbortCannotCertifyPendingRecord");
            return abortNow();
        }
        if (s.abortRequested && !s.observation && s.phase != RetailPagePilotState::Phase::AwaitAbort)
            return abortNow();

        if (s.phase == RetailPagePilotState::Phase::AwaitFallback ||
            s.phase == RetailPagePilotState::Phase::AwaitMesh ||
            s.phase == RetailPagePilotState::Phase::AwaitModel ||
            s.phase == RetailPagePilotState::Phase::AwaitReadyHistory ||
            s.phase == RetailPagePilotState::Phase::AwaitRelease ||
            s.phase == RetailPagePilotState::Phase::AwaitAbort)
        {
            const int complete = s.phase == RetailPagePilotState::Phase::AwaitAbort && s.abortRecordSettled ?
                1 : recordComplete();
            if (complete == 0) return current();
            if (complete < 0) return failed("RetailRecordRejectedOrStale");
            if (s.phase == RetailPagePilotState::Phase::AwaitAbort)
            {
                s.abortRecordSettled = true;
                if (s.worker)
                {
                    const auto debt = s.worker->Snapshot();
                    if (debt.liveJobs || debt.queued || debt.active || debt.ready || debt.reservedBytes)
                    { r.pageWorkState = "RetailAbortWaitingWorker"; return current(); }
                }
                s.RefreshWorkerReport();
                s.worker.reset(); // join before the held native file and source refs leave
#ifdef _WIN32
                if(!RemoveRetailFile(s.file,s.path))return failed("RetailOwnedFileRemoveFailed");
#endif
                for (auto& row : s.history) row.present = false;
                s.modelReady.fill(false); s.resident.fill(0);
                updateResident();
                s.path.clear(); s.diskBase.reset(); s.admission = {};
                s.phase = RetailPagePilotState::Phase::Aborted;
                r.active = false; r.status = GeometryReportStatus::Ready;
                r.pageWorkState = "RetailAllRecordsAbsent";
                r.retailRecordMeshesAbsent = uint32_t(s.history.size());
                r.retailRecordModelsAbsent = uint32_t(std::count_if(s.models.begin(),s.models.end(),
                    [](uint32_t id) { return id != UINT32_MAX; }));
                return current();
            }
            if (s.phase == RetailPagePilotState::Phase::AwaitRelease)
            {
                for (auto& row : s.history)
                    if (row.page != UINT32_MAX &&
                        std::find(s.nonrootPages.begin(),s.nonrootPages.end(),row.page) != s.nonrootPages.end())
                    { row.present = false; s.clusterMesh[row.cluster] = 0; s.clusterIndices[row.cluster] = 0; }
                for (auto page : s.nonrootPages) s.resident[page] = 0;
                s.modelReady[2] = false;
                s.phase = RetailPagePilotState::Phase::Released;
                r.status = GeometryReportStatus::Ready;
                r.pageWorkState = "RetailNonrootRecordsAbsent";
                r.producerModel = s.models[1];
                r.rendererModel = s.receipts[1]->Observe().rendererModel;
                r.hierarchySelectedClusters = s.rootClusters;
                updateResident();
                if (s.abortRequested) return abortNow();
                return current();
            }
            // A consumed packet is enough to retire its recorded resources.
            // Abort does not need to create a fresh ready-history packet.
            if (s.abortRequested) return abortNow();
            if (s.phase == RetailPagePilotState::Phase::AwaitReadyHistory)
            {
                const uint32_t slot = s.readyModelSlot;
                if (slot < 1 || slot >= s.models.size() || !s.modelReady[slot])
                    return failed("RetailReadyHistorySlotInvalid");
                s.readyModelSlot = UINT32_MAX;
                updateResident();
                if (s.abortRequested) return abortNow();
                if (slot == 1 && r.retailRecordDistinctCuts)
                {
                    s.target = RetailPagePilotState::Target::Fine;
                    s.phase = RetailPagePilotState::Phase::Advance;
                    r.pageWorkState = "RetailRootHistoryPresent";
                }
                else
                {
                    s.phase = slot == 1 ? RetailPagePilotState::Phase::RootOnlyReady :
                        RetailPagePilotState::Phase::FineReady;
                    r.status = GeometryReportStatus::Ready;
                    r.pageWorkState = slot == 1 ? "RetailRootOnlyRecordReady" :
                        slot == 2 ? "RetailFineRecordReady" : "RetailRefillRecordReady";
                    return current();
                }
            }
            else if (s.phase == RetailPagePilotState::Phase::AwaitFallback)
            {
                s.modelReady[0] = true; s.pendingModelSlot = UINT32_MAX;
                s.phase = RetailPagePilotState::Phase::Advance;
                s.target = RetailPagePilotState::Target::Root;
                r.pageWorkState = "RetailFallbackRecordPresent";
            }
            else if (s.phase == RetailPagePilotState::Phase::AwaitMesh)
            {
                const auto cluster = s.pendingCluster;
                if (cluster >= s.Package().clusters.size() || s.decodedPage.clusters.empty() ||
                    s.clusterCursor >= s.decodedPage.clusters.size()) return failed("RetailPageCursorInvalid");
                const auto& payload = s.decodedPage.clusters[s.clusterCursor];
                if (payload.cluster != cluster) return failed("RetailPageClusterMismatch");
                s.clusterMesh[cluster] = s.history.back().producer;
                s.clusterIndices[cluster] = uint32_t(payload.indices.size());
                ++s.clusterCursor; s.pendingCluster = UINT32_MAX;
                s.phase = RetailPagePilotState::Phase::StagePage;
                r.pageWorkState = "RetailMeshRecordPresent";
            }
            else
            {
                const uint32_t slot = s.pendingModelSlot;
                if (slot >= s.models.size()) return failed("RetailModelSlotInvalid");
                s.modelReady[slot] = true; s.pendingModelSlot = UINT32_MAX;
                const auto& selected = slot == 1 ? s.rootClusters : s.fineClusters;
                r.hierarchySelectedClusters = selected;
                r.hierarchySelectedTriangles = 0;
                for (auto cluster : selected) r.hierarchySelectedTriangles += s.clusterIndices[cluster] / 3;
                r.producerModel = s.models[slot];
                if (!enqueueReadyHistory(slot)) return failed("RetailReadyHistoryUnavailable");
                updateResident(); return current();
            }
            updateResident();
            if (s.abortRequested) return abortNow();
        }
        if (s.phase == RetailPagePilotState::Phase::RootOnlyReady ||
            s.phase == RetailPagePilotState::Phase::FineReady ||
            s.phase == RetailPagePilotState::Phase::Released) return current();
        if (!s.SourceCurrent()) return failed("RetailSourceStale");
        const auto& package = s.Package();
        if (s.phase == RetailPagePilotState::Phase::AwaitWorker)
        {
            auto result = s.worker->Take(r.pageEpoch,s.workerRequest);
            if (!result)
            {
                const auto stats = s.worker->Snapshot();
                if (stats.failedEpoch == r.pageEpoch && stats.failedRequest == s.workerRequest)
                    return failed("RetailDiskPageWorkerFailed");
                return current();
            }
            const auto& source = result->hierarchySource;
            const auto& authority = *s.diskBase;
            const uint32_t page = s.pendingPage;
            bool exact = result->epoch == r.pageEpoch && result->request == s.workerRequest &&
                result->status == DecodeStatus::Decoded &&
                result->hierarchyDiskStatus == HierarchicalDiskReadStatus::Read &&
                result->ownership && source && result->hierarchyPageId == page &&
                result->hierarchyIdentity == package.identity &&
                source->pageId == page && source->path == authority.path &&
                source->metadataBytes == authority.metadataBytes &&
                source->expectedIdentity == authority.expectedIdentity &&
                source->expectedMetadataSha256 == authority.expectedMetadataSha256 &&
                source->fileBytes == authority.fileBytes && page < package.pages.size() &&
                result->hierarchyPage.group == package.pages[page].group &&
                result->hierarchyPage.clusters.size() == package.pages[page].clusters.size() &&
                HierarchicalOriginalOutside::ExactOriginalFinePageVertices(result->hierarchyPage,
                    s.admission.actual->actual.fine.vertices);
            for (size_t i = 0; exact && i < result->hierarchyPage.clusters.size(); ++i)
                exact = result->hierarchyPage.clusters[i].cluster == package.pages[page].clusters[i].cluster;
            if (!exact || !s.SourceCurrent()) return failed("RetailDiskPageStaleOrInvalid");
            const uint64_t pageCharge = HierarchicalDiskPageKnownBytes(result->hierarchyPage);
            const uint64_t debt = s.worker->Snapshot().reservedBytes;
            if (r.knownPayloadBytes > workingLimit || debt > workingLimit - r.knownPayloadBytes ||
                pageCharge > workingLimit - r.knownPayloadBytes - debt) return failed("RetailDiskPageCapacity");
            s.decodedPage = std::move(result->hierarchyPage);
            s.decodedPageCharge = pageCharge; r.knownPayloadBytes += pageCharge;
            s.clusterCursor = 0; s.workerRequest = 0;
            s.phase = RetailPagePilotState::Phase::StagePage;
            ++r.retailRecordWorkerReads;
            r.pageWorkState = "RetailDiskPageDecoded";
            result.reset(); // worker reservation exits before per-mesh staging
        }
        if (s.phase == RetailPagePilotState::Phase::StagePage)
        {
            if (s.clusterCursor == s.decodedPage.clusters.size())
            {
                if (s.pendingPage >= package.pages.size()) return failed("RetailPageIndexInvalid");
                s.resident[s.pendingPage] = 1;
                s.pendingPage = UINT32_MAX;
                s.decodedPage = {};
                if (s.decodedPageCharge <= r.knownPayloadBytes)
                    r.knownPayloadBytes -= s.decodedPageCharge;
                s.decodedPageCharge = 0; s.phase = RetailPagePilotState::Phase::Advance;
                updateResident(); r.pageWorkState = "RetailPageRecordsPresent";
            }
            else
            {
                if (s.lastMeshFrame == _frameCounter)
                { r.pageWorkState = "RetailOneMeshPerFrame"; return current(); }
                const auto& cluster = s.decodedPage.clusters[s.clusterCursor];
                if (s.history.size() >= Record::MaxMeshes || cluster.cluster >= package.clusters.size() ||
                    cluster.material != 0 || cluster.vertices.empty() || cluster.indices.empty() ||
                    s.clusterMesh[cluster.cluster]) return failed("RetailPageClusterInvalid");
                ResourceOp mesh; mesh.kind = ResourceOp::MeshCreate;
                mesh.verts = cluster.vertices;
                mesh.indices.reserve(cluster.indices.size());
                for (const auto& vertex : mesh.verts)
                    if (!RetailEnclosed(vertex,s.admission.sphereRadius)) return failed("RetailPageOutsideRadius");
                for (auto index : cluster.indices)
                { if (index > INT32_MAX) return failed("RetailPageIndexOverflow");
                  mesh.indices.push_back(VertexIndex(index)); }
                const uint64_t extra = mesh.verts.capacity()*sizeof(SVertex) +
                    mesh.indices.capacity()*sizeof(VertexIndex) + 2*sizeof(ResourceOp) + sizeof(Record) + 256;
                const uint64_t debt = s.worker->Snapshot().reservedBytes;
                if (r.knownPayloadBytes > workingLimit || debt > workingLimit-r.knownPayloadBytes ||
                    extra > workingLimit-r.knownPayloadBytes-debt)
                    return failed("RetailMeshCapacity");
                const uint64_t ticket = nextRequest();
                if (!ticket) return failed("RetailMeshRequestExhausted");
                mesh.mesh = AllocateProducerMesh();
                if (!mesh.mesh) return failed("RetailMeshIdExhausted");
                const uint64_t producer = mesh.mesh;
                std::vector<ResourceOp> staged; staged.reserve(2); staged.push_back(std::move(mesh));
                const std::array<Record::MeshExpectation,1> expected{{
                    {producer,Record::ExpectedMesh::Present,0}}};
                if (!enqueueRecord(expected,{},ticket,staged)) return failed("RetailMeshRecordPacketInvalid");
                s.history.push_back({producer,0,cluster.cluster,s.pendingPage,true});
                s.pendingCluster = cluster.cluster;
                s.lastMeshFrame = _frameCounter;
                s.phase = RetailPagePilotState::Phase::AwaitMesh;
                r.knownPayloadBytes += extra; r.meshCount = uint32_t(s.history.size());
                ++r.retailRecordFreshIds;
                r.pageWorkState = "RetailOneMeshQueued";
                return current();
            }
        }
        if (s.phase == RetailPagePilotState::Phase::Advance)
        {
            HierarchicalCutPlan plan;
            if (!s.Plan(plan)) return failed("RetailCutPlanInvalid");
            r.hierarchyRequiredMask = r.hierarchyMissingMask = 0;
            for (auto page : plan.requiredPages) r.hierarchyRequiredMask |= uint64_t(1) << page;
            for (auto page : plan.missingPages) r.hierarchyMissingMask |= uint64_t(1) << page;
            if (!plan.missingPages.empty())
            {
                if (!s.worker || s.worker->Snapshot().reservedBytes ||
                    s.worker->Snapshot().liveJobs) return current();
                HierarchicalPageBatch batch;
                if (r.knownPayloadBytes > workingLimit) return failed("RetailPageBudgetExceeded");
                const uint64_t available = workingLimit-r.knownPayloadBytes;
                std::array<uint8_t,64> inFlight{};
                if (!SelectHierarchicalPageBatch(package,plan,
                    std::span<const uint8_t>(inFlight.data(),package.pages.size()),
                    1,available,std::min<uint64_t>(available,65536),batch) || batch.pages.size() != 1)
                    return failed("RetailPageBatchCapacity");
                auto input = std::make_shared<HierarchicalDiskPageInput>(*s.diskBase);
                input->pageId = batch.pages.front();
                const uint64_t inputCharge = HierarchicalDiskPageInputKnownBytes(*input);
                if (inputCharge > available) return failed("RetailPageInputCapacity");
                const uint64_t ticket = nextRequest();
                if (!ticket) return failed("RetailPageRequestExhausted");
                const auto queued = s.worker->SubmitHierarchyDisk(r.pageEpoch,ticket,input,available-inputCharge);
                if (queued != AuthoredPageWorker::SubmitStatus::Queued)
                {
                    if (queued != AuthoredPageWorker::SubmitStatus::Busy)
                        return failed("RetailPageWorkerSubmissionRejected");
                    r.pageWorkState = "RetailPageWorkerBusy"; return current();
                }
                s.pendingPage = input->pageId; s.workerRequest = ticket;
                s.phase = RetailPagePilotState::Phase::AwaitWorker;
                r.pageRequest = ticket; r.pageWorkState = "RetailIndependentPageQueued";
                return current();
            }
            const auto& selected = s.target == RetailPagePilotState::Target::Root ?
                s.rootClusters : s.fineClusters;
            if (plan.state != HierarchicalCutState::RequestedCut ||
                plan.selectedClusters != selected || selected.empty() ||
                selected.size() > render::ModelAdmissionReceipt::MaxMeshes)
                return failed("RetailCompleteCutUnavailable");
            const uint32_t slot = s.Slot();
            if (s.models[slot] != UINT32_MAX || s.modelReady[slot])
                return failed("RetailCutModelSlotReused");
            ResourceOp model; model.kind = ResourceOp::ModelRegister;
            model.sphereRadius = s.admission.sphereRadius;
            model.name = slot == 1 ? "private-retail-root-record" :
                slot == 2 ? "private-retail-fine-record" : "private-retail-refill-record";
            model.sections.reserve(selected.size()); model.materials.reserve(selected.size());
            std::array<uint64_t,Record::MaxMeshes> expectedHandles{};
            std::array<Record::MeshExpectation,Record::MaxMeshes> expectedMeshes{};
            for (size_t i = 0; i < selected.size(); ++i)
            {
                const auto cluster = selected[i];
                if (cluster >= package.clusters.size() || !s.clusterMesh[cluster] ||
                    !s.resident[package.clusters[cluster].page] || !s.clusterIndices[cluster])
                    return failed("RetailCutMeshNotResident");
                auto* history = s.Find(s.clusterMesh[cluster]);
                if (!history || !history->present || !history->renderer)
                    return failed("RetailCutMeshUnobserved");
                auto section = s.admission.section;
                section.mesh = history->producer; section.index_begin = 0;
                section.index_count = s.clusterIndices[cluster];
                model.sections.push_back(section);
                model.materials.push_back(s.admission.material);
                expectedHandles[i] = history->producer;
                expectedMeshes[i] = {history->producer,Record::ExpectedMesh::Present,0};
            }
            model.lods.push_back(WgrModelLod{1,0,uint32_t(selected.size()),0});
            const uint64_t extra = model.sections.capacity()*sizeof(WgrModelSection) +
                model.materials.capacity()*sizeof(WgrModelMaterial) +
                model.lods.capacity()*sizeof(WgrModelLod) + 2*sizeof(ResourceOp) +
                sizeof(render::ModelAdmissionReceipt) + sizeof(Record) + 512;
            const uint64_t readyHistoryReserve = sizeof(ResourceOp) + sizeof(Record) + 256;
            const uint64_t debt = s.worker->Snapshot().reservedBytes;
            if (r.knownPayloadBytes > workingLimit || debt > workingLimit-r.knownPayloadBytes ||
                extra + readyHistoryReserve > workingLimit-r.knownPayloadBytes-debt)
                return failed("RetailCutModelCapacity");
            const uint64_t ticket = nextRequest();
            if (!ticket) return failed("RetailCutModelRequestExhausted");
            model.model = AllocateProducerModel();
            if (model.model == WGR_INVALID_MODEL) return failed("RetailCutModelIdExhausted");
            const uint32_t modelId = model.model;
            model.modelAdmissionReceipt = std::make_shared<render::ModelAdmissionReceipt>(
                modelId,_geometryOwnerEpoch,r.sourceAdmissionEpoch,ticket,
                std::span(expectedHandles.data(),selected.size()));
            if (!model.modelAdmissionReceipt->ValidFor(modelId)) return failed("RetailCutReceiptInvalid");
            const auto receipt = model.modelAdmissionReceipt;
            const std::array<Record::ModelExpectation,1> expectedModel{{
                {modelId,Record::ExpectedModel::MappedValid}}};
            std::vector<ResourceOp> staged; staged.reserve(2); staged.push_back(std::move(model));
            if (!enqueueRecord(std::span(expectedMeshes.data(),selected.size()),expectedModel,ticket,staged))
                return failed("RetailCutRecordPacketInvalid");
            s.models[slot] = modelId; s.receipts[slot] = receipt;
            s.pendingModelSlot = slot; s.phase = RetailPagePilotState::Phase::AwaitModel;
            r.knownPayloadBytes += extra; ++r.retailRecordFreshIds;
            r.pageWorkState = "RetailCompleteCutModelQueued";
            return current();
        }
        return current();
    }
    catch (...) { return failed("RetailRecordOwnerException"); }
}
} // namespace Poseidon
