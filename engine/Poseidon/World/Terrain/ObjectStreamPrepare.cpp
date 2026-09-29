// Asynchronous cold-model preparation for streamed object admission. See the header for the
// thread-safety boundary; this file is the mechanism only.

#include <Poseidon/World/Terrain/ObjectStreamPrepare.hpp>
#include <Poseidon/World/Terrain/WarmTextureProvenance.hpp>
#include <Poseidon/World/Terrain/ObjectStreamTextureNames.hpp>
#include <Poseidon/World/Terrain/ObjectStreamRapStageNames.hpp>
#include <Poseidon/World/Terrain/ObjectStreamProxyMaterialScan.hpp>
#include <Poseidon/World/Terrain/ObjectStreamTreeFixture.hpp>
#include <Poseidon/Asset/Addon/VirtualPath.hpp>
#include <Poseidon/World/Terrain/LoosePaaPreparation.hpp>
#include <Poseidon/World/Terrain/PaaPreparationInflight.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>

#include <Poseidon/Asset/Formats/Material/EmatMaterialAdapter.hpp> // ReadEmatFileLoose (pure, worker-safe)
#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <Poseidon/Foundation/Threads/PoThread.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/Foundation/Threads/WatchDog.hpp>
#include <Poseidon/Foundation/Platform/FpEnvironment.hpp>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>      // ReadPAABlockChain (pure, worker-safe)
#include <Poseidon/Graphics/Textures/PreparedTextures.hpp> // the worker->main-thread mip-chain store
#include <Poseidon/Graphics/Textures/DdsImport.hpp>
#include <Poseidon/Dev/Diag/StreamingDiag.hpp>
#include <Poseidon/Asset/Formats/Enfusion/EnfusionMount.hpp>
#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/World/Model/ModelMemory.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/IO/Streams/ArchiveSourceBinding.hpp>
#include <Poseidon/World/Terrain/WarmTextureJobPolicy.hpp>
#include <Poseidon/World/Model/ModelCache.hpp>
#include <Poseidon/World/Model/ModelDerivedCache.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

