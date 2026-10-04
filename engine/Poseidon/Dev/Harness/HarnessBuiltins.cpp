#include <Poseidon/Dev/Harness/HarnessBuiltins.hpp>
#include <Poseidon/Input/InputSubsystem.hpp>
#include <Poseidon/Input/KeyInput.hpp>
using namespace Poseidon;
#include <Poseidon/Dev/Harness/HarnessServer.hpp>
#include <Poseidon/Dev/Harness/HarnessProtocol.hpp>

#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/Core/ITerrainRenderer.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/WorldInputContext.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Weather/AirflowField.hpp>
#include <Poseidon/World/Weather/WindModel.hpp>
#include <Poseidon/World/Weather/SnowField.hpp>
#include <Poseidon/World/Weather/MudField.hpp>
#include <Poseidon/World/Weather/SandField.hpp>
#include <Poseidon/World/Weather/RainWaterField.hpp>
#include <Poseidon/World/Weather/SunlightDrying.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/World/Weather/SandGroundAdmission.hpp>
#include <Poseidon/World/Weather/MudGroundAdmission.hpp>
#include <Poseidon/World/Entities/Infantry/UniformWetness.hpp>
#include <Poseidon/World/Weather/RainVolume.hpp>
#include <Poseidon/World/Physics/PhysicsWorld.hpp>
#include <Poseidon/Dev/Diag/PhysicsCorpus.hpp>
#include <Poseidon/Dev/Diag/TerrainBrush.hpp>
#include <Poseidon/Dev/Diag/CaveEditor.hpp>
#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/World/Terrain/TerrainCrater.hpp>
#include <Poseidon/Dev/Diag/StreamingDiag.hpp>
#include <Poseidon/Graphics/Textures/PreparedTextures.hpp>
#include <Poseidon/Graphics/Textures/DeferredProceduralAdmission.hpp>
#include <Poseidon/IO/Streams/ArchiveSourceBinding.hpp>
#include <Poseidon/World/Terrain/WarmTextureProvenance.hpp>
#include <Poseidon/Dev/Diag/BallisticsRecorder.hpp>
#include <Poseidon/Dev/Diag/FrameProfiler.hpp>
#include <Poseidon/Dev/Diag/RainWaterCapture.hpp>
#include <fstream>
#include <Poseidon/Core/Version.hpp>
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
#include <Poseidon/World/Entities/Vehicles/Air/Helicopter.hpp>
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
#include <cfloat>
#include <cstdlib>
#include <string>
#include <thread>

namespace Poseidon
{
extern Input GInput;
}

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
    const auto selectedWarmStageProbe = [](const std::string&, cJSON*) -> std::string {
            if (!Foundation::IsMainThread() || !GEngine)
                return HarnessProtocol::ErrorResponse("owner renderer required");
            const auto report = GEngine->ProbeWarmPatnikStage();
            auto* result = cJSON_CreateObject();
            const char* statuses[] = {"Disabled", "WrongOwner", "Unsupported", "Ready"};
            const auto ordinal = static_cast<unsigned>(report.status);
            cJSON_AddStringToObject(result, "status", ordinal < 4 ? statuses[ordinal] : "Unsupported");
            const auto target = Streaming::WarmTextureProvenance::TargetKey();
            cJSON_AddStringToObject(result, "source", target.empty() ? "" : target.data());
#define PATNIK_BOOL(field) cJSON_AddBoolToObject(result, #field, report.field)
            PATNIK_BOOL(textureFound); PATNIK_BOOL(targetKeyMatched);
            PATNIK_BOOL(gpuResident); PATNIK_BOOL(uploadTried);
            PATNIK_BOOL(initializedMips); PATNIK_BOOL(initializedBinding);
            PATNIK_BOOL(currentBinding); PATNIK_BOOL(canWarmPrepare);
            PATNIK_BOOL(storeEntry); PATNIK_BOOL(storeChain); PATNIK_BOOL(storeWarmBound);
            PATNIK_BOOL(storePublicationValid); PATNIK_BOOL(storeWithinTtl);
            PATNIK_BOOL(storeSameMember); PATNIK_BOOL(storeUploadedMark);
            PATNIK_BOOL(traceEnabled); PATNIK_BOOL(traceTruncated);
#undef PATNIK_BOOL
            cJSON_AddNumberToObject(result, "storeGeneration", static_cast<double>(report.storeGeneration));
            cJSON_AddNumberToObject(result, "traceExactToken", report.traceExactToken);
            cJSON_AddNumberToObject(result, "traceSources", report.traceSources);
            cJSON_AddNumberToObject(result, "traceRows", report.traceRows);
            cJSON_AddStringToObject(result, "scope", "Existing bank and exact mounted member only; store peek does not Take or sweep. Trace token is exact only while currentBinding=true; numeric events require matching token and untruncated rows. No upload, eviction, pixels or general source freshness claim.");
            return HarnessProtocol::JsonResponse(result);
        };
    hs.RegisterCommand({"stream_warm_stage_probe", "Read-only selected fixed material-stage texture/store state", {}},
        selectedWarmStageProbe);
    hs.RegisterCommand({"stream_warm_patnik_probe", "Legacy alias for selected fixed material-stage probe", {}},
        selectedWarmStageProbe);
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
            auto* trace = Streaming::WarmTextureProvenance::Active();
            if (trace) trace->FlushPromotions(); // all logging outside preparer/store mutexes
            if (trace) trace->FlushRetirements(); // bounded diagnostic cut outside store mutex
            const auto traceSummary = trace ? trace->Observe() : Streaming::WarmTextureProvenance::Summary{};
            const auto snapshot = ArchiveSourceBinding::SnapshotStats();
            auto* result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "enabled", snapshot.enabled);
            cJSON_AddBoolToObject(result, "cacheHandoffEnabled", snapshot.cacheHandoffEnabled);
#define ARCHIVE_BINDING_FIELD(field) cJSON_AddNumberToObject(result, #field, static_cast<double>(snapshot.field))
            ARCHIVE_BINDING_FIELD(liveBindings); ARCHIVE_BINDING_FIELD(knownCppBytes);
            ARCHIVE_BINDING_FIELD(capBindings); ARCHIVE_BINDING_FIELD(capBytes);
            ARCHIVE_BINDING_FIELD(wrappedReads); ARCHIVE_BINDING_FIELD(initRetains);
            ARCHIVE_BINDING_FIELD(captureRefused); ARCHIVE_BINDING_FIELD(capacityRefused);
            ARCHIVE_BINDING_FIELD(allocationRefused);
            ARCHIVE_BINDING_FIELD(weakCells); ARCHIVE_BINDING_FIELD(weakKnownCppBytes); ARCHIVE_BINDING_FIELD(capWeakCells);
            ARCHIVE_BINDING_FIELD(weakCapacityRefused); ARCHIVE_BINDING_FIELD(weakAllocationRefused);
            ARCHIVE_BINDING_FIELD(weakWrappersPublished); ARCHIVE_BINDING_FIELD(weakInitializedHandoffs);
            ARCHIVE_BINDING_FIELD(weakFailedInitCompletions);
            ARCHIVE_BINDING_FIELD(weakPeakReservationCells); ARCHIVE_BINDING_FIELD(weakPeakReservationBytes);
            ARCHIVE_BINDING_FIELD(traceKnownCppBytes);
            cJSON_AddBoolToObject(result, "warmTraceEnabled", trace != nullptr);
            cJSON_AddNumberToObject(result, "warmTraceSources", traceSummary.sources);
            cJSON_AddNumberToObject(result, "warmTraceRows", traceSummary.rows);
            cJSON_AddBoolToObject(result, "warmTraceTruncated", traceSummary.truncated);
