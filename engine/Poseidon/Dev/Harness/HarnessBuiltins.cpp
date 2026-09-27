#include <Poseidon/Dev/Harness/HarnessBuiltins.hpp>
#include <Poseidon/Input/InputSubsystem.hpp>
using namespace Poseidon;
#include <Poseidon/Dev/Harness/HarnessServer.hpp>
#include <Poseidon/Dev/Harness/HarnessProtocol.hpp>

#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Weather/SnowField.hpp>
#include <Poseidon/World/Weather/RainVolume.hpp>
#include <Poseidon/World/Physics/PhysicsWorld.hpp>
#include <Poseidon/Dev/Diag/PhysicsCorpus.hpp>
#include <Poseidon/Dev/Diag/StreamingDiag.hpp>
#include <Poseidon/Graphics/Textures/PreparedTextures.hpp>
#include <Poseidon/IO/Streams/ArchiveSourceBinding.hpp>
#include <Poseidon/Dev/Diag/BallisticsRecorder.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Dev/Debug/DevPanelWidgets.hpp>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>
#include <cjson/cJSON.h>
#include <stdint.h>
#include <vector>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Memory/MemAlloc.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
// wingdi.h defines GetObject as a macro (GetObjectA). Undef it before the
// network headers so INetworkManager's virtual GetObject keeps its real name
// consistently across the interface (NetworkIface.hpp) and its overrides
// (NetworkImpl.hpp) — otherwise the macro mangles one but not the other and
// NetworkManager looks abstract.
#ifdef GetObject
#undef GetObject
#endif
#include <Poseidon/Network/Network.hpp>
#include <Poseidon/Network/NetworkImplServer.hpp>
#include <Poseidon/Network/NetworkImpl.hpp>
#include <Poseidon/Network/NetworkConfig.hpp>
#include <Poseidon/Network/MasterServerServiceClient.hpp>
#include <Poseidon/Network/MasterServerBrowser.hpp>
#include <Poseidon/Network/HttpTestRewrite.hpp>
#include <Poseidon/AI/AIUnit.hpp>
#include <Poseidon/AI/InfantryCombat.hpp>
#include <Poseidon/Game/Commands/GameStateExt.hpp>
#include <Poseidon/AI/AIGroup.hpp>
#include <Poseidon/World/Entities/Weapons/Weapons.hpp>
#include <Poseidon/Core/ModId.hpp>
#include <Poseidon/Core/ServerModResolve.hpp>
#include <Poseidon/Core/PendingConnect.hpp>
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/UI/UITestEngine.hpp>
#include <Poseidon/UI/Controls/UIControls.hpp>
#include <Poseidon/UI/InGame/InGameUI.hpp>
#include <Poseidon/World/Entities/Infantry/Person.hpp>
#include <Poseidon/World/Entities/Infantry/SoldierOld.hpp>
#include <Poseidon/World/Entities/Infantry/MoveActions.hpp>
#include <Poseidon/AI/EntityAI.hpp>
#include <Evaluator/express.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <climits>
#include <unordered_set>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <string>
#include <thread>

namespace Poseidon::Dev
{
namespace HarnessBuiltins
{

static const char* SimulationStatusName(Streaming::SimulationResidencyStatus status)
{
    using Status = Streaming::SimulationResidencyStatus;
    switch (status) {
        case Status::Disabled: return "Disabled";
        case Status::Pending: return "Pending";
        case Status::Ready: return "Ready";
        case Status::Unknown: return "Unknown";
        case Status::Invalid: return "Invalid";
        case Status::CapacityExceeded: return "CapacityExceeded";
        case Status::WrongOwner: return "WrongOwner";
    }
    return "Invalid";
}

void RegisterStreamingProbes(HarnessServer& hs)
{
    hs.RegisterCommand({"stream_source_diagnostic", "Explicit bounded source-snapshot request/poll; no geometry or Ready proof", {}},
        [](const std::string&, cJSON* root) -> std::string {
            if (!GLandscape) return HarnessProtocol::ErrorResponse("no landscape");
            const auto* index = cJSON_GetObjectItemCaseSensitive(root, "modelIndex");
            if (!cJSON_IsNumber(index) || !std::isfinite(index->valuedouble) || index->valuedouble < 0 ||
                index->valuedouble > UINT32_MAX || std::floor(index->valuedouble) != index->valuedouble)
                return HarnessProtocol::ErrorResponse("exact nonnegative integer modelIndex required");
            const auto modelIndex = static_cast<uint32_t>(index->valuedouble);
            const auto* action = cJSON_GetObjectItemCaseSensitive(root, "action");
            if (action && (!cJSON_IsString(action) ||
                (std::strcmp(action->valuestring, "request") && std::strcmp(action->valuestring, "poll"))))
                return HarnessProtocol::ErrorResponse("action must be request or poll");
            using Request = ObjectStreamPreparer::SourceEnvelopeRequest;
            auto request = Request::Unavailable;
            const bool requesting = action && !std::strcmp(action->valuestring, "request");
            if (requesting) request = GLandscape->RequestModernSourceDiagnostic(modelIndex);
            const auto snapshot = GLandscape->SnapshotModernSourceDiagnostic(modelIndex);
            if (!requesting) request = snapshot.requestResult;
            const char* requestName = "Unavailable";
            switch (request) {
                case Request::Requested: requestName = "Requested"; break;
                case Request::TooLate: requestName = "TooLate"; break;
                case Request::Capacity: requestName = "Capacity"; break;
                case Request::WrongThread: requestName = "WrongThread"; break;
                case Request::Unavailable: break;
            }
            using Status = Landscape::ModernSourceDiagnosticStatus;
            const char* status = "Invalid";
            switch (snapshot.status) {
                case Status::NotRequested: status = "NotRequested"; break;
                case Status::Pending: status = "Pending"; break;
                case Status::LatestSnapshot: status = "LatestSnapshot"; break;
                case Status::Refused: status = "Refused"; break;
                case Status::Expired: status = "Expired"; break;
                case Status::Interrupted: status = "Interrupted"; break;
                case Status::StaleInventory: status = "StaleInventory"; break;
                case Status::WrongThread: status = "WrongThread"; break;
                case Status::Invalid: break;
            }
            using Observation = Streaming::StaticPlainRouteObservation;
            const char* classNow = "Unknown";
            switch (snapshot.classNow) {
                case Observation::EmptyClassNow: classNow = "EmptyClassNow"; break;
                case Observation::Unsupported: classNow = "Unsupported"; break;
                case Observation::WrongThread: classNow = "WrongThread"; break;
                case Observation::StaleSource: classNow = "StaleSource"; break;
                case Observation::Unknown: break;
            }
            const auto& latest = snapshot.latestSnapshot;
            auto* result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "scope", "latest original parse source facts and immediate config observation; not final bounds, Ready, disk freshness or per-request completion");
            cJSON_AddNumberToObject(result, "modelIndex", modelIndex);
            cJSON_AddStringToObject(result, "requestResult", requestName);
            cJSON_AddStringToObject(result, "status", status);
            cJSON_AddBoolToObject(result, "pendingDemand", snapshot.pendingDemand);
            cJSON_AddBoolToObject(result, "diagnosticActive", GLandscape->HasModernSourceDiagnosticDemand());
            cJSON_AddBoolToObject(result, "attempted", latest.attempted);
            cJSON_AddNumberToObject(result, "inventoryGeneration", double(latest.generation));
            cJSON_AddStringToObject(result, "modelIdentity", latest.modelIdentity.c_str());
            cJSON_AddBoolToObject(result, "sourceEvidence", latest.envelope.State() == Streaming::StaticSourceEnvelopeState::SourceEvidence);
            auto* reasons = cJSON_AddArrayToObject(result, "sourceReasons");
            using Reason = Streaming::StaticSourceEnvelopeReason;
            const std::pair<Reason, const char*> sourceReasons[] = {
                {Reason::Generation,"Generation"}, {Reason::Format,"Format"}, {Reason::Capacity,"Capacity"},
                {Reason::NonFinite,"NonFinite"}, {Reason::NoVertices,"NoVertices"}, {Reason::MissingRole,"MissingRole"},
                {Reason::Proxy,"Proxy"}, {Reason::Animation,"Animation"}, {Reason::LandConform,"LandConform"},
                {Reason::InvalidFace,"InvalidFace"}, {Reason::InconsistentPurpose,"InconsistentPurpose"},
                {Reason::IncompleteSource,"IncompleteSource"}
            };
            for (const auto& [reason, name] : sourceReasons)
                if (latest.envelope.HasReason(reason)) cJSON_AddItemToArray(reasons, cJSON_CreateString(name));
            cJSON_AddBoolToObject(result, "plainSourceFacts", latest.plainSource.State() == Streaming::StaticPlainSourceSummaryState::SourceFacts);
            cJSON_AddStringToObject(result, "configModelName", std::string(latest.plainSource.ConfigModelName()).c_str());
            cJSON_AddStringToObject(result, "latestSnapshotClassObservation", classNow);
            if (const auto radius = latest.envelope.RadiusForGeneration(latest.generation))
                cJSON_AddNumberToObject(result, "sourceRadius", *radius);
            cJSON_AddNumberToObject(result, "sourcePayloadBytes", double(latest.envelope.PayloadBytes()));
            return HarnessProtocol::JsonResponse(result);
        });
    hs.RegisterCommand({"stream_identity_probe", "Read world IDs and geometry presence without retaining objects", {}},
        [](const std::string&, cJSON* root) -> std::string {
            if (!GLandscape) return HarnessProtocol::ErrorResponse("no landscape");
            std::vector<int> ids;
            const auto worldIds = GLandscape->GetObjectIDList();
            std::unordered_set<int> presentIds;
            for (int i = 0; i < worldIds.Size(); ++i) presentIds.insert(worldIds[i]);
            const auto* requested = cJSON_GetObjectItemCaseSensitive(root, "ids");
            if (requested) {
                if (!cJSON_IsArray(requested) || cJSON_GetArraySize(requested) > 16)
                    return HarnessProtocol::ErrorResponse("ids must be an array of at most 16 integers");
                for (int i = 0; i < cJSON_GetArraySize(requested); ++i) {
                    const auto* id = cJSON_GetArrayItem(requested, i);
                    if (!cJSON_IsNumber(id) || !std::isfinite(id->valuedouble) || id->valuedouble < 0 ||
                        id->valuedouble > INT_MAX || std::floor(id->valuedouble) != id->valuedouble)
                        return HarnessProtocol::ErrorResponse("invalid object ID");
                    ids.push_back(id->valueint);
                }
            } else {
                const auto* x = cJSON_GetObjectItemCaseSensitive(root, "x");
                const auto* z = cJSON_GetObjectItemCaseSensitive(root, "z");
                if (!cJSON_IsNumber(x) || !cJSON_IsNumber(z) || !std::isfinite(x->valuedouble) || !std::isfinite(z->valuedouble))
                    return HarnessProtocol::ErrorResponse("finite x/z or explicit ids required");
                const auto all = GLandscape->GetObjectIDList();
                std::vector<std::pair<double, int>> nearest;
                for (int i = 0; i < all.Size(); ++i) {
                    auto* object = GLandscape->FindObject(all[i]);
                    if (!object || !object->GetShape()) continue;
                    const double dx = object->Position().X() - x->valuedouble;
                    const double dz = object->Position().Z() - z->valuedouble;
                    nearest.emplace_back(dx * dx + dz * dz, all[i]);
                }
                const auto* limit = cJSON_GetObjectItemCaseSensitive(root, "limit");
                if (limit && (!cJSON_IsNumber(limit) || !std::isfinite(limit->valuedouble) ||
                    limit->valuedouble < 1 || limit->valuedouble > 64 || std::floor(limit->valuedouble) != limit->valuedouble))
                    return HarnessProtocol::ErrorResponse("limit must be an integer 1..64");
                const size_t count = std::min<size_t>(limit ? limit->valueint : 16, nearest.size());
                std::partial_sort(nearest.begin(), nearest.begin() + count, nearest.end());
                for (size_t i = 0; i < count; ++i) ids.push_back(nearest[i].second);
            }
            auto* result = cJSON_CreateObject();
            auto* objects = cJSON_AddArrayToObject(result, "objects");
            for (int id : ids) {
                auto* item = cJSON_CreateObject();
                cJSON_AddNumberToObject(item, "id", id);
                auto* object = presentIds.contains(id) ? GLandscape->FindObject(id) : nullptr;
                cJSON_AddBoolToObject(item, "present", object != nullptr);
                if (object) {
                    cJSON_AddBoolToObject(item, "destroyed", object->IsDestroyed());
                    cJSON_AddNumberToObject(item, "destroyPhase", object->GetDestroyed());
                    cJSON_AddNumberToObject(item, "rawDamage", object->GetRawTotalDammage());
                    cJSON_AddBoolToObject(item, "mustBeSaved", object->MustBeSaved());
                    cJSON_AddBoolToObject(item, "visualResident", object->IsVisualResident());
                    cJSON_AddNumberToObject(item, "x", object->Position().X());
                    cJSON_AddNumberToObject(item, "z", object->Position().Z());
                    cJSON_AddNumberToObject(item, "y", object->Position().Y());
                    cJSON_AddNumberToObject(item, "radius", object->GetRadius());
                    // Read the actual instance frame without resolving or pinning
                    // an object. Column order is right, up, forward, translation.
                    auto* frame = cJSON_AddArrayToObject(item, "frame");
                    const auto& transform = object->Transform();
                    for (const auto& column : {transform.DirectionAside(), transform.DirectionUp(),
                                               transform.Direction(), transform.Position()})
                        for (int axis = 0; axis < 3; ++axis)
                            cJSON_AddItemToArray(frame, cJSON_CreateNumber(column[axis]));
                    const auto* shape = object->GetShape();
                    if (shape) cJSON_AddStringToObject(item, "model", shape->Name());
                    cJSON_AddBoolToObject(item, "hasGeometry", shape && shape->FindGeometryLevel() >= 0);
                    cJSON_AddBoolToObject(item, "hasFireGeometry", shape && shape->FindFireGeometryLevel() >= 0);
                    cJSON_AddBoolToObject(item, "hasViewGeometry", shape && shape->FindViewGeometryLevel() >= 0);
                    cJSON_AddBoolToObject(item, "shapeVisualDeferred", shape && shape->DeferredVisualRendering());
                    int normalBuffers = 0;
                    if (shape)
                        for (int level = 0; level < shape->NLevels(); ++level)
                            if (shape->IsNormalLevel(level) && shape->Level(level) && shape->Level(level)->GetVertexBuffer())
                                ++normalBuffers;
                    cJSON_AddNumberToObject(item, "normalVertexBuffers", normalBuffers);
                }
                cJSON_AddItemToArray(objects, item);
            }
            return HarnessProtocol::JsonResponse(result);
        });
    hs.RegisterCommand({"stream_simulation_residency", "Read-only owner simulation-residency snapshot; never pumps or resolves", {}},
        [](const std::string&, cJSON*) -> std::string {
            if (!GLandscape) return HarnessProtocol::ErrorResponse("no landscape");
            const auto state = GLandscape->SnapshotModernSimulationResidency();
            const char* status = "Invalid";
            using Status = Streaming::SimulationResidencyStatus;
            switch (state.status) {
                case Status::Disabled: status = "Disabled"; break;
                case Status::Pending: status = "Pending"; break;
                case Status::Ready: status = "Ready"; break;
                case Status::Unknown: status = "Unknown"; break;
                case Status::Invalid: break;
                case Status::CapacityExceeded: status = "CapacityExceeded"; break;
                case Status::WrongOwner: status = "WrongOwner"; break;
            }
            auto* result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "status", status);
            // These are CPU installation/lease observations, never a clear-ray
            // result or evidence that existing legacy collision users are migrated.
            const auto number = [&](const char* name, uint64_t value) {
                cJSON_AddNumberToObject(result, name, static_cast<double>(value));
            };
            number("inventoryRows", state.inventoryRows);
            number("inventoryTotal", state.inventoryTotal);
            cJSON_AddBoolToObject(result, "metadataComplete", state.metadataComplete);
            number("metadataRecordBytes", state.metadataRecordBytes);
            number("referencedModels", state.referencedModels);
            number("activeModels", state.activeModels);
            number("sparseLeaseRecords", state.sparseLeaseRecords);
            number("metadataPlacementRefusals", state.metadataPlacementRefusals);
            number("metadataModelRefusals", state.metadataModelRefusals);
            number("metadataByteRefusals", state.metadataByteRefusals);
            number("activeModelRefusals", state.activeModelRefusals);
            number("coverageGeneration", state.coverageGeneration);
            number("models", state.models);
            number("preparedModels", state.preparedModels);
            number("unknownModels", state.unknownModels);
            number("regions", state.regions);
            number("readyRegions", state.readyRegions);
            number("pendingRegions", state.pendingRegions);
            number("unknownRegions", state.unknownRegions);
            number("refusedRegions", state.refusedRegions);
            number("watchedPlacements", state.watchedPlacements);
            number("liveWatchedPlacements", state.liveWatchedPlacements);
            number("queryRegions", state.queryRegions);
            number("queryBorrowedModels", state.queryBorrowedModels);
              number("queryBorrowedEntries", state.queryBorrowedEntries);
              number("registeredBorrowedModels", state.registeredBorrowedModels);
              number("registeredBorrowShapeCharge", state.registeredBorrowShapeCharge);
              number("registeredValidationBudgetRefusals", state.registeredValidationBudgetRefusals);
              cJSON_AddBoolToObject(result, "registeredMetadataEverComplete", state.registeredMetadataEverComplete);
            number("readyQueries", state.readyQueries);
            number("pendingQueries", state.pendingQueries);
            number("unknownQueries", state.unknownQueries);
            number("refusedQueries", state.refusedQueries);
            number("leasedPlacements", state.leasedPlacements);
            number("positiveScannedRegions", state.positiveScannedRegions);
            number("positiveLeasedPlacements", state.positiveLeasedPlacements);
            number("positiveColdRequested", state.positiveColdRequested);
            number("positiveColdPrepared", state.positiveColdPrepared);
            number("positiveColdCreated", state.positiveColdCreated);
            number("positiveColdRefused", state.positiveColdRefused);
            number("positiveColdInterrupted", state.positiveColdInterrupted);
            number("positiveColdWorkingModels", state.positiveColdWorkingModels);
            number("positiveColdFactsBytes", state.positiveColdFactsBytes);
            number("positiveColdTailCalls", state.positiveColdTailCalls);
            number("positiveColdBankCalls", state.positiveColdBankCalls);
            cJSON_AddNumberToObject(result, "positiveColdLastBankMs", state.positiveColdLastBankMs);
            cJSON_AddNumberToObject(result, "positiveColdMaxBankMs", state.positiveColdMaxBankMs);
            cJSON_AddNumberToObject(result, "positiveColdLastTailMs", state.positiveColdLastTailMs);
            cJSON_AddNumberToObject(result, "positiveColdMaxTailMs", state.positiveColdMaxTailMs);
            number("created", state.created);
            number("released", state.released);
            number("queueRefused", state.queueRefused);
            number("payloadCapacityCharge", state.payloadCapacityCharge);
            number("shapeSchedulingCharge", state.shapeSchedulingCharge);
            number("shapeElementBytes", state.shapeElementBytes);
            number("lastPumpVisits", state.lastPumpVisits);
            number("ownerPumps", state.ownerPumps);
            number("fireCollisionCalls", state.fireCollisionCalls);
            number("fireCollisionCandidateTests", state.fireCollisionCandidateTests);
            number("fireCollisionWorkRefusals", state.fireCollisionWorkRefusals);
            cJSON_AddNumberToObject(result, "lastFireCollisionMs", state.lastFireCollisionMs);
            ObjectStreamResidencyCounters visualState;
            GLandscape->SnapshotObjectStreamDiag(visualState);
            number("requiredObjects", visualState.requiredObjects);
            if (GEngine)
                cJSON_AddStringToObject(result, "rendererName", GEngine->GetRendererName());
            cJSON_AddNumberToObject(result, "lastPumpMs", state.lastPumpMs);
            return HarnessProtocol::JsonResponse(result);
        });
    hs.RegisterCommand({"stream_fire_probe", "Read-only bounded world fire-geometry segment query", {}},
        [](const std::string&, cJSON* root) -> std::string {
            if (!GLandscape) return HarnessProtocol::ErrorResponse("no landscape");
            Vector3 endpoints[2];
            const char* keys[] = {"from", "to"};
            for (int end = 0; end < 2; ++end) {
                const auto* coords = cJSON_GetObjectItemCaseSensitive(root, keys[end]);
                if (!cJSON_IsArray(coords) || cJSON_GetArraySize(coords) != 3)
                    return HarnessProtocol::ErrorResponse("from/to require [x,y,z]");
                for (int axis = 0; axis < 3; ++axis) {
                    const auto* value = cJSON_GetArrayItem(coords, axis);
                    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
                        std::abs(value->valuedouble) > 1000000)
                        return HarnessProtocol::ErrorResponse("finite bounded coordinates required");
                    endpoints[end][axis] = static_cast<float>(value->valuedouble);
                }
            }
            const float length = endpoints[0].Distance(endpoints[1]);
            if (length < 0.001f || length > 2000)
                return HarnessProtocol::ErrorResponse("segment length must be 0.001..2000 metres");
            CollisionBuffer hits;
            GLandscape->ObjectCollision(hits, nullptr, nullptr, endpoints[0], endpoints[1], 0.01f, ObjIntersectFire);
            auto* result = cJSON_CreateObject();
            auto* contacts = cJSON_AddArrayToObject(result, "contacts");
            for (int i = 0; i < hits.Size(); ++i) {
                const auto& hit = hits[i];
                if (!hit.object) continue;
                auto* item = cJSON_CreateObject();
                cJSON_AddNumberToObject(item, "id", hit.object->ID());
                if (hit.object->GetShape()) cJSON_AddStringToObject(item, "model", hit.object->GetShape()->Name());
                cJSON_AddItemToArray(contacts, item);
            }
            return HarnessProtocol::JsonResponse(result);
        });
}