namespace Poseidon
{

namespace
{
// Exact first-touch pilot, one authored Multi material and its NormalMap per
// building. Other materials, layers and proxy models retain ordinary admission.
struct BuildingRapFixture
{
    const char* model;
    const char* material;
    const char* normal;
};
constexpr std::array<BuildingRapFixture, 3> BuildingRapFixtures{{
    {R"(dz\structures\residential\tenements\tenement_small.p3d)",
     R"(dz\structures\residential\tenements\data\tb_small_flats_walls.rvmat)",
     R"(dz\structures\data\plaster\plaster_flats01_nohq.paa)"},
    {R"(dz\structures\residential\schools\city_school.p3d)",
     R"(dz\structures\residential\schools\data\school_ext_walls.rvmat)",
     R"(dz\structures\data\concrete\concrete_bare4_nohq.paa)"},
    {R"(dz\structures\residential\police\village_policestation.p3d)",
     R"(dz\structures\residential\police\data\police_station_wall_ext.rvmat)",
     R"(dz\structures\data\plaster\police_station_wall_nohq.paa)"}
}};
// Measured first-touch NormalMap uploads on tenement_small. The owner captures
// authored material members; only a completed physical raP parse may associate
// one of these keys with that model. The list itself confers no source authority.
constexpr const char* TenementMaterialPrefix = R"(dz\structures\residential\tenements\data\)";
constexpr std::array<const char*, 4> TenementMultistageNormals{{
    R"(dz\structures\data\plaster\plaster_flats02_nohq.paa)",
    R"(dz\structures\data\concrete\concrete_panels_dirty_nohq.paa)",
    R"(dz\structures\data\concrete\concrete_panels_nohq.paa)",
    R"(dz\structures\data\plaster\plaster_flats03_nohq.paa)"
}};
bool IsTenementMultistageNormal(const std::string& key)
{
    return std::find_if(TenementMultistageNormals.begin(), TenementMultistageNormals.end(),
        [&](const char* wanted) { return key == wanted; }) != TenementMultistageNormals.end();
}
int BuildingRapFixtureOrdinal(const std::string& model)
{
    for (size_t i = 0; i < BuildingRapFixtures.size(); ++i)
        if (model == BuildingRapFixtures[i].model) return static_cast<int>(i);
    return -1;
}
// Diagnostic filename overlap only: no archive identity, cache ownership or reuse verdict.
// Constructed only for the explicit optional PBO diagnostic. Fixed storage bounds metadata.
// Log saturation bounds output, not instrumentation: attempt clocks/scans continue
// while enabled, so this diagnostic is unsuitable for normal throughput acceptance.
struct PaaAttemptDiagnostics
{
    struct Slot { uint64_t generation = 0; size_t length = 0, active = 0; char key[1024]{}; };
    std::mutex mutex;
    std::array<Slot, 128> slots{};
    std::atomic<unsigned> rows{0};
    unsigned NextLogRow()
    {
        unsigned row = rows.load(std::memory_order_relaxed);
        while (row < 65 && !rows.compare_exchange_weak(row, row + 1, std::memory_order_relaxed)) {}
        return row; // saturates: never wraps into another logging burst
    }
    struct Ticket
    {
        PaaAttemptDiagnostics* owner = nullptr;
        size_t slot = 128;
        bool overlap = false, overflow = false;
        Ticket(PaaAttemptDiagnostics* diagnostic, const std::string& key, uint64_t generation) : owner(diagnostic)
        {
            if (!owner) return;
            if (key.size() >= sizeof(Slot::key)) { overflow = true; return; }
            std::lock_guard<std::mutex> lock(owner->mutex);
            size_t empty = owner->slots.size();
            for (size_t i = 0; i < owner->slots.size(); ++i)
            {
                auto& s = owner->slots[i];
                if (!s.active) { if (empty == owner->slots.size()) empty = i; continue; }
                if (s.generation == generation && s.length == key.size() &&
                    std::memcmp(s.key, key.data(), key.size()) == 0)
                { slot = i; overlap = true; ++s.active; return; }
            }
            if (empty == owner->slots.size()) { overflow = true; return; }
            slot = empty;
            auto& s = owner->slots[slot];
            s.generation = generation; s.length = key.size(); s.active = 1;
            std::memcpy(s.key, key.data(), key.size());
        }
        Ticket(const Ticket&) = delete;
        Ticket& operator=(const Ticket&) = delete;
        ~Ticket()
        {
            if (owner && slot < owner->slots.size())
            { std::lock_guard<std::mutex> lock(owner->mutex); --owner->slots[slot].active; }
        }
    };
};
struct PaaAttemptRow
{
    uint64_t preSkipped = 0, inFlightSuppressed = 0, inFlightPassThrough = 0, started = 0, overlapping = 0, untracked = 0;
    uint64_t readFailed = 0, accepted = 0, rejected = 0, cancelledBeforeRead = 0, cancelledBeforePut = 0, threw = 0;
    double readDecodeMs = 0, rejectedReadDecodeMs = 0, rejectedReadDecodeFactsMs = 0;
};

template<class Reserve, class Release>
bool PrepareNativeDds(const std::string& key, uint64_t generation, size_t& bytes,
                      bool prepareBc3, bool bc3Only, const Bc3EncodingObserver* observer,
                      const render::DdsPublicationToken& publication,
                      const render::DdsPublicationObserver& publicationObserver,
                      Reserve&& reserve, Release&& release)
{
    try
    {
        if (!publication.Valid()) return false;
        const auto& mount = Asset::Formats::Enfusion::EnfusionMount::Instance();
        const auto mountGeneration = mount.Generation();
        auto source = std::make_unique<TextureSourceDDS>();
        PacLevelMem mips[16];
        const auto options = CaptureDdsPreparationOptions();
        const bool loaded = source->InitFromReader(key.c_str(), mips, 16,
            [&](const char* name, std::vector<uint8_t>& out) {
                if (mount.Generation() != mountGeneration) return false;
                const auto request = mount.MakeReadRequest(name);
                if (!request || mount.Generation() != mountGeneration) return false;
                std::string error;
                return request->Read(out, error);
            }, options);
        if (!loaded || mount.Generation() != mountGeneration) return false;
        const size_t encodeReservation = prepareBc3 ? source->CompositeBc3Reservation() : 0;
        if (encodeReservation && reserve(encodeReservation))
        {
            const auto began = std::chrono::steady_clock::now();
            bool encoded = false;
            bool reservationActive = true;
            auto finish = [&](bool published) {
                reservationActive = false; // never subtract twice if the release callback throws
                release(encodeReservation, published,
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count());
            };
            try
            {
                encoded = source->PrepareCompositeBc3(encodeReservation, observer);
                if (bc3Only && !encoded)
                {
                    source.reset();
                    finish(false);
                    return false;
                }
                // A mount/world may have changed during this comparatively long
                // pure CPU operation. reserve(0) validates the model job only.
                if (mount.Generation() != mountGeneration || !reserve(0))
                {
                    source.reset();
                    finish(false);
                    return false;
                }
                bytes = source->PreparedByteSize();
                if (publicationObserver.beforePut) publicationObserver.beforePut(publicationObserver.context, encoded);
                const bool stored = render::PreparedTextureStore::Instance().PutDdsPrepared(
                    key, std::move(source), options, generation, &publication);
                finish(stored && encoded);
                return stored;
            }
            catch (...)
            {
                if (reservationActive)
                {
                    source.reset(); // destroy outstanding pixels before releasing their charge
                    finish(false);
                }
                throw;
            }
        }
        // Narrow mode never retains ordinary DDS when the sidecar was ineligible
        // or its optional scratch reservation was refused. Owner fallback remains.
        if (bc3Only || mount.Generation() != mountGeneration || !reserve(0)) return false;
        bytes = source->PreparedByteSize();
        if (publicationObserver.beforePut) publicationObserver.beforePut(publicationObserver.context, false);
        return render::PreparedTextureStore::Instance().PutDdsPrepared(key, std::move(source), options, generation, &publication);
    }
    catch (...)
    {
        // Optional preparation may fail; keep the parsed model and let its
        // ordinary main-thread material path report/retry. Never lose a worker.
        return false;
    }
}

// Native model conversion keeps the source .emat path in its IR. Reading that material through
// the mounted archive is safe here: EnfusionMount returns an owned request, and the parser plus
// inheritance fold are pure. This deliberately does not touch the texture bank or GPU state.
bool ReadNativeEmat(const Asset::Formats::Enfusion::EnfusionMount& mount, const std::string& path,
                    Asset::Material::EmatMaterial& out, bool& complete)
{
    complete = false;
    const auto request = mount.MakeReadRequest(path);
    if (!request)
        return false;
    std::vector<uint8_t> bytes;
    std::string error;
    if (!request->Read(bytes, error) || bytes.empty())
        return false;
    namespace Mat = Asset::Material;
    out = Mat::ParseEmat(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    if (!out.valid())
        return false;
    complete = Mat::ResolveEmatInheritance(out,
                                [&mount](const std::string& parentPath, std::string& text)
                                {
                                    const auto parent = mount.MakeReadRequest(parentPath);
                                    if (!parent)
                                        return false;
                                    std::vector<uint8_t> parentBytes;
                                    std::string parentError;
                                    if (!parent->Read(parentBytes, parentError) || parentBytes.empty())
                                        return false;
                                    text.assign(reinterpret_cast<const char*>(parentBytes.data()), parentBytes.size());
                                    return true;
                                });
    return true;
}
}

// ------------------------------------------------------------------------------------------
// Levers. All read once; the admit path logs which arm it is running.
// ------------------------------------------------------------------------------------------

bool ObjectStreamPreparer::WarmTextureJobsEnabled()
{
    static const bool enabled = [] {
        const char* sources = std::getenv("WGR_OBJECT_STREAM_WARM_TEXTURES");
        const char* jobs = std::getenv("WGR_OBJECT_STREAM_WARM_TEXTURE_JOBS");
        return sources && std::strcmp(sources, "1") == 0 && jobs && std::strcmp(jobs, "1") == 0 &&
            render::PreparedTextureStore::Enabled();
    }();
    return enabled;
}

// Exact opt-in. The ordinary model/texture publication policy is unchanged.
static bool WarmPublishedReuseEnabled()
{
    static const bool enabled = [] {
        if (!ObjectStreamPreparer::WarmTextureJobsEnabled()) return false;
        const char* value = std::getenv("WGR_OBJECT_STREAM_WARM_PUBLISHED_REUSE");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

bool ObjectStreamPreparer::AsyncEnabled()
{
    static const bool on = []
    {
        const char* v = std::getenv("WGR_OBJECT_STREAM_ASYNC");
        // Default ON. The worker touches nothing but std:: and the header-only P3D readers
        // (see LoadLooseFile and the header note); the parts that are not thread-safe were
        // left on the main thread rather than made safe. WGR_OBJECT_STREAM_ASYNC=0 is the
        // A/B and restores the synchronous admit loop exactly.
        return !(v && std::strcmp(v, "0") == 0);
    }();
    return on;
}

uint32_t ObjectStreamPreparer::WorkerCount()
{
    static const uint32_t n = []
    {
        const char* v = std::getenv("WGR_OBJECT_STREAM_ASYNC_WORKERS");
        // Two, not "one per core": the parse is memory-bandwidth-bound and shares the machine
        // with the render thread, the enkiTS TaskPool and the audio mixer. Two keeps a
        // 30 m/s camera fed on Everon (a recentre needs a few hundred distinct models over
        // ~2 s, at tens of ms each) without contending for the cores the frame is using.
        const long parsed = v ? std::strtol(v, nullptr, 10) : 2;
        return static_cast<uint32_t>(std::clamp<long>(parsed, 1, 16));
    }();
    return n;
}

size_t ObjectStreamPreparer::QueueLimit()
{
    static const size_t n = []
    {
        const char* v = std::getenv("WGR_OBJECT_STREAM_ASYNC_QUEUE");
        const long parsed = v ? std::strtol(v, nullptr, 10) : 512;
        return static_cast<size_t>(std::clamp<long>(parsed, 1, 100000));
    }();
    return n;
}

size_t ObjectStreamPreparer::ReadyLimit()
{
    static const size_t n = []
    {
        const char* v = std::getenv("WGR_OBJECT_STREAM_ASYNC_READY");
        const long parsed = v ? std::strtol(v, nullptr, 10) : 128;
        return static_cast<size_t>(std::clamp<long>(parsed, 1, 100000));
    }();
    return n;
}

// ------------------------------------------------------------------------------------------
// State shared between the main thread and the workers.
//
// LOCKING. `mutex` guards everything below except `states[]`, which are atomics so that the
// main thread's per-placement Query in the admit loop is a load and nothing else. Every
// transition of a state is still made under the mutex, so a Query that reads Ready is
// guaranteed to find the IR in `models[]` on the following Take (which locks). The parse
// itself runs with the mutex released; the critical sections are pointer moves and counter
// bumps. Freeing an IR (megabytes) is always done with the mutex released, too.
// ------------------------------------------------------------------------------------------
uint64_t ObjectStreamPreparer::ReadyByteBudget()
{
    static const uint64_t bytes = [] {
        const char* value = std::getenv("WGR_OBJECT_STREAM_READY_MB");
        const long mb = value ? std::strtol(value, nullptr, 10) : 512;
        return static_cast<uint64_t>(std::clamp<long>(mb, 0, 16384)) * 1024 * 1024;
    }();
    return bytes;
}

struct ObjectStreamPreparer::Impl
{
    using State = ObjectStreamPreparer::State;
    using ModelPtr = std::shared_ptr<Model::Model>;

    std::unique_ptr<Streaming::PaaPreparationInflight> paaInflight; // experimental OFF: no allocation
    std::atomic<bool> paaInflightProofLogged{false};
    std::unique_ptr<PaaAttemptDiagnostics> paaAttemptDiagnostics; // diagnostic OFF adds no tracker allocation
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::thread> workers;
    bool stop = false;
    Bc3EncodingObserver encodingObserver; // immutable copy, set before workers start
    render::DdsPublicationObserver publicationObserver;
    std::shared_ptr<render::DdsPublicationInventory> ddsPublication;
    // Bumped by Reset. A worker that finishes a parse started under an older generation
    // discards its result: the slot it would write belongs to a different world now.
    // Generation tokens distinguish workers from an earlier world after Reset;
    // all writes and cancellation checks also hold the queue mutex.
    std::atomic<uint64_t> generation{0};

    size_t count = 0;
    std::vector<std::string> paths;
    std::vector<uint8_t> prepareCompositeBc3; // written only by owner Request; workers capture before unlocking
    struct RadiusSlot
    {
        float radius = 0.0f;
        RadiusProvenance provenance = RadiusProvenance::None;
    };
    std::vector<RadiusSlot> radiusCertificates; // no retained IR, shape, or duplicate path strings
    struct SourceEnvelopeSlot
    {
        bool requested = false;
        bool attempted = false;
        uint64_t diagnosticParseToken = 0;
        SourceConversionToken convertedToken;
        Streaming::StaticSourceEnvelope envelope;
        Streaming::StaticPlainSourceSummary plainSource;
    };
    std::vector<SourceEnvelopeSlot> sourceEnvelopes; // lazy, bounded, no IR/config pointers
    uint64_t nextDiagnosticParseToken = 0; // owner only; never advanced by ordinary Request
    // Caller holds queue mutex. No new identity scans or allocation; parse tokens
    // are unique across model indices and survive Reset sequence advancement.
    bool SourceBindingMatches(uint32_t index, SourceConversionToken token) const
    {
        return token.generation != 0 && token.diagnosticParseToken != 0 &&
            token.generation == generation.load(std::memory_order_relaxed) && index < sourceEnvelopes.size() &&
            sourceEnvelopes[index].attempted &&
            sourceEnvelopes[index].diagnosticParseToken == token.diagnosticParseToken;
    }

    // Caller holds mutex. RadiusSlot belongs to this generation's inventory;
    // Reset destroys/reinitializes the slots, while payload retirement leaves them alone.
    bool PublishRadius(uint32_t index, uint64_t gen, const std::string& identity,
                       float radius, RadiusProvenance provenance)
    {
        if (gen != generation.load(std::memory_order_relaxed) || index >= count || paths[index] != identity)
            return false;
        if (!std::isfinite(radius) || radius < 0.0f)
        {
            radiusCertificates[index] = {}; // a current failed measurement invalidates old evidence
            return false;
        }
        // An already installed/adapted bound outranks a later speculative parse.
        if (provenance == RadiusProvenance::ParsedODOL &&
            radiusCertificates[index].provenance == RadiusProvenance::AdaptedShape)
            return true;
        radiusCertificates[index] = {radius, provenance};
        return true;
    }
    std::vector<std::optional<BankReadRequest>> packedReads;
    // Owner-only, reset per world: one exact capture attempt per selected fixture.
    uint8_t rapFixtureAttempts = 0;
    uint8_t rapBuildingAttempts = 0;
    std::unique_ptr<std::atomic<uint8_t>[]> states;
    std::vector<ModelPtr> models;
    // Protected by mutex. In-flight slots stay occupied until their worker exits.
    std::vector<bool> stale;
    std::deque<uint32_t> queue;
    // Stage 3: conversion jobs and their results. `convertedShapes[i]` is owned here
    // between the worker finishing and TakeConverted; Reset/DropStale delete stale
    // ones OUTSIDE the mutex (a fresh shape holds no vertex buffers, so its
    // destructor touches only refcounted textures and the locked fast allocator).
    struct PackedTextureRead
    {
        std::string key;
        BankReadRequest read;
        std::shared_ptr<const ArchiveSourceBinding> initializedSource; // null on original cold jobs
        const BankReadRequest* initializedRead = nullptr; // borrowed ONLY from owned immutable binding
        bool physicalWitness = false; // exact building pilot; read owns the leased member
        const BankReadRequest& ReadRequest() const { return initializedRead ? *initializedRead : read; }
    };
    static constexpr size_t PackedTextureLimit = 16 * 1024 * 1024;
    static constexpr size_t RapMaterialLimit = 256 * 1024;
    static constexpr size_t RapCandidateLimit = RapStageCandidateHandoff::MaxKnownBytes;
    static constexpr size_t ProxyModelLimit = DayzProxyPrefetchBounds::MaxProxyModels;
    static constexpr size_t ProxyModelMemberLimit = DayzProxyPrefetchBounds::MaxProxyMember;
    static constexpr size_t ProxyRapLimit = 16;
    static constexpr size_t ProxyPaaLimit = 32;
    struct CapturedRapMaterial
    {
        std::string key;
        BankReadRequest read; // exact immutable physical member; never a QFBank pointer
    };
    struct CapturedProxyModel
    {
        std::string key;
        BankReadRequest read;
    };
    struct ProxyMaterialCandidate
    {
        std::string proxyKey, materialKey;
        BankReadMemberIdentity proxyMember;
    };
    struct ConvertJob
    {
        std::vector<PackedTextureRead> textures;
        std::vector<CapturedRapMaterial> rapMaterials; // typed NormalMap owner handoff, no PAA work
        std::vector<CapturedProxyModel> proxyModels; // leased P3D sources; worker scans one at a time
        uint8_t proxyPhase = 0; // 0=P3D discovery, 1=proxy raP discovery, 2=ordinary conversion
        bool proxyCaptureIncomplete = false;
        uint64_t materialStageSourceBytes = 0; // parent and proxy PAA source cap is shared
        int8_t buildingRapFixture = -1; // worker discovery before conversion, exact pilot only
        bool tenementMultistage = false; // separate default-off measured first-touch slice
        bool broadParentPaa = false; // exact-tenement authored PAA worker preparation
        bool broadCaptureIncomplete = false;

        std::unique_ptr<render::ColdPaaRead> coldTexture;
        uint64_t textureGeneration = 0;
        render::DdsPublicationToken texturePublication;
        bool texturePublicationRequired = false;
        uint32_t index = 0;
        bool radiusIdentityAccepted = false;
        uint64_t radiusGeneration = 0;
        std::string radiusIdentity; // copied owner token, never a borrowed model/shape
        SourceConversionToken sourceToken; // bound to THIS converted model job only
        uint64_t reservation = 0;
        ModelPtr model;
        std::shared_ptr<const Model::ShapeAdapter::AdapterBankTables> tables;
    };
    std::deque<ConvertJob> convertQueue;
    struct PendingRap
    {
        uint32_t index = uint32_t(-1);
        std::optional<ConvertJob> job;
        std::unique_ptr<RapStageCandidateHandoff> handoff;
        std::vector<ProxyMaterialCandidate> proxyMaterials; // first owner pass only
    };
    std::array<PendingRap, BuildingRapFixtures.size()> pendingRap{};
    struct WarmJob
    {
        uint32_t index = 0;
        uint64_t generation = 0, textureGeneration = 0, metadataBytes = 0;
        render::DdsPublicationToken publication;
        std::vector<WarmTextureRead> reads;
        size_t next = 0, slot = 0;
    };
    struct WarmSlot { bool occupied = false; uint32_t index = 0; uint64_t generation = 0, publicationEpoch = 0; };
    struct WarmState
    {
        std::vector<WarmJob> queue;
        std::array<WarmSlot, Streaming::WarmTextureJobPolicy::JobLimit> slots{};
        size_t captureCursor = 0; // owner-only ordinal, no persistent shape reference
        Streaming::WarmTextureJobPolicy::FirstMemberFairness earlyFirst;
        WarmState() : earlyFirst([] {
            const char* flag = std::getenv("WGR_OBJECT_STREAM_WARM_EARLY_SLICE");
            return flag && std::strcmp(flag, "1") == 0;
        }())
        {
            queue.reserve(Streaming::WarmTextureJobPolicy::JobLimit);
            if (queue.capacity() > Streaming::WarmTextureJobPolicy::JobLimit)
                throw std::bad_alloc(); // refuse optional storage outside its explicit cap
        }
    };
    std::unique_ptr<WarmState> warm; // no queue/slot heap allocation until BOTH opt-in flags

    uint64_t WarmStorageBytes() const
    { return warm ? sizeof(WarmState) + warm->queue.capacity() * sizeof(WarmJob) : 0; }
    bool HasWarmByteRoom() const
    {
        return Streaming::WarmTextureJobPolicy::ActiveRoom(stats.warmTextureActive) &&
            Streaming::WarmTextureJobPolicy::ScratchFits(stats.readyByteBudget,
            stats.readyPayloadBytes, stats.parseReservedBytes, stats.conversionReservedBytes,
            stats.textureEncodeReservedBytes, stats.warmTextureScratchBytes, stats.warmTextureMetadataBytes + WarmStorageBytes());
    }
    // Called only under mutex after convertQueue has had priority. The opt-in
    // permits one first member ahead of a runnable cold parse; later members
    // and all other warm jobs retain the existing cold-first ordering until
    // an actual cold parse completes.
    bool ShouldRunWarm(bool& earlySelected)
    {
        earlySelected = false;
        if (!warm || warm->queue.empty() || !HasWarmByteRoom()) return false;
        const bool coldRunnable = !queue.empty() && stats.ready < ObjectStreamPreparer::ReadyLimit() && HasParseByteRoom();
        if (!coldRunnable) return true; // retain the existing warm-on-cold-blocked path
        if (!warm->earlyFirst.enabled) return false; // no extra headroom work with the candidate OFF
        if (!Streaming::WarmTextureJobPolicy::ScratchAndNextParseFit(stats.readyByteBudget,
            stats.readyPayloadBytes, stats.parseReservedBytes, ParseReservation(),
            stats.conversionReservedBytes, stats.textureEncodeReservedBytes,
            stats.warmTextureScratchBytes, stats.warmTextureMetadataBytes + WarmStorageBytes()))
            return false;
        earlySelected = warm->earlyFirst.TryStart(true, warm->queue.front().next,
            stats.warmTextureActive);
        return earlySelected;
    }
    bool PreparePackedTexture(const PackedTextureRead& texture, uint64_t jobGen, uint32_t index,
        uint64_t textureGeneration, const render::DdsPublicationToken& publication,
        bool publicationRequired, bool boundedParentPaa, PaaAttemptRow& attemptRow,
        uint64_t& prepared, uint64_t& preparedBytes, uint64_t& skipped, uint64_t& unreadable);

    // Empty/default-off. Allocated only with a valid optional publication inventory.
    std::vector<std::unique_ptr<render::ColdPaaOwned>> convertedColdTextures;
    std::vector<LODShapeWithShadow*> convertedShapes;
    std::vector<ModelPtr> convertedModels;
    // Two selected material fixtures per world. Empty slots allocate nothing when off.
    struct RapReady { uint32_t index = uint32_t(-1); std::unique_ptr<RapStageCandidateHandoff> payload; };
    std::array<RapReady, 2> convertedRapStages{};
    std::vector<uint64_t> conversionBytes;
    Stats stats;
    std::vector<uint64_t> payloadBytes;
    uint64_t ParseReservation() const
    {
        uint64_t estimate = 16ull * 1024 * 1024;
        if (!queue.empty() && queue.front() < packedReads.size() && packedReads[queue.front()])
            estimate += packedReads[queue.front()]->bytes; // owned source buffer overlaps parsing
        return stats.readyByteBudget ? std::min(estimate, stats.readyByteBudget) : estimate;
    }
    bool HasParseByteRoom() const
    {
        if (!stats.readyByteBudget) return true;
        uint64_t remaining = stats.readyByteBudget;
        for (const uint64_t charge : {stats.readyPayloadBytes, stats.parseReservedBytes,
                                     stats.conversionReservedBytes, stats.textureEncodeReservedBytes, stats.warmTextureScratchBytes, stats.warmTextureMetadataBytes, WarmStorageBytes()})
        {
            if (charge > remaining) return false;
            remaining -= charge;
        }
        return ParseReservation() <= remaining;
    }

    // Per model index: when Request() enqueued it, for the request->ready / request->taken
    // latency figures. Written under the mutex; a DropStale->re-Request simply restamps.
    std::vector<uint64_t> requestedAtUs;

    static uint64_t NowUs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    }

    State Get(uint32_t index) const
    {
        return static_cast<State>(states[index].load(std::memory_order_acquire));
    }
    void Set(uint32_t index, State s) { states[index].store(static_cast<uint8_t>(s), std::memory_order_release); }

    void WorkerMain(uint32_t workerIndex);
};


bool ObjectStreamPreparer::Impl::PreparePackedTexture(const PackedTextureRead& texture,
    uint64_t jobGen, uint32_t index, uint64_t textureGeneration,
    const render::DdsPublicationToken& publication, bool publicationRequired,
    bool boundedParentPaa,
    PaaAttemptRow& attemptRow, uint64_t& prepared, uint64_t& preparedBytes,
    uint64_t& skipped, uint64_t& unreadable)
{
    auto& store = render::PreparedTextureStore::Instance();
    using Proof = Streaming::WarmTextureProvenance;
    struct TraceExit
    {
        Proof* proof = nullptr; uint32_t token = 0;
        Proof::Event outcome = Proof::Event::WorkerOutcomeUnknown;
        ~TraceExit() { if (proof) Proof::Emit(proof->Stamp(token, outcome)); }
    } trace;
    if (texture.initializedSource)
    {
        trace.proof = Proof::Active();
        trace.token = Proof::Match(texture.key, texture.initializedSource);
    }

    {
        std::lock_guard<std::mutex> pendingLock(mutex);
        if (stop || jobGen != generation.load(std::memory_order_relaxed) || index >= count || stale[index] ||
            (publicationRequired && !publication.Valid())) { trace.outcome = Proof::Event::WorkerCancelled; if (paaAttemptDiagnostics) ++attemptRow.cancelledBeforeRead; return false; }
    }
    if (!((texture.initializedSource || texture.physicalWitness) ?
            store.ShouldPrepareWarm(texture.key) : store.ShouldPrepare(texture.key)))
    { trace.outcome = Proof::Event::WorkerSkipped; ++skipped; if (paaAttemptDiagnostics) ++attemptRow.preSkipped; return true; }
    Streaming::PaaPreparationInflight::Result inflight{Streaming::PaaPreparationInflight::Outcome::UnleasedOrInvalid, {}};
    if (paaInflight && publicationRequired)
    {
        inflight = paaInflight->Acquire(texture.key, textureGeneration, texture.ReadRequest(), publication);
        if (inflight.outcome == Streaming::PaaPreparationInflight::Outcome::Suppressed)
        {
            trace.outcome = Proof::Event::WorkerSuppressed;
            if (paaAttemptDiagnostics) ++attemptRow.inFlightSuppressed;
            if (!paaInflightProofLogged.exchange(true, std::memory_order_relaxed))
            {
                const auto counters = paaInflight->Snapshot();
                LOG_INFO(World, "PAA prep inflight: actual exact-member suppression observed; suppressed={} active={} metadataBytes={} peakMetadataBytes={} overflow={} unsupported={} contended={} owner fallback retained",
                    counters.suppressed, counters.active, counters.metadataBytes, counters.peakMetadataBytes,
                    counters.overflow, counters.unsupported, counters.contended);
            }
            return true; // no peer payload/Ready promise: ordinary owner reader is unchanged
        }
        if (paaAttemptDiagnostics && inflight.outcome != Streaming::PaaPreparationInflight::Outcome::Tracked)
            ++attemptRow.inFlightPassThrough;
    }
    PaaAttemptDiagnostics::Ticket attempt(paaAttemptDiagnostics.get(), texture.key, textureGeneration);
    if (paaAttemptDiagnostics) { ++attemptRow.started; attemptRow.overlapping += attempt.overlap; attemptRow.untracked += attempt.overflow; }
    const auto readBegan = paaAttemptDiagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    try
    {
        PAABlockChain chain;
        {
            std::vector<char> source;
            if (!texture.ReadRequest().Read(source) || source.capacity() > 2 * PackedTextureLimit ||
                !ReadPAABlockChainBuffer(source.data(), source.size(),
                    chain, nullptr, PackedTextureLimit)) { trace.outcome = Proof::Event::WorkerReadFailed; ++unreadable; if (paaAttemptDiagnostics) ++attemptRow.readFailed; return true; }
        }
        const double readDecodeMs = paaAttemptDiagnostics ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readBegan).count() : 0;
        if (paaAttemptDiagnostics) attemptRow.readDecodeMs += readDecodeMs;
        const size_t bytes = chain.blocks.size();
        if (boundedParentPaa && (preparedBytes > 128ull * 1024 * 1024 ||
            bytes > 128ull * 1024 * 1024 - preparedBytes))
        { trace.outcome = Proof::Event::WorkerPutRefused; ++skipped; return true; }
        // Source storage has already been released. The <=16 MiB
        // chain and compact alpha plane fit the existing 64 MiB
        // scratch reservation, including conservative growth.
        auto cancelled = [&] {
            std::lock_guard<std::mutex> pendingLock(mutex);
            return stop || jobGen != generation.load(std::memory_order_relaxed) || index >= count || stale[index] ||
                (publicationRequired && !publication.Valid());
        };
        std::unique_ptr<render::PreparedAlphaFacts> facts;
        try {
            if (store.CanRetainAlphaFacts(texture.key, chain, texture.ReadRequest(), textureGeneration,
                    publicationRequired ? &publication : nullptr))
                facts = render::PreparedTextureStore::BuildAlphaFacts(chain, texture.ReadRequest(), cancelled);
        }
        catch (...) { } // optional analysis failure retains the original upload chain
        if (cancelled()) { trace.outcome = Proof::Event::WorkerCancelled; store.NoteAlphaFactsCancelled(); if (paaAttemptDiagnostics) ++attemptRow.cancelledBeforePut; return false; }
        // CPU read/decode/facts subset excludes the observer latch and Put.
        const double readDecodeFactsMs = paaAttemptDiagnostics ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readBegan).count() : 0;
        if (publicationObserver.beforePaaPut)
            publicationObserver.beforePaaPut(publicationObserver.context, bool(facts));
        if (store.Put(texture.key, std::move(chain), textureGeneration, std::move(facts),
                publicationRequired ? &publication : nullptr, texture.initializedSource,
                texture.physicalWitness ? &texture.read : nullptr))
        { trace.outcome = Proof::Event::WorkerPut; ++prepared; preparedBytes += bytes; if (paaAttemptDiagnostics) ++attemptRow.accepted; }
        else { trace.outcome = Proof::Event::WorkerPutRefused; ++skipped; if (paaAttemptDiagnostics) { ++attemptRow.rejected; attemptRow.rejectedReadDecodeMs += readDecodeMs; attemptRow.rejectedReadDecodeFactsMs += readDecodeFactsMs; } }
    }
    catch (...) { trace.outcome = Proof::Event::WorkerReadFailed; ++unreadable; if (paaAttemptDiagnostics) ++attemptRow.threw; } // ordinary owner-thread decode remains the fallback
    return true;
}

void ObjectStreamPreparer::Impl::WorkerMain(uint32_t workerIndex)
{
    // Below-normal priority: this thread must never win a core from the render thread.
    //
    // This was a #ifdef _WIN32 direct SetThreadPriority call, working around the Foundation
    // helper passing a thread ID where a HANDLE is needed. The helper is fixed (PoThread.cpp,
    // 2026-08-31), so the workaround is gone -- keeping it would have left the underlying bug
    // in place for every other caller while looking like this one was fine.
    Foundation::poSetMyPriority(-1);

    // Roadmap 8.1. This thread runs full ShapeAdapter geometry conversion whose output is
    // installed verbatim on the main thread, so it must compute in the main thread's
    // floating-point environment -- a new thread inherits the platform default MXCSR, not
    // ours, and with ENGINE_CONFIG.enablePIII the two differ in flush-to-zero. Workers are
    // created long after InitFPU, so the capture is always live by the time we get here.
    Foundation::ApplyMainFpEnvironment();

    (void)workerIndex;

    std::unique_lock<std::mutex> lock(mutex);
    for (;;)
    {
        // BACK-PRESSURE, NOT DROPPING. The queue is in distance order and the main thread
        // consumes Ready IRs in the same order, so if the workers run more than ReadyLimit
        // ahead, the right thing is to pause here until Take() makes room -- not to discard
        // the oldest Ready (the nearest, most-needed models) or the newest (parsed for
        // nothing). Take() and Reset() notify.
        // Count the park BEFORE waiting, and only when the ready bound is the thing doing
        // the blocking: queue work exists, no conversion to run instead, and the ready set
        // is at its limit. An empty queue is "nothing to do", not back-pressure, and must
        // not be counted as the bound biting.
        if (!stop && convertQueue.empty() && !queue.empty() && stats.ready >= ObjectStreamPreparer::ReadyLimit())
            ++stats.readyFullParks;
        if (!stop && convertQueue.empty() && !queue.empty() && !HasParseByteRoom())
            ++stats.readyByteParks;
        cv.wait(lock, [&] {
            return stop || !convertQueue.empty() || (warm && !warm->queue.empty() && HasWarmByteRoom()) ||
                   (!queue.empty() && stats.ready < ObjectStreamPreparer::ReadyLimit() && HasParseByteRoom());
        });
        if (stop)
            return;
        // Conversions first: each one unblocks an install the admit loop is waiting
        // on, and the set is bounded by what the main thread submitted.
        if (!convertQueue.empty())
        {
            ConvertJob job = std::move(convertQueue.front());
            convertQueue.pop_front();
            // Only active workers allocate source/decode scratch. Queued and ready
            // jobs keep their model charge, not dozens of nonexistent 64 MiB buffers.
            uint64_t alphaMetadataScratch = 0;
            for (const auto& texture : job.textures)
                if (texture.read.archiveLease)
                    alphaMetadataScratch = std::max<uint64_t>(alphaMetadataScratch,
                        sizeof(render::PreparedAlphaFacts) + texture.read.archive.capacity() + 1 + 64);
            const uint64_t rapScratch = job.rapMaterials.empty() ? 0 :
                RapMaterialLimit + job.rapMaterials.size() * 16 * 240 + 8192;
            // No P3D/ODOL model parse is allowed on this speculative path:
            // compressed arrays can amplify a small physical member before an
            // IR-size check. The byte scanner retains only short candidate keys.
            const uint64_t proxyScratch = job.proxyModels.empty() ? 0 :
                2 * ProxyModelMemberLimit + 8192;
            const uint64_t textureScratch = ((job.textures.empty() && !job.coldTexture) ? 0 :
                4 * PackedTextureLimit + 8192 + alphaMetadataScratch) + rapScratch + proxyScratch;
            stats.conversionReservedBytes += textureScratch;
            stats.peakConversionReservedBytes = std::max(stats.peakConversionReservedBytes, stats.conversionReservedBytes);
            const uint64_t jobGen = generation.load(std::memory_order_relaxed);
            const std::string conversionPath = paths[job.index];
            lock.unlock();

            // Exact building pilot: raP parsing stays on the worker. A valid
            // NormalMap name needs one owner round-trip for the CURRENT PAA
            // binding before conversion resumes. Failure simply converts now.
            if (job.buildingRapFixture >= 0)
            {
                std::unique_ptr<RapStageCandidateHandoff> candidate;
                std::vector<ProxyMaterialCandidate> proxyDiscovered;
                lock.lock();
                const bool mayRead = !stop && jobGen == generation.load(std::memory_order_relaxed) &&
                    job.index < count && !stale[job.index] && paths[job.index] == conversionPath &&
                    job.texturePublication.Valid();
                lock.unlock();
                try
                {
                    const auto& fixture = BuildingRapFixtures[static_cast<size_t>(job.buildingRapFixture)];
                    if (mayRead && job.proxyPhase == 0 && !job.proxyModels.empty())
                    {
                        // The lexical scan cannot prove full IR coverage, even
                        // when every physical member was captured successfully.
                        job.proxyCaptureIncomplete = true;
                        for (const auto& selected : job.proxyModels)
                        {
                            {
                                std::lock_guard<std::mutex> guard(mutex);
                                if (stop || jobGen != generation.load(std::memory_order_relaxed) ||
                                    job.index >= count || stale[job.index] ||
                                    paths[job.index] != conversionPath || !job.texturePublication.Valid())
                                    break;
                            }
                            BankReadMemberIdentity member;
                            std::vector<char> bytes;
                            if (!selected.read.CopyMemberIdentity(member) ||
                                selected.read.bytes > ProxyModelMemberLimit ||
                                !selected.read.Read(bytes) || bytes.size() != selected.read.bytes ||
                                bytes.capacity() > 2 * ProxyModelMemberLimit)
                            { job.proxyCaptureIncomplete = true; continue; }
                            const auto scan = Streaming::ScanProxyMaterialNames(
                                std::span<const char>(bytes.data(), bytes.size()));
                            std::vector<char>().swap(bytes);
                            if (!scan.supportedSignature || scan.overflow)
                                job.proxyCaptureIncomplete = true;
                            for (const auto& key : scan.names)
                            {
                                if (std::any_of(proxyDiscovered.begin(), proxyDiscovered.end(),
                                    [&](const auto& prior) { return prior.materialKey == key; })) continue;
                                if (proxyDiscovered.size() >= ProxyRapLimit)
                                { job.proxyCaptureIncomplete = true; break; }
                                proxyDiscovered.push_back({selected.key, key, member});
                            }
                            if (proxyDiscovered.size() >= ProxyRapLimit) break;
                        }
                    }
                    if (mayRead && job.proxyPhase == 1 && job.broadParentPaa &&
                        job.buildingRapFixture == 0 && job.rapMaterials.size() <= ProxyRapLimit)
                    {
                        candidate = std::make_unique<RapStageCandidateHandoff>();
                        candidate->generation = jobGen;
                        candidate->modelIndex = job.index;
                        candidate->broadParentPaa = true;
                        candidate->proxyPaa = true;
                        candidate->overflow = job.proxyCaptureIncomplete;
                        for (const auto& selected : job.rapMaterials)
                        {
                            BankReadMemberIdentity member;
                            std::vector<char> bytes;
                            if (!selected.read.CopyMemberIdentity(member) ||
                                selected.read.bytes > RapMaterialLimit ||
                                !selected.read.Read(bytes) || bytes.size() != selected.read.bytes)
                            { candidate->overflow = true; continue; }
                            const auto result = Streaming::ExtractOwnedRapStageNames(
                                std::span<const char>(bytes.data(), bytes.size()), selected.key);
                            if (result.status != Streaming::RapStageNamesStatus::Complete)
                            { candidate->overflow = true; continue; }
                            for (const auto& name : result.names)
                            {
                                if (std::any_of(candidate->materials.begin(), candidate->materials.end(),
                                    [&](const auto& prior) { return prior.stageName == name; })) continue;
                                if (std::count_if(candidate->materials.begin(), candidate->materials.end(),
                                    [&](const auto& prior) { return prior.key == selected.key; }) >=
                                    RapStageCandidateHandoff::MaxStagesPerMaterial)
                                { candidate->overflow = true; break; }
                                if (candidate->materials.size() >= ProxyPaaLimit)
                                { candidate->overflow = true; break; }
                                candidate->materials.push_back({selected.key, name,
                                    RapStageCandidateHandoff::Consumer::AuthoredStage, member});
                            }
                        }
                        if (candidate->materials.empty() || !candidate->Bounded()) candidate.reset();
                    }
                    else if (mayRead && job.proxyPhase == 0 && job.broadParentPaa &&
                        job.buildingRapFixture == 0 && job.rapMaterials.size() <= 16)
                    {
                        candidate = std::make_unique<RapStageCandidateHandoff>();
                        candidate->generation = jobGen;
                        candidate->modelIndex = job.index;
                        candidate->broadParentPaa = true;
                        candidate->overflow = job.broadCaptureIncomplete;
                        for (const auto& selected : job.rapMaterials)
                        {
                            BankReadMemberIdentity member;
                            std::vector<char> bytes;
                            if (!selected.read.CopyMemberIdentity(member) ||
                                selected.read.bytes > RapMaterialLimit ||
                                !selected.read.Read(bytes) || bytes.size() != selected.read.bytes)
                            { candidate->overflow = true; continue; }
                            const auto result = Streaming::ExtractOwnedRapStageNames(
                                std::span<const char>(bytes.data(), bytes.size()), selected.key);
                            if (result.status != Streaming::RapStageNamesStatus::Complete)
                            { candidate->overflow = true; continue; }
                            for (const auto& name : result.names)
                            {
                                if (std::any_of(candidate->materials.begin(), candidate->materials.end(),
                                    [&](const auto& prior) { return prior.stageName == name; })) continue;
                                if (candidate->materials.size() >= RapStageCandidateHandoff::MaxCandidates)
                                { candidate->overflow = true; break; }
                                candidate->materials.push_back({selected.key, name,
                                    RapStageCandidateHandoff::Consumer::AuthoredStage, member});
                            }
                        }
                        if (candidate->materials.empty() || !candidate->Bounded()) candidate.reset();
                    }
                    else if (mayRead && job.tenementMultistage &&
                        job.buildingRapFixture == 0 && job.rapMaterials.size() <= 16)
                    {
                        for (const auto& selected : job.rapMaterials)
                        {
                            if (candidate && candidate->materials.size() >=
                                TenementMultistageNormals.size()) break;
                            BankReadMemberIdentity member;
                            std::vector<char> bytes;
                            if (!selected.read.CopyMemberIdentity(member) ||
                                selected.read.bytes > RapMaterialLimit ||
                                !selected.read.Read(bytes) || bytes.size() != selected.read.bytes)
                                continue;
                            const auto result = Streaming::ExtractOwnedRapStageNames(
                                std::span<const char>(bytes.data(), bytes.size()), selected.key);
                            if (result.status != Streaming::RapStageNamesStatus::Complete) continue;
                            for (const auto& stage : result.normalStages)
                            {
                                if (!IsTenementMultistageNormal(stage.name)) continue;
                                if (!candidate)
                                {
                                    candidate = std::make_unique<RapStageCandidateHandoff>();
                                    candidate->generation = jobGen; candidate->modelIndex = job.index;
                                }
                                if (std::none_of(candidate->materials.begin(), candidate->materials.end(),
                                    [&](const auto& prior) { return prior.stageName == stage.name; }))
                                    candidate->materials.push_back({selected.key, stage.name,
                                        static_cast<RapStageCandidateHandoff::Consumer>(stage.layer), member});
                                if (candidate->materials.size() >= TenementMultistageNormals.size()) break;
                            }
                        }
                        if (candidate && !candidate->Bounded()) candidate.reset();
                    }
                    else if (mayRead && !job.tenementMultistage &&
                        job.rapMaterials.size() == 1 && job.rapMaterials[0].key == fixture.material)
                    {
                        const auto& selected = job.rapMaterials[0];
                        BankReadMemberIdentity member;
                        std::vector<char> bytes;
                        if (selected.read.CopyMemberIdentity(member) &&
                            selected.read.bytes <= RapMaterialLimit && selected.read.Read(bytes) &&
                            bytes.size() == selected.read.bytes)
                        {
                            const auto result = Streaming::ExtractOwnedRapStageNames(
                                std::span<const char>(bytes.data(), bytes.size()), selected.key);
                            if (result.status == Streaming::RapStageNamesStatus::Complete &&
                                result.normalMapName == fixture.normal)
                            {
                                candidate = std::make_unique<RapStageCandidateHandoff>();
                                candidate->generation = jobGen; candidate->modelIndex = job.index;
                                candidate->materials.push_back({selected.key, result.normalMapName,
                                    RapStageCandidateHandoff::Consumer::NormalMap, member});
                                if (!candidate->Bounded()) candidate.reset();
                            }
                        }
                    }
                }
                catch (...) { candidate.reset(); job.proxyCaptureIncomplete = true; }
                const size_t slot = static_cast<size_t>(job.buildingRapFixture);
                const bool multistage = job.tenementMultistage;
                const bool broadParent = job.broadParentPaa;
                const size_t materialCount = job.rapMaterials.size();
                const bool hadProxyModels = !job.proxyModels.empty();
                job.buildingRapFixture = -1;
                job.rapMaterials.clear(); // release physical archive lease outside queue mutex
                job.proxyModels.clear(); // release P3D leases after bounded byte scan
                static std::atomic<unsigned> discoveryRows{0};
                static std::atomic<bool> multistageDiscoveryLogged{false};
                const bool logDiscovery = broadParent || multistage ?
                    !multistageDiscoveryLogged.exchange(true, std::memory_order_relaxed) :
                    discoveryRows.fetch_add(1, std::memory_order_relaxed) < BuildingRapFixtures.size();
                if (logDiscovery)
                    LOG_INFO(World, "RVMAT raP building discovery: modelIndex={} model={} material={} stage={} candidates={} multistage={} (worker parse; no owner PAA binding yet)",
                        job.index, conversionPath,
                        broadParent ? "parent-IR-physical-raP" :
                            (multistage ? "authored-tenement-materials" : BuildingRapFixtures[slot].material),
                        broadParent ? "authored-file-backed-PAA" :
                            (multistage ? "trace-top-four-normals" : BuildingRapFixtures[slot].normal),
                        candidate ? candidate->materials.size() : 0,
                        multistage);
                if (job.proxyPhase == 0 && hadProxyModels)
                    LOG_INFO(World, "DayZ proxy prefetch: modelIndex={} proxyMaterialCandidates={} captureIncomplete={} sourceReady={} phase=proxy-P3D speculative=true",
                        job.index, proxyDiscovered.size(), job.proxyCaptureIncomplete, !proxyDiscovered.empty());
                if (job.proxyPhase == 1)
                    LOG_INFO(World, "DayZ proxy prefetch: modelIndex={} proxyRaP={} stageCandidates={} candidateOverflow={} sourceReady={} phase=proxy-raP speculative=true",
                        job.index, materialCount, candidate ? candidate->materials.size() : 0,
                        candidate ? candidate->overflow : true, candidate != nullptr);
                if (broadParent && job.proxyPhase == 0)
                    LOG_INFO(World, "DayZ physical prefetch: modelIndex={} parentMaterials={} stageCandidates={} candidateOverflow={} sourceReady={} (worker parent raP names; proxy coverage separate)",
                        job.index, materialCount, candidate ? candidate->materials.size() : 0,
                        candidate ? candidate->overflow : true, candidate != nullptr);
                lock.lock();
                const bool current = !stop && jobGen == generation.load(std::memory_order_relaxed) &&
                    job.index < count && !stale[job.index] && paths[job.index] == conversionPath &&
                    (!job.sourceToken.diagnosticParseToken ||
                     SourceBindingMatches(job.index, job.sourceToken));
                if (!current)
                {
                    stats.conversionReservedBytes -= job.reservation + textureScratch;
                    if (jobGen == generation.load(std::memory_order_relaxed) && job.index < count)
                        Set(job.index, State::Unknown);
                    ++stats.dropped;
                    cv.notify_all();
                    lock.unlock();
                    job.textures.clear(); job.texturePublication = {};
                    job.model.reset(); job.tables.reset(); job.coldTexture.reset();
                    lock.lock();
                    continue;
                }
                uint64_t proxyCharge = proxyDiscovered.capacity() * sizeof(ProxyMaterialCandidate);
                for (const auto& source : proxyDiscovered)
                    proxyCharge += source.proxyKey.capacity() + 1 + source.materialKey.capacity() + 1;
                if (proxyCharge > 64 * 1024)
                { std::vector<ProxyMaterialCandidate>().swap(proxyDiscovered);
                  proxyCharge = 0; job.proxyCaptureIncomplete = true; }
                if ((candidate || !proxyDiscovered.empty()) && job.radiusIdentityAccepted &&
                    job.texturePublication.Valid() && !pendingRap[slot].job)
                {
                    job.reservation += proxyCharge;
                    stats.conversionReservedBytes += proxyCharge;
                    stats.conversionReservedBytes -= textureScratch; // no worker scratch while parked
                    pendingRap[slot].index = job.index;
                    pendingRap[slot].job.emplace(std::move(job));
                    pendingRap[slot].handoff = std::move(candidate);
                    pendingRap[slot].proxyMaterials = std::move(proxyDiscovered);
                    Set(pendingRap[slot].index, State::RapCapture);
                    cv.notify_all();
                    continue;
                }
                lock.unlock();
            }

            Poseidon::WatchDogItem watch = Poseidon::WatchScopeFor("objconvert", conversionPath);
            const auto convStart = std::chrono::steady_clock::now();
            LODShapeWithShadow* shape = nullptr;
            try
            {
                shape = Model::ShapeAdapter::convertToLODShape(*job.model, false, job.tables.get(),
                                                               /*finishTail=*/false);
            }
            catch (...)
            {
                shape = nullptr; // main thread will repeat synchronously via Failed-like fallback
            }
            const double convMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - convStart).count();
            const auto conversionCompleted = paaAttemptDiagnostics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            if (shape)
            {
                // ODOL's deferred tail never changes this scalar; MLOD already
                // ran its ordinary all-LOD/frame bounding-sphere computation.
                // Publish before optional texture work, without another vertex scan.
                std::lock_guard<std::mutex> radiusLock(mutex);
                if (jobGen == generation.load(std::memory_order_relaxed) &&
                    job.index < count && !stale[job.index] && paths[job.index] == conversionPath &&
                    job.radiusIdentityAccepted && job.radiusGeneration == jobGen &&
                    (job.model->sourceFormat == "ODOL" || job.model->sourceFormat == "MLOD"))
                    PublishRadius(job.index, jobGen, job.radiusIdentity,
                                  shape->BoundingSphere(), RadiusProvenance::AdaptedShape);
            }
            // Opt-in first handoff only. A successful read is from the owner's
            // identity-leased physical .rvmat member; it is not a current texture
            // binding, a prepared PAA, or a StoreTake. Never publish observations
            // from an obsolete model generation or invalid source association.
            struct RapObservation
            {
                std::string key, names;
                int status = -1; // -1 = physical read refused
                size_t count = 0;
                BankReadMemberIdentity member;
            };
            std::vector<RapObservation> rapObservations;
            std::unique_ptr<RapStageCandidateHandoff> rapHandoff;
            if (shape && job.radiusIdentityAccepted && job.radiusGeneration == jobGen &&
                !job.rapMaterials.empty())
            {
                try
                {
                    for (const auto& selected : job.rapMaterials)
                    {
                        {
                            std::lock_guard<std::mutex> guard(mutex);
                            if (stop || jobGen != generation.load(std::memory_order_relaxed) ||
                                job.index >= count || stale[job.index] || paths[job.index] != conversionPath ||
                                (job.sourceToken.diagnosticParseToken &&
                                 !SourceBindingMatches(job.index, job.sourceToken)))
                                break;
                        }
                        RapObservation observed;
                        observed.key = selected.key;
                        if (!selected.read.CopyMemberIdentity(observed.member)) break;
                        std::vector<char> bytes;
                        if (selected.read.bytes <= RapMaterialLimit && selected.read.Read(bytes) &&
                            bytes.size() == selected.read.bytes)
                        {
                            auto result = Streaming::ExtractOwnedRapStageNames(
                                std::span<const char>(bytes.data(), bytes.size()), selected.key);
                            observed.status = static_cast<int>(result.status);
                            observed.count = result.names.size();
                            for (const auto& name : result.names)
                            {
                                if (!observed.names.empty()) observed.names += ';';
                                observed.names += name;
                            }
                            if (result.status == Streaming::RapStageNamesStatus::Complete &&
                                !result.normalMapName.empty())
                            {
                                if (!rapHandoff)
                                {
                                    rapHandoff = std::make_unique<RapStageCandidateHandoff>();
                                    rapHandoff->generation = jobGen; rapHandoff->modelIndex = job.index;
                                }
                                rapHandoff->materials.push_back({selected.key,
                                    std::move(result.normalMapName),
                                    RapStageCandidateHandoff::Consumer::NormalMap,
                                    observed.member});
                            }
                        }
                        rapObservations.push_back(std::move(observed));
                    }
                }
                catch (...) { rapObservations.clear(); rapHandoff.reset(); } // Optional discovery cannot fail conversion.
            }
            if (rapHandoff && !rapHandoff->Bounded()) rapHandoff.reset();
            PaaAttemptRow attemptRow;
            auto reportAttempts = [&](bool published, double publicationDelayMs) {
                if (!paaAttemptDiagnostics || job.textures.empty()) return;
                const unsigned row = paaAttemptDiagnostics->NextLogRow();
                if (row < 64)
                    LOG_INFO(World, "PAA prepare attempts: generation={} textureGeneration={} modelIndex={} inputs={} published={} preSkipped={} inFlightSuppressed={} inFlightPassThrough={} started={} filenameOverlap={} trackingOverflow={} readFailed={} putAccepted={} putRejected={} cancelledBeforeRead={} cancelledBeforePut={} threw={} successfulReadDecodeMs={:.3f} rejectedReadDecodeMs={:.3f} rejectedReadDecodeFactsMs={:.3f} conversionCompleteToTerminalMs={:.3f}",
                        jobGen, job.textureGeneration, job.index, job.textures.size(), published, attemptRow.preSkipped, attemptRow.inFlightSuppressed, attemptRow.inFlightPassThrough, attemptRow.started,
                        attemptRow.overlapping, attemptRow.untracked, attemptRow.readFailed, attemptRow.accepted,
                        attemptRow.rejected, attemptRow.cancelledBeforeRead, attemptRow.cancelledBeforePut, attemptRow.threw, attemptRow.readDecodeMs,
                        attemptRow.rejectedReadDecodeMs, attemptRow.rejectedReadDecodeFactsMs, publicationDelayMs);
                else if (row == 64) LOG_INFO(World, "PAA prepare attempts: log truncated after 64 conversion rows; filename/generation overlap is not physical identity or full coverage");
            };
            std::unique_ptr<render::ColdPaaOwned> ownedCold;
            if (shape && job.coldTexture)
            {
                auto cancelled = [&] {
                    std::lock_guard<std::mutex> guard(mutex);
                    return stop || jobGen != generation.load(std::memory_order_relaxed) ||
                        job.index >= count || stale[job.index] || !job.texturePublication.Valid();
                };
                ownedCold = render::PrepareColdPaa(std::move(*job.coldTexture), job.texturePublication, cancelled);
                // Existing per-instance worker seam; no borrowed pixel/source arguments.
                // For this branch it observes a completed chain before Converted publication, not a store Put.
                try { if (ownedCold && publicationObserver.beforePaaPut)
                    publicationObserver.beforePaaPut(publicationObserver.context, false); }
                catch (...) { ownedCold.reset(); }
                if (cancelled()) ownedCold.reset();
            }
            uint64_t prepared = 0, preparedBytes = 0, skipped = 0, unreadable = 0;
            const auto textureStart = std::chrono::steady_clock::now();
            if (shape)
            {
                auto& store = render::PreparedTextureStore::Instance();
                for (const auto& texture : job.textures)
                {
                    if (ownedCold && texture.key == ownedCold->read.key &&
                        texture.ReadRequest().SameArchiveMember(ownedCold->read.source->Request())) continue;
                    if (!PreparePackedTexture(texture, jobGen, job.index, job.textureGeneration,
                        job.texturePublication, job.texturePublicationRequired,
                        job.broadParentPaa, attemptRow,
                        prepared, preparedBytes, skipped, unreadable)) break;
                }
            }

            const double textureMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - textureStart).count();

            lock.lock();
            // Scratch debt survives payload destruction on every terminal/cancel path.
            if (job.coldTexture) { if (ownedCold) ++stats.coldTexturePrepared; else ++stats.coldTextureRefused; }
            if (jobGen != generation.load(std::memory_order_relaxed) || stale[job.index] ||
                (job.sourceToken.diagnosticParseToken && !SourceBindingMatches(job.index, job.sourceToken)))
            {
                if (jobGen == generation.load(std::memory_order_relaxed))
                    Set(job.index, State::Unknown);
                ++stats.dropped;
                stats.conversionReservedBytes -= job.reservation;
                cv.notify_all();
                const double terminalMs = paaAttemptDiagnostics ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - conversionCompleted).count() : 0;
                lock.unlock();
                reportAttempts(false, terminalMs);
                delete shape;
                ownedCold.reset(); job.coldTexture.reset();
                job.rapMaterials.clear(); // release archive leases OUTSIDE queue mutex
                rapHandoff.reset();
                rapObservations.clear();
                job.model.reset();
                job.tables.reset();
                lock.lock();
                stats.conversionReservedBytes -= textureScratch; cv.notify_all();
                continue;
            }
            stats.texPrepared += prepared;
            stats.texPreparedBytes += preparedBytes;
            stats.texSkipped += skipped;
            stats.texUnreadable += unreadable;
            stats.texMs += textureMs;
            stats.convertMs += convMs;
            stats.convertMaxMs = std::max(stats.convertMaxMs, convMs);
            const bool publishedShape = shape && job.index < count;
            if (publishedShape)
            {
                conversionBytes[job.index] = job.reservation;
                convertedShapes[job.index] = shape;
                shape = nullptr; // Ownership is now in the published slot.
                convertedModels[job.index] = std::move(job.model);
                if (rapHandoff) for (auto& slot : convertedRapStages)
                    if (!slot.payload)
                    { slot.index = job.index; slot.payload = std::move(rapHandoff); break; }
                if (ownedCold && job.index < convertedColdTextures.size())
                {
                    const auto bytes = ownedCold->KnownBytes();
                    if (stats.coldTextureReadyBytes <= render::ColdPaaOwned::ReadyLimit &&
                        bytes <= render::ColdPaaOwned::ReadyLimit - stats.coldTextureReadyBytes &&
                        (!stats.readyByteBudget || (stats.readyPayloadBytes <= stats.readyByteBudget &&
                        bytes <= stats.readyByteBudget - stats.readyPayloadBytes)))
                    {
                        stats.readyPayloadBytes += bytes; stats.coldTextureReadyBytes += bytes;
                        stats.peakReadyPayloadBytes = std::max(stats.peakReadyPayloadBytes, stats.readyPayloadBytes);
                        convertedColdTextures[job.index] = std::move(ownedCold);
                    }
                    else ++stats.coldTextureRefused; // normal Converted geometry still progresses; owner reads normally
                }
                if (job.index < sourceEnvelopes.size()) sourceEnvelopes[job.index].convertedToken = job.sourceToken;
                Set(job.index, State::Converted);
                ++stats.converted;
            }
            else
            {
                stats.conversionReservedBytes -= job.reservation;
                cv.notify_all();
                // Conversion failed: sticky Failed, the admit loop falls back to the
                // synchronous path for this model exactly like a parse failure.
                if (job.index < count)
                    Set(job.index, State::Failed);
                ++stats.failed;
            }
            const double terminalMs = paaAttemptDiagnostics ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - conversionCompleted).count() : 0;
            // Last references can release bank resources or large model storage.
            // Their destructors must never run under the preparer's queue mutex.
            lock.unlock();
            reportAttempts(publishedShape, terminalMs);
            if (publishedShape && !rapObservations.empty())
            {
                try
                {
                    static std::atomic<unsigned> rapRows{0};
                    for (const auto& observed : rapObservations)
                    {
                        const unsigned row = rapRows.fetch_add(1, std::memory_order_relaxed);
                        if (row >= 16) break;
                        LOG_INFO(World, "RVMAT raP source discovery: generation={} modelIndex={} model={} material={} status={} stages={} memberOffset={} memberBytes={} archiveBytes={} volume={} names={} (diagnostic only; no PAA capture/Take/upload)",
                            jobGen, job.index, conversionPath, observed.key, observed.status, observed.count,
                            observed.member.offset, observed.member.bytes, observed.member.archiveBytes,
                            observed.member.volume, observed.names);
                    }
                }
                catch (...) {} // Diagnostic logging must not kill the worker.
            }
            delete shape;
            ownedCold.reset(); job.coldTexture.reset();
            job.rapMaterials.clear(); // release archive leases OUTSIDE queue mutex
            rapHandoff.reset();
            rapObservations.clear();
            job.model.reset();
            job.tables.reset();
            lock.lock();
            stats.conversionReservedBytes -= textureScratch; cv.notify_all();
            continue;
        }
        bool earlyWarm = false;
        if (ShouldRunWarm(earlyWarm))
        {
            const size_t coldQueueAtSelection = queue.size();
            WarmJob job = std::move(warm->queue.front()); warm->queue.erase(warm->queue.begin());
            stats.warmTextureQueued = warm->queue.size(); ++stats.warmTextureActive;
            stats.warmTextureScratchBytes += Streaming::WarmTextureJobPolicy::ScratchBytes;
            stats.warmTextureScratchBytesPeak = std::max(stats.warmTextureScratchBytesPeak, stats.warmTextureScratchBytes);
            lock.unlock();
            uint64_t prepared = 0, bytes = 0, skipped = 0, unreadable = 0;
            PaaAttemptRow row;
            const auto began = std::chrono::steady_clock::now();
            bool keep = job.publication.Valid();
            if (keep)
            {
                try
                {
                    const auto& read = job.reads[job.next];
                    PackedTextureRead texture{read.key, {}, read.initializedSource, &read.initializedSource->Request()};
                    if (auto* proof = Streaming::WarmTextureProvenance::Active())
                    {
                        if (earlyWarm)
                            Streaming::WarmTextureProvenance::Emit(proof->Stamp(
                                Streaming::WarmTextureProvenance::Match(read.key, read.initializedSource),
                                Streaming::WarmTextureProvenance::Event::EarlyWorkerStarted,
                                coldQueueAtSelection, job.generation));
                        Streaming::WarmTextureProvenance::Emit(proof->Stamp(
                            Streaming::WarmTextureProvenance::Match(read.key, read.initializedSource),
                            Streaming::WarmTextureProvenance::Event::WorkerStarted, job.index, job.generation));
                    }
                    keep = PreparePackedTexture(texture, job.generation, job.index, job.textureGeneration,
                        job.publication, true, false, row, prepared, bytes, skipped, unreadable);
                    ++job.next;
                }
                catch (...) { ++unreadable; ++job.next; }
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
            lock.lock();
            --stats.warmTextureActive;
            stats.warmTextureScratchBytes -= Streaming::WarmTextureJobPolicy::ScratchBytes;
            stats.warmTextureMembersPrepared += prepared; stats.warmTextureMembersSkipped += skipped;
            stats.warmTextureMembersUnreadable += unreadable; stats.warmTextureMs += ms;
            keep = keep && !stop && job.generation == generation.load(std::memory_order_relaxed) &&
                job.index < count && !stale[job.index] && job.publication.Valid();
            if (keep && job.next < job.reads.size())
            {
                try { warm->queue.push_back(std::move(job)); stats.warmTextureQueued = warm->queue.size(); }
                catch (...) { keep = false; }
                if (keep) { cv.notify_all(); continue; }
            }
            if (!keep) ++stats.warmTextureJobsCancelled; else ++stats.warmTextureJobsCompleted;
            stats.warmTextureMetadataBytes -= job.metadataBytes;
            warm->slots[job.slot] = {};
            cv.notify_all();
            lock.unlock();
            job.reads.clear(); job.publication = {}; // release leases outside queue mutex
            lock.lock();
            continue;
        }
        const uint32_t index = queue.front();
        const uint64_t reservation = ParseReservation();
        queue.pop_front();
        --stats.queued;
        if (index >= count)
            continue;
        const uint64_t gen = generation.load(std::memory_order_relaxed);
        const std::string path = paths[index];
        const bool prepareBc3 = prepareCompositeBc3[index] != 0;
        const bool prepareEnvelope = index < sourceEnvelopes.size() && sourceEnvelopes[index].requested;
        const render::DdsPublicationToken publication{ddsPublication, index,
            ddsPublication ? ddsPublication->Epoch(index) : 0};
        auto packedRead = packedReads[index];
        Set(index, State::Parsing);
        stats.parseReservedBytes += reservation;
        lock.unlock();

        // ---- unlocked: the parse ------------------------------------------------------
        // Watched for the whole unlocked span -- parse AND texture prepare -- because a
        // worker that wedges here is the quietest failure this file can produce. The main
        // thread does not wait on it: the model simply stays Parsing forever, the admit
        // loop keeps falling back, and the only visible symptom is that streaming quietly
        // stops making progress. Nothing is logged today, and there is no timer on the
        // path because every existing timer here is recorded on completion.
        //
        // Reachability, stated because it is not obvious and a scope that never runs reads
        // as "nothing stalled here": these workers exist only for the modern-OPRW object
        // table (Landscape::EnsureModernObjectPreparer), so a classic CWA world never
        // enters this loop at all. Verified 2026-09-01 -- a full Eden mission load with a
        // 1 ms watchdog deadline produced no `objstream` report, because the preparer was
        // never constructed. The scope covers the A2/A3-lineage worlds, where this is the
        // async path and the stall would otherwise be silent.
        Poseidon::WatchDogItem watch = Poseidon::WatchScopeFor("objstream", path);
        const auto start = std::chrono::steady_clock::now();
        ModelPtr model;
        std::string error;
        bool opened = false;
        bool nativeArchiveModel = false;
        try
        {
            model = ModelCache::LoadLooseFile(path, &error, &opened);
            if (!opened && packedRead)
            {
                std::vector<char> bytes;
                opened = true;
                if (packedRead->Read(bytes))
                    model = ModelCache::LoadOwnedBytes(bytes.data(), bytes.size(), path, error);
                else
                    error = "owned archive read failed; retry through VFS";
            }
            // RFG-103: a native `.xob` is not a loose file -- it lives in a Reforger pak. The
            // world loader's external loader (ModelCache::SetExternalLoader) reads the pak
            // (one ifstream per read) and runs the pure converter, so it is safe here; without
            // this every native model was "not loose" and fell back to the main thread
            // (measured: prep=0/503, sync=503, 226 ms parse hitches per house).
            if (!opened && !model)
            {
                const size_t n = path.size();
                const bool xob = n > 4 && (path[n - 4] == '.') && (path[n - 3] == 'x' || path[n - 3] == 'X') &&
                                 (path[n - 2] == 'o' || path[n - 2] == 'O') && (path[n - 1] == 'b' || path[n - 1] == 'B');
                if (xob)
                    if (ModelCache::ExternalLoader external = ModelCache::GetExternalLoader())
                    {
                        std::string why;
                        model = external(path, why);
                        nativeArchiveModel = model != nullptr;
                        opened = true; // it was ours to load; a null model is a parse failure, not "not loose"
                        if (!model)
                            error = why.empty() ? "external loader declined" : why;
                    }
            }
        }
        catch (const std::exception& e)
        {
            // LoadLooseFile catches around the readers; this is for the allocation of the
            // Model itself or anything else outside that try. A worker must never unwind
            // out of its loop.
            model.reset();
            opened = true;
            error = e.what();
        }
        catch (...)
        {
            model.reset();
            opened = true;
            error = "unknown exception";
        }
        // An identity-leased narrow PBO source may be the final archive handle
        // after Reset retires its inventory. Release it while unlocked, on every
        // parse success/failure path, before reacquiring the preparer queue mutex.
        packedRead.reset();
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        // Canonical loaders (including owned DDC deserialization) return a fresh
        // IR. External loaders may share/mutate theirs: never inspect it here.
        Streaming::StaticSourceEnvelope sourceEnvelope;
        Streaming::StaticPlainSourceSummary plainSource;
        bool envelopeScanned = false;
        double envelopeMs = 0.0;
        if (prepareEnvelope && model && !nativeArchiveModel)
        {
            const auto envelopeStart = std::chrono::steady_clock::now();
            try
            {
                sourceEnvelope = Streaming::BuildStaticSourceEnvelope(*model, gen,
                    Streaming::StaticSourceCoverage::FullCompiledIR);
                plainSource = Streaming::BuildStaticPlainSourceSummary(*model, sourceEnvelope);
                envelopeScanned = true;
            }
            catch (...) {} // optional evidence cannot fail normal preparation
            envelopeMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - envelopeStart).count();
        }

        // ---- unlocked: the texture prepare (stage 2) ------------------------------------
        // The IR names its face textures (Model::Material::texturePath / embeddedStages);
        // read the block-compressed loose PAAs among them NOW, on this thread, so the main
        // thread's EnsureUploaded inside RegisterGpuModel takes ready blocks instead of a
        // file open + LZO per texture. Everything used here is pure (ReadPAABlockChain:
        // ifstream + LZO; PreparedTextureStore: its own mutex): no texture bank, no file
        // server, no GPU. Textures the store refuses (already uploaded, already stored,
        // over budget) are skipped; textures that are not loose DXT PAAs simply fail to
        // read here and take the old path on the main thread -- counted, never fatal.
        // Materials resolved at registration time (.rvmat/.emat maps) are not in the IR and
        // are NOT prepared; the admit split's texture timers measure what that costs.
        uint64_t texPrepared = 0, texPreparedBytes = 0, texSkipped = 0, texUnreadable = 0;
        double texMs = 0.0;
        if (model && render::PreparedTextureStore::Enabled())
        {
            const auto texStart = std::chrono::steady_clock::now();
            try
            {
                render::PreparedTextureStore& store = render::PreparedTextureStore::Instance();
                const uint64_t textureGeneration = store.Generation();
                const bool bc3Only = nativeArchiveModel && render::PreparedTextureStore::NativeDdsBc3Only();
                const auto& mount = Asset::Formats::Enfusion::EnfusionMount::Instance();
                const auto names = CollectObjectStreamTextureNames(*model, nativeArchiveModel,
                    render::PreparedTextureStore::NativeDdsEnabled(),
                    [&](const std::string& path) {
                        // Loose files may be case-sensitive. Native archive entries
                        // have a canonical identity, without any filesystem fallback.
                        return nativeArchiveModel ? mount.CanonicalPath(path).value_or(std::string{}) : path;
                    },
                    [&](const std::string& path, Asset::Material::EmatMaterial& material, bool& complete) {
                        if (nativeArchiveModel) return ReadNativeEmat(mount, path, material, complete);
                        // Preserve the loose reader's route and partial-inheritance
                        // fallback, while exposing whether a retry can still add maps.
                        std::string text;
                        if (!Asset::Material::ReadEmatTextLoose(path, text)) return false;
                        material = Asset::Material::ParseEmat(text);
                        if (!material.valid()) return false;
                        complete = Asset::Material::ResolveEmatInheritance(material,
                            [](const std::string& parent, std::string& parentText) {
                                return Asset::Material::ReadEmatTextLoose(parent, parentText);
                            });
                        return true;
                    });
                for (const std::string& key : names)
                {
                    // Filter before store lookups or source reads. Raw material
                    // stages and nested tint wrappers stay on their ordinary path.
                    if (bc3Only && (!prepareBc3 || !key.starts_with("enfa|")))
                        continue;
                    {
                        // Poll between textures, never hold the queue mutex during I/O.
                        // Check generation first: Reset may have replaced/shrunk stale.
                        std::lock_guard<std::mutex> pendingLock(mutex);
                        if (stop || gen != generation.load(std::memory_order_relaxed) || stale[index])
                            break;
                    }
                    if (!store.ShouldPrepare(key))
                    {
                        ++texSkipped;
                        continue;
                    }
                    if (nativeArchiveModel && render::PreparedTextureStore::NativeDdsEnabled() && key.size() >= 4 &&
                        (key.ends_with(".edds") || key.ends_with(".dds")))
                    {
                        size_t bytes = 0;
                        if (PrepareNativeDds(key, textureGeneration, bytes, prepareBc3, bc3Only,
                            encodingObserver.afterTopMip ? &encodingObserver : nullptr,
                            publication, publicationObserver,
                            [&](size_t needed) {
                                std::lock_guard<std::mutex> budgetLock(mutex);
                                if (stop || gen != generation.load(std::memory_order_relaxed) ||
                                    index >= count || stale[index]) return false;
                                if (stats.readyByteBudget)
                                {
                                    uint64_t remaining = stats.readyByteBudget;
                                    for (const uint64_t charge : {stats.readyPayloadBytes, stats.parseReservedBytes,
                                                                 stats.conversionReservedBytes, stats.textureEncodeReservedBytes})
                                        remaining -= std::min(remaining, charge);
                                    if (needed > remaining) { ++stats.workerBc3BudgetSkipped; return false; }
                                }
                                stats.textureEncodeReservedBytes += needed;
                                stats.peakTextureEncodeReservedBytes = std::max(stats.peakTextureEncodeReservedBytes,
                                                                              stats.textureEncodeReservedBytes);
                                return true;
                            },
                            [&](size_t reserved, bool prepared, double ms) {
                                std::lock_guard<std::mutex> budgetLock(mutex);
                                stats.textureEncodeReservedBytes -= reserved;
                                stats.workerBc3Prepared += prepared;
                                stats.workerBc3Ms += ms;
                                cv.notify_all();
                            }))
                        {
                            ++texPrepared;
                            texPreparedBytes += bytes;
                        }
                        else
                        {
                            ++texUnreadable;
                        }
                        continue;
                    }
                    auto prepared = Streaming::PrepareLoosePaa(key, SIZE_MAX);
                    if (prepared.kind == Streaming::LoosePaaPreparation::Kind::Unavailable)
                    {
                        ++texUnreadable; // Ordinary owner fallback retains full quality.
                        continue;
                    }
                    if (prepared.kind == Streaming::LoosePaaPreparation::Kind::Rgba)
                    {
                        auto& image = prepared.image;
                        const size_t imageBytes = image.rgba.size();
                        if (store.PutDecoded(key, std::move(image), textureGeneration))
                        {
                            ++texPrepared;
                            texPreparedBytes += imageBytes;
                        }
                        else
                        {
                            ++texSkipped;
                        }
                        continue;
                    }
                    auto& chain = prepared.chain;
                    const size_t bytes = chain.blocks.size();
                    if (store.Put(key, std::move(chain), textureGeneration))
                    {
                        ++texPrepared;
                        texPreparedBytes += bytes;
                    }
                    else
                    {
                        ++texSkipped; // raced with another worker or the budget: fine
                    }
                }
            }
            catch (...)
            {
                // Texture-name collection and loose decodes can allocate too.
                // Keep this optional stage from unwinding out of the worker loop.
                ++texUnreadable;
            }
            texMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - texStart).count();
        }
        // ---------------------------------------------------------------------------------

        const uint64_t payload = model ? Model::ResidentPayloadBytes(*model) : 0;
        lock.lock();
        stats.parseReservedBytes -= reservation;
        cv.notify_all(); // failed/stale completion also releases its reservation
        if (warm && gen == generation.load(std::memory_order_relaxed) && !stale[index])
            warm->earlyFirst.ColdCompleted();
        stats.texPrepared += texPrepared;
        stats.texPreparedBytes += texPreparedBytes;
        stats.texSkipped += texSkipped;
        stats.texUnreadable += texUnreadable;
        stats.texMs += texMs;
        stats.sourceEnvelopeScans += envelopeScanned;
        stats.sourceEnvelopeMs += envelopeMs;
        if (gen != generation.load(std::memory_order_relaxed) || stale[index])
        {
            // Do not republish work discarded by a world or camera-window change.
            if (gen == generation.load(std::memory_order_relaxed))
                Set(index, State::Unknown);
            ++stats.dropped;
            lock.unlock();
            model.reset();
            lock.lock();
            continue;
        }
        stats.workerMs += ms;
        stats.workerMaxMs = std::max(stats.workerMaxMs, ms);
        const bool parseFailed = !model && opened;
        if (prepareEnvelope && index < sourceEnvelopes.size() && sourceEnvelopes[index].requested)
        {
            auto& slot = sourceEnvelopes[index];
            slot.attempted = true;
            slot.envelope = sourceEnvelope;
            slot.plainSource = plainSource;
            stats.sourceEnvelopePublished += sourceEnvelope.State() == Streaming::StaticSourceEnvelopeState::SourceEvidence;
        }
        if (model)
        {
            // Canonical ODOL parses preserve the serialized engine sphere exactly
            // through adaptation/tail. IR MLOD/Xob bbox spheres are not equivalent.
            if (!nativeArchiveModel && model->sourceFormat == "ODOL")
                PublishRadius(index, gen, model->sourcePath, model->boundingSphere.radius,
                              RadiusProvenance::ParsedODOL);
            payloadBytes[index] = payload;
            stats.readyPayloadBytes += payload;
            stats.peakReadyPayloadBytes = std::max(stats.peakReadyPayloadBytes, stats.readyPayloadBytes);
            if (stats.readyByteBudget && payload > stats.readyByteBudget) ++stats.oversizedPayloads;
            models[index] = std::move(model);
            Set(index, State::Ready);
            ++stats.prepared;
            ++stats.ready;
            if (index < requestedAtUs.size() && requestedAtUs[index] != 0)
            {
                const uint64_t latency = NowUs() - requestedAtUs[index];
                stats.readyLatencyUsTotal += latency;
                stats.readyLatencyUsMax = std::max(stats.readyLatencyUsMax, latency);
            }
        }
        else if (!opened)
        {
            Set(index, State::NotLoose);
            ++stats.notLoose;
        }
        else
        {
            Set(index, State::Failed);
            ++stats.failed;
        }
        if (parseFailed)
        {
            lock.unlock();
            // The main thread will repeat this load through ShapeBank::New and hit the same
            // failure; say once, here, what it was, so the row is not a mystery.
            LOG_WARN(World,
                     "Object stream prepare: {} did not parse on the worker ({}); main thread will retry "
                     "synchronously",
                     path, error);
            lock.lock();
        }
    }
}