#undef ARCHIVE_BINDING_FIELD
            cJSON_AddStringToObject(result, "scope", "Mapped raw PAA Init evidence; liveBindings counts strong native leases, weakCells counts separately charged cache/control metadata within the same total byte cap. Cumulative publication/first Init completion survives cell destruction; peakReservation is charged metadata overlap, not allocator/RSS. Expired cached evidence remains Unknown; no freshness, physical-memory, useful-upload or performance proof");
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
    hs.RegisterCommand({"geometry_generated_admission", "One-shot actual generated texture capture/stage/escape diagnostic", {}},
        [](const std::string&, cJSON*) -> std::string {
            namespace A=render::procedural::admission;
            const char* fixture=std::getenv("WGR_GEOMETRY_PAGE_FIXTURE");
            if(!Foundation::IsMainThread()||!GEngine||!GWorld||GWorld->GetMode()==GModeNetware||
               !GEngine->TextBank()||!fixture||std::strcmp(fixture,"1")||!A::Enabled())
                return HarnessProtocol::ErrorResponse("requires owner-thread singleplayer and both exact private/generated flags");
            static bool used=false;
            if(used)return HarnessProtocol::ErrorResponse("one generated admission diagnostic per process");
            used=true;
            try {
                auto ledger=std::make_shared<A::Ledger>();auto* bank=GEngine->TextBank();
                Ref<Texture> staged,escaped;
                {
                    A::CaptureScope scope(ledger,true);
                    staged=bank->Load("#(argb,7,9,3)color(0.137,0.271,0.413,1)");
                }
                const auto held=ledger->Read();
                const bool capturedHeld=staged&&!staged->IsGpuResident()&&held.deferred==1&&held.pendingCount==1&&
                    held.pendingBytes>0&&held.pendingBytes<=A::MaxPendingBytes&&!held.aborted;
                {
                    A::StageScope scope(ledger);
                    if(staged)bank->UseMipmap(staged,0,0);
                }
                const auto owner=ledger->Read();
                const bool ownerResident=staged&&staged->IsGpuResident()&&owner.staged==1&&owner.escaped==0&&
                    owner.pendingCount==0&&owner.pendingBytes==0&&!owner.aborted;
                if(staged)bank->UseMipmap(staged,0,0);
                const bool repeatStable=ledger->Read().staged==owner.staged;
                {
                    A::CaptureScope scope(ledger,true);
                    escaped=bank->Load("#(argb,7,9,3)color(0.173,0.317,0.467,1)");
                }
                const bool escapedHeld=escaped&&!escaped->IsGpuResident()&&ledger->Read().pendingCount==1;
                if(escaped)bank->UseMipmap(escaped,0,0); // Ordinary consumer retains ordinary rendering.
                const auto final=ledger->Read();
                const bool escapedResident=escaped&&escaped->IsGpuResident()&&final.deferred==2&&final.staged==1&&
                    final.escaped==1&&final.aborted&&!final.failed&&!final.immediateFallback&&!final.nameMismatch&&
                    final.pendingCount==0&&final.pendingBytes==0;
                auto eager=bank->Load("#(argb,7,9,3)color(0.193,0.353,0.491,1)");
                const bool eagerResident=eager&&eager->IsGpuResident()&&ledger->Read().deferred==2;
                auto* result=cJSON_CreateObject();
                cJSON_AddBoolToObject(result,"passed",capturedHeld&&ownerResident&&repeatStable&&escapedHeld&&escapedResident&&eagerResident);
                cJSON_AddBoolToObject(result,"capturedHeld",capturedHeld);cJSON_AddBoolToObject(result,"ownerResident",ownerResident);
                cJSON_AddBoolToObject(result,"repeatStable",repeatStable);cJSON_AddBoolToObject(result,"escapedHeld",escapedHeld);
                cJSON_AddBoolToObject(result,"escapedResident",escapedResident);cJSON_AddBoolToObject(result,"eagerResident",eagerResident);
                cJSON_AddNumberToObject(result,"deferred",double(final.deferred));cJSON_AddNumberToObject(result,"staged",double(final.staged));
                cJSON_AddNumberToObject(result,"escaped",double(final.escaped));cJSON_AddNumberToObject(result,"failed",double(final.failed));
                cJSON_AddNumberToObject(result,"heldBytes",double(held.pendingBytes));cJSON_AddNumberToObject(result,"pendingBytes",double(final.pendingBytes));
                cJSON_AddStringToObject(result,"scope","Actual TextureBank Load/UseMipmap generated holding, one owner create and ordinary escape create; no complete material/model admission or driver completion proof");
                return HarnessProtocol::JsonResponse(result);
            }catch(...){return HarnessProtocol::ErrorResponse("generated admission diagnostic allocation failed");}
        });
    hs.RegisterCommand({"geometry_page_fixture", "Controlled authored resident geometry page draw fixture", {}},
        [](const std::string&, cJSON* root) -> std::string {
            if(!Foundation::IsMainThread() || !GEngine || !GWorld || GWorld->GetMode()==GModeNetware)
                return HarnessProtocol::ErrorResponse("requires owner-thread singleplayer; allocation additionally requires startup WGR_GEOMETRY_PAGE_FIXTURE=1");
            const char* name=HarnessProtocol::GetString(root,"action");
            if(!name) return HarnessProtocol::ErrorResponse("fixture action required");
            Engine::GeometryPageFixtureAction action;
            if(!std::strcmp(name,"begin")) action=Engine::GeometryPageFixtureAction::Begin;
            else if(!std::strcmp(name,"beginClod")) action=Engine::GeometryPageFixtureAction::BeginClod;
            else if(!std::strcmp(name,"beginPaged")) action=Engine::GeometryPageFixtureAction::BeginPaged;
            else if(!std::strcmp(name,"beginClodPaged")) action=Engine::GeometryPageFixtureAction::BeginClodPaged;
            else if(!std::strcmp(name,"beginDiskClodPaged")) action=Engine::GeometryPageFixtureAction::BeginDiskClodPaged;
            else if(!std::strcmp(name,"beginOriginalDiskClodPaged")) action=Engine::GeometryPageFixtureAction::BeginOriginalDiskClodPaged;
            else if(!std::strcmp(name,"beginOriginalHierarchy")) action=Engine::GeometryPageFixtureAction::BeginOriginalHierarchy;
            else if(!std::strcmp(name,"beginOriginalHierarchyDisk")) action=Engine::GeometryPageFixtureAction::BeginOriginalHierarchyDisk;
            else if(!std::strcmp(name,"stepHierarchy")) action=Engine::GeometryPageFixtureAction::StepHierarchy;
            else if(!std::strcmp(name,"rootHierarchy")) action=Engine::GeometryPageFixtureAction::RootHierarchy;
            else if(!std::strcmp(name,"fineHierarchy")) action=Engine::GeometryPageFixtureAction::FineHierarchy;
            else if(!std::strcmp(name,"intermediateHierarchy")) action=Engine::GeometryPageFixtureAction::IntermediateHierarchy;
            else if(!std::strcmp(name,"localHierarchy")) action=Engine::GeometryPageFixtureAction::LocalHierarchy;
            else if(!std::strcmp(name,"releaseHierarchyNonroot")) action=Engine::GeometryPageFixtureAction::ReleaseHierarchyNonroot;
            else if(!std::strcmp(name,"projectHierarchy")) action=Engine::GeometryPageFixtureAction::ProjectHierarchy;
            else if(!std::strcmp(name,"followHierarchy")) action=Engine::GeometryPageFixtureAction::FollowHierarchy;
            else if(!std::strcmp(name,"stopHierarchy")) action=Engine::GeometryPageFixtureAction::StopHierarchy;
            else if(!std::strcmp(name,"probeTextureReuseOnly")) action=Engine::GeometryPageFixtureAction::ProbeTextureReuseOnly;
            else if(!std::strcmp(name,"probeRetailOdol7")) action=Engine::GeometryPageFixtureAction::ProbeRetailOdol7;
            else if(!std::strcmp(name,"probeRetailColdWorld")) action=Engine::GeometryPageFixtureAction::ProbeRetailColdWorld;
            else if(!std::strcmp(name,"beginRetailWorldVisible")) action=Engine::GeometryPageFixtureAction::BeginRetailWorldVisible;
            else if(!std::strcmp(name,"pollRetailWorldVisible")) action=Engine::GeometryPageFixtureAction::PollRetailWorldVisible;
            else if(!std::strcmp(name,"rootRetailWorldVisible")) action=Engine::GeometryPageFixtureAction::RootRetailWorldVisible;
            else if(!std::strcmp(name,"fineRetailWorldVisible")) action=Engine::GeometryPageFixtureAction::FineRetailWorldVisible;
            else if(!std::strcmp(name,"restoreRetailWorldVisible")) action=Engine::GeometryPageFixtureAction::RestoreRetailWorldVisible;
            else if(!std::strcmp(name,"moveRetailWorldVisible")) action=Engine::GeometryPageFixtureAction::MoveRetailWorldVisible;
            else if(!std::strcmp(name,"removeRetailWorldVisible")) action=Engine::GeometryPageFixtureAction::RemoveRetailWorldVisible;
            else if(!std::strcmp(name,"followRetailWorldVisible")) action=Engine::GeometryPageFixtureAction::FollowRetailWorldVisible;
            else if(!std::strcmp(name,"stopRetailWorldVisible")) action=Engine::GeometryPageFixtureAction::StopRetailWorldVisible;
            else if(!std::strcmp(name,"pollRetailRecords")) action=Engine::GeometryPageFixtureAction::PollRetailRecords;
            else if(!std::strcmp(name,"beginRetailVisible")) action=Engine::GeometryPageFixtureAction::BeginRetailVisible;
            else if(!std::strcmp(name,"rootRetailVisible")) action=Engine::GeometryPageFixtureAction::RootRetailVisible;
            else if(!std::strcmp(name,"fineRetailVisible")) action=Engine::GeometryPageFixtureAction::FineRetailVisible;
            else if(!std::strcmp(name,"pollRetailVisible")) action=Engine::GeometryPageFixtureAction::PollRetailVisible;
            else if(!std::strcmp(name,"removeRetailVisible")) action=Engine::GeometryPageFixtureAction::RemoveRetailVisible;
            else if(!std::strcmp(name,"fallbackRetailVisible")) action=Engine::GeometryPageFixtureAction::FallbackRetailVisible;
            else if(!std::strcmp(name,"releaseRetailRecords")) action=Engine::GeometryPageFixtureAction::ReleaseRetailRecords;
            else if(!std::strcmp(name,"refillRetailRecords")) action=Engine::GeometryPageFixtureAction::RefillRetailRecords;
            else if(!std::strcmp(name,"abortRetailRecords")) action=Engine::GeometryPageFixtureAction::AbortRetailRecords;
            else if(!std::strcmp(name,"statusRetailRecords")) action=Engine::GeometryPageFixtureAction::SnapshotRetailRecords;
            else if(!std::strcmp(name,"followCamera")) action=Engine::GeometryPageFixtureAction::FollowCamera;
            else if(!std::strcmp(name,"followProjected")) action=Engine::GeometryPageFixtureAction::FollowProjected;
            else if(!std::strcmp(name,"stopCamera")) action=Engine::GeometryPageFixtureAction::StopCamera;
            else if(!std::strcmp(name,"requestCoarse")) action=Engine::GeometryPageFixtureAction::RequestCoarse;
            else if(!std::strcmp(name,"pollCoarse")) action=Engine::GeometryPageFixtureAction::PollCoarse;
            else if(!std::strcmp(name,"requestFine")) action=Engine::GeometryPageFixtureAction::RequestFine;
            else if(!std::strcmp(name,"pollFine")) action=Engine::GeometryPageFixtureAction::PollFine;
            else if(!std::strcmp(name,"cancelFine")) action=Engine::GeometryPageFixtureAction::CancelFine;
            else if(!std::strcmp(name,"holdFine")) action=Engine::GeometryPageFixtureAction::HoldFine;
            else if(!std::strcmp(name,"releaseFine")) action=Engine::GeometryPageFixtureAction::ReleaseFine;
            else if(!std::strcmp(name,"evictFine")) action=Engine::GeometryPageFixtureAction::EvictFine;
            else if(!std::strcmp(name,"fallback")) action=Engine::GeometryPageFixtureAction::Fallback;
            else if(!std::strcmp(name,"coarse")) action=Engine::GeometryPageFixtureAction::Coarse;
            else if(!std::strcmp(name,"fine")) action=Engine::GeometryPageFixtureAction::Fine;
            else if(!std::strcmp(name,"observe")) action=Engine::GeometryPageFixtureAction::Observe;
            else if(!std::strcmp(name,"status")) action=Engine::GeometryPageFixtureAction::Snapshot;
            else if(!std::strcmp(name,"abort")) action=Engine::GeometryPageFixtureAction::Abort;
            else return HarnessProtocol::ErrorResponse("unknown fixture action");
            float xyz[3]={1200,11.5f,1200};
            const char* keys[3]={"x","y","z"};
            for(int i=0;i<3;++i) if(const auto* item=cJSON_GetObjectItemCaseSensitive(root,keys[i])) {
                if(!cJSON_IsNumber(item)||!std::isfinite(item->valuedouble)||std::fabs(item->valuedouble)>1e7)
                    return HarnessProtocol::ErrorResponse("fixture position must be finite and bounded");
                xyz[i]=float(item->valuedouble);
            }
            Engine::GeometryPageDiskFixtureInput disk;
            const Engine::GeometryPageDiskFixtureInput* diskArgument=nullptr;
            if(action==Engine::GeometryPageFixtureAction::BeginDiskClodPaged || action==Engine::GeometryPageFixtureAction::BeginOriginalDiskClodPaged) {
                const bool originalMode=action==Engine::GeometryPageFixtureAction::BeginOriginalDiskClodPaged;
                // These are externally captured producer keys, never refreshed from
                // the cache file or a manifest on the owner/frame thread.
                const auto* input=cJSON_GetObjectItemCaseSensitive(root,"disk");
                const auto* producer=input?cJSON_GetObjectItemCaseSensitive(input,"producer"):nullptr;
                const auto* path=input?cJSON_GetObjectItemCaseSensitive(input,"path"):nullptr;
                if(!cJSON_IsObject(input)||!cJSON_IsObject(producer)||!cJSON_IsString(path)||!path->valuestring)
                    return HarnessProtocol::ErrorResponse("disk path and trusted external producer object required");
                size_t length=0;
                while(length<disk.path.size() && path->valuestring[length]) {
                    const auto ch=static_cast<unsigned char>(path->valuestring[length]);
                    if(ch<32 || ch>126)return HarnessProtocol::ErrorResponse("disk pilot path must be bounded ASCII");
                    disk.path[length]=char(ch);++length;
                }
                if(!length || length==disk.path.size())return HarnessProtocol::ErrorResponse("disk pilot path exceeds 1023 characters");
                const auto number=[](const cJSON* object,const char* key,uint32_t& value) {
                    const auto* item=object?cJSON_GetObjectItemCaseSensitive(object,key):nullptr;
                    if(!cJSON_IsNumber(item)||!std::isfinite(item->valuedouble)||item->valuedouble<0 ||
                        item->valuedouble>UINT32_MAX || std::floor(item->valuedouble)!=item->valuedouble)return false;
                    value=uint32_t(item->valuedouble);return true;
                };
                const auto hexadecimal=[](const cJSON* object,const char* key,uint8_t* output,size_t bytes) {
                    const auto* item=object?cJSON_GetObjectItemCaseSensitive(object,key):nullptr;
                    if(!cJSON_IsString(item)||!item->valuestring)return false;
                    const auto digit=[](char c)->int {return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;};
                    for(size_t i=0;i<bytes;++i) {
                        // Read sequentially: a short string must not inspect beyond its NUL.
                        const char a=item->valuestring[i*2];if(!a)return false;
                        const char b=item->valuestring[i*2+1];if(!b)return false;
                        const int high=digit(a),low=digit(b);if(high<0||low<0)return false;
                        output[i]=uint8_t((high<<4)|low);
                    }
                    return item->valuestring[bytes*2]==0;
                };
                const auto word=[&](const cJSON* object,const char* key,uint64_t& value) {
                    uint8_t bytes[8];if(!hexadecimal(object,key,bytes,8))return false;
                    value=0;for(auto b:bytes)value=(value<<8)|b;return true;
                };
                const auto source=[&](const cJSON* object,Engine::GeometryPageSourceIdentity& key) {
                    return cJSON_IsObject(object) && hexadecimal(object,"sourceSha256",key.sha256.data(),32) &&
                        word(object,"geometryOptionsHex",key.geometryOptions) && word(object,"materialOptionsHex",key.materialOptions) &&
                        number(object,"producerVersion",key.producerVersion) && number(object,"coarseRepresentation",key.coarseRepresentation) &&
                        number(object,"fineRepresentation",key.fineRepresentation) && number(object,"vertexLayout",key.vertexLayout) && number(object,"materialMapping",key.materialMapping);
                };
                const auto* original=cJSON_GetObjectItemCaseSensitive(producer,"originalSource");
                const auto* selected=cJSON_GetObjectItemCaseSensitive(producer,"selectedCut");
                const auto* selectedSource=selected?cJSON_GetObjectItemCaseSensitive(selected,"source"):nullptr;
                const auto* packing=selected?cJSON_GetObjectItemCaseSensitive(selected,"packing"):nullptr;
                const auto* revision=cJSON_GetObjectItemCaseSensitive(producer,"clodLibraryRevision");
                if(!source(original,disk.original)||!source(selectedSource,disk.selected)||!cJSON_IsObject(packing) ||
                    !number(producer,"schemaVersion",disk.schemaVersion)||(!originalMode&&!number(producer,"controlledPilotVersion",disk.pilotVersion))||
                    !number(producer,"diskCodecSchema",disk.codecSchema)||!number(producer,"sVertexBytes",disk.vertexBytes)||
                    !word(producer,"sVertexLayoutKeyHex",disk.layoutKey)||!number(producer,"clodAdapterVersion",disk.clodAdapter)||
                    !number(producer,"ramAdapterVersion",disk.ramAdapter)||(!originalMode&&!hexadecimal(producer,"helperSha256",disk.helperSha256.data(),32))||
                    !number(selected,"formatVersion",disk.formatVersion)||!number(selected,"algorithmVersion",disk.algorithmVersion)||
                    !number(packing,"clusterVertices",disk.clusterVertices)||!number(packing,"clusterTriangles",disk.clusterTriangles)||
                    !number(packing,"pageBytes",disk.pageBytes)||!cJSON_IsString(revision)||!revision->valuestring)
                    return HarnessProtocol::ErrorResponse("complete exact disk pilot versions, source/cut keys, packing and helper hash required");
                if(originalMode) {
                    uint32_t admission=0,sourceBytes=0;
                    const auto* type=cJSON_GetObjectItemCaseSensitive(producer,"producer");
                    if(disk.schemaVersion!=2 || !cJSON_IsString(type)||!type->valuestring || std::strcmp(type->valuestring,"controlled-original-mlod") ||
                        !number(producer,"originalSubsetVersion",disk.originalSubsetVersion)||disk.originalSubsetVersion!=1 ||
                        !number(producer,"sourceBytes",sourceBytes)||!sourceBytes||sourceBytes>128*1024 ||
                        !number(root,"sourceAdmissionEpoch",admission)||!admission)
                        return HarnessProtocol::ErrorResponse("original source requires schema2/subset1, trusted byte length and startup sourceAdmissionEpoch");
                    disk.sourceAdmissionEpoch=admission;disk.originalSourceBytes=sourceBytes;
                }
                for(size_t i=0;i<40;++i) {
                    const char c=revision->valuestring[i];
                    if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))
                        return HarnessProtocol::ErrorResponse("clod library revision must be 40 hexadecimal characters");
                    disk.libraryRevision[i]=c;
                }
                if(revision->valuestring[40])return HarnessProtocol::ErrorResponse("clod library revision must be exactly 40 characters");
                diskArgument=&disk;
            }
            Engine::GeometryPageCameraDemandInput camera;
            const Engine::GeometryPageCameraDemandInput* cameraArgument=nullptr;
            if(action==Engine::GeometryPageFixtureAction::FollowCamera||action==Engine::GeometryPageFixtureAction::FollowProjected) {
                const char* rails[]={"nearDistance","farDistance"};double* values[]={&camera.nearDistance,&camera.farDistance};
                for(size_t i=0;i<2;++i)if(const auto* v=cJSON_GetObjectItemCaseSensitive(root,rails[i])){
                    if(!cJSON_IsNumber(v)||!std::isfinite(v->valuedouble)||v->valuedouble<=0||v->valuedouble>100000)
                        return HarnessProtocol::ErrorResponse("camera prefetch rails must be finite positive bounded distances");*values[i]=v->valuedouble;}
                const char* times[]={"dwellMs","retryMs"};uint32_t* milliseconds[]={&camera.dwellMs,&camera.retryMs};
                for(size_t i=0;i<2;++i)if(const auto* v=cJSON_GetObjectItemCaseSensitive(root,times[i])){
                    if(!cJSON_IsNumber(v)||!std::isfinite(v->valuedouble)||v->valuedouble<0||v->valuedouble>2000||std::floor(v->valuedouble)!=v->valuedouble)
                        return HarnessProtocol::ErrorResponse("camera prefetch intervals must be integer milliseconds <=2000");*milliseconds[i]=uint32_t(v->valuedouble);}
                if(action==Engine::GeometryPageFixtureAction::FollowProjected) {
                    const char* names[]={"enterUpper","leaveUpper"};double* values[]={&camera.enterUpper,&camera.leaveUpper};
                    for(size_t i=0;i<2;++i)if(const auto* v=cJSON_GetObjectItemCaseSensitive(root,names[i])){
                        if(!cJSON_IsNumber(v)||!std::isfinite(v->valuedouble)||v->valuedouble<0||v->valuedouble>10000)
                            return HarnessProtocol::ErrorResponse("projected prefetch thresholds must be finite nonnegative bounded values");*values[i]=v->valuedouble;}
                }
                cameraArgument=&camera;
            }
            Engine::GeometryPageHierarchyDemandInput hierarchy;
            const Engine::GeometryPageHierarchyDemandInput* hierarchyArgument=nullptr;
            if(action==Engine::GeometryPageFixtureAction::IntermediateHierarchy||
               action==Engine::GeometryPageFixtureAction::LocalHierarchy||
               action==Engine::GeometryPageFixtureAction::ReleaseHierarchyNonroot||
               action==Engine::GeometryPageFixtureAction::FollowHierarchy||
               action==Engine::GeometryPageFixtureAction::ProjectHierarchy) {
                const auto integer=[&](const char* key,uint64_t& value) {
                    const auto* item=cJSON_GetObjectItemCaseSensitive(root,key);
                    if(!cJSON_IsNumber(item)||!std::isfinite(item->valuedouble)||item->valuedouble<1||
                        item->valuedouble>9007199254740991.0||std::floor(item->valuedouble)!=item->valuedouble)return false;
                    value=uint64_t(item->valuedouble);return true;
                };
                uint64_t groups=0;
                if(!integer("sourceAdmissionEpoch",hierarchy.sourceAdmissionEpoch)||
                   !integer("pageEpoch",hierarchy.pageEpoch)||
                   !integer("expectedRequestId",hierarchy.expectedRequestId)||
                   !integer("groupCount",groups)||groups>64)
                    return HarnessProtocol::ErrorResponse("exact bounded hierarchy source, epoch, request and group count required");
                hierarchy.groupCount=uint32_t(groups);
                if(action==Engine::GeometryPageFixtureAction::ProjectHierarchy) {
                    const auto* allowance=cJSON_GetObjectItemCaseSensitive(root,"pixelIndicatorAllowance");
                    if(!integer("frameGeneration",hierarchy.frameGeneration)||
                       !cJSON_IsNumber(allowance)||!std::isfinite(allowance->valuedouble)||
                       allowance->valuedouble<=0||allowance->valuedouble>10000)
                        return HarnessProtocol::ErrorResponse("exact completed frame and bounded positive indicator allowance required");
                    hierarchy.pixelIndicatorAllowance=allowance->valuedouble;
                }
                const auto* hash=cJSON_GetObjectItemCaseSensitive(root,"packageSha256");
                if(!cJSON_IsString(hash)||!hash->valuestring||std::strlen(hash->valuestring)!=64)
                    return HarnessProtocol::ErrorResponse("exact hierarchy package SHA-256 required");
                const auto digit=[](char c)->int {
                    if(c>='0'&&c<='9')return c-'0';
                    if(c>='a'&&c<='f')return c-'a'+10;
                    if(c>='A'&&c<='F')return c-'A'+10;
                    return -1;
                };
                for(size_t i=0;i<32;++i) {
                    const int hi=digit(hash->valuestring[2*i]),lo=digit(hash->valuestring[2*i+1]);
                    if(hi<0||lo<0)return HarnessProtocol::ErrorResponse("hierarchy package SHA-256 must be hexadecimal");
                    hierarchy.packageSha256[i]=uint8_t(hi*16+lo);
                }
                const auto threshold=[&](const cJSON* item,float& value) {
                    if(!cJSON_IsNumber(item)||!std::isfinite(item->valuedouble)||item->valuedouble<0||
                        item->valuedouble>=double(FLT_MAX))return false;
                    value=float(item->valuedouble);
                    return std::isfinite(value)&&value<FLT_MAX;
                };
                if(action==Engine::GeometryPageFixtureAction::IntermediateHierarchy) {
                    float value=0;
                    if(!threshold(cJSON_GetObjectItemCaseSensitive(root,"threshold"),value))
                        return HarnessProtocol::ErrorResponse("finite hierarchy threshold required");
                    std::fill_n(hierarchy.groupThresholds.begin(),hierarchy.groupCount,value);
                } else if(action==Engine::GeometryPageFixtureAction::LocalHierarchy) {
                    const auto* values=cJSON_GetObjectItemCaseSensitive(root,"groupThresholds");
                    if(!cJSON_IsArray(values)||cJSON_GetArraySize(values)!=int(hierarchy.groupCount))
                        return HarnessProtocol::ErrorResponse("exact bounded hierarchy group thresholds required");
                    for(uint32_t i=0;i<hierarchy.groupCount;++i)
                        if(!threshold(cJSON_GetArrayItem(values,int(i)),hierarchy.groupThresholds[i]))
                            return HarnessProtocol::ErrorResponse("hierarchy group threshold must be finite nonnegative");
                }
                hierarchyArgument=&hierarchy;
            }
            Engine::GeometryPageHierarchyDiskFixtureInput hierarchyDisk;
            const Engine::GeometryPageHierarchyDiskFixtureInput* hierarchyDiskArgument=nullptr;
            if(action==Engine::GeometryPageFixtureAction::BeginOriginalHierarchyDisk) {
                const auto* input=cJSON_GetObjectItemCaseSensitive(root,"hierarchyDisk");
                if(!cJSON_IsObject(input))return HarnessProtocol::ErrorResponse("hierarchyDisk authority required");
                const auto boundedInteger=[](const cJSON* object,const char* key,uint64_t& output,uint64_t maximum,uint64_t minimum=1) {
                    const auto* item=cJSON_GetObjectItemCaseSensitive(object,key);
                    if(!cJSON_IsNumber(item)||!std::isfinite(item->valuedouble)||item->valuedouble<double(minimum)||
                       item->valuedouble>double(maximum)||std::floor(item->valuedouble)!=item->valuedouble)return false;
                    output=uint64_t(item->valuedouble);return true;
                };
                uint64_t metadataBytes=0;
                if(!boundedInteger(root,"sourceAdmissionEpoch",hierarchyDisk.sourceAdmissionEpoch,9007199254740991ULL)||
                   !boundedInteger(root,"originalSourceBytes",hierarchyDisk.originalSourceBytes,128*1024)||
                   !boundedInteger(input,"fileBytes",hierarchyDisk.fileBytes,Engine::GeometryPageHierarchyDiskFixtureInput::MaxFileBytes)||
                   !boundedInteger(input,"metadataBytes",metadataBytes,Engine::GeometryPageHierarchyDiskFixtureInput::MaxMetadataBytes))
                    return HarnessProtocol::ErrorResponse("bounded hierarchy disk sizes and source epoch required");
                const auto digit=[](char c)->int {
                    if(c>='0'&&c<='9')return c-'0';
                    if(c>='a'&&c<='f')return c-'a'+10;
                    if(c>='A'&&c<='F')return c-'A'+10;
                    return -1;
                };
                const auto hash=[&](const cJSON* object,const char* key,uint8_t* output) {
                    const auto* item=cJSON_GetObjectItemCaseSensitive(object,key);
                    if(!cJSON_IsString(item)||!item->valuestring||std::strlen(item->valuestring)!=64)return false;
                    for(size_t i=0;i<32;++i) {
                        const int hi=digit(item->valuestring[2*i]),lo=digit(item->valuestring[2*i+1]);
                        if(hi<0||lo<0)return false;output[i]=uint8_t(hi*16+lo);
                    }
                    return true;
                };
                const auto word=[&](const cJSON* object,const char* key,uint64_t& output) {
                    const auto* item=cJSON_GetObjectItemCaseSensitive(object,key);
                    if(!cJSON_IsString(item)||!item->valuestring||std::strlen(item->valuestring)!=16)return false;
                    uint64_t value=0;for(size_t i=0;i<16;++i) {
                        const int d=digit(item->valuestring[i]);if(d<0)return false;value=(value<<4)|uint64_t(d);
                    }
                    output=value;return true;
                };
                const auto* source=cJSON_GetObjectItemCaseSensitive(input,"source");
                uint64_t producer=0,coarse=0,fine=0,layout=0,mapping=0;
                if(!cJSON_IsObject(source)||
                   !hash(input,"packageSha256",hierarchyDisk.packageSha256.data())||
                   !hash(input,"metadataSha256",hierarchyDisk.metadataSha256.data())||
                   !hash(source,"sourceSha256",hierarchyDisk.original.sha256.data())||
                   !word(source,"geometryOptionsHex",hierarchyDisk.original.geometryOptions)||
                   !word(source,"materialOptionsHex",hierarchyDisk.original.materialOptions)||
                   !boundedInteger(source,"producerVersion",producer,UINT32_MAX)||
                   !boundedInteger(source,"coarseRepresentation",coarse,UINT32_MAX,0)||
                   !boundedInteger(source,"fineRepresentation",fine,UINT32_MAX,0)||
                   !boundedInteger(source,"vertexLayout",layout,UINT32_MAX)||
                   !boundedInteger(source,"materialMapping",mapping,UINT32_MAX))
                    return HarnessProtocol::ErrorResponse("exact hierarchy identity required");
                hierarchyDisk.original.producerVersion=uint32_t(producer);
                hierarchyDisk.original.coarseRepresentation=uint32_t(coarse);
                hierarchyDisk.original.fineRepresentation=uint32_t(fine);
                hierarchyDisk.original.vertexLayout=uint32_t(layout);
                hierarchyDisk.original.materialMapping=uint32_t(mapping);
                const auto* path=cJSON_GetObjectItemCaseSensitive(input,"path");
                const auto* hex=cJSON_GetObjectItemCaseSensitive(input,"metadataHex");
                if(!cJSON_IsString(path)||!path->valuestring||!cJSON_IsString(hex)||!hex->valuestring)
                    return HarnessProtocol::ErrorResponse("bounded hierarchy path and prefix required");
                size_t length=0;while(length<hierarchyDisk.path.size()&&path->valuestring[length]) {
                    const auto ch=static_cast<unsigned char>(path->valuestring[length]);
                    if(ch<32||ch>126)return HarnessProtocol::ErrorResponse("hierarchy path must be bounded ASCII");
                    hierarchyDisk.path[length]=char(ch);++length;
                }
                if(!length||length==hierarchyDisk.path.size())
                    return HarnessProtocol::ErrorResponse("hierarchy path exceeds 1023 bytes");
                size_t hexLength=0;while(hexLength<=2*Engine::GeometryPageHierarchyDiskFixtureInput::MaxMetadataBytes&&hex->valuestring[hexLength])++hexLength;
                if(hexLength!=2*metadataBytes)
                    return HarnessProtocol::ErrorResponse("hierarchy prefix hex length mismatch");
                hierarchyDisk.metadataBytes.resize(size_t(metadataBytes));
                for(size_t i=0;i<metadataBytes;++i) {
                    const int hi=digit(hex->valuestring[2*i]),lo=digit(hex->valuestring[2*i+1]);
                    if(hi<0||lo<0)return HarnessProtocol::ErrorResponse("hierarchy prefix must be hex");
                    hierarchyDisk.metadataBytes[i]=uint8_t(hi*16+lo);
                }
                hierarchyDiskArgument=&hierarchyDisk;
            }
            const auto report=GEngine->ControlGeometryPageFixture(action,xyz[0],xyz[1],xyz[2],diskArgument,cameraArgument,hierarchyArgument,hierarchyDiskArgument);
            const char* status="Failed";
            switch(report.status) {
                case Engine::GeometryReportStatus::Unsupported: status="Disabled"; break;
                case Engine::GeometryReportStatus::Pending: status="Pending"; break;
                case Engine::GeometryReportStatus::Busy: status="Busy"; break;
                case Engine::GeometryReportStatus::Ready: status="Ready"; break;
                case Engine::GeometryReportStatus::Invalid: status="Invalid"; break;
                case Engine::GeometryReportStatus::Failed: break;
            }
            auto* result=cJSON_CreateObject(); cJSON_AddStringToObject(result,"status",status);
            cJSON_AddNumberToObject(result,"requestId",double(report.requestId));
            cJSON_AddNumberToObject(result,"observedRequestId",double(report.observedRequestId));
            cJSON_AddNumberToObject(result,"knownPayloadBytes",double(report.knownPayloadBytes));
            cJSON_AddBoolToObject(result,"textureReuseProbe",report.textureReuseProbe);
            cJSON_AddBoolToObject(result,"textureReusePassed",report.textureReusePassed);
            cJSON_AddBoolToObject(result,"textureReuseHeldDenied",report.textureReuseHeldDenied);
            cJSON_AddBoolToObject(result,"textureReuseResidentStable",report.textureReuseResidentStable);
            cJSON_AddBoolToObject(result,"textureReuseUnexpectedDenied",report.textureReuseUnexpectedDenied);
            cJSON_AddBoolToObject(result,"textureReuseRetriesResident",report.textureReuseRetriesResident);
            cJSON_AddBoolToObject(result,"retailSourceProbe",report.retailSourceProbe);
            cJSON_AddBoolToObject(result,"retailSourceExported",report.retailSourceExported);
            cJSON_AddBoolToObject(result,"retailSourceOtherPassesRequired",report.retailSourceOtherPassesRequired);
            cJSON_AddBoolToObject(result,"retailRecordOnly",report.retailRecordOnly);
            cJSON_AddBoolToObject(result,"retailVisiblePilot",report.retailVisiblePilot);
            cJSON_AddBoolToObject(result,"retailVisiblePresent",report.retailVisiblePresent);
            cJSON_AddBoolToObject(result,"retailVisibleRemoved",report.retailVisibleRemoved);
            cJSON_AddBoolToObject(result,"retailVisibleReturned",report.retailVisibleReturned);
            cJSON_AddBoolToObject(result,"retailFineTriangleSetExact",report.retailFineTriangleSetExact);
            cJSON_AddStringToObject(result,"retailVisiblePhase",report.retailVisiblePhase.c_str());
            cJSON_AddBoolToObject(result,"retailWorldVisiblePilot",report.retailWorldVisiblePilot);
            cJSON_AddBoolToObject(result,"retailWorldOriginalOtherViews",report.retailWorldOriginalOtherViews);
            cJSON_AddBoolToObject(result,"retailWorldPagePresent",report.retailWorldPagePresent);
            cJSON_AddBoolToObject(result,"retailWorldRestored",report.retailWorldRestored);
            cJSON_AddBoolToObject(result,"retailWorldReturned",report.retailWorldReturned);
            cJSON_AddStringToObject(result,"retailWorldPhase",report.retailWorldPhase.c_str());
            cJSON_AddBoolToObject(result,"retailWorldAutoEnabled",report.retailWorldAutoEnabled);
            cJSON_AddBoolToObject(result,"retailWorldSurfaceCertified",report.retailWorldSurfaceCertified);
            cJSON_AddBoolToObject(result,"retailWorldProjectedBounded",report.retailWorldProjectedBounded);
            cJSON_AddBoolToObject(result,"retailWorldForcedFine",report.retailWorldForcedFine);
            cJSON_AddBoolToObject(result,"retailRecordDistinctCuts",report.retailRecordDistinctCuts);
            cJSON_AddBoolToObject(result,"retailPixelSourceVerified",report.retailPixelSourceVerified);
            cJSON_AddStringToObject(result,"retailPixelSourceSha256",report.retailPixelSourceSha256.c_str());
