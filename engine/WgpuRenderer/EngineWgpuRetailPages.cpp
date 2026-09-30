#include "EngineWgpu.hpp"
#include "RetailPagePilotAdmission.hpp"
#include "RetailPageInstanceObservation.hpp"
#include <Poseidon/Graphics/Rendering/AuthoredGeometryPageWorker.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalOriginalOutside.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalBinaryDemand.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageProjectedSurfaceBound.hpp>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cfloat>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace Poseidon
{
namespace
{
constexpr uint64_t RetailLimit = 1024 * 1024;
constexpr uint32_t RetailWorldResidencyCycleLimit = 3;
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
    enum class VisiblePhase : uint8_t { Dormant, AwaitAdd, Reference, AwaitSwitch,
        Root, Fine, AwaitRemove, Removed, Failed };
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
    // Only models created by this private record owner may be retired here.
    // The conventional world's retained model lease is never inserted here.
    std::array<bool,4> ownsModels{};
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
    bool reusableWorldFine = false;
    bool releaseRecordSettled = false;
    uint32_t retiringFineSlot = UINT32_MAX, retiringMeshCount = 0;
    uint64_t residencyReservation = 0, retirementAckRequest = 0;
    uint32_t lastRetiredMeshes = 0, compactedMeshes = 0, retiredFineModels = 0;
    Phase phase = Phase::AwaitFallback;
    Target target = Target::Root;
    std::shared_ptr<Record> observation;
    VisiblePhase visiblePhase = VisiblePhase::Dormant;
    WgrInstance visiblePlacement{};
    uint32_t visibleInstance = UINT32_MAX, visibleSelectedSlot = UINT32_MAX,
        visiblePendingSlot = UINT32_MAX, visibleSwitches = 0, visibleRemoveAttempts = 0;
    uint64_t visibleBirth = 0, visibleRequest = 0, visibleCameraGeneration = 0;
    uint32_t visibleRendererSlot = 0;
    bool visibleRecoveryRemove = false, visibleSourceFineReference = false;
    std::shared_ptr<render::RetailPageInstanceObservation> visibleObservation;

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
    uint32_t Slot() const { return target == Target::Root ? 1 :
        target == Target::Fine || reusableWorldFine ? 2 : 3; }
    uint32_t FineSlot() const { return reusableWorldFine ? 2 : refilled && modelReady[3] ? 3 : 2; }
};

// One selected, installed world object and one private page producer slot.  The
// original normal model is retained independently of the world object's lease
// until a returned-frame receipt proves that the private page is absent.
struct EngineWgpu::RetailWorldVisibleState
{
    enum class Phase : uint8_t { AwaitTakeover, Fine, Root, AwaitUpdate,
        AwaitRestore, AwaitCleanup, Restored, Removed, Failed };
    Phase phase = Phase::AwaitTakeover;
    Ref<LODShapeWithShadow> normalShape;
    bool modelRetained = false, invalidated = false;
    uint64_t ownerEpoch = 0, objectBirth = 0, sourceEpoch = 0, shapeBirth = 0,
        shapeQueryRevision = 0, pageEpoch = 0, pageBirth = 0, requestId = 0,
        cameraGeneration = 0;
    uint32_t originalProducer = UINT32_MAX, pageProducer = UINT32_MAX;
    uint32_t originalRenderer = 0, pageRenderer = 0;
    uint32_t selectedSlot = UINT32_MAX, pendingSlot = UINT32_MAX;
    uint32_t switches = 0, cleanupAttempts = 0;
    render::RetailWorldInstanceTransaction::ModelIdentity originalModel, pageModel;
    WgrInstance original{}, page{}; // both carry producer model IDs
    std::shared_ptr<render::RetailWorldInstanceTransaction> pair;
    std::shared_ptr<render::RetailPageInstanceObservation> cleanup;
    std::shared_ptr<const GeometryPages::RigidFinalSurface::Proof> surface;
    std::optional<GeometryPages::HierarchicalBinaryDemand::Policy> autoPolicy;
    std::chrono::steady_clock::time_point lastAutoObservation{};
    uint64_t lastAutoFrameGeneration = 0;
    bool autoEnabled = false;
    // Actual-world-only residency permits three cycles. Complete retirement
    // evidence precedes CPU history compaction; renderer model IDs still append.
    // Other private fixtures retain their original single-cycle history.
    enum class Residence : uint8_t { Disabled, Both, RetiringFine, RootOnly,
        RestoringForRefill, Refilling, RetakingFine, Complete, Cancelled };
    Residence residence = Residence::Disabled;
    bool residencyEnabled = false;
    uint64_t residencyTriggerCamera = 0, retirementRequest = 0, refillRequest = 0, fallbackCamera = 0;
    uint64_t fallbackRequest = 0, fallbackPageBirth = 0, fallbackEpoch = 0, refillAtFallback = 0;
    uint32_t fallbackOriginalFlags = UINT32_MAX;
    bool fallbackPageAbsent = false, refillHandoff = false;
    uint32_t residencyCycles = 0;
    uint64_t autoObservationOffset = 0, autoRefineOffset = 0, autoCoarsenOffset = 0;
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
        const char* worldAutoFlag = std::getenv("WGR_GEOMETRY_PAGE_RETAIL_WORLD_AUTO");
        const bool worldAuto = worldAutoFlag && worldAutoFlag[0] == '1' && !worldAutoFlag[1];
        const auto proof = input.certifiedSurface;
        if (worldAuto)
        {
            if (!proof || !proof->certificate || !proof->root || !proof->fine ||
                !input.knownProofBytes || input.knownProofBytes > 512 * 1024 ||
                input.knownAuthorityBytes < input.knownProofBytes || !actual || !product ||
                !product->manifest || !product->image ||
                proof->root->knownCapacityBytes > 512 * 1024 ||
                proof->fine->knownCapacityBytes >
                    512 * 1024 - proof->root->knownCapacityBytes ||
                input.knownProofBytes != sizeof(RigidFinalSurface::Proof) +
                    sizeof(RigidFinalSurface::Certificate) +
                    proof->root->knownCapacityBytes + proof->fine->knownCapacityBytes + 512 ||
                proof->certificate->sourceAdmissionEpoch != actual->sourceAdmissionEpoch ||
                proof->certificate->ownerEpoch != actual->ownerEpoch ||
                proof->certificate->modelBirth != actual->modelBirth ||
                proof->certificate->shapeQueryRevision != actual->shapeQueryRevision ||
                !(proof->certificate->identity == product->manifest->metadata.identity) ||
                !(proof->certificate->originalSource == actual->actual.source) ||
                proof->certificate->fileSha256 != product->fileSha256 ||
                proof->certificate->metadataSha256 != product->image->metadataSha256)
                return failure;
        }
        else if (proof || input.knownProofBytes) return failure;
        if (!actual || !product || !product->fineTriangleSetExact || !product->image || !product->manifest || !input.modelLease ||
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
        for (const auto& uv : input.material.layer_uv)
            if (uv[0]!=1 || uv[1]!=1 || uv[2]!=0 || uv[3]!=0) return failure;
        for (const auto& colour : input.material.layer_colour)
            for (auto component : colour) if (component!=1) return failure;
        const char* visibleGate = std::getenv("WGR_GEOMETRY_PAGE_RETAIL_VISIBLE");
        const bool visibleSourceFineReference = visibleGate && !std::strcmp(visibleGate, "1");
        const char* worldVisibleGate = std::getenv("WGR_GEOMETRY_PAGE_RETAIL_WORLD_VISIBLE");
        // This selects an allocation optimization, never source/world authority.
        // The actual world retains its conventional original for other passes
        // and refill fallback, so it needs no duplicate diagnostic GPU model.
        const bool actualWorldOnly = worldVisibleGate && !std::strcmp(worldVisibleGate,"1") &&
            !visibleSourceFineReference;
        const char* worldResidencyGate = std::getenv("WGR_GEOMETRY_PAGE_RETAIL_WORLD_RESIDENCY");
        const bool reusableWorldFine = actualWorldOnly && worldAuto && worldResidencyGate &&
            !std::strcmp(worldResidencyGate,"1");
        const size_t referenceIndex = visibleSourceFineReference ? 1 : 0;
        if (actual->finalLevels[referenceIndex] < 0) return failure;
        const auto& reference = visibleSourceFineReference ? actual->actual.fine : actual->actual.coarse;
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
        threshold = worldAuto ? std::bit_cast<float>(proof->certificate->rootThresholdBits) :
            std::nextafter(threshold, std::numeric_limits<float>::infinity());
        if (!std::isfinite(threshold) || threshold == FLT_MAX ||
            !PlanHierarchicalCut(p, threshold,
                {p.identity, std::span<const uint8_t>(allResident.data(), p.pages.size())}, root) ||
            !PlanHierarchicalCut(p, 0,
                {p.identity, std::span<const uint8_t>(allResident.data(), p.pages.size())}, fine) ||
            root.state != HierarchicalCutState::RequestedCut || fine.state != HierarchicalCutState::RequestedCut ||
            root.selectedClusters.empty() || fine.selectedClusters.empty() ||
            root.selectedClusters.size() > Record::MaxMeshes || fine.selectedClusters.size() > Record::MaxMeshes)
            return failure;
        if (worldAuto &&
            (!RigidFinalSurface::MatchesSelected(*proof->root,root,threshold,proof->root->packed) ||
             !RigidFinalSurface::MatchesSelected(*proof->fine,fine,0.f,proof->fine->packed) ||
             proof->certificate->rootTriangles != proof->root->packed.indices.size()/3 ||
             proof->certificate->fineTriangles != proof->fine->packed.indices.size()/3))
            return failure;
        const bool distinct = root.selectedClusters != fine.selectedClusters;
        uint32_t initialMeshes = actualWorldOnly ? 0 : 1, refillMeshes = 0;
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
        state->visibleSourceFineReference = visibleSourceFineReference;
        state->reusableWorldFine = reusableWorldFine;
        state->admission = std::move(input);
        state->rootClusters = root.selectedClusters; state->fineClusters = fine.selectedClusters;
        state->rootPages = root.requiredPages; state->finePages = fine.requiredPages;
        for (auto page : state->finePages)
            if (std::find(state->rootPages.begin(), state->rootPages.end(), page) == state->rootPages.end())
                state->nonrootPages.push_back(page);
        state->rootThreshold = threshold;
        if (reusableWorldFine && distinct)
        {
            // Reserve the bounded three-cycle producer's worst private queue
            // phases up front. Existing per-operation capacity checks and
            // cumulative charges remain conservative; this is not a claim of
            // constant renderer memory (renderer model births append rows).
            uint64_t upload=0, clusters=0;
            for (auto page : state->nonrootPages)
            { upload += p.pages[page].uploadBytes; clusters += p.pages[page].clusters.size(); }
            const uint64_t oneCycle = 2*upload + clusters*(2*sizeof(ResourceOp)+sizeof(Record)+256) +
                2*fine.selectedClusters.size()*(sizeof(WgrModelSection)+sizeof(WgrModelMaterial)) +
                (Record::MaxMeshes+7)*sizeof(ResourceOp) +
                4*sizeof(Record)+sizeof(render::ModelAdmissionReceipt)+2048;
            if (oneCycle > workingLimit/RetailWorldResidencyCycleLimit) return failure;
            state->residencyReservation=oneCycle*RetailWorldResidencyCycleLimit;
        }
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
            state->history.capacity() * sizeof(RetailPagePilotState::MeshRow) + 4096 +
            state->residencyReservation;
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
        state->report.retailFineTriangleSetExact = product->fineTriangleSetExact;
        state->report.retailWorldSurfaceCertified = worldAuto;
        state->report.retailWorldSurfaceStatus = worldAuto ?
            uint32_t(RigidFinalSurface::Status::Certified) : 0;
        state->report.retailWorldSurfaceUpper = worldAuto ?
            proof->certificate->hausdorffUpper : 0;
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
        state->report.retailVisibleReferenceSourceLevel = actualWorldOnly ? UINT32_MAX :
            uint32_t(actual->finalLevels[referenceIndex]);
        state->report.retailVisibleReferenceTriangles = actualWorldOnly ? 0 :
            uint32_t(reference.indices.size() / 3);
        state->report.knownPayloadBytes = known;
        state->report.retailWorldResidencyCycleLimit = reusableWorldFine ? RetailWorldResidencyCycleLimit : 1;
        state->report.retailWorldResidencyReservedBytes = state->residencyReservation;
        state->report.active = true;
        state->report.pageEpoch = _nextGeometryPageEpoch++;
        if (actualWorldOnly)
        {
            state->phase = RetailPagePilotState::Phase::Advance;
            state->target = RetailPagePilotState::Target::Root;
            state->report.pageWorkState = "RetailWorldPagesWithoutDiagnosticReference";
            _retailPagePilot = std::move(state);
            return _retailPagePilot->report;
        }
        state->report.pageWorkState = referenceIndex == 1 ?
            "RetailSourceFineReferenceQueued" : "RetailFallbackQueued";
        if (_frameCounter == UINT32_MAX) return failure;
        ResourceOp mesh; mesh.kind = ResourceOp::MeshCreate;
        mesh.verts = reference.vertices;
        mesh.indices.reserve(reference.indices.size());
        for (auto index : reference.indices)
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
        model.name = visibleSourceFineReference ?
            "private-retail-source-fine-reference" : "private-retail-fallback-record";
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
        state->ownsModels[0] = true;
        state->receipts[0] = model.modelAdmissionReceipt;
        state->observation = std::move(observation);
        state->pendingModelSlot = 0; state->pendingRequest = initialTicket;
        state->history.push_back({producerMesh,0,UINT32_MAX,UINT32_MAX,true});
        state->report.meshCount = 1;
        state->report.producerModel = modelId;
        state->report.requestId = state->report.retailRecordRequestId = initialTicket;
        state->report.retailRecordFreshIds = 2;
        state->report.retailDiagnosticReferenceAllocated = true;
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
    auto current = [&]() -> GeometryPageFixtureReport {
        s.RefreshWorkerReport();
        r.retailWorldResidencyCycleLimit = s.reusableWorldFine ? RetailWorldResidencyCycleLimit : 1;
        r.retailWorldResidencyReservedBytes = s.residencyReservation;
        r.retailWorldLastRetiredMeshes = s.lastRetiredMeshes;
        r.retailWorldCompactedMeshes = s.compactedMeshes;
        r.retailWorldRetiredFineModels = s.retiredFineModels;
        r.retailWorldRetirementAckRequest = s.retirementAckRequest;
        return r;
    };
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
        if (slot >= s.models.size() || !s.ownsModels[slot] || !s.modelReady[slot] || !s.SourceCurrent() ||
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
            {
                if (!s.ownsModels[i]) return false;
                models[modelCount++] = {s.models[i], s.modelReady[i] ?
                    Record::ExpectedModel::MappedValid : Record::ExpectedModel::Absent};
            }
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
        if (_retailWorldVisible &&
            _retailWorldVisible->phase != RetailWorldVisibleState::Phase::Restored &&
            _retailWorldVisible->phase != RetailWorldVisibleState::Phase::Removed)
            return refusal("RetailWorldRestoreOrRemoveRequired");
        if (s.visiblePhase != RetailPagePilotState::VisiblePhase::Dormant &&
            s.visiblePhase != RetailPagePilotState::VisiblePhase::Removed)
            return refusal("RetailVisibleRemoveRequired");
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
        if (s.history.empty())
        {
            // The world-only path may be cancelled before its first private
            // mesh/model is ever queued. No zero-row renderer receipt exists:
            // this is owner cancellation of an unused producer, not a GPU ACK.
            if (r.retailDiagnosticReferenceAllocated || r.retailRecordFreshIds ||
                std::any_of(s.models.begin(),s.models.end(),[](uint32_t id){return id!=UINT32_MAX;}) ||
                std::any_of(s.ownsModels.begin(),s.ownsModels.end(),[](bool owned){return owned;}) ||
                std::any_of(s.clusterMesh.begin(),s.clusterMesh.end(),[](uint64_t id){return id!=0;}))
                return failed("RetailAbortMissingHistory");
            s.RefreshWorkerReport();
            s.worker.reset(); // join before exact owned-file cleanup
#ifdef _WIN32
            if (!RemoveRetailFile(s.file,s.path)) return failed("RetailOwnedFileRemoveFailed");
#endif
            s.resident.fill(0); updateResident();
            s.path.clear(); s.diskBase.reset(); s.admission = {};
            s.phase = RetailPagePilotState::Phase::Aborted;
            r.active = false; r.status = GeometryReportStatus::Ready;
            r.pageWorkState = "RetailCancelledBeforeGpuResources";
            r.retailRecordMeshesPresent = r.retailRecordMeshesAbsent = 0;
            r.retailRecordModelsPresent = r.retailRecordModelsAbsent = 0;
            return current();
        }
        std::array<Record::MeshExpectation, Record::MaxMeshes> meshes{};
        std::array<Record::ModelExpectation, Record::MaxModels> models{};
        std::vector<ResourceOp> staged;
        staged.reserve(s.history.size() + s.models.size() + 1);
        size_t meshCount = 0, modelCount = 0;
        for (size_t slot=0;slot<s.models.size();++slot) if (s.models[slot] != UINT32_MAX)
        {
            if (!s.ownsModels[slot]) return failed("RetailAbortUnownedModel");
            const auto model=s.models[slot];
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
            const uint32_t fineSlot=s.FineSlot();
            const bool autoRelease = _retailWorldVisible &&
                _retailWorldVisible->residencyEnabled &&
                _retailWorldVisible->residence == RetailWorldVisibleState::Residence::RetiringFine;
            if (s.phase != RetailPagePilotState::Phase::FineReady || !r.retailRecordDistinctCuts ||
                s.nonrootPages.empty() ||
                (_retailWorldVisible &&
                 (_retailWorldVisible->phase != RetailWorldVisibleState::Phase::Root ||
                  (_retailWorldVisible->autoEnabled && !autoRelease) ||
                  _retailWorldVisible->pair || _retailWorldVisible->cleanup ||
                  !r.retailWorldReturned)) ||
                (s.visiblePhase != RetailPagePilotState::VisiblePhase::Dormant &&
                 s.visiblePhase != RetailPagePilotState::VisiblePhase::Removed &&
                 (s.visiblePhase != RetailPagePilotState::VisiblePhase::Root ||
                  s.visibleSelectedSlot != 1 || !r.retailVisibleReturned)) ||
                (s.refilled && !s.reusableWorldFine) || !s.SourceCurrent() || !s.worker ||
                s.worker->Snapshot().reservedBytes || s.observation ||
                s.models[fineSlot] == UINT32_MAX || !s.ownsModels[1] || !s.ownsModels[fineSlot] ||
                !s.modelReady[1] || !s.modelReady[fineSlot])
                return refusal("RetailReleaseUnavailable");
            std::array<Record::MeshExpectation, Record::MaxMeshes> meshes{};
            std::array<Record::ModelExpectation, Record::MaxModels> models{};
            std::vector<ResourceOp> staged; staged.reserve(s.history.size() + 2);
            ResourceOp retire; retire.kind = ResourceOp::ModelRetire; retire.model = s.models[fineSlot];
            staged.push_back(std::move(retire));
            uint32_t retiredMeshes=0;
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
                    ++retiredMeshes;
                }
                meshes[i] = {row.producer, nonroot ? Record::ExpectedMesh::Absent :
                    row.present ? Record::ExpectedMesh::Present : Record::ExpectedMesh::Absent,
                    nonroot || !row.present ? row.renderer : 0};
            }
            size_t modelCount=0;
            if (s.models[0] != UINT32_MAX)
            {
                if (!s.ownsModels[0] || !s.modelReady[0]) return refusal("RetailReleaseUnownedReference");
                models[modelCount++] = {s.models[0], Record::ExpectedModel::MappedValid};
            }
            models[modelCount++] = {s.models[1], Record::ExpectedModel::MappedValid};
            models[modelCount++] = {s.models[fineSlot], Record::ExpectedModel::Absent};
            const uint64_t ticket = nextRequest();
            if (!ticket) return refusal("RetailReleaseRequestExhausted");
            const uint64_t extra = staged.capacity()*sizeof(ResourceOp) + sizeof(Record) + 256;
            if (r.knownPayloadBytes > workingLimit || extra > workingLimit - r.knownPayloadBytes ||
                !enqueueRecord(std::span(meshes.data(),s.history.size()),
                    std::span(models.data(),modelCount),ticket,staged)) return refusal("RetailReleaseCapacity");
            s.phase = RetailPagePilotState::Phase::AwaitRelease;
            s.retiringFineSlot=fineSlot; s.retiringMeshCount=retiredMeshes; s.releaseRecordSettled=false;
            s.pendingModelSlot = UINT32_MAX;
            r.knownPayloadBytes += extra; r.status = GeometryReportStatus::Pending;
            r.pageWorkState = "RetailNonrootRetireQueued";
            return current();
        }
        if (action == GeometryPageFixtureAction::RefillRetailRecords)
        {
            if (const auto world = _retailWorldVisible; world && world->residencyEnabled)
            {
                // This bool is only a synchronous owner handoff. It does not
                // authenticate a source or replace the returned/literal facts.
                if (!world->refillHandoff || world->residence != RetailWorldVisibleState::Residence::Refilling ||
                    world->phase != RetailWorldVisibleState::Phase::Restored || world->pair || world->cleanup ||
                    world->invalidated || world->residencyCycles >=
                        (s.reusableWorldFine ? RetailWorldResidencyCycleLimit : 1) || !r.retailWorldRestored ||
                    !r.retailWorldReturned || r.retailWorldPagePresent || !world->fallbackPageAbsent ||
                    world->fallbackOriginalFlags != 0 || !world->fallbackRequest || !world->fallbackEpoch ||
                    world->fallbackPageBirth != world->pageBirth || world->refillAtFallback != 0 ||
                    !world->fallbackCamera || world->fallbackCamera != world->cameraGeneration)
                    return refusal("RetailRefillConventionalHandoffRequired");
                AcquireProducerWindow("retail world refill literal conventional rows");
                const auto original = GetRetailWorldInstanceEvidence(world->originalProducer,
                    world->originalModel,world->originalRenderer);
                const auto page = GetRetailWorldInstanceEvidence(world->pageProducer,world->pageModel,world->pageRenderer);
                WgrInstance expected = world->original; expected.model = world->originalModel.rendererModel;
                const WgrInstance zero{};
                if (!original.getterSucceeded || !page.getterSucceeded ||
                    original.producerState != render::RetailWorldInstanceTransaction::ProducerState::Present ||
                    original.consumerInstanceBirth != world->objectBirth ||
                    original.consumerModelBirth != world->originalModel.modelBirth ||
                    original.mappedRendererHandle != world->originalRenderer ||
                    original.mappedRendererModel != world->originalModel.rendererModel ||
                    original.cpu.status != WGR_INSTANCE_CPU_FACT_PRESENT ||
                    original.cpu.queried_handle != world->originalRenderer ||
                    original.cpu.instance_epoch < world->fallbackEpoch ||
                    std::memcmp(&expected,&original.cpu.row,sizeof(expected)) ||
                    page.producerState != render::RetailWorldInstanceTransaction::ProducerState::Unmapped ||
                    page.consumerInstanceBirth || page.mappedRendererHandle ||
                    page.cpu.status != WGR_INSTANCE_CPU_FACT_ABSENT ||
                    page.cpu.queried_handle != world->pageRenderer || page.cpu.resolved_slot != UINT32_MAX ||
                    page.cpu.instance_epoch != original.cpu.instance_epoch ||
                    std::memcmp(&zero,&page.cpu.row,sizeof(zero)))
                    return refusal("RetailRefillConventionalRowsChanged");
            }
            if (s.phase != RetailPagePilotState::Phase::Released || !r.retailRecordDistinctCuts ||
                (s.refilled && !s.reusableWorldFine) || !s.SourceCurrent() || s.history.size() >= Record::MaxMeshes ||
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
            const int complete = (s.phase == RetailPagePilotState::Phase::AwaitAbort && s.abortRecordSettled) ||
                (s.phase == RetailPagePilotState::Phase::AwaitRelease && s.releaseRecordSettled) ?
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
                s.releaseRecordSettled=true;
                if (s.reusableWorldFine)
                {
                    const auto world=_retailWorldVisible;
                    if (!world || !world->residencyEnabled || world->invalidated ||
                        world->cleanup || world->pair ||
                        (world->phase != RetailWorldVisibleState::Phase::Root &&
                         world->phase != RetailWorldVisibleState::Phase::Restored))
                    { r.pageWorkState="RetailRetireCompactionWaitingWorldAck"; return current(); }
                    const auto debt=s.worker->Snapshot();
                    if (debt.liveJobs || debt.queued || debt.active || debt.ready || debt.reservedBytes ||
                        !s.decodedPage.clusters.empty() || s.pendingModelSlot != UINT32_MAX)
                        return failed("RetailRetireCompactionOutstandingOwner");
                    AcquireProducerWindow("retail world proved retired history compaction");
                    if (!s.SourceCurrent() || world->invalidated || world->pair || world->cleanup ||
                        s.retiringFineSlot != 2 || !s.ownsModels[2] || !s.modelReady[2] ||
                        world->selectedSlot == 2 || !s.retiringMeshCount)
                        return failed("RetailRetireCompactionAuthority");
                    // Only complete actual absent mesh/model evidence permits
                    // dropping history. Old handles are never queried again.
                    s.lastRetiredMeshes=s.retiringMeshCount;
                    s.retirementAckRequest=r.observedRequestId;
                    ++s.retiredFineModels;
                }
                for (auto& row : s.history)
                    if (row.page != UINT32_MAX &&
                        std::find(s.nonrootPages.begin(),s.nonrootPages.end(),row.page) != s.nonrootPages.end())
                    { row.present = false; s.clusterMesh[row.cluster] = 0; s.clusterIndices[row.cluster] = 0; }
                for (auto page : s.nonrootPages) s.resident[page] = 0;
                s.modelReady[s.retiringFineSlot] = false;
                if (s.reusableWorldFine)
                {
                    const auto before=s.history.size();
                    std::erase_if(s.history,[&](const RetailPagePilotState::MeshRow& row) {
                        return !row.present && row.page != UINT32_MAX &&
                            std::find(s.nonrootPages.begin(),s.nonrootPages.end(),row.page) != s.nonrootPages.end();
                    });
                    const auto removed=before-s.history.size();
                    if (removed != s.lastRetiredMeshes) return failed("RetailRetiredHistoryCountMismatch");
                    s.compactedMeshes+=uint32_t(removed);
                    s.models[2]=UINT32_MAX; s.receipts[2].reset(); s.ownsModels[2]=false;
                    s.workerRequest=0;
                }
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
                r.pageWorkState = s.visibleSourceFineReference ?
                    "RetailSourceFineReferenceRecordPresent" : "RetailFallbackRecordPresent";
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
            s.ownsModels[slot] = true;
            s.pendingModelSlot = slot; s.phase = RetailPagePilotState::Phase::AwaitModel;
            r.knownPayloadBytes += extra; ++r.retailRecordFreshIds;
            r.pageWorkState = "RetailCompleteCutModelQueued";
            return current();
        }
        return current();
    }
    catch (...) { return failed("RetailRecordOwnerException"); }
}

Engine::GeometryPageFixtureReport EngineWgpu::StepRetailPageVisiblePilot(GeometryPageFixtureAction action)
{
    using InstanceRecord = render::RetailPageInstanceObservation;
    GeometryPageFixtureReport unavailable;
    unavailable.status = GeometryReportStatus::Invalid;
    unavailable.pageWorkState = "RetailVisibleUnavailable";
    const char* gate = std::getenv("WGR_GEOMETRY_PAGE_RETAIL_VISIBLE");
    if (!gate || std::strcmp(gate,"1") || !Foundation::IsMainThread() || !_renderer ||
        !_gpuDriven || !_retailPagePilot) return unavailable;
    auto& s = *_retailPagePilot;
    auto& r = s.report;
    auto result = [&]() -> GeometryPageFixtureReport { s.RefreshWorkerReport(); return r; };
    auto refuse = [&](const char* why) -> GeometryPageFixtureReport {
        auto copy = result(); copy.status = GeometryReportStatus::Invalid;
        copy.pageWorkState = why; return copy;
    };
    auto fail = [&](const char* why) -> GeometryPageFixtureReport {
        s.visiblePhase = RetailPagePilotState::VisiblePhase::Failed;
        r.status = GeometryReportStatus::Failed; r.pageWorkState = why;
        r.retailVisiblePhase = why; return result();
    };
    auto nextRequest = [&]() -> uint64_t {
        if (!_nextGeometryPageRequest || _nextGeometryPageRequest == UINT64_MAX) return 0;
        return _nextGeometryPageRequest++;
    };
    auto workerEmpty = [&]() {
        if (!s.worker) return false;
        const auto debt = s.worker->Snapshot();
        return !debt.liveJobs && !debt.queued && !debt.active && !debt.ready && !debt.reservedBytes;
    };
    auto modelReady = [&](uint32_t slot) {
        return slot < s.models.size() && s.models[slot] != UINT32_MAX && s.modelReady[slot] &&
            s.receipts[slot] && s.receipts[slot]->ProducerModel() == s.models[slot] &&
            s.receipts[slot]->OwnerEpoch() == _geometryOwnerEpoch &&
            s.receipts[slot]->SourceAdmissionEpoch() == r.sourceAdmissionEpoch &&
            s.receipts[slot]->Observe().state == render::ModelAdmissionReceipt::State::ConsumedAccepted;
    };
    auto cutPresent = [&](uint32_t slot) {
        if (!modelReady(slot)) return false;
        if (slot == 0) return !s.history.empty() && s.history.front().present &&
            s.history.front().renderer && s.history.front().producer;
        const auto& selected = slot == 1 ? s.rootClusters : s.fineClusters;
        if (selected.empty() || selected.size() > render::ModelAdmissionReceipt::MaxMeshes)
            return false;
        for (uint32_t cluster : selected)
        {
            if (cluster >= s.clusterMesh.size() || !s.clusterMesh[cluster]) return false;
            const auto* row = s.Find(s.clusterMesh[cluster]);
            if (!row || !row->present || !row->renderer || row->page >= s.resident.size() ||
                !s.resident[row->page]) return false;
        }
        return true;
    };
    try
    {
        if (action == GeometryPageFixtureAction::BeginRetailVisible)
        {
            if (s.visiblePhase != RetailPagePilotState::VisiblePhase::Dormant ||
                !r.active || (s.phase != RetailPagePilotState::Phase::FineReady &&
                              s.phase != RetailPagePilotState::Phase::RootOnlyReady) ||
                s.observation || !workerEmpty() || !s.SourceCurrent() || !cutPresent(0) ||
                !_nextRetailVisibleBirth || _nextRetailVisibleBirth == UINT64_MAX)
                return refuse("RetailVisibleAdmissionUnavailable");
            constexpr uint64_t abortReserve = (Record::MaxMeshes + Record::MaxModels + 1) *
                sizeof(ResourceOp) + sizeof(Record) + 256;
            // Add + at most eight switches + at most three exact Remove
            // attempts, including two idempotent returned-frame retries.
            constexpr uint64_t visibleReservation = 12 *
                (sizeof(InstanceOp) + sizeof(InstanceRecord) + 64);
            if (r.knownPayloadBytes > RetailLimit - abortReserve ||
                visibleReservation > RetailLimit - abortReserve - r.knownPayloadBytes)
                return refuse("RetailVisibleCapacity");
            WgrInstance placement{};
            placement.world.m[0] = placement.world.m[5] = placement.world.m[10] =
                placement.world.m[15] = 1;
            placement.world.m[12] = 1200; placement.world.m[13] = 15;
            placement.world.m[14] = 1240;
            placement.center = {1200,15,1240,1};
            placement.model = s.models[0];
            placement.flags = WGR_INSTANCE_MAIN_CAMERA_ONLY;
            _instanceOps.reserve(_instanceOps.size() + 1);
            const uint32_t instance = ReserveRetailInstanceHandle();
            if (instance == UINT32_MAX) return refuse("RetailVisibleInstanceIdExhausted");
            const uint64_t ticket = nextRequest();
            if (!ticket) { ReleaseRetailInstanceHandle(instance); return refuse("RetailVisibleRequestExhausted"); }
            InstanceRecord::Request request;
            request.binding = {_geometryOwnerEpoch,r.sourceAdmissionEpoch,r.pageEpoch,ticket,
                _nextRetailVisibleBirth,instance,s.models[0]};
            request.action = InstanceRecord::Action::Add;
            request.expectedState = InstanceRecord::Expected::Present;
            request.expected = placement;
            std::shared_ptr<InstanceRecord> receipt;
            try { receipt = std::make_shared<InstanceRecord>(request); }
            catch (...) { ReleaseRetailInstanceHandle(instance); throw; }
            if (!receipt->Valid())
            { ReleaseRetailInstanceHandle(instance); return refuse("RetailVisibleAddPacketInvalid"); }
            EnqueueInstanceAddAt(instance,placement,receipt);
            s.visibleObservation = std::move(receipt);
            s.visiblePlacement = placement; s.visibleInstance = instance;
            s.visiblePendingSlot = 0; s.visibleBirth = _nextRetailVisibleBirth++;
            s.visibleRequest = ticket; s.visiblePhase = RetailPagePilotState::VisiblePhase::AwaitAdd;
            r.retailVisiblePilot = true; r.retailRecordOnly = false;
            r.retailVisibleBirth = s.visibleBirth; r.retailVisibleRequest = ticket;
            r.retailVisiblePhase = "AwaitAdd"; r.retailVisibleReturned = false;
            r.retailVisiblePresent = false; r.retailVisibleRemoved = false;
            r.producerInstance = instance; r.retailVisibleModel = s.models[0];
            r.knownPayloadBytes += visibleReservation;
            r.status = GeometryReportStatus::Pending;
            r.pageWorkState = "RetailVisibleAddQueued";
            return result();
        }
        if (action == GeometryPageFixtureAction::PollRetailVisible)
        {
            if (!s.visibleObservation) return result();
            const auto observed = s.visibleObservation->Observe();
            if (observed.state == InstanceRecord::State::Requested)
            { r.status = GeometryReportStatus::Pending; return result(); }
            const auto request = s.visibleObservation->GetRequest();
            const bool exactToken = observed.binding.ownerEpoch == _geometryOwnerEpoch &&
                observed.binding.sourceAdmissionEpoch == r.sourceAdmissionEpoch &&
                observed.binding.pageEpoch == r.pageEpoch &&
                observed.binding.requestId == s.visibleRequest &&
                observed.binding.privateBirth == s.visibleBirth &&
                observed.binding.producerInstance == s.visibleInstance &&
                observed.binding == request.binding;
            auto armRecoveryRemove = [&](uint32_t slot, bool alreadyAbsent) {
                if (!exactToken || !observed.drained || !observed.rendererHandle ||
                    !observed.instanceEpoch || slot >= s.models.size() ||
                    request.binding.producerModel != s.models[slot] ||
                    (alreadyAbsent ? observed.rendererModel != UINT32_MAX :
                        observed.rendererModel == UINT32_MAX)) return false;
                s.visiblePlacement = request.expected;
                s.visibleSelectedSlot = slot; s.visibleRendererSlot = observed.rendererHandle;
                s.visibleRecoveryRemove = true;
                s.visibleObservation.reset(); // only a terminal receipt reaches this path
                r.retailVisiblePresent = false; r.retailVisibleRemoved = false;
                r.retailVisibleReturned = false;
                r.retailVisibleSlot = UINT32_MAX;
                r.retailVisibleRenderer = r.retailVisibleModel = UINT32_MAX;
                r.rendererInstance = UINT32_MAX;
                return true;
            };
            if (observed.state == InstanceRecord::State::Rejected)
            {
                // Camera acquisition or return can fail after an exact drain.
                // The frozen row is cleanup authority only, never visible-frame
                // acceptance. A drained Absent Remove gets a fresh idempotent
                // Remove receipt on the same historical handle (bounded below).
                const bool remove = request.action == InstanceRecord::Action::Remove;
                const uint32_t slot = remove ? s.visibleSelectedSlot : s.visiblePendingSlot;
                if (armRecoveryRemove(slot,remove))
                    return fail(remove ? "RetailVisibleAbsentNeedsReturnRetry" :
                        "RetailVisiblePresentNeedsRemove");
                return fail("RetailVisibleReceiptRejectedOrUndrained");
            }
            if (observed.state != InstanceRecord::State::Consumed ||
                !exactToken ||
                !observed.frameAttempt || observed.renderReturnStatus != 0 ||
                !observed.instanceEpoch || !observed.rendererHandle)
                return fail("RetailVisibleReceiptRejectedOrStale");
            const bool removing = s.visiblePhase == RetailPagePilotState::VisiblePhase::AwaitRemove;
            if (!removing)
            {
                const uint32_t slot = s.visiblePendingSlot;
                if (!s.SourceCurrent() || !cutPresent(slot) ||
                    observed.binding.producerModel != s.models[slot] ||
                    observed.rendererModel != s.receipts[slot]->Observe().rendererModel)
                {
                    if (armRecoveryRemove(slot,false))
                        return fail("RetailVisibleSourceOrModelStaleRemoveRequired");
                    return fail("RetailVisibleSourceOrModelStale");
                }
                s.visibleSelectedSlot = slot;
                s.visiblePlacement.model = s.models[slot];
                s.visiblePhase = slot == 0 ? RetailPagePilotState::VisiblePhase::Reference :
                    slot == 1 ? RetailPagePilotState::VisiblePhase::Root :
                    RetailPagePilotState::VisiblePhase::Fine;
                r.retailVisiblePresent = true; r.retailVisibleRemoved = false;
                r.retailVisiblePhase = slot == 0 ?
                    (s.visibleSourceFineReference ? "SourceFineReference" : "Reference") :
                    slot == 1 ? "Root" : "Fine";
                r.retailVisibleRenderer = observed.rendererModel;
                r.retailVisibleSlot = slot;
                r.retailVisibleModel = s.models[slot]; r.producerModel = s.models[slot];
                r.pageWorkState = "RetailVisibleMainReturned";
            }
            else
            {
                if (observed.rendererModel != UINT32_MAX ||
                    observed.rendererHandle != s.visibleRendererSlot)
                    return fail("RetailVisibleRemoveNotAbsent");
                s.visiblePhase = RetailPagePilotState::VisiblePhase::Removed;
                ReleaseRetailInstanceHandle(s.visibleInstance);
                r.retailVisiblePresent = false; r.retailVisibleRemoved = true;
                r.retailVisiblePhase = "Removed";
                r.retailVisibleRenderer = UINT32_MAX;
                r.retailVisibleSlot = UINT32_MAX;
                r.rendererInstance = UINT32_MAX;
                r.pageWorkState = "RetailVisibleRemovedAfterMainReturn";
                s.visibleRecoveryRemove = false;
            }
            s.visibleRendererSlot = observed.rendererHandle;
            s.visibleCameraGeneration = observed.camera.generation;
            r.retailVisibleCameraGeneration = observed.camera.generation;
            s.visiblePendingSlot = UINT32_MAX;
            s.visibleObservation.reset();
            r.retailVisibleFrame = observed.frameAttempt;
            r.retailVisibleReturned = true;
            if (!removing) r.rendererInstance = observed.rendererHandle;
            r.status = GeometryReportStatus::Ready;
            return result();
        }
        if (action == GeometryPageFixtureAction::RemoveRetailVisible)
        {
            const bool recovery = s.visibleRecoveryRemove &&
                s.visiblePhase == RetailPagePilotState::VisiblePhase::Failed;
            if (!r.retailVisiblePilot ||
                (!recovery && (!r.retailVisibleReturned || !r.retailVisiblePresent)) ||
                s.visibleObservation || s.visibleRendererSlot == 0 ||
                s.visibleRemoveAttempts >= 3 ||
                s.visibleSelectedSlot >= s.models.size() ||
                s.visiblePhase == RetailPagePilotState::VisiblePhase::Removed ||
                (s.visiblePhase == RetailPagePilotState::VisiblePhase::Failed && !recovery))
                return refuse("RetailVisibleRemoveUnavailable");
            _instanceOps.reserve(_instanceOps.size() + 1);
            const uint64_t ticket = nextRequest();
            if (!ticket) return refuse("RetailVisibleRequestExhausted");
            InstanceRecord::Request request;
            request.binding = {_geometryOwnerEpoch,r.sourceAdmissionEpoch,r.pageEpoch,ticket,
                s.visibleBirth,s.visibleInstance,s.models[s.visibleSelectedSlot]};
            request.action = InstanceRecord::Action::Remove;
            request.expectedState = InstanceRecord::Expected::Absent;
            request.expected = s.visiblePlacement;
            request.previousRenderer = s.visibleRendererSlot;
            request.previousCameraGeneration = s.visibleCameraGeneration;
            auto receipt = std::make_shared<InstanceRecord>(request);
            if (!receipt->Valid()) return refuse("RetailVisibleRemovePacketInvalid");
            EnqueueInstanceRemove(s.visibleInstance,receipt);
            s.visibleObservation = std::move(receipt); s.visibleRequest = ticket;
            ++s.visibleRemoveAttempts;
            s.visiblePendingSlot = UINT32_MAX;
            s.visiblePhase = RetailPagePilotState::VisiblePhase::AwaitRemove;
            r.retailVisibleRequest = ticket; r.retailVisibleReturned = false;
            r.retailVisiblePhase = "AwaitRemove";
            r.status = GeometryReportStatus::Pending;
            r.pageWorkState = "RetailVisibleRemoveQueued";
            return result();
        }
        uint32_t slot = UINT32_MAX;
        if (action == GeometryPageFixtureAction::RootRetailVisible) slot = 1;
        else if (action == GeometryPageFixtureAction::FineRetailVisible)
            slot = s.FineSlot();
        else if (action == GeometryPageFixtureAction::FallbackRetailVisible) slot = 0;
        else return refuse("RetailVisibleActionUnsupported");
        if (!r.retailVisiblePilot || !r.retailVisibleReturned || !r.retailVisiblePresent ||
            s.visibleObservation || s.visibleSwitches >= 8 || s.visibleSelectedSlot == slot ||
            s.visibleRendererSlot == 0 || !s.SourceCurrent() || !workerEmpty() ||
            !cutPresent(slot) ||
            (s.phase != RetailPagePilotState::Phase::FineReady &&
             s.phase != RetailPagePilotState::Phase::RootOnlyReady &&
             s.phase != RetailPagePilotState::Phase::Released))
            return refuse("RetailVisibleCutUnavailable");
        WgrInstance placement = s.visiblePlacement;
        placement.model = s.models[slot];
        _instanceOps.reserve(_instanceOps.size() + 1);
        const uint64_t ticket = nextRequest();
        if (!ticket) return refuse("RetailVisibleRequestExhausted");
        InstanceRecord::Request request;
        request.binding = {_geometryOwnerEpoch,r.sourceAdmissionEpoch,r.pageEpoch,ticket,
            s.visibleBirth,s.visibleInstance,s.models[slot]};
        request.action = InstanceRecord::Action::Update;
        request.expectedState = InstanceRecord::Expected::Present;
        request.expected = placement;
        request.previousRenderer = s.visibleRendererSlot;
        request.previousCameraGeneration = s.visibleCameraGeneration;
        auto receipt = std::make_shared<InstanceRecord>(request);
        if (!receipt->Valid()) return refuse("RetailVisibleUpdatePacketInvalid");
        EnqueueInstanceUpdate(s.visibleInstance,placement,receipt);
        s.visibleObservation = std::move(receipt);
        s.visiblePendingSlot = slot; s.visibleRequest = ticket; ++s.visibleSwitches;
        s.visiblePhase = RetailPagePilotState::VisiblePhase::AwaitSwitch;
        r.retailVisibleRequest = ticket; r.retailVisibleReturned = false;
        r.retailVisiblePhase = "AwaitSwitch";
        r.status = GeometryReportStatus::Pending;
        r.pageWorkState = "RetailVisibleCutUpdateQueued";
        return result();
    }
    catch (...) { return fail("RetailVisibleOwnerException"); }
}
Engine::GeometryPageFixtureReport EngineWgpu::StepRetailWorldVisible(GeometryPageFixtureAction action)
{
    using Pair = render::RetailWorldInstanceTransaction;
    using Cleanup = render::RetailPageInstanceObservation;
    GeometryPageFixtureReport unavailable;
    unavailable.status = GeometryReportStatus::Invalid;
    unavailable.pageWorkState = "RetailWorldVisibleUnavailable";
    auto enabled = [](const char* name) {
        const char* value = std::getenv(name);
        return value && value[0] == '1' && !value[1];
    };
    if (!Foundation::IsMainThread() || !_renderer || !_gpuDriven ||
        !enabled("WGR_GEOMETRY_PAGE_RETAIL_WORLD_VISIBLE") ||
        !enabled("WGR_GEOMETRY_PAGE_RETAIL_COLD_SOURCE") ||
        !enabled("WGR_GEOMETRY_PAGE_RETAIL_SOURCE") ||
        !enabled("WGR_OBJECT_STREAM_DAYZ_REUSE_ONLY") ||
        enabled("WGR_GEOMETRY_PAGE_RETAIL_VISIBLE"))
        return unavailable;
    const bool worldAuto = enabled("WGR_GEOMETRY_PAGE_RETAIL_WORLD_AUTO");
    const bool worldResidency = worldAuto && enabled("WGR_GEOMETRY_PAGE_RETAIL_WORLD_RESIDENCY");
    auto pilot = _retailPagePilot;
    if (!pilot) return unavailable;
    auto& p = *pilot;
    auto& report = p.report;
    auto current = [&]() -> GeometryPageFixtureReport {
        p.RefreshWorkerReport();
        report.retailWorldResidencyCycleLimit=p.reusableWorldFine ? RetailWorldResidencyCycleLimit : 1;
        report.retailWorldResidencyReservedBytes=p.residencyReservation;
        report.retailWorldLastRetiredMeshes=p.lastRetiredMeshes;
        report.retailWorldCompactedMeshes=p.compactedMeshes;
        report.retailWorldRetiredFineModels=p.retiredFineModels;
        report.retailWorldRetirementAckRequest=p.retirementAckRequest;
        if (_retailWorldVisible)
        {
            const auto& s = *_retailWorldVisible;
            constexpr const char* names[] = {"Disabled","Both","RetiringFine","RootOnly",
                "RestoringForRefill","Refilling","RetakingFine","Complete","Cancelled"};
            report.retailWorldResidencyPhase = names[uint32_t(s.residence)];
            report.retailWorldResidencyEnabled = s.residencyEnabled;
            report.retailWorldResidencyCycles = s.residencyCycles;
            report.retailWorldResidencyTriggerCamera = s.residencyTriggerCamera;
            report.retailWorldRetirementRequest = s.retirementRequest;
            report.retailWorldRefillRequest = s.refillRequest;
            report.retailWorldFallbackCameraGeneration = s.fallbackCamera;
            report.retailWorldFallbackRequest = s.fallbackRequest;
            report.retailWorldFallbackPageBirth = s.fallbackPageBirth;
            report.retailWorldFallbackInstanceEpoch = s.fallbackEpoch;
            report.retailWorldFallbackOriginalFlags = s.fallbackOriginalFlags;
            report.retailWorldFallbackPageAbsent = s.fallbackPageAbsent;
            report.retailWorldRefillRequestAtFallback = s.refillAtFallback;
            report.retailWorldConventionalFallback =
                s.phase == RetailWorldVisibleState::Phase::Restored && report.retailWorldRestored;
        }
        return report;
    };
    auto refuse = [&](const char* why) {
        auto copy = current(); copy.status = GeometryReportStatus::Invalid;
        copy.pageWorkState = why; return copy;
    };
    auto fail = [&](const char* why) {
        report.status = GeometryReportStatus::Failed;
        report.pageWorkState = why;
        report.retailWorldPhase = why;
        return current();
    };
    auto serial = [](uint64_t v) { return v && v != UINT64_MAX; };
    auto nextRequest = [&]() {
        if (!serial(_nextRetailWorldRequest) ||
            _nextRetailWorldRequest >= UINT64_MAX - 1) return uint64_t{0};
        return _nextRetailWorldRequest++;
    };
    auto model = [&](uint32_t producer) -> Pair::ModelIdentity {
        if (producer == UINT32_MAX) return {};
        const auto found = _modelIdOf.find(producer);
        if (found == _modelIdOf.end() || found->second == WGR_INVALID_MODEL ||
            found->second == UINT32_MAX) return {};
        return {producer, found->second, uint64_t(producer) + 1};
    };
    auto ready = [&](uint32_t slot) {
        return slot < p.models.size() && p.models[slot] != UINT32_MAX &&
            p.modelReady[slot] && p.receipts[slot] &&
            p.receipts[slot]->ProducerModel() == p.models[slot] &&
            p.receipts[slot]->OwnerEpoch() == _geometryOwnerEpoch &&
            p.receipts[slot]->SourceAdmissionEpoch() == report.sourceAdmissionEpoch &&
            p.receipts[slot]->Observe().state ==
                render::ModelAdmissionReceipt::State::ConsumedAccepted;
    };
    auto cut = [&](uint32_t slot) {
        if (slot != 1 && slot != 2 && slot != 3) return false;
        if (!ready(slot)) return false;
        const auto& clusters = slot == 1 ? p.rootClusters : p.fineClusters;
        if (clusters.empty() || clusters.size() > render::ModelAdmissionReceipt::MaxMeshes)
            return false;
        for (auto cluster : clusters)
        {
            if (cluster >= p.clusterMesh.size() || !p.clusterMesh[cluster]) return false;
            const auto* row = p.Find(p.clusterMesh[cluster]);
            if (!row || !row->present || !row->renderer || row->page >= p.resident.size() ||
                !p.resident[row->page]) return false;
        }
        return true;
    };
    auto sourceCurrent = [&](const RetailWorldVisibleState& s) {
        const auto* source = _retailWorldSource.get();
        return source && !source->worldInvalidated && source->worldObject &&
            source->worldObjectBirth == s.objectBirth &&
            source->worldProducerInstance == s.originalProducer &&
            source->shape.GetRef() == s.normalShape.GetRef() &&
            source->shapeBirth == s.shapeBirth &&
            p.admission.actual &&
            p.admission.actual->shapeQueryRevision == s.shapeQueryRevision &&
            report.sourceAdmissionEpoch == s.sourceEpoch &&
            report.pageEpoch == s.pageEpoch &&
            _geometryOwnerEpoch == s.ownerEpoch && p.SourceCurrent();
    };
    auto row = [](Pair::ModelIdentity identity, WgrInstance value) {
        Pair::RowExpectation result; result.model = identity;
        value.model = identity.producerModel; result.row = value; return result;
    };
    auto queue = [&](RetailWorldVisibleState& s, Pair::Request request) {
        auto receipt = std::make_shared<Pair>(request);
        if (!receipt->Valid()) return false;
        AcquireProducerWindow("retail world pair preflight");
        const auto before = GetRetailWorldBefore(request, 1);
        if (receipt->CheckBefore(before) != Pair::Refusal::None) return false;
        _instanceOps.reserve(_instanceOps.size() + 1);
        if (_retailWorldStableFrame)
        {
            _retailWorldStableFrame->Invalidate();
            _retailWorldStableFrame.reset();
        }
        InstanceOp operation{};
        operation.kind = InstanceOp::WorldPair;
        operation.handle = s.originalProducer;
        operation.worldPair = receipt;
        _instanceOps.push_back(std::move(operation));
        s.pair = std::move(receipt);
        s.requestId = request.binding.requestId;
        report.retailWorldRequest = s.requestId;
        report.retailWorldReturned = false;
        report.status = GeometryReportStatus::Pending;
        return true;
    };
    auto binding = [&](const RetailWorldVisibleState& s, uint64_t ticket) {
        Pair::Binding b;
        b.ownerEpoch = s.ownerEpoch; b.worldObjectBirth = s.objectBirth;
        b.sourceAdmissionEpoch = s.sourceEpoch; b.shapeBirth = s.shapeBirth;
        b.shapeQueryRevision = s.shapeQueryRevision; b.pageEpoch = s.pageEpoch;
        b.requestId = ticket; b.originalProducerInstance = s.originalProducer;
        b.pageProducerInstance = s.pageProducer; b.originalInstanceBirth = s.objectBirth;
        b.pageInstanceBirth = s.pageBirth;
        return b;
    };
    auto armStable = [&](RetailWorldVisibleState& s) {
        using Stable = render::RetailWorldStableFrameObservation;
        if (!s.autoEnabled || s.pair || s.cleanup || s.invalidated ||
            !serial(s.cameraGeneration) || !s.pageRenderer || !sourceCurrent(s)) return false;
        Stable::Request request;
        request.binding = binding(s,s.requestId);
        auto hidden = s.original; hidden.flags = (hidden.flags & WGR_INSTANCE_SURFACE_RECEIVERS) | WGR_INSTANCE_OTHER_VIEWS_ONLY;
        request.original = row(s.originalModel,hidden);
        request.page = row(s.pageModel,s.page);
        request.originalRendererHandle = s.originalRenderer;
        request.pageRendererHandle = s.pageRenderer;
        request.previousCameraGeneration = s.cameraGeneration;
        auto tracker = std::make_shared<Stable>(request);
        if (!tracker->Valid()) return false;
        AcquireProducerWindow("retail world stable frame arm");
        _retailWorldStableFrame = std::move(tracker);
        return true;
    };
    auto finishAbsent = [&](RetailWorldVisibleState& s, bool restored) {
        if (_retailWorldStableFrame)
        {
            _retailWorldStableFrame->Invalidate();
            _retailWorldStableFrame.reset();
        }
        ReleaseRetailInstanceHandle(s.pageProducer);
        if (s.modelRetained)
        {
            ReleaseGpuModel(s.normalShape.GetRef());
            s.modelRetained = false;
        }
        s.phase = restored ? RetailWorldVisibleState::Phase::Restored :
            RetailWorldVisibleState::Phase::Removed;
        if (restored && s.residence == RetailWorldVisibleState::Residence::RestoringForRefill)
            s.fallbackCamera = s.cameraGeneration; // exact successful joined Restore receipt, not a desired pose
        s.autoEnabled = false; s.autoPolicy.reset();
        report.retailWorldAutoEnabled = false;
        s.pair.reset(); s.cleanup.reset();
        report.retailWorldPagePresent = false;
        report.retailWorldSelectedTriangles = 0;
        report.hierarchySelectedTriangles = 0;
        // A normal Restore certifies flags0. Invalidation cleanup instead
        // established flags0, historical absence, or a visible replacement;
        // this flag no longer asserts that our original is the other-views row.
        report.retailWorldOriginalOtherViews = false;
        report.retailWorldRestored = restored;
        report.retailWorldPhase = restored ? "Restored" : "InvalidatedPageAbsent";
        report.retailWorldReturned = true;
        report.status = restored ? GeometryReportStatus::Ready : GeometryReportStatus::Failed;
        report.pageWorkState = restored ? "RetailWorldRestoreReturned" :
            "RetailWorldInvalidatedPageRemoved";
    };
    // A stale actor/source never receives an owner-side old-placement write.
    // The consumer may restore only its exact still-current flags64 original;
    // cleanup then addresses only the quarantined page handle and birth.
    auto cleanup = [&](RetailWorldVisibleState& s) {
        if (s.cleanup || s.cleanupAttempts >= 3 || !s.pageRenderer ||
            !serial(s.pageBirth) || !ready(s.selectedSlot)) return false;
        AcquireProducerWindow("retail world stale page cleanup");
        if (_retailWorldStableFrame)
        {
            _retailWorldStableFrame->Invalidate();
            _retailWorldStableFrame.reset();
        }
        // A mount change alone does not remove or move the original. Ask the
        // consumer to roll back its exact active pair first. Its rollback may
        // restore only the same current flags64 row; it will not write an old
        // placement over a moved/replaced original. Never remove our page while
        // the original might still be hidden from the main camera.
        InvalidateRetailWorldConsumer(s.originalProducer);
        const auto original = GetRetailWorldInstanceEvidence(
            s.originalProducer, s.originalModel, s.originalRenderer);
        bool originalSafe = false;
        if (original.producerState == Pair::ProducerState::Present &&
            original.getterSucceeded &&
            original.cpu.status == WGR_INSTANCE_CPU_FACT_PRESENT)
        {
            if (original.consumerInstanceBirth == s.objectBirth &&
                original.mappedRendererHandle == s.originalRenderer &&
                original.mappedRendererModel == s.originalModel.rendererModel)
            {
                WgrInstance baseline = s.original;
                baseline.model = s.originalModel.rendererModel;
                originalSafe = !std::memcmp(&baseline,&original.cpu.row,sizeof(baseline));
            }
            else if (original.consumerInstanceBirth != s.objectBirth &&
                (original.cpu.row.flags & ~WGR_INSTANCE_SURFACE_RECEIVERS) == 0)
                originalSafe = true; // ordinary moved/replaced row; never rewrite it
        }
        else if (original.producerState == Pair::ProducerState::Unmapped &&
            !original.consumerInstanceBirth && original.getterSucceeded &&
            original.cpu.status == WGR_INSTANCE_CPU_FACT_ABSENT &&
            original.cpu.resolved_slot == UINT32_MAX)
        {
            const WgrInstance zero{};
            originalSafe = !std::memcmp(&zero,&original.cpu.row,sizeof(zero));
        }
        if (!originalSafe) return false;
        const auto observed = GetRetailWorldInstanceEvidence(
            s.pageProducer, s.pageModel, s.pageRenderer);
        if (observed.producerInstance != s.pageProducer || !observed.getterSucceeded ||
            observed.cpu.queried_handle != s.pageRenderer) return false;
        if (observed.producerState == Pair::ProducerState::Present)
        {
            if (observed.mappedRendererHandle != s.pageRenderer ||
                observed.consumerInstanceBirth != s.pageBirth ||
                observed.mappedRendererModel != s.pageModel.rendererModel ||
                observed.cpu.status != WGR_INSTANCE_CPU_FACT_PRESENT) return false;
            WgrInstance expected = s.page;
            expected.model = s.pageModel.rendererModel;
            if (std::memcmp(&expected, &observed.cpu.row, sizeof(expected))) return false;
        }
        else if (observed.producerState != Pair::ProducerState::Unmapped ||
            observed.consumerInstanceBirth ||
            observed.cpu.status != WGR_INSTANCE_CPU_FACT_ABSENT ||
            observed.cpu.resolved_slot != UINT32_MAX) return false;
        if (observed.cpu.status == WGR_INSTANCE_CPU_FACT_ABSENT)
        {
            const WgrInstance zero{};
            if (std::memcmp(&zero,&observed.cpu.row,sizeof(zero))) return false;
        }
        const uint64_t ticket = nextRequest();
        if (!ticket) return false;
        Cleanup::Request request;
        request.binding = {s.ownerEpoch,s.sourceEpoch,s.pageEpoch,ticket,s.pageBirth,
            s.pageProducer,s.pageModel.producerModel};
        request.action = Cleanup::Action::Remove;
        request.expectedState = Cleanup::Expected::Absent;
        request.expected = s.page;
        request.previousRenderer = s.pageRenderer;
        request.previousCameraGeneration = s.cameraGeneration;
        auto receipt = std::make_shared<Cleanup>(request);
        if (!receipt->Valid()) return false;
        _instanceOps.reserve(_instanceOps.size() + 1);
        EnqueueInstanceRemove(s.pageProducer, receipt);
        s.cleanup = std::move(receipt);
        s.phase = RetailWorldVisibleState::Phase::AwaitCleanup;
        ++s.cleanupAttempts;
        report.retailWorldRequest = ticket;
        report.retailWorldPhase = "AwaitInvalidationRemove";
        report.retailWorldReturned = false;
        report.status = GeometryReportStatus::Pending;
        report.pageWorkState = "RetailWorldInvalidationRemoveQueued";
        return true;
    };
    auto cancelResidency = [&](RetailWorldVisibleState& s) {
        s.residencyEnabled = false;
        s.residence = RetailWorldVisibleState::Residence::Cancelled;
        s.autoEnabled = false; s.autoPolicy.reset();
        report.retailWorldAutoEnabled = false;
        AcquireProducerWindow("retail world residency cancellation");
        if (_retailWorldStableFrame)
        {
            _retailWorldStableFrame->Invalidate();
            _retailWorldStableFrame.reset();
        }
        // Cancellation does not forgive the actual worker's active/reservation
        // debt, erase staged history, or release the trusted owned GHP file.
        if (p.worker) p.worker->Cancel(report.pageEpoch);
    };
    auto retakeFine = [&](RetailWorldVisibleState& s) -> bool {
        if (!s.residencyEnabled || s.residence != RetailWorldVisibleState::Residence::Refilling ||
            s.residencyCycles >= (p.reusableWorldFine ? RetailWorldResidencyCycleLimit : 1) ||
            s.phase != RetailWorldVisibleState::Phase::Restored ||
            s.pair || s.cleanup || s.invalidated || s.modelRetained || !report.retailWorldRestored ||
            !report.retailWorldReturned || report.retailWorldPagePresent || !p.refilled ||
            p.phase != RetailPagePilotState::Phase::FineReady || p.observation || !cut(p.FineSlot()) ||
            !sourceCurrent(s) || !p.worker || !serial(_nextRetailWorldPageBirth) ||
            _nextRetailWorldPageBirth >= UINT64_MAX - 1) return false;
        const auto debt = p.worker->Snapshot();
        if (debt.liveJobs || debt.queued || debt.active || debt.ready || debt.reservedBytes) return false;
        AcquireProducerWindow("retail world refill fine takeover preflight");
        if (!sourceCurrent(s) || GetRetailWorldConsumerBirth(s.originalProducer) != s.objectBirth)
            return false;
        const auto normal = _gpuModels.find(s.normalShape.GetRef());
        if (normal == _gpuModels.end() || normal->second != s.originalModel.producerModel ||
            model(normal->second) != s.originalModel) return false;
        const auto refs = _gpuModelRefs.find(s.normalShape.GetRef());
        if (refs != _gpuModelRefs.end() && refs->second == UINT32_MAX) return false;
        const auto literal = GetRetailWorldInstanceEvidence(s.originalProducer,s.originalModel,s.originalRenderer);
        WgrInstance expected = s.original; expected.model = s.originalModel.rendererModel;
        if (!literal.getterSucceeded || literal.producerState != Pair::ProducerState::Present ||
            literal.consumerInstanceBirth != s.objectBirth || literal.consumerModelBirth != s.originalModel.modelBirth ||
            literal.mappedRendererHandle != s.originalRenderer || literal.mappedRendererModel != s.originalModel.rendererModel ||
            literal.cpu.status != WGR_INSTANCE_CPU_FACT_PRESENT ||
            std::memcmp(&expected,&literal.cpu.row,sizeof(expected))) return false;
        // Reuse the already charged bounded owner state. The old page slot was
        // released ONLY by the exact returned Restore absence receipt.
        RetailWorldVisibleState next = s;
        if (_nextInstanceHandle == UINT32_MAX) return false;
        next.pageProducer = _nextInstanceHandle++; // new finite-pilot producer, not the released old free-list cell
        next.pageBirth = _nextRetailWorldPageBirth; next.pageRenderer = 0;
        const uint32_t fineSlot=p.FineSlot();
        next.pageModel = model(p.models[fineSlot]);
        next.page = s.original; next.page.model = p.models[fineSlot];
        next.page.flags = (next.page.flags & WGR_INSTANCE_SURFACE_RECEIVERS) | WGR_INSTANCE_MAIN_CAMERA_ONLY;
        next.pendingSlot = fineSlot; next.cleanupAttempts = 0;
        next.residence = RetailWorldVisibleState::Residence::RetakingFine;
        const auto ticket = nextRequest();
        if (!ticket || next.pageModel.producerModel == UINT32_MAX)
        { ReleaseRetailInstanceHandle(next.pageProducer); return false; }
        Pair::Request request; request.binding = binding(next,ticket);
        request.action = Pair::Action::Takeover;
        request.originalBefore = row(next.originalModel,next.original);
        auto hidden = next.original; hidden.flags = (hidden.flags & WGR_INSTANCE_SURFACE_RECEIVERS) | WGR_INSTANCE_OTHER_VIEWS_ONLY;
        request.originalAfter = row(next.originalModel,hidden);
        request.pageAfter = row(next.pageModel,next.page);
        request.originalRendererHandle = next.originalRenderer;
        request.previousCameraGeneration = next.cameraGeneration;
        bool queued = false;
        try
        {
            RetainGpuModel(next.normalShape.GetRef()); next.modelRetained = true;
            queued = queue(next,request);
        }
        catch (...)
        {
            if (next.modelRetained) ReleaseGpuModel(next.normalShape.GetRef());
            ReleaseRetailInstanceHandle(next.pageProducer); throw;
        }
        if (!queued)
        {
            ReleaseGpuModel(next.normalShape.GetRef());
            ReleaseRetailInstanceHandle(next.pageProducer); return false;
        }
        ++_nextRetailWorldPageBirth;
        next.phase = RetailWorldVisibleState::Phase::AwaitTakeover;
        s = std::move(next);
        report.retailWorldRestored = false; report.retailWorldPageBirth = s.pageBirth;
        report.retailWorldPageInstance = s.pageProducer; report.retailWorldPageModel = s.pageModel.producerModel;
        report.retailWorldPageRenderer = UINT32_MAX;
        report.retailWorldPhase = "AwaitRefilledFineTakeover";
        report.pageWorkState = "RetailWorldRefilledFineTakeoverQueued";
        return true;
    };
    try
    {
        if (action == GeometryPageFixtureAction::BeginRetailWorldVisible)
        {
            const auto* source = _retailWorldSource.get();
            if (_retailWorldVisible || !source || source->worldInvalidated ||
                !source->worldObject || !serial(source->worldObjectBirth) ||
                source->worldProducerInstance == UINT32_MAX ||
                p.visiblePhase != RetailPagePilotState::VisiblePhase::Dormant ||
                !report.active || p.phase != RetailPagePilotState::Phase::FineReady ||
                p.observation || !p.SourceCurrent() || !cut(2) ||
                !p.admission.actual || !serial(_nextRetailWorldPageBirth) ||
                _nextRetailWorldPageBirth >= UINT64_MAX - 1)
                return refuse("RetailWorldBeginUnavailable");
            const auto surface = p.admission.certifiedSurface;
            if (worldAuto && (!surface || !surface->certificate || !surface->root ||
                !surface->fine || !report.retailWorldSurfaceCertified ||
                !(surface->certificate->identity == p.Package().identity) ||
                surface->certificate->sourceAdmissionEpoch != report.sourceAdmissionEpoch ||
                surface->certificate->ownerEpoch != _geometryOwnerEpoch ||
                surface->certificate->modelBirth != source->shapeBirth ||
                surface->certificate->shapeQueryRevision !=
                    p.admission.actual->shapeQueryRevision ||
                surface->root->clusters != p.rootClusters ||
                surface->fine->clusters != p.fineClusters ||
                surface->certificate->fileSha256 != p.admission.product->fileSha256))
                return refuse("RetailWorldSurfaceAuthorityUnavailable");
            constexpr uint64_t reservation = sizeof(RetailWorldVisibleState) +
                12 * (sizeof(Pair) + sizeof(InstanceOp) + 128) +
                3 * (sizeof(Cleanup) + sizeof(InstanceOp) + 128);
            const uint64_t autoReservation = worldAuto ?
                sizeof(render::RetailWorldStableFrameObservation) + 128 : 0;
            constexpr uint64_t abortReserve = (Record::MaxMeshes + Record::MaxModels + 1) *
                sizeof(ResourceOp) + sizeof(Record) + 256;
            constexpr uint64_t workingLimit = RetailLimit - abortReserve;
            if (report.knownPayloadBytes > workingLimit ||
                reservation + autoReservation > workingLimit - report.knownPayloadBytes)
                return refuse("RetailWorldInstanceCapacity");
            AcquireProducerWindow("retail world original model and row");
            const auto normal = _gpuModels.find(source->shape.GetRef());
            if (normal == _gpuModels.end() || normal->second != source->worldOriginal.model ||
                normal->second == UINT32_MAX || model(normal->second).producerModel == UINT32_MAX)
                return refuse("RetailWorldOriginalModelUnavailable");
            const auto refs = _gpuModelRefs.find(source->shape.GetRef());
            if (refs != _gpuModelRefs.end() && refs->second == UINT32_MAX)
                return refuse("RetailWorldModelLeaseOverflow");
            const auto originalModel = model(normal->second);
            const auto original = GetRetailWorldInstanceEvidence(
                source->worldProducerInstance, originalModel, 0);
            if (original.producerState != Pair::ProducerState::Present ||
                original.consumerInstanceBirth != source->worldObjectBirth ||
                !original.getterSucceeded ||
                original.cpu.status != WGR_INSTANCE_CPU_FACT_PRESENT ||
                !original.mappedRendererHandle ||
                original.mappedRendererModel != originalModel.rendererModel ||
                original.cpu.queried_handle != original.mappedRendererHandle)
                return refuse("RetailWorldOriginalBirthOrRowUnavailable");
            WgrInstance expected = source->worldOriginal;
            expected.model = originalModel.rendererModel;
            if ((expected.flags & ~WGR_INSTANCE_SURFACE_RECEIVERS) || std::memcmp(&expected, &original.cpu.row, sizeof(expected)))
                return refuse("RetailWorldOriginalMovedOrChanged");
            auto state = std::make_shared<RetailWorldVisibleState>();
            state->normalShape = source->shape;
            state->ownerEpoch = _geometryOwnerEpoch;
            state->objectBirth = source->worldObjectBirth;
            state->sourceEpoch = report.sourceAdmissionEpoch;
            state->shapeBirth = source->shapeBirth;
            state->shapeQueryRevision = p.admission.actual->shapeQueryRevision;
            state->pageEpoch = report.pageEpoch;
            state->pageBirth = _nextRetailWorldPageBirth;
            state->originalProducer = source->worldProducerInstance;
            state->originalRenderer = original.mappedRendererHandle;
            state->originalModel = originalModel;
            state->original = source->worldOriginal;
            state->pageModel = model(p.models[2]);
            state->page = source->worldOriginal;
            state->page.model = p.models[2];
            state->page.flags = (state->page.flags & WGR_INSTANCE_SURFACE_RECEIVERS) | WGR_INSTANCE_MAIN_CAMERA_ONLY;
            state->pendingSlot = 2;
            state->surface = surface;
            if (!sourceCurrent(*state)) return refuse("RetailWorldSourceChangedBeforeBegin");
            const uint32_t pageProducer = ReserveRetailInstanceHandle();
            if (pageProducer == UINT32_MAX) return refuse("RetailWorldPageSlotExhausted");
            state->pageProducer = pageProducer;
            const uint64_t ticket = nextRequest();
            if (!ticket) { ReleaseRetailInstanceHandle(pageProducer);
                return refuse("RetailWorldRequestExhausted"); }
            Pair::Request request;
            request.binding = binding(*state, ticket);
            request.action = Pair::Action::Takeover;
            request.originalBefore = row(state->originalModel,state->original);
            auto hidden = state->original; hidden.flags = (hidden.flags & WGR_INSTANCE_SURFACE_RECEIVERS) | WGR_INSTANCE_OTHER_VIEWS_ONLY;
            request.originalAfter = row(state->originalModel,hidden);
            request.pageAfter = row(state->pageModel,state->page);
            request.originalRendererHandle = state->originalRenderer;
            // The retained model lease prevents normal registration retirement
            // while its original row is hidden in the main camera.
            bool queued = false;
            try
            {
                RetainGpuModel(state->normalShape.GetRef());
                state->modelRetained = true;
                queued = queue(*state, request);
            }
            catch (...)
            {
                if (state->modelRetained) ReleaseGpuModel(state->normalShape.GetRef());
                ReleaseRetailInstanceHandle(pageProducer);
                throw;
            }
            if (!queued)
            {
                ReleaseGpuModel(state->normalShape.GetRef());
                ReleaseRetailInstanceHandle(pageProducer);
                return refuse("RetailWorldTakeoverPreflightRefused");
            }
            ++_nextRetailWorldPageBirth;
            _retailWorldVisible = std::move(state);
            report.knownPayloadBytes += reservation + autoReservation;
            report.retailWorldVisiblePilot = true;
            report.retailRecordOnly = false;
            report.retailWorldOriginalInstance = source->worldProducerInstance;
            report.retailWorldPageInstance = pageProducer;
            report.retailWorldPageBirth = _retailWorldVisible->pageBirth;
            report.retailWorldPageModel = p.models[2];
            report.retailWorldPhase = "AwaitTakeover";
            report.pageWorkState = "RetailWorldFineTakeoverQueued";
            return current();
        }
        auto state = _retailWorldVisible;
        if (!state) return refuse("RetailWorldNotBegun");
        auto& s = *state;
        if (action == GeometryPageFixtureAction::FollowRetailWorldVisible)
        {
            if (!worldAuto || !s.surface || !s.surface->certificate ||
                !s.surface->root || !s.surface->fine || s.autoEnabled ||
                s.pair || s.cleanup || s.invalidated ||
                (s.phase != RetailWorldVisibleState::Phase::Fine &&
                 s.phase != RetailWorldVisibleState::Phase::Root) ||
                p.phase != RetailPagePilotState::Phase::FineReady ||
                !ready(1) || !ready(2) || !cut(1) || !cut(2) ||
                !sourceCurrent(s) || !report.retailWorldReturned)
                return refuse("RetailWorldAutoFollowUnavailable");
            GeometryPages::HierarchicalBinaryDemand::Binding policyBinding;
            policyBinding.identity = s.surface->certificate->identity;
            policyBinding.privateOwnerEpoch = s.ownerEpoch;
            policyBinding.sourceAdmissionEpoch = s.sourceEpoch;
            policyBinding.pageEpoch = s.pageEpoch;
            policyBinding.modelBirth = s.shapeBirth;
            if (!policyBinding.Valid()) return refuse("RetailWorldAutoBindingInvalid");
            s.autoPolicy.emplace(policyBinding);
            s.autoEnabled = true;
            s.autoObservationOffset = s.autoRefineOffset = s.autoCoarsenOffset = 0;
            if (worldResidency && s.residence == RetailWorldVisibleState::Residence::Disabled &&
                !p.refilled && !p.nonrootPages.empty())
            {
                s.residencyEnabled = true;
                s.residence = RetailWorldVisibleState::Residence::Both;
            }
            s.lastAutoObservation = {};
            s.lastAutoFrameGeneration = 0;
            if (!armStable(s))
            {
                s.autoEnabled = false; s.autoPolicy.reset();
                return refuse("RetailWorldAutoStableFrameUnavailable");
            }
            report.retailWorldAutoEnabled = true;
            report.retailWorldAutoObservations = 0;
            report.retailWorldAutoRefines = 0;
            report.retailWorldAutoCoarsens = 0;
            report.retailWorldSafeRootObservations = 0;
            report.pageWorkState = "RetailWorldAutoFollowing";
            return current();
        }
        if (action == GeometryPageFixtureAction::StopRetailWorldVisible)
        {
            if (!s.autoEnabled && !s.residencyEnabled) return refuse("RetailWorldAutoNotFollowing");
            AcquireProducerWindow("retail world stable frame stop");
            if (s.residencyEnabled)
            {
                cancelResidency(s);
                if (!s.pair && !s.cleanup && !s.invalidated &&
                    (s.phase == RetailWorldVisibleState::Phase::Root ||
                     s.phase == RetailWorldVisibleState::Phase::Fine))
                    return StepRetailWorldVisible(GeometryPageFixtureAction::RestoreRetailWorldVisible);
                report.pageWorkState = "RetailWorldResidencyStoppedPendingCleanup";
                return current();
            }
            s.autoEnabled = false;
            s.autoPolicy.reset();
            if (_retailWorldStableFrame)
            {
                _retailWorldStableFrame->Invalidate();
                _retailWorldStableFrame.reset();
            }
            report.retailWorldAutoEnabled = false;
            report.retailWorldSafeRootObservations = 0;
            report.retailWorldProjectedBounded = false;
            report.retailWorldForcedFine = false;
            report.pageWorkState = "RetailWorldAutoStopped";
            return current();
        }
        if (action == GeometryPageFixtureAction::PollRetailWorldVisible)
        {
            if (s.residence == RetailWorldVisibleState::Residence::Cancelled &&
                p.phase == RetailPagePilotState::Phase::Aborted &&
                (s.phase == RetailWorldVisibleState::Phase::Restored ||
                 s.phase == RetailWorldVisibleState::Phase::Removed)) return current();
            if ((s.phase == RetailWorldVisibleState::Phase::Restored ||
                 s.phase == RetailWorldVisibleState::Phase::Removed) &&
                !s.residencyEnabled && s.residence != RetailWorldVisibleState::Residence::Cancelled)
                return current();
            if (!sourceCurrent(s)) s.invalidated = true;
            AcquireProducerWindow("retail world selected birth poll");
            if (GetRetailWorldConsumerBirth(s.originalProducer) != s.objectBirth)
                s.invalidated = true;
            if (s.invalidated && _retailWorldStableFrame)
            {
                _retailWorldStableFrame->Invalidate();
                _retailWorldStableFrame.reset();
            }
            if (s.invalidated)
            {
                if (s.residencyEnabled) cancelResidency(s);
                s.autoEnabled = false; s.autoPolicy.reset();
                report.retailWorldAutoEnabled = false;
            }
            if (s.cleanup)
            {
                const auto result = s.cleanup->Observe();
                if (result.state == Cleanup::State::Requested) return current();
                if (result.state == Cleanup::State::Consumed &&
                    result.binding == s.cleanup->GetRequest().binding &&
                    result.binding.privateBirth == s.pageBirth &&
                    result.binding.producerInstance == s.pageProducer &&
                    result.rendererHandle == s.pageRenderer &&
                    result.rendererModel == UINT32_MAX && result.renderReturnStatus == 0 &&
                    result.camera.generation > s.cameraGeneration &&
                    result.camera.generation != UINT64_MAX)
                {
                    s.cameraGeneration = result.camera.generation;
                    report.retailWorldCameraGeneration = s.cameraGeneration;
                    SetRetailWorldConsumerBirth(s.pageProducer,0);
                    finishAbsent(s,false);
                    return current();
                }
                s.cleanup.reset();
                return fail("RetailWorldInvalidationRemoveUnconfirmed");
            }
            if (s.pair)
            {
                const auto snapshot = s.pair->Observe();
                if (snapshot.state == Pair::State::Requested) return current();
                const auto request = s.pair->GetRequest();
                if (snapshot.state != Pair::State::Consumed ||
                    snapshot.binding != request.binding ||
                    snapshot.binding.requestId != s.requestId ||
                    snapshot.renderReturnStatus != 0 ||
                    !snapshot.drained || !snapshot.camera.generation)
                {
                    s.phase = RetailWorldVisibleState::Phase::Failed;
                    if (!s.pageRenderer && snapshot.drained &&
                        snapshot.pageRendererHandle)
                        s.pageRenderer = snapshot.pageRendererHandle;
                    s.pair.reset();
                    if (request.action == Pair::Action::Takeover)
                    {
                        s.selectedSlot = s.pendingSlot;
                        s.pageModel = request.pageAfter.model;
                        s.page = request.pageAfter.row;
                        AcquireProducerWindow("retail world rejected takeover evidence");
                        const auto page = GetRetailWorldInstanceEvidence(
                            s.pageProducer,s.pageModel,0);
                        if (page.producerState == Pair::ProducerState::Present &&
                            page.consumerInstanceBirth == s.pageBirth)
                            s.pageRenderer = page.mappedRendererHandle;
                    }
                    cleanup(s); // unknown partial mutation keeps the slot and lease quarantined
                    return fail("RetailWorldPairRejectedOrUnreturned");
                }
                s.cameraGeneration = snapshot.camera.generation;
                s.pageRenderer = snapshot.pageRendererHandle;
                s.pair.reset();
                report.retailWorldCameraGeneration = s.cameraGeneration;
                report.retailWorldPageRenderer = s.pageRenderer;
                report.retailWorldReturned = true;
                if (request.action == Pair::Action::Restore)
                {
                    if (s.residence == RetailWorldVisibleState::Residence::RestoringForRefill)
                    {
                        // The immutable consumed Restore receipt has checked
                        // actual original0 and exact historical page ABSENT in
                        // this returned frame. Retain its proof across refill.
                        s.fallbackRequest = snapshot.binding.requestId;
                        s.fallbackPageBirth = snapshot.binding.pageInstanceBirth;
                        s.fallbackEpoch = snapshot.instanceEpoch;
                        s.fallbackOriginalFlags = request.originalAfter.row.flags;
                        s.fallbackPageAbsent = true;
                        s.refillAtFallback = s.refillRequest;
                    }
                    finishAbsent(s,true);
                    return current();
                }
                s.selectedSlot = s.pendingSlot;
                s.pageModel = request.pageAfter.model;
                s.page = request.pageAfter.row;
                s.phase = s.selectedSlot == 1 ? RetailWorldVisibleState::Phase::Root :
                    RetailWorldVisibleState::Phase::Fine;
                report.retailWorldPagePresent = true;
                report.retailWorldOriginalOtherViews = true;
                report.retailWorldPageModel = s.pageModel.producerModel;
                const auto& selected = s.selectedSlot == 1 ? p.rootClusters : p.fineClusters;
                uint64_t triangles = 0;
                for (auto cluster : selected)
                {
                    if (cluster >= p.clusterIndices.size() || p.clusterIndices[cluster] % 3)
                        return fail("RetailWorldSelectedTriangleRecordInvalid");
                    triangles += p.clusterIndices[cluster] / 3;
                }
                if (!triangles || triangles > UINT32_MAX ||
                    (s.surface && triangles != (s.selectedSlot == 1 ?
                        s.surface->certificate->rootTriangles :
                        s.surface->certificate->fineTriangles)))
                    return fail("RetailWorldSelectedTriangleMismatch");
                report.retailWorldSelectedTriangles = uint32_t(triangles);
                report.hierarchySelectedTriangles = uint32_t(triangles);
                report.retailWorldPhase = s.selectedSlot == 1 ? "Root" : "Fine";
                // The returned pair certifies literal rows, not continued source
                // authority. A source change while its queue was in flight must
                // never publish Ready or restore an old placement via a new pair.
                if (s.invalidated || !sourceCurrent(s))
                {
                    s.invalidated = true;
                    s.phase = RetailWorldVisibleState::Phase::Failed;
                    cancelResidency(s);
                    cleanup(s);
                    return fail("RetailWorldReturnedSourceInvalidated");
                }
                report.status = GeometryReportStatus::Ready;
                report.pageWorkState = "RetailWorldPairReturned";
                if (s.residence == RetailWorldVisibleState::Residence::RetakingFine)
                {
                    if (s.selectedSlot != p.FineSlot() || !s.residencyEnabled || !sourceCurrent(s) ||
                        s.residencyCycles >= (p.reusableWorldFine ? RetailWorldResidencyCycleLimit : 1))
                    {
                        cancelResidency(s);
                        s.phase = RetailWorldVisibleState::Phase::Failed;
                        cleanup(s);
                        return fail("RetailWorldRefillTakeoverAuthorityRefused");
                    }
                    ++s.residencyCycles;
                    s.residence = RetailWorldVisibleState::Residence::Complete;
                    s.autoObservationOffset = report.retailWorldAutoObservations;
                    s.autoRefineOffset = report.retailWorldAutoRefines;
                    s.autoCoarsenOffset = report.retailWorldAutoCoarsens;
                    s.autoPolicy.emplace(GeometryPages::HierarchicalBinaryDemand::Binding{
                        s.surface->certificate->identity,s.ownerEpoch,s.sourceEpoch,s.pageEpoch,s.shapeBirth});
                    s.autoEnabled = true;
                    s.lastAutoObservation = {}; s.lastAutoFrameGeneration = 0;
                    report.retailWorldAutoEnabled = true;
                    report.pageWorkState = "RetailWorldRefilledFineReturned";
                }
                if (s.autoEnabled && !s.invalidated && !armStable(s))
                    return fail("RetailWorldStableFrameArmRefused");
            }
            if (s.invalidated)
            {
                if (s.phase == RetailWorldVisibleState::Phase::Restored ||
                    s.phase == RetailWorldVisibleState::Phase::Removed)
                {
                    StepRetailPageRecordPilot(GeometryPageFixtureAction::AbortRetailRecords);
                    StepRetailPageRecordPilot(GeometryPageFixtureAction::PollRetailRecords);
                    return current();
                }
                if (!cleanup(s)) return fail("RetailWorldInvalidatedPageQuarantined");
                return current();
            }
            using Residence = RetailWorldVisibleState::Residence;
            if (s.residence == Residence::Cancelled)
            {
                if (s.phase == RetailWorldVisibleState::Phase::Restored ||
                    s.phase == RetailWorldVisibleState::Phase::Removed)
                {
                    StepRetailPageRecordPilot(GeometryPageFixtureAction::AbortRetailRecords);
                    StepRetailPageRecordPilot(GeometryPageFixtureAction::PollRetailRecords);
                    return current();
                }
                if (!s.pair && !s.cleanup &&
                    (s.phase == RetailWorldVisibleState::Phase::Root ||
                     s.phase == RetailWorldVisibleState::Phase::Fine))
                    return StepRetailWorldVisible(GeometryPageFixtureAction::RestoreRetailWorldVisible);
                if (!s.pair && !s.cleanup && s.phase == RetailWorldVisibleState::Phase::Failed)
                {
                    if (!cleanup(s)) return fail("RetailWorldCancelledPageQuarantined");
                    return current();
                }
                return current();
            }
            if (s.residence == Residence::RetiringFine || s.residence == Residence::RootOnly ||
                s.residence == Residence::RestoringForRefill || s.residence == Residence::Refilling)
            {
                // Pending record packets and worker jobs keep their immutable
                // actual owners. No per-frame bake, direct disk read or GPU probe.
                if (p.phase != RetailPagePilotState::Phase::Released &&
                    p.phase != RetailPagePilotState::Phase::FineReady)
                {
                    const auto progressed = StepRetailPageRecordPilot(GeometryPageFixtureAction::PollRetailRecords);
                    if (progressed.status == GeometryReportStatus::Failed ||
                        progressed.status == GeometryReportStatus::Invalid)
                    {
                        cancelResidency(s);
                        if (s.phase == RetailWorldVisibleState::Phase::Root)
                            return StepRetailWorldVisible(GeometryPageFixtureAction::RestoreRetailWorldVisible);
                        return fail("RetailWorldRefillFailedConventionalFallback");
                    }
                }
                if (s.residence == Residence::RetiringFine && p.phase == RetailPagePilotState::Phase::Released)
                    s.residence = Residence::RootOnly;
                if (s.residence == Residence::RestoringForRefill &&
                    s.phase == RetailWorldVisibleState::Phase::Restored &&
                    p.phase == RetailPagePilotState::Phase::Released)
                {
                    if (!serial(s.fallbackCamera) || s.fallbackCamera != s.cameraGeneration ||
                        !report.retailWorldRestored || !report.retailWorldReturned || report.retailWorldPagePresent)
                    { cancelResidency(s); return fail("RetailWorldRefillMissingConventionalAck"); }
                    s.residence = Residence::Refilling;
                    s.refillHandoff = true;
                    struct HandoffReset { bool& value; ~HandoffReset() { value = false; } } handoff{s.refillHandoff};
                    const auto requested = StepRetailPageRecordPilot(GeometryPageFixtureAction::RefillRetailRecords);
                    if (requested.status != GeometryReportStatus::Pending)
                    { cancelResidency(s); return fail("RetailWorldRefillRequestRefusedConventionalFallback"); }
                    s.refillRequest = 0; // filled only when the actual one-page job is admitted
                    return current();
                }
                if (s.residence == Residence::Refilling)
                {
                    if (p.workerRequest) s.refillRequest = p.workerRequest;
                    if (p.phase == RetailPagePilotState::Phase::FineReady)
                    {
                        if (!retakeFine(s))
                        { cancelResidency(s); return fail("RetailWorldRefilledTakeoverRefusedConventionalFallback"); }
                    }
                    return current();
                }
                // Root stable camera observations continue during retirement;
                // a near/Unknown demand may restore immediately, before its ACK.
                if (s.phase == RetailWorldVisibleState::Phase::Restored) return current();
            }
            if (s.autoEnabled && s.autoPolicy && !s.pair && !s.cleanup &&
                (s.phase == RetailWorldVisibleState::Phase::Fine ||
                 s.phase == RetailWorldVisibleState::Phase::Root))
            {
                using namespace GeometryPages;
                using Binary = HierarchicalBinaryDemand::Policy;
                const auto now = std::chrono::steady_clock::now();
                auto requireFine = [&](bool uncertain=true) -> GeometryPageFixtureReport {
                    s.lastAutoObservation = now;
                    if (uncertain) {
                        report.retailWorldProjectedBounded = false;
                        report.retailWorldForcedFine = true;
                    }
                    if (s.residencyEnabled &&
                        (s.residence == Residence::RetiringFine || s.residence == Residence::RootOnly))
                    {
                        // A camera may teleport; prefetch cannot guarantee a
                        // valid coarse discrepancy throughout disk latency.
                        // Keep original rendering while the complete Fine cut
                        // is rebuilt, rather than selecting missing triangles.
                        s.residence = Residence::RestoringForRefill;
                        s.autoEnabled = false; s.autoPolicy.reset();
                        report.retailWorldAutoEnabled = false;
                        return StepRetailWorldVisible(GeometryPageFixtureAction::RestoreRetailWorldVisible);
                    }
                    const auto changed = StepRetailWorldVisible(GeometryPageFixtureAction::FineRetailWorldVisible);
                    if (changed.status == GeometryReportStatus::Pending) return changed;
                    // The finite pilot must not keep an uncertified Root when
                    // its switch allowance is exhausted. Restore is uncapped.
                    s.autoEnabled = false; s.autoPolicy.reset();
                    report.retailWorldAutoEnabled = false;
                    AcquireProducerWindow("retail world unsafe root fallback");
                    if (_retailWorldStableFrame) {
                        _retailWorldStableFrame->Invalidate();
                        _retailWorldStableFrame.reset();
                    }
                    return StepRetailWorldVisible(GeometryPageFixtureAction::RestoreRetailWorldVisible);
                };
                if (s.lastAutoObservation != std::chrono::steady_clock::time_point{} &&
                    now - s.lastAutoObservation < std::chrono::milliseconds(100))
                    return current();
                const auto tracker = _retailWorldStableFrame;
                if (!tracker) return fail("RetailWorldAutoStableFrameMissing");
                const auto stable = tracker->Observe();
                if (stable.state != render::RetailWorldStableFrameObservation::State::Ready)
                {
                    if (stable.refusedFrames && stable.frameAttempt) {
                        // A refused frame breaks consecutive safe observations;
                        // it supplies no fabricated camera generation to Policy.
                        s.autoPolicy->Observe({});
                        report.retailWorldSafeRootObservations = 0;
                        s.lastAutoObservation = now;
                    }
                    // Initial Unknown has no returned frame yet. A refused
                    // attempted frame while Root cannot justify staying coarse.
                    if (s.selectedSlot == 1 && stable.refusedFrames &&
                        stable.frameAttempt && !s.pair)
                    {
                        report.retailWorldProjectedBounded = false;
                        report.retailWorldForcedFine = true;
                        const auto changed = requireFine();
                        if (changed.status == GeometryReportStatus::Pending)
                        {
                            if (report.retailWorldAutoRefines != UINT64_MAX)
                                ++report.retailWorldAutoRefines;
                            return current();
                        }
                        return changed;
                    }
                    return current();
                }
                if (stable.binding != binding(s,s.requestId)) {
                    s.autoPolicy->Observe({});
                    report.retailWorldSafeRootObservations = 0;
                    if (s.selectedSlot == 1) return requireFine();
                    return current();
                }
                if (stable.camera.generation <= s.lastAutoFrameGeneration ||
                    stable.camera.generation == UINT64_MAX)
                    return current();
                s.lastAutoObservation = now;
                s.lastAutoFrameGeneration = stable.camera.generation;
                report.retailWorldStableFrameGeneration = stable.camera.generation;
                const auto& certificate = *s.surface->certificate;
                ProjectedSurface::Input projection;
                auto& key = projection.binding;
                key.originalSource = certificate.originalSource;
                key.selectedKey = certificate.selectedDescriptor;
                key.coarsePackedSha256 = certificate.rootScalarPackedSha256;
                key.finePackedSha256 = certificate.fineScalarPackedSha256;
                key.coarseCut = certificate.rootCut;
                key.fineCut = certificate.fineCut;
                key.coarseCutCount = certificate.rootCutCount;
                key.fineCutCount = certificate.fineCutCount;
                key.coarseThresholdBits = certificate.rootThresholdBits;
                key.fineThresholdBits = certificate.fineThresholdBits;
                key.certificateAlgorithmVersion =
                    RigidFinalSurface::Certificate::AlgorithmVersion;
                key.fineToCoarseUpperBits = std::bit_cast<uint64_t>(certificate.fineToRoot);
                key.coarseToFineUpperBits = std::bit_cast<uint64_t>(certificate.rootToFine);
                key.hausdorffUpperBits = std::bit_cast<uint64_t>(certificate.hausdorffUpper);
                projection.cut = {stable.camera.generation,s.pageModel.modelBirth,
                    certificate.sourceAdmissionEpoch};
                projection.minimum = certificate.minimum;
                projection.maximum = certificate.maximum;
                projection.surfaceDistanceUpper = certificate.hausdorffUpper;
                projection.viewportWidth = stable.camera.render_width;
                projection.viewportHeight = stable.camera.render_height;
                projection.clipNear = -stable.camera.projection[14];
                std::copy(std::begin(stable.camera.projection),
                    std::end(stable.camera.projection),projection.projection.begin());
                std::copy(std::begin(stable.camera.view),
                    std::end(stable.camera.view),projection.view.begin());
                std::copy(std::begin(s.page.world.m),
                    std::end(s.page.world.m),projection.model.begin());
                for (size_t axis=0;axis<3;++axis)
                    projection.model[12+axis] -= stable.camera.camera_position[axis];
                ProjectedSurface::Result projected;
                const auto projectedStatus = ProjectedSurface::Build(
                    projection,key,projection.cut,projected);
                const bool bounded = projectedStatus == ProjectedSurface::Status::Bounded;
                report.retailWorldProjectionStatus = uint32_t(projectedStatus)+1;
                report.retailWorldProjectedBounded = bounded;
                report.retailWorldProjectedUpper = bounded ? projected.euclideanPixelUpper : 0;
                report.retailWorldForcedFine = !bounded;
                const bool root = s.selectedSlot == 1;
                const double allowance = root ? Binary::RefineAllowance :
                    Binary::CoarsenAllowance;
                HierarchicalBinaryDemand::Observation observation;
                observation.binding = {certificate.identity,s.ownerEpoch,s.sourceEpoch,
                    s.pageEpoch,s.shapeBirth};
                observation.frameGeneration = stable.camera.generation;
                observation.requiredViewMask = 1; // fixed returned main camera only
                observation.maximumIndicator = bounded ? projected.euclideanPixelUpper : 0;
                observation.allowance = allowance;
                observation.currentlyRoot = root;
                observation.forcedFine = !bounded;
                observation.wantsFine = !bounded ||
                    projected.euclideanPixelUpper > allowance;
                const auto decision = s.autoPolicy->Observe(observation);
                const auto counters = s.autoPolicy->Stats();
                const auto total = [](uint64_t base,uint64_t value) {
                    return value > UINT64_MAX-base ? UINT64_MAX : base+value;
                };
                report.retailWorldAutoObservations = total(s.autoObservationOffset,counters.observations);
                report.retailWorldAutoRefines = std::max(report.retailWorldAutoRefines,
                    total(s.autoRefineOffset,counters.refine));
                report.retailWorldAutoCoarsens = std::max(report.retailWorldAutoCoarsens,
                    total(s.autoCoarsenOffset,counters.coarsen));
                // Keep the last satisfied three-frame coarsen witness visible
                // after Policy resets its internal consecutive counter.
                report.retailWorldSafeRootObservations = std::max(
                    report.retailWorldSafeRootObservations,
                    decision == HierarchicalBinaryDemand::Decision::Coarsen ?
                        Binary::RequiredSafeObservations :
                        s.autoPolicy->SafeRootObservations());
                if (decision == HierarchicalBinaryDemand::Decision::Refine)
                    return requireFine(!bounded);
                if (decision == HierarchicalBinaryDemand::Decision::Coarsen)
                    return StepRetailWorldVisible(
                        GeometryPageFixtureAction::RootRetailWorldVisible);
                if (decision == HierarchicalBinaryDemand::Decision::Refused &&
                    root) return requireFine();
                if (root && bounded && projected.euclideanPixelUpper <= Binary::CoarsenAllowance &&
                    s.residencyEnabled && (s.residence == Residence::Both ||
                        (p.reusableWorldFine && s.residence == Residence::Complete)) &&
                    s.residencyCycles < (p.reusableWorldFine ? RetailWorldResidencyCycleLimit : 1) &&
                    p.phase == RetailPagePilotState::Phase::FineReady &&
                    (!p.refilled || p.reusableWorldFine) &&
                    !p.nonrootPages.empty() && !p.observation && !s.pair && !s.cleanup)
                {
                    // Only a fresh successful Root frame with its own joined
                    // certified bound authorizes retirement. The older Fine
                    // coarsen decision alone is not enough after camera motion.
                    s.residencyTriggerCamera = stable.camera.generation;
                    s.residence = Residence::RetiringFine;
                    // A later cycle cannot borrow the previous returned Restore
                    // witness or worker request as its own refill authority.
                    s.refillRequest=0; s.fallbackCamera=s.fallbackRequest=s.fallbackPageBirth=s.fallbackEpoch=0;
                    s.refillAtFallback=0; s.fallbackOriginalFlags=UINT32_MAX;
                    s.fallbackPageAbsent=false;
                    const auto released = StepRetailPageRecordPilot(GeometryPageFixtureAction::ReleaseRetailRecords);
                    if (released.status != GeometryReportStatus::Pending)
                    {
                        // Nothing was retired; preserve complete Fine and stop
                        // this finite attempt rather than retrying allocations.
                        s.residencyEnabled = false; s.residence = Residence::Disabled;
                        return refuse("RetailWorldFineRetirementNotAdmitted");
                    }
                    s.retirementRequest = report.requestId;
                    return current();
                }
            }
            return current();
        }
        if (s.pair || s.cleanup || s.invalidated ||
            s.phase == RetailWorldVisibleState::Phase::Failed ||
            s.phase == RetailWorldVisibleState::Phase::Removed ||
            s.phase == RetailWorldVisibleState::Phase::Restored ||
            !sourceCurrent(s) || !report.retailWorldReturned ||
            !report.retailWorldPagePresent)
            return refuse("RetailWorldCutUnavailable");
        if (action == GeometryPageFixtureAction::FineRetailWorldVisible && s.residencyEnabled &&
            (s.residence == RetailWorldVisibleState::Residence::RetiringFine ||
             s.residence == RetailWorldVisibleState::Residence::RootOnly))
        {
            s.residence = RetailWorldVisibleState::Residence::RestoringForRefill;
            s.autoEnabled = false; s.autoPolicy.reset(); report.retailWorldAutoEnabled = false;
            return StepRetailWorldVisible(GeometryPageFixtureAction::RestoreRetailWorldVisible);
        }
        uint32_t slot = UINT32_MAX;
        if (action == GeometryPageFixtureAction::RootRetailWorldVisible) slot = 1;
        else if (action == GeometryPageFixtureAction::FineRetailWorldVisible)
            slot = p.FineSlot();
        else if (action != GeometryPageFixtureAction::RestoreRetailWorldVisible)
            return refuse("RetailWorldActionUnsupported");
        if (action != GeometryPageFixtureAction::RestoreRetailWorldVisible &&
            (slot == s.selectedSlot || s.switches >= 8 || !cut(slot)))
            return refuse("RetailWorldCutNotReady");
        if (action == GeometryPageFixtureAction::RestoreRetailWorldVisible &&
            !ready(s.selectedSlot)) return refuse("RetailWorldRestoreModelUnavailable");
        if (action == GeometryPageFixtureAction::RestoreRetailWorldVisible && s.residencyEnabled &&
            s.residence != RetailWorldVisibleState::Residence::RestoringForRefill)
            cancelResidency(s); // explicit restore/stop ends this finite residency objective
        AcquireProducerWindow("retail world cut model");
        const uint64_t ticket = nextRequest();
        if (!ticket) return refuse("RetailWorldRequestExhausted");
        Pair::Request request;
        request.binding = binding(s,ticket);
        request.originalRendererHandle = s.originalRenderer;
        request.pageRendererHandle = s.pageRenderer;
        request.previousCameraGeneration = s.cameraGeneration;
        auto hidden = s.original; hidden.flags = (hidden.flags & WGR_INSTANCE_SURFACE_RECEIVERS) | WGR_INSTANCE_OTHER_VIEWS_ONLY;
        request.originalBefore = row(s.originalModel,hidden);
        request.originalAfter = request.originalBefore;
        request.pageBefore = row(s.pageModel,s.page);
        if (action == GeometryPageFixtureAction::RestoreRetailWorldVisible)
        {
            request.action = Pair::Action::Restore;
            request.originalAfter = row(s.originalModel,s.original);
        }
        else
        {
            request.action = Pair::Action::UpdatePage;
            auto page = s.page; page.model = p.models[slot];
            request.pageAfter = row(model(p.models[slot]),page);
            if (request.pageAfter.model.producerModel == UINT32_MAX)
                return refuse("RetailWorldNewCutModelUnavailable");
        }
        if (!queue(s,request)) return refuse("RetailWorldPairPreflightRefused");
        s.pendingSlot = slot;
        if (request.action == Pair::Action::Restore)
        {
            s.phase = RetailWorldVisibleState::Phase::AwaitRestore;
            report.retailWorldPhase = "AwaitRestore";
            report.pageWorkState = "RetailWorldRestoreQueued";
        }
        else
        {
            ++s.switches;
            s.phase = RetailWorldVisibleState::Phase::AwaitUpdate;
            report.retailWorldPhase = "AwaitCutUpdate";
            report.pageWorkState = "RetailWorldCutQueued";
        }
        return current();
    }
    catch (...) { return fail("RetailWorldOwnerExceptionQuarantined"); }
}
} // namespace Poseidon