// ------------------------------------------------------------------------------------------

ObjectStreamPreparer::ObjectStreamPreparer(uint64_t readyByteBudget, const Bc3EncodingObserver* observer,
                                         const render::DdsPublicationObserver* publicationObserver)
    : _impl(std::make_unique<Impl>())
{
    _impl->stats.readyByteBudget = readyByteBudget;
    static const bool attemptDiagnostic = [] {
        const char* diag = std::getenv("WGR_PAA_PREP_ATTEMPT_DIAG");
        const char* pbo = std::getenv("WGR_OBJECT_STREAM_PBO_TEXTURES");
        return diag && std::strcmp(diag, "1") == 0 && pbo && pbo[0] == '1';
    }();
    if (attemptDiagnostic)
        try { _impl->paaAttemptDiagnostics = std::make_unique<PaaAttemptDiagnostics>(); }
        catch (...) { LOG_WARN(World, "PAA prepare attempts: diagnostic allocation failed; overlap coverage unavailable"); } // normal preparation remains available
    static const bool inflightExperiment = [] {
        const char* flag = std::getenv("WGR_PAA_PREP_INFLIGHT");
        const char* pbo = std::getenv("WGR_OBJECT_STREAM_PBO_TEXTURES");
        return flag && std::strcmp(flag, "1") == 0 && pbo && pbo[0] == '1';
    }();
    if (inflightExperiment)
        try { _impl->paaInflight = std::make_unique<Streaming::PaaPreparationInflight>(); }
        catch (...) { LOG_WARN(World, "PAA prep inflight: optional metadata allocation failed; ordinary preparation retained"); }
    if (WarmTextureJobsEnabled())
        try { _impl->warm = std::make_unique<Impl::WarmState>(); }
        catch (...) { } // optional allocation failure leaves all owner fallback paths unchanged
    if (observer) _impl->encodingObserver = *observer;
    if (publicationObserver) _impl->publicationObserver = *publicationObserver;
}

