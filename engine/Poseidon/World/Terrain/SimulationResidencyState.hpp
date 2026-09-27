#pragma once

#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Terrain/SimulationResidency.hpp>
#include <Poseidon/World/Terrain/SimulationResidencyPolicy.hpp>
#include <Poseidon/World/Terrain/SimulationDemand.hpp>
#include <Poseidon/World/Terrain/SimulationResidencyCapacity.hpp>
#include <Poseidon/World/Terrain/ObjectStreamPrepare.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/Foundation/Common/Filenames.hpp>

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <deque>
#include <optional>
#include <algorithm>
#include <memory>
#include <string_view>

namespace Poseidon::Streaming
{

// Allocated only by the default-off owner service. All weak records and shape
// references are created, read and destroyed on the owner, never by AI workers.
struct SimulationResidencyState
{
    struct RegionKey
    {
        const Object* actor = nullptr;
        uint8_t channel = 0; // 0 neighborhood, SimulationQueryPurpose otherwise
        bool operator==(const RegionKey&) const = default;
    };
    struct RegionKeyHash
    {
        size_t operator()(const RegionKey& key) const
        { return std::hash<const Object*>{}(key.actor) ^ (size_t(key.channel) * size_t(0x9e3779b9)); }
    };
    enum class ModelState { Waiting, Ready, Unknown, CapacityExceeded };
    enum class PositiveColdStage { Parse, Convert, Held };
    struct PositiveColdFacts
    {
        ObjectStreamPreparer::SourceEnvelopeSnapshot source;
        PositiveColdStage stage = PositiveColdStage::Parse;
        uint32_t selectedPlacement = 0;
        uint64_t shapeRevision = 0, factCharge = 0;
        uint32_t residentEntries = 0;
        bool retiring = false;
    };
    // Complete authored inventory metadata, no duplicate placement indices.
    // Group's full fixed record (including empty active fields) is byte charged.
    struct ModelMetadataRecord
    {
        uint64_t memberCount = 0;
        CoverageEnvelope origins{};
        bool validRows = true;
        double maximumScale = 0;
    };
    struct Group : ModelMetadataRecord
    {
        bool sourceValidated = false, charged = false, requestOutstanding = false;
        Ref<LODShapeWithShadow> shape;
        // Selected query ownership, deduplicated per model. The current full
        // producer already charges this same shape; this is not another asset
        // allocation or permission to admit metadata beyond its model cap.
        Ref<LODShapeWithShadow> queryBorrow;
        uint32_t queryEntries = 0;
        // A weak observation of an actual cached shape, not source-file evidence
        // or durable config eligibility. Every use rechecks the canonical bank
        // and current config on the owner. No full-inventory strong retention.
        Link<LODShape> registeredShape;
        uint64_t registeredRevision = 0, registeredBorrowCharge = 0;
        float registeredRadius = 0;
        ClipFlags registeredHints = 0;
        uint64_t revision = 0, payloadCharge = 0, shapeCharge = 0;
        float radius = 0;
        SimulationFactoryRoute route = SimulationFactoryRoute::Unsupported;
        ModelState status = ModelState::Waiting;
        std::unique_ptr<PositiveColdFacts> positiveCold;
        bool positiveColdRefused = false;
        bool CanEnlistPositiveCold() const
        {
            // A legacy handoff can own charges/request intent before a shape
            // exists. Cold admission must never replace that accounting.
            return status == ModelState::Waiting && !positiveCold && !positiveColdRefused &&
                !shape && !queryBorrow && queryEntries == 0 && !charged && !requestOutstanding && !sourceValidated &&
                payloadCharge == 0 && shapeCharge == 0;
        }
    };
    struct Entry
    {
        OLink<Object> object;
        Link<LODShape> shape;
        std::array<float, 12> frame{};
        float radius = 0;
        uint64_t positiveRevision = 0;
        int positiveId = -1;
        uint32_t positiveColdModel = std::numeric_limits<uint32_t>::max();
        uint32_t queryModel = std::numeric_limits<uint32_t>::max();
        bool MatchesPositiveResident(const Object& live) const
        {
            auto* current = live.GetShape();
            return static_cast<Object*>(object) == &live && current && current == shape.GetRef() &&
                positiveRevision != 0 && current->QueryPolicyRevision() == positiveRevision &&
                SimulationRoute(static_cast<const char*>(current->GetPropertyClass())) == SimulationFactoryRoute::Plain &&
                live.ID() == positiveId && live.Static() && live.GetType() == Primary && !live.MustBeSaved() && !live.IsDestroyed() &&
                live.GetDestroyed() == 0 && live.GetRawTotalDammage() == 0 &&
                SimulationFrameSample(live.Transform()) == frame && live.GetRadius() == radius;
        }
    };
    // Positive observation of an ALREADY resident Plain instance only. No group
    // shape, source certificate, factory, config lookup or query readiness.
    static std::optional<Entry> ObservePositiveResident(Object& object, int authoredId,
        const std::array<float, 12>& authoredRows)
    {
        if (!Foundation::IsMainThread()) return std::nullopt;
        auto* shape = object.GetShape();
        if (!shape || object.ID() != authoredId ||
            SimulationRoute(static_cast<const char*>(shape->GetPropertyClass())) != SimulationFactoryRoute::Plain ||
            !SimulationShapeAdmissible(*shape)) return std::nullopt;
        const auto canonical = RepairWorldObjectPlacementFrame(SimulationPlacementFrame(authoredRows), false, false,
            (shape->GetOrHints() & ClipLandKeep) != 0);
        if (!ValidSimulationFrame(canonical) || !std::isfinite(object.GetRadius()) ||
            object.GetRadius() != canonical.Scale() * shape->BoundingSphere()) return std::nullopt;
        Entry out; out.object = &object; out.shape = shape;
        out.frame = SimulationFrameSample(canonical); out.radius = object.GetRadius();
        out.positiveRevision = shape->QueryPolicyRevision();
        out.positiveId = authoredId;
        if (!out.MatchesPositiveResident(object)) return std::nullopt;
        return out;
    }
    struct Watch
    {
        OLink<Object> object;
        Link<LODShape> shape;
        std::array<float, 12> frame{};
        uint32_t model = 0;
        int id = -1, type = 0;
        float radius = 0;
        bool authored = false;
        // Only the owner may read weak witnesses. Shape policy/content scope is
        // validated separately; arbitrary mutable per-LOD APIs are excluded.
        bool MatchesImmutableObject(const LODShapeWithShadow* expectedShape, SimulationFactoryRoute expectedRoute) const
        {
            Object* live = static_cast<Object*>(object);
            if (!live) return true; // Joined cleanup may retain a harmless tombstone.
            return authored && live->ID() == id && live->Static() && live->GetType() == type &&
                live->GetType() == (expectedRoute == SimulationFactoryRoute::Road ? Network : Primary) &&
                live->GetShape() == shape.GetRef() && live->GetShape() == expectedShape &&
                SimulationFrameSample(live->Transform()) == frame && live->GetRadius() == radius &&
                !live->MustBeSaved() && !live->IsDestroyed() && live->GetDestroyed() == 0 && live->GetRawTotalDammage() == 0;
        }
    };
    struct Region
    {
        OLink<Object> actor;
        CoveragePoint from{}, to{};
        double radius = 0;
        std::array<float, 12> actorFrame{};
        std::array<float, 3> actorSpeed{};
        float actorRadius = 0;
        int actorId = -1;
        bool automatic = false;
        bool positiveOnly = false, positiveScanComplete = false;
        uint64_t positiveAdmissionGeneration = 0;
        size_t positiveValidationCursor = 0;
        uint64_t seen = 0, planGeneration = 0;
        uint64_t automaticSeenVisit = 0;
        SimulationFireHeartbeat fireHeartbeat;
        SimulationResidencyStatus status = SimulationResidencyStatus::Pending;
        SimulationDemand plan;
        size_t cell = 0, offset = 0, pendingNew = 0;
        std::vector<uint32_t> active, pending;
        std::unordered_set<uint32_t> activeSet, pendingSet;
        bool FireInterestExpired(uint8_t channel, SimulationFireHeartbeat::Clock::time_point now) const
        {
            return channel == uint8_t(SimulationQueryPurpose::Fire) && fireHeartbeat.Expired(now);
        }
        SimulationResidencyStatus FireStatusAtPublication(uint8_t channel, SimulationResidencyStatus status,
            SimulationFireHeartbeat::Clock::time_point now) const
        {
            return status == SimulationResidencyStatus::Ready && FireInterestExpired(channel, now) ?
                SimulationResidencyStatus::Pending : status;
        }
        bool NeedsPositiveRestart(uint64_t worldGeneration, uint64_t admissionGeneration) const
        {
            return !positiveOnly || planGeneration != worldGeneration ||
                (positiveScanComplete && positiveAdmissionGeneration != admissionGeneration);
        }
    };