void RegisterScreenshot(HarnessServer& hs)
{
    RegisterStreamingProbes(hs);
    hs.RegisterCommand({"stream_lod_demand", "Explicit bounded selected retained-GPU LOD demand diagnostic", {}},
        [](const std::string&, cJSON* root) -> std::string {
            if (!GEngine) return HarnessProtocol::ErrorResponse("engine unavailable");
            const auto* actionNode = cJSON_GetObjectItemCaseSensitive(root, "action");
            if (!cJSON_IsString(actionNode)) return HarnessProtocol::ErrorResponse("request, poll or cancel action required");
            const std::string action = actionNode->valuestring;
            const auto integer = [](const cJSON* node, double maximum) {
                return cJSON_IsNumber(node) && std::isfinite(node->valuedouble) && node->valuedouble >= 0 &&
                    node->valuedouble <= maximum && std::floor(node->valuedouble) == node->valuedouble;
            };
            uint64_t requestId = 0; Engine::LodDemandReport report;
            auto status = Engine::GeometryReportStatus::Invalid;
            if (action == "request") {
                const auto* ids = cJSON_GetObjectItemCaseSensitive(root, "modelIds");
                const auto* frames = cJSON_GetObjectItemCaseSensitive(root, "frames");
                if (!cJSON_IsArray(ids) || cJSON_GetArraySize(ids) < 1 || cJSON_GetArraySize(ids) > 8 ||
                    !integer(frames, 64) || frames->valuedouble < 1) return HarnessProtocol::ErrorResponse("1..8 distinct renderer modelIds and 1..64 frames required");
                std::vector<uint32_t> models;
                for (int i = 0; i < cJSON_GetArraySize(ids); ++i) {
                    const auto* id = cJSON_GetArrayItem(ids, i);
                    if (!integer(id, UINT32_MAX - 1)) return HarnessProtocol::ErrorResponse("uint32 renderer handles excluding the invalid sentinel required");
                    models.push_back(static_cast<uint32_t>(id->valuedouble));
                }
                status = GEngine->RequestLodDemandReport(models, static_cast<uint32_t>(frames->valuedouble), requestId);
            } else if (action == "poll" || action == "cancel") {
                const auto* id = cJSON_GetObjectItemCaseSensitive(root, "requestId");
                if (!integer(id, 9007199254740991.0) || id->valuedouble < 1) return HarnessProtocol::ErrorResponse("exact positive requestId required");
                requestId = static_cast<uint64_t>(id->valuedouble);
                status = action == "poll" ? GEngine->PollLodDemandReport(requestId, report) : GEngine->CancelLodDemandReport(requestId);
            } else return HarnessProtocol::ErrorResponse("unknown LOD diagnostic action");
            const char* name = "Failed";
            switch (status) {
                case Engine::GeometryReportStatus::Unsupported: name = "Unsupported"; break;
                case Engine::GeometryReportStatus::Pending: name = "Pending"; break;
                case Engine::GeometryReportStatus::Busy: name = "Busy"; break;
                case Engine::GeometryReportStatus::Ready: name = report.samplingStatus == 3 ? "Cancelled" : "Ready"; break;
                case Engine::GeometryReportStatus::Invalid: name = "Invalid"; break;
                case Engine::GeometryReportStatus::Failed: break;
            }
            auto* result = cJSON_CreateObject(); cJSON_AddStringToObject(result, "status", name);
            cJSON_AddNumberToObject(result, "requestId", static_cast<double>(requestId));
            cJSON_AddStringToObject(result, "scope", "selected monotonic renderer handles; retained LODs demanded by actually dispatched cull passes; no asset freshness, paging or clearance proof");
            cJSON_AddBoolToObject(result, "ownershipCoverageComplete", false); cJSON_AddBoolToObject(result, "cachedViewCoverageComplete", false);
            cJSON_AddBoolToObject(result, "snapshotAvailable", report.requestId == requestId && requestId != 0);
            if (report.requestId == requestId && requestId) {
#define LOD_DEMAND_FIELD(field) cJSON_AddNumberToObject(result, #field, static_cast<double>(report.field))
                LOD_DEMAND_FIELD(currentFrame); LOD_DEMAND_FIELD(lastSampleFrame); LOD_DEMAND_FIELD(framesRequested);
                LOD_DEMAND_FIELD(framesAttempted); LOD_DEMAND_FIELD(framesSampled); LOD_DEMAND_FIELD(droppedFrames);
                LOD_DEMAND_FIELD(mapFailures); LOD_DEMAND_FIELD(droppedDispatches); LOD_DEMAND_FIELD(dispatchedViews); LOD_DEMAND_FIELD(passMask);
#undef LOD_DEMAND_FIELD
                cJSON_AddNumberToObject(result, "sampleAgeFrames", static_cast<double>(report.currentFrame - std::min(report.currentFrame, report.lastSampleFrame)));
                auto* rows = cJSON_AddArrayToObject(result, "rows");
                const char* states[] = {"Live", "Missing", "Retired", "UnsupportedLodCount"};
                for (const auto& row : report.rows) {
                    auto* item = cJSON_CreateObject(); cJSON_AddNumberToObject(item, "rendererModel", row.rendererModel);
                    cJSON_AddNumberToObject(item, "lodCount", row.lodCount); cJSON_AddNumberToObject(item, "lodMask", row.lodMask);
                    cJSON_AddStringToObject(item, "state", row.state < 4 ? states[row.state] : "Unknown"); cJSON_AddItemToArray(rows, item);
                }
            }
            return HarnessProtocol::JsonResponse(result);
        });
    hs.RegisterCommand({"archive_source_bindings", "Read-only bounded actual-read PAA provenance counters", {}},
        [](const std::string&, cJSON*) -> std::string {
            const auto snapshot = ArchiveSourceBinding::SnapshotStats();
            auto* result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "enabled", snapshot.enabled);
#define ARCHIVE_BINDING_FIELD(field) cJSON_AddNumberToObject(result, #field, static_cast<double>(snapshot.field))
            ARCHIVE_BINDING_FIELD(liveBindings); ARCHIVE_BINDING_FIELD(knownCppBytes);
            ARCHIVE_BINDING_FIELD(capBindings); ARCHIVE_BINDING_FIELD(capBytes);
            ARCHIVE_BINDING_FIELD(wrappedReads); ARCHIVE_BINDING_FIELD(initRetains);
            ARCHIVE_BINDING_FIELD(captureRefused); ARCHIVE_BINDING_FIELD(capacityRefused);
            ARCHIVE_BINDING_FIELD(allocationRefused);
#undef ARCHIVE_BINDING_FIELD
            cJSON_AddStringToObject(result, "scope", "Mapped raw PAA bytes and their initialized headers only; shared retained C++ metadata charge, excludes kernel/allocator/RSS cost; counters alone do not prove worker jobs, upload readiness or work moved");
            return HarnessProtocol::JsonResponse(result);
        });
    hs.RegisterCommand({"shared_mesh_lifetime_test", "Test-only private retained mesh and two CPU borrowers; no world-object resolution", {}},
        [](const std::string&, cJSON* root) -> std::string {
            const char* enabled = std::getenv("WGR_SHARED_MESH_LIFETIME_FIXTURE");
            if (!enabled || std::strcmp(enabled, "1") || !Foundation::IsMainThread() || !GEngine || !GWorld ||
                GWorld->GetMode() == GModeNetware)
                return HarnessProtocol::ErrorResponse("requires owner-thread singleplayer and WGR_SHARED_MESH_LIFETIME_FIXTURE=1");
            const char* name = HarnessProtocol::GetString(root, "action");
            if (!name) return HarnessProtocol::ErrorResponse("fixture action required");
            Engine::SharedMeshFixtureAction action;
            if (!std::strcmp(name, "begin")) action = Engine::SharedMeshFixtureAction::Begin;
            else if (!std::strcmp(name, "beginStandalone")) action = Engine::SharedMeshFixtureAction::BeginStandalone;
            else if (!std::strcmp(name, "dropNewest")) action = Engine::SharedMeshFixtureAction::DropNewest;
            else if (!std::strcmp(name, "dropProducer")) action = Engine::SharedMeshFixtureAction::ReleaseProducer;
            else if (!std::strcmp(name, "dropOlder")) action = Engine::SharedMeshFixtureAction::DropOldest;
            else if (!std::strcmp(name, "observe")) action = Engine::SharedMeshFixtureAction::Observe;
            else if (!std::strcmp(name, "status")) action = Engine::SharedMeshFixtureAction::Snapshot;
            else if (!std::strcmp(name, "abort")) action = Engine::SharedMeshFixtureAction::Abort;
            else return HarnessProtocol::ErrorResponse("unknown fixture action");
            const Shape* source = nullptr;
            if (action == Engine::SharedMeshFixtureAction::Begin || action == Engine::SharedMeshFixtureAction::BeginStandalone) {
                // FindObject and Level are existing-resident lookups. Neither
                // resolves an authored placement nor initiates a shape load.
                Object* object = GLandscape ? GLandscape->FindObject(1001) : nullptr;
                const auto* shape = object ? object->GetShape() : nullptr;
                if (!object || !object->IsVisualResident() || !shape || shape->NLevels() < 1 || !(source = shape->Level(0)))
                    return HarnessProtocol::ErrorResponse("already visually resident original fixture object 1001 and CPU level 0 required");
            }
            const auto report = GEngine->ControlSharedMeshLifetimeFixture(action, source);
            const char* status = "Failed";
            switch (report.status) {
                case Engine::GeometryReportStatus::Unsupported: status = "Unsupported"; break;
                case Engine::GeometryReportStatus::Pending: status = "Pending"; break;
                case Engine::GeometryReportStatus::Busy: status = "Busy"; break;
                case Engine::GeometryReportStatus::Ready: status = "Ready"; break;
                case Engine::GeometryReportStatus::Invalid: status = "Invalid"; break;
                case Engine::GeometryReportStatus::Failed: break;
            }
            auto* result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "status", status);
            cJSON_AddNumberToObject(result, "requestId", double(report.requestId));
            cJSON_AddNumberToObject(result, "observedRequestId", double(report.observedRequestId));
            // Full-generation renderer identities must survive JSON transport.
            cJSON_AddStringToObject(result, "producer", std::to_string(report.producer).c_str());
            cJSON_AddStringToObject(result, "rendererHandle", std::to_string(report.rendererHandle).c_str());
            cJSON_AddNumberToObject(result, "knownPayloadBytes", double(report.knownPayloadBytes));
            cJSON_AddNumberToObject(result, "meshState", report.meshState);
            cJSON_AddNumberToObject(result, "cpuBorrowers", report.cpuBorrowers);
            cJSON_AddBoolToObject(result, "producerOwned", report.producerOwned);
            cJSON_AddBoolToObject(result, "sourceCopied", report.sourceCopied);
            cJSON_AddBoolToObject(result, "privateAllocation", report.privateAllocation);
            cJSON_AddBoolToObject(result, "ledgerEnabled", report.ledgerEnabled);
            cJSON_AddBoolToObject(result, "ledgerRecordObserved", report.ledgerRecordObserved);
            cJSON_AddBoolToObject(result, "ledgerScheduledRetired", report.ledgerScheduledRetired);
            cJSON_AddNumberToObject(result, "ledgerCpuBorrowers", double(report.ledgerCpuBorrowers));
            cJSON_AddBoolToObject(result, "ledgerProducerOwned", report.ledgerProducerOwned);
            cJSON_AddBoolToObject(result, "standalone", report.standalone);
            cJSON_AddNumberToObject(result, "witnessKnownMetadataBytes", double(report.witnessKnownMetadataBytes));
            cJSON_AddNumberToObject(result, "witnessPeakKnownMetadataBytes", double(report.witnessPeakKnownMetadataBytes));
            cJSON_AddNumberToObject(result, "witnessCapacityRefused", double(report.witnessCapacityRefused));
            cJSON_AddStringToObject(result, "scope", "Private copied mesh lifetime and generational record state; no world rendering, queue-completion or device-free proof");
            return HarnessProtocol::JsonResponse(result);
        });
    hs.RegisterCommand({"geometry_allocations", "Request/poll bounded retained mesh allocation diagnostics", {}},
        [](const std::string&, cJSON* root) -> std::string {
            if (!GEngine) return HarnessProtocol::ErrorResponse("no renderer");
            uint64_t requestId = 0;
            Engine::GeometryAllocationReport report;
            Engine::GeometryReportStatus status;
            const auto* id = cJSON_GetObjectItemCaseSensitive(root, "requestId");
            if (id) {
                if (!cJSON_IsNumber(id) || !std::isfinite(id->valuedouble) ||
                    id->valuedouble < 1 || id->valuedouble > 9007199254740991.0 ||
                    std::floor(id->valuedouble) != id->valuedouble)
                    return HarnessProtocol::ErrorResponse("positive exact integer requestId required");
                requestId = static_cast<uint64_t>(id->valuedouble);
                status = GEngine->PollGeometryAllocationReport(requestId, report);
            } else {
                uint32_t limits[] = {256, 2048, 65536};
                const char* keys[] = {"maxModels", "maxRows", "maxSectionVisits"};
                for (int i = 0; i < 3; ++i) {
                    const auto* value = cJSON_GetObjectItemCaseSensitive(root, keys[i]);
                    if (!value) continue;
                    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
                        value->valuedouble < 1 || value->valuedouble > limits[i] ||
                        std::floor(value->valuedouble) != value->valuedouble)
                        return HarnessProtocol::ErrorResponse("integer report limits outside supported bounds");
                    limits[i] = static_cast<uint32_t>(value->valuedouble);
                }
                status = GEngine->RequestGeometryAllocationReport(limits[0], limits[1], limits[2], requestId);
            }
            const char* name = "Failed";
            switch (status) {
                case Engine::GeometryReportStatus::Unsupported: name = "Unsupported"; break;
                case Engine::GeometryReportStatus::Pending: name = "Pending"; break;
                case Engine::GeometryReportStatus::Busy: name = "Busy"; break;
                case Engine::GeometryReportStatus::Ready: name = "Ready"; break;
                case Engine::GeometryReportStatus::Invalid: name = "Invalid"; break;
                case Engine::GeometryReportStatus::Failed: break;
            }
            auto* result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "status", name);
            cJSON_AddNumberToObject(result, "requestId", static_cast<double>(requestId));
            if (status == Engine::GeometryReportStatus::Ready) {
                cJSON_AddBoolToObject(result, "valid", report.valid);
                cJSON_AddBoolToObject(result, "truncated", report.truncated);
                cJSON_AddBoolToObject(result, "ownershipCoverageComplete", report.ownershipCoverageComplete);
                cJSON_AddStringToObject(result, "scope", "retained whole-mesh allocations; overlapping LOD rows; not selected usage or reclaimable memory");
#define GEOMETRY_REPORT_FIELD(field) cJSON_AddNumberToObject(result, #field, static_cast<double>(report.field))
                GEOMETRY_REPORT_FIELD(uniqueVertexBytes); GEOMETRY_REPORT_FIELD(uniqueIndexBytes);
                GEOMETRY_REPORT_FIELD(overlapWithinInspectedMeshes);
                GEOMETRY_REPORT_FIELD(overlapWithinInspectedVertexBytes); GEOMETRY_REPORT_FIELD(overlapWithinInspectedIndexBytes);
                GEOMETRY_REPORT_FIELD(poolLiveBytes); GEOMETRY_REPORT_FIELD(poolCapacityBytes); GEOMETRY_REPORT_FIELD(poolRetiredBytes);
                GEOMETRY_REPORT_FIELD(registryEntriesAtRequest); GEOMETRY_REPORT_FIELD(registryEntriesVisited); GEOMETRY_REPORT_FIELD(ineligibleEntriesSkipped); GEOMETRY_REPORT_FIELD(modelsRequested); GEOMETRY_REPORT_FIELD(modelsVisited);
                GEOMETRY_REPORT_FIELD(lodsInVisitedModels); GEOMETRY_REPORT_FIELD(sectionVisits); GEOMETRY_REPORT_FIELD(uniqueMeshes);
                GEOMETRY_REPORT_FIELD(missingModels); GEOMETRY_REPORT_FIELD(missingMeshes); GEOMETRY_REPORT_FIELD(invalidRanges); GEOMETRY_REPORT_FIELD(unresolvedNames);
                GEOMETRY_REPORT_FIELD(maxModels); GEOMETRY_REPORT_FIELD(maxRows); GEOMETRY_REPORT_FIELD(maxSectionVisits);
                cJSON_AddBoolToObject(result, "persistentAttributionEnabled", report.persistentAttributionEnabled);
                cJSON_AddBoolToObject(result, "persistentAttributionComplete", report.persistentAttributionComplete);
                cJSON_AddStringToObject(result, "persistentAttributionScope", "Producer/model/CPU-owner facts at a drained event cut; bytes are submitted-geometry estimates; draw, cached-view, in-flight and actual GPU retirement coverage incomplete");
                GEOMETRY_REPORT_FIELD(persistentAttributionEpoch); GEOMETRY_REPORT_FIELD(persistentIncompleteReasons);
                GEOMETRY_REPORT_FIELD(producerEstimatedVertexBytes); GEOMETRY_REPORT_FIELD(producerEstimatedIndexBytes);
                GEOMETRY_REPORT_FIELD(persistentCpuReferencedVertexBytes); GEOMETRY_REPORT_FIELD(persistentCpuReferencedIndexBytes);
                GEOMETRY_REPORT_FIELD(persistentRetainedSharedCpuVertexBytes); GEOMETRY_REPORT_FIELD(persistentRetainedSharedCpuIndexBytes);
                GEOMETRY_REPORT_FIELD(persistentStandaloneCpuVertexBytes); GEOMETRY_REPORT_FIELD(persistentStandaloneCpuIndexBytes);
                GEOMETRY_REPORT_FIELD(persistentTrackedAllocations); GEOMETRY_REPORT_FIELD(persistentTrackedBorrowerRecords);
                GEOMETRY_REPORT_FIELD(persistentOwnerEstimatedBytes); GEOMETRY_REPORT_FIELD(persistentZeroOwnerEstimatedBytes);
                GEOMETRY_REPORT_FIELD(persistentCpuBorrowers); GEOMETRY_REPORT_FIELD(persistentModelLodLinks);
                GEOMETRY_REPORT_FIELD(persistentMailboxPending); GEOMETRY_REPORT_FIELD(persistentMailboxDropped);
                GEOMETRY_REPORT_FIELD(persistentCancelledCreates); GEOMETRY_REPORT_FIELD(persistentReclaimableBytes);
                GEOMETRY_REPORT_FIELD(persistentStandaloneWitnessLive); GEOMETRY_REPORT_FIELD(persistentStandaloneWitnessKnownBytes);
                GEOMETRY_REPORT_FIELD(persistentStandaloneWitnessPeakKnownBytes); GEOMETRY_REPORT_FIELD(persistentStandaloneWitnessRefused);
                cJSON_AddBoolToObject(result, "rendererMeshFactsEnabled", report.rendererMeshFactsEnabled);
                cJSON_AddBoolToObject(result, "rendererMeshFactsValid", report.rendererMeshFactsValid);
                cJSON_AddBoolToObject(result, "rendererMeshFactsComplete", report.rendererMeshFactsComplete);
                cJSON_AddBoolToObject(result, "rendererMeshFactsTruncated", report.rendererMeshFactsTruncated);
                cJSON_AddStringToObject(result, "rendererMeshFactsScope", "Generational renderer records and exact source pool range sizes at an owner cut; absence is record removal, not queue completion or device memory release; ownership coverage remains incomplete");
                GEOMETRY_REPORT_FIELD(rendererMeshFactCalls); GEOMETRY_REPORT_FIELD(rendererMeshFactEpoch);
                GEOMETRY_REPORT_FIELD(rendererMeshFactsRequested); GEOMETRY_REPORT_FIELD(rendererMeshFactsInspected);
                GEOMETRY_REPORT_FIELD(rendererMeshFactsPresent); GEOMETRY_REPORT_FIELD(rendererMeshFactsAbsent);
                GEOMETRY_REPORT_FIELD(rendererMeshFactsInvalid); GEOMETRY_REPORT_FIELD(rendererMeshFactsDuplicateHandles);
                GEOMETRY_REPORT_FIELD(rendererMeshFactsUnknownMappings); GEOMETRY_REPORT_FIELD(rendererMeshFactsScheduledRetired);
                GEOMETRY_REPORT_FIELD(rendererMeshFactsPresentScheduledRetired); GEOMETRY_REPORT_FIELD(rendererMeshFactsAbsentScheduledRetired);
                GEOMETRY_REPORT_FIELD(rendererMeshFactsAbsentWithPersistentOwners);
                GEOMETRY_REPORT_FIELD(rendererMeshFactVertexBytes); GEOMETRY_REPORT_FIELD(rendererMeshFactIndexBytes);
                cJSON_AddBoolToObject(result, "rendererMeshRecordScopeValid", report.rendererMeshRecordScopeValid);
                cJSON_AddBoolToObject(result, "rendererMeshRecordClosureComplete", report.rendererMeshRecordClosureComplete);
                cJSON_AddBoolToObject(result, "rendererMeshPoolResidualValid", report.rendererMeshPoolResidualValid);
                GEOMETRY_REPORT_FIELD(rendererMeshLiveRecords); GEOMETRY_REPORT_FIELD(rendererMeshRecordPoolGeneration);
                GEOMETRY_REPORT_FIELD(rendererMeshRecordPoolLiveBytes); GEOMETRY_REPORT_FIELD(rendererMeshPoolUnattributedBytes);
                cJSON_AddStringToObject(result, "rendererMeshRecordScope", "Current generational mesh records and logical pool source-range payload at the same owner cut; excludes ownership, uploader references, retired-buffer capacity and physical GPU memory");
                cJSON_AddBoolToObject(result, "rendererRegisteredRefsEnabled", report.rendererRegisteredRefsEnabled);
                cJSON_AddBoolToObject(result, "rendererRegisteredRefsValid", report.rendererRegisteredRefsValid);
                cJSON_AddBoolToObject(result, "rendererRegisteredRefsComplete", report.rendererRegisteredRefsComplete);
                cJSON_AddStringToObject(result, "rendererRegisteredRefsScope", "Bounded current registered model/LOD section census for requested full-generation handles; sample rows max8; excludes uploader, caller frame arrays, skin/bake and in-flight work; zero references do not prove safe reclamation");
                GEOMETRY_REPORT_FIELD(rendererRegisteredRefsCalls); GEOMETRY_REPORT_FIELD(rendererRegisteredRefsRows);
                GEOMETRY_REPORT_FIELD(rendererRegisteredModelsVisited); GEOMETRY_REPORT_FIELD(rendererRegisteredLodsVisited);
                GEOMETRY_REPORT_FIELD(rendererRegisteredSectionsVisited); GEOMETRY_REPORT_FIELD(rendererRegisteredMissingRecords);
                GEOMETRY_REPORT_FIELD(rendererRegisteredReferencedMeshes); GEOMETRY_REPORT_FIELD(rendererRegisteredModelRefs);
                GEOMETRY_REPORT_FIELD(rendererRegisteredModelLodRefs); GEOMETRY_REPORT_FIELD(rendererRegisteredSectionOccurrences);
                GEOMETRY_REPORT_FIELD(rendererRegisteredRefusalFlags);
                auto* referenceSamples = cJSON_AddArrayToObject(result, "rendererRegisteredRefSamples");
                for (const auto& row : report.rendererRegisteredRefSamples) {
                    auto* item = cJSON_CreateObject();
                    // Preserve the full handle even above JSON's exact number range.
                    cJSON_AddStringToObject(item, "mesh", std::to_string(row.mesh).c_str());
                    cJSON_AddNumberToObject(item, "models", double(row.models));
                    cJSON_AddNumberToObject(item, "modelLods", double(row.modelLods));
                    cJSON_AddNumberToObject(item, "sections", double(row.sections));
                    cJSON_AddNumberToObject(item, "flags", row.flags);
                    cJSON_AddItemToArray(referenceSamples, item);
                }
                cJSON_AddBoolToObject(result, "rendererMeshAckEnabled", report.rendererMeshAckEnabled);
                cJSON_AddBoolToObject(result, "rendererMeshAckValid", report.rendererMeshAckValid);
                cJSON_AddStringToObject(result, "rendererMeshAckScope", "Immutable mesh-record cut followed by completion of queue work preceding the labelled main submission; no future-reference, device-free or shared-pool reclamation proof");
                GEOMETRY_REPORT_FIELD(rendererMeshAckTicket); GEOMETRY_REPORT_FIELD(rendererMeshAckEpoch);
                GEOMETRY_REPORT_FIELD(rendererMeshAckPoolGeneration); GEOMETRY_REPORT_FIELD(rendererMeshAckSubmission);
                GEOMETRY_REPORT_FIELD(rendererMeshAckRequested); GEOMETRY_REPORT_FIELD(rendererMeshAckPresent); GEOMETRY_REPORT_FIELD(rendererMeshAckAbsent);
                GEOMETRY_REPORT_FIELD(rendererMeshAckVertexBytes); GEOMETRY_REPORT_FIELD(rendererMeshAckIndexBytes);
                GEOMETRY_REPORT_FIELD(rendererMeshAckPoolLiveBytes); GEOMETRY_REPORT_FIELD(rendererMeshAckPoolCapacityBytes); GEOMETRY_REPORT_FIELD(rendererMeshAckPoolRetiredBytes);
                GEOMETRY_REPORT_FIELD(rendererMeshAckState);
#undef GEOMETRY_REPORT_FIELD
                auto* rows = cJSON_AddArrayToObject(result, "rows");
                for (const auto& row : report.rows) {
                    auto* item = cJSON_CreateObject();
                    cJSON_AddStringToObject(item, "asset", row.asset.c_str());
#define GEOMETRY_ROW_FIELD(field) cJSON_AddNumberToObject(item, #field, static_cast<double>(row.field))
                    GEOMETRY_ROW_FIELD(producerModel); GEOMETRY_ROW_FIELD(rendererModel); GEOMETRY_ROW_FIELD(lodIndex);
                    GEOMETRY_ROW_FIELD(resolution); GEOMETRY_ROW_FIELD(vertexBytes); GEOMETRY_ROW_FIELD(indexBytes);
                    GEOMETRY_ROW_FIELD(sections); GEOMETRY_ROW_FIELD(liveMeshes); GEOMETRY_ROW_FIELD(missingMeshes);
#undef GEOMETRY_ROW_FIELD
                    cJSON_AddItemToArray(rows, item);
                }
            }
            return HarnessProtocol::JsonResponse(result);
        });
    hs.RegisterCommand({"stream_existing_damage", "Test-only mutation of an already installed object; never resolves or pins", {}},
        [](const std::string&, cJSON* root) -> std::string {
            const auto* enabled = std::getenv("WGR_SIMULATION_RESIDENCY_TEST");
            if (!enabled || std::strcmp(enabled, "1") || !GLandscape || !GLandscape->ModernSimulationResidencyEnabled())
                return HarnessProtocol::ErrorResponse("requires enabled simulation residency and WGR_SIMULATION_RESIDENCY_TEST=1");
            const auto* id = cJSON_GetObjectItemCaseSensitive(root, "id");
            const auto* damage = cJSON_GetObjectItemCaseSensitive(root, "damage");
            if (!cJSON_IsNumber(id) || !std::isfinite(id->valuedouble) || id->valuedouble < 0 ||
                id->valuedouble > INT_MAX || std::floor(id->valuedouble) != id->valuedouble ||
                !cJSON_IsNumber(damage) || !std::isfinite(damage->valuedouble) ||
                damage->valuedouble <= 0 || damage->valuedouble >= 1)
                return HarnessProtocol::ErrorResponse("integer existing ID and non-destructive damage in (0,1) required");
            auto* object = GLandscape->FindObject(id->valueint);
            if (!object || !object->Static() || object->IsVisualResident())
                return HarnessProtocol::ErrorResponse("existing logical-only static object required");
            object->SetDammage(static_cast<float>(damage->valuedouble));
            auto* result = cJSON_CreateObject();
            cJSON_AddNumberToObject(result, "rawDamage", object->GetRawTotalDammage());
            cJSON_AddBoolToObject(result, "mustBeSaved", object->MustBeSaved());
            return HarnessProtocol::JsonResponse(result);
        });
    hs.RegisterCommand({"stream_existing_move", "Test-only move of an existing logical static object; never resolves or pins", {}},
        [](const std::string&, cJSON* root) -> std::string {
            const auto* enabled = std::getenv("WGR_SIMULATION_RESIDENCY_TEST");
            if (!enabled || std::strcmp(enabled, "1") || !GLandscape || !GLandscape->ModernSimulationResidencyEnabled())
                return HarnessProtocol::ErrorResponse("requires enabled simulation residency and WGR_SIMULATION_RESIDENCY_TEST=1");
            const auto* id = cJSON_GetObjectItemCaseSensitive(root, "id");
            const auto* coords = cJSON_GetObjectItemCaseSensitive(root, "position");
            if (!cJSON_IsNumber(id) || !std::isfinite(id->valuedouble) || id->valuedouble < 0 ||
                id->valuedouble > INT_MAX || std::floor(id->valuedouble) != id->valuedouble ||
                !cJSON_IsArray(coords) || cJSON_GetArraySize(coords) != 3)
                return HarnessProtocol::ErrorResponse("integer existing ID and [x,y,z] position required");
            Vector3 position;
            for (int axis = 0; axis < 3; ++axis) {
                const auto* value = cJSON_GetArrayItem(coords, axis);
                if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) || std::abs(value->valuedouble) > 1000000)
                    return HarnessProtocol::ErrorResponse("finite bounded position required");
                position[axis] = static_cast<float>(value->valuedouble);
            }
            auto* object = GLandscape->FindObject(id->valueint);
            if (!object || !object->Static() || object->IsVisualResident())
                return HarnessProtocol::ErrorResponse("existing logical-only static object required");
            object->Move(position);
            return HarnessProtocol::OkResponse();
        });
    hs.RegisterCommand({"stream_residency", "Read residency; optional experimental test budget", {}},
        [](const std::string&, cJSON* root) -> std::string {
            if (!GLandscape) return HarnessProtocol::ErrorResponse("no landscape");
            if (const auto* budget = cJSON_GetObjectItemCaseSensitive(root, "budget")) {
                if (!cJSON_IsNumber(budget) || !std::isfinite(budget->valuedouble) ||
                    budget->valuedouble < 2000 || budget->valuedouble > 20000 ||
                    std::floor(budget->valuedouble) != budget->valuedouble ||
                    !GLandscape->SetObjectStreamTestBudget(static_cast<uint32_t>(budget->valuedouble)))
                    return HarnessProtocol::ErrorResponse("requires streamed world, WGR_OBJECT_STREAM_TEST_BUDGET=1 and integer budget 2000..20000");
            }
            ObjectStreamResidencyCounters state;
            GLandscape->SnapshotObjectStreamDiag(state);
            auto* result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "valid", state.valid);
            cJSON_AddNumberToObject(result, "readyPayloadBytes", static_cast<double>(state.prep.readyPayloadBytes));
            cJSON_AddNumberToObject(result, "parseReservedBytes", static_cast<double>(state.prep.parseReservedBytes));
            cJSON_AddNumberToObject(result, "conversionReservedBytes", static_cast<double>(state.prep.conversionReservedBytes));
            cJSON_AddNumberToObject(result, "radiusCertificateBytes", static_cast<double>(state.prep.radiusCertificateBytes));
            cJSON_AddNumberToObject(result, "sourceEnvelopeBytes", static_cast<double>(state.prep.sourceEnvelopeBytes));
            cJSON_AddNumberToObject(result, "sourceEnvelopeScans", static_cast<double>(state.prep.sourceEnvelopeScans));
            cJSON_AddNumberToObject(result, "sourceEnvelopePublished", static_cast<double>(state.prep.sourceEnvelopePublished));
            cJSON_AddNumberToObject(result, "sourceEnvelopeMs", state.prep.sourceEnvelopeMs);
            cJSON_AddNumberToObject(result, "bc3PolicyBytes", static_cast<double>(state.prep.bc3PolicyBytes));
            cJSON_AddNumberToObject(result, "textureEncodeReservedBytes", static_cast<double>(state.prep.textureEncodeReservedBytes));
            cJSON_AddNumberToObject(result, "peakTextureEncodeReservedBytes", static_cast<double>(state.prep.peakTextureEncodeReservedBytes));
            cJSON_AddNumberToObject(result, "workerBc3Prepared", static_cast<double>(state.prep.workerBc3Prepared));
            cJSON_AddNumberToObject(result, "workerBc3BudgetSkipped", static_cast<double>(state.prep.workerBc3BudgetSkipped));
            cJSON_AddNumberToObject(result, "workerBc3Ms", state.prep.workerBc3Ms);
            cJSON_AddNumberToObject(result, "ddsPublicationBytes", static_cast<double>(state.prep.ddsPublicationBytes));
            cJSON_AddNumberToObject(result, "texturePublicationBytes", static_cast<double>(state.prep.texturePublicationBytes));
            cJSON_AddBoolToObject(result, "warmTextureJobsEnabled", ObjectStreamPreparer::WarmTextureJobsEnabled());
#define WARM_PREP_FIELD(field) cJSON_AddNumberToObject(result, #field, static_cast<double>(state.prep.field))
            WARM_PREP_FIELD(warmTextureJobsSubmitted); WARM_PREP_FIELD(warmTextureJobsCompleted);
            WARM_PREP_FIELD(warmTextureJobsCancelled); WARM_PREP_FIELD(warmTextureJobsCoalesced);
            WARM_PREP_FIELD(warmTextureJobsRejected); WARM_PREP_FIELD(warmTextureMembersPrepared);
            WARM_PREP_FIELD(warmTextureMembersSkipped); WARM_PREP_FIELD(warmTextureMembersUnreadable);
            WARM_PREP_FIELD(warmTextureQueued); WARM_PREP_FIELD(warmTextureActive);
            WARM_PREP_FIELD(warmTextureMetadataBytes); WARM_PREP_FIELD(warmTextureMetadataBytesPeak);
            WARM_PREP_FIELD(warmTextureScratchBytes); WARM_PREP_FIELD(warmTextureScratchBytesPeak);
            WARM_PREP_FIELD(warmTextureQueueBytes);
            WARM_PREP_FIELD(warmTextureMs);
#undef WARM_PREP_FIELD
            const auto texturePrep = render::PreparedTextureStore::Instance().SnapshotStats();
            cJSON_AddNumberToObject(result, "warmPreparedPuts", static_cast<double>(texturePrep.warmPuts));
            cJSON_AddNumberToObject(result, "warmPreparedTakes", static_cast<double>(texturePrep.warmTakes));
            cJSON_AddNumberToObject(result, "warmPreparedSourceRefused", static_cast<double>(texturePrep.warmSourceRefused));
            cJSON_AddNumberToObject(result, "warmPreparedUploads", static_cast<double>(texturePrep.warmUploads));
            cJSON_AddNumberToObject(result, "warmPreparedUploadBytes", static_cast<double>(texturePrep.warmUploadBytes));
            cJSON_AddNumberToObject(result, "warmPreparedUploadFailed", static_cast<double>(texturePrep.warmUploadFailed));
            cJSON_AddNumberToObject(result, "preparedTextureEntries", static_cast<double>(texturePrep.entries));
              cJSON_AddNumberToObject(result, "preparedTextureCapacityBytes", static_cast<double>(texturePrep.bytes));
              cJSON_AddNumberToObject(result, "preparedAlphaFactsPuts", static_cast<double>(texturePrep.alphaFactsPuts));
              cJSON_AddNumberToObject(result, "preparedAlphaFactsHits", static_cast<double>(texturePrep.alphaFactsHits));
              cJSON_AddNumberToObject(result, "preparedAlphaFactsMisses", static_cast<double>(texturePrep.alphaFactsMisses));
              cJSON_AddNumberToObject(result, "preparedAlphaFactsCancelled", static_cast<double>(texturePrep.alphaFactsCancelled));
              cJSON_AddNumberToObject(result, "preparedAlphaFactsCapacitySkipped", static_cast<double>(texturePrep.alphaFactsCapacitySkipped));
              cJSON_AddNumberToObject(result, "preparedPaaCancelled", static_cast<double>(texturePrep.paaCancelled));
            cJSON_AddNumberToObject(result, "preparedDdsPuts", static_cast<double>(texturePrep.ddsPreparedPuts));
            cJSON_AddNumberToObject(result, "preparedDdsTakes", static_cast<double>(texturePrep.ddsPreparedTakes));
            cJSON_AddNumberToObject(result, "preparedDdsConfigurationMisses", static_cast<double>(texturePrep.ddsConfigurationMisses));
            cJSON_AddNumberToObject(result, "preparedDdsCancelled", static_cast<double>(texturePrep.ddsCancelled));
            cJSON_AddNumberToObject(result, "peakConversionReservedBytes", static_cast<double>(state.prep.peakConversionReservedBytes));
            cJSON_AddNumberToObject(result, "peakReadyPayloadBytes", static_cast<double>(state.prep.peakReadyPayloadBytes));
            cJSON_AddNumberToObject(result, "readyByteParks", static_cast<double>(state.prep.readyByteParks));
            cJSON_AddNumberToObject(result, "readyByteBudget", static_cast<double>(state.prep.readyByteBudget));
            cJSON_AddNumberToObject(result, "resident", state.residentObjects);
            cJSON_AddNumberToObject(result, "required", state.requiredObjects);
            cJSON_AddNumberToObject(result, "logicalOnly", state.logicalOnlyObjects);
            cJSON_AddNumberToObject(result, "budget", state.budget);
            cJSON_AddNumberToObject(result, "desired", GLandscape->ObjectStreamDesiredCount());
            cJSON_AddBoolToObject(result, "pending", GLandscape->ObjectStreamPending());
            cJSON_AddNumberToObject(result, "centerX", GLandscape->ObjectStreamCenterX());
            cJSON_AddNumberToObject(result, "centerZ", GLandscape->ObjectStreamCenterZ());
            cJSON_AddNumberToObject(result, "admitUpdates", static_cast<double>(state.admitUpdates));
            cJSON_AddNumberToObject(result, "lastObjectId", GLandscape->GetLastObjectID());
            Engine::GpuMemoryStatsOut gpu{};
            const bool gpuValid = GEngine && GEngine->GetGpuMemoryStats(gpu);
            cJSON_AddBoolToObject(result, "gpuMemoryValid", gpuValid);
            if (gpuValid)
            {
                cJSON_AddNumberToObject(result, "geometryLiveBytes", static_cast<double>(gpu.geometryLiveBytes));
                cJSON_AddNumberToObject(result, "geometryCapacityBytes", static_cast<double>(gpu.geometryCapacityBytes));
                cJSON_AddNumberToObject(result, "geometryRetiredBytes", static_cast<double>(gpu.geometryRetiredBytes));
                cJSON_AddNumberToObject(result, "objectTextureBytes", static_cast<double>(gpu.objectTextureBytes));
                cJSON_AddNumberToObject(result, "objectTextureCount", gpu.objectTextureCount);
                cJSON_AddNumberToObject(result, "backendAllocationBytes", static_cast<double>(gpu.backendAllocationBytes));
            }
            return HarnessProtocol::JsonResponse(result);
        });
    hs.RegisterCommand({"weather_visibility", "Read-only weather, terrain and object draw ranges", {}},
        [](const std::string&, cJSON*) -> std::string {
            if (!GScene || !GLandscape) return HarnessProtocol::ErrorResponse("no world");
            auto* result = cJSON_CreateObject();
            cJSON_AddNumberToObject(result, "rain", GLandscape->GetRainDensity());
            cJSON_AddNumberToObject(result, "fog", GLandscape->GetFog());
            cJSON_AddNumberToObject(result, "terrainRange", GScene->GetFogMaxRange());
            cJSON_AddNumberToObject(result, "baseRange", GScene->GetBaseFogMaxRange());
            cJSON_AddNumberToObject(result, "objectRange", GScene->GetObjectDrawDistance());
            cJSON_AddNumberToObject(result, "tacticalVisibility", GScene->GetTacticalVisibility());
            cJSON_AddNumberToObject(result, "selectedRange", ENGINE_CONFIG.tacticalZ);
            return HarnessProtocol::JsonResponse(result);
        });
    hs.RegisterCommand({"water_bathymetry", "Read a bounded terrain-height grid for the ocean fixture", {}},
        [](const std::string&, cJSON* root) -> std::string
        {
            if (!GLandscape) return HarnessProtocol::ErrorResponse("no landscape");
            const auto* x = cJSON_GetObjectItemCaseSensitive(root, "x");
            const auto* z = cJSON_GetObjectItemCaseSensitive(root, "z");
            if (!cJSON_IsNumber(x) || !cJSON_IsNumber(z) || !std::isfinite(x->valuedouble) || !std::isfinite(z->valuedouble))
                return HarnessProtocol::ErrorResponse("finite x/z required");
            auto* result = cJSON_CreateObject();
            auto* rows = cJSON_AddArrayToObject(result, "samples");
            for (int j = -8; j <= 8; ++j)
                for (int i = -8; i <= 8; ++i)
                {
                    const float px = static_cast<float>(x->valuedouble) + i * 16.0f;
                    const float pz = static_cast<float>(z->valuedouble) + j * 16.0f;
                    auto* sample = cJSON_CreateArray();
                    cJSON_AddItemToArray(sample, cJSON_CreateNumber(px));
                    cJSON_AddItemToArray(sample, cJSON_CreateNumber(pz));
                    cJSON_AddItemToArray(sample, cJSON_CreateNumber(GLandscape->SurfaceY(px, pz)));
                    cJSON_AddItemToArray(rows, sample);
                }
            return HarnessProtocol::JsonResponse(result);
        });
    hs.RegisterCommand({"water_probe", "Read or set existing renderer-only water diagnostic controls", {}},
        [](const std::string&, cJSON* root) -> std::string {
            if (!GEngine || !GEngine->SupportsWater()) return HarnessProtocol::ErrorResponse("GPU water unavailable");
            auto settings = GEngine->GetWaterSettings();
            bool changed = false;
            const auto number = [&](const char* key, float& value, float low, float high) {
                const auto* item = cJSON_GetObjectItemCaseSensitive(root, key);
                if (!item) return true;
                if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
                    item->valuedouble < low || item->valuedouble > high) return false;
                value = float(item->valuedouble); changed = true; return true;
            };
            if (!number("scale", settings.waveScale, 0.25f, 4.0f) ||
                !number("shoreGain", settings.shoreWaveGain, 0.0f, 2.0f) ||
                !number("fixedTime", settings.freeze.fixedTime, 0.0f, 100000.0f))
                return HarnessProtocol::ErrorResponse("water probe value out of range");
            const auto flag = [&](const char* key, bool& value) {
                if (const auto* item = cJSON_GetObjectItemCaseSensitive(root, key)) {
                    if (!cJSON_IsBool(item)) return false;
                    value = cJSON_IsTrue(item); changed = true;
                }
                return true;
            };
            if (!flag("freezeTime", settings.freeze.freezeTime) ||
                !flag("freezeFoam", settings.freeze.freezeFoam))
                return HarnessProtocol::ErrorResponse("water probe expects boolean freezes");
            if (changed) GEngine->SetWaterSettings(settings);
            auto* response = cJSON_CreateObject();
            cJSON_AddNumberToObject(response,"scale",settings.waveScale);
            cJSON_AddNumberToObject(response,"amplitude",settings.waveAmp);
            cJSON_AddNumberToObject(response,"warp",settings.warpAmp);
            cJSON_AddNumberToObject(response,"shoreGain",settings.shoreWaveGain);
            cJSON_AddNumberToObject(response,"preset",settings.cascadePreset);
            cJSON_AddNumberToObject(response,"fftResolution",settings.fftResolution);
            cJSON_AddNumberToObject(response,"geometryQuality",settings.geometryQuality);
            cJSON_AddNumberToObject(response,"fixedTime",settings.freeze.fixedTime);
            cJSON_AddBoolToObject(response,"freezeTime",settings.freeze.freezeTime);
            cJSON_AddBoolToObject(response,"freezeFoam",settings.freeze.freezeFoam);
            return HarnessProtocol::JsonResponse(response);
        });
    hs.RegisterCommand({"screenshot", "Capture screenshot to file", {{"path", "string", true}}},
                       [](const std::string&, cJSON* root) -> std::string
                       {
                           const char* path = HarnessProtocol::GetString(root, "path");
                           if (!path || !path[0])
                               return HarnessProtocol::ErrorResponse("screenshot requires path");
                           if (!GEngine)
                               return HarnessProtocol::ErrorResponse("no engine available");
                           GEngine->Screenshot(path);
                           cJSON* resp = cJSON_CreateObject();
                           cJSON_AddStringToObject(resp, "path", path);
                           return HarnessProtocol::JsonResponse(resp);
                       });
}