ObjectStreamPreparer::~ObjectStreamPreparer()
{
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        _impl->stop = true;
        if (_impl->ddsPublication) _impl->ddsPublication->InvalidateAll();
    }
    _impl->cv.notify_all();
    for (std::thread& worker : _impl->workers)
        if (worker.joinable())
            worker.join();
    _impl->workers.clear();
    // SIM-807. `convertedShapes` holds RAW LODShapeWithShadow pointers that this
    // class owns between the worker finishing and TakeConverted. Reset and
    // DropStale both delete them; destruction did not, so every shape converted
    // but not yet installed was leaked when the preparer went away. The vector's
    // own destructor cannot help -- it destroys the pointers, not the objects --
    // and the symptom is invisible short of a leak checker, because a shape that
    // was never installed is one nothing else references either.
    // Safe here and only here: the workers are joined above, so no one can be
    // writing the slot.
    for (LODShapeWithShadow*& shape : _impl->convertedShapes)
    {
        delete shape;
        shape = nullptr;
    }
    _impl->convertedShapes.clear();
}

bool ObjectStreamPreparer::Running() const
{
    return !_impl->workers.empty();
}

size_t ObjectStreamPreparer::ModelCount() const
{
    // Written only by Reset, on the main thread, which is the only caller of this too.
    return _impl->count;
}