#define PAGE_FIXTURE_NUMBER(field) cJSON_AddNumberToObject(result,#field,double(report.field))
            PAGE_FIXTURE_NUMBER(retailWorldRequest);PAGE_FIXTURE_NUMBER(retailWorldPageBirth);PAGE_FIXTURE_NUMBER(retailWorldCameraGeneration);
            PAGE_FIXTURE_NUMBER(retailWorldOriginalInstance);PAGE_FIXTURE_NUMBER(retailWorldPageInstance);
            PAGE_FIXTURE_NUMBER(retailWorldPageModel);PAGE_FIXTURE_NUMBER(retailWorldPageRenderer);
            PAGE_FIXTURE_NUMBER(retailWorldSelectedTriangles);PAGE_FIXTURE_NUMBER(retailWorldSafeRootObservations);
            PAGE_FIXTURE_NUMBER(retailWorldProjectionStatus);PAGE_FIXTURE_NUMBER(retailWorldSurfaceStatus);
            PAGE_FIXTURE_NUMBER(retailWorldStableFrameGeneration);PAGE_FIXTURE_NUMBER(retailWorldAutoObservations);
            PAGE_FIXTURE_NUMBER(retailWorldAutoRefines);PAGE_FIXTURE_NUMBER(retailWorldAutoCoarsens);
            PAGE_FIXTURE_NUMBER(retailWorldProjectedUpper);PAGE_FIXTURE_NUMBER(retailWorldSurfaceUpper);
            cJSON_AddBoolToObject(result,"retailWorldResidencyEnabled",report.retailWorldResidencyEnabled);
            cJSON_AddBoolToObject(result,"retailWorldConventionalFallback",report.retailWorldConventionalFallback);
            cJSON_AddStringToObject(result,"retailWorldResidencyPhase",report.retailWorldResidencyPhase.c_str());
            PAGE_FIXTURE_NUMBER(retailWorldResidencyCycles);PAGE_FIXTURE_NUMBER(retailWorldResidencyTriggerCamera);
            PAGE_FIXTURE_NUMBER(retailWorldResidencyCycleLimit);PAGE_FIXTURE_NUMBER(retailWorldLastRetiredMeshes);
            PAGE_FIXTURE_NUMBER(retailWorldCompactedMeshes);PAGE_FIXTURE_NUMBER(retailWorldRetiredFineModels);
            PAGE_FIXTURE_NUMBER(retailWorldResidencyReservedBytes);PAGE_FIXTURE_NUMBER(retailWorldRetirementAckRequest);
            PAGE_FIXTURE_NUMBER(retailWorldRetirementRequest);PAGE_FIXTURE_NUMBER(retailWorldRefillRequest);
            PAGE_FIXTURE_NUMBER(retailWorldFallbackCameraGeneration);
            PAGE_FIXTURE_NUMBER(retailWorldFallbackRequest);PAGE_FIXTURE_NUMBER(retailWorldFallbackPageBirth);
            PAGE_FIXTURE_NUMBER(retailWorldFallbackInstanceEpoch);PAGE_FIXTURE_NUMBER(retailWorldRefillRequestAtFallback);
            PAGE_FIXTURE_NUMBER(retailWorldFallbackOriginalFlags);
            cJSON_AddBoolToObject(result,"retailWorldFallbackPageAbsent",report.retailWorldFallbackPageAbsent);
            PAGE_FIXTURE_NUMBER(retailVisibleBirth);PAGE_FIXTURE_NUMBER(retailVisibleRequest);PAGE_FIXTURE_NUMBER(retailVisibleFrame);
            PAGE_FIXTURE_NUMBER(retailVisibleRenderer);PAGE_FIXTURE_NUMBER(retailVisibleSlot);PAGE_FIXTURE_NUMBER(retailVisibleModel);
            PAGE_FIXTURE_NUMBER(retailVisibleReferenceSourceLevel);PAGE_FIXTURE_NUMBER(retailVisibleReferenceTriangles);PAGE_FIXTURE_NUMBER(retailVisibleCameraGeneration);
            cJSON_AddBoolToObject(result,"retailDiagnosticReferenceAllocated",report.retailDiagnosticReferenceAllocated);
            PAGE_FIXTURE_NUMBER(textureReuseAllowed);PAGE_FIXTURE_NUMBER(textureReuseDenied);PAGE_FIXTURE_NUMBER(textureReuseOwnerCreates);
            PAGE_FIXTURE_NUMBER(retailSourcePrepareStatus);PAGE_FIXTURE_NUMBER(retailSourceExportStatus);
            PAGE_FIXTURE_NUMBER(retailHierarchyProductStatus);PAGE_FIXTURE_NUMBER(retailHierarchyPages);
            PAGE_FIXTURE_NUMBER(retailRecordMeshesPresent);PAGE_FIXTURE_NUMBER(retailRecordMeshesAbsent);
            PAGE_FIXTURE_NUMBER(retailRecordModelsPresent);PAGE_FIXTURE_NUMBER(retailRecordModelsAbsent);
            PAGE_FIXTURE_NUMBER(retailRecordRequestId);PAGE_FIXTURE_NUMBER(retailRecordWorkerReads);PAGE_FIXTURE_NUMBER(retailRecordFreshIds);
            PAGE_FIXTURE_NUMBER(modelAdmissionAccepted);PAGE_FIXTURE_NUMBER(modelAdmissionRejected);PAGE_FIXTURE_NUMBER(modelAdmissionPending);
            PAGE_FIXTURE_NUMBER(producerModel); PAGE_FIXTURE_NUMBER(rendererModel);
            PAGE_FIXTURE_NUMBER(producerInstance); PAGE_FIXTURE_NUMBER(rendererInstance);
            PAGE_FIXTURE_NUMBER(meshCount); PAGE_FIXTURE_NUMBER(meshesPresent); PAGE_FIXTURE_NUMBER(meshesAbsent);
            PAGE_FIXTURE_NUMBER(coarseTriangles); PAGE_FIXTURE_NUMBER(fineTriangles); PAGE_FIXTURE_NUMBER(coarsePages); PAGE_FIXTURE_NUMBER(finePages);
            PAGE_FIXTURE_NUMBER(hierarchyPages);PAGE_FIXTURE_NUMBER(hierarchyRootPages);PAGE_FIXTURE_NUMBER(hierarchyResidentPages);
            PAGE_FIXTURE_NUMBER(hierarchyRetainedCutModels);
            PAGE_FIXTURE_NUMBER(hierarchyDiskPageReads);
            PAGE_FIXTURE_NUMBER(hierarchyReleaseCount);PAGE_FIXTURE_NUMBER(hierarchyRefillCount);
            PAGE_FIXTURE_NUMBER(hierarchyProjectedGeneration);PAGE_FIXTURE_NUMBER(hierarchyProjectedRequired);
            PAGE_FIXTURE_NUMBER(hierarchyProjectedOutside);PAGE_FIXTURE_NUMBER(hierarchyProjectedForcedFine);
            PAGE_FIXTURE_NUMBER(hierarchyProjectedAllowance);PAGE_FIXTURE_NUMBER(hierarchyProjectedMaximumIndicator);
            cJSON_AddBoolToObject(result,"hierarchyAutoEnabled",report.hierarchyAutoEnabled);
            cJSON_AddBoolToObject(result,"hierarchyAutoPending",report.hierarchyAutoPending);
            PAGE_FIXTURE_NUMBER(hierarchyAutoObservations);PAGE_FIXTURE_NUMBER(hierarchyAutoRefines);PAGE_FIXTURE_NUMBER(hierarchyAutoCoarsens);
            PAGE_FIXTURE_NUMBER(hierarchyAutoSafeRootObservations);
            cJSON_AddBoolToObject(result,"hierarchySourceVerticesExact",report.hierarchySourceVerticesExact);
            cJSON_AddBoolToObject(result,"hierarchyRetirementPending",report.hierarchyRetirementPending);
            cJSON_AddStringToObject(result,"hierarchyRetiredPageMaskExact",std::to_string(report.hierarchyRetiredPageMask).c_str());
            PAGE_FIXTURE_NUMBER(hierarchySelectedTriangles);PAGE_FIXTURE_NUMBER(hierarchyCutModel);PAGE_FIXTURE_NUMBER(hierarchyThreshold);
            PAGE_FIXTURE_NUMBER(hierarchyDemandGroupCount);
            PAGE_FIXTURE_NUMBER(hierarchyRequiredMask);PAGE_FIXTURE_NUMBER(hierarchyResidentMask);PAGE_FIXTURE_NUMBER(hierarchyMissingMask);
            cJSON_AddStringToObject(result,"hierarchyRequiredMaskExact",std::to_string(report.hierarchyRequiredMask).c_str());
            cJSON_AddStringToObject(result,"hierarchyResidentMaskExact",std::to_string(report.hierarchyResidentMask).c_str());
            cJSON_AddStringToObject(result,"hierarchyMissingMaskExact",std::to_string(report.hierarchyMissingMask).c_str());
            PAGE_FIXTURE_NUMBER(clodAdapterVersion); PAGE_FIXTURE_NUMBER(clodGroups); PAGE_FIXTURE_NUMBER(clodClusters);
            PAGE_FIXTURE_NUMBER(fallbackTriangles); PAGE_FIXTURE_NUMBER(authoredFineTriangles); PAGE_FIXTURE_NUMBER(clodBakeMs);
            PAGE_FIXTURE_NUMBER(coarseThreshold); PAGE_FIXTURE_NUMBER(fineThreshold);
            PAGE_FIXTURE_NUMBER(clodRamAdapterVersion);PAGE_FIXTURE_NUMBER(clodRamKnownSourceBytes);PAGE_FIXTURE_NUMBER(pageSourceKnownBytes);
            PAGE_FIXTURE_NUMBER(pageEpoch); PAGE_FIXTURE_NUMBER(pageRequest); PAGE_FIXTURE_NUMBER(pageReservedBytes);
            PAGE_FIXTURE_NUMBER(pageCompleted); PAGE_FIXTURE_NUMBER(pageCancelled); PAGE_FIXTURE_NUMBER(pageQueued);
            PAGE_FIXTURE_NUMBER(pageActive); PAGE_FIXTURE_NUMBER(pageReady); PAGE_FIXTURE_NUMBER(pageLiveJobs);
            PAGE_FIXTURE_NUMBER(expectedPresentMeshes); PAGE_FIXTURE_NUMBER(expectedAbsentMeshes);
            PAGE_FIXTURE_NUMBER(observedExpectedPresentMeshes); PAGE_FIXTURE_NUMBER(observedExpectedAbsentMeshes);
            PAGE_FIXTURE_NUMBER(fineGeneration); PAGE_FIXTURE_NUMBER(fineEvictionCount);
            PAGE_FIXTURE_NUMBER(retiredFineFirst); PAGE_FIXTURE_NUMBER(retiredFineCount);
            PAGE_FIXTURE_NUMBER(x); PAGE_FIXTURE_NUMBER(y); PAGE_FIXTURE_NUMBER(z);
            PAGE_FIXTURE_NUMBER(renderReturnStatus);
            PAGE_FIXTURE_NUMBER(sourceAdmissionEpoch);PAGE_FIXTURE_NUMBER(originalSourceBytes);PAGE_FIXTURE_NUMBER(sourceAdmissionKnownBytes);
            PAGE_FIXTURE_NUMBER(helperCount);PAGE_FIXTURE_NUMBER(originalOriginRadius);
            PAGE_FIXTURE_NUMBER(surfaceCertificateStatus);PAGE_FIXTURE_NUMBER(surfaceCertificateKnownBytes);PAGE_FIXTURE_NUMBER(selectedSurfaceUpper);
            PAGE_FIXTURE_NUMBER(cameraDemandRevision);PAGE_FIXTURE_NUMBER(cameraRequestedRevision);PAGE_FIXTURE_NUMBER(cameraObservations);
            PAGE_FIXTURE_NUMBER(cameraRequests);PAGE_FIXTURE_NUMBER(cameraCancels);PAGE_FIXTURE_NUMBER(cameraAttempts);
            PAGE_FIXTURE_NUMBER(cameraDistance);PAGE_FIXTURE_NUMBER(preparedProducerModel);
            PAGE_FIXTURE_NUMBER(projectedDemandGeneration);PAGE_FIXTURE_NUMBER(projectedDemandStatus);PAGE_FIXTURE_NUMBER(projectedDemandUpper);
            PAGE_FIXTURE_NUMBER(privateAutoFineCameraGeneration);PAGE_FIXTURE_NUMBER(privateAutoFineSourceRequest);
            PAGE_FIXTURE_NUMBER(privateAutoCoarseObservations);PAGE_FIXTURE_NUMBER(privateAutoCoarseCameraGeneration);PAGE_FIXTURE_NUMBER(privateAutoCoarseSourceRequest);PAGE_FIXTURE_NUMBER(privateAutoCoarseCertifiedUpper);
            PAGE_FIXTURE_NUMBER(cameraTupleStatus);PAGE_FIXTURE_NUMBER(cameraTupleIndex);PAGE_FIXTURE_NUMBER(cameraTupleSource);
            PAGE_FIXTURE_NUMBER(cameraTupleGeneration);PAGE_FIXTURE_NUMBER(cameraTupleModelBirth);PAGE_FIXTURE_NUMBER(cameraTupleCertificateGeneration);
            PAGE_FIXTURE_NUMBER(cameraTupleVersion);PAGE_FIXTURE_NUMBER(cameraTupleStructBytes);
            PAGE_FIXTURE_NUMBER(cameraTupleOutputWidth);PAGE_FIXTURE_NUMBER(cameraTupleOutputHeight);
            PAGE_FIXTURE_NUMBER(viewportWidth);PAGE_FIXTURE_NUMBER(viewportHeight);PAGE_FIXTURE_NUMBER(projectedSurfaceStatus);
            PAGE_FIXTURE_NUMBER(cameraTupleClipNear);PAGE_FIXTURE_NUMBER(cameraTupleProjectionXScale);PAGE_FIXTURE_NUMBER(cameraTupleProjectionYScale);
            PAGE_FIXTURE_NUMBER(projectedSurfaceDepthMinimum);PAGE_FIXTURE_NUMBER(projectedSurfaceXUpper);PAGE_FIXTURE_NUMBER(projectedSurfaceYUpper);PAGE_FIXTURE_NUMBER(projectedSurfaceEuclideanUpper);