    SimulationResidencyStats stats;
    std::vector<Group> groups;
    // Sparse, bounded live lease debt; zero-count rows are erased. There is no
    // allocation proportional to the landscape's millions of placement rows.
    std::unordered_map<uint32_t, uint32_t> leaseCounts;
    std::unordered_map<uint32_t, Entry> entries;
    std::deque<uint32_t> cleanupQueue;
    // Complete bounded registry of actual modern-placement admissions. Weak
    // watchers survive lease0; they neither instantiate nor retain an Object.
    std::unordered_map<uint32_t, Watch> watches;
    std::deque<uint32_t> watchCleanupQueue;
    bool watchCapacityExceeded = false;
    std::unordered_map<RegionKey, Region, RegionKeyHash> regions;
    size_t inventoryCursor = 0, modelCursor = 0, actorCursor = 0, regionCursor = 0;
    uint64_t tick = 0, generation = 1;
    uint64_t automaticActorVisits = 0;
    mutable SimulationFireCollisionConsumption fireCollisionConsumption;
    uint64_t positiveAdmissionGeneration = 1;
    std::vector<uint32_t> positiveColdWork;
    size_t positiveColdCursor = 0, legacyShapeSlots = 0;
    bool inventoryComplete = false, inventoryInvalid = false;
    uint64_t referencedModels = 0;
    bool activeModelCapacityExceeded = false;
    double placementPadding = 0;
    size_t registeredCursor = 0, registeredPaddingCursor = 0, registeredBorrowedModels = 0;
    uint64_t registeredBorrowShapeCharge = 0;
    mutable uint64_t registeredValidationBudgetRefusals = 0;
    bool registeredMetadataEverComplete = false, registeredMetadataDirty = false;
    double registeredPadding = 0;