void ObjectStreamPreparer::Reset(const std::string* paths, size_t modelCount)
{
    static const bool pboTextures = [] {
        const char* flag = std::getenv("WGR_OBJECT_STREAM_PBO_TEXTURES");
        return render::PreparedTextureStore::Enabled() && flag && flag[0] == '1';
    }();
    auto publication = (render::PreparedTextureStore::NativeDdsEnabled() || pboTextures || WarmTextureJobsEnabled() ||
        TenementPhysicalPaaScope::Enabled() || render::ColdPaaHandoffEnabled())
        ? render::DdsPublicationInventory::Create(modelCount) : nullptr;
    std::vector<std::unique_ptr<render::ColdPaaOwned>> freshCold;
    if (render::ColdPaaHandoffEnabled() && publication)
    { try { freshCold.resize(modelCount); } catch (...) { freshCold.clear(); } }
    std::vector<std::optional<BankReadRequest>> packedReads(modelCount);
    const char* packedFlag = std::getenv("WGR_OBJECT_STREAM_PBO");
    const char* treeFlag = std::getenv("WGR_OBJECT_STREAM_PBO_TREE_FIXTURE");
    const bool broadPacked = packedFlag && packedFlag[0] == '1';
    const bool narrowTree = !broadPacked && treeFlag && std::strcmp(treeFlag, "1") == 0;
    size_t packedCount = 0;
    uint8_t attemptedTree = 0;
    if (narrowTree && (!GUseFileBanks || ModelDerivedCache::Instance().Enabled()))
        LOG_INFO(World, "Tree fixture PBO source unavailable: fileBanks={} modelDdcEnabled={}",
            GUseFileBanks, ModelDerivedCache::Instance().Enabled());
    if ((broadPacked || narrowTree) && GUseFileBanks && !ModelDerivedCache::Instance().Enabled())
    {
        for (size_t i = 0; i < modelCount; ++i)
        {
            const auto& path = paths[i];
            if (narrowTree)
            {
                const int ordinal = Streaming::TreeFixtureModelOrdinal(path);
                if (ordinal < 0 || (attemptedTree & (1u << ordinal))) continue;
                attemptedTree |= static_cast<uint8_t>(1u << ordinal); // one archive lookup per fixture
            }
            if (path.size() < 4 || path.substr(path.size() - 4) != ".p3d") continue;
            try
            {
                if (auto* bank = QIFStreamB::AutoBank(path.c_str()))
                {
                    const char* member = path.c_str() + bank->GetPrefix().GetLength();
                    auto read = bank->CaptureReadRequest(member, narrowTree);
                    if (read && (!narrowTree ||
                        (read->HasArchiveIdentity() && Streaming::EligibleTreeFixtureBytes(read->bytes))))
                    {
                        packedReads[i] = std::move(read);
                        ++packedCount;
                    }
                    else if (narrowTree)
                        LOG_INFO(World, "Tree fixture PBO source refused: model={} reason=identity-or-size", path);
                }
                else if (narrowTree)
                    LOG_INFO(World, "Tree fixture PBO source refused: model={} reason=no-mounted-bank", path);
            }
            catch (...)
            {
                if (narrowTree) LOG_INFO(World, "Tree fixture PBO source refused: model={} reason=capture-exception", path);
                else throw;
            }
        }
    }
    if (packedCount)
    {
        if (narrowTree) LOG_INFO(World, "Tree fixture PBO source: {} exact identity-leased read requests", packedCount);
        else LOG_INFO(World, "Object stream prepare: {} owned PBO read requests", packedCount);
    }
    std::vector<std::optional<BankReadRequest>> retiredPackedReads;
    std::vector<Impl::ModelPtr> freeLater;
    std::vector<LODShapeWithShadow*> freeShapes;
    std::deque<Impl::ConvertJob> freeConversions;
    std::array<std::unique_ptr<RapStageCandidateHandoff>, 2> freeRap;
    std::array<std::unique_ptr<RapStageCandidateHandoff>, BuildingRapFixtures.size()> freePendingRap;
    std::array<std::vector<Impl::ProxyMaterialCandidate>, BuildingRapFixtures.size()> freeProxyMaterials;
    std::array<Impl::WarmJob, Streaming::WarmTextureJobPolicy::JobLimit> freeWarm;
    size_t freeWarmCount = 0;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        ++_impl->generation;
        if (_impl->ddsPublication) _impl->ddsPublication->InvalidateAll();
        publication.swap(_impl->ddsPublication); // old metadata retires outside queue mutex
        _impl->stats.texturePublicationBytes = _impl->ddsPublication ? _impl->ddsPublication->MetadataBytes() : 0;
        _impl->stats.ddsPublicationBytes = render::PreparedTextureStore::NativeDdsEnabled()
            ? _impl->stats.texturePublicationBytes : 0;
        _impl->stats.dropped += _impl->stats.ready;
        _impl->stats.queued = 0;
        _impl->stats.ready = 0;
        _impl->stats.readyPayloadBytes = 0;
        freshCold.swap(_impl->convertedColdTextures); // old payload/leases free outside queue lock
        _impl->stats.coldTextureReadyBytes = 0;
        _impl->stats.coldTextureSlotBytes = _impl->convertedColdTextures.capacity() * sizeof(std::unique_ptr<render::ColdPaaOwned>);
        // Do not clear parseReservedBytes: old-generation workers still own their reservation.
        _impl->payloadBytes.assign(modelCount, 0);
        _impl->rapFixtureAttempts = 0;
        _impl->rapBuildingAttempts = 0;
        _impl->queue.clear();
        // Jobs own model IR and bank-table references, not just queue indices.
        // Their last-reference destructors must not run under the queue mutex.
        if (_impl->warm)
        {
            _impl->warm->captureCursor = 0;
            _impl->warm->earlyFirst.Reset();
            for (auto& job : _impl->warm->queue) freeWarm[freeWarmCount++] = std::move(job);
            _impl->warm->queue.clear(); // moved-from entries only; preserve reserved8 storage
        }
        for (size_t i = 0; i < freeWarmCount; ++i)
        {
            const auto& job = freeWarm[i];
            _impl->stats.warmTextureMetadataBytes -= job.metadataBytes;
            _impl->warm->slots[job.slot] = {};
            ++_impl->stats.warmTextureJobsCancelled;
        }
        _impl->stats.warmTextureQueued = 0;
        // Active old-generation jobs keep both their slot and metadata debt.
        freeConversions.swap(_impl->convertQueue);
        for (const auto& job : freeConversions)
            _impl->stats.conversionReservedBytes -= job.reservation;
        for (size_t i = 0; i < _impl->pendingRap.size(); ++i)
        {
            auto& pending = _impl->pendingRap[i];
            if (pending.job)
            {
                _impl->stats.conversionReservedBytes -= pending.job->reservation;
                freeConversions.push_back(std::move(*pending.job));
                pending.job.reset();
            }
            freePendingRap[i] = std::move(pending.handoff);
            freeProxyMaterials[i] = std::move(pending.proxyMaterials);
            pending.index = uint32_t(-1);
        }
        for (const uint64_t bytes : _impl->conversionBytes)
            _impl->stats.conversionReservedBytes -= bytes;
        // Active jobs retain their local reservation across generation changes.
        _impl->conversionBytes.assign(modelCount, 0);
        for (Impl::ModelPtr& m : _impl->models)
            if (m)
                freeLater.push_back(std::move(m));
        for (LODShapeWithShadow*& sh : _impl->convertedShapes)
            if (sh)
            {
                freeShapes.push_back(sh);
                sh = nullptr;
            }
        for (Impl::ModelPtr& m : _impl->convertedModels)
            if (m)
                freeLater.push_back(std::move(m));
        _impl->convertedShapes.assign(modelCount, nullptr);
        _impl->convertedModels.assign(modelCount, nullptr);
        for (size_t i = 0; i < _impl->convertedRapStages.size(); ++i)
        {
            freeRap[i] = std::move(_impl->convertedRapStages[i].payload);
            _impl->convertedRapStages[i].index = uint32_t(-1);
        }
        _impl->models.assign(modelCount, nullptr);
        _impl->stale.assign(modelCount, false);
        _impl->requestedAtUs.assign(modelCount, 0);
        _impl->paths.assign(paths, paths + modelCount);
        std::vector<uint8_t>(modelCount, 0).swap(_impl->prepareCompositeBc3);
        _impl->stats.bc3PolicyBytes = _impl->prepareCompositeBc3.capacity();
        // Old workers keep their encoding reservation until publication/destruction.
        // Fresh capacity tracks this inventory, rather than retaining a previous
        // world's larger certificate allocation after Reset.
        std::vector<Impl::RadiusSlot>(modelCount).swap(_impl->radiusCertificates);
        _impl->stats.radiusCertificateBytes =
            _impl->radiusCertificates.capacity() * sizeof(Impl::RadiusSlot);
        std::vector<Impl::SourceEnvelopeSlot>().swap(_impl->sourceEnvelopes);
        _impl->stats.sourceEnvelopeBytes = 0;
        retiredPackedReads.swap(_impl->packedReads); // old identity leases retire outside queue mutex
        _impl->packedReads.swap(packedReads);
        _impl->count = modelCount;
        _impl->states = modelCount > 0 ? std::make_unique<std::atomic<uint8_t>[]>(modelCount) : nullptr;
        for (size_t i = 0; i < modelCount; ++i)
            _impl->states[i].store(static_cast<uint8_t>(State::Unknown), std::memory_order_relaxed);
        if (modelCount > 0 && _impl->workers.empty() && AsyncEnabled())
        {
            const uint32_t n = WorkerCount();
            _impl->workers.reserve(n);
            for (uint32_t i = 0; i < n; ++i)
                _impl->workers.emplace_back([impl = _impl.get(), i] { impl->WorkerMain(i); });
            _impl->stats.workers = n;
        }
    }
    // Workers parked on the ready-limit predicate must re-evaluate: the ready set is empty now.
    _impl->cv.notify_all();
    freeLater.clear();
    for (LODShapeWithShadow* sh : freeShapes)
        delete sh;
}

bool ObjectStreamPreparer::Request(uint32_t index)
{
    return RequestInternal(index, false) == SourceEnvelopeRequest::Requested;
}

ObjectStreamPreparer::SourceEnvelopeRequest ObjectStreamPreparer::RequestWithStaticSourceEnvelope(uint32_t index)
{
    if (!Foundation::IsMainThread()) return SourceEnvelopeRequest::WrongThread;
    return RequestInternal(index, true);
}

ObjectStreamPreparer::SourceEnvelopeRequest ObjectStreamPreparer::RequestInternal(uint32_t index, bool sourceEnvelope)
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    if (index >= _impl->count || _impl->workers.empty())
        return SourceEnvelopeRequest::Unavailable;
    if (!sourceEnvelope) _impl->stale[index] = false; // preserve ordinary coalescing/rescue behavior
    const State current = _impl->Get(index);
    if (current != State::Unknown)
    {
        // The dedup hit. Split so the reported rate is the rate of asks that actually
        // saved a parse -- see the comment on Stats::coalesced.
        if (current == State::NotLoose || current == State::Failed)
            ++_impl->stats.coalescedStuck;
        else
            ++_impl->stats.coalesced;
        return SourceEnvelopeRequest::TooLate;
    }
    if (_impl->queue.size() >= QueueLimit())
    {
        ++_impl->stats.rejectedFull;
        return SourceEnvelopeRequest::Capacity;
    }
    if (sourceEnvelope && (_impl->count > SourceEnvelopeInventoryLimit ||
        _impl->paths[index].size() > SourceEnvelopeIdentityLimit ||
        _impl->nextDiagnosticParseToken == UINT64_MAX)) return SourceEnvelopeRequest::Capacity;
    if (sourceEnvelope && _impl->sourceEnvelopes.empty())
    {
        try { _impl->sourceEnvelopes.resize(_impl->count); }
        catch (...) { return SourceEnvelopeRequest::Capacity; }
        _impl->stats.sourceEnvelopeBytes = _impl->sourceEnvelopes.capacity() * sizeof(Impl::SourceEnvelopeSlot);
    }
    if (index < _impl->sourceEnvelopes.size())
    {
        _impl->sourceEnvelopes[index] = {};
        _impl->sourceEnvelopes[index].requested = sourceEnvelope;
        if (sourceEnvelope)
            _impl->sourceEnvelopes[index].diagnosticParseToken = ++_impl->nextDiagnosticParseToken;
    }
    _impl->stale[index] = false;
    _impl->Set(index, State::Queued);
    // Only the captured owner may read the mutable developer checkbox.
    // Other callers retain ordinary preparation without this optional sidecar.
    _impl->prepareCompositeBc3[index] = render::PreparedTextureStore::NativeDdsEnabled() &&
                                      Foundation::IsMainThread() &&
                                      Poseidon::Dev::GResidencyLevers().compressComposites;
    _impl->queue.push_back(index);
    if (index < _impl->requestedAtUs.size())
        _impl->requestedAtUs[index] = Impl::NowUs();
    ++_impl->stats.queued;
    ++_impl->stats.requested;
    _impl->cv.notify_one();
    return SourceEnvelopeRequest::Requested;
}