#undef PAGE_FIXTURE_NUMBER
#define PAGE_FIXTURE_BOOL(field) cJSON_AddBoolToObject(result,#field,report.field)
            PAGE_FIXTURE_BOOL(clodPilot); PAGE_FIXTURE_BOOL(fallbackSelected);
            PAGE_FIXTURE_BOOL(hierarchicalPilot);
            PAGE_FIXTURE_BOOL(hierarchicalDiskPilot);
            PAGE_FIXTURE_BOOL(hierarchyLocalDemand);
            PAGE_FIXTURE_BOOL(pagedPilot); PAGE_FIXTURE_BOOL(fineResident); PAGE_FIXTURE_BOOL(fineEvictionPending);
            PAGE_FIXTURE_BOOL(diskPilot); PAGE_FIXTURE_BOOL(coarseResident);
            PAGE_FIXTURE_BOOL(originalFilePilot);PAGE_FIXTURE_BOOL(sourceSnapshotValidated);PAGE_FIXTURE_BOOL(sourceAdmissionConsumed);
            PAGE_FIXTURE_BOOL(surfaceCertificateAdmitted);PAGE_FIXTURE_BOOL(surfaceSelectedVerified);
            PAGE_FIXTURE_BOOL(cameraTupleEnabled);PAGE_FIXTURE_BOOL(cameraTupleValid);PAGE_FIXTURE_BOOL(projectedSurfaceIdealBound);
            cJSON_AddStringToObject(result,"projectedSurfaceScope","Read-only ideal nonjittered main1:1 projected selected-union bound; excludes authored fallback, shader floating-point/raster/pixel/visibility/attributes/all-pass quality and cut selection.");
            PAGE_FIXTURE_BOOL(projectedDemandActive);PAGE_FIXTURE_BOOL(projectedDemandHasBound);
            PAGE_FIXTURE_BOOL(privateAutoFineEnabled);PAGE_FIXTURE_BOOL(privateAutoFineCommitted);
            PAGE_FIXTURE_BOOL(privateAutoCoarseEnabled);PAGE_FIXTURE_BOOL(privateAutoCoarseCommitted);
            PAGE_FIXTURE_BOOL(cameraDemandActive);PAGE_FIXTURE_BOOL(cameraDesiredFine);PAGE_FIXTURE_BOOL(cameraHasObservation);PAGE_FIXTURE_BOOL(finePrepared);
            PAGE_FIXTURE_BOOL(active); PAGE_FIXTURE_BOOL(fineSelected); PAGE_FIXTURE_BOOL(cpuHelpersUnchanged);
            PAGE_FIXTURE_BOOL(mainFrameReturned); PAGE_FIXTURE_BOOL(cascadeConfigured); PAGE_FIXTURE_BOOL(localShadowConfigured);
#undef PAGE_FIXTURE_BOOL
            auto* unionMinimum=cJSON_AddArrayToObject(result,"selectedUnionMinimum");
            auto* unionMaximum=cJSON_AddArrayToObject(result,"selectedUnionMaximum");
            for(size_t i=0;i<3;++i){cJSON_AddItemToArray(unionMinimum,cJSON_CreateNumber(report.selectedUnionMinimum[i]));
                cJSON_AddItemToArray(unionMaximum,cJSON_CreateNumber(report.selectedUnionMaximum[i]));}
            auto* cameraPosition=cJSON_AddArrayToObject(result,"cameraTuplePosition");
            for(float coordinate:report.cameraTuplePosition)
                cJSON_AddItemToArray(cameraPosition,cJSON_CreateNumber(coordinate));
            cJSON_AddStringToObject(result,"surfaceCertificateScope","Verified selected packed coarse/fine union object-space distance only; excludes authored fallback, pixels, attributes and all-pass quality. surfaceSelectedVerified refers to the latest successful bound worker package, not render completion.");
            auto* history=cJSON_AddArrayToObject(result,"rendererMeshHandles");
            for(size_t i=0;i<std::min<size_t>(report.meshCount,64);++i)
                cJSON_AddItemToArray(history,cJSON_CreateString(std::to_string(report.rendererMeshHandles[i]).c_str()));
            auto* pass=cJSON_AddObjectToObject(result,"passFacts");
            if(report.demandViews.enabled) {
                const auto& d=report.demandViews;auto* views=cJSON_AddObjectToObject(result,"demandViews");
                cJSON_AddBoolToObject(views,"available",d.available);
                cJSON_AddBoolToObject(views,"mainPacketJoined",d.mainPacketJoined);
#define DEMAND_VIEW_NUMBER(field) cJSON_AddNumberToObject(views,#field,double(d.field))
                DEMAND_VIEW_NUMBER(status);DEMAND_VIEW_NUMBER(viewCount);DEMAND_VIEW_NUMBER(generation);
                DEMAND_VIEW_NUMBER(instanceEpoch);DEMAND_VIEW_NUMBER(required);DEMAND_VIEW_NUMBER(known);DEMAND_VIEW_NUMBER(unknown);
                DEMAND_VIEW_NUMBER(mainCameraIndex);DEMAND_VIEW_NUMBER(mainCameraSource);
#undef DEMAND_VIEW_NUMBER
                auto* rows=cJSON_AddArrayToObject(views,"rows");
                for(size_t i=0;i<36;++i)if(d.required&(uint64_t(1)<<i)) {
                    const auto& row=d.rows[i];auto* item=cJSON_CreateObject();cJSON_AddItemToArray(rows,item);
#define DEMAND_ROW_NUMBER(field) cJSON_AddNumberToObject(item,#field,double(row.field))
                    DEMAND_ROW_NUMBER(id);DEMAND_ROW_NUMBER(kind);DEMAND_ROW_NUMBER(status);DEMAND_ROW_NUMBER(provenance);
                    DEMAND_ROW_NUMBER(width);DEMAND_ROW_NUMBER(height);DEMAND_ROW_NUMBER(originX);DEMAND_ROW_NUMBER(originY);
                    DEMAND_ROW_NUMBER(flags);DEMAND_ROW_NUMBER(generation);
#undef DEMAND_ROW_NUMBER
                }
                cJSON_AddStringToObject(views,"scope","CPU camera provenance only; no COUNT/cache-image/automatic-demand/retirement authority");
            }
            cJSON_AddBoolToObject(pass,"valid",report.passFacts.valid);
#define PAGE_PASS_NUMBER(field) cJSON_AddNumberToObject(pass,#field,double(report.passFacts.field))
            PAGE_PASS_NUMBER(version); PAGE_PASS_NUMBER(structBytes); PAGE_PASS_NUMBER(enabled);
            PAGE_PASS_NUMBER(capabilities); PAGE_PASS_NUMBER(required); PAGE_PASS_NUMBER(pending); PAGE_PASS_NUMBER(recordedThisFrame);
            PAGE_PASS_NUMBER(frame); PAGE_PASS_NUMBER(instanceEpoch); PAGE_PASS_NUMBER(cascadeEpoch); PAGE_PASS_NUMBER(cascadeFrame);
            PAGE_PASS_NUMBER(giRsmEpoch); PAGE_PASS_NUMBER(giRsmFrame); PAGE_PASS_NUMBER(reflectionEpoch); PAGE_PASS_NUMBER(reflectionFrame); PAGE_PASS_NUMBER(localCount);
            PAGE_PASS_NUMBER(localDrawMask); PAGE_PASS_NUMBER(localValidMask); PAGE_PASS_NUMBER(interiorDrawMask); PAGE_PASS_NUMBER(interiorValidMask);
            PAGE_PASS_NUMBER(cascadeDrawMask); PAGE_PASS_NUMBER(cascadeCount);