// Session-scoped local variable space shared across all eval/exec calls for
// the lifetime of the process (each test runs in its own game process, so
// there is no cross-test bleed).  Provides a local scope so that SQF using
// `private _x = value` and subsequent `_x` references work across eval calls.
static GameVarSpace s_evalScope;

void RegisterSqf(HarnessServer& hs)
{
    hs.RegisterCommand({"stream_simulation_query", "Test-only independent fire residency channel; never a collision-clear result or pump", {}},
        [](const std::string&, cJSON* root) -> std::string {
            const auto* enabled = std::getenv("WGR_SIMULATION_RESIDENCY_TEST");
            if (!enabled || std::strcmp(enabled, "1") || !GLandscape || !GWorld || GWorld->GetMode() == GModeNetware)
                return HarnessProtocol::ErrorResponse("requires singleplayer and WGR_SIMULATION_RESIDENCY_TEST=1");
            const auto* actorName = HarnessProtocol::GetString(root, "actor");
            const auto* action = HarnessProtocol::GetString(root, "action");
            if (!actorName || !actorName[0] || std::strlen(actorName) > 64 || !action ||
                (std::strcmp(action, "queue") && std::strcmp(action, "status") && std::strcmp(action, "release") && std::strcmp(action, "collide")))
                return HarnessProtocol::ErrorResponse("actor variable and queue/status/release/collide required");
            for (const char* p = actorName; *p; ++p)
                if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                    (*p >= '0' && *p <= '9') || *p == '_'))
                    return HarnessProtocol::ErrorResponse("actor must name one script variable");
            auto* gs = GWorld->GetGameState();
            gs->BeginContext(&s_evalScope);
            GameValue value = gs->EvaluateMultiple(actorName);
            gs->EndContext();
            Object* actor = value.GetType() == GameObject
                ? static_cast<GameDataObject*>(value.GetData())->GetObject().GetLink() : nullptr;
            if (!actor) return HarnessProtocol::ErrorResponse("existing actor required");
            const auto* purposeValue = cJSON_GetObjectItemCaseSensitive(root, "purpose");
            if (purposeValue && (!cJSON_IsNumber(purposeValue) || !std::isfinite(purposeValue->valuedouble) ||
                purposeValue->valuedouble < 0 || purposeValue->valuedouble > 255 ||
                std::floor(purposeValue->valuedouble) != purposeValue->valuedouble))
                return HarnessProtocol::ErrorResponse("purpose must be an integer 0..255");
            const auto purpose = static_cast<Streaming::SimulationQueryPurpose>(purposeValue ? purposeValue->valueint : 1);
            if (!std::strcmp(action, "release")) {
                GLandscape->ReleaseModernSimulationQuery(*actor, purpose);
                return HarnessProtocol::OkResponse();
            }
            Vector3 endpoints[2];
            const char* keys[] = {"from", "to"};
            for (int end = 0; end < 2; ++end) {
                const auto* coords = cJSON_GetObjectItemCaseSensitive(root, keys[end]);
                if (!cJSON_IsArray(coords) || cJSON_GetArraySize(coords) != 3)
                    return HarnessProtocol::ErrorResponse("from/to require [x,y,z]");
                for (int axis = 0; axis < 3; ++axis) {
                    const auto* item = cJSON_GetArrayItem(coords, axis);
                    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) || std::abs(item->valuedouble) > 1000000)
                        return HarnessProtocol::ErrorResponse("finite bounded endpoints required");
                    endpoints[end][axis] = static_cast<float>(item->valuedouble);
                }
            }
            const auto* radius = cJSON_GetObjectItemCaseSensitive(root, "radius");
            if (!cJSON_IsNumber(radius) || !std::isfinite(radius->valuedouble) || radius->valuedouble < 0 || radius->valuedouble > 1000)
                return HarnessProtocol::ErrorResponse("finite radius 0..1000 required");
            // Queue and trace the same engine-float radius. Otherwise a caller
            // can enqueue a narrower double domain than Intersect actually uses.
            const float engineRadius = static_cast<float>(radius->valuedouble);
            CollisionBuffer contacts;
            const auto status = !std::strcmp(action, "collide")
                ? (purpose == Streaming::SimulationQueryPurpose::Fire
                    ? GLandscape->TryModernSimulationFireCollision(contacts, *actor, nullptr, nullptr, endpoints[0], endpoints[1], engineRadius)
                    : Streaming::SimulationResidencyStatus::Invalid)
                : !std::strcmp(action, "queue")
                ? GLandscape->QueueModernSimulationQuery(*actor, purpose, endpoints[0], endpoints[1], double(engineRadius))
                : GLandscape->ModernSimulationQueryStatus(*actor, purpose, endpoints[0], endpoints[1], double(engineRadius));
            auto* result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "status", SimulationStatusName(status));
            cJSON_AddStringToObject(result, "scope", "static terrain-object residency; not collision clearance");
            if (!std::strcmp(action, "collide")) {
                auto* hits = cJSON_AddArrayToObject(result, "contacts");
                for (int i = 0; i < contacts.Size(); ++i) {
                    if (!contacts[i].object) continue;
                    auto* item = cJSON_CreateObject();
                    cJSON_AddNumberToObject(item, "id", contacts[i].object->ID());
                    cJSON_AddItemToArray(hits, item);
                }
            }
            return HarnessProtocol::JsonResponse(result);
        });
    hs.RegisterCommand({"ai_combat", "SP combat setting and passive audit fixture state", {{"improved", "int", false}}},
        [](const std::string&, cJSON* root) -> std::string {
            if (!GWorld || GWorld->GetMode() == GModeNetware)
                return HarnessProtocol::ErrorResponse("singleplayer required");
            const int mode = HarnessProtocol::GetInt(root, "improved", -1);
            if (mode == 0 || mode == 1) InfantryCombat::SetImproved(mode == 1);
            cJSON* resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "improved", InfantryCombat::Improved());
            GameState* gs = GWorld->GetGameState();
            gs->BeginContext(&s_evalScope);
            GameValue value = gs->EvaluateMultiple("auditShooter");
            gs->EndContext();
            Man* man = value.GetType() == GameObject
                ? dyn_cast<Man>(static_cast<GameDataObject*>(value.GetData())->GetObject().GetLink()) : nullptr;
            if (man)
            {
                cJSON_AddBoolToObject(resp, "active", man->ImprovedAutonomousCombat());
                cJSON_AddBoolToObject(resp, "hasTarget", man->GetFireTarget() != nullptr);
                cJSON_AddNumberToObject(resp, "selected", man->SelectedWeapon());
                cJSON_AddNumberToObject(resp, "laneYields", man->CombatLaneYields());
                cJSON_AddNumberToObject(resp, "repositions", man->CombatRepositions());
                cJSON_AddNumberToObject(resp, "repositionProbes", man->CombatRepositionProbes());
                cJSON_AddNumberToObject(resp, "pauseState", man->CombatPauseState());
                cJSON_AddBoolToObject(resp, "laneBlocked", man->CombatLaneBlocked());
                cJSON_AddStringToObject(resp, "gate", man->CombatLastGate());
                cJSON* slots = cJSON_AddArrayToObject(resp, "slots");
                for (int i = 0; i < man->NMagazineSlots(); ++i)
                {
                    const auto& slot = man->GetMagazineSlot(i);
                    const auto* mode = man->GetWeaponMode(i);
                    if (!slot._magazine || !mode || !mode->_ammo) continue;
                    cJSON* row = cJSON_CreateObject();
                    cJSON_AddNumberToObject(row, "slot", i);
                    cJSON_AddNumberToObject(row, "ammo", slot._magazine->_ammo);
                    cJSON_AddStringToObject(row, "name", slot._magazine->_type->GetName());
                    cJSON_AddNumberToObject(row, "speed", slot._magazine->_type->_initSpeed);
                    cJSON_AddNumberToObject(row, "minRange", mode->_ammo->minRange);
                    cJSON_AddNumberToObject(row, "maxRange", mode->_ammo->maxRange);
                    cJSON_AddNumberToObject(row, "blastRange", mode->_ammo->indirectHitRange);
                    cJSON_AddItemToArray(slots, row);
                }
            }
            return HarnessProtocol::JsonResponse(resp);
        });
    hs.RegisterCommand({"ai_cover_probe", "Explicit fixture-only eye/muzzle visibility samples", {}},
        [](const std::string&, cJSON*) -> std::string {
            if (!GWorld || !GLandscape || GWorld->GetMode() == GModeNetware)
                return HarnessProtocol::ErrorResponse("singleplayer world required");
            GameState* gs = GWorld->GetGameState();
            gs->BeginContext(&s_evalScope);
            GameValue sv = gs->EvaluateMultiple("auditShooter");
            GameValue tv = gs->EvaluateMultiple("auditTarget");
            GameValue wv = gs->EvaluateMultiple("auditWall");
            gs->EndContext();
            EntityAI* shooter = sv.GetType() == GameObject ? dyn_cast<EntityAI>(static_cast<GameDataObject*>(sv.GetData())->GetObject().GetLink()) : nullptr;
            EntityAI* target = tv.GetType() == GameObject ? dyn_cast<EntityAI>(static_cast<GameDataObject*>(tv.GetData())->GetObject().GetLink()) : nullptr;
            if (!shooter || !target) return HarnessProtocol::ErrorResponse("fixture objects required");
            cJSON* resp = cJSON_CreateObject();
            Object* wall = wv.GetType() == GameObject ? static_cast<GameDataObject*>(wv.GetData())->GetObject().GetLink() : nullptr;
            if (wall && wall->GetShape())
            {
                cJSON_AddStringToObject(resp, "wallShape", wall->GetShape()->GetName());
                cJSON_AddNumberToObject(resp, "wallX", wall->Position().X());
                cJSON_AddNumberToObject(resp, "wallZ", wall->Position().Z());
                cJSON_AddNumberToObject(resp, "wallY", wall->Position().Y());
                cJSON_AddNumberToObject(resp, "wallRadius", wall->GetRadius());
            }
            const Vector3 muzzle = shooter->PositionModelToWorld(shooter->GetWeaponPoint(shooter->SelectedWeapon()));
            const Vector3 aim = target->AimingPosition();
            const Vector3 head = target->CameraPosition();
            if (wall)
            {
                CollisionBuffer hits;
                wall->Intersect(hits, aim, muzzle, 0, ObjIntersectFire);
                cJSON_AddNumberToObject(resp, "wallAimHits", hits.Size());
                cJSON_AddBoolToObject(resp, "wallFireOcclusion", wall->OcclusionFire());
                cJSON_AddBoolToObject(resp, "targetIgnoresWall", target->IgnoreObstacle(wall, ObjIntersectFire));
            }
            cJSON_AddNumberToObject(resp, "preferred", GLandscape->Visible(muzzle, shooter, target, 0.9f, ObjIntersectFire));
            cJSON_AddNumberToObject(resp, "muzzleToHead", GLandscape->Visible(muzzle, head, 0.12f, shooter, target, ObjIntersectFire));
            cJSON_AddNumberToObject(resp, "eyeToHead", GLandscape->Visible(shooter->CameraPosition(), head, 0.12f, shooter, target, ObjIntersectView));
            cJSON_AddNumberToObject(resp, "aimHeight", aim.Y() - target->Position().Y());
            cJSON_AddNumberToObject(resp, "headHeight", head.Y() - target->Position().Y());
            return HarnessProtocol::JsonResponse(resp);
        });
    hs.RegisterCommand({"ai_stress_state", "Passive infantry state near player (no AI decision evaluation)", {{"recordShots", "int", false}}},
        [](const std::string&, cJSON* root) -> std::string {
            if (!GWorld || !GLandscape || !GWorld->PlayerOn() || GWorld->GetMode() == GModeNetware)
                return HarnessProtocol::ErrorResponse("singleplayer required");
            const int record = HarnessProtocol::GetInt(root, "recordShots", -1);
            if (record == 0 || record == 1)
            {
                Ballistics::SetEnabled(record == 1);
                Ballistics::SetDrawTrails(false);
                Ballistics::Store().SetCapacity(128);
            }
            cJSON* resp = cJSON_CreateObject();
            cJSON* rows = cJSON_AddArrayToObject(resp, "units");
            int xMin, xMax, zMin, zMax;
            const Vector3 player = GWorld->PlayerOn()->Position();
            ObjRadiusRectangle(xMin, xMax, zMin, zMax, player, player, 1200);
            for (int z = zMin; z <= zMax; ++z)
                for (int x = xMin; x <= xMax; ++x)
                {
                    const auto* list = GLandscape->GetObjects(z, x).GetList();
                    if (!list) continue;
                    for (int i = 0; i < list->GetTypeVehicleCount(); ++i)
                    {
                        const Man* man = dyn_cast<Man>(list->GetTypeVehicle(i));
                        if (!man || man->IsDammageDestroyed() || man == GWorld->PlayerOn()) continue;
                        cJSON* row = cJSON_CreateObject();
                        cJSON_AddStringToObject(row, "name", man->GetDebugName());
                        cJSON_AddNumberToObject(row, "speed", man->Speed().Size());
                        cJSON_AddNumberToObject(row, "wantedSpeed", man->WalkSpeedWanted());
                        cJSON_AddNumberToObject(row, "importance", man->GetLastImportance());
                        cJSON_AddNumberToObject(row, "pauseState", man->CombatPauseState());
                        cJSON_AddNumberToObject(row, "unitState", man->Brain() ? man->Brain()->GetState() : -1);
                        cJSON_AddNumberToObject(row, "pathSize", man->Brain() ? man->Brain()->GetPath().Size() : 0);
                        cJSON_AddBoolToObject(row, "laneBlocked", man->CombatLaneBlocked());
                        cJSON_AddNumberToObject(row, "laneYields", man->CombatLaneYields());
                        cJSON_AddNumberToObject(row, "repositions", man->CombatRepositions());
                        cJSON_AddNumberToObject(row, "repositionProbes", man->CombatRepositionProbes());
                        cJSON_AddStringToObject(row, "gate", man->CombatLastGate());
                        cJSON_AddNumberToObject(row, "distance", man->Position().Distance(player));
                        cJSON_AddNumberToObject(row, "height", man->Position().Y());
                        cJSON_AddStringToObject(row, "move", man->GetPrimaryMoveName());
                        cJSON_AddBoolToObject(row, "prepare", man->IsFirePrepare());
                        const auto* target = man->GetFireTarget();
                        cJSON_AddBoolToObject(row, "target", target != nullptr);
                        cJSON_AddBoolToObject(row, "playerTarget", target && target->idExact == GWorld->PlayerOn());
                        const int weapon = man->SelectedWeapon();
                        if (weapon >= 0 && weapon < man->NMagazineSlots())
                        {
                            cJSON_AddNumberToObject(row, "barrelY", man->GetWeaponDirection(weapon).Y());
                            const auto* mode = man->GetWeaponMode(weapon);
                            if (mode && mode->_ammo) cJSON_AddNumberToObject(row, "range", mode->_ammo->maxRange);
                        }
                        cJSON_AddItemToArray(rows, row);
                    }
                }
            cJSON* shots = cJSON_AddArrayToObject(resp, "shots");
            const auto& store = Ballistics::Store();
            for (int i = 0; i < store.Size(); ++i)
            {
                const auto& shot = store.At(i);
                cJSON* row = cJSON_CreateObject();
                cJSON_AddNumberToObject(row, "id", shot.id);
                cJSON_AddStringToObject(row, "ammo", shot.ammo.Get());
                cJSON_AddStringToObject(row, "shooter", shot.shooter.Get());
                cJSON_AddNumberToObject(row, "dirY", shot.dirY);
                cJSON_AddNumberToObject(row, "speed", shot.muzzleSpeed);
                cJSON_AddItemToArray(shots, row);
            }
            return HarnessProtocol::JsonResponse(resp);
        });
    hs.RegisterCommand({"crew_seat_status", "Read-only SP fixture seat lifecycle status", {}},
        [](const std::string&, cJSON*) -> std::string {
            if (!GWorld || GWorld->GetMode() == GModeNetware)
                return HarnessProtocol::ErrorResponse("singleplayer required");
            AIUnit* focus = GWorld->FocusOn();
            AIGroup* group = focus ? focus->GetGroup() : nullptr;
            AIUnit* unit = group ? group->UnitWithID(2) : nullptr;
            const Man* man = unit ? dyn_cast<Man>(unit->GetPerson()) : nullptr;
            cJSON* resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp,"present",man != nullptr);
            cJSON_AddBoolToObject(resp,"active",man && man->GetActiveCargoWeaponSeat());
            cJSON_AddBoolToObject(resp,"inCargo",unit && unit->IsInCargo());
            cJSON_AddBoolToObject(resp,"alive",man && !man->IsDammageDestroyed());
            return HarnessProtocol::JsonResponse(resp);
        });
    hs.RegisterCommand({"crew_take_control", "SP harness: take player control of the actual fixture passenger", {}},
        [](const std::string&, cJSON*) -> std::string {
            if (!GWorld || GWorld->GetMode() == GModeNetware || GWorld->GetCameraEffect())
                return HarnessProtocol::ErrorResponse("singleplayer without scripted camera required");
            AIUnit* focus = GWorld->FocusOn();
            AIGroup* group = focus ? focus->GetGroup() : nullptr;
            AIUnit* unit = group ? group->UnitWithID(2) : nullptr;
            Man* man = unit ? dyn_cast<Man>(unit->GetPerson()) : nullptr;
            if (!man || !unit->IsInCargo() || !man->GetHierachyParent() || !man->IsLocal())
                return HarnessProtocol::ErrorResponse("local seated fixture passenger required");
            GWorld->SwitchPlayerTo(man);
            GWorld->SetPlayerManual(true);
            GWorld->SwitchCameraTo(man, CamInternal);
            return HarnessProtocol::OkResponse();
        });
    hs.RegisterCommand({"crew_cursor", "SP harness: aim the normal UI cursor, without firing or changing weapon state",
                        {{"x","float",true},{"y","float",true},{"z","float",true}}},
        [](const std::string&, cJSON* root) -> std::string {
            if (!GWorld || GWorld->GetMode() == GModeNetware || !GWorld->UI())
                return HarnessProtocol::ErrorResponse("singleplayer UI required");
            const auto* man = dyn_cast<Man>(GWorld->PlayerOn());
            if (!man || !man->GetActiveCargoWeaponSeat())
                return HarnessProtocol::ErrorResponse("active player passenger required");
            const Vector3 dir(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(root,"x")),
                              cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(root,"y")),
                              cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(root,"z")));
            if (!std::isfinite(dir.SquareSize()) || dir.SquareSize() < .001f)
                return HarnessProtocol::ErrorResponse("finite nonzero direction required");
            GWorld->UI()->SetCursorMode(true);
            GWorld->UI()->SetCursorDirection(dir.Normalized());
            return HarnessProtocol::OkResponse();
        });
    hs.RegisterCommand({"crew_request_reload", "SP harness: request the existing cargo magazine reload action", {}},
                       [](const std::string&, cJSON*) -> std::string
                       {
                           if (!GWorld || GWorld->GetMode() == GModeNetware)
                               return HarnessProtocol::ErrorResponse("singleplayer world required");
                           AIUnit* focus = GWorld->FocusOn();
                           AIGroup* group = focus ? focus->GetGroup() : nullptr;
                           AIUnit* unit = group ? group->UnitWithID(2) : nullptr;
                           Man* man = unit ? dyn_cast<Man>(unit->GetPerson()) : nullptr;
                           const auto* seat = man ? man->GetActiveCargoWeaponSeat() : nullptr;
                           if (!seat || !seat->HasTransitions())
                               return HarnessProtocol::ErrorResponse("active transition-enabled cargo seat required");
                           const int weapon = man->SelectedWeapon();
                           if (weapon < 0 || weapon >= man->NMagazineSlots())
                               return HarnessProtocol::ErrorResponse("selected rifle required");
                           const auto& slot = man->GetMagazineSlot(weapon);
                           if (!man->IsCargoWeaponCandidate(weapon) || !slot._magazine)
                               return HarnessProtocol::ErrorResponse("configured rifle magazine required");
                           const int spare = man->FindMagazineByType(slot._muzzle, slot._magazine->_type);
                           if (spare < 0 || man->GetMagazine(spare) == slot._magazine || !man->ReloadMagazine(weapon, spare))
                               return HarnessProtocol::ErrorResponse("no spare magazine or reload refused");
                           cJSON* resp = cJSON_CreateObject();
                           cJSON_AddBoolToObject(resp, "reloadRequested", true);
                           return HarnessProtocol::JsonResponse(resp);
                       });
    hs.RegisterCommand({"crew_driver_limited", "Set the fixture driver's actual subgroup to LIMITED, not vehicle velocity", {}},
                       [](const std::string&, cJSON*) -> std::string
                       {
                           if (!GWorld || GWorld->GetMode() == GModeNetware)
                               return HarnessProtocol::ErrorResponse("singleplayer world required");
                           AIUnit* focus = GWorld->FocusOn();
                           AIGroup* group = focus ? focus->GetGroup() : nullptr;
                           AIUnit* passenger = group ? group->UnitWithID(2) : nullptr;
                           Transport* vehicle = passenger && passenger->IsInCargo() ? passenger->GetVehicleIn() : nullptr;
                           AIUnit* driver = vehicle ? vehicle->DriverBrain() : nullptr;
                           AISubgroup* subgroup = driver ? driver->GetSubgroup() : nullptr;
                           if (!subgroup || driver->IsPlayer() || !driver->GetPerson() || !driver->GetPerson()->IsLocal())
                               return HarnessProtocol::ErrorResponse("local AI driver subgroup required");
                           subgroup->SetSpeedMode(SpeedLimited);
                           cJSON* resp = cJSON_CreateObject();
                           cJSON_AddBoolToObject(resp, "limited", subgroup->GetSpeedMode() == SpeedLimited);
                           return HarnessProtocol::JsonResponse(resp);
                       });
    hs.RegisterCommand({"crew_pose_probe", "Read-only SP passenger pose/muzzle diagnostics", {}},
                       [](const std::string&, cJSON*) -> std::string
                       {
                           if (!GWorld || GWorld->GetMode() == GModeNetware)
                               return HarnessProtocol::ErrorResponse("singleplayer world required");
                           AIUnit* focus = GWorld->FocusOn();
                           AIGroup* group = focus ? focus->GetGroup() : nullptr;
                           AIUnit* unit = group ? group->UnitWithID(2) : nullptr;
                           Man* man = unit ? dyn_cast<Man>(unit->GetPerson()) : nullptr;
                           if (!man || !unit->IsInCargo() || !man->GetHierachyParent())
                               return HarnessProtocol::ErrorResponse("cargo infantry in group slot 1 required");
                           int weapon = -1;
                           for (int i = 0; i < man->NMagazineSlots(); ++i)
                           {
                               const auto* mode = man->GetWeaponMode(i);
                               if (mode && mode->_ammo && mode->_ammo->_simulation == AmmoShotBullet)
                               {
                                   weapon = i;
                                   break;
                               }
                           }
                           if (weapon < 0)
                               return HarnessProtocol::ErrorResponse("bullet weapon required");
                           const int selected = man->SelectedWeapon();
                           if (selected >= 0 && selected < man->NMagazineSlots())
                           {
                               const auto* mode = man->GetWeaponMode(selected);
                               if (mode && mode->_ammo && mode->_ammo->_simulation == AmmoShotBullet)
                                   weapon = selected;
                           }
                           const Matrix4 seat = man->WorldTransform();
                           const Vector3 muzzle = seat.FastTransform(man->GetWeaponPoint(weapon));
                           const Vector3 direction = man->GetWeaponDirection(weapon);
                           cJSON* resp = cJSON_CreateObject();
                           cJSON_AddStringToObject(resp, "move", man->GetCurrentMove());
                           cJSON_AddStringToObject(resp, "primaryMove", man->GetPrimaryMoveName());
                           cJSON_AddStringToObject(resp, "externalMove", man->GetExternalMoveName());
                           cJSON_AddBoolToObject(resp, "lookAround", InputSubsystem::Instance().IsLookAroundEnabled());
                           cJSON_AddNumberToObject(resp, "phase", man->GetLegPhase());
                           cJSON_AddBoolToObject(resp, "cargoWeaponActive", man->GetActiveCargoWeaponSeat() != nullptr);
                           cJSON_AddBoolToObject(resp, "cargoCandidate", man->IsCargoWeaponCandidate(weapon));
                           const auto& candidateSlot = man->GetMagazineSlot(weapon);
                           if (candidateSlot._weapon)
                           {
                               cJSON_AddStringToObject(resp, "candidateName", candidateSlot._weapon->GetName());
                               cJSON_AddNumberToObject(resp, "candidateKind", candidateSlot._weapon->_weaponType);
                               cJSON_AddBoolToObject(resp, "candidateTurret", candidateSlot._weapon->_shotFromTurret);
                           }
                           cJSON_AddBoolToObject(resp, "weaponsDisabled", man->WeaponsDisabled());
                           cJSON_AddBoolToObject(resp, "local", man->IsLocal());
                           cJSON_AddBoolToObject(resp, "player", unit->IsPlayer());
                           Transport* vehicle = unit->GetVehicleIn();
                           if (Transport* assigned = unit->VehicleAssigned(); assigned && assigned->Type()->NCargoGetInPos() > 0)
                           {
                               const Vector3 entry = assigned->GetUnitGetInPos(unit);
                               cJSON_AddNumberToObject(resp, "entryX", entry.X());
                               cJSON_AddNumberToObject(resp, "entryY", entry.Y() - GLandscape->SurfaceY(entry.X(), entry.Z()));
                               cJSON_AddNumberToObject(resp, "entryZ", entry.Z());
                               cJSON_AddNumberToObject(resp, "entryRadius", assigned->Type()->GetGetInRadius());
                           }
                           cJSON_AddBoolToObject(resp, "vehicleDestroyed", !vehicle || vehicle->IsDammageDestroyed());
                           if (vehicle)
                           {
                               for (int i = 0; i < vehicle->GetManCargo().Size(); ++i)
                               {
                                   if (vehicle->GetManCargo()[i] != man) continue;
                                   cJSON_AddNumberToObject(resp, "cargoIndex", i);
                                   const auto* spec = vehicle->Type()->GetCargoWeaponSeat(i);
                                   cJSON_AddBoolToObject(resp, "seatConfigured", spec != nullptr);
                                   if (spec)
                                   {
                                       cJSON_AddStringToObject(resp, "seatAction", spec->action);
                                       cJSON_AddNumberToObject(resp, "aiTrackingRadiansPerSecond", spec->AITrackingRate());
                                   }
                               }
                           }
                           cJSON_AddBoolToObject(resp, "cargoWeaponLineClear", man->CargoWeaponLineClear(weapon));
                           if (GWorld->UI())
                           {
                               const Vector3 cursor = GWorld->UI()->GetCursorDirection();
                               cJSON_AddNumberToObject(resp, "cursorX", cursor.X());
                               cJSON_AddNumberToObject(resp, "cursorY", cursor.Y());
                               cJSON_AddNumberToObject(resp, "cursorZ", cursor.Z());
                           }
                           cJSON_AddBoolToObject(resp, "fireTarget", man->GetFireTarget() != nullptr);
                           Man::AimingDiagnostics aiming;
                           cJSON_AddNumberToObject(resp, "aimed", man->GetAimed(weapon, man->GetFireTarget(), &aiming));
                           cJSON_AddStringToObject(resp, "aimGate", aiming.gate);
                           cJSON_AddNumberToObject(resp, "aimVisibility", aiming.visibility);
                           cJSON_AddNumberToObject(resp, "aimError", aiming.error);
                           cJSON_AddNumberToObject(resp, "aimTargetSize", aiming.targetSize);
                           cJSON_AddBoolToObject(resp, "loaded", man->GetWeaponLoaded(weapon));
                           cJSON_AddBoolToObject(resp, "ready", man->GetWeaponReady(weapon, man->GetFireTarget()));
                           cJSON_AddBoolToObject(resp, "prepareOnly", man->IsFirePrepare());
                           cJSON_AddNumberToObject(resp, "selectedWeapon", man->SelectedWeapon());
                           cJSON_AddBoolToObject(resp, "reloadInProgress", man->IsActionInProgress(MFReload));
                           cJSON_AddBoolToObject(resp, "manual", man->QIsManual());
                           cJSON_AddBoolToObject(resp, "playerSuspended", GWorld->GetPlayerSuspended());
                           cJSON_AddBoolToObject(resp, "cameraEffect", GWorld->GetCameraEffect() != nullptr);
                           cJSON_AddBoolToObject(resp, "weaponManipulation", man->EnableWeaponManipulation());
                           cJSON_AddNumberToObject(resp, "reloadInput", InputSubsystem::Instance().GetAction(UAReloadMagazine));
                           cJSON_AddNumberToObject(resp, "nearTrackAge", Glob.time - man->GetTrackNearTargetsTime());
                           cJSON_AddNumberToObject(resp, "nearestEnemyDist2", unit->GetNearestEnemyDist2());
                           cJSON_AddBoolToObject(resp, "hasAI", unit->HasAI());
                           const Vector3 eye = man->GetEyeDirection();
                           cJSON_AddNumberToObject(resp, "eyeX", eye.X());
                           cJSON_AddNumberToObject(resp, "eyeY", eye.Y());
                           cJSON_AddNumberToObject(resp, "eyeZ", eye.Z());
                           const Vector3 pivot = seat.FastTransform(man->GetWeaponCenter(weapon));
                           cJSON_AddNumberToObject(resp, "pivotX", pivot.X());
                           cJSON_AddNumberToObject(resp, "pivotY", pivot.Y());
                           cJSON_AddNumberToObject(resp, "pivotZ", pivot.Z());
                           if (GLandscape)
                           {
                               auto contacts = [&](const char* name, Vector3Par start, Vector3Par end)
                               {
                                   CollisionBuffer hits;
                                   GLandscape->ObjectCollision(hits, man, nullptr, start, end, .02f, ObjIntersectFire);
                                   cJSON* list = cJSON_AddArrayToObject(resp, name);
                                   for (int i = 0; i < hits.Size(); ++i)
                                   {
                                       const auto& hit = hits[i];
                                       cJSON* item = cJSON_CreateObject();
                                       cJSON_AddBoolToObject(item, "carrier", hit.object == vehicle);
                                       cJSON_AddBoolToObject(item, "shooter", hit.object == man);
                                       cJSON_AddNumberToObject(item, "component", hit.component);
                                       cJSON_AddNumberToObject(item, "hierarchy", hit.hierLevel);
                                       cJSON_AddNumberToObject(item, "under", hit.under);
                                       cJSON_AddItemToArray(list, item);
                                   }
                               };
                               contacts("pivotContacts", pivot, muzzle);
                               contacts("muzzleContacts", muzzle, muzzle + direction.Normalized() * 2.0f);
                               cJSON_AddNumberToObject(resp, "groundClearance",
                                   GLandscape->IntersectWithGround(nullptr, muzzle, direction.Normalized(), 0, 2.0f));
                           }
                           if (Target* target = man->GetFireTarget())
                           {
                               cJSON_AddBoolToObject(resp, "friendlyLineClear", man->CheckFriendlyFire(weapon, target));
                               cJSON_AddNumberToObject(resp, "targetX", target->position.X());
                               cJSON_AddNumberToObject(resp, "targetY", target->position.Y());
                               cJSON_AddNumberToObject(resp, "targetZ", target->position.Z());
                               cJSON_AddNumberToObject(resp, "targetError", target->posError.Size());
                               cJSON_AddNumberToObject(resp, "lastSeenAge", Glob.time - target->lastSeen);
                               cJSON_AddNumberToObject(resp, "visibilityAge", Glob.time - GWorld->VisibilityTime(unit, target->idExact));
                               cJSON_AddNumberToObject(resp, "sensorVisibility", GWorld->Visibility(unit, target->idExact));
                               if (target->idExact)
                               {
                                   cJSON_AddNumberToObject(resp, "exactX", target->idExact->Position().X());
                                   cJSON_AddNumberToObject(resp, "exactZ", target->idExact->Position().Z());
                                   cJSON_AddNumberToObject(resp, "aimX", target->idExact->AimingPosition().X());
                                   cJSON_AddNumberToObject(resp, "aimZ", target->idExact->AimingPosition().Z());
                               }
                               if (GLandscape && target->idExact)
                                   cJSON_AddNumberToObject(resp, "muzzleVisibility",
                                       GLandscape->Visible(muzzle, man, target->idExact, .9f, ObjIntersectFire));
                           }
                           cJSON_AddNumberToObject(resp, "muzzleX", muzzle.X());
                           cJSON_AddNumberToObject(resp, "muzzleY", muzzle.Y());
                           cJSON_AddNumberToObject(resp, "muzzleZ", muzzle.Z());
                           cJSON_AddNumberToObject(resp, "directionX", direction.X());
                           cJSON_AddNumberToObject(resp, "directionY", direction.Y());
                           cJSON_AddNumberToObject(resp, "directionZ", direction.Z());
                           cJSON_AddNumberToObject(resp, "seatForwardX", seat.Direction().X());
                           cJSON_AddNumberToObject(resp, "seatForwardZ", seat.Direction().Z());
                           return HarnessProtocol::JsonResponse(resp);
                       });
    hs.RegisterCommand({"crew_projectile_probe", "SP harness only: raw bullet launch, not gameplay/FFV acceptance", {}},
                       [](const std::string&, cJSON*) -> std::string
                       {
                           if (!GWorld || GWorld->GetMode() == GModeNetware)
                               return HarnessProtocol::ErrorResponse("singleplayer world required");
                           AIUnit* focus = GWorld->FocusOn();
                           AIGroup* group = focus ? focus->GetGroup() : nullptr;
                           AIUnit* unit = group ? group->UnitWithID(2) : nullptr;
                           Person* person = unit ? unit->GetPerson() : nullptr;
                           Man* man = dyn_cast<Man>(person);
                           if (!man || !person->IsLocal() || !unit->IsInCargo() ||
                               !person->GetHierachyParent() || person->IsDammageDestroyed())
                               return HarnessProtocol::ErrorResponse("local living cargo unit in group slot 1 required");
                           int weapon = -1;
                           for (int i = 0; i < person->NMagazineSlots(); ++i)
                           {
                               const auto& slot = person->GetMagazineSlot(i);
                               const auto* mode = person->GetWeaponMode(i);
                               if (slot._magazine && slot._magazine->_ammo > 0 && mode && mode->_ammo &&
                                   mode->_ammo->_simulation == AmmoShotBullet)
                               {
                                   weapon = i;
                                   break;
                               }
                           }
                           if (weapon < 0)
                               return HarnessProtocol::ErrorResponse("loaded bullet weapon required");
                           const auto& slot = person->GetMagazineSlot(weapon);
                           const float muzzleSpeed = slot._magazine->_type->_initSpeed;
                           const Vector3 offset = person->GetWeaponPoint(weapon);
                           const Matrix4 frame = person->WorldTransform();
                           const Vector3 expectedPos = frame.FastTransform(offset);
                           const auto* carrier = unit->GetVehicleIn();
                           const Vector3 centre = carrier->WorldTransform().FastTransform(carrier->GetCenterOfMass());
                           const Vector3 inheritedSpeed = carrier->WorldSpeed() +
                               carrier->AngVelocity().CrossProduct(expectedPos - centre);
                           const Vector3 expectedWeaponDir = frame.Rotate(man->GetWeaponRelDirection(weapon));
                           const float weaponDirectionError =
                               (person->GetWeaponDirection(weapon) - expectedWeaponDir).Size();
                           // Deliberately bypasses animation/readiness/ammo logic to test only
                           // the shared projectile constructor. Never an FFV gameplay success.
                           if (!person->FireMGun(weapon, offset, VForward, nullptr))
                               return HarnessProtocol::ErrorResponse("raw projectile creation failed");
                           Entity* shot = person->GetLastShot();
                           if (!shot)
                               return HarnessProtocol::ErrorResponse("no created projectile");
                           const Vector3 expectedSpeed = inheritedSpeed + shot->Direction() * muzzleSpeed;
                           cJSON* resp = cJSON_CreateObject();
                           cJSON_AddNumberToObject(resp, "positionError", (shot->Position() - expectedPos).Size());
                           cJSON_AddNumberToObject(resp, "velocityError", (shot->Speed() - expectedSpeed).Size());
                           cJSON_AddNumberToObject(resp, "weaponDirectionError", weaponDirectionError);
                           cJSON_AddNumberToObject(resp, "directionDot", shot->Direction() * frame.Direction());
                           cJSON_AddNumberToObject(resp, "worldX", shot->Position().X());
                           cJSON_AddNumberToObject(resp, "worldZ", shot->Position().Z());
                           cJSON_AddNumberToObject(resp, "inheritedSpeed", inheritedSpeed.Size());
                           // This measures construction only. Retire before simulation so
                           // the neutral seated pose cannot shoot the Jeep or its occupants.
                           GWorld->RemoveFastVehicle(shot);
                           return HarnessProtocol::JsonResponse(resp);
                       });
    hs.RegisterCommand({"eval", "Evaluate SQF expression and return result as string", {{"code", "string", true}}},
                       [](const std::string&, cJSON* root) -> std::string
                       {
                           const char* code = HarnessProtocol::GetString(root, "code");
                           if (!code || !code[0])
                               return HarnessProtocol::ErrorResponse("code required");
                           GameState* gs = GWorld ? GWorld->GetGameState() : nullptr;
                           if (!gs)
                               return HarnessProtocol::ErrorResponse("no game state available");
                           gs->BeginContext(&s_evalScope);
                           GameValue result = gs->EvaluateMultiple(code);
                           gs->EndContext();
                           cJSON* resp = cJSON_CreateObject();
                           RString text = result.GetText();
                           cJSON_AddStringToObject(resp, "result", (const char*)text);
                           return HarnessProtocol::JsonResponse(resp);
                       });
    hs.RegisterCommand({"exec", "Execute SQF code (fire-and-forget)", {{"code", "string", true}}},
                       [](const std::string&, cJSON* root) -> std::string
                       {
                           const char* code = HarnessProtocol::GetString(root, "code");
                           if (!code || !code[0])
                               return HarnessProtocol::ErrorResponse("code required");
                           GameState* gs = GWorld ? GWorld->GetGameState() : nullptr;
                           if (!gs)
                               return HarnessProtocol::ErrorResponse("no game state available");
                           gs->BeginContext(&s_evalScope);
                           gs->Execute(code);
                           gs->EndContext();
                           return HarnessProtocol::OkResponse();
                       });
}