ObjectStreamPreparer::SourceEnvelopeSnapshot ObjectStreamPreparer::QueryStaticSourceEnvelope(uint32_t index) const
{
    SourceEnvelopeSnapshot out;
    if (!Foundation::IsMainThread()) return out;
    std::lock_guard<std::mutex> lock(_impl->mutex);
    out.generation = _impl->generation.load(std::memory_order_relaxed);
    if (index >= _impl->count || _impl->paths[index].size() > SourceEnvelopeIdentityLimit) return out;
    out.modelIdentity = _impl->paths[index];
    if (index < _impl->sourceEnvelopes.size())
    {
        out.attempted = _impl->sourceEnvelopes[index].attempted;
        out.diagnosticParseToken = _impl->sourceEnvelopes[index].diagnosticParseToken;
        out.envelope = _impl->sourceEnvelopes[index].envelope;
        out.plainSource = _impl->sourceEnvelopes[index].plainSource;
    }
    return out;
}

ObjectStreamPreparer::State ObjectStreamPreparer::Query(uint32_t index) const
{
    if (index >= _impl->count)
        return State::Unknown;
    return _impl->Get(index);
}

ObjectStreamPreparer::RadiusCertificate ObjectStreamPreparer::QueryRadius(uint32_t index) const
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    RadiusCertificate out;
    out.generation = _impl->generation.load(std::memory_order_relaxed);
    if (index >= _impl->count) return out;
    out.modelIdentity = _impl->paths[index];
    const auto& slot = _impl->radiusCertificates[index];
    out.provenance = slot.provenance;
    out.radius = slot.radius;
    if (slot.provenance != RadiusProvenance::None) out.state = RadiusState::Certified;
    return out;
}

bool ObjectStreamPreparer::RecordAdaptedRadius(uint32_t index, uint64_t generation,
                                             const std::string& modelIdentity, float radius)
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    return _impl->PublishRadius(index, generation, modelIdentity, radius, RadiusProvenance::AdaptedShape);
}

std::shared_ptr<Model::Model> ObjectStreamPreparer::Take(uint32_t index)
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    if (index >= _impl->count || _impl->Get(index) != State::Ready)
        return nullptr;
    std::shared_ptr<Model::Model> model = std::move(_impl->models[index]);
    _impl->models[index].reset();
    _impl->Set(index, State::Unknown);
    if (index < _impl->requestedAtUs.size() && _impl->requestedAtUs[index] != 0)
    {
        const uint64_t latency = Impl::NowUs() - _impl->requestedAtUs[index];
        _impl->stats.takeLatencyUsTotal += latency;
        _impl->stats.takeLatencyUsMax = std::max(_impl->stats.takeLatencyUsMax, latency);
        _impl->requestedAtUs[index] = 0;
    }
    ++_impl->stats.taken;
    --_impl->stats.ready;
    _impl->stats.readyPayloadBytes -= _impl->payloadBytes[index];
    _impl->payloadBytes[index] = 0;
    // Room in the ready set: wake a worker parked on the ready-limit predicate.
    _impl->cv.notify_one();
    return model;
}

size_t ObjectStreamPreparer::DropStale(const uint32_t* epochs, size_t count, uint32_t currentEpoch)
{
    std::unique_ptr<render::ColdPaaOwned> freeCold;
    std::vector<Impl::ModelPtr> freeLater;
    std::vector<LODShapeWithShadow*> freeShapes;
    std::deque<Impl::ConvertJob> freeConversions;
    std::array<std::unique_ptr<RapStageCandidateHandoff>, 2> freeRap;
    std::array<std::unique_ptr<RapStageCandidateHandoff>, BuildingRapFixtures.size()> freePendingRap;
    std::array<std::vector<Impl::ProxyMaterialCandidate>, BuildingRapFixtures.size()> freeProxyMaterials;
    std::array<Impl::WarmJob, Streaming::WarmTextureJobPolicy::JobLimit> freeWarm;
    size_t freeWarmCount = 0;
    size_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        const size_t n = std::min(count, _impl->count);
        // Main-owner handoff is before CancelModel: only entries already Put
        // with a still-valid model token can become source-scoped. The store
        // scans at most 32 insertion-order entries and performs no reads.
        // Put after this cut retains its model token and the loop below cancels it.
        if (_impl->ddsPublication && WarmPublishedReuseEnabled() && Foundation::IsMainThread())
            render::PreparedTextureStore::Instance().PromotePublishedWarmBeforeCancel(
                *_impl->ddsPublication, epochs, _impl->stale, n, currentEpoch);
        bool queueTouched = false;
        for (size_t i = 0; i < n; ++i)
        {
            if (epochs[i] != currentEpoch && !_impl->stale[i] && _impl->ddsPublication)
                _impl->ddsPublication->CancelModel(i);
            _impl->stale[i] = epochs[i] != currentEpoch;
            if (epochs[i] == currentEpoch)
                continue;
            if (i < _impl->sourceEnvelopes.size()) _impl->sourceEnvelopes[i] = {};
            const State state = _impl->Get(static_cast<uint32_t>(i));
            if (state == State::Ready)
            {
                freeLater.push_back(std::move(_impl->models[i]));
                _impl->models[i].reset();
                _impl->Set(static_cast<uint32_t>(i), State::Unknown);
                --_impl->stats.ready;
                _impl->stats.readyPayloadBytes -= _impl->payloadBytes[i];
                _impl->payloadBytes[i] = 0;
                ++dropped;
            }
            else if (state == State::Converted)
            {
                _impl->stats.conversionReservedBytes -= _impl->conversionBytes[i];
                _impl->conversionBytes[i] = 0;
                if (i < _impl->convertedColdTextures.size() && _impl->convertedColdTextures[i])
                {
                    const auto bytes = _impl->convertedColdTextures[i]->KnownBytes();
                    _impl->stats.readyPayloadBytes -= bytes; _impl->stats.coldTextureReadyBytes -= bytes;
                    auto retired = std::move(_impl->convertedColdTextures[i]);
                    retired->retiredNext = std::move(freeCold); freeCold = std::move(retired);
                }
                if (_impl->convertedShapes[i])
                {
                    freeShapes.push_back(_impl->convertedShapes[i]);
                    _impl->convertedShapes[i] = nullptr;
                }
                freeLater.push_back(std::move(_impl->convertedModels[i]));
                _impl->convertedModels[i].reset();
                for (size_t slot = 0; slot < _impl->convertedRapStages.size(); ++slot)
                    if (_impl->convertedRapStages[slot].payload && _impl->convertedRapStages[slot].index == i)
                    {
                        freeRap[slot] = std::move(_impl->convertedRapStages[slot].payload);
                        _impl->convertedRapStages[slot].index = uint32_t(-1);
                    }
                _impl->Set(static_cast<uint32_t>(i), State::Unknown);
                ++dropped;
            }
            else if (state == State::RapCapture)
            {
                for (size_t slot = 0; slot < _impl->pendingRap.size(); ++slot)
                {
                    auto& pending = _impl->pendingRap[slot];
                    if (pending.index != i || !pending.job) continue;
                    _impl->stats.conversionReservedBytes -= pending.job->reservation;
                    freeConversions.push_back(std::move(*pending.job));
                    pending.job.reset();
                    freePendingRap[slot] = std::move(pending.handoff);
                    freeProxyMaterials[slot] = std::move(pending.proxyMaterials);
                    pending.index = uint32_t(-1);
                    break;
                }
                _impl->Set(static_cast<uint32_t>(i), State::Unknown);
                ++dropped;
            }
            else if (state == State::Queued)
            {
                _impl->Set(static_cast<uint32_t>(i), State::Unknown);
                queueTouched = true;
                ++dropped;
            }
        }
        if (queueTouched)
        {
            // Rebuild rather than erase-in-place: the queue holds indices, and an index whose
            // state is no longer Queued is exactly one we un-queued above.
            std::deque<uint32_t> kept;
            for (uint32_t index : _impl->queue)
                if (index < _impl->count && _impl->Get(index) == State::Queued)
                    kept.push_back(index);
            _impl->queue.swap(kept);
            _impl->stats.queued = _impl->queue.size();
        }
        // Queued conversions own IR and bank tables too. Release them outside the
        // mutex; active conversions observe stale on completion instead.
        std::deque<Impl::ConvertJob> keptConversions;
        for (auto& job : _impl->convertQueue)
        {
            if (_impl->stale[job.index])
            {
                _impl->Set(job.index, State::Unknown);
                _impl->stats.conversionReservedBytes -= job.reservation;
                freeConversions.push_back(std::move(job));
                ++dropped;
            }
            else
                keptConversions.push_back(std::move(job));
        }
        _impl->convertQueue.swap(keptConversions);
        if (_impl->warm) for (auto it = _impl->warm->queue.begin(); it != _impl->warm->queue.end();)
        {
            if (it->index >= _impl->count || _impl->stale[it->index] || !it->publication.Valid())
            {
                _impl->stats.warmTextureMetadataBytes -= it->metadataBytes;
                _impl->warm->slots[it->slot] = {};
                freeWarm[freeWarmCount++] = std::move(*it); it = _impl->warm->queue.erase(it);
                ++_impl->stats.warmTextureJobsCancelled;
            }
            else ++it;
        }
        _impl->stats.warmTextureQueued = _impl->warm ? _impl->warm->queue.size() : 0;
        _impl->stats.dropped += dropped;
    }
    if (dropped > 0 || freeWarmCount > 0)
        _impl->cv.notify_all(); // room in the ready set, or a shorter queue: let parked workers re-check
    freeLater.clear();
    for (LODShapeWithShadow* sh : freeShapes)
        delete sh;
    while (freeCold) { auto next = std::move(freeCold->retiredNext); freeCold = std::move(next); }
    return dropped;
}

ObjectStreamPreparer::WarmCaptureCursor ObjectStreamPreparer::QueryWarmTextureCaptureCursor() const
{
    if (!Foundation::IsMainThread() || !WarmTextureJobsEnabled()) return {};
    std::lock_guard<std::mutex> lock(_impl->mutex);
    return _impl->warm ? WarmCaptureCursor{_impl->generation.load(std::memory_order_relaxed), _impl->warm->captureCursor} : WarmCaptureCursor{};
}
bool ObjectStreamPreparer::RecordWarmTextureCaptureCursor(WarmCaptureCursor token, size_t nextOrdinal)
{
    if (!Foundation::IsMainThread() || !WarmTextureJobsEnabled()) return false;
    std::lock_guard<std::mutex> lock(_impl->mutex);
    if (!_impl->warm || !token.generation || token.generation != _impl->generation.load(std::memory_order_relaxed)) return false;
    _impl->warm->captureCursor = nextOrdinal;
    return true;
}

ObjectStreamPreparer::WarmTextureSubmit ObjectStreamPreparer::SubmitWarmTextures(
    uint32_t index, const RadiusCertificate& inventoryToken, std::vector<WarmTextureRead> reads)
{
    if (!WarmTextureJobsEnabled()) return WarmTextureSubmit::Disabled;
    if (!Foundation::IsMainThread()) return WarmTextureSubmit::WrongThread;
    // Numeric receipt outlives every queue lock. It never owns a source or key.
    struct SubmitTrace
    {
        Streaming::WarmTextureProvenance* proof = Streaming::WarmTextureProvenance::Active();
        std::array<uint32_t, 8> tokens{}; size_t count = 0; bool submitted = false, stamped = false;
        std::array<Streaming::WarmTextureProvenance::Row, 8> rows{};
        void Capture() { if (!proof || stamped) return; stamped = true;
            for (size_t i = 0; i < count; ++i) rows[i] = proof->Stamp(tokens[i], submitted ?
                Streaming::WarmTextureProvenance::Event::Submitted :
                Streaming::WarmTextureProvenance::Event::SubmitRefused); }
        ~SubmitTrace() { Capture(); for (size_t i = 0; i < count; ++i)
            Streaming::WarmTextureProvenance::Emit(rows[i]); }
    } trace;
    if (trace.proof) for (const auto& read : reads)
    {
        if (trace.count == trace.tokens.size()) break;
        const auto token = Streaming::WarmTextureProvenance::Match(read.key, read.initializedSource);
        if (token) trace.tokens[trace.count++] = token;
    }
    // Validate bounded immutable evidence before taking the queue lock. No bank
    // lookup, source read or new source lease is performed by this API.
    uint64_t charge = sizeof(Impl::WarmState) + Streaming::WarmTextureJobPolicy::JobLimit * sizeof(Impl::WarmJob) + 256; // conservatively charge all queue storage per live job
    if (reads.empty() || reads.size() > Streaming::WarmTextureJobPolicy::MemberLimit ||
        reads.capacity() > Streaming::WarmTextureJobPolicy::MemberLimit ||
        !Streaming::WarmTextureJobPolicy::AddCharge(charge, reads.capacity() * sizeof(WarmTextureRead)))
        return WarmTextureSubmit::Unsupported;
    for (const auto& read : reads)
    {
        if (!read.initializedSource || read.key.empty() || read.key.size() >= 1024 || read.key.capacity() > 8192 ||
            read.key.find('\0') != std::string::npos || !read.initializedSource->Request().archiveLease ||
            !read.initializedSource->Request().bytes || read.initializedSource->Request().bytes > Impl::PackedTextureLimit ||
            !read.initializedSource->Request().SameArchiveMember(read.initializedSource->Request()) ||
            !Streaming::WarmTextureJobPolicy::AddCharge(charge, read.key.capacity() + 1) ||
            !Streaming::WarmTextureJobPolicy::AddCharge(charge, read.initializedSource->KnownCppBytes()))
            return WarmTextureSubmit::Unsupported;
    }
    if (!_impl->warm) return WarmTextureSubmit::Capacity;
    Impl::WarmJob job;
    job.index = index; job.reads = std::move(reads);
    job.textureGeneration = render::PreparedTextureStore::Instance().Generation();
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        if (index >= _impl->count || _impl->workers.empty() || !_impl->ddsPublication ||
            inventoryToken.generation != _impl->generation.load(std::memory_order_relaxed) ||
            inventoryToken.modelIdentity != _impl->paths[index])
            return WarmTextureSubmit::InvalidInventory;
        job.generation = inventoryToken.generation;
        job.publication = {_impl->ddsPublication, index, _impl->ddsPublication->Epoch(index)};
        if (_impl->stale[index] || !job.publication.Valid()) return WarmTextureSubmit::InvalidInventory;
        if (!Streaming::WarmTextureJobPolicy::AddCharge(charge, _impl->ddsPublication->MetadataBytes()))
            return WarmTextureSubmit::Capacity;
        size_t empty = _impl->warm->slots.size(), jobs = 0;
        for (size_t i = 0; i < _impl->warm->slots.size(); ++i)
        {
            const auto& slot = _impl->warm->slots[i];
            if (!slot.occupied) { if (empty == _impl->warm->slots.size()) empty = i; continue; }
            ++jobs;
            if (slot.index == index && slot.generation == job.generation && slot.publicationEpoch == job.publication.epoch)
            { ++_impl->stats.warmTextureJobsCoalesced; return WarmTextureSubmit::Coalesced; }
        }
        if (!Streaming::WarmTextureJobPolicy::Admit(jobs, job.reads.size(), _impl->stats.warmTextureMetadataBytes + _impl->WarmStorageBytes(), charge))
        { ++_impl->stats.warmTextureJobsRejected; return WarmTextureSubmit::Capacity; }
        job.metadataBytes = charge; job.slot = empty;
        try { _impl->warm->queue.push_back(std::move(job)); }
        catch (...) { ++_impl->stats.warmTextureJobsRejected; return WarmTextureSubmit::Capacity; }
        _impl->warm->slots[empty] = {true, index, inventoryToken.generation, _impl->ddsPublication->Epoch(index)};
        _impl->stats.warmTextureMetadataBytes += charge;
        _impl->stats.warmTextureMetadataBytesPeak = std::max(_impl->stats.warmTextureMetadataBytesPeak, _impl->stats.warmTextureMetadataBytes);
        _impl->stats.warmTextureQueued = _impl->warm ? _impl->warm->queue.size() : 0;
        ++_impl->stats.warmTextureJobsSubmitted;
        trace.submitted = true; trace.Capture(); // stamp before worker dequeue, log after queue mutex
    }
    _impl->cv.notify_all();
    return WarmTextureSubmit::Submitted;
}