#undef PAGE_PASS_NUMBER
            auto addEpochArray=[&](const char* name,const uint64_t* values,size_t count) {
                auto* array=cJSON_AddArrayToObject(pass,name);
                for(size_t i=0;i<count;++i) cJSON_AddItemToArray(array,cJSON_CreateNumber(double(values[i])));
            };
            addEpochArray("localEpochs",report.passFacts.localEpochs,24);
            addEpochArray("localFrames",report.passFacts.localFrames,24);
            addEpochArray("interiorEpochs",report.passFacts.interiorEpochs,5);
            addEpochArray("interiorFrames",report.passFacts.interiorFrames,5);
            cJSON_AddStringToObject(result,"helperHashBefore",report.helperHashBefore.c_str());
            cJSON_AddStringToObject(result,"helperHashAfter",report.helperHashAfter.c_str());
            cJSON_AddStringToObject(result,"sourceSha256",report.sourceSha256.c_str());
            cJSON_AddStringToObject(result,"clodLibraryRevision",report.clodLibraryRevision.c_str());
            cJSON_AddStringToObject(result,"pageWorkState",report.pageWorkState.c_str());
            cJSON_AddStringToObject(result,"diskReadStatus",report.diskReadStatus.c_str());
            cJSON_AddStringToObject(result,"diskRequestedRepresentation",report.diskRequestedRepresentation.c_str());
            cJSON_AddStringToObject(result,"selectedCutSha256",report.selectedCutSha256.c_str());
            cJSON_AddStringToObject(result,"hierarchyPackageSha256",report.hierarchyPackageSha256.c_str());
            auto* hierarchyClusters=cJSON_AddArrayToObject(result,"hierarchySelectedClusters");
            for(auto cluster:report.hierarchySelectedClusters)cJSON_AddItemToArray(hierarchyClusters,cJSON_CreateNumber(cluster));
            auto* hierarchyThresholds=cJSON_AddArrayToObject(result,"hierarchyDemandThresholds");
            for(uint32_t g=0;g<report.hierarchyDemandGroupCount&&g<64;++g)
                cJSON_AddItemToArray(hierarchyThresholds,cJSON_CreateNumber(report.hierarchyDemandThresholds[g]));
            auto* hierarchyGroupErrors=cJSON_AddArrayToObject(result,"hierarchyGroupErrors");
            for(uint32_t g=0;g<report.hierarchyDemandGroupCount&&g<64;++g)
                cJSON_AddItemToArray(hierarchyGroupErrors,cJSON_CreateNumber(report.hierarchyGroupErrors[g]));
            cJSON_AddStringToObject(result,"scope",report.retailWorldVisiblePilot ? "One actual source-bound world placement: original other-view row and private main-only page joined to fresh returned camera; CPU row facts, not pixels, GPU completion, general world routing, all-pass quality or performance acceptance" : report.retailVisiblePilot ? "Actual source-bound retail PAC and ODOL private main-camera instance plus fresh returned camera; no world replacement, same-placement other-pass coverage, pixel parity, device completion or performance acceptance" : report.retailRecordOnly ? "Actual source-bound retail PAC upload and ODOL final geometry page records; no instances, pixels, world routing, all-pass coverage, device completion or performance acceptance" : report.retailSourceProbe ? "Actual retail final Shape/Init source probe only; no world routing or all-pass coverage" : report.textureReuseProbe ? "Actual generated texture denial, frozen resident reuse and ordinary retry; no complete material/model authority" : "Private programmatically authored box/curved-grid pilot; explicit authored/selected-CLOD RAM, original-source hierarchy RAM or independently authenticated disk pages, or private selected-cut disk value-worker; hierarchy stages at most one page per action and selects only complete retained root/fine/intermediate/local cuts; disk expected keys supplied externally and raw pilot/helper source compared independently without owner file I/O/bake; explicit CLOD bake scratch/latency and physical GPU allocation peaks not hard-bounded; returned main frame and full-generation record cut, not all-pass/pixel, mounted-file provenance, queue-completion or device-free proof");
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
                GEOMETRY_REPORT_FIELD(persistentPrunedAllocations); GEOMETRY_REPORT_FIELD(persistentPrunedBorrowers);
                GEOMETRY_REPORT_FIELD(persistentRetirementCandidates); GEOMETRY_REPORT_FIELD(persistentCleanupKnownCapacityBytes);
                GEOMETRY_REPORT_FIELD(persistentDetachedHistory); GEOMETRY_REPORT_FIELD(persistentBorrowerKnownRecordBytes);
                GEOMETRY_REPORT_FIELD(persistentSelectedDiagnosticPins);
                cJSON_AddStringToObject(result, "persistentCleanupScope", "Diagnostic metadata history only; known bytes count the fixed candidate array and raw borrower records, excluding map overhead, allocator and RSS; no physical GPU release or reclaim proof");
                cJSON_AddBoolToObject(result, "parkedReportEnabled", report.parkedReportEnabled);
                cJSON_AddBoolToObject(result, "parkedCaptureComplete", report.parkedCaptureComplete);
                cJSON_AddBoolToObject(result, "parkedJoinComplete", report.parkedJoinComplete);
                cJSON_AddBoolToObject(result, "parkedTruncated", report.parkedTruncated);
                cJSON_AddNumberToObject(result, "parkedModelsVisited", static_cast<double>(report.parkedModelsVisited));
                cJSON_AddNumberToObject(result, "parkedModelsCaptured", static_cast<double>(report.parkedModelsCaptured));
                cJSON_AddNumberToObject(result, "parkedMetadataVisits", static_cast<double>(report.parkedMetadataVisits));
                cJSON_AddNumberToObject(result, "parkedInvalidEntries", static_cast<double>(report.parkedInvalidEntries));
                cJSON_AddNumberToObject(result, "parkedLinkVisits", static_cast<double>(report.parkedLinkVisits));
                cJSON_AddNumberToObject(result, "parkedUniqueAllocations", static_cast<double>(report.parkedUniqueAllocations));
                cJSON_AddNumberToObject(result, "parkedKnownCandidateBytes", static_cast<double>(report.parkedKnownCandidateBytes));
                cJSON_AddNumberToObject(result, "parkedCpuBorrowedAllocations", static_cast<double>(report.parkedCpuBorrowedAllocations));
                cJSON_AddNumberToObject(result, "parkedCpuBorrowedBytes", static_cast<double>(report.parkedCpuBorrowedBytes));
                cJSON_AddNumberToObject(result, "parkedUnborrowedAllocations", static_cast<double>(report.parkedUnborrowedAllocations));
                cJSON_AddNumberToObject(result, "parkedUnborrowedBytes", static_cast<double>(report.parkedUnborrowedBytes));
                cJSON_AddNumberToObject(result, "parkedOutsideLinkedAllocations", static_cast<double>(report.parkedOutsideLinkedAllocations));
                cJSON_AddNumberToObject(result, "parkedOutsideLinkedBytes", static_cast<double>(report.parkedOutsideLinkedBytes));
                cJSON_AddNumberToObject(result, "parkedAbsentRetiredAllocations", static_cast<double>(report.parkedAbsentRetiredAllocations));
                cJSON_AddNumberToObject(result, "parkedUnknownAllocations", static_cast<double>(report.parkedUnknownAllocations));
                cJSON_AddNumberToObject(result, "parkedUnknownKnownBytes", static_cast<double>(report.parkedUnknownKnownBytes));
                cJSON_AddNumberToObject(result, "parkedRequestKnownCapacityBytes", static_cast<double>(report.parkedRequestKnownCapacityBytes));
                cJSON_AddNumberToObject(result, "parkedLinkKnownCapacityBytes", static_cast<double>(report.parkedLinkKnownCapacityBytes));
                cJSON_AddStringToObject(result, "parkedScope", "Earlier producer zero-ref selection joined with one renderer-drain CPU/model-link/mesh-record cut; logical candidates, not reclaimability or complete ownership");
                cJSON_AddStringToObject(result, "parkedMetadataScope", "Known request/vector capacities only; excludes allocator, transient joins, kernel and RSS");
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
            cJSON_AddNumberToObject(result, "coldAlphaPeeks", static_cast<double>(
                Poseidon::Dev::GTextureStreamCounters().coldAlphaPeeks.load(std::memory_order_relaxed)));
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
            cJSON_AddNumberToObject(result, "shapeCacheSize", static_cast<double>(state.shapeCacheSize));
            cJSON_AddNumberToObject(result, "shapeCacheLimit", static_cast<double>(state.shapeCacheLimit));
            cJSON_AddNumberToObject(result, "shapeCacheHits", static_cast<double>(state.shapeCacheHits));
            cJSON_AddNumberToObject(result, "shapeCacheInserts", static_cast<double>(state.shapeCacheInserts));
            cJSON_AddNumberToObject(result, "shapeCacheDrops", static_cast<double>(state.shapeCacheDrops));
            Engine::GpuMemoryStatsOut gpu{};
            const bool gpuValid = GEngine && GEngine->GetGpuMemoryStats(gpu);
            cJSON_AddBoolToObject(result, "gpuMemoryValid", gpuValid);
            if (gpuValid)
            {
                cJSON_AddNumberToObject(result, "gpuTrackedBytes", static_cast<double>(gpu.trackedBytes));
                cJSON_AddNumberToObject(result, "gpuBudgetBytes", static_cast<double>(gpu.budgetBytes));
                cJSON_AddBoolToObject(result, "gpuOverBudget", gpu.overBudget);
                cJSON_AddNumberToObject(result, "geometryLiveBytes", static_cast<double>(gpu.geometryLiveBytes));
                cJSON_AddNumberToObject(result, "geometryCapacityBytes", static_cast<double>(gpu.geometryCapacityBytes));
                cJSON_AddNumberToObject(result, "geometryRetiredBytes", static_cast<double>(gpu.geometryRetiredBytes));
                cJSON_AddNumberToObject(result, "objectTextureBytes", static_cast<double>(gpu.objectTextureBytes));
                cJSON_AddNumberToObject(result, "objectTextureCount", gpu.objectTextureCount);
                cJSON_AddNumberToObject(result, "backendAllocationBytes", static_cast<double>(gpu.backendAllocationBytes));
            }
            return HarnessProtocol::JsonResponse(result);
        });
    hs.RegisterCommand({"frame_performance", "Read-only published renderer timings and completed CPU frame window; never waits for GPU", {}},
        [](const std::string&, cJSON*) -> std::string {
            if (!GEngine) return HarnessProtocol::ErrorResponse("no renderer");
            auto* result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, "version", GetVersionString().Data());
            cJSON_AddStringToObject(result, "renderer", GEngine->GetRendererName().Data());
            cJSON_AddStringToObject(result, "scope", "Latest nonblocking published GPU/CPU encode timings; rows may lag game state and are not same-frame GPU completion proof. CPU window contains at most256 completed main-loop frames. Negative timing is unavailable, never zero cost.");
            if (GEngine->SupportsTemporalTuning()) {
                const auto ti = GEngine->GetTemporalInfo();
                cJSON_AddNumberToObject(result, "renderWidth", ti.renderWidth);
                cJSON_AddNumberToObject(result, "renderHeight", ti.renderHeight);
                cJSON_AddNumberToObject(result, "outputWidth", ti.outputWidth);
                cJSON_AddNumberToObject(result, "outputHeight", ti.outputHeight);
                cJSON_AddNumberToObject(result, "msaaSamples", ti.msaaSamples);
                cJSON_AddNumberToObject(result, "activeUpscaler", ti.activeUpscaler);
            }
            constexpr int cap = Engine::kGpuRegionEnd;
            float gpu[cap], cpu[cap];
            std::fill_n(gpu, cap, -1.f); std::fill_n(cpu, cap, -1.f);
            const int gpuCount = std::clamp(GEngine->GetWaterGpuTimings(gpu, cap), 0, cap);
            const int cpuCount = std::clamp(GEngine->GetCpuTimings(cpu, cap), 0, cap);
            cJSON_AddBoolToObject(result, "gpuTimestampsAvailable", gpuCount > 0);
            auto* rows = cJSON_AddArrayToObject(result, "regions");
            for (int i = 0; i < std::max(gpuCount, cpuCount); ++i) {
                auto* row = cJSON_CreateObject();
                cJSON_AddNumberToObject(row, "index", i);
                cJSON_AddStringToObject(row, "name", GEngine->GetWaterGpuTimingName(i));
                cJSON_AddNumberToObject(row, "gpuMs", i < gpuCount && std::isfinite(gpu[i]) ? gpu[i] : -1.f);
                cJSON_AddNumberToObject(row, "cpuMs", i < cpuCount && std::isfinite(cpu[i]) ? cpu[i] : -1.f);
                cJSON_AddNumberToObject(row, "containedBy", Engine::GpuTimerContainedBy(i));
                cJSON_AddBoolToObject(row, "container", Engine::IsGpuTimerContainer(i));
                cJSON_AddItemToArray(rows, row);
            }
            const auto& profiler = GFrameProfiler();
            cJSON_AddNumberToObject(result, "completedCpuFrames", profiler.FrameCount());
            const auto total = profiler.TotalStats();
            cJSON_AddNumberToObject(result, "cpuFrameAverageMs", total.avgMs);
            cJSON_AddNumberToObject(result, "cpuFrameP95Ms", total.p95Ms);
            cJSON_AddNumberToObject(result, "cpuFrameMaxMs", total.maxMs);
            auto* phases = cJSON_AddArrayToObject(result, "cpuPhases");
            for (int i = 0; i < FrameProfiler::PhaseCount; ++i) {
                const auto stats = profiler.Stats(static_cast<FrameProfiler::Phase>(i));
                auto* row = cJSON_CreateObject();
                cJSON_AddStringToObject(row, "name", FrameProfiler::PhaseName(i));
                cJSON_AddNumberToObject(row, "averageMs", stats.avgMs);
                cJSON_AddNumberToObject(row, "p95Ms", stats.p95Ms);
                cJSON_AddNumberToObject(row, "maxMs", stats.maxMs);
                cJSON_AddItemToArray(phases, row);
            }
            return HarnessProtocol::JsonResponse(result);
        });
    hs.RegisterCommand({"dev_wet_soil_diagnostic", "Owner-only opt-in actual wet-soil fragment colour modes; no source/history writes", {}},
        [](const std::string&, cJSON* root) -> std::string {
            if (!Foundation::IsMainThread() || !GEngine || !GLandscape || !GScene || !GScene->GetCamera())
                return HarnessProtocol::ErrorResponse("wet-soil diagnostic needs actual world and main-thread renderer owner");
            const char* action = HarnessProtocol::GetString(root, "action");
            const auto* value = cJSON_GetObjectItemCaseSensitive(root, "mode");
            int mode = -1;
            if (action && std::strcmp(action, "set") == 0) {
                if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
                    value->valuedouble < 0 || value->valuedouble > 4 ||
                    std::floor(value->valuedouble) != value->valuedouble)
                    return HarnessProtocol::ErrorResponse("wet-soil diagnostic mode must be an integer 0..4");
                mode = int(value->valuedouble);
            } else if (!action || std::strcmp(action, "state") != 0 || value) {
                return HarnessProtocol::ErrorResponse("wet-soil diagnostic action is state without mode, or set integer 0..4");
            }
            const int current = GEngine->ControlWetSoilDiagnostic(mode);
            if (current < 0) return HarnessProtocol::ErrorResponse("wet-soil diagnostic unavailable: needs owner, wgpu HDR and exact WGR_WET_SOIL_DIAGNOSTIC=1");
            auto* out = cJSON_CreateObject();
            cJSON_AddNumberToObject(out, "mode", current);
            const auto* camera = GScene->GetCamera();
            auto* pose = cJSON_CreateObject();
            const auto vector = [&](const char* name, const Vector3& v) {
                auto* values = cJSON_CreateArray();
                cJSON_AddItemToArray(values, cJSON_CreateNumber(v.X()));
                cJSON_AddItemToArray(values, cJSON_CreateNumber(v.Y()));
                cJSON_AddItemToArray(values, cJSON_CreateNumber(v.Z()));
                cJSON_AddItemToObject(pose, name, values);
            };
            vector("position", camera->Position());
            vector("direction", camera->Direction());
            vector("up", camera->DirectionUp());
            cJSON_AddItemToObject(out, "camera", pose);
            cJSON_AddStringToObject(out, "scope", "Producer enum queued in existing terrain padding. Colour modes expose actual fragment inputs with actual surviving water overlay and alpha; no forced material/wetness/shelter, GPU completion or numeric screenshot-channel proof.");
            return HarnessProtocol::JsonResponse(out);
        });
    hs.RegisterCommand({"terrain_coverage", "Read-only latest producer terrain selection and heightfield source", {}},
        [](const std::string&, cJSON*) -> std::string {
            if (!Foundation::IsMainThread() || !GEngine || !GLandscape)
                return HarnessProtocol::ErrorResponse("terrain coverage requires current main-thread world");
            auto* terrain = GEngine->GetTerrainRenderer();
            TerrainDrawCoverage coverage;
            if (!terrain || !terrain->GetDrawCoverage(coverage))
                return HarnessProtocol::ErrorResponse("no current admitted terrain selection");
            if (coverage.heightRevision != GLandscape->HeightRevision() ||
                coverage.terrainRange != GLandscape->GetTerrainRange() ||
                coverage.terrainGrid != GLandscape->GetTerrainGrid())
                return HarnessProtocol::ErrorResponse("terrain selection source is stale");
            auto* out = cJSON_CreateObject();
            cJSON_AddStringToObject(out, "world", GLandscape->GetName());
            cJSON_AddNumberToObject(out, "frame", static_cast<double>(coverage.frame));
            cJSON_AddNumberToObject(out, "heightRevision", coverage.heightRevision);
            cJSON_AddNumberToObject(out, "terrainRange", coverage.terrainRange);
            cJSON_AddNumberToObject(out, "terrainGrid", coverage.terrainGrid);
            cJSON_AddBoolToObject(out, "ready", coverage.ready);
            cJSON_AddBoolToObject(out, "background", coverage.background);
            cJSON_AddBoolToObject(out, "pagedMaterials", coverage.pagedMaterials);
            cJSON_AddNumberToObject(out, "patches", coverage.patches);
            cJSON_AddNumberToObject(out, "outsideLegacyRect", coverage.outsideLegacyRect);
            cJSON_AddNumberToObject(out, "cameraFar", coverage.cameraFar);
            cJSON_AddNumberToObject(out, "farthestPatchXZ", coverage.farthestPatch);
            const auto array = [&](const char* name, const auto& values) {
                auto* result = cJSON_CreateArray();
                for (auto value : values) cJSON_AddItemToArray(result, cJSON_CreateNumber(value));
                cJSON_AddItemToObject(out, name, result);
            };
            array("camera", coverage.camera); array("legacyRect", coverage.legacyRect);
            array("treeBounds", coverage.treeBounds); array("levels", coverage.levels);
            cJSON_AddStringToObject(out, "scope", "Latest producer-selected patches over the current uploaded heightfield. outsideLegacyRect counts patches wholly outside the old draw rectangle; farthestPatchXZ is horizontal corner reach. No GPU completion, residency completion or pixel proof.");
            return HarnessProtocol::JsonResponse(out);
        });
    hs.RegisterCommand({"weather_visibility", "Read-only weather, terrain and object draw ranges", {}},
        [](const std::string&, cJSON*) -> std::string {
            if (!GScene || !GLandscape) return HarnessProtocol::ErrorResponse("no world");
            auto* result = cJSON_CreateObject();
            cJSON_AddNumberToObject(result, "rain", GLandscape->GetRainDensity());
            cJSON_AddNumberToObject(result, "overcast", GLandscape->GetOvercast());
            // Sky look is producer-owned: NextFrame/PushRenderParams update it
            // before queue publication; the consumer uses frame-owned copies.
            // Never read this plain getter from the harness TCP/worker thread.
            const bool skyAvailable = Foundation::IsMainThread() && GEngine && GEngine->SupportsSky();
            cJSON_AddBoolToObject(result, "rendererSkyAvailable", skyAvailable);
            if (skyAvailable) {
                const auto sky = GEngine->GetSkySettings();
                auto* look = cJSON_CreateObject();
                cJSON_AddBoolToObject(look, "enabled", sky.enabled);
                cJSON_AddBoolToObject(look, "skyLighting", sky.skyLighting);
                cJSON_AddBoolToObject(look, "layeredFog", sky.layeredFog);
                const auto fogFields = [&](const char* name, std::initializer_list<float> values) {
                    auto* array = cJSON_CreateArray();
                    for (float value : values) cJSON_AddItemToArray(array, cJSON_CreateNumber(value));
                    cJSON_AddItemToObject(look, name, array);
                };
                fogFields("fogTerrain", {sky.fogTerrainFollow, sky.fogTerrainReference, sky.fogValleyStrength, sky.fogValleyRadius});
                fogFields("fogPatch", {sky.fogPatchStrength, sky.fogPatchHorizontalScale, sky.fogPatchVerticalScale, sky.fogCoverageFeather});
                fogFields("fogLayer0", {sky.fogLayerBase[0], sky.fogLayerTop[0], sky.fogLayerFeather[0], sky.fogLayerExtinction[0]});
                fogFields("fogLayer1", {sky.fogLayerBase[1], sky.fogLayerTop[1], sky.fogLayerFeather[1], sky.fogLayerExtinction[1]});
                fogFields("fogOptics", {sky.fogLayerAlbedo, sky.fogLayerG, sky.fogFarClose, sky.fogFalloff});
                cJSON_AddBoolToObject(look, "coverageFromWeather", sky.cloudCoverageFromWeather);
                cJSON_AddBoolToObject(look, "coveragePinned", std::getenv("WGR_CLOUD_COVERAGE") != nullptr);
                cJSON_AddNumberToObject(look, "coverage", sky.cloudCoverage);
                cJSON_AddNumberToObject(look, "clearCoverage", sky.cloudCoverageClear);
                cJSON_AddNumberToObject(look, "fullCoverage", sky.cloudCoverageFull);
                cJSON_AddStringToObject(look, "scope", "Latest producer-owned CPU sky settings, updated by render-parameter packing. May lag world weather; no frame identity, Rust acceptance, GPU completion or pixel/cloud-map proof. skyLighting is a look setting, not proof of the active HDR shader route.");
                cJSON_AddItemToObject(result, "rendererSky", look);
            }
            const auto* sun = GScene->MainLight();
            cJSON_AddBoolToObject(result, "sunAvailable", sun != nullptr);
            if (sun) {
                cJSON_AddNumberToObject(result, "sunTravelY", sun->SunDirection().Y());
                cJSON_AddNumberToObject(result, "nightEffect", sun->NightEffect());
                cJSON_AddNumberToObject(result, "solarDrying", SunlightDryingExposure(
                    sun->SunDirection().Y(), GLandscape->GetOvercast(), sun->NightEffect()));
            }
            cJSON_AddNumberToObject(result, "particleDensity", GRain.EffectiveDensity());
            cJSON_AddBoolToObject(result, "particleSnowflakes", GRain.Params().snowflakes);
            cJSON_AddNumberToObject(result, "liquidRain", UniformWettingDensity(GLandscape->GetRainDensity(),
                GRain.EffectiveDensity(), GRain.Params().snowflakes));
              cJSON_AddNumberToObject(result, "fog", GLandscape->GetFog());
              cJSON_AddNumberToObject(result, "snowAdjustedFog", GSnow().RenderFog(GLandscape->GetFog()));
            cJSON_AddNumberToObject(result, "terrainRange", GScene->GetFogMaxRange());
            cJSON_AddNumberToObject(result, "baseRange", GScene->GetBaseFogMaxRange());
            cJSON_AddNumberToObject(result, "objectRange", GScene->GetObjectDrawDistance());
            cJSON_AddNumberToObject(result, "tacticalVisibility", GScene->GetTacticalVisibility());
            cJSON_AddNumberToObject(result, "selectedRange", ENGINE_CONFIG.tacticalZ);
            return HarnessProtocol::JsonResponse(result);
        });
    hs.RegisterCommand({"weather_particles", "Query/set only local rain mode, density and snowflake diagnostics", {}},
        [](const std::string&, cJSON* root) -> std::string {
            if (!GLandscape) return HarnessProtocol::ErrorResponse("no landscape");
            const char* action = HarnessProtocol::GetString(root, "action");
            if (!action || (std::strcmp(action, "state") != 0 && std::strcmp(action, "set") != 0))
                return HarnessProtocol::ErrorResponse("weather_particles action must be state or set");
            auto params = GRain.Params();
            auto mode = GRain.Mode();
            const auto* modeItem = cJSON_GetObjectItemCaseSensitive(root, "mode");
            const auto* density = cJSON_GetObjectItemCaseSensitive(root, "density");
            const auto* flakes = cJSON_GetObjectItemCaseSensitive(root, "snowflakes");
            if (std::strcmp(action, "state") == 0 && (modeItem || density || flakes))
                return HarnessProtocol::ErrorResponse("state is read-only; use set for particle fields");
            if (std::strcmp(action, "set") == 0)
            {
                if (!modeItem && !density && !flakes)
                    return HarnessProtocol::ErrorResponse("set requires a particle field");
                if (modeItem)
                {
                    if (!cJSON_IsString(modeItem)) return HarnessProtocol::ErrorResponse("particle mode must be a string");
                    const char* value = modeItem->valuestring;
                    if (std::strcmp(value, "off") == 0) mode = RainOff;
                    else if (std::strcmp(value, "legacy") == 0) mode = RainLegacy;
                    else if (std::strcmp(value, "particle") == 0) mode = RainParticle;
                    else if (std::strcmp(value, "both") == 0) mode = RainBoth;
                    else return HarnessProtocol::ErrorResponse("unknown particle mode");
                }
                if (density)
                {
                    if (!cJSON_IsNumber(density) || !std::isfinite(density->valuedouble) ||
                        density->valuedouble < 0.0 || density->valuedouble > 1.0)
                        return HarnessProtocol::ErrorResponse("particle density must be finite in [0,1]");
                    params.densityOverride = float(density->valuedouble);
                }
                if (flakes)
                {
                    if (!cJSON_IsBool(flakes)) return HarnessProtocol::ErrorResponse("snowflakes must be boolean");
                    params.snowflakes = cJSON_IsTrue(flakes);
                }
                // Validate the complete candidate before changing either producer.
                GRain.SetParams(params);
                GRain.SetMode(mode);
            }
            const char* name = mode == RainOff ? "off" : mode == RainLegacy ? "legacy" : mode == RainBoth ? "both" : "particle";
            auto* out = cJSON_CreateObject();
            cJSON_AddStringToObject(out, "mode", name);
            cJSON_AddNumberToObject(out, "densityOverride", GRain.Params().densityOverride);
            cJSON_AddBoolToObject(out, "snowflakes", GRain.Params().snowflakes);
            cJSON_AddNumberToObject(out, "effectiveDensity", GRain.EffectiveDensity());
            cJSON_AddNumberToObject(out, "weatherRain", GLandscape->GetRainDensity());
            cJSON_AddNumberToObject(out, "liquidRain", UniformWettingDensity(GLandscape->GetRainDensity(),
                GRain.EffectiveDensity(), GRain.Params().snowflakes));
            return HarnessProtocol::JsonResponse(out);
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
    hs.RegisterCommand({"player_weapon_state", "Read-only real player's selected bullet muzzle and direction", {}},
        [](const std::string&, cJSON*) -> std::string {
            if (!GWorld || !GLandscape || GWorld->GetMode() == GModeNetware)
                return HarnessProtocol::ErrorResponse("singleplayer world required");
            Man* man = dyn_cast<Man>(GWorld->GetRealPlayer());
            if (!man || !man->GetShape() || !man->IsLocal() || man->IsDammageDestroyed())
                return HarnessProtocol::ErrorResponse("living local infantry player required");
            const int weapon = man->SelectedWeapon();
            if (weapon < 0 || weapon >= man->NMagazineSlots())
                return HarnessProtocol::ErrorResponse("selected weapon missing");
            const auto* mode = man->GetWeaponMode(weapon);
            if (!mode || !mode->_ammo || mode->_ammo->_simulation != AmmoShotBullet)
                return HarnessProtocol::ErrorResponse("selected bullet weapon required");
            const Vector3 position = man->Position();
            const Vector3 muzzle = man->WorldTransform().FastTransform(man->GetWeaponPoint(weapon));
            const Vector3 direction = man->GetWeaponDirection(weapon);
            for (const float value : {position.X(),position.Y(),position.Z(),muzzle.X(),muzzle.Y(),muzzle.Z(),
                                     direction.X(),direction.Y(),direction.Z()})
                if (!std::isfinite(value)) return HarnessProtocol::ErrorResponse("nonfinite weapon state");
            if (direction.SquareSize() < 0.5f || direction.SquareSize() > 1.5f)
                return HarnessProtocol::ErrorResponse("invalid weapon direction");
            cJSON* out = cJSON_CreateObject();
            cJSON_AddBoolToObject(out,"readonly",true);
            cJSON_AddNumberToObject(out,"timeMs",Glob.time.toInt());
            cJSON_AddNumberToObject(out,"selectedWeapon",weapon);
            cJSON_AddStringToObject(out,"model",man->GetShape()->GetName());
            cJSON_AddNumberToObject(out,"x",position.X()); cJSON_AddNumberToObject(out,"y",position.Y()); cJSON_AddNumberToObject(out,"z",position.Z());
            cJSON_AddNumberToObject(out,"muzzleX",muzzle.X()); cJSON_AddNumberToObject(out,"muzzleY",muzzle.Y()); cJSON_AddNumberToObject(out,"muzzleZ",muzzle.Z());
            cJSON_AddNumberToObject(out,"directionX",direction.X()); cJSON_AddNumberToObject(out,"directionY",direction.Y()); cJSON_AddNumberToObject(out,"directionZ",direction.Z());
            cJSON_AddBoolToObject(out,"manual",GWorld->PlayerManual());
            cJSON_AddBoolToObject(out,"playerSuspended",GWorld->GetPlayerSuspended());
            cJSON_AddBoolToObject(out,"cameraEffect",GWorld->GetCameraEffect()!=nullptr);
            cJSON_AddNumberToObject(out,"cameraType",int(GWorld->GetCameraType()));
            return HarnessProtocol::JsonResponse(out);
        });
    hs.RegisterCommand({"rotor_state", "Read-only nearest helicopter flight/input state; no flight setters",
                        {{"x","float",true},{"z","float",true},{"radius","float",false}}},
        [](const std::string&, cJSON* root) -> std::string {
            if (!GWorld || !GLandscape || GWorld->GetMode() == GModeNetware)
                return HarnessProtocol::ErrorResponse("singleplayer world/landscape required");
            const auto* x = cJSON_GetObjectItemCaseSensitive(root,"x");
            const auto* z = cJSON_GetObjectItemCaseSensitive(root,"z");
            const auto* radiusValue = cJSON_GetObjectItemCaseSensitive(root,"radius");
            const double extent = double(GLandscape->GetLandRange()) * GLandscape->GetLandGrid();
            if (!cJSON_IsNumber(x) || !cJSON_IsNumber(z) || !std::isfinite(x->valuedouble) ||
                !std::isfinite(z->valuedouble) || !std::isfinite(extent) || extent <= 0 ||
                x->valuedouble < 0 || z->valuedouble < 0 || x->valuedouble >= extent || z->valuedouble >= extent ||
                x->valuedouble > 1000000 || z->valuedouble > 1000000)
                return HarnessProtocol::ErrorResponse("finite in-map world x/z required");
            const double radius = radiusValue ? radiusValue->valuedouble : 16.0;
            if ((radiusValue && !cJSON_IsNumber(radiusValue)) || !std::isfinite(radius) || radius < 1 || radius > 50)
                return HarnessProtocol::ErrorResponse("radius must be finite 1..50 metres");
            const int count = GWorld->NVehicles();
            if (count < 0 || count > 4096)
                return HarnessProtocol::ErrorResponse("vehicle census exceeds bounded diagnostic capacity");
            Helicopter* nearest = nullptr;
            double nearestSquared = radius * radius;
            unsigned candidates = 0;
            for (int i = 0; i < count; ++i) {
                auto* heli = dyn_cast<Helicopter>(GWorld->GetVehicle(i));
                if (!heli) continue;
                const Vector3 p = heli->Position();
                if (!std::isfinite(p.X()) || !std::isfinite(p.Y()) || !std::isfinite(p.Z())) continue;
                const double dx = double(p.X())-x->valuedouble, dz = double(p.Z())-z->valuedouble;
                const double distance = dx*dx+dz*dz;
                if (distance > radius*radius) continue;
                ++candidates;
                if (!nearest || distance < nearestSquared ||
                    (distance == nearestSquared && heli->RenderId() < nearest->RenderId())) {
                    nearest = heli; nearestSquared = distance;
                }
            }
            if (!nearest) return HarnessProtocol::ErrorResponse("no finite helicopter within bounded radius");
            if (!nearest->GetType()) return HarnessProtocol::ErrorResponse("helicopter type unavailable");
            const Vector3 p = nearest->Position(), up = nearest->DirectionUp(), speed = nearest->Speed();
            const float ground = GLandscape->SurfaceY(p.X(),p.Z());
            const float clearance = p.Y()-ground;
            const float rpm = nearest->RotorSpeed(), fuel = nearest->GetFuel(), damage = nearest->GetTotalDammage();
            auto& input = InputSubsystem::Instance();
            const float moveUp = input.GetAction(InputContext::HeliPilot,UAMoveUp);
            const float moveDown = input.GetAction(InputContext::HeliPilot,UAMoveDown);
            const float rawMoveUp = input.GetAction(InputContext::HeliPilot,UAMoveUp,false);
            const float forward = input.GetMoveForward(InputContext::HeliPilot);
            const float fastForward = input.GetMoveFastForward(InputContext::HeliPilot);
            const float back = input.GetAction(InputContext::HeliPilot,UAMoveBack);
            const float turnLeft = input.GetAction(InputContext::HeliPilot,UATurnLeft);
            const float turnRight = input.GetAction(InputContext::HeliPilot,UATurnRight);
            const float moveLeft = input.GetAction(InputContext::HeliPilot,UAMoveLeft);
            const float moveRight = input.GetAction(InputContext::HeliPilot,UAMoveRight);
            const Vector3 wind = GLandscape->GetWind();
            HelicopterHoverDiagnostics hover{};
            const bool hoverAvailable = nearest->ReadHoverDiagnostics(hover);
            const float localScale = GAirflow.LocalScale();
            for (const float value : {p.X(),p.Y(),p.Z(),up.X(),up.Y(),up.Z(),speed.X(),speed.Y(),speed.Z(),
                                      ground,clearance,rpm,fuel,damage,moveUp,moveDown,rawMoveUp,localScale,
                                      forward,fastForward,back,turnLeft,turnRight,moveLeft,moveRight,
                                      wind.X(),wind.Y(),wind.Z()})
                if (!std::isfinite(value)) return HarnessProtocol::ErrorResponse("nonfinite helicopter/input state");
            if (hoverAvailable)
                for (const float value : {hover.wantedSpeed.X(),hover.wantedSpeed.Y(),hover.wantedSpeed.Z(),
                                          hover.mouseDirection.X(),hover.mouseDirection.Y(),hover.mouseDirection.Z(),
                                          hover.wantedHeight,hover.wantedHeading,hover.wantedDive})
                    if (!std::isfinite(value)) return HarnessProtocol::ErrorResponse("nonfinite helicopter hover state");
            Person* driver = nearest->Driver();
            AIUnit* driverBrain = driver ? driver->Brain() : nullptr;
            Person* player = GWorld->PlayerOn();
            const auto resolved = GWorld->ResolveInputContextResolution();
            auto* out = cJSON_CreateObject();
            cJSON_AddBoolToObject(out,"readonly",true);
            cJSON_AddNumberToObject(out,"timeMs",Glob.time.toInt());
            cJSON_AddNumberToObject(out,"object",nearest->RenderId());
            cJSON_AddStringToObject(out,"class",nearest->GetType()->GetName());
            cJSON_AddStringToObject(out,"model",nearest->GetShape() ? nearest->GetShape()->Name() : "");
            cJSON_AddNumberToObject(out,"candidates",candidates);
            cJSON_AddNumberToObject(out,"distanceXZ",std::sqrt(nearestSquared));
            cJSON_AddNumberToObject(out,"x",p.X()); cJSON_AddNumberToObject(out,"y",p.Y()); cJSON_AddNumberToObject(out,"z",p.Z());
            cJSON_AddNumberToObject(out,"groundY",ground); cJSON_AddNumberToObject(out,"clearance",clearance);
            cJSON_AddNumberToObject(out,"rpm",rpm); cJSON_AddBoolToObject(out,"engineOn",nearest->EngineIsOn());
            cJSON_AddNumberToObject(out,"upX",up.X()); cJSON_AddNumberToObject(out,"upY",up.Y()); cJSON_AddNumberToObject(out,"upZ",up.Z());
            cJSON_AddNumberToObject(out,"speedX",speed.X()); cJSON_AddNumberToObject(out,"speedY",speed.Y()); cJSON_AddNumberToObject(out,"speedZ",speed.Z());
            cJSON_AddNumberToObject(out,"fuel",fuel); cJSON_AddNumberToObject(out,"damage",damage);
            cJSON_AddBoolToObject(out,"destroyed",nearest->IsDammageDestroyed());
            cJSON_AddBoolToObject(out,"local",nearest->IsLocal()); cJSON_AddBoolToObject(out,"airborne",nearest->Airborne());
            cJSON_AddBoolToObject(out,"driverPresent",driver != nullptr);
            cJSON_AddBoolToObject(out,"driverBrain",driverBrain != nullptr);
            cJSON_AddBoolToObject(out,"driverLocal",driver && driver->IsLocal());
            cJSON_AddBoolToObject(out,"driverAlive",driver && !driver->IsDammageDestroyed());
            cJSON_AddBoolToObject(out,"driverIsPlayer",driver && driver == player);
            cJSON_AddBoolToObject(out,"pilotIsPlayer",player && nearest->PilotUnit() == player->Brain());
            cJSON_AddBoolToObject(out,"focusIsDriver",driverBrain && driverBrain == GWorld->FocusOn());
            cJSON_AddNumberToObject(out,"driverState",driverBrain ? int(driverBrain->GetState()) : -1);
            cJSON_AddBoolToObject(out,"manual",nearest->QIsManual());
            cJSON_AddBoolToObject(out,"driverManual",driver && driver->QIsManual());
            cJSON_AddBoolToObject(out,"playerManual",GWorld->PlayerManual());
            cJSON_AddBoolToObject(out,"playerSuspended",GWorld->GetPlayerSuspended());
            cJSON_AddBoolToObject(out,"cameraEffect",GWorld->GetCameraEffect() != nullptr);
            cJSON_AddStringToObject(out,"inputContext",InputContextName(input.GetContext()));
            cJSON_AddStringToObject(out,"resolvedContext",InputContextName(resolved.context));
            cJSON_AddStringToObject(out,"inputSeat",InputSeatContextName(resolved.seat));
            cJSON_AddBoolToObject(out,"resolvedHelicopter",resolved.transport == nearest);
            cJSON_AddNumberToObject(out,"moveUp",moveUp); cJSON_AddNumberToObject(out,"moveDown",moveDown);
            cJSON_AddNumberToObject(out,"unfocusedMoveUp",rawMoveUp);
            cJSON_AddNumberToObject(out,"focusLost",GInput.gameFocusLost);
            cJSON_AddNumberToObject(out,"qKey",GInput.keyboard.keys[SDL_SCANCODE_Q]);
            cJSON_AddNumberToObject(out,"altKey",GInput.keyboard.keys[SDL_SCANCODE_LALT]);
            cJSON_AddNumberToObject(out,"cameraType",int(GWorld->GetCameraType()));
            cJSON_AddBoolToObject(out,"mouseTurnActive",input.IsMouseTurnActive());
            cJSON_AddBoolToObject(out,"lookAroundEnabled",input.IsLookAroundEnabled());
            cJSON_AddBoolToObject(out,"lookAroundToggled",input.IsLookAroundToggled());
            cJSON_AddBoolToObject(out,"joystickActive",input.IsJoystickActive());
            cJSON_AddBoolToObject(out,"joystickThrustActive",input.IsJoystickThrustActive());
            cJSON_AddNumberToObject(out,"moveForward",forward); cJSON_AddNumberToObject(out,"fastForward",fastForward);
            cJSON_AddNumberToObject(out,"moveBack",back);
            cJSON_AddNumberToObject(out,"turnLeft",turnLeft); cJSON_AddNumberToObject(out,"turnRight",turnRight);
            cJSON_AddNumberToObject(out,"moveLeft",moveLeft); cJSON_AddNumberToObject(out,"moveRight",moveRight);
            cJSON_AddNumberToObject(out,"windX",wind.X()); cJSON_AddNumberToObject(out,"windY",wind.Y()); cJSON_AddNumberToObject(out,"windZ",wind.Z());
            cJSON_AddBoolToObject(out,"hoverStateAvailable",hoverAvailable);
            if (hoverAvailable) {
                cJSON_AddBoolToObject(out,"hoveringAutopilot",hover.hovering);
                cJSON_AddBoolToObject(out,"pilotSpeedHelper",hover.speedHelper);
                cJSON_AddBoolToObject(out,"pilotHeightHelper",hover.heightHelper);
                cJSON_AddBoolToObject(out,"pilotDirectionHelper",hover.directionHelper);
                cJSON_AddNumberToObject(out,"pilotWantedX",hover.wantedSpeed.X());
                cJSON_AddNumberToObject(out,"pilotWantedY",hover.wantedSpeed.Y());
                cJSON_AddNumberToObject(out,"pilotWantedZ",hover.wantedSpeed.Z());
                cJSON_AddNumberToObject(out,"mouseWantedX",hover.mouseDirection.X());
                cJSON_AddNumberToObject(out,"mouseWantedY",hover.mouseDirection.Y());
                cJSON_AddNumberToObject(out,"mouseWantedZ",hover.mouseDirection.Z());
                cJSON_AddNumberToObject(out,"pilotWantedHeight",hover.wantedHeight);
                cJSON_AddNumberToObject(out,"pilotWantedHeading",hover.wantedHeading);
                cJSON_AddNumberToObject(out,"pilotWantedDive",hover.wantedDive);
            }
            cJSON_AddBoolToObject(out,"airflowEnabled",GAirflow.LocalEnabled()); cJSON_AddNumberToObject(out,"airflowScale",localScale);
            const auto* camera = GScene ? GScene->GetCamera() : nullptr;
            const float distance = camera ? (camera->Position()-p).Size() : -1.0f;
            cJSON_AddBoolToObject(out,"cameraDistanceValid",camera && std::isfinite(distance));
            cJSON_AddNumberToObject(out,"cameraDistance",std::isfinite(distance) ? distance : -1.0f);
            return HarnessProtocol::JsonResponse(out);
        });
    hs.RegisterCommand({"dev_rain_water", "Read-only simulation runoff cells and water budget", {}},
        [](const std::string&, cJSON* root) -> std::string {
            const char* action = HarnessProtocol::GetString(root,"action");
            if (!GWorld || !action) return HarnessProtocol::ErrorResponse("rainwater query requires world and action");
            const auto& field = GRainWater();
            auto* out = cJSON_CreateObject();
            const auto addExactDoubles=[&](double pending,const RainWaterField::Budget& budget){
                cJSON_AddStringToObject(out,"pendingSecondsBits",RainWaterCapture::DoubleBits(pending).data());
                cJSON_AddStringToObject(out,"rainVolumeBits",RainWaterCapture::DoubleBits(budget.rain).data());
                cJSON_AddStringToObject(out,"infiltrationVolumeBits",RainWaterCapture::DoubleBits(budget.infiltration).data());
                cJSON_AddStringToObject(out,"evaporationVolumeBits",RainWaterCapture::DoubleBits(budget.evaporation).data());
                cJSON_AddStringToObject(out,"outletVolumeBits",RainWaterCapture::DoubleBits(budget.outlet).data());
            };
            if (std::strcmp(action,"coarse_capture") == 0) {
                // Deliberate one-time paused export, never in the measured
                // solver/renderer path. The caller cannot supply an output path.
                static bool captured=false;
                const char* flag=std::getenv("POSEIDON_RAIN_WATER_CAPTURE");
                const char* directory=std::getenv("POSEIDON_RAIN_WATER_CAPTURE_DIR");
                const auto refuse=[&](const char* error){cJSON_Delete(out);return HarnessProtocol::ErrorResponse(error);};
                if(!flag||std::strcmp(flag,"1")!=0||!directory||captured)
                    return refuse("coarse capture requires exact opt-in, designated directory and unused one-time gate");
                if(!GLandscape||GWorld->GetAcceleratedTime()!=0)
                    return refuse("coarse capture requires an actual paused world");
                const auto view=field.CoarseCapture();
                const auto grid=RainWaterField::AlignedSourceGrid(GLandscape->GetTerrainRange(),GLandscape->GetTerrainGrid());
                if(!view||!field.SourceReady()||grid.side<2||!field.MatchesDomain(grid.side,grid.side,grid.spacing)||
                   view->seaLevel!=GLandscape->GetSeaLevel())
                    return refuse("coarse capture refuses invalid, fine, stale or mismatched source domain/sea");
                // Compare the actual raw source bits, not interpolated SurfaceY
                // or rounded renderer heads. This witnesses current native bed.
                for(int z=0;z<grid.side;++z)for(int x=0;x<grid.side;++x)
                    if(std::bit_cast<uint32_t>(view->bed[size_t(z)*grid.side+x])!=
                       std::bit_cast<uint32_t>(GLandscape->ClippedData(z*grid.stride,x*grid.stride)))
                        return refuse("coarse capture refuses stale native bed bits");
                std::error_code ec;
                const std::filesystem::path requestedDirectory(directory);
                if(!requestedDirectory.is_absolute())return refuse("coarse capture requires an absolute campaign directory");
                const auto canonical=std::filesystem::weakly_canonical(requestedDirectory,ec);
                if(ec||!std::filesystem::is_directory(canonical,ec)||ec)
                    return refuse("coarse capture directory is unavailable");
                const auto path=RainWaterCapture::CampaignOutputPath(canonical);
                if(!path||std::filesystem::exists(*path,ec)||ec)
                    return refuse("coarse capture must create its designated campaign file exactly once");
                std::ofstream stream(*path,std::ios::binary);
                const bool written=stream&&RainWaterCapture::Write(stream,*view);
                stream.close();
                if(!written||!stream) {
                    std::filesystem::remove(*path,ec);
                    return refuse("coarse capture file write failed");
                }
                captured=true;
                cJSON_AddBoolToObject(out,"readonly",true);cJSON_AddBoolToObject(out,"sourceCurrent",true);
                cJSON_AddStringToObject(out,"sourceWitness","bit-exact-current-native-coarse-bed");
                cJSON_AddStringToObject(out,"format",RainWaterCapture::Magic);
                cJSON_AddStringToObject(out,"floatEncoding","ieee754-binary32-le");
                cJSON_AddStringToObject(out,"path",path->string().c_str());
                cJSON_AddStringToObject(out,"world",GLandscape->GetName());
                cJSON_AddStringToObject(out,"worldToken",std::to_string(reinterpret_cast<uintptr_t>(GLandscape)).c_str());
                cJSON_AddStringToObject(out,"sourceRevision",std::to_string(GLandscape->HeightRevision()).c_str());
                cJSON_AddStringToObject(out,"generationExact",std::to_string(view->generation).c_str());
                cJSON_AddStringToObject(out,"revisionExact",std::to_string(view->revision).c_str());
                cJSON_AddNumberToObject(out,"timeMs",Glob.time.toInt());
                cJSON_AddNumberToObject(out,"timeScale",GWorld->GetAcceleratedTime());
                cJSON_AddNumberToObject(out,"width",view->width);cJSON_AddNumberToObject(out,"height",view->height);
                cJSON_AddNumberToObject(out,"spacing",view->spacing);cJSON_AddNumberToObject(out,"originX",view->originX);
                cJSON_AddNumberToObject(out,"originZ",view->originZ);cJSON_AddNumberToObject(out,"seaLevel",view->seaLevel);
                cJSON_AddNumberToObject(out,"pendingSeconds",view->pending);
                addExactDoubles(view->pending,view->budget);
                cJSON_AddNumberToObject(out,"terrainRange",GLandscape->GetTerrainRange());
                cJSON_AddNumberToObject(out,"terrainSpacing",GLandscape->GetTerrainGrid());
                cJSON_AddNumberToObject(out,"sourceStride",grid.stride);
                cJSON_AddNumberToObject(out,"nativeBedBitsMatched",view->bed.size());
                cJSON_AddNumberToObject(out,"headerBytes",RainWaterCapture::HeaderBytes);
                cJSON_AddNumberToObject(out,"bytes",RainWaterCapture::HeaderBytes+view->bed.size()*sizeof(float)*2);
                cJSON_AddNumberToObject(out,"maximumRainMetresPerSecond",RainWaterMaximumRainMetresPerSecond);
                cJSON_AddNumberToObject(out,"stepSeconds",RainWaterField::StepSeconds);
                cJSON_AddNumberToObject(out,"weatherRain",GLandscape->GetRainDensity());
                cJSON_AddNumberToObject(out,"effectiveParticleRain",GRain.EffectiveDensity());
                cJSON_AddBoolToObject(out,"particleSnowflakes",GRain.Params().snowflakes);
                cJSON_AddNumberToObject(out,"liquidRain",UniformWettingDensity(GLandscape->GetRainDensity(),
                    GRain.EffectiveDensity(),GRain.Params().snowflakes));
                const auto* sun=GScene ? GScene->MainLight() : nullptr;
                cJSON_AddNumberToObject(out,"solarDrying",sun ? SunlightDryingExposure(
                    sun->SunDirection().Y(),GLandscape->GetOvercast(),sun->NightEffect()) : 0);
                cJSON_AddNumberToObject(out,"windInput",GWind.IsActive() ? GWind.Sample().speed/20.0f : 0);
            } else if (std::strcmp(action,"sample") == 0) {
                const auto* x = cJSON_GetObjectItemCaseSensitive(root,"x");
                const auto* z = cJSON_GetObjectItemCaseSensitive(root,"z");
                if (!cJSON_IsNumber(x) || !cJSON_IsNumber(z) ||
                    !std::isfinite(x->valuedouble) || !std::isfinite(z->valuedouble)) {
                    cJSON_Delete(out); return HarnessProtocol::ErrorResponse("sample needs finite world x/z");
                }
                const auto sample = field.At(float(x->valuedouble),float(z->valuedouble));
                cJSON_AddBoolToObject(out,"valid",sample.valid);
                cJSON_AddNumberToObject(out,"depth",sample.depth);
                cJSON_AddNumberToObject(out,"height",sample.height);
                cJSON_AddNumberToObject(out,"flowX",sample.flowX);
                cJSON_AddNumberToObject(out,"flowZ",sample.flowZ);
                if (GLandscape) cJSON_AddNumberToObject(out,"terrainY",GLandscape->SurfaceY(float(x->valuedouble),float(z->valuedouble)));
            } else if (std::strcmp(action,"state") == 0) {
                const auto& budget = field.WaterBudget();
                cJSON_AddNumberToObject(out,"width",field.Width());
                cJSON_AddNumberToObject(out,"height",field.Height());
                cJSON_AddNumberToObject(out,"spacing",field.Spacing());
                cJSON_AddNumberToObject(out,"volume",field.Volume());
                cJSON_AddNumberToObject(out,"rainVolume",budget.rain);
                cJSON_AddNumberToObject(out,"infiltrationVolume",budget.infiltration);
                cJSON_AddNumberToObject(out,"evaporationVolume",budget.evaporation);
                cJSON_AddNumberToObject(out,"outletVolume",budget.outlet);
                cJSON_AddNumberToObject(out,"pendingSeconds",field.PendingSeconds());
                const char* captureFlag=std::getenv("POSEIDON_RAIN_WATER_CAPTURE");
                if(captureFlag&&std::strcmp(captureFlag,"1")==0)
                    addExactDoubles(field.PendingSeconds(),budget);
                cJSON_AddNumberToObject(out,"generation",static_cast<double>(field.Generation()));
                cJSON_AddBoolToObject(out,"fineActive",field.FineActive());
                cJSON_AddBoolToObject(out,"sourceReady",field.SourceReady());
                cJSON_AddNumberToObject(out,"fineTiles",double(field.FineTileCount()));
            } else if (std::strcmp(action,"fine_state") == 0) {
                if (!GLandscape) { cJSON_Delete(out); return HarnessProtocol::ErrorResponse("fine query requires landscape"); }
                const auto* x=cJSON_GetObjectItemCaseSensitive(root,"x");
                const auto* z=cJSON_GetObjectItemCaseSensitive(root,"z");
                const bool selected=x||z;
                if (selected && (!cJSON_IsNumber(x)||!cJSON_IsNumber(z)||
                    !std::isfinite(x->valuedouble)||!std::isfinite(z->valuedouble))) {
                    cJSON_Delete(out); return HarnessProtocol::ErrorResponse("fine selection needs finite world x/z");
                }
                // Bounded <=8*1024 cells; this copy/query never advances water.
                const auto snapshot=field.FineSnapshot();
                cJSON_AddBoolToObject(out,"readonly",true);
                cJSON_AddBoolToObject(out,"fineActive",field.FineActive());
                cJSON_AddBoolToObject(out,"sourceReady",field.SourceReady());
                cJSON_AddBoolToObject(out,"valid",snapshot.valid);
                cJSON_AddNumberToObject(out,"tiles",double(field.FineTileCount()));
                cJSON_AddNumberToObject(out,"cells",double(snapshot.surfaces.size()));
                cJSON_AddNumberToObject(out,"generation",double(snapshot.generation));
                cJSON_AddNumberToObject(out,"sourceRevision",double(snapshot.sourceRevision));
                cJSON_AddNumberToObject(out,"heightRevision",double(GLandscape->HeightRevision()));
                cJSON_AddNumberToObject(out,"terrainRange",GLandscape->GetTerrainRange());
                cJSON_AddNumberToObject(out,"terrainSpacing",GLandscape->GetTerrainGrid());
                cJSON_AddNumberToObject(out,"seaLevel",GLandscape->GetSeaLevel());
                // Decimal string preserves the opaque owner token beyond JSON's 53-bit integers.
                cJSON_AddStringToObject(out,"worldToken",std::to_string(reinterpret_cast<uintptr_t>(GLandscape)).c_str());
                double volume=0,integrated=0;
                for (const auto& surface:snapshot.surfaces) {
                    const auto& cell=surface.owner;
                    volume+=cell.volume;
                    integrated+=surface.geometry.Volume(cell.head,cell.size*cell.size);
                    if (selected && x->valuedouble>=cell.x && x->valuedouble<cell.x+cell.size &&
                        z->valuedouble>=cell.z && z->valuedouble<cell.z+cell.size) {
                        auto* detail=cJSON_CreateObject();
                        cJSON_AddNumberToObject(detail,"x",cell.x); cJSON_AddNumberToObject(detail,"z",cell.z);
                        cJSON_AddNumberToObject(detail,"size",cell.size); cJSON_AddNumberToObject(detail,"head",cell.head);
                        cJSON_AddNumberToObject(detail,"volume",cell.volume); cJSON_AddNumberToObject(detail,"parent",surface.parent);
                        const double support=surface.geometry.Height((x->valuedouble-cell.x)/cell.size,(z->valuedouble-cell.z)/cell.size);
                        cJSON_AddNumberToObject(detail,"supportY",support);
                        cJSON_AddNumberToObject(detail,"localDepth",std::max(0.0,cell.head-support));
                        auto* corners=cJSON_CreateArray();
                        for (const auto h:{surface.geometry.Corner00(),surface.geometry.Corner10(),surface.geometry.Corner01(),surface.geometry.Corner11()})
                            cJSON_AddItemToArray(corners,cJSON_CreateNumber(h));
                        cJSON_AddItemToObject(detail,"corners",corners); cJSON_AddItemToObject(out,"selected",detail);
                    }
                }
                cJSON_AddNumberToObject(out,"fineVolume",volume);
                cJSON_AddNumberToObject(out,"integratedVolume",integrated);
                // Inspect only the retained bounded snapshot. No promotion,
                // sampling, clock advance or terrain edit occurs in this query.
                struct TileFacts {
                    size_t count=0;double volume=0,integrated=0;
                    double minBed=INFINITY,maxBed=-INFINITY,minHead=INFINITY,maxHead=-INFINITY;
                    double minDepth=INFINITY,maxDepth=0;
                    bool finite=true;std::array<bool,1024> covered{};
                };
                std::map<std::pair<int,int>,TileFacts> owners;
                const bool sourceCurrent=snapshot.valid&&field.SourceReady()&&
                    snapshot.sourceRevision==GLandscape->HeightRevision()&&
                    GLandscape->GetTerrainGrid()==6.25f&&field.Spacing()==25&&
                    field.Width()>0&&snapshot.generation==field.Generation()&&snapshot.revision==field.Revision();
                for(const auto& surface:snapshot.surfaces) {
                    const auto& cell=surface.owner;
                    if(field.Width()<=0)break;
                    const int tx=int(surface.parent%field.Width())/8,tz=int(surface.parent/field.Width())/8;
                    const auto key=std::pair{tx,tz};
                    if(!owners.contains(key)&&owners.size()>=RainWaterField::FineTileLimit)continue;
                    auto& tile=owners[key];++tile.count;
                    const double ox=200.0*tx-12.5,oz=200.0*tz-12.5;
                    const double ix=(cell.x-ox)/6.25,iz=(cell.z-oz)/6.25;
                    const bool finite=surface.geometry.Finite()&&std::isfinite(cell.x)&&std::isfinite(cell.z)&&
                        std::isfinite(cell.head)&&std::isfinite(cell.volume)&&cell.volume>=0&&cell.size==6.25&&
                        ix>=0&&ix<32&&iz>=0&&iz<32&&std::floor(ix)==ix&&std::floor(iz)==iz;
                    if(!finite){tile.finite=false;continue;}
                    const size_t index=size_t(iz)*32+size_t(ix);
                    if(tile.covered[index])tile.finite=false;
                    tile.covered[index]=true;
                    const double heldIntegral=surface.geometry.Volume(cell.head,cell.size*cell.size);
                    if(!std::isfinite(heldIntegral)||heldIntegral<0){tile.finite=false;continue;}
                    tile.volume+=cell.volume;tile.integrated+=heldIntegral;
                    tile.minBed=std::min(tile.minBed,surface.geometry.Minimum());
                    tile.maxBed=std::max(tile.maxBed,surface.geometry.Maximum());
                    tile.minHead=std::min(tile.minHead,cell.head);tile.maxHead=std::max(tile.maxHead,cell.head);
                    tile.minDepth=std::min(tile.minDepth,std::max(0.0,cell.head-surface.geometry.Maximum()));
                    tile.maxDepth=std::max(tile.maxDepth,std::max(0.0,cell.head-surface.geometry.Minimum()));
                }
                auto* rows=cJSON_AddArrayToObject(out,"owners");
                for(const auto& [key,tile]:owners) {
                    auto* row=cJSON_CreateObject();cJSON_AddItemToArray(rows,row);
                    const bool complete=tile.finite&&tile.count==1024&&
                        std::all_of(tile.covered.begin(),tile.covered.end(),[](bool present){return present;});
                    const double x=200.0*key.first-12.5,z=200.0*key.second-12.5;
                    const bool bounds=x>=0&&z>=0&&x+200<=(GLandscape->GetTerrainRange()-1)*6.25&&
                        z+200<=(GLandscape->GetTerrainRange()-1)*6.25;
                    cJSON_AddNumberToObject(row,"tileX",key.first);cJSON_AddNumberToObject(row,"tileZ",key.second);
                    cJSON_AddNumberToObject(row,"x",x);cJSON_AddNumberToObject(row,"z",z);cJSON_AddNumberToObject(row,"size",200);
                    cJSON_AddNumberToObject(row,"cells",double(tile.count));cJSON_AddBoolToObject(row,"finite",tile.finite);
                    cJSON_AddBoolToObject(row,"complete",complete);cJSON_AddBoolToObject(row,"sourceCurrent",sourceCurrent&&complete&&bounds);
                    if(complete) {
                        cJSON_AddNumberToObject(row,"volume",tile.volume);cJSON_AddNumberToObject(row,"integratedVolume",tile.integrated);
                        cJSON_AddNumberToObject(row,"bedMin",tile.minBed);cJSON_AddNumberToObject(row,"bedMax",tile.maxBed);
                        cJSON_AddNumberToObject(row,"headMin",tile.minHead);cJSON_AddNumberToObject(row,"headMax",tile.maxHead);
                        cJSON_AddNumberToObject(row,"minDepth",tile.minDepth);cJSON_AddNumberToObject(row,"maxDepth",tile.maxDepth);
                    }
                }
            } else {
                cJSON_Delete(out); return HarnessProtocol::ErrorResponse("read-only state, fine_state or sample only");
            }
            cJSON_AddNumberToObject(out,"revision",double(field.Revision()));
            return HarnessProtocol::JsonResponse(out);
        });
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
    hs.RegisterCommand({"dev_mud", "Read-only mud history and physical terrain height; no synthetic stamps", {}},
        [](const std::string&, cJSON* root) -> std::string {
            if (!GWorld || !GLandscape) return HarnessProtocol::ErrorResponse("mud probe requires world");
            const char* action = HarnessProtocol::GetString(root, "action");
            if (!action || (std::strcmp(action, "state") != 0 && std::strcmp(action, "sample") != 0))
                return HarnessProtocol::ErrorResponse("mud probe action must be state or sample");
            const auto& mud = GMud();
            float x = 0, z = 0;
            if (std::strcmp(action, "sample") == 0)
            {
                const auto* east = cJSON_GetObjectItemCaseSensitive(root, "x");
                const auto* north = cJSON_GetObjectItemCaseSensitive(root, "z");
                if (!cJSON_IsNumber(east) || !cJSON_IsNumber(north) ||
                    !std::isfinite(east->valuedouble) || !std::isfinite(north->valuedouble) ||
                    std::abs(east->valuedouble) > 1000000 || std::abs(north->valuedouble) > 1000000)
                    return HarnessProtocol::ErrorResponse("mud sample needs finite bounded x/z");
                x = float(east->valuedouble); z = float(north->valuedouble);
            }
            auto* out = cJSON_CreateObject();
            cJSON_AddBoolToObject(out, "enabled", mud.enabled);
            cJSON_AddNumberToObject(out, "wetness", mud.Wetness());
            cJSON_AddNumberToObject(out, "chunks", double(mud.Chunks()));
            cJSON_AddNumberToObject(out, "revision", double(mud.Revision()));
            cJSON_AddNumberToObject(out, "rejected", double(mud.Rejected()));
            if (std::strcmp(action, "sample") == 0)
            {
                float dx = 0, dz = 0;
                const float height = GLandscape->SurfaceY(x, z, &dx, &dz);
                const auto gradient = mud.HeightGradientAt(x, z);
                cJSON_AddNumberToObject(out, "x", x); cJSON_AddNumberToObject(out, "z", z);
                cJSON_AddNumberToObject(out, "offset", mud.HeightOffsetAt(x, z));
                cJSON_AddNumberToObject(out, "mudDx", gradient[0]); cJSON_AddNumberToObject(out, "mudDz", gradient[1]);
                cJSON_AddNumberToObject(out, "surfaceY", height);
                cJSON_AddNumberToObject(out, "surfaceDx", dx); cJSON_AddNumberToObject(out, "surfaceDz", dz);
                const auto& surface = GLandscape->SurfaceAt(x, z);
                const float grid = GLandscape->GetLandGrid();
                const bool inMap = std::isfinite(grid) && grid > 0 && x >= 0 && z >= 0 &&
                    x < GLandscape->GetLandRange()*grid && z < GLandscape->GetLandRange()*grid;
                const int cx = inMap ? int(std::floor(x/grid)) : 0, cz = inMap ? int(std::floor(z/grid)) : 0;
                const int id = inMap ? GLandscape->GetTexture(cz, cx) : -1;
                const Texture* texture = id >= 0 ? GLandscape->GetTexture(id) : nullptr;
                const char* textureName = texture ? texture->GetName().Data() : "";
                cJSON_AddStringToObject(out, "world", GLandscape->GetName());
                cJSON_AddStringToObject(out, "texture", textureName);
                cJSON_AddStringToObject(out, "surfaceClass", surface._class.Data());
                cJSON_AddStringToObject(out, "files", surface._name.Data());
                cJSON_AddStringToObject(out, "sound", surface._soundEnv.Data());
                cJSON_AddStringToObject(out, "character", surface._character.Data());
                const bool source = inMap && GLandscape->GetEnfusionSurfaceCount() == 0 &&
                    GLandscape->GetA3TerrainMaterial(id) == nullptr &&
                    (StockMudSoil(surface._class.Data(), surface._name.Data(), surface._soundEnv.Data(), surface._character.Data()) ||
                     StockNogovaMudSoil(GLandscape->GetName(), textureName, surface._class.Data(), surface._name.Data(),
                        surface._soundEnv.Data(), surface._character.Data(), x/grid-cx, z/grid-cz));
                cJSON_AddBoolToObject(out, "sourceEligible", source);
            }
            return HarnessProtocol::JsonResponse(out);
        });
    hs.RegisterCommand({"dev_sand", "Read-only sand history and physical terrain height; no synthetic stamps; signed loose rims", {}},
        [](const std::string&, cJSON* root) -> std::string {
            if (!GWorld || !GLandscape) return HarnessProtocol::ErrorResponse("sand probe requires world");
            const char* action = HarnessProtocol::GetString(root, "action");
            if (!action || (std::strcmp(action, "state") != 0 && std::strcmp(action, "sample") != 0))
                return HarnessProtocol::ErrorResponse("sand probe action must be state or sample");
            const auto& sand = GSand();
            float x = 0, z = 0;
            if (std::strcmp(action, "sample") == 0)
            {
                const auto* east = cJSON_GetObjectItemCaseSensitive(root, "x");
                const auto* north = cJSON_GetObjectItemCaseSensitive(root, "z");
                if (!cJSON_IsNumber(east) || !cJSON_IsNumber(north) ||
                    !std::isfinite(east->valuedouble) || !std::isfinite(north->valuedouble) ||
                    std::abs(east->valuedouble) > 1000000 || std::abs(north->valuedouble) > 1000000)
                    return HarnessProtocol::ErrorResponse("sand sample needs finite bounded x/z");
                x = float(east->valuedouble); z = float(north->valuedouble);
            }
            auto* out = cJSON_CreateObject();
            cJSON_AddBoolToObject(out, "enabled", sand.enabled);
            cJSON_AddNumberToObject(out, "wetness", sand.Wetness());
            cJSON_AddNumberToObject(out, "chunks", double(sand.Chunks()));
            cJSON_AddNumberToObject(out, "revision", double(sand.Revision()));
            cJSON_AddNumberToObject(out, "rejected", double(sand.Rejected()));
            if (std::strcmp(action, "sample") == 0)
            {
                float dx = 0, dz = 0;
                const float height = GLandscape->SurfaceY(x, z, &dx, &dz);
                const auto gradient = sand.HeightGradientAt(x, z);
                cJSON_AddNumberToObject(out, "x", x); cJSON_AddNumberToObject(out, "z", z);
                cJSON_AddNumberToObject(out, "offset", sand.HeightOffsetAt(x, z));
                cJSON_AddNumberToObject(out, "sandDx", gradient[0]); cJSON_AddNumberToObject(out, "sandDz", gradient[1]);
                cJSON_AddNumberToObject(out, "surfaceY", height);
                cJSON_AddNumberToObject(out, "surfaceDx", dx); cJSON_AddNumberToObject(out, "surfaceDz", dz);
                const auto& surface = GLandscape->SurfaceAt(x, z);
                const float grid = GLandscape->GetLandGrid();
                const bool inMap = std::isfinite(grid) && grid > 0 && x >= 0 && z >= 0 &&
                    x < GLandscape->GetLandRange()*grid && z < GLandscape->GetLandRange()*grid;
                const int cx = inMap ? int(std::floor(x/grid)) : 0, cz = inMap ? int(std::floor(z/grid)) : 0;
                const int id = inMap ? GLandscape->GetTexture(cz, cx) : -1;
                const Texture* texture = id >= 0 ? GLandscape->GetTexture(id) : nullptr;
                const char* textureName = texture ? texture->GetName().Data() : "";
                cJSON_AddStringToObject(out, "world", GLandscape->GetName());
                cJSON_AddStringToObject(out, "texture", textureName);
                cJSON_AddStringToObject(out, "surfaceClass", surface._class.Data());
                cJSON_AddStringToObject(out, "files", surface._name.Data());
                cJSON_AddStringToObject(out, "sound", surface._soundEnv.Data());
                cJSON_AddStringToObject(out, "character", surface._character.Data());
                bool source = inMap && GLandscape->GetEnfusionSurfaceCount() == 0 &&
                    GLandscape->GetA3TerrainMaterial(id) == nullptr && SandSourceInterior(x/grid-cx, z/grid-cz) &&
                    StockSandSurface(surface._class.Data(), surface._name.Data(), surface._soundEnv.Data(), surface._character.Data());
                for (const float v : {0.25f, 0.75f})
                    for (const float u : {0.25f, 0.75f})
                    {
                        if (!source) continue;
                        const auto& corner = GLandscape->SurfaceAt((cx+u)*grid, (cz+v)*grid);
                        source = StockSandSurface(corner._class.Data(), corner._name.Data(), corner._soundEnv.Data(), corner._character.Data());
                    }
                cJSON_AddBoolToObject(out, "sourceEligible", source);
            }
            return HarnessProtocol::JsonResponse(out);
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
    hs.RegisterCommand({"corpse_pose_state", "Read-only complete stock corpse pose admission measurements", {}},
        [](const std::string&, cJSON*) -> std::string {
            if (!AppConfig::Instance().DevMode() || !GWorld || GWorld->GetMode() != GModeArcade)
                return HarnessProtocol::ErrorResponse("corpse pose diagnostic requires --dev and local single player");
            CorpsePoseDiagnostic measured;
            RString refusal;
            if (!Man::GetCorpsePoseDiagnostic(measured, refusal))
                return HarnessProtocol::ErrorResponse(static_cast<const char*>(refusal));
            auto vector = [](Vector3Val value) {
                const double xyz[] = {value.X(), value.Y(), value.Z()};
                return cJSON_CreateDoubleArray(xyz, 3);
            };
            auto matrix = [](Matrix4Val value) {
                double entries[12];
                for (int row = 0; row < 3; ++row)
                    for (int col = 0; col < 4; ++col) entries[row * 4 + col] = value(row, col);
                return cJSON_CreateDoubleArray(entries, 12);
            };
            cJSON* out = cJSON_CreateObject();
            cJSON_AddBoolToObject(out, "readonly", true);
            cJSON_AddBoolToObject(out, "solver", measured.articulated);
            cJSON_AddBoolToObject(out, "frozen", measured.frozen);
            cJSON_AddItemToObject(out, "objectTransform", matrix(measured.object));
            cJSON_AddBoolToObject(out, "retainedBounds", measured.retainedBounds);
            cJSON_AddItemToObject(out, "retainedMinimum", vector(measured.retainedMinimum));
            cJSON_AddItemToObject(out, "retainedMaximum", vector(measured.retainedMaximum));
            cJSON_AddNumberToObject(out, "retainedRadius", measured.retainedRadius);
            cJSON_AddBoolToObject(out, "rigAdmitted", measured.articulated);
            cJSON_AddBoolToObject(out, "contactAdmitted", false);
            cJSON_AddStringToObject(out, "contactScope", "road-support clearance and corpse-ignored geometry center ray; not convex overlap");
            cJSON_AddStringToObject(out, "anchorScope", "shared source selection centroid; paired final-bone endpoints, no calibrated joint frame");
            cJSON_AddStringToObject(out, "matrixLayout", "row-major 3x4 model-space affine; actual post-head/aim/graphical-leg palette with dead face tail");
            cJSON_AddStringToObject(out, "entity", measured.entity.c_str());
            cJSON_AddStringToObject(out, "model", measured.model.c_str());
            cJSON_AddNumberToObject(out, "capturedMs", measured.capturedMs);
            cJSON_AddNumberToObject(out, "measuredMs", measured.measuredMs);
            cJSON_AddBoolToObject(out, "headIdentityFlag", measured.headIdentityFlag);
            cJSON_AddBoolToObject(out, "gunIdentityFlag", measured.gunIdentityFlag);
            cJSON_AddNumberToObject(out, "headIdentityError", measured.headIdentityError);
            cJSON_AddNumberToObject(out, "gunIdentityError", measured.gunIdentityError);
            cJSON_AddNumberToObject(out, "legIdentityError", measured.legIdentityError);
            cJSON_AddItemToObject(out, "headCorrection", matrix(measured.headCorrection));
            cJSON_AddItemToObject(out, "gunCorrection", matrix(measured.gunCorrection));
            cJSON_AddItemToObject(out, "legCorrection", matrix(measured.legCorrection));
            auto* bones = cJSON_AddArrayToObject(out, "bones");
            for (const auto& bone : measured.bones) cJSON_AddItemToArray(bones, cJSON_CreateString(bone.c_str()));
            auto* levels = cJSON_AddArrayToObject(out, "levels");
            for (const auto& level : measured.levels)
            {
                auto* item = cJSON_CreateObject(); cJSON_AddItemToArray(levels, item);
                cJSON_AddStringToObject(item, "role", level.role.c_str());
                cJSON_AddNumberToObject(item, "level", level.level);
                cJSON_AddNumberToObject(item, "points", level.points);
                cJSON_AddNumberToObject(item, "compared", level.compared);
                cJSON_AddNumberToObject(item, "skippedPointTails", level.skippedPointTails);
                cJSON_AddNumberToObject(item, "unweighted", level.unweighted);
                cJSON_AddBoolToObject(item, "faceEvaluated", level.faceEvaluated);
                cJSON_AddBoolToObject(item, "pointOnly", level.pointOnly);
                cJSON_AddNumberToObject(item, "minAbsDeterminant", level.minAbsDeterminant);
                cJSON_AddNumberToObject(item, "maxPointPaletteError", level.maxPointPaletteError);
                cJSON_AddItemToObject(item, "paletteMin", vector(level.paletteMin));
                cJSON_AddItemToObject(item, "paletteMax", vector(level.paletteMax));
                auto* palette = cJSON_AddArrayToObject(item, "palette");
                for (const auto& bone : level.palette) cJSON_AddItemToArray(palette, matrix(bone));
                auto* points = cJSON_AddArrayToObject(item, "palettePoints");
                for (const auto& point : level.palettePoints) cJSON_AddItemToArray(points, vector(point));
                auto* consumers = cJSON_AddArrayToObject(item, "consumerPoints");
                for (const auto& point : level.consumerPoints) cJSON_AddItemToArray(consumers, vector(point));
                cJSON_AddItemToObject(item,"consumerPointIndices",cJSON_CreateIntArray(level.consumerPointIndices.data(),int(level.consumerPointIndices.size())));
            }
            auto* proxies = cJSON_AddArrayToObject(out, "proxies");
            for (const auto& proxy : measured.proxies)
            {
                auto* item = cJSON_CreateObject(); cJSON_AddItemToArray(proxies, item);
                cJSON_AddNumberToObject(item, "level", proxy.level);
                cJSON_AddStringToObject(item, "selection", proxy.selection.c_str());
                cJSON_AddItemToObject(item, "matrix", matrix(proxy.matrix));
            }
            auto* hulls = cJSON_AddArrayToObject(out, "hulls");
            for (const auto& hull : measured.hulls)
            {
                auto* item = cJSON_CreateObject(); cJSON_AddItemToArray(hulls, item);
                cJSON_AddStringToObject(item, "name", hull.name.c_str());
                cJSON_AddStringToObject(item, "bone", hull.bone.c_str());
                cJSON_AddNumberToObject(item, "points", hull.points);
                cJSON_AddBoolToObject(item, "exclusiveFullWeight", hull.exclusiveFullWeight);
                cJSON_AddItemToObject(item, "worldMin", vector(hull.worldMin));
                cJSON_AddItemToObject(item, "worldMax", vector(hull.worldMax));
                cJSON_AddNumberToObject(item, "minRoadSupportClearance", hull.minRoadSupportClearance);
                cJSON_AddNumberToObject(item, "externalCenterRayHits", hull.externalCenterRayHits);
            }
            auto* anchors = cJSON_AddArrayToObject(out, "anchors");
            for (const auto& anchor : measured.anchors)
            {
                auto* item = cJSON_CreateObject(); cJSON_AddItemToArray(anchors, item);
                cJSON_AddStringToObject(item, "first", anchor.first.c_str());
                cJSON_AddStringToObject(item, "second", anchor.second.c_str());
                cJSON_AddNumberToObject(item, "sharedVertices", anchor.sharedVertices);
                cJSON_AddItemToObject(item, "modelAnchor", vector(anchor.modelAnchor));
                cJSON_AddItemToObject(item, "firstWorld", vector(anchor.firstWorld));
                cJSON_AddItemToObject(item, "secondWorld", vector(anchor.secondWorld));
                cJSON_AddNumberToObject(item, "separation", anchor.separation);
            }
            return HarnessProtocol::JsonResponse(out);
        });
    hs.RegisterCommand(
        {"dev_cave_editor", "Create/undo bounded mission-local horizontal caves", {}},
        [](const std::string&, cJSON* root) -> std::string
        {
            if (!AppConfig::Instance().DevMode() || !GWorld || !GLandscape || GWorld->GetMode() == GModeNetware)
                return HarnessProtocol::ErrorResponse("requires --dev and a single-player world");
            const char* action = HarnessProtocol::GetString(root, "action");
            if (!action)
                return HarnessProtocol::ErrorResponse("needs state, create or undo");
            auto recordId = SelectedEditorCaveId();
            if (const auto* id = cJSON_GetObjectItemCaseSensitive(root, "id"))
            {
                if (!cJSON_IsNumber(id) || !std::isfinite(id->valuedouble) || id->valuedouble < 1 ||
                    id->valuedouble > double(std::numeric_limits<std::uint32_t>::max()) ||
                    std::floor(id->valuedouble) != id->valuedouble)
                    return HarnessProtocol::ErrorResponse("id must be a positive integral editor ID");
                recordId = static_cast<std::uint32_t>(id->valuedouble);
            }
            auto number = [&](const char* key, float& value)
            {
                const auto* v = cJSON_GetObjectItemCaseSensitive(root, key);
                if (!cJSON_IsNumber(v) || !std::isfinite(v->valuedouble))
                    return false;
                value = float(v->valuedouble);
                return std::isfinite(value);
            };
            float x = 0, y = 0, z = 0;
            const bool point =
                cJSON_HasObjectItem(root, "x") || cJSON_HasObjectItem(root, "y") || cJSON_HasObjectItem(root, "z");
            if (point && (!number("x", x) || !number("y", y) || !number("z", z)))
                return HarnessProtocol::ErrorResponse("point requires finite x/y/z");
            const float extent = (GLandscape->GetTerrainRange() - 1) * GLandscape->GetTerrainGrid();
            if (point && (x < 0 || z < 0 || x >= extent || z >= extent))
                return HarnessProtocol::ErrorResponse("point outside terrain");
            if (std::strcmp(action, "create") == 0)
            {
                float heading, width, height, length;
                if (!point || !number("heading", heading) || !number("width", width) || !number("height", height) ||
                    !number("length", length))
                    return HarnessProtocol::ErrorResponse("create needs x/y/z, heading, width, height, length");
                if (!CreateEditorCave(x, y, z, heading, width, height, length))
                    return HarnessProtocol::ErrorResponse(EditorCaveStatus());
            }
            else if (std::strcmp(action, "trench") == 0)
            {
                float heading, width, depth, length;
                if (!point || !number("heading", heading) || !number("width", width) || !number("depth", depth) ||
                    !number("length", length))
                    return HarnessProtocol::ErrorResponse("trench needs x/y/z, heading, width, depth, length");
                const bool tank = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "tank"));
                if (!CreateEditorTrench(x, y, z, heading, width, depth, length, tank))
                    return HarnessProtocol::ErrorResponse(EditorCaveStatus());
            }
            else if (std::strcmp(action, "undo") == 0)
            {
                if (!RemoveLastEditorCave())
                    return HarnessProtocol::ErrorResponse(EditorCaveStatus());
            }
            else if (std::strcmp(action, "select") == 0)
            {
                if (!SelectEditorCave(recordId))
                    return HarnessProtocol::ErrorResponse(EditorCaveStatus());
            }
            else if (std::strcmp(action, "rotate") == 0)
            {
                float heading;
                if (!number("heading", heading))
                    return HarnessProtocol::ErrorResponse("rotate needs finite heading");
                if (!RotateEditorCave(recordId, heading))
                    return HarnessProtocol::ErrorResponse(EditorCaveStatus());
            }
            else if (std::strcmp(action, "delete") == 0)
            {
                if (!DeleteEditorCave(recordId))
                    return HarnessProtocol::ErrorResponse(EditorCaveStatus());
            }
            else if (std::strcmp(action, "state") != 0)
                return HarnessProtocol::ErrorResponse("state, create, trench, undo, select, rotate or delete only");
            auto* out = cJSON_CreateObject();
            cJSON_AddNumberToObject(out, "count", double(EditorCaveCount()));
            cJSON_AddNumberToObject(out, "selectedId", SelectedEditorCaveId());
            auto* records = cJSON_AddArrayToObject(out, "records");
            for (const auto& r : EditorCaves())
            {
                auto* entry = cJSON_CreateObject();
                cJSON_AddItemToArray(records, entry);
                cJSON_AddNumberToObject(entry, "id", r.id);
                cJSON_AddNumberToObject(entry, "objectId", r.objectId);
                cJSON_AddStringToObject(entry, "sourceName", r.sourceName.c_str());
                cJSON_AddStringToObject(entry, "kind",
                                        r.tool == ExcavationTool::Cave         ? "cave"
                                        : r.tool == ExcavationTool::TankTrench ? "tankTrench"
                                                                               : "infantryTrench");
                cJSON_AddNumberToObject(entry, "x", r.x);
                cJSON_AddNumberToObject(entry, "y", r.y);
                cJSON_AddNumberToObject(entry, "z", r.z);
                cJSON_AddNumberToObject(entry, "heading", r.heading);
                cJSON_AddNumberToObject(entry, "width", r.width);
                cJSON_AddNumberToObject(entry, "height", r.height);
                cJSON_AddNumberToObject(entry, "length", r.length);
            }
            cJSON_AddStringToObject(out, "status", EditorCaveStatus());
            cJSON_AddBoolToObject(out, "missionLocal", true);
            cJSON_AddStringToObject(out, "worldName", GLandscape->GetName());
            cJSON_AddNumberToObject(out, "grid", GLandscape->GetTerrainGrid());
            cJSON_AddNumberToObject(out, "heightRevision", double(GLandscape->HeightRevision()));
            cJSON_AddNumberToObject(out, "brushDefaultRadius", TerrainBrush().radius);
            if (point)
            {
                float floor = 0;
                cJSON_AddBoolToObject(out, "inFootprint", GLandscape->InTerrainHole(x, z, &floor));
                cJSON_AddBoolToObject(out,"atHeight",GLandscape->InTerrainHole(x,z,nullptr,y));
                cJSON_AddNumberToObject(out,"holeFloor",floor);
                cJSON_AddNumberToObject(out,"terrainY",GLandscape->SurfaceY(x,z));
                cJSON_AddNumberToObject(out,"roadY",GLandscape->RoadSurfaceY(Vector3(x,y,z)));
                Vector3 camera(x,y,z);
                cJSON_AddNumberToObject(out,"cameraFloorY",GLandscape->CameraFloorY(camera,nullptr,false));
                if (cJSON_HasObjectItem(root,"rayFromY") || cJSON_HasObjectItem(root,"rayToY"))
                {
                    float fromY,toY;
                    if (!number("rayFromY",fromY) || !number("rayToY",toY) || fromY==toY || std::abs(fromY-toY)>100)
                    {
                        cJSON_Delete(out);
                        return HarnessProtocol::ErrorResponse("ray needs finite unequal heights, <=100m apart");
                    }
                    Vector3 hit;
                    const Vector3 from(x,fromY,z);
                    const float legacyResult=GLandscape->IntersectWithGround(&hit,from,
                        Vector3(0,toY>fromY ? 1.0f : -1.0f,0),0,std::abs(fromY-toY));
                    const float distance=(hit-from).Size();
                    // This legacy API returns maxDist*1.1 and an endpoint beyond
                    // the segment on MISS; it does not return a negative sentinel.
                    const bool hitSegment=legacyResult>=0 && distance<=std::abs(fromY-toY)+0.001f;
                    cJSON_AddBoolToObject(out,"terrainRayHit",hitSegment);
                    cJSON_AddNumberToObject(out,"legacyRayResult",legacyResult);
                    if (hitSegment)
                    {
                        cJSON_AddNumberToObject(out,"terrainRayY",hit.Y());
                        cJSON_AddNumberToObject(out,"terrainRayDistance",distance);
                    }
                }
                float edges[64*4];
                const int count=GLandscape->TerrainHoleDrawEdges(camera,400,edges,64);
                cJSON_AddNumberToObject(out,"drawRecords",count);
                cJSON_AddBoolToObject(out,"boundedHeader",count>0 && edges[3]==2);
                if (count>0 && edges[3]==2)
                    cJSON_AddNumberToObject(out, "ceilingY", edges[2]);
            }
            return HarnessProtocol::JsonResponse(out);
        });
    hs.RegisterCommand({"dev_terrain_brush", "Bounded real single-player terrain edit transaction", {}},
        [](const std::string&,cJSON* root) -> std::string {
            if (!AppConfig::Instance().DevMode() || !GWorld || !GLandscape || GWorld->GetMode()==GModeNetware)
                return HarnessProtocol::ErrorResponse("terrain brush requires --dev and a single-player world");
            const char* action=HarnessProtocol::GetString(root,"action");
            if (!action) return HarnessProtocol::ErrorResponse("terrain brush needs action");
            const int range=GLandscape->GetTerrainRange(); const float spacing=GLandscape->GetTerrainGrid();
            if (range<2 || range>2049 || !std::isfinite(spacing) || spacing<=0)
                return HarnessProtocol::ErrorResponse("unsupported actual terrain grid");
            const auto* x=cJSON_GetObjectItemCaseSensitive(root,"x");
            const auto* z=cJSON_GetObjectItemCaseSensitive(root,"z");
            const bool selected=x||z; const double extent=double(range-1)*spacing;
            if (selected && (!cJSON_IsNumber(x)||!cJSON_IsNumber(z)||!std::isfinite(x->valuedouble)||
                !std::isfinite(z->valuedouble)||x->valuedouble<0||z->valuedouble<0||x->valuedouble>extent||z->valuedouble>extent))
                return HarnessProtocol::ErrorResponse("terrain sample needs finite in-domain x/z");
            if (std::strcmp(action,"paint")==0) {
                const auto* radius=cJSON_GetObjectItemCaseSensitive(root,"radius");
                const auto* delta=cJSON_GetObjectItemCaseSensitive(root,"delta");
                if (!selected || !cJSON_IsNumber(radius)||!cJSON_IsNumber(delta)||
                    !std::isfinite(radius->valuedouble)||!std::isfinite(delta->valuedouble)||
                    radius->valuedouble<=0||radius->valuedouble>100||delta->valuedouble==0||std::abs(delta->valuedouble)>8||
                    TerrainBrushRadius(float(radius->valuedouble),spacing)>100)
                    return HarnessProtocol::ErrorResponse("paint needs radius (0,100], nonzero delta [-8,8], finite x/z");
                if (!PaintTerrain(float(x->valuedouble),float(z->valuedouble),float(radius->valuedouble),float(delta->valuedouble)))
                    return HarnessProtocol::ErrorResponse("actual terrain/collision edit transaction refused");
            } else if (std::strcmp(action,"restore")==0) {
                if (!RestoreEditedTerrain()) return HarnessProtocol::ErrorResponse("actual terrain restore transaction refused");
            } else if (std::strcmp(action,"state")!=0) return HarnessProtocol::ErrorResponse("state, paint or restore only");
            auto* out=cJSON_CreateObject();
            cJSON_AddBoolToObject(out,"readonly",std::strcmp(action,"state")==0);
            cJSON_AddBoolToObject(out,"baseline",HasTerrainEditBaseline());
            cJSON_AddBoolToObject(out,"fineSourceReady",GRainWater().SourceReady());
            cJSON_AddNumberToObject(out,"range",range); cJSON_AddNumberToObject(out,"spacing",spacing);
            cJSON_AddNumberToObject(out,"heightRevision",double(GLandscape->HeightRevision()));
            cJSON_AddNumberToObject(out,"seaLevel",GLandscape->GetSeaLevel());
            if (selected) {
                const int ix=std::clamp(int(std::round(x->valuedouble/spacing)),0,range-1);
                const int iz=std::clamp(int(std::round(z->valuedouble/spacing)),0,range-1);
                cJSON_AddNumberToObject(out,"vertexX",ix); cJSON_AddNumberToObject(out,"vertexZ",iz);
                cJSON_AddNumberToObject(out,"vertexWorldX",ix*spacing); cJSON_AddNumberToObject(out,"vertexWorldZ",iz*spacing);
                cJSON_AddNumberToObject(out,"vertexHeight",GLandscape->GetHeight(iz,ix));
            }
            if (const auto* world=Physics::GetPhysicsWorld()) {
                const auto stats=world->GetStats();
                cJSON_AddBoolToObject(out,"terrainRegistered",stats.terrainRegistered);
                cJSON_AddNumberToObject(out,"physicsWidth",stats.terrainWidth); cJSON_AddNumberToObject(out,"physicsHeight",stats.terrainHeight);
                cJSON_AddNumberToObject(out,"physicsSpacing",stats.terrainCellSize);
            }
            return HarnessProtocol::JsonResponse(out);
        });
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

static SDL_WindowID HarnessInputWindow()
{
    if (SDL_Window* window = SDL_GetKeyboardFocus()) return SDL_GetWindowID(window);
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    const SDL_WindowID id = windows && count == 1 ? SDL_GetWindowID(windows[0]) : 0;
    SDL_free(windows);
    return id;
}

void RegisterKeyInjection(HarnessServer& hs)
{
    hs.RegisterCommand({"mouse_motion", "Inject SDL mouse movement; optional window position for ImGui",
                        {{"dx","int",true},{"dy","int",true},{"x","int",false},{"y","int",false}}},
        [](const std::string&, cJSON* root) -> std::string {
            const int dx = HarnessProtocol::GetInt(root,"dx");
            const int dy = HarnessProtocol::GetInt(root,"dy");
            if (dx < -1000 || dx > 1000 || dy < -1000 || dy > 1000)
                return HarnessProtocol::ErrorResponse("mouse movement out of range");
            SDL_Event event{};
            event.type = SDL_EVENT_MOUSE_MOTION;
            event.motion.windowID = HarnessInputWindow();
            event.motion.xrel = static_cast<float>(dx);
            event.motion.yrel = static_cast<float>(dy);
            const bool hasX = cJSON_HasObjectItem(root, "x");
            const bool hasY = cJSON_HasObjectItem(root, "y");
            if (hasX != hasY) return HarnessProtocol::ErrorResponse("window position requires x and y");
            if (hasX)
            {
                const int x = HarnessProtocol::GetInt(root, "x");
                const int y = HarnessProtocol::GetInt(root, "y");
                if (!GEngine || x < 0 || y < 0 || x >= GEngine->Width() || y >= GEngine->Height())
                    return HarnessProtocol::ErrorResponse("window position out of range");
                event.motion.x = static_cast<float>(x);
                event.motion.y = static_cast<float>(y);
            }
            if (!SDL_PushEvent(&event)) return HarnessProtocol::ErrorResponse("mouse event rejected");
            return HarnessProtocol::OkResponse();
        });
    hs.RegisterCommand({"mouse_button", "Inject SDL mouse button state", {{"button","int",true},{"down","bool",true}}},
        [](const std::string&, cJSON* root) -> std::string {
            const int button = HarnessProtocol::GetInt(root,"button");
            if (button < 1 || button > 5) return HarnessProtocol::ErrorResponse("invalid mouse button");
            SDL_Event event{};
            event.button.down = HarnessProtocol::GetBool(root,"down");
            event.button.windowID = HarnessInputWindow();
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