    // Lexical owner-operation view only: never stored in Group/State or returned
    // by Landscape. Config pointers/string_views die before that operation ends.
    struct RegisteredConfigView
    {
        struct Named { std::string_view name; const ParamEntry* entry; };
        std::vector<Named> models;
        RegisteredConfigView() = default;
        RegisteredConfigView(const RegisteredConfigView&) = delete;
        RegisteredConfigView& operator=(const RegisteredConfigView&) = delete;
    };
    static std::optional<std::string_view> RegisteredVisible(const char* text, size_t cap)
    {
        if (!text) return std::nullopt;
        for (size_t n = 0; n <= cap; ++n)
        {
            if (!text[n]) return std::string_view(text, n);
            if (static_cast<unsigned char>(text[n]) < 32 || static_cast<unsigned char>(text[n]) > 126) return std::nullopt;
        }
        return std::nullopt;
    }
    static int CompareRegisteredName(std::string_view a, std::string_view b)
    {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c + 'a' - 'A') : c; };
        for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
            if (lower(a[i]) != lower(b[i])) return lower(a[i]) < lower(b[i]) ? -1 : 1;
        return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
    }
    template<class Budget>
    static SimulationResidencyStatus AuditRegisteredGeometryNow(const LODShapeWithShadow& shape, Budget&& budget)
    {
        using Status = SimulationResidencyStatus;
        if (!Foundation::IsMainThread()) return Status::WrongOwner;
        const double parentRadius = shape.BoundingSphere();
        if (!std::isfinite(parentRadius) || parentRadius < 0) return Status::Unknown;
        // BoundingSphere is centred on the adjusted model-coordinate origin,
        // not BoundingCenter/GeometryCenter. Read the actual query LOD positions
        // and the boxes/spheres used by Object::Intersect. ODOL authored bounds
        // are NOT presumed to enclose them. No Calculate/ReadLOD/setter is used.
        const std::array<int, 3> roles = {shape.FindGeometryLevel(), shape.FindFireGeometryLevel(), shape.FindViewGeometryLevel()};
        for (size_t role = 0; role < roles.size(); ++role)
        {
            if (!budget()) return Status::Pending;
            if (std::find(roles.begin(), roles.begin() + role, roles[role]) != roles.begin() + role) continue;
            if (roles[role] < 0 || roles[role] >= shape.NLevels()) return Status::Unknown;
            const auto* level = shape.Level(roles[role]); if (!level) return Status::Unknown;
            // Conforming query vertices can move during Object::Intersect.
            // Per-level hints and raw clips are not parent-revision witnesses.
            if (level->GetOrHints() & (ClipLandKeep | ClipLandOn)) return Status::Unknown;
            const auto min = level->Min(), max = level->Max(), center = level->BSphereCenter();
            const double radius = level->BSphereRadius();
            if (!std::isfinite(radius) || radius < 0) return Status::Unknown;
            for (size_t axis = 0; axis < 3; ++axis)
                if (!std::isfinite(min[axis]) || !std::isfinite(max[axis]) || !std::isfinite(center[axis]) || min[axis] > max[axis]) return Status::Unknown;
            // Float sqrt/multiply can round the stored sphere down. Permit only
            // relative float roundoff; no absolute slack near zero radii.
            const double parentLimit = parentRadius * (1 + 32 * std::numeric_limits<float>::epsilon());
            const double levelLimit = radius * (1 + 32 * std::numeric_limits<float>::epsilon());
            for (int i = 0; i < level->NPos(); ++i)
            {
                if (!budget()) return Status::Pending;
                const auto point = level->Pos(i);
                if (level->Clip(i) & (ClipLandKeep | ClipLandOn)) return Status::Unknown;
                double originDistance2 = 0, centerDistance2 = 0;
                for (size_t axis = 0; axis < 3; ++axis)
                {
                    const double value = point[axis], delta = value - double(center[axis]);
                    if (!std::isfinite(value) || value < min[axis] || value > max[axis]) return Status::Unknown;
                    originDistance2 += value * value; centerDistance2 += delta * delta;
                }
                if (!std::isfinite(originDistance2) || !std::isfinite(centerDistance2) ||
                    originDistance2 > parentLimit * parentLimit || centerDistance2 > levelLimit * levelLimit) return Status::Unknown;
            }
            // Reject malformed vertex references before the existing convex
            // narrow phase. This is not a complete convex-topology/plane proof.
            for (auto face = level->BeginFaces(); face < level->EndFaces(); level->NextFace(face))
            {
                if (!budget()) return Status::Pending;
                const auto& polygon = level->Face(face);
                if (polygon.N() < 3 || polygon.N() > MaxPoly) return Status::Unknown;
                for (int vertex = 0; vertex < polygon.N(); ++vertex)
                    if (polygon.GetVertex(vertex) >= level->NPos()) return Status::Unknown;
            }
        }
        return budget() ? Status::Ready : Status::Pending;
    }
    template<class Budget>
    static SimulationResidencyStatus BuildRegisteredConfigView(RegisteredConfigView& view, const ParamEntry& root, Budget&& budget)
    {
        using Status = SimulationResidencyStatus;
        if (!Foundation::IsMainThread()) return Status::WrongOwner;
        view.models.clear();
        const auto* cls = root.GetClassInterface();
        if (!cls || root.IsError() || cls->HasBase() || root.GetEntryCount() < 0 || root.GetEntryCount() > 4096) return Status::Unknown;
        const ParamEntry* models = nullptr;
        size_t nameBytes = 0;
        for (int i = 0; i < root.GetEntryCount(); ++i)
        {
            if (!budget()) return Status::Pending;
            const auto& entry = root.GetEntry(i); const auto name = RegisteredVisible(entry.GetName().Data(), 256);
            if (entry.IsError() || !name || name->empty() || name->size() > 1024 * 1024 - nameBytes) return Status::Unknown;
            nameBytes += name->size();
            if (CompareRegisteredName(*name, "CfgModels") == 0)
            { if (models) return Status::Unknown; models = &entry; }
        }
        if (!models) return budget() ? Status::Ready : Status::Pending;
        cls = models->GetClassInterface();
        if (!cls || models->IsError() || cls->HasBase() || models->GetEntryCount() < 0 || models->GetEntryCount() > 4096) return Status::Unknown;
        view.models.reserve(size_t(models->GetEntryCount())); // <=4096 fixed-size references; no source/shape duplication
        for (int i = 0; i < models->GetEntryCount(); ++i)
        {
            if (!budget()) return Status::Pending;
            const auto& entry = models->GetEntry(i); const auto name = RegisteredVisible(entry.GetName().Data(), 256);
            if (entry.IsError() || !name || name->empty() || name->size() > 1024 * 1024 - nameBytes) return Status::Unknown;
            nameBytes += name->size(); view.models.push_back({*name, &entry});
        }
        std::sort(view.models.begin(), view.models.end(), [](const auto& a, const auto& b) { return CompareRegisteredName(a.name, b.name) < 0; });
        for (size_t i = 1; i < view.models.size(); ++i)
            if (CompareRegisteredName(view.models[i - 1].name, view.models[i].name) == 0) return Status::Unknown;
        return budget() ? Status::Ready : Status::Pending;
    }

    // Current engine-visible strings only. This proves no parser/source coverage
    // and does not borrow the source-summary EmptyClassNow verdict. Unsupported
    // inheritance/encodings refuse instead of emulating an unbounded lookup.
    template<class Budget>
    static SimulationResidencyStatus ProbeRegisteredPlainNow(const ShapeBank& bank, const char* path,
        const ParamEntry& config, LODShapeWithShadow*& result, Budget&& budget, const RegisteredConfigView* sameOperation = nullptr)
    {
        using Status = SimulationResidencyStatus;
        result = nullptr;
        if (!Foundation::IsMainThread()) return Status::WrongOwner;
        if (!budget()) return Status::Pending;
        if (!path) return Status::Unknown;
        const auto visible = [](const char* text, size_t cap) -> std::optional<std::string_view> {
            if (!text) return std::nullopt;
            for (size_t n = 0; n <= cap; ++n)
            {
                if (!text[n]) return std::string_view(text, n);
                if (static_cast<unsigned char>(text[n]) < 32 || static_cast<unsigned char>(text[n]) > 126) return std::nullopt;
            }
            return std::nullopt;
        };
        const auto identity = visible(path, 127);
        if (!identity || identity->empty()) return Status::Unknown;
        auto* shape = bank.Find(path, false, true);
        if (!shape) return Status::Unknown;
        const auto name = visible(shape->Name(), 127);
        const auto equal = [](std::string_view a, std::string_view b) {
            if (a.size() != b.size()) return false;
            const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c + 'a' - 'A') : c; };
            for (size_t i = 0; i < a.size(); ++i) if (lower(a[i]) != lower(b[i])) return false;
            return true;
        };
        if (!name || !equal(*identity, *name) || *shape->GetPropertyClass() != 0 ||
            shape->NLevels() > MAX_LOD_LEVELS || !SimulationShapeAdmissible(*shape)) return Status::Unknown;
        // VertexTable setters/mutable references are not versioned by the parent
        // QueryPolicyRevision. Never cache this point/bound verdict across an
        // owner operation: every complete validation re-reads all three roles.
        const auto geometryStatus = AuditRegisteredGeometryNow(*shape, budget);
        if (geometryStatus != Status::Ready) return geometryStatus;
        const auto find = [&](const ParamEntry& parent, std::string_view key, int cap,
                              const ParamEntry*& found) -> Status {
            found = nullptr;
            const auto* cls = parent.GetClassInterface();
            if (!cls || parent.IsError() || cls->HasBase()) return Status::Unknown;
            const int count = parent.GetEntryCount();
            if (count < 0 || count > cap) return Status::Unknown;
            for (int i = 0; i < count; ++i)
            {
                if (!budget()) return Status::Pending;
                const auto& entry = parent.GetEntry(i);
                const auto text = visible(entry.GetName().Data(), 256);
                if (entry.IsError() || !text || text->empty()) return Status::Unknown;
                if (equal(*text, key)) { if (found) return Status::Unknown; found = &entry; }
            }
            return Status::Ready;
        };
        char basename[128]{}; Foundation::GetFilename(basename, path);
        for (char& c : basename) if (c == ' ' || c == '-' || c == '/' || c == '(' || c == ')') c = '_';
        const auto shortName = visible(basename, 127);
        if (!shortName || shortName->empty()) return Status::Unknown;
        const ParamEntry *models = nullptr, *model = nullptr, *properties = nullptr;
        auto status = Status::Ready;
        if (sameOperation)
        {
            const auto found = std::lower_bound(sameOperation->models.begin(), sameOperation->models.end(), *shortName,
                [](const auto& entry, std::string_view name) { return CompareRegisteredName(entry.name, name) < 0; });
            if (found != sameOperation->models.end() && CompareRegisteredName(found->name, *shortName) == 0) model = found->entry;
        }
        else status = find(config, "CfgModels", 4096, models);
        if (status != Status::Ready) return status;
        if (models || model)
        {
            if (models) status = find(*models, *shortName, 4096, model);
            if (status != Status::Ready) return status;
            if (model)
            {
                status = find(*model, "properties", 128, properties);
                if (status != Status::Ready) return status;
                if (properties)
                {
                    if (properties->IsError() || !properties->IsArray()) return Status::Unknown;
                    const int count = properties->GetSize();
                    if (count < 0 || count > 256 || count % 2) return Status::Unknown;
                    for (int i = 0; i < count; i += 2)
                    {
                        if (!budget()) return Status::Pending;
                        const auto& key = (*properties)[i]; const auto& value = (*properties)[i + 1];
                        if (!key.IsTextValue() || !value.IsTextValue()) return Status::Unknown;
                        const auto keyString = key.GetValue(), valueString = value.GetValue();
                        const auto keyText = visible(keyString.Data(), 256), valueText = visible(valueString.Data(), 256);
                        if (!keyText || keyText->empty() || !valueText || (equal(*keyText, "class") && !valueText->empty())) return Status::Unknown;
                    }
                }
            }
        }
        if (!budget()) return Status::Pending;
        result = shape; return Status::Ready;
    }
    bool RegisteredShapeMatches(uint32_t index, const LODShapeWithShadow* shape) const
    {
        if (!Foundation::IsMainThread() || index >= groups.size() || !shape) return false;
        const auto& g = groups[index];
        return !g.positiveCold && g.registeredShape.GetRef() == shape && g.registeredRevision != 0 &&
            g.registeredRevision == shape->QueryPolicyRevision() && g.registeredRadius == shape->BoundingSphere() &&
            g.registeredHints == shape->GetOrHints() && *shape->GetPropertyClass() == 0;
    }
    template<class Budget>
    SimulationResidencyStatus RefreshRegisteredModel(uint32_t index, const ShapeBank& bank, const char* path,
        const ParamEntry& config, bool& changed, Budget&& budget)
    {
        using Status = SimulationResidencyStatus;
        if (index >= groups.size() || !groups[index].validRows || groups[index].positiveCold) return Status::Unknown;
        auto& g = groups[index]; LODShapeWithShadow* shape = nullptr;
        const auto status = ProbeRegisteredPlainNow(bank, path, config, shape, budget);
        if (status != Status::Ready) return status;
        if (!RegisteredShapeMatches(index, shape))
        {
            g.registeredShape = static_cast<LODShape*>(shape); g.registeredRevision = shape->QueryPolicyRevision();
            g.registeredRadius = shape->BoundingSphere(); g.registeredHints = shape->GetOrHints();
            changed = registeredMetadataDirty = true; registeredPaddingCursor = 0; registeredPadding = 0;
        }
        return Status::Ready;
    }
    template<class Budget>
    SimulationResidencyStatus ValidateAdmittedMetadata(Budget&& budget) const
    {
        using Status = SimulationResidencyStatus;
        if (!Foundation::IsMainThread()) return Status::WrongOwner;
        if (watchCapacityExceeded || stats.status == Status::CapacityExceeded || activeModelCapacityExceeded)
            return Status::CapacityExceeded;
        if (inventoryInvalid) return Status::Unknown;
        if (!inventoryComplete) return Status::Pending;
        bool pending = false;
        for (const auto& g : groups)
        {
            if (!budget()) return Status::Pending;
            if (g.status == ModelState::CapacityExceeded) return Status::CapacityExceeded;
            if (g.status == ModelState::Unknown || !g.validRows) return Status::Unknown;
            if (g.status != ModelState::Ready) { pending = true; continue; }
            if (!g.memberCount) continue;
            if (!g.shape || !g.revision || g.revision != g.shape->QueryPolicyRevision() ||
                g.radius != g.shape->BoundingSphere() || !SimulationShapeAdmissible(*g.shape)) return Status::Unknown;
            // Finite source vertices and a parent policy revision are not an
            // enclosure proof: authored bounds and per-LOD setters can differ.
            // Re-read every referenced model, including origins outside the query,
            // under ONE caller-owned deadline. No cached/prefix verdict is Ready.
            const auto geometry = AuditRegisteredGeometryNow(*g.shape, budget);
            if (geometry != Status::Ready) return geometry;
        }
        return budget() && !pending ? Status::Ready : Status::Pending;
    }
    template<class Budget>
    SimulationResidencyStatus ValidateRegisteredMetadata(const ShapeBank& bank, const std::vector<RStringB>& paths,
        const ParamEntry& config, Budget&& budget) const
    {
        using Status = SimulationResidencyStatus;
        if (!Foundation::IsMainThread()) return Status::WrongOwner;
        if (watchCapacityExceeded || stats.status == Status::CapacityExceeded) return Status::CapacityExceeded;
        if (inventoryInvalid || paths.size() != groups.size()) return Status::Unknown;
        if (!inventoryComplete) return Status::Pending;
        RegisteredConfigView currentConfig;
        const auto configStatus = BuildRegisteredConfigView(currentConfig, config, budget);
        if (configStatus != Status::Ready)
        { if (configStatus == Status::Pending) ++registeredValidationBudgetRefusals; return configStatus; }
        for (uint32_t i = 0; i < groups.size(); ++i)
        {
            const auto& g = groups[i]; if (!g.memberCount) continue;
            if (!budget()) { ++registeredValidationBudgetRefusals; return Status::Pending; }
            if (!g.validRows) return Status::Unknown;
            LODShapeWithShadow* shape = nullptr;
            const auto status = ProbeRegisteredPlainNow(bank, paths[i], config, shape, budget, &currentConfig);
            if (status != Status::Ready)
            { if (status == Status::Pending) ++registeredValidationBudgetRefusals; return status; }
            if (!RegisteredShapeMatches(i, shape)) return Status::Unknown;
        }
        return Status::Ready; // Metadata only, NEVER region/query geometry readiness.
    }
    bool CanScheduleShape(uint64_t charge) const
    {
        return registeredBorrowShapeCharge <= SimulationMaxShapeCharge &&
            stats.shapeSchedulingCharge <= SimulationMaxShapeCharge - registeredBorrowShapeCharge &&
            charge <= SimulationMaxShapeCharge - registeredBorrowShapeCharge - stats.shapeSchedulingCharge;
    }
    template<class Budget>
    SimulationResidencyStatus AdvanceRegisteredPadding(const ShapeBank& bank, const std::vector<RStringB>& paths,
        const ParamEntry& config, Budget&& budget)
    {
        using Status = SimulationResidencyStatus;
        const auto proof = ValidateRegisteredMetadata(bank, paths, config, budget);
        if (proof != Status::Ready) return proof;
        if (!registeredMetadataDirty) return registeredMetadataEverComplete ? Status::Ready : Status::Pending;
        size_t visits = 0;
        while (registeredPaddingCursor < groups.size() && visits++ < 1024 && budget())
        {
            const auto& g = groups[registeredPaddingCursor++]; if (!g.memberCount) continue;
            const auto upper = BuildEngineBroadphaseEnvelope(g.origins.max, g.maximumScale, g.registeredRadius);
            const auto lower = BuildEngineBroadphaseEnvelope(g.origins.min, g.maximumScale, g.registeredRadius);
            if (!upper || !lower) { inventoryInvalid = true; return Status::Unknown; }
            for (size_t axis = 0; axis < 3; ++axis)
                registeredPadding = std::max({registeredPadding,
                    upper->max[axis] - g.origins.max[axis], g.origins.min[axis] - lower->min[axis]});
        }
        if (registeredPaddingCursor != groups.size()) return Status::Pending;
        const auto finalProof = ValidateRegisteredMetadata(bank, paths, config, budget);
        if (finalProof != Status::Ready) return finalProof;
        if (generation == std::numeric_limits<uint64_t>::max()) { inventoryInvalid = true; return Status::Unknown; }
        ++generation; placementPadding = registeredPadding;
        registeredMetadataDirty = false; registeredMetadataEverComplete = true;
        return Status::Ready;
    }
    enum class RegisteredBorrowResult { Held, Unknown, CapacityExceeded };
    RegisteredBorrowResult HoldRegisteredQueryModel(uint32_t index, LODShapeWithShadow* shape)
    {
        using Result = RegisteredBorrowResult;
        if (!Foundation::IsMainThread() || !activeModelCapacityExceeded || watchCapacityExceeded || inventoryInvalid || !registeredMetadataEverComplete ||
            registeredMetadataDirty || !RegisteredShapeMatches(index, shape)) return Result::Unknown;
        auto& g = groups[index];
        if (g.shape || g.charged || g.requestOutstanding || g.positiveCold) return Result::Unknown;
        if (g.queryBorrow) return g.registeredBorrowCharge && g.queryBorrow.GetRef() == shape ? Result::Held : Result::Unknown;
        if (legacyShapeSlots + positiveColdWork.size() + registeredBorrowedModels >= SimulationMaxModels) return Result::CapacityExceeded;
        const uint64_t bytes = SimulationShapeElementBytes(*shape);
        if (bytes > (SimulationMaxShapeCharge - 1024 * 1024) / 4) return Result::CapacityExceeded;
        const uint64_t charge = bytes * 4 + 1024 * 1024;
        if (!CanScheduleShape(charge)) return Result::CapacityExceeded;
        g.queryBorrow = shape; g.registeredBorrowCharge = charge;
        ++registeredBorrowedModels; registeredBorrowShapeCharge += charge;
        return Result::Held;
    }
    void ReleaseUnusedRegisteredBorrow(uint32_t index)
    {
        if (!Foundation::IsMainThread() || index >= groups.size()) return;
        auto& g = groups[index];
        if (!g.registeredBorrowCharge || g.queryEntries) return;
        registeredBorrowShapeCharge -= g.registeredBorrowCharge; --registeredBorrowedModels;
        g.registeredBorrowCharge = 0; g.queryBorrow = nullptr;
    }

    bool MatchesQueryBorrow(const Entry& entry) const
    {
        if (!Foundation::IsMainThread() || entry.queryModel >= groups.size()) return false;
        const auto& g = groups[entry.queryModel];
        Object* object = static_cast<Object*>(entry.object);
        const bool modelCurrent = g.registeredBorrowCharge ? RegisteredShapeMatches(entry.queryModel, g.queryBorrow.GetRef()) :
            (g.status == ModelState::Ready && g.queryBorrow.GetRef() == g.shape.GetRef() &&
             g.revision != 0 && g.shape && g.shape->QueryPolicyRevision() == g.revision);
        return object && modelCurrent && g.queryEntries != 0 && g.queryBorrow &&
            object->GetShape() == g.queryBorrow.GetRef() && entry.shape.GetRef() == g.queryBorrow.GetRef() &&
            (g.registeredBorrowCharge || g.queryBorrow->QueryPolicyRevision() == g.revision);
    }
    bool AttachQueryBorrow(uint32_t model, Entry& entry)
    {
        if (!Foundation::IsMainThread() || model >= groups.size() ||
            watchCapacityExceeded || inventoryInvalid) return false;
        auto& g = groups[model];
        Object* object = static_cast<Object*>(entry.object);
        const bool registered = activeModelCapacityExceeded && g.registeredBorrowCharge &&
            registeredMetadataEverComplete && !registeredMetadataDirty && RegisteredShapeMatches(model, g.queryBorrow.GetRef());
        auto* expected = registered ? g.queryBorrow.GetRef() : g.shape.GetRef();
        if (!object || g.positiveCold || !expected || object->GetShape() != expected || entry.shape.GetRef() != expected ||
            (!registered && (activeModelCapacityExceeded || g.status != ModelState::Ready || g.revision == 0 || expected->QueryPolicyRevision() != g.revision))) return false;
        if (entry.queryModel != std::numeric_limits<uint32_t>::max())
            return entry.queryModel == model && MatchesQueryBorrow(entry);
        if (g.queryEntries == std::numeric_limits<uint32_t>::max() ||
            (g.queryBorrow && g.queryBorrow.GetRef() != expected) ||
            (!g.queryBorrow && g.queryEntries != 0)) return false;
        g.queryBorrow = expected; ++g.queryEntries; entry.queryModel = model;
        return true;
    }
    void ReleaseQueryBorrow(Entry& entry)
    {
        if (!Foundation::IsMainThread()) return;
        if (entry.queryModel < groups.size())
        {
            auto& g = groups[entry.queryModel];
            if (g.queryEntries && --g.queryEntries == 0)
            { if (g.registeredBorrowCharge) ReleaseUnusedRegisteredBorrow(entry.queryModel); else g.queryBorrow = nullptr; }
        }
        entry.queryModel = std::numeric_limits<uint32_t>::max();
    }

    void FinishInventory()
    {
        if (inventoryComplete) return;
        inventoryComplete = true;
        for (auto& g : groups) if (!g.memberCount) g.status = ModelState::Ready;
        if (referencedModels > SimulationMaxModels)
        { activeModelCapacityExceeded = true; ++stats.activeModelRefusals; }
    }

    void RetirePositiveCold(uint32_t index, bool refused = false)
    {
        auto& g = groups[index];
        if (!g.positiveCold) return;
        stats.positiveColdFactsBytes -= g.positiveCold->factCharge;
        if (g.charged)
        {
            stats.payloadCapacityCharge -= g.payloadCharge;
            stats.shapeSchedulingCharge -= g.shapeCharge;
        }
        g.shape = nullptr; g.positiveCold.reset(); g.positiveColdRefused = refused;
        g.requestOutstanding = g.charged = g.sourceValidated = false;
        g.payloadCharge = g.shapeCharge = g.revision = 0;
        auto found = std::find(positiveColdWork.begin(), positiveColdWork.end(), index);
        if (found != positiveColdWork.end()) positiveColdWork.erase(found);
        if (positiveColdWork.empty())
        { std::vector<uint32_t>().swap(positiveColdWork); positiveColdCursor = 0; }
    }

    bool HasLease(uint32_t index) const
    {
        return leaseCounts.contains(index);
    }
    bool CanAcquireLease(uint32_t index) const
    {
        return HasLease(index) || leaseCounts.size() < SimulationMaxLeasedPlacements;
    }
    bool AcquireLease(uint32_t index)
    {
        if (!CanAcquireLease(index)) return false;
        ++leaseCounts[index];
        return true;
    }
    void DropLease(uint32_t index)
    {
        const auto found = leaseCounts.find(index);
        if (found != leaseCounts.end())
        {
            if (found->second <= 1) leaseCounts.erase(found);
            else --found->second;
        }
    }
    void DropPending(Region& r)
    {
        for (auto i : r.pending) if (!r.activeSet.contains(i)) DropLease(i);
        r.pending.clear(); r.pendingSet.clear(); r.pendingNew = 0;
        r.plan = {}; r.cell = r.offset = 0; r.planGeneration = 0;
        r.positiveScanComplete = false; r.positiveValidationCursor = 0;
    }
    void DropRegion(Region& r)
    {
        DropPending(r);
        for (auto i : r.active) DropLease(i);
        r.active.clear(); r.activeSet.clear();
    }
    void CompleteReplacement(Region& r, bool completeCoverage = true)
    {
        for (auto i : r.active) if (!r.pendingSet.contains(i)) DropLease(i);
        r.active.swap(r.pending); r.activeSet.swap(r.pendingSet);
        r.pending.clear(); r.pendingSet.clear(); r.pendingNew = 0;
        r.positiveOnly = !completeCoverage;
        r.positiveScanComplete = !completeCoverage;
        r.status = completeCoverage ? SimulationResidencyStatus::Ready : SimulationResidencyStatus::Pending;
    }
};

} // namespace Poseidon::Streaming