void RegisterSnowProbe(HarnessServer& hs)
{
    hs.RegisterCommand({"dev_reset_probe", "Exercise renderer settings before a master reset", {}},
        [](const std::string&, cJSON*) -> std::string {
            if (!GEngine || !GEngine->SupportsSky()) return HarnessProtocol::ErrorResponse("requires procedural sky");
            GEngine->SetSkyCirrus(false, false);
            GEngine->SetSkyCirrusPuffiness(0.2f, 0.3f);
            GEngine->SetSkyCirrusLook(0.1f, 0.2f);
            GEngine->SetSkyCirrusSoftness(0.9f);
            GEngine->SetFogFarClose(0.4f);
            GEngine->SetGodRays(false, 1.0f, 0.00001f, 1000.0f, 0.3f, 0.2f, 8, 4);
            auto road = GEngine->GetRoadSettings();
            road.liftFlat = 0.03f;
            GEngine->SetRoadSettings(road);
            return HarnessProtocol::OkResponse();
        });
    hs.RegisterCommand({"dev_snow", "Inspect snow or exercise settings resets (never clears tracks)",
                        {{"action", "string", true}}},
        [](const std::string&, cJSON* root) -> std::string {
            const char* action = HarnessProtocol::GetString(root, "action");
            if (!GWorld || !action) return HarnessProtocol::ErrorResponse("snow probe requires world and action");
            auto& snow = GSnow();
            if (std::strcmp(action, "reset-panel") == 0) ResetPanelSettings();
            else if (std::strcmp(action, "reset-settings") == 0) snow.ResetSettings();
            else if (std::strcmp(action, "enable") == 0) snow.enabled = true;
            else if (std::strcmp(action, "disable") == 0) snow.enabled = false;
            else if (std::strcmp(action, "storm") == 0) snow.ApplySnowstormPreset();
            else if (std::strcmp(action, "depth-limit") == 0)
            {
                const cJSON* metres = cJSON_GetObjectItemCaseSensitive(root, "metres");
                if (!cJSON_IsNumber(metres) || !(metres->valuedouble >= 0.01 && metres->valuedouble <= 1))
                    return HarnessProtocol::ErrorResponse("depth limit requires metres in [0.01, 1]");
                snow.maxDepth = static_cast<float>(metres->valuedouble);
            }
            else if (std::strcmp(action, "deposit") == 0)
            {
                const cJSON* metres = cJSON_GetObjectItemCaseSensitive(root, "metres");
                if (!cJSON_IsNumber(metres) || !(metres->valuedouble > 0 && metres->valuedouble <= 1))
                    return HarnessProtocol::ErrorResponse("deposit requires metres in (0, 1]");
                snow.Deposit(static_cast<float>(metres->valuedouble));
            }
            else if (std::strcmp(action, "sample") == 0)
            {
                const auto* x = cJSON_GetObjectItemCaseSensitive(root, "x");
                const auto* z = cJSON_GetObjectItemCaseSensitive(root, "z");
                if (!cJSON_IsNumber(x) || !cJSON_IsNumber(z) ||
                    !std::isfinite(x->valuedouble) || !std::isfinite(z->valuedouble) ||
                    std::abs(x->valuedouble) > 1000000 || std::abs(z->valuedouble) > 1000000)
                    return HarnessProtocol::ErrorResponse("sample needs bounded world x/z");
                auto* out = cJSON_CreateObject();
                cJSON_AddNumberToObject(out, "deficit", snow.DeficitAt(
                    static_cast<int>(std::floor(x->valuedouble / SnowField::CellSize)),
                    static_cast<int>(std::floor(z->valuedouble / SnowField::CellSize))));
                return HarnessProtocol::JsonResponse(out);
            }
            else if (std::strcmp(action, "state") != 0) return HarnessProtocol::ErrorResponse("unknown snow action");
            cJSON* out = cJSON_CreateObject();
            cJSON_AddBoolToObject(out, "enabled", snow.enabled);
            cJSON_AddBoolToObject(out, "falling", snow.falling);
            cJSON_AddBoolToObject(out, "geometry", snow.detailedGeometry);
            cJSON_AddNumberToObject(out, "rate", snow.metresPerMinute);
            cJSON_AddNumberToObject(out, "maxDepth", snow.maxDepth);
            cJSON_AddNumberToObject(out, "flakes", snow.flakeMultiplier);
            cJSON_AddNumberToObject(out, "depth", snow.Depth());
            cJSON_AddNumberToObject(out, "chunks", static_cast<double>(snow.Chunks()));
            cJSON_AddNumberToObject(out, "liveFlakes", GSnowFlakes.Stats().liveDrops);
            cJSON_AddNumberToObject(out, "drawnFlakes", GSnowFlakes.Stats().drawnDrops);
            return HarnessProtocol::JsonResponse(out);
        });
}