bool ObjectStreamPreparer::SubmitConvert(uint32_t index, std::shared_ptr<Model::Model> model,
                                         std::shared_ptr<const Model::ShapeAdapter::AdapterBankTables> tables,
                                         const RadiusCertificate* inventoryToken,
                                         const SourceEnvelopeSnapshot* originalSource,
                                         bool ownerTextureAdmission,
                                         RapStageCapture rapStageCapture, void* rapStageContext)
{
    if (!model || !tables)
        return false;
    if (originalSource && !Foundation::IsMainThread()) return false;
    // Owner-thread snapshot only. Workers receive immutable archive spans and names,
    // never Texture pointers, bank handles, ParamFile state or the file-server MRU.
    std::unique_ptr<render::ColdPaaRead> coldTexture;
    if (ownerTextureAdmission && render::ColdPaaHandoffEnabled() && Foundation::IsMainThread() && WorkerCount() <= 2)
    {
        try {
            size_t visits = 0;
            for (size_t lod = 0; lod < tables->textures.size() && lod < model->lodLevels.size() && lod < 64; ++lod) {
                const auto& level = tables->textures[lod];
                if (++visits > 256 || coldTexture) break;
                const auto& mesh = model->lodLevels[lod].mesh;
                if (model->lodLevels[lod].purpose != Model::LodPurpose::Visual ||
                    !render::ColdPaaPrimaryHasGeometry(mesh.vertices.size(), mesh.triangles.size(), mesh.quads.size())) continue;
                for (const auto& texture : level) {
                    if (++visits > 256 || coldTexture) break;
                    if (!texture || texture->IsGpuResident()) continue;
                    render::ColdPaaRead read;
                    if (texture->CaptureColdPaaRead(read) && read.Valid())
                        coldTexture = std::make_unique<render::ColdPaaRead>(std::move(read));
                }
            }
        } catch (...) { coldTexture.reset(); } // Optional metadata failure falls back, no model-state change.
    }
    std::vector<Impl::CapturedRapMaterial> rapMaterials;
    int buildingRapFixture = -1;
    bool tenementMultistage = false;
    bool broadParentPaa = false;
    bool broadCaptureIncomplete = false;
    std::vector<Impl::CapturedProxyModel> proxyModels;
    bool proxyCaptureIncomplete = false;
    if (TenementPhysicalPaaScope::Enabled() && ownerTextureAdmission &&
        Foundation::IsMainThread() && GUseFileBanks && !originalSource &&
        render::PreparedTextureStore::Enabled() &&
        model->sourcePath == R"(dz\structures\residential\tenements\tenement_small.p3d)")
    {
        // Parent IR only. Do not infer proxy closure from names: the adapter's
        // config/file probe decides those later, on the owner thread.
        try
        {
            size_t visits = 0;
            uint64_t materialBytes = 0;
            for (const auto& level : model->lodLevels)
            {
                if (level.purpose != Model::LodPurpose::Visual) continue;
                for (const auto& material : level.mesh.materials)
                {
                    if (++visits > 256 || rapMaterials.size() >= 16)
                    { broadCaptureIncomplete = true; break; }
                    const auto path = Asset::VirtualPath::Parse(material.materialPath);
                    const std::string& key = path.canonical();
                    if (key.empty()) continue;
                    if (key.size() > 240 || path.extension() != ".rvmat" || path.looksHostAbsolute())
                    { broadCaptureIncomplete = true; continue; }
                    if (std::any_of(rapMaterials.begin(), rapMaterials.end(),
                        [&](const auto& prior) { return prior.key == key; })) continue;
                    QFBank* bank = QIFStreamB::AutoBank(key.c_str());
                    if (!bank) { broadCaptureIncomplete = true; continue; }
                    const RString prefix = bank->GetPrefix();
                    if (prefix.GetLength() < 0 ||
                        static_cast<size_t>(prefix.GetLength()) >= key.size() ||
                        CmpStartStr(key.c_str(), prefix))
                    { broadCaptureIncomplete = true; continue; }
                    auto read = bank->CaptureReadRequest(key.c_str() + prefix.GetLength(), true);
                    if (!read || !read->HasArchiveIdentity() || !read->bytes ||
                        read->bytes > Impl::RapMaterialLimit ||
                        materialBytes + read->bytes > 4ull * 1024 * 1024 ||
                        !bank->MatchesMountedMember(key.c_str() + prefix.GetLength(), *read))
                    { broadCaptureIncomplete = true; continue; }
                    materialBytes += read->bytes;
                    rapMaterials.push_back({key, std::move(*read)});
                }
                if (visits > 256 || rapMaterials.size() >= 16)
                { broadCaptureIncomplete = true; break; }
            }
            broadParentPaa = !rapMaterials.empty();
            if (broadParentPaa) buildingRapFixture = 0; // reuse the single parked raP slot
            LOG_INFO(World, "DayZ physical prefetch: parent model={} capturedRaP={} visitCount={} captureIncomplete={} materialSourceBytes={} maxRaP=16 maxVisits=256 (proxy IR excluded)",
                model->sourcePath, rapMaterials.size(), visits, broadCaptureIncomplete, materialBytes);
        }
        catch (...) { rapMaterials.clear(); broadParentPaa = false; buildingRapFixture = -1;
            broadCaptureIncomplete = true; }
    }
    static const bool dayzProxyPrefetch = [] {
        const char* flag = std::getenv("WGR_OBJECT_STREAM_DAYZ_PROXY_PREFETCH");
        return flag && std::strcmp(flag, "1") == 0;
    }();
    if (dayzProxyPrefetch && broadParentPaa && model->sourceFormat == "ODOL")
    {
        // These are speculative file-backed one-hop proxy candidates. The adapter's
        // later config probe may skip or replace any of them; only final PAA Init and
        // upload can prove actual consumption. Never load a proxy shape here.
        try
        {
            size_t visits = 0;
            uint64_t sourceBytes = 0;
            for (const auto& level : model->lodLevels)
            {
                if (level.purpose != Model::LodPurpose::Visual) continue;
                for (const auto& proxy : level.mesh.proxies)
                {
                    if (++visits > 256 || proxyModels.size() >= Impl::ProxyModelLimit)
                    { proxyCaptureIncomplete = true; break; }
                    const auto normalized = Model::ShapeAdapter::normalizeProxyModelName(proxy.name);
                    const RString shapeName = GetShapeName(RString(normalized.modelName.c_str()));
                    const auto path = Asset::VirtualPath::Parse(static_cast<const char*>(shapeName));
                    const std::string& key = path.canonical();
                    if (key.empty() || key.size() > 240 || path.extension() != ".p3d" ||
                        path.looksHostAbsolute())
                    { proxyCaptureIncomplete = true; continue; }
                    if (std::any_of(proxyModels.begin(), proxyModels.end(),
                        [&](const auto& prior) { return prior.key == key; })) continue;
                    QFBank* bank = QIFStreamB::AutoBank(key.c_str());
                    if (!bank) { proxyCaptureIncomplete = true; continue; }
                    const RString prefix = bank->GetPrefix();
                    if (prefix.GetLength() < 0 || static_cast<size_t>(prefix.GetLength()) >= key.size() ||
                        CmpStartStr(key.c_str(), prefix))
                    { proxyCaptureIncomplete = true; continue; }
                    auto read = bank->CaptureReadRequest(key.c_str() + prefix.GetLength(), true);
                    if (!read || !read->HasArchiveIdentity() ||
                        !DayzProxyPrefetchBounds::AdmitProxyModel(
                            proxyModels.size(), sourceBytes, read->bytes) ||
                        !bank->MatchesMountedMember(key.c_str() + prefix.GetLength(), *read))
                    { proxyCaptureIncomplete = true; continue; }
                    sourceBytes += read->bytes;
                    proxyModels.push_back({key, std::move(*read)});
                }
                if (visits > 256 || proxyModels.size() >= Impl::ProxyModelLimit)
                { proxyCaptureIncomplete = true; break; }
            }
            LOG_INFO(World, "DayZ proxy prefetch: parent={} capturedP3D={} visits={} sourceBytes={} captureIncomplete={} maxP3D=16 memberLimit=4194304 sourceLimit=33554432 speculative=true",
                model->sourcePath, proxyModels.size(), visits, sourceBytes, proxyCaptureIncomplete);
        }
        catch (...) { proxyModels.clear(); proxyCaptureIncomplete = true; }
    }
    // Exact selected DayZ fixtures only. No lookup, bank load or allocation in
    // the default path. A fixture is attempted at most once per world, whether
    // capture succeeds or not; it cannot grow per-model admission work unboundedly.
    static const bool rapDiscovery = [] {
        const char* value = std::getenv("WGR_OBJECT_STREAM_RAP_STAGE_SOURCE");
        return value && std::strcmp(value, "1") == 0;
    }();
    bool rapCandidateAdmissible = false;
    if (rapDiscovery && Streaming::SelectedTreeFixtureModel(model->sourcePath) &&
        !originalSource && ownerTextureAdmission && Foundation::IsMainThread() && GUseFileBanks)
    {
        std::lock_guard<std::mutex> guard(_impl->mutex);
        rapCandidateAdmissible = index < _impl->count && !_impl->workers.empty() &&
            _impl->Get(index) == State::Unknown && _impl->paths[index] == model->sourcePath;
    }
    if (rapCandidateAdmissible &&
        (_impl->rapFixtureAttempts & 3u) != 3u)
    {
        constexpr std::array<const char*, 2> fixtures{{
            R"(dz\plants\tree\data\t_piceaabies_2d_trunk.rvmat)",
            R"(dz\plants\tree\data\d_piceaabies_stumpb_trunk_a.rvmat)"}};
        try
        {
        size_t visited = 0;
        for (const auto& level : model->lodLevels)
        {
            if (visited >= 256 || rapMaterials.size() >= fixtures.size()) break;
            if (level.purpose != Model::LodPurpose::Visual) continue;
            for (const auto& material : level.mesh.materials)
            {
                if (++visited > 256 || rapMaterials.size() >= fixtures.size()) break;
                const auto path = Asset::VirtualPath::Parse(material.materialPath);
                if (path.empty() || path.looksHostAbsolute() || path.canonical().size() > 240) continue;
                for (size_t fixture = 0; fixture < fixtures.size(); ++fixture)
                {
                    if (path.canonical() != fixtures[fixture] ||
                        (_impl->rapFixtureAttempts & (1u << fixture))) continue;
                    _impl->rapFixtureAttempts |= static_cast<uint8_t>(1u << fixture);
                    const char* refusal = "no-mounted-bank";
                    try
                    {
                        if (QFBank* bank = QIFStreamB::AutoBank(path.canonical().c_str()))
                        {
                            refusal = "prefix-mismatch";
                            const RString prefix = bank->GetPrefix();
                            if (prefix.GetLength() >= 0 &&
                                static_cast<size_t>(prefix.GetLength()) < path.canonical().size() &&
                                !CmpStartStr(path.canonical().c_str(), prefix))
                            {
                                const char* member = path.canonical().c_str() + prefix.GetLength();
                                auto read = bank->CaptureReadRequest(member, true);
                                refusal = !read ? "identity-capture-refused" :
                                    (!read->HasArchiveIdentity() ? "no-physical-identity" :
                                    (read->bytes > Impl::RapMaterialLimit ? "material-over-256KiB" : nullptr));
                                if (!refusal)
                                    rapMaterials.push_back({path.canonical(), std::move(*read)});
                            }
                        }
                    }
                    catch (...) { refusal = "capture-exception"; }
                    if (refusal)
                    {
                        static std::atomic<unsigned> refusalRows{0};
                        if (refusalRows.fetch_add(1, std::memory_order_relaxed) < 16)
                            LOG_INFO(World, "RVMAT raP source capture refused: fixture={} material={} reason={} (owner-only diagnostic; ordinary admission unchanged)",
                                fixture, path.canonical(), refusal);
                    }
                }
            }
        }
        }
        catch (...) { rapMaterials.clear(); } // Optional diagnostic must not affect admission.
    }
    static const bool buildingRapPilot = [] {
        const char* value = std::getenv("WGR_OBJECT_STREAM_RAP_BUILDING_PILOT");
        return value && std::strcmp(value, "1") == 0;
    }();
    static const bool multistagePilot = [] {
        const char* value = std::getenv("WGR_OBJECT_STREAM_RAP_BUILDING_MULTISTAGE");
        return value && std::strcmp(value, "1") == 0;
    }();
    if (!broadParentPaa && buildingRapPilot && rapStageCapture && ownerTextureAdmission &&
        Foundation::IsMainThread() && WarmTextureJobsEnabled() &&
        ArchiveSourceBinding::ModelReadScope::MaterialPreflightReservationEnabled() &&
        render::PreparedTextureStore::Enabled() && GUseFileBanks && !originalSource)
    {
        const int fixture = BuildingRapFixtureOrdinal(model->sourcePath);
        if (fixture >= 0 && !(_impl->rapBuildingAttempts & (1u << fixture)))
        {
            bool currentJob = false;
            {
                std::lock_guard<std::mutex> guard(_impl->mutex);
                currentJob = index < _impl->count && !_impl->workers.empty() &&
                    _impl->Get(index) == State::Unknown && _impl->paths[index] == model->sourcePath &&
                    _impl->ddsPublication && _impl->ddsPublication->Epoch(index) != 0;
            }
            if (currentJob)
            {
                _impl->rapBuildingAttempts |= static_cast<uint8_t>(1u << fixture);
                try
                {
                    size_t visited = 0;
                    const auto& wanted = BuildingRapFixtures[static_cast<size_t>(fixture)];
                    const bool multistage = fixture == 0 && multistagePilot;
                    for (const auto& level : model->lodLevels)
                    {
                        if (visited >= 256 || (multistage ? rapMaterials.size() >= 16 : buildingRapFixture >= 0)) break;
                        if (level.purpose != Model::LodPurpose::Visual) continue;
                        for (const auto& material : level.mesh.materials)
                        {
                            if (++visited > 256 || (multistage && rapMaterials.size() >= 16)) break;
                            const auto path = Asset::VirtualPath::Parse(material.materialPath);
                            const std::string& key = path.canonical();
                            if (key.size() > 240 || path.extension() != ".rvmat" ||
                                path.looksHostAbsolute() ||
                                (multistage ? key.compare(0, std::strlen(TenementMaterialPrefix),
                                    TenementMaterialPrefix) != 0 : key != wanted.material) ||
                                std::any_of(rapMaterials.begin(), rapMaterials.end(),
                                    [&](const auto& prior) { return prior.key == key; })) continue;
                            if (QFBank* bank = QIFStreamB::AutoBank(key.c_str()))
                            {
                                const RString prefix = bank->GetPrefix();
                                if (prefix.GetLength() >= 0 &&
                                    static_cast<size_t>(prefix.GetLength()) < key.size() &&
                                    !CmpStartStr(key.c_str(), prefix))
                                {
                                    auto read = bank->CaptureReadRequest(
                                        key.c_str() + prefix.GetLength(), true);
                                    if (read && read->HasArchiveIdentity() && read->bytes &&
                                        read->bytes <= Impl::RapMaterialLimit &&
                                        bank->MatchesMountedMember(key.c_str() + prefix.GetLength(), *read))
                                    {
                                        rapMaterials.push_back({key, std::move(*read)});
                                        buildingRapFixture = fixture;
                                        tenementMultistage = multistage;
                                    }
                                }
                            }
                            if (!multistage) break;
                        }
                    }
                }
                catch (...) { rapMaterials.clear(); buildingRapFixture = -1; tenementMultistage = false; }
            }
        }
    }
    // The Converted handoff reaches the owner immediately before ObjectCreate, so
    // a warm job submitted there cannot reliably finish before first registration.
    // This separate exact-fixture opt-in captures one current NormalMap source now;
    // the existing conversion worker then prepares it before publishing Converted.
    static const bool earlyRapStage = [] {
        const char* value = std::getenv("WGR_OBJECT_STREAM_RAP_STAGE_EARLY_PREPARE");
        return value && std::strcmp(value, "1") == 0;
    }();
    std::optional<WarmTextureRead> earlyRapRead;
    if (earlyRapStage && rapDiscovery && rapStageCapture && ownerTextureAdmission &&
        Streaming::SelectedTreeFixtureModel(model->sourcePath) &&
        Foundation::IsMainThread() && WarmTextureJobsEnabled() &&
        ArchiveSourceBinding::ModelReadScope::MaterialPreflightReservationEnabled() &&
        render::PreparedTextureStore::Enabled() &&
        !originalSource && !rapMaterials.empty())
    {
        constexpr const char* normalKey = R"(dz\plants\tree\data\t_piceaabies_trunk_no.paa)";
        try
        {
            for (const auto& material : rapMaterials)
            {
                if (!material.read.HasArchiveIdentity() ||
                    material.read.bytes > Impl::RapMaterialLimit) continue;
                // A captured member is historical evidence. Confirm that the
                // material mount still selects it before capturing its stage.
                QFBank* bank = QIFStreamB::AutoBank(material.key.c_str());
                if (!bank) continue;
                const RString prefix = bank->GetPrefix();
                if (prefix.GetLength() < 0 ||
                    static_cast<size_t>(prefix.GetLength()) >= material.key.size() ||
                    CmpStartStr(material.key.c_str(), prefix)) continue;
                auto current = bank->CaptureReadRequest(
                    material.key.c_str() + prefix.GetLength(), true);
                if (!current || !current->SameArchiveMember(material.read)) continue;
                std::vector<char> bytes;
                if (!material.read.Read(bytes) || bytes.size() != material.read.bytes) continue;
                const auto stages = Streaming::ExtractOwnedRapStageNames(
                    std::span<const char>(bytes.data(), bytes.size()), material.key);
                if (stages.status != Streaming::RapStageNamesStatus::Complete ||
                    stages.normalMapName != normalKey) continue;
                WarmTextureRead candidate;
                if (!rapStageCapture(rapStageContext, normalKey, candidate) ||
                    candidate.key != normalKey || !candidate.initializedSource ||
                    !candidate.initializedSource->Request().HasArchiveIdentity() ||
                    !candidate.initializedSource->Request().bytes ||
                    candidate.initializedSource->Request().bytes > Impl::PackedTextureLimit ||
                    !render::PreparedTextureStore::Instance().ShouldPrepareWarm(candidate.key))
                    continue;
                earlyRapRead.emplace(std::move(candidate));
                break; // both selected raP fixtures name the same physical normal PAA
            }
        }
        catch (...) { earlyRapRead.reset(); } // Optional work never changes model admission.
    }
    std::vector<Impl::PackedTextureRead> textures;
    const uint64_t textureGeneration = render::PreparedTextureStore::Instance().Generation();
    if (earlyRapRead)
    {
        auto source = std::move(earlyRapRead->initializedSource);
        const BankReadRequest* request = &source->Request();
        textures.push_back({std::move(earlyRapRead->key), {}, std::move(source), request});
    }
    const char* flag = std::getenv("WGR_OBJECT_STREAM_PBO_TEXTURES");
    if (flag && flag[0] == '1' && GUseFileBanks && render::PreparedTextureStore::Enabled())
    {
        // With both existing experiments explicitly enabled, prefer the source
        // already initialized by BuildAdapterBankTables. Its immutable binding
        // describes the physical mounted member that EnsureUploaded will use;
        // a new name-based archive capture cannot establish that association.
        // This is owner-only metadata capture. The worker receives no Texture.
        if (ownerTextureAdmission && render::ColdPaaHandoffEnabled() && Foundation::IsMainThread())
        {
            for (const auto& level : tables->textures)
            {
                for (const auto& texture : level)
                {
                    if (textures.size() >= 64) break;
                    if (!texture || texture->IsGpuResident()) continue;
                    try
                    {
                        render::ColdPaaRead captured;
                        if (!texture->CaptureColdPaaRead(captured) || !captured.Valid() ||
                            captured.source->Request().bytes > Impl::PackedTextureLimit ||
                            !render::PreparedTextureStore::Instance().ShouldPrepareWarm(captured.key) ||
                            std::any_of(textures.begin(), textures.end(), [&](const auto& t) { return t.key == captured.key; }))
                            continue;
                        auto source = std::move(captured.source);
                        const BankReadRequest* request = &source->Request();
                        textures.push_back({std::move(captured.key), {}, std::move(source), request});
                    }
                    catch (...) {} // Optional preparation never prevents ordinary admission.
                }
                if (textures.size() >= 64) break;
            }
        }
        auto note = [&](const std::string& raw)
        {
            if (textures.size() >= 64) return; // bounded per-model metadata; remaining inputs use VFS
            const std::string key = render::PreparedTextureStore::Key(raw);
            if (key.size() < 4 || (key.substr(key.size()-4) != ".paa" && key.substr(key.size()-4) != ".pac")) return;
            if (!render::PreparedTextureStore::Instance().ShouldPrepare(key)) return;
            if (std::any_of(textures.begin(), textures.end(), [&](const auto& t) { return t.key == key; })) return;
            if (auto* bank = QIFStreamB::AutoBank(key.c_str()))
            {
                const char* member = key.c_str() + bank->GetPrefix().GetLength();
                auto read = bank->CaptureReadRequest(member, true);
                if (!read) read = bank->CaptureReadRequest(member); // unsupported identity retains ordinary preparation
                if (read && read->bytes <= Impl::PackedTextureLimit)
                    textures.push_back({key, std::move(*read)});
            }
        };
        for (const auto& level : model->lodLevels)
            for (const auto& material : level.mesh.materials)
            {
                note(material.texturePath);
                for (const auto& stage : material.embeddedStages) note(stage.texturePath);
            }
        for (const auto& level : tables->textures)
            for (const auto& texture : level)
                if (texture) note(static_cast<const char*>(texture->GetName()));
    }
    std::lock_guard<std::mutex> lock(_impl->mutex);
    if (index >= _impl->count || _impl->workers.empty())
        return false;
    if (_impl->Get(index) != State::Unknown)
        return false;
    SourceConversionToken sourceToken;
    if (originalSource)
    {
        sourceToken = {originalSource->generation, originalSource->diagnosticParseToken};
        if (!originalSource->attempted || originalSource->modelIdentity != _impl->paths[index] ||
            !_impl->SourceBindingMatches(index, sourceToken)) return false;
    }
    if (index < _impl->sourceEnvelopes.size()) _impl->sourceEnvelopes[index].convertedToken = {};
    _impl->stale[index] = false;
    _impl->Set(index, State::Converting);
    Impl::ConvertJob job;
    job.index = index;
    if (_impl->convertedColdTextures.size() != _impl->count) coldTexture.reset();
    job.texturePublicationRequired = !textures.empty() || bool(coldTexture) ||
        buildingRapFixture >= 0 || !proxyModels.empty();
    if (job.texturePublicationRequired)
        job.texturePublication = {_impl->ddsPublication, index,
            _impl->ddsPublication ? _impl->ddsPublication->Epoch(index) : 0};
    job.sourceToken = sourceToken;
    job.radiusGeneration = _impl->generation.load(std::memory_order_relaxed);
    // Admission lowercases sourcePath for ShapeBank after taking the IR. Only
    // an exact owner inventory token authorizes this otherwise ambiguous mapping.
    // A rejected explicit token never falls back to inference from sourcePath.
    job.radiusIdentityAccepted = inventoryToken
        ? inventoryToken->generation == job.radiusGeneration && inventoryToken->modelIdentity == _impl->paths[index]
        : model->sourcePath == _impl->paths[index];
    if (job.radiusIdentityAccepted) job.radiusIdentity = _impl->paths[index];
    // Retain the IR charge plus an equal workspace/output allowance. This is a
    // scheduling estimate, not a measurement of Shape allocations. Useful
    // conversions always progress; their debt parks subsequent speculative parses.
    job.reservation = 2 * Model::ResidentPayloadBytes(*model);
    if (coldTexture)
    {
        if (!job.texturePublication.Valid()) coldTexture.reset();
        else job.reservation += coldTexture->KnownMetadataBytes() +
            job.texturePublication.inventory->MetadataBytes();
    }
    job.coldTexture = std::move(coldTexture);
    if (!rapMaterials.empty())
    {
        job.reservation += (broadParentPaa ? Impl::RapCandidateLimit : 4 * 1024) +
            rapMaterials.capacity() * sizeof(Impl::CapturedRapMaterial);
        for (const auto& material : rapMaterials)
            job.reservation += material.key.capacity() + 1 + material.read.archive.capacity() + 1 +
                material.read.ArchiveIdentityBytes() + 64;
    }
    job.rapMaterials = std::move(rapMaterials);
    job.buildingRapFixture = static_cast<int8_t>(buildingRapFixture);
    job.tenementMultistage = tenementMultistage;
    job.broadParentPaa = broadParentPaa;
    job.broadCaptureIncomplete = broadCaptureIncomplete;
    if (!proxyModels.empty())
    {
        job.reservation += proxyModels.capacity() * sizeof(Impl::CapturedProxyModel);
        for (const auto& proxy : proxyModels)
            job.reservation += proxy.key.capacity() + 1 + proxy.read.archive.capacity() + 1 +
                proxy.read.ArchiveIdentityBytes() + 64;
    }
    job.proxyModels = std::move(proxyModels);
    job.proxyCaptureIncomplete = proxyCaptureIncomplete;
    if (!textures.empty())
    {
        job.reservation += textures.capacity() * sizeof(Impl::PackedTextureRead);
        for (const auto& texture : textures)
            job.reservation += texture.key.capacity() + 1 + texture.read.archive.capacity() + 1 +
                texture.read.ArchiveIdentityBytes() + (texture.read.archiveLease ? 64 : 0) +
                (texture.initializedSource ? texture.initializedSource->KnownCppBytes() : 0);
    }
    // Scratch is charged when a worker starts, and released before publishing.
    // Retained chains are charged separately by PreparedTextureStore.
    job.textures = std::move(textures);
    job.textureGeneration = textureGeneration;
    _impl->stats.conversionReservedBytes += job.reservation;
    _impl->stats.peakConversionReservedBytes = std::max(
        _impl->stats.peakConversionReservedBytes, _impl->stats.conversionReservedBytes);
    job.model = std::move(model);
    job.tables = std::move(tables);
    _impl->convertQueue.push_back(std::move(job));
    _impl->cv.notify_one();
    return true;
}