void RegisterPhysicsProbe(HarnessServer& hs)
{
    hs.RegisterCommand({"dev_physics_probe", "Inspect or launch bounded collision probes (engine XYZ metres)",
                        {{"action", "string", true}}},
        [](const std::string&, cJSON* root) -> std::string {
            const char* action = HarnessProtocol::GetString(root, "action");
            if (!GWorld || !GLandscape || !action)
                return HarnessProtocol::ErrorResponse("requires world and action");
            if (std::strcmp(action, "init") == 0)
            {
                if (!PhysicsCorpusHasRun()) RunPhysicsCorpus();
                return HarnessProtocol::OkResponse();
            }
            auto* world = Physics::GetPhysicsWorld();
            if (!world || !world->IsCreated()) return HarnessProtocol::ErrorResponse("init physics first");
            if (std::strcmp(action, "state") == 0)
            {
                std::vector<Physics::ProbeSample> samples;
                world->GetProbeSamples(samples);
                cJSON* out = cJSON_CreateObject();
                auto* list = cJSON_AddArrayToObject(out, "positions");
                for (const auto& sample : samples)
                {
                    const float position[] = {sample.position.X(), sample.position.Y(), sample.position.Z()};
                    cJSON_AddItemToArray(list, cJSON_CreateFloatArray(position, 3));
                }
                cJSON_AddNumberToObject(out, "bodies", world->GetStats().bodies);
                return HarnessProtocol::JsonResponse(out);
            }
            auto readVector = [&](const char* name, Vector3& value) {
                auto* array = cJSON_GetObjectItemCaseSensitive(root, name);
                if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) != 3) return false;
                for (int i = 0; i < 3; ++i)
                {
                    auto* item = cJSON_GetArrayItem(array, i);
                    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
                        std::abs(item->valuedouble) > 1000000.0) return false;
                    value[i] = static_cast<float>(item->valuedouble);
                }
                return true;
            };
            Vector3 from, to;
            if (!readVector("from", from) || !readVector("to", to) ||
                (to-from).Size() < 0.01f || (to-from).Size() > 100.0f)
                return HarnessProtocol::ErrorResponse("requires finite from/to, segment 0.01..100 m");
            if (std::strcmp(action, "spawn") == 0)
            {
                std::vector<Physics::ProbeSample> samples;
                world->GetProbeSamples(samples);
                if (samples.size() >= 32) return HarnessProtocol::ErrorResponse("probe test limit reached");
                Physics::SphereProbeDef def;
                def.position = from;
                def.velocity = (to-from).Normalized() * 20.0f;
                def.restitution = 0;
                if (!world->SpawnSphereProbe(def).IsValid()) return HarnessProtocol::ErrorResponse("spawn failed");
                return HarnessProtocol::OkResponse();
            }
            if (std::strcmp(action, "ray") != 0) return HarnessProtocol::ErrorResponse("unknown physics action");
            Vector3 hit(VZero);
            const bool solid = world->CastRay(from, to, hit, {Physics::ColliderFlags::Solid}).IsValid();
            cJSON* out = cJSON_CreateObject();
            cJSON_AddBoolToObject(out, "solid", solid);
            cJSON_AddNumberToObject(out, "distance", solid ? (hit-from).Size() : -1.0f);
            return HarnessProtocol::JsonResponse(out);
        });
}