bool ObjectStreamPreparer::ResumeRapCapture(uint32_t index, RapStageCapture capture, void* context)
{
    if (!Foundation::IsMainThread()) return false;
    std::optional<Impl::ConvertJob> job;
    std::unique_ptr<RapStageCandidateHandoff> handoff;
    std::vector<Impl::ProxyMaterialCandidate> proxyMaterials;
    size_t slot = BuildingRapFixtures.size();
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        if (index >= _impl->count || _impl->Get(index) != State::RapCapture) return false;
        for (size_t i = 0; i < _impl->pendingRap.size(); ++i)
            if (_impl->pendingRap[i].index == index && _impl->pendingRap[i].job)
            {
                slot = i;
                job.emplace(std::move(*_impl->pendingRap[i].job));
                _impl->pendingRap[i].job.reset();
                handoff = std::move(_impl->pendingRap[i].handoff);
                proxyMaterials = std::move(_impl->pendingRap[i].proxyMaterials);
                _impl->pendingRap[i].index = uint32_t(-1);
                break;
            }
        if (!job)
        {
            // A missing parked job must not strand the admission loop in
            // RapCapture. Failed invokes the ordinary synchronous fallback.
            _impl->Set(index, State::Failed);
            _impl->cv.notify_all();
            return false;
        }
    }
    const uint64_t originalReservation = job->reservation;
    const size_t existingTextures = job->textures.size();
    uint64_t addedCharge = 0;
    const bool multistage = job->tenementMultistage && slot == 0;
    const bool broadParent = job->broadParentPaa && slot == 0;
    const bool firstProxyPass = job->proxyPhase == 0 && !proxyMaterials.empty();
    const bool proxyPaa = job->proxyPhase == 1 && handoff && handoff->proxyPaa;
    uint64_t addedSourceBytes = job->materialStageSourceBytes; // parent + proxy stages, not primary inputs
    size_t sourceRefused = 0;
    size_t proxyRaPCaptured = 0, proxySourceRefused = 0;
    try
    {
        const auto& fixture = BuildingRapFixtures[slot];
        if (handoff && handoff->Bounded() &&
            handoff->generation == job->radiusGeneration && handoff->modelIndex == index &&
            (broadParent || multistage || handoff->materials.size() == 1) &&
            job->texturePublication.Valid() &&
            job->textureGeneration == render::PreparedTextureStore::Instance().Generation())
        {
            for (const auto& material : handoff->materials)
            {
                if ((broadParent ?
                        material.consumer != RapStageCandidateHandoff::Consumer::AuthoredStage :
                    multistage ?
                        material.consumer > RapStageCandidateHandoff::Consumer::LayerNormal3 :
                        material.consumer != RapStageCandidateHandoff::Consumer::NormalMap) ||
                    (broadParent ? false : multistage ?
                        material.key.compare(0, std::strlen(TenementMaterialPrefix),
                            TenementMaterialPrefix) != 0 ||
                            !IsTenementMultistageNormal(material.stageName) :
                        material.key != fixture.material || material.stageName != fixture.normal))
                    continue;
                // Resolve the CURRENT mounted material, not merely the captured
                // raP name. Loose/text/include/remounted material falls through.
                QFBank* bank = QIFStreamB::AutoBank(material.key.c_str());
                if (bank)
                {
                    const RString prefix = bank->GetPrefix();
                    if (prefix.GetLength() >= 0 &&
                        static_cast<size_t>(prefix.GetLength()) < material.key.size() &&
                        !CmpStartStr(material.key.c_str(), prefix))
                    {
                        auto current = bank->CaptureReadRequest(
                            material.key.c_str() + prefix.GetLength(), true);
                        BankReadMemberIdentity currentMember;
                        if (current && current->CopyMemberIdentity(currentMember) &&
                            material.SameCurrentMember(currentMember) &&
                            bank->MatchesMountedMember(material.key.c_str() + prefix.GetLength(), *current) &&
                            render::PreparedTextureStore::Instance().ShouldPrepareWarm(material.stageName))
                        {
                            // The building pilot does not initialize a texture just to hold
                            // scarce ArchiveSourceBinding capacity. Capture the currently
                            // mounted physical PAA member; upload later proves that Init used
                            // this exact member and that it is still mounted.
                            const std::string& key = material.stageName;
                            if (key.capacity() <= 1024 && job->textures.size() < existingTextures +
                                    (proxyPaa ? Impl::ProxyPaaLimit : broadParent ? RapStageCandidateHandoff::MaxCandidates :
                                        (multistage ? TenementMultistageNormals.size() : 1)) &&
                                std::none_of(job->textures.begin(), job->textures.end(),
                                    [&](const auto& prior) { return prior.key == key; }))
                            {
                                QFBank* textureBank = QIFStreamB::AutoBank(key.c_str());
                                const size_t texturePrefix = textureBank ?
                                    static_cast<size_t>(textureBank->GetPrefix().GetLength()) : key.size();
                                if (textureBank && texturePrefix < key.size() &&
                                    !CmpStartStr(key.c_str(), textureBank->GetPrefix()))
                                {
                                    auto read = textureBank->CaptureReadRequest(
                                        key.c_str() + texturePrefix, true);
                                    if (read && read->HasArchiveIdentity() && read->bytes &&
                                        read->bytes <= Impl::PackedTextureLimit &&
                                        (!broadParent || DayzProxyPrefetchBounds::AdmitMaterialSource(
                                            addedSourceBytes, read->bytes)) &&
                                        textureBank->MatchesMountedMember(key.c_str() + texturePrefix, *read))
                                    {
                                        const uint64_t knownSourceBytes = key.capacity() + 1 +
                                            read->archive.capacity() + 1 + read->ArchiveIdentityBytes() +
                                            (read->archiveLease ? 64 : 0);
                                        if (knownSourceBytes <= 8 * 1024)
                                        {
                                            const size_t previousCapacity = job->textures.capacity();
                                            Impl::PackedTextureRead texture;
                                            texture.key = key;
                                            texture.read = std::move(*read);
                                            texture.physicalWitness = true;
                                            addedSourceBytes += texture.read.bytes;
                                            job->textures.push_back(std::move(texture));
                                            addedCharge += knownSourceBytes +
                                                (job->textures.capacity() - previousCapacity) *
                                                    sizeof(Impl::PackedTextureRead);
                                        }
                                    }
                                    else ++sourceRefused;
                                }
                                else ++sourceRefused;
                            }
                            else if (key.capacity() > 1024 || job->textures.size() >= existingTextures +
                                (proxyPaa ? Impl::ProxyPaaLimit : broadParent ? RapStageCandidateHandoff::MaxCandidates :
                                    (multistage ? TenementMultistageNormals.size() : 1))) ++sourceRefused;
                        }
                    }
                }
            }
        }
        if (firstProxyPass && job->texturePublication.Valid() &&
            job->textureGeneration == render::PreparedTextureStore::Instance().Generation())
        {
            uint64_t proxyRapBytes = 0;
            for (const auto& selected : proxyMaterials)
            {
                if (job->rapMaterials.size() >= Impl::ProxyRapLimit)
                { job->proxyCaptureIncomplete = true; break; }
                QFBank* proxyBank = QIFStreamB::AutoBank(selected.proxyKey.c_str());
                if (!proxyBank) { ++proxySourceRefused; continue; }
                const RString proxyPrefix = proxyBank->GetPrefix();
                if (proxyPrefix.GetLength() < 0 ||
                    static_cast<size_t>(proxyPrefix.GetLength()) >= selected.proxyKey.size() ||
                    CmpStartStr(selected.proxyKey.c_str(), proxyPrefix))
                { ++proxySourceRefused; continue; }
                auto currentProxy = proxyBank->CaptureReadRequest(
                    selected.proxyKey.c_str() + proxyPrefix.GetLength(), true);
                BankReadMemberIdentity currentProxyMember;
                if (!currentProxy || !currentProxy->CopyMemberIdentity(currentProxyMember) ||
                    !(currentProxyMember == selected.proxyMember) ||
                    !proxyBank->MatchesMountedMember(
                        selected.proxyKey.c_str() + proxyPrefix.GetLength(), *currentProxy))
                { ++proxySourceRefused; continue; }
                if (std::any_of(job->rapMaterials.begin(), job->rapMaterials.end(),
                    [&](const auto& prior) { return prior.key == selected.materialKey; })) continue;
                QFBank* materialBank = QIFStreamB::AutoBank(selected.materialKey.c_str());
                if (!materialBank) { ++proxySourceRefused; continue; }
                const RString materialPrefix = materialBank->GetPrefix();
                if (materialPrefix.GetLength() < 0 ||
                    static_cast<size_t>(materialPrefix.GetLength()) >= selected.materialKey.size() ||
                    CmpStartStr(selected.materialKey.c_str(), materialPrefix))
                { ++proxySourceRefused; continue; }
                auto read = materialBank->CaptureReadRequest(
                    selected.materialKey.c_str() + materialPrefix.GetLength(), true);
                if (!read || !read->HasArchiveIdentity() || !read->bytes ||
                    read->bytes > Impl::RapMaterialLimit ||
                    proxyRapBytes > 4ull * 1024 * 1024 - read->bytes ||
                    !materialBank->MatchesMountedMember(
                        selected.materialKey.c_str() + materialPrefix.GetLength(), *read))
                { ++proxySourceRefused; continue; }
                proxyRapBytes += read->bytes;
                const size_t previousCapacity = job->rapMaterials.capacity();
                addedCharge += selected.materialKey.capacity() + 1 + read->archive.capacity() + 1 +
                    read->ArchiveIdentityBytes() + 64;
                job->rapMaterials.push_back({selected.materialKey, std::move(*read)});
                addedCharge += (job->rapMaterials.capacity() - previousCapacity) *
                    sizeof(Impl::CapturedRapMaterial);
                ++proxyRaPCaptured;
            }
        }
    }
    catch (...) { addedCharge = 0; addedSourceBytes = job->materialStageSourceBytes;
        job->textures.resize(existingTextures); job->rapMaterials.clear();
        job->proxyCaptureIncomplete = true; } // preserve ordinary conversion
    if (proxySourceRefused || (proxyPaa && sourceRefused))
        job->proxyCaptureIncomplete = true;
    static std::atomic<unsigned> captureRows{0};
    static std::atomic<bool> multistageCaptureLogged{false};
    const bool logCapture = broadParent || multistage ?
        !multistageCaptureLogged.exchange(true, std::memory_order_relaxed) :
        captureRows.fetch_add(1, std::memory_order_relaxed) < BuildingRapFixtures.size();
    if (logCapture)
        LOG_INFO(World, "RVMAT raP building owner capture: modelIndex={} model={} stage={} physicalMembersCaptured={} multistage={} (conversion resumes; upload validation still required)",
            index, BuildingRapFixtures[slot].model,
            broadParent ? "authored-file-backed-PAA" :
                (multistage ? "trace-top-four-normals" : BuildingRapFixtures[slot].normal),
            job->textures.size() - existingTextures, multistage);
    if (broadParent && !proxyPaa)
    {
        LOG_INFO(World, "DayZ physical prefetch: modelIndex={} stageCandidates={} captured={} sourceRefused={} candidateOverflow={} addedStageSourceBytes={} addedStageSourceLimit={} complete=false reason=parent-only-authored-superset",
            index, handoff ? handoff->materials.size() : 0, job->textures.size() - existingTextures,
            sourceRefused, handoff ? handoff->overflow : true, addedSourceBytes,
            128ull * 1024 * 1024);
        for (size_t i = existingTextures; i < job->textures.size(); ++i)
            LOG_INFO(World, "DayZ physical prefetch captured: source={} memberBytes={} row={} limit=64",
                job->textures[i].key, job->textures[i].read.bytes, i - existingTextures + 1);
    }
    if (firstProxyPass)
        LOG_INFO(World, "DayZ proxy prefetch: modelIndex={} proxyMaterialCandidates={} capturedRaP={} sourceRefused={} captureIncomplete={} phase=proxy-material-owner speculative=true",
            index, proxyMaterials.size(), proxyRaPCaptured, proxySourceRefused,
            job->proxyCaptureIncomplete);
    if (proxyPaa)
    {
        LOG_INFO(World, "DayZ proxy prefetch: modelIndex={} stageCandidates={} capturedPAA={} sourceRefused={} candidateOverflow={} addedSourceBytes={} sharedStageSourceBytes={} sharedStageSourceLimit=134217728 complete=false phase=proxy-PAA-owner speculative=true",
            index, handoff->materials.size(), job->textures.size() - existingTextures, sourceRefused,
            handoff->overflow, addedSourceBytes - job->materialStageSourceBytes,
            addedSourceBytes);
        for (size_t i = existingTextures; i < job->textures.size(); ++i)
            LOG_INFO(World, "DayZ proxy prefetch captured: source={} memberBytes={} row={} limit=32 speculative=true",
                job->textures[i].key, job->textures[i].read.bytes, i - existingTextures + 1);
    }
    if (firstProxyPass)
    {
        job->proxyPhase = proxyRaPCaptured ? 1 : 2;
        job->buildingRapFixture = proxyRaPCaptured ? 0 : -1;
    }
    else if (job->proxyPhase == 1)
        job->proxyPhase = 2;
    job->materialStageSourceBytes = addedSourceBytes;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        const bool current = !_impl->stop && index < _impl->count &&
            _impl->Get(index) == State::RapCapture && !_impl->stale[index] &&
            ((handoff && handoff->generation == _impl->generation.load(std::memory_order_relaxed)) ||
             (firstProxyPass && job->radiusGeneration == _impl->generation.load(std::memory_order_relaxed))) &&
            _impl->paths[index] == job->model->sourcePath &&
            (!job->sourceToken.diagnosticParseToken ||
             _impl->SourceBindingMatches(index, job->sourceToken));
        if (current)
        {
            try
            {
                job->reservation += addedCharge;
                _impl->convertQueue.push_back(std::move(*job));
                _impl->stats.conversionReservedBytes += addedCharge;
                _impl->stats.peakConversionReservedBytes = std::max(
                    _impl->stats.peakConversionReservedBytes, _impl->stats.conversionReservedBytes);
                _impl->Set(index, State::Converting);
                _impl->cv.notify_one();
                return true;
            }
            catch (...) { job->reservation -= addedCharge; }
        }
        _impl->stats.conversionReservedBytes -= originalReservation;
        if (job->radiusGeneration == _impl->generation.load(std::memory_order_relaxed) &&
            index < _impl->count && _impl->Get(index) == State::RapCapture)
            _impl->Set(index, current ? State::Failed : State::Unknown);
        _impl->cv.notify_all();
    }
    return false;
}

ObjectStreamPreparer::ConvertedShape ObjectStreamPreparer::TakeConverted(uint32_t index)
{
    ConvertedShape out;
    std::unique_lock<std::mutex> lock(_impl->mutex);
    if (index >= _impl->count || _impl->Get(index) != State::Converted)
        return out;
    _impl->stats.conversionReservedBytes -= _impl->conversionBytes[index];
    _impl->conversionBytes[index] = 0;
    _impl->cv.notify_all();
    out.shape = _impl->convertedShapes[index];
    _impl->convertedShapes[index] = nullptr;
    if (index < _impl->convertedColdTextures.size() && _impl->convertedColdTextures[index])
    {
        const auto bytes = _impl->convertedColdTextures[index]->KnownBytes();
        _impl->stats.readyPayloadBytes -= bytes; _impl->stats.coldTextureReadyBytes -= bytes;
        out.coldTexture = std::move(_impl->convertedColdTextures[index]);
    }
    out.model = std::move(_impl->convertedModels[index]);
    _impl->convertedModels[index].reset();
    for (auto& slot : _impl->convertedRapStages)
        if (slot.payload && slot.index == index)
        { out.rapStages = std::move(slot.payload); slot.index = uint32_t(-1); break; }
    _impl->Set(index, State::Unknown);
    if (index < _impl->sourceEnvelopes.size())
    {
        // Read the token stored with the actual completed job, never infer it
        // from whichever parse snapshot happens to be current now.
        out.sourceToken = _impl->sourceEnvelopes[index].convertedToken;
        _impl->sourceEnvelopes[index].convertedToken = {};
    }
    if (out.sourceToken.diagnosticParseToken && !_impl->SourceBindingMatches(index, out.sourceToken))
    {
        ++_impl->stats.dropped;
        lock.unlock(); // shape/model retirement is never under the queue mutex
        delete out.shape;
        out = {};
    }
    return out;
}

ObjectStreamPreparer::Stats ObjectStreamPreparer::SnapshotStats() const
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    auto result = _impl->stats;
    result.warmTextureQueueBytes = _impl->WarmStorageBytes();
    return result;
}

} // namespace Poseidon