void RegisterHttpFixtures(HarnessServer& hs)
{
    hs.RegisterCommand({"http_fixture",
                        "Map an exact HTTP URL to a loopback fixture URL",
                        {{"url", "string", true}, {"target", "string", true}}},
                       [](const std::string&, cJSON* root) -> std::string
                       {
                           const char* url = HarnessProtocol::GetString(root, "url");
                           const char* target = HarnessProtocol::GetString(root, "target");
                           if (!url || !url[0] || !target || !target[0])
                               return HarnessProtocol::ErrorResponse("http_fixture requires url and target");
                           if (!RegisterHttpTestRewrite(url, target))
                               return HarnessProtocol::ErrorResponse(
                                   "http_fixture requires an HTTP URL and loopback target");
                           return HarnessProtocol::OkResponse();
                       });
}

std::string AnswerNetworkQuery(const char* what)
{
    if (!what)
        return {};
    if (std::strcmp(what, "players") == 0)
    {
        cJSON* resp = cJSON_CreateObject();
        cJSON* arr = cJSON_AddArrayToObject(resp, "players");
        try
        {
            INetworkManager& netMgr = GetNetworkManager();
            AutoArray<NetPlayerInfo, Foundation::MemAllocSA> players;
            netMgr.GetPlayers(players);
            for (int i = 0; i < players.Size(); i++)
            {
                cJSON* pobj = cJSON_CreateObject();
                cJSON_AddNumberToObject(pobj, "dpid", players[i].dpid);
                cJSON_AddStringToObject(pobj, "name", players[i].name);
                cJSON_AddItemToArray(arr, pobj);
            }
        }
        catch (...)
        {
        }
        return HarnessProtocol::JsonResponse(resp);
    }
    if (std::strcmp(what, "von_state") == 0)
    {
        cJSON* resp = cJSON_CreateObject();
        cJSON* arr = cJSON_AddArrayToObject(resp, "speakers");
        try
        {
            INetworkManager& netMgr = GetNetworkManager();
            AutoArray<NetVoiceSpeakerInfo, Foundation::MemAllocSA> speakers;
            netMgr.GetVoiceSpeakers(speakers);
            for (int i = 0; i < speakers.Size(); i++)
            {
                cJSON* pobj = cJSON_CreateObject();
                cJSON_AddNumberToObject(pobj, "dpid", speakers[i].player);
                cJSON_AddBoolToObject(pobj, "active", speakers[i].active);
                cJSON_AddNumberToObject(pobj, "level", speakers[i].level);
                const PlayerIdentity* identity = netMgr.FindIdentity(speakers[i].player);
                cJSON_AddStringToObject(pobj, "name", identity ? identity->GetName() : "");
                cJSON_AddItemToArray(arr, pobj);
            }
        }
        catch (...)
        {
        }
        return HarnessProtocol::JsonResponse(resp);
    }
    if (std::strcmp(what, "mission") == 0)
    {
        cJSON* resp = cJSON_CreateObject();
        try
        {
            INetworkManager& netMgr = GetNetworkManager();
            const MissionHeader* mh = netMgr.GetMissionHeader();
            if (mh)
            {
                cJSON_AddStringToObject(resp, "name", mh->name);
                cJSON_AddStringToObject(resp, "island", mh->island);
                cJSON_AddStringToObject(resp, "fileName", mh->fileName);
                cJSON_AddBoolToObject(resp, "jip", mh->joinInProgress);
            }
        }
        catch (...)
        {
        }
        return HarnessProtocol::JsonResponse(resp);
    }
    if (std::strcmp(what, "ngs") == 0)
    {
        cJSON* resp = cJSON_CreateObject();
        try
        {
            INetworkManager& netMgr = GetNetworkManager();
            cJSON_AddNumberToObject(resp, "server_state", static_cast<int>(netMgr.GetServerState()));
            cJSON_AddNumberToObject(resp, "client_state", static_cast<int>(netMgr.GetGameState()));
        }
        catch (...)
        {
            cJSON_AddNumberToObject(resp, "server_state", -1);
            cJSON_AddNumberToObject(resp, "client_state", -1);
        }
        return HarnessProtocol::JsonResponse(resp);
    }
    if (std::strcmp(what, "play_state") == 0)
    {
        cJSON* resp = cJSON_CreateObject();
        try
        {
            INetworkManager& netMgr = GetNetworkManager();
            cJSON_AddNumberToObject(resp, "server_state", static_cast<int>(netMgr.GetServerState()));
            cJSON_AddNumberToObject(resp, "client_state", static_cast<int>(netMgr.GetGameState()));
            cJSON_AddBoolToObject(resp, "has_role", netMgr.GetMyPlayerRole() != nullptr);
        }
        catch (...)
        {
            cJSON_AddNumberToObject(resp, "server_state", -1);
            cJSON_AddNumberToObject(resp, "client_state", -1);
            cJSON_AddBoolToObject(resp, "has_role", false);
        }

        UITestEngine ui;
        ControlsContainer* display = ui.GetActiveDisplay();
        cJSON_AddNumberToObject(resp, "display", display ? display->IDD() : -1);
        cJSON_AddBoolToObject(resp, "in_gameplay", GApp && GApp->IsInGameplay());
        cJSON_AddBoolToObject(resp, "has_world", GWorld != nullptr);
        cJSON_AddNumberToObject(resp, "world_mode", GWorld ? static_cast<int>(GWorld->GetMode()) : -1);
        cJSON_AddNumberToObject(resp, "frame", GEngine ? GEngine->GetFrameCounter() : 0);
        cJSON_AddNumberToObject(resp, "time_ms", Glob.time.toInt());

        Person* player = GWorld ? GWorld->GetRealPlayer() : nullptr;
        AIUnit* brain = player ? player->Brain() : nullptr;
        EntityAI* vehicle = brain ? brain->GetVehicle() : nullptr;
        cJSON_AddBoolToObject(resp, "has_player", player != nullptr);
        cJSON_AddBoolToObject(resp, "player_local", player && player->IsLocal());
        cJSON_AddBoolToObject(resp, "player_destroyed", player && player->IsDammageDestroyed());
        cJSON_AddBoolToObject(resp, "has_brain", brain != nullptr);
        cJSON_AddBoolToObject(resp, "has_vehicle", vehicle != nullptr);
        cJSON_AddBoolToObject(resp, "vehicle_local", vehicle && vehicle->IsLocal());
        cJSON_AddBoolToObject(resp, "player_active", GWorld && player && GWorld->PlayerOn() == player);
        if (player)
        {
            Vector3Val pos = player->Position();
            Vector3Val speed = player->Speed();
            cJSON_AddNumberToObject(resp, "player_x", pos.X());
            cJSON_AddNumberToObject(resp, "player_z", pos.Z());
            cJSON_AddNumberToObject(resp, "player_speed_x", speed.X());
            cJSON_AddNumberToObject(resp, "player_speed_z", speed.Z());
        }
        return HarnessProtocol::JsonResponse(resp);
    }
    if (std::strcmp(what, "connections") == 0)
    {
        cJSON* resp = cJSON_CreateObject();
        cJSON* arr = cJSON_AddArrayToObject(resp, "connections");
        cJSON_AddNumberToObject(resp, "server_state", static_cast<int>(GNetworkManager.GetServerState()));
        NetworkServer* server = GNetworkManager.GetServer();
        if (!server)
            return HarnessProtocol::JsonResponse(resp);

        AutoArray<NetworkConnectionSnapshot, Foundation::MemAllocSA> connections;
        server->GetConnectionSnapshots(connections);
        for (int i = 0; i < connections.Size(); i++)
        {
            const NetworkConnectionSnapshot& connection = connections[i];
            cJSON* item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "dpid", connection.dpid);
            cJSON_AddStringToObject(item, "name", connection.name);
            cJSON_AddNumberToObject(item, "state", static_cast<int>(connection.state));
            cJSON_AddBoolToObject(item, "jip", connection.jip);
            cJSON_AddBoolToObject(item, "botClient", connection.botClient);
            cJSON_AddBoolToObject(item, "hasConnectionInfo", connection.hasConnectionInfo);
            cJSON_AddNumberToObject(item, "latencyMs", connection.latencyMs);
            cJSON_AddNumberToObject(item, "throughputBps", connection.throughputBps);
            cJSON_AddNumberToObject(item, "avgPingMs", connection.avgPingMs);
            cJSON_AddNumberToObject(item, "avgBandwidthBps", connection.avgBandwidthBps);
            cJSON_AddItemToArray(arr, item);
        }
        return HarnessProtocol::JsonResponse(resp);
    }
    if (std::strcmp(what, "roles") == 0)
    {
        cJSON* resp = cJSON_CreateObject();
        cJSON* arr = cJSON_AddArrayToObject(resp, "roles");
        try
        {
            INetworkManager& netMgr = GetNetworkManager();
            cJSON_AddNumberToObject(resp, "player", netMgr.GetPlayer());
            cJSON_AddNumberToObject(resp, "game_state", static_cast<int>(netMgr.GetGameState()));
            cJSON_AddNumberToObject(resp, "server_state", static_cast<int>(netMgr.GetServerState()));
            const int n = netMgr.NPlayerRoles();
            cJSON_AddNumberToObject(resp, "count", n);
            for (int i = 0; i < n; ++i)
            {
                const PlayerRole* role = netMgr.GetPlayerRole(i);
                if (!role)
                    continue;
                cJSON* item = cJSON_CreateObject();
                cJSON_AddNumberToObject(item, "index", i);
                cJSON_AddNumberToObject(item, "side", static_cast<int>(role->side));
                cJSON_AddNumberToObject(item, "group", role->group);
                cJSON_AddNumberToObject(item, "unit", role->unit);
                cJSON_AddNumberToObject(item, "player", role->player);
                cJSON_AddBoolToObject(item, "locked", role->roleLocked);
                cJSON_AddItemToArray(arr, item);
            }
        }
        catch (...)
        {
        }
        return HarnessProtocol::JsonResponse(resp);
    }
    return {};
}

namespace
{
RString ResolveHarnessMasterServerHost()
{
    return GetNetworkMasterServer();
}

void AddMasterServerServiceSession(cJSON* object, const char* serverId, const MasterServerServiceSession& session)
{
    cJSON_AddStringToObject(object, "serverId", serverId != nullptr ? serverId : "");
    cJSON_AddStringToObject(object, "address", session.address.c_str());
    cJSON_AddNumberToObject(object, "hostport", session.hostPort);
    cJSON_AddStringToObject(object, "hostname", session.hostName.c_str());
    cJSON_AddStringToObject(object, "gametype", session.mission.c_str());
    cJSON_AddNumberToObject(object, "actver", session.actualVersion);
    cJSON_AddNumberToObject(object, "reqver", session.requiredVersion);
    cJSON_AddNumberToObject(object, "state", session.gameState);
    cJSON_AddNumberToObject(object, "numplayers", session.numPlayers);
    cJSON_AddNumberToObject(object, "maxplayers", session.maxPlayers);
    cJSON_AddBoolToObject(object, "password", session.password);
    cJSON_AddStringToObject(object, "impl", session.transportImplementation.c_str());
    cJSON_AddStringToObject(object, "mod", session.mod.c_str());
    cJSON_AddBoolToObject(object, "equalModRequired", session.equalModRequired);
}

void AddMasterServerServiceMod(cJSON* object, const MasterServerServiceModCatalogEntry& mod)
{
    cJSON_AddStringToObject(object, "modId", mod.modId.c_str());
    cJSON_AddStringToObject(object, "name", mod.name.c_str());
    cJSON_AddStringToObject(object, "version", mod.version.c_str());
    cJSON_AddStringToObject(object, "description", mod.description.c_str());
    cJSON* authors = cJSON_AddArrayToObject(object, "authors");
    for (const auto& author : mod.authors)
    {
        cJSON_AddItemToArray(authors, cJSON_CreateString(author.c_str()));
    }
    cJSON_AddStringToObject(object, "homepageUrl", mod.homepageUrl.c_str());
    cJSON_AddStringToObject(object, "downloadUrl", mod.downloadUrl.c_str());
    cJSON_AddNumberToObject(object, "sizeBytes", static_cast<double>(mod.sizeBytes));
}

void AddMasterServerServiceUsageServer(cJSON* object, const MasterServerServiceModUsageServer& server)
{
    cJSON_AddStringToObject(object, "serverId", server.serverId.c_str());
    cJSON_AddStringToObject(object, "hostname", server.hostName.c_str());
    cJSON_AddStringToObject(object, "gametype", server.mission.c_str());
    cJSON_AddNumberToObject(object, "players", server.players);
    cJSON_AddNumberToObject(object, "maxPlayers", server.maxPlayers);
    cJSON_AddBoolToObject(object, "password", server.password);
}

std::string AnswerMasterServerServerDetailQuery(cJSON* root)
{
    const char* serverId = HarnessProtocol::GetString(root, "serverId");
    if (!serverId || !serverId[0])
    {
        return HarnessProtocol::ErrorResponse("serverId required");
    }

    const RString& masterServer = ResolveHarnessMasterServerHost();
    if (masterServer.GetLength() == 0)
    {
        return HarnessProtocol::ErrorResponse("no master-server configured");
    }

    MasterServerServiceServerDetail detail;
    if (!FetchMasterServerServiceServerDetail(masterServer, serverId, nullptr, detail))
    {
        return HarnessProtocol::ErrorResponse("master-server detail fetch failed");
    }

    cJSON* resp = cJSON_CreateObject();
    cJSON* server = cJSON_AddObjectToObject(resp, "server");
    AddMasterServerServiceSession(server, serverId, detail.server);

    cJSON* players = cJSON_AddArrayToObject(resp, "players");
    for (const auto& player : detail.players)
    {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", player.name.c_str());
        cJSON_AddStringToObject(item, "role", player.role.c_str());
        cJSON_AddItemToArray(players, item);
    }

    cJSON* mods = cJSON_AddArrayToObject(resp, "mods");
    for (const auto& mod : detail.mods)
    {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "modId", mod.modId.c_str());
        cJSON_AddStringToObject(item, "name", mod.name.c_str());
        cJSON_AddBoolToObject(item, "known", mod.known);
        cJSON_AddStringToObject(item, "version", mod.version.c_str());
        cJSON_AddStringToObject(item, "description", mod.description.c_str());
        cJSON_AddStringToObject(item, "homepageUrl", mod.homepageUrl.c_str());
        cJSON_AddStringToObject(item, "downloadUrl", mod.downloadUrl.c_str());
        cJSON_AddItemToArray(mods, item);
    }

    cJSON* playerHistory = cJSON_AddArrayToObject(resp, "playerHistory");
    for (const auto& sample : detail.playerHistory)
    {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "observedUnixMs", static_cast<double>(sample.observedUnixMs));
        cJSON_AddNumberToObject(item, "players", sample.players);
        cJSON_AddItemToArray(playerHistory, item);
    }

    cJSON* recentSessions = cJSON_AddArrayToObject(resp, "recentSessions");
    for (const auto& session : detail.recentSessions)
    {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "mission", session.mission.c_str());
        cJSON_AddStringToObject(item, "label", session.label.c_str());
        cJSON_AddNumberToObject(item, "playedMinutes", session.playedMinutes);
        cJSON_AddNumberToObject(item, "peakPlayers", session.peakPlayers);
        cJSON_AddNumberToObject(item, "endedUnixMs", static_cast<double>(session.endedUnixMs));
        cJSON_AddItemToArray(recentSessions, item);
    }

    return HarnessProtocol::JsonResponse(resp);
}

std::string AnswerMasterServerModDetailQuery(cJSON* root)
{
    const char* modId = HarnessProtocol::GetString(root, "modId");
    if (!modId || !modId[0])
    {
        return HarnessProtocol::ErrorResponse("modId required");
    }

    const RString& masterServer = ResolveHarnessMasterServerHost();
    if (masterServer.GetLength() == 0)
    {
        return HarnessProtocol::ErrorResponse("no master-server configured");
    }

    MasterServerServiceModCatalogEntry mod;
    if (!FetchMasterServerServiceModDetail(masterServer, modId, nullptr, mod))
    {
        return HarnessProtocol::ErrorResponse("master-server mod detail fetch failed");
    }

    cJSON* resp = cJSON_CreateObject();
    AddMasterServerServiceMod(resp, mod);
    return HarnessProtocol::JsonResponse(resp);
}

std::string AnswerMasterServerModVersionsQuery(cJSON* root)
{
    const char* modId = HarnessProtocol::GetString(root, "modId");
    if (!modId || !modId[0])
    {
        return HarnessProtocol::ErrorResponse("modId required");
    }

    const RString& masterServer = ResolveHarnessMasterServerHost();
    if (masterServer.GetLength() == 0)
    {
        return HarnessProtocol::ErrorResponse("no master-server configured");
    }

    std::vector<MasterServerServiceModCatalogEntry> versions;
    if (!FetchMasterServerServiceModVersions(masterServer, modId, nullptr, versions))
    {
        return HarnessProtocol::ErrorResponse("master-server mod versions fetch failed");
    }

    cJSON* resp = cJSON_CreateObject();
    cJSON* items = cJSON_AddArrayToObject(resp, "versions");
    for (const auto& version : versions)
    {
        cJSON* item = cJSON_CreateObject();
        AddMasterServerServiceMod(item, version);
        cJSON_AddItemToArray(items, item);
    }
    return HarnessProtocol::JsonResponse(resp);
}

std::string AnswerMasterServerModServersQuery(cJSON* root)
{
    const char* modId = HarnessProtocol::GetString(root, "modId");
    if (!modId || !modId[0])
    {
        return HarnessProtocol::ErrorResponse("modId required");
    }

    const RString& masterServer = ResolveHarnessMasterServerHost();
    if (masterServer.GetLength() == 0)
    {
        return HarnessProtocol::ErrorResponse("no master-server configured");
    }

    std::vector<MasterServerServiceModUsageServer> servers;
    if (!FetchMasterServerServiceModServers(masterServer, modId, nullptr, servers))
    {
        return HarnessProtocol::ErrorResponse("master-server mod servers fetch failed");
    }

    cJSON* resp = cJSON_CreateObject();
    cJSON* items = cJSON_AddArrayToObject(resp, "servers");
    for (const auto& server : servers)
    {
        cJSON* item = cJSON_CreateObject();
        AddMasterServerServiceUsageServer(item, server);
        cJSON_AddItemToArray(items, item);
    }
    return HarnessProtocol::JsonResponse(resp);
}

std::string AnswerMasterServerModListQuery(cJSON* root)
{
    const RString& masterServer = ResolveHarnessMasterServerHost();
    if (masterServer.GetLength() == 0)
    {
        return HarnessProtocol::ErrorResponse("no master-server configured");
    }

    const char* query = HarnessProtocol::GetString(root, "q");

    std::vector<MasterServerServiceModCatalogEntry> mods;
    if (!FetchMasterServerServiceModList(masterServer, query, nullptr, mods))
    {
        return HarnessProtocol::ErrorResponse("master-server mod list fetch failed");
    }

    cJSON* resp = cJSON_CreateObject();
    cJSON* items = cJSON_AddArrayToObject(resp, "mods");
    for (const auto& mod : mods)
    {
        cJSON* item = cJSON_CreateObject();
        AddMasterServerServiceMod(item, mod);
        cJSON_AddItemToArray(items, item);
    }
    cJSON_AddNumberToObject(resp, "count", static_cast<int>(mods.size()));
    return HarnessProtocol::JsonResponse(resp);
}

// Read a JSON string array into normalized ModIds (skips blanks).
static std::vector<ModId> ReadModIdArray(cJSON* root, const char* field)
{
    std::vector<ModId> ids;
    cJSON* arr = cJSON_GetObjectItem(root, field);
    if (arr != nullptr && cJSON_IsArray(arr))
    {
        cJSON* it = nullptr;
        cJSON_ArrayForEach(it, arr)
        {
            if (cJSON_IsString(it) && it->valuestring != nullptr)
            {
                ModId id(it->valuestring);
                if (!id.Empty())
                {
                    ids.push_back(std::move(id));
                }
            }
        }
    }
    return ids;
}

// mp_resolve: fetch the live catalog, then resolve a server's required mod list
// (`mod` + `equalmod`) against the player's `installed`/`active` arrays via the OOP
// ServerModResolver. Returns the diff the MP-join UI would render/act on.
std::string AnswerMpResolveQuery(cJSON* root)
{
    const RString& masterServer = ResolveHarnessMasterServerHost();
    if (masterServer.GetLength() == 0)
    {
        return HarnessProtocol::ErrorResponse("no master-server configured");
    }

    std::vector<MasterServerServiceModCatalogEntry> mods;
    if (!FetchMasterServerServiceModList(masterServer, nullptr, nullptr, mods))
    {
        return HarnessProtocol::ErrorResponse("master-server mod list fetch failed");
    }

    ModCatalog catalog;
    for (const auto& m : mods)
    {
        catalog.Add(ModCatalogEntry(ModId(m.modId), m.name, m.downloadUrl, m.sizeBytes));
    }

    const char* serverMod = HarnessProtocol::GetString(root, "mod");
    const ServerModList server(serverMod != nullptr ? serverMod : "",
                               HarnessProtocol::GetBool(root, "equalmod", false));
    const ServerModResolver resolver(ReadModIdArray(root, "installed"), ReadModIdArray(root, "active"));
    const ServerModResolution res = resolver.Resolve(server, catalog);

    cJSON* resp = cJSON_CreateObject();
    cJSON* sat = cJSON_AddArrayToObject(resp, "satisfied");
    for (const ModId& id : res.Satisfied())
    {
        cJSON_AddItemToArray(sat, cJSON_CreateString(id.Value().c_str()));
    }
    cJSON* dl = cJSON_AddArrayToObject(resp, "toDownload");
    for (const ModDownload& d : res.ToDownload())
    {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", d.id.Value().c_str());
        cJSON_AddStringToObject(item, "name", d.name.c_str());
        cJSON_AddStringToObject(item, "url", d.downloadUrl.c_str());
        cJSON_AddNumberToObject(item, "size", static_cast<double>(d.sizeBytes));
        cJSON_AddItemToArray(dl, item);
    }
    cJSON* dis = cJSON_AddArrayToObject(resp, "toDisable");
    for (const ModId& id : res.ToDisable())
    {
        cJSON_AddItemToArray(dis, cJSON_CreateString(id.Value().c_str()));
    }
    cJSON* blk = cJSON_AddArrayToObject(resp, "blocked");
    for (const ModId& id : res.Blocked())
    {
        cJSON_AddItemToArray(blk, cJSON_CreateString(id.Value().c_str()));
    }
    cJSON_AddBoolToObject(resp, "needsWork", res.NeedsWork());
    cJSON_AddBoolToObject(resp, "canProceed", res.CanProceed());
    cJSON_AddNumberToObject(resp, "catalogCount", static_cast<int>(mods.size()));
    return HarnessProtocol::JsonResponse(resp);
}

// mp_join: arm the deferred connect to <address>:<port> and request a mod-apply
// re-mount with <modpath> (the required set, already resolved + downloaded via
// mp_resolve + download). The AppIdle pending-connect hook finishes the join once
// the rebuilt menu is up. Returns immediately; the join completes over later frames.
std::string AnswerMpJoinQuery(cJSON* root)
{
    const char* address = HarnessProtocol::GetString(root, "address");
    if (address == nullptr || address[0] == '\0')
    {
        return HarnessProtocol::ErrorResponse("mp_join requires address");
    }
    const int port = HarnessProtocol::GetInt(root, "port", 2302);
    const char* password = HarnessProtocol::GetString(root, "password");
    const char* modpath = HarnessProtocol::GetString(root, "modpath");

    GPendingConnect().Arm(address, port, password != nullptr ? password : "");
    // Re-mount only when a modpath KEY is present; absent → arm + connect at the menu
    // without re-mounting (isolates the connect path from the re-mount in tests).
    const bool remount = cJSON_GetObjectItem(root, "modpath") != nullptr;
    if (remount && GApp != nullptr)
    {
        GApp->RequestRemountWithMods(modpath != nullptr ? modpath : "");
    }

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "armed", true);
    cJSON_AddBoolToObject(resp, "remount", remount);
    cJSON_AddStringToObject(resp, "address", address);
    cJSON_AddNumberToObject(resp, "port", port);
    cJSON_AddStringToObject(resp, "modpath", modpath != nullptr ? modpath : "");
    return HarnessProtocol::JsonResponse(resp);
}

std::string AnswerMasterServerListQuery(cJSON* root)
{
    const RString& masterServer = ResolveHarnessMasterServerHost();
    if (masterServer.GetLength() == 0)
    {
        return HarnessProtocol::ErrorResponse("no master-server configured");
    }

    // Optional filter — pointers stay valid for the synchronous fetch below.
    MasterServerBrowserFilter filter{};
    const char* serverName = HarnessProtocol::GetString(root, "serverName");
    const char* missionName = HarnessProtocol::GetString(root, "missionName");
    if (serverName)
        filter.serverName = serverName;
    if (missionName)
        filter.missionName = missionName;
    filter.minPlayers = HarnessProtocol::GetInt(root, "minPlayers");
    filter.maxPlayers = HarnessProtocol::GetInt(root, "maxPlayers");
    cJSON* includeFull = cJSON_GetObjectItem(root, "includeFullServers");
    filter.includeFullServers = includeFull == nullptr || cJSON_IsTrue(includeFull);

    MasterServerBrowser* browser = CreateMasterServerBrowser(masterServer, nullptr, nullptr);
    if (!browser)
    {
        return HarnessProtocol::ErrorResponse("master-server browser create failed");
    }

    // The browser fetch runs on a worker thread; this harness query is
    // synchronous, so pump Think until it lands (bounded ~6s wait).
    UpdateMasterServerBrowser(browser, filter);
    for (int i = 0; i < 600 && GetMasterServerBrowserState(browser) != MasterServerBrowserState::Idle; ++i)
    {
        ThinkMasterServerBrowser(browser);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    cJSON* resp = cJSON_CreateObject();
    cJSON* items = cJSON_AddArrayToObject(resp, "servers");
    const int count = GetMasterServerBrowserCount(browser);
    for (int i = 0; i < count; i++)
    {
        MasterServerSessionInfo info;
        if (!TryGetMasterServerBrowserSession(browser, i, info))
        {
            continue;
        }
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "address", info.address);
        cJSON_AddNumberToObject(item, "hostport", info.hostPort);
        cJSON_AddStringToObject(item, "hostname", info.hostName);
        cJSON_AddStringToObject(item, "gametype", info.mission);
        cJSON_AddNumberToObject(item, "actver", info.actualVersion);
        cJSON_AddNumberToObject(item, "reqver", info.requiredVersion);
        cJSON_AddStringToObject(item, "vertag", info.versionTag);
        cJSON_AddNumberToObject(item, "state", info.gameState);
        cJSON_AddNumberToObject(item, "ping", info.ping);
        cJSON_AddNumberToObject(item, "numplayers", info.numPlayers);
        cJSON_AddNumberToObject(item, "maxplayers", info.maxPlayers);
        cJSON_AddBoolToObject(item, "password", info.password);
        cJSON_AddStringToObject(item, "mod", info.mod);
        cJSON_AddBoolToObject(item, "equalmod", info.equalModRequired);
        cJSON_AddItemToArray(items, item);
    }
    cJSON_AddNumberToObject(resp, "count", count);

    DestroyMasterServerBrowser(browser);
    return HarnessProtocol::JsonResponse(resp);
}

std::string AnswerDownloadQuery(cJSON* root)
{
    const char* url = HarnessProtocol::GetString(root, "url");
    const char* dest = HarnessProtocol::GetString(root, "dest");
    if (!url || !url[0])
    {
        return HarnessProtocol::ErrorResponse("url required");
    }
    if (!dest || !dest[0])
    {
        return HarnessProtocol::ErrorResponse("dest required");
    }

    struct DownloadProgress
    {
        int64_t received = 0;
        int64_t total = 0;
        int calls = 0;
    };
    DownloadProgress progress;

    const DownloadFileResult result =
        DownloadMasterServerServiceFile(url, nullptr, dest, &progress,
                                        [](void* instance, int64_t received, int64_t total)
                                        {
                                            auto* p = static_cast<DownloadProgress*>(instance);
                                            p->received = received;
                                            p->total = total;
                                            ++p->calls;
                                        });
    if (result != DownloadFileResult::Success)
    {
        return HarnessProtocol::ErrorResponse("download failed");
    }

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "received", static_cast<double>(progress.received));
    cJSON_AddNumberToObject(resp, "total", static_cast<double>(progress.total));
    cJSON_AddNumberToObject(resp, "progressCalls", progress.calls);
    return HarnessProtocol::JsonResponse(resp);
}
} // namespace

std::string AnswerServiceQuery(const char* what, cJSON* root)
{
    if (what && std::strcmp(what, "master_server_server_detail") == 0)
    {
        return AnswerMasterServerServerDetailQuery(root);
    }
    if (what && std::strcmp(what, "master_server_mod_detail") == 0)
    {
        return AnswerMasterServerModDetailQuery(root);
    }
    if (what && std::strcmp(what, "master_server_mod_versions") == 0)
    {
        return AnswerMasterServerModVersionsQuery(root);
    }
    if (what && std::strcmp(what, "master_server_mod_servers") == 0)
    {
        return AnswerMasterServerModServersQuery(root);
    }
    if (what && std::strcmp(what, "master_server_mod_list") == 0)
    {
        return AnswerMasterServerModListQuery(root);
    }
    if (what && std::strcmp(what, "master_server_list") == 0)
    {
        return AnswerMasterServerListQuery(root);
    }
    if (what && std::strcmp(what, "mp_resolve") == 0)
    {
        return AnswerMpResolveQuery(root);
    }
    if (what && std::strcmp(what, "mp_join") == 0)
    {
        return AnswerMpJoinQuery(root);
    }
    if (what && std::strcmp(what, "download") == 0)
    {
        return AnswerDownloadQuery(root);
    }
    return {};
}

void RegisterKeyInjection(HarnessServer& hs)
{
    hs.RegisterCommand({"mouse_motion", "Inject relative SDL mouse movement", {{"dx","int",true},{"dy","int",true}}},
        [](const std::string&, cJSON* root) -> std::string {
            const int dx = HarnessProtocol::GetInt(root,"dx");
            const int dy = HarnessProtocol::GetInt(root,"dy");
            if (dx < -1000 || dx > 1000 || dy < -1000 || dy > 1000)
                return HarnessProtocol::ErrorResponse("mouse movement out of range");
            SDL_Event event{};
            event.type = SDL_EVENT_MOUSE_MOTION;
            event.motion.xrel = static_cast<float>(dx);
            event.motion.yrel = static_cast<float>(dy);
            if (!SDL_PushEvent(&event)) return HarnessProtocol::ErrorResponse("mouse event rejected");
            return HarnessProtocol::OkResponse();
        });
    hs.RegisterCommand({"mouse_button", "Inject SDL mouse button state", {{"button","int",true},{"down","bool",true}}},
        [](const std::string&, cJSON* root) -> std::string {
            const int button = HarnessProtocol::GetInt(root,"button");
            if (button < 1 || button > 5) return HarnessProtocol::ErrorResponse("invalid mouse button");
            SDL_Event event{};
            event.button.down = HarnessProtocol::GetBool(root,"down");
            event.type = event.button.down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            event.button.button = static_cast<Uint8>(button);
            if (!SDL_PushEvent(&event)) return HarnessProtocol::ErrorResponse("mouse event rejected");
            return HarnessProtocol::OkResponse();
        });
    hs.RegisterCommand(
        {"key", "Inject SDL scancode key press", {{"sc", "int", true}, {"mod", "int", false}, {"hold", "bool", false}}},
        [](const std::string&, cJSON* root) -> std::string
        {
            unsigned sc = static_cast<unsigned>(HarnessProtocol::GetInt(root, "sc"));
            unsigned mod = static_cast<unsigned>(HarnessProtocol::GetInt(root, "mod"));
            bool hold = HarnessProtocol::GetBool(root, "hold");
            SDL_Event ev = {};
            ev.type = SDL_EVENT_KEY_DOWN;
            ev.key.scancode = static_cast<SDL_Scancode>(sc);
            ev.key.key = SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(sc), SDL_KMOD_NONE, false);
            ev.key.mod = static_cast<SDL_Keymod>(mod);
            ev.key.down = true;
            SDL_PushEvent(&ev);
            if (!hold)
            {
                ev.type = SDL_EVENT_KEY_UP;
                ev.key.down = false;
                SDL_PushEvent(&ev);
            }
            return HarnessProtocol::OkResponse();
        });
    hs.RegisterCommand({"key_up", "Release a held key", {{"sc", "int", true}}},
                       [](const std::string&, cJSON* root) -> std::string
                       {
                           unsigned sc = static_cast<unsigned>(HarnessProtocol::GetInt(root, "sc"));
                           SDL_Event ev = {};
                           ev.type = SDL_EVENT_KEY_UP;
                           ev.key.scancode = static_cast<SDL_Scancode>(sc);
                           ev.key.key = SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(sc), SDL_KMOD_NONE, false);
                           ev.key.down = false;
                           SDL_PushEvent(&ev);
                           return HarnessProtocol::OkResponse();
                       });
}

namespace
{
// Build the JSON for `query what=display` from the active display.
// Walks idcs -1..1999 — covers the entire harness / production idc
// range with room to spare; ranges higher than that aren't used.
std::string AnswerUIDisplayQuery(UITestEngine& uiTestEngine)
{
    cJSON* resp = cJSON_CreateObject();
    auto* container = uiTestEngine.GetActiveDisplay();
    cJSON_AddNumberToObject(resp, "idd", container ? container->IDD() : -1);
    if (container)
    {
        cJSON* controls = cJSON_AddArrayToObject(resp, "controls");
        for (int idc = -1; idc < 2000; idc++)
        {
            IControl* ctrl = container->GetCtrl(idc);
            if (!ctrl)
                continue;
            cJSON* c = cJSON_CreateObject();
            cJSON_AddNumberToObject(c, "idc", ctrl->IDC());
            cJSON_AddStringToObject(c, "type", UITestEngine::GetControlTypeName(ctrl));
            cJSON_AddBoolToObject(c, "visible", ctrl->IsVisible());
            cJSON_AddBoolToObject(c, "enabled", ctrl->IsEnabled());
            auto* control = dynamic_cast<Control*>(ctrl);
            if (control)
            {
                cJSON* pos = cJSON_CreateObject();
                cJSON_AddNumberToObject(pos, "x", control->X());
                cJSON_AddNumberToObject(pos, "y", control->Y());
                cJSON_AddNumberToObject(pos, "w", control->W());
                cJSON_AddNumberToObject(pos, "h", control->H());
                cJSON_AddItemToObject(c, "pos", pos);
            }
            std::string text = UITestEngine::GetControlText(ctrl);
            if (!text.empty())
                cJSON_AddStringToObject(c, "text", text.c_str());
            cJSON_AddItemToArray(controls, c);
        }
    }
    return HarnessProtocol::JsonResponse(resp);
}
} // namespace

void RegisterUIQuery(HarnessServer& hs, UITestEngine& uiTestEngine)
{
    // Capture the underlying pointer by value rather than the parameter
    // reference — the parameter goes out of scope when this function
    // returns, but the lambda lives on inside HarnessServer's command
    // map.  The pointed-to objects (m_uiTestEngine in the calling app)
    // outlive the server, so the captured pointer stays valid.
    UITestEngine* ui = &uiTestEngine;
    hs.RegisterCommand({"query", "Query UI state (what: display)", {{"what", "string", true}}},
                       [ui](const std::string&, cJSON* root) -> std::string
                       {
                           const char* what = HarnessProtocol::GetString(root, "what");
                           if (what && std::strcmp(what, "display") == 0)
                               return AnswerUIDisplayQuery(*ui);
                           return HarnessProtocol::ErrorResponse("unknown query target");
                       });
}

void RegisterWaitDisplay(HarnessServer& hs, UITestEngine& uiTestEngine)
{
    HarnessServer* hsPtr = &hs;
    UITestEngine* ui = &uiTestEngine;
    hs.RegisterCommand(
        {"wait_display", "Block until display IDD appears", {{"idd", "int", true}, {"timeout_ms", "int", false}}},
        [hsPtr, ui](const std::string& name, cJSON* root) -> std::string
        {
            int targetIDD = HarnessProtocol::GetInt(root, "idd", -1);
            int timeoutMs = HarnessProtocol::GetInt(root, "timeout_ms", 30000);
            int currentIDD = ui->GetActiveDisplayIDD();
            if (currentIDD == targetIDD)
            {
                cJSON* resp = cJSON_CreateObject();
                cJSON_AddNumberToObject(resp, "idd", currentIDD);
                return HarnessProtocol::JsonResponse(resp);
            }
            if (timeoutMs <= 0)
                return HarnessProtocol::ErrorResponse("wait_display timeout");
            cJSON_ReplaceItemInObjectCaseSensitive(root, "timeout_ms", cJSON_CreateNumber(timeoutMs - 16));
            char* updated = cJSON_PrintUnformatted(root);
            HarnessCommand retry;
            retry.name = name;
            retry.raw = updated;
            cJSON_free(updated);
            hsPtr->ReEnqueueCommand(retry);
            return "";
        });
}

} // namespace HarnessBuiltins
} // namespace Poseidon::Dev
