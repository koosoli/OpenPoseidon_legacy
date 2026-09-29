#pragma once

// Asynchronous preparation of COLD MODELS for the streamed-object admission path
// (Landscape::UpdateModernObjectResidency, LandSave.cpp).
//
// THE PROBLEM THIS SOLVES. Admission of a streamed object whose model is not resident is
// synchronous: file read + ODOL/MLOD decode + IR compile + ShapeAdapter conversion + vertex
// buffer creation + GPU registration, all on the main thread, ~90+ ms per cold model measured
// on Reforger Everon. The admit ceiling (WGR_OBJECT_STREAM_MAX_MS) can spread a batch over
// frames but cannot bound a frame below `ceiling + one object`, because one object per update
// must always be attempted or the fill never terminates. A single cold model is therefore the
// residual stall, and only taking its cost off the main thread removes it.
//
// WHAT MOVES OFF THE MAIN THREAD, AND WHY ONLY THAT. The cold path was read end to end for
// thread safety (the analysis is in the session handover; the short form is here so the
// boundary is not moved by accident):
//
//   file read + P3D parse + Model::compile   ->  ModelCache::LoadLooseFile. Pure std:: plus the
//                                                header-only readers; no statics, no globals.
//                                                SAFE on a worker. THIS IS WHAT WORKERS DO.
//   ShapeAdapter::convertToLODShape           ->  RUNS ON WORKERS since 2026-08-31, and this
//                                                line said MAIN THREAD ONLY until then.
//
//   The three things that made it main-thread-only were each removed rather than worked
//   around, which is why the rule changed and not merely the practice:
//     * the global bank lookups (GlobLoadTexture, GTexMaterialBank.New,
//       Texture::LoadHeaders) were lifted OUT of the conversion into
//       BuildAdapterBankTables, which the main thread runs first and hands over as a
//       value (AdapterBankTables);
//     * the proxy tail (NewProxyObject / NewObject, recursive ShapeBank::New) was split
//       into FinishOdolAdapterTail, which the main thread runs at install;
//     * the RStringB property tables were the last hazard and were a genuine bug --
//       CompactBuffer's reference count was non-atomic, so two workers interning the
//       same name freed a live buffer (SIM-807). Fixed at the source.
//
//   Verified, not assumed: SIM-807's harness converts a reference shape on one thread and
//   64 jobs on N, comparing component hashes -- 30 of 30 runs bit-identical across 1, 2,
//   4, 8 and 16 threads at two selection counts. WGR_OBJECT_STREAM_ASYNC_ADAPT=0 restores
//   the synchronous path.
//
//   (Corrected after an ownership audit found this banner still asserting the old rule
//   while the shipping default contradicted it. A reader trusting it would either
//   re-serialise a path worth 3+ s of cold admission or revert the default as a bug.)
//   ShapeBank cache insert + OptimizeOneShape ->  AssetCache (unlocked) + GEngine->
//                                                CreateVertexBuffer. MAIN THREAD ONLY.
//   ObjectCreate / AddObject / RegisterGpuModel  MAIN THREAD ONLY.
//   GFileServer / QFBank (PBO-packed files)  ->  FileServerST keeps an unlocked MRU array
//                                                (FileCache::Store / MoveToFront), so a worker
//                                                may NOT read through it. A model that is not
//                                                a loose file is reported NotLoose and admitted
//                                                on the main thread exactly as before.
//
// So a worker produces the compiled IR (std::shared_ptr<Model::Model>) and nothing else; the
// main thread's admission installs it through ShapeBank::NewFromModel, which runs the adapter
// and the vertex-buffer build. That is a partial win by construction: the parse is what moves,
// the adapter and GPU registration stay. The per-model split timer on the admit path
// (`Object stream cold model:` log row) says how the ~90 ms actually divides, so if the parse
// turns out to be the small part this is a measured null result rather than an assumed win.
//
// CONTRACT WITH THE FRAME. Nothing here ever blocks the main thread on a worker: Request
// enqueues, Query reads an atomic, Take moves a pointer under a mutex that workers hold only
// for pointer-sized critical sections (the parse itself runs unlocked). An object whose model
// is not ready is SKIPPED this frame, not waited for. Reset drops everything by bumping a
// generation number and does not join; only the destructor joins.
//
// LEVERS. WGR_OBJECT_STREAM_ASYNC=0 restores the synchronous path exactly (workers are never
// started, the admit loop is the old loop). WGR_OBJECT_STREAM_ASYNC_WORKERS (default 2),
// WGR_OBJECT_STREAM_ASYNC_QUEUE (default 512 outstanding requests),
// WGR_OBJECT_STREAM_ASYNC_READY (default 128 parsed-but-not-yet-installed models held; the
// IR of a large building is megabytes, and a world names ~1,000 models, so this bound is what
// keeps a fast camera from parsing the whole world into RAM ahead of itself. Workers PAUSE
// when it is full rather than dropping anything: the queue and the admit loop share one
// distance order, so the oldest Ready IR is the next one wanted. DropStale, run once per
// recentre, is what evicts IRs the new window no longer wants, so a full ready set can never
// be a full set of unwanted models.)

#include <Poseidon/Graphics/Textures/ColdPaaHandoff.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <Poseidon/World/Terrain/StaticSourceEnvelope.hpp>
#include <Poseidon/World/Terrain/StaticPlainSourceSummary.hpp>

namespace Poseidon
{
class ArchiveSourceBinding;
// Captured from successful source Init, never reconstructed from a texture name.
struct WarmTextureRead
{
    std::string key;
    std::shared_ptr<const ArchiveSourceBinding> initializedSource;
};

// Exact-tenement speculative proxy preparation has separate source caps, but
// accepted material PAA sources share the parent pilot's 128 MiB envelope.
struct DayzProxyPrefetchBounds
{
    static constexpr uint64_t MaxProxyMember = 4ull * 1024 * 1024;
    static constexpr uint64_t MaxProxySources = 32ull * 1024 * 1024;
    static constexpr uint64_t MaxMaterialSources = 128ull * 1024 * 1024;
    static constexpr size_t MaxProxyModels = 16;
    static bool AdmitProxyModel(size_t accepted, uint64_t sourceBytes, uint64_t memberBytes) noexcept
    { return accepted < MaxProxyModels && memberBytes != 0 && memberBytes <= MaxProxyMember &&
        sourceBytes <= MaxProxySources && memberBytes <= MaxProxySources - sourceBytes; }
    static bool AdmitMaterialSource(uint64_t sourceBytes, uint64_t memberBytes) noexcept
    { return memberBytes != 0 && sourceBytes <= MaxMaterialSources &&
        memberBytes <= MaxMaterialSources - sourceBytes; }
};

// Advisory NormalMap names from completed, exact-source raP reads. No texture
// authority travels with this payload; the owner rechecks each material member
// and captures the current physical PAA member before optional preparation.
struct RapStageCandidateHandoff
{
    enum class Consumer : uint8_t { NormalMap, LayerNormal1, LayerNormal2, LayerNormal3, AuthoredStage };
    struct Material
    {
        std::string key, stageName;
        Consumer consumer = Consumer::NormalMap;
        BankReadMemberIdentity sourceMember;
        bool SameCurrentMember(const BankReadMemberIdentity& current) const noexcept
        { return sourceMember == current; }
    };
    static constexpr size_t MaxMaterials = 16, MaxStagesPerMaterial = 16;
    static constexpr size_t MaxCandidates = 64;
    bool broadParentPaa = false;
    bool proxyPaa = false; // separate second capture; parent limits remain unchanged
    bool overflow = false; // partial candidate set; never a complete dependency claim
    uint64_t generation = 0;
    uint32_t modelIndex = 0;
    std::vector<Material> materials;
    static constexpr size_t MaxKnownBytes = 64 * 1024;
    bool Bounded() const
    {
        const size_t limit = proxyPaa ? 32 : (broadParentPaa ? MaxCandidates : 4);
        const size_t byteLimit = broadParentPaa ? MaxKnownBytes : 4 * 1024;
        if (!generation || materials.empty() || materials.size() > limit ||
            materials.capacity() > limit) return false;
        size_t charge = sizeof(*this);
        auto add = [&](size_t bytes) { if (charge > byteLimit || bytes > byteLimit - charge) return false; charge += bytes; return true; };
        if (!add(materials.capacity() * sizeof(Material))) return false;
        size_t uniqueMaterials = 0;
        for (size_t i = 0; i < materials.size(); ++i)
        {
            const auto& material = materials[i];
            if (material.key.empty() || material.key.size() > 240 ||
                material.stageName.empty() || material.stageName.size() > 240 ||
                material.consumer > (broadParentPaa ? Consumer::AuthoredStage : Consumer::LayerNormal3) ||
                !material.sourceMember.bytes ||
                material.sourceMember.offset > material.sourceMember.archiveBytes ||
                material.sourceMember.bytes > material.sourceMember.archiveBytes - material.sourceMember.offset ||
                !add(material.key.capacity() + 1) || !add(material.stageName.capacity() + 1))
                return false;
            size_t appearances = 0;
            bool seenEarlier = false;
            for (size_t j = 0; j < materials.size(); ++j)
                if (materials[j].key == material.key)
                {
                    ++appearances;
                    if (j < i) seenEarlier = true;
                    if (j < i && materials[j].consumer == material.consumer &&
                        (!broadParentPaa || materials[j].stageName == material.stageName)) return false;
                }
            if (appearances > (broadParentPaa ? MaxStagesPerMaterial : 4) ||
                (!seenEarlier && ++uniqueMaterials > (broadParentPaa ? MaxMaterials : 4))) return false;
        }
        return true;
    }
};
struct Bc3EncodingObserver;
namespace render { struct DdsPublicationObserver; struct ColdPaaOwned; }
class LODShapeWithShadow;
namespace Model
{
struct Model;
namespace ShapeAdapter
{
struct AdapterBankTables;
}
}

class ObjectStreamPreparer
{
  public:
    enum class State : uint8_t
    {
        Unknown = 0, // never requested (or dropped / taken since): the caller may Request it
        Queued,      // in the queue, no worker has picked it up yet
        Parsing,     // a worker is inside LoadLooseFile for it
        Ready,       // IR available; Take() hands it over
        NotLoose,    // sticky: not a loose file, the main thread must load it via the file server
        Failed,      // sticky: loose file exists but did not parse; main thread repeats the old path
        Converting,  // stage 3: a worker is inside convertToLODShape for it
        Converted,   // stage 3: shape available; TakeConverted() hands it over
        RapCapture,  // selected building raP parsed; owner must resume conversion
    };

    struct Stats
    {
        uint64_t requested = 0; // Request() calls that actually enqueued
        uint64_t rejectedFull = 0; // Request() refused because the queue was at its limit
        // REQUEST DEDUPLICATION, MEASURED (roadmap 1.3 "deduplicate simultaneous requests").
        // Dedup here is structural -- Request() refuses any index that is not Unknown, so a
        // model asked for by twenty placements in one update is parsed once -- but until
        // these counters it was invisible, and an invisible coalescer is one nobody can tell
        // from a coalescer that stopped working. The two are split because only one of them
        // is a saving:
        //   coalesced      the index is Queued/Parsing/Ready/Converting/Converted, i.e. the
        //                  first request is doing the work and this caller rides on it. This
        //                  is the win; `coalesced / (coalesced + requested)` is the share of
        //                  asks that cost nothing.
        //   coalescedStuck the index is sticky NotLoose or Failed. Also refused, also not a
        //                  duplicate parse -- but the caller falls through to the synchronous
        //                  main-thread path, so counting it as a saving would overstate the
        //                  dedup rate by however many PBO-packed models the world has (on a
        //                  packed world, all of them).
        uint64_t coalesced = 0;
        uint64_t coalescedStuck = 0;
        // THE READY BOUND, MEASURED. ReadyLimit() back-pressure parks a worker that has queue
        // work but nowhere to put the result. The queue bound next to it has `rejectedFull`;
        // this one had nothing, so "the workers are idle because the main thread is not
        // taking" and "the workers are idle because there is nothing to do" looked identical
        // from outside. Counted once per park, under the mutex, immediately before the wait.
        uint64_t readyFullParks = 0;
        uint64_t readyByteParks = 0;
        uint64_t readyPayloadBytes = 0;
        uint64_t coldTextureSlotBytes = 0, coldTexturePrepared = 0, coldTextureRefused = 0;
        uint64_t coldTextureReadyBytes = 0; // subset of readyPayloadBytes, known C++ capacity only
        uint64_t parseReservedBytes = 0; // includes old-generation workers until they finish
        uint64_t peakReadyPayloadBytes = 0;
        uint64_t conversionReservedBytes = 0; // IR plus estimated conversion output/workspace
        uint64_t peakConversionReservedBytes = 0;
        uint64_t oversizedPayloads = 0; // useful oversize results admitted, never discarded/reparsed
        uint64_t readyByteBudget = 0;
        uint64_t prepared = 0;  // parses that produced an IR
        uint64_t taken = 0;     // IRs handed to the main thread (installed)
        uint64_t notLoose = 0;
        uint64_t failed = 0;
        uint64_t dropped = 0;    // requests / Ready IRs discarded by DropStale, Reset, or a stale generation
        double workerMs = 0.0;   // wall time workers spent inside LoadLooseFile, cumulative
        double workerMaxMs = 0.0; // the single slowest parse
        // Request() -> Ready and Request() -> Take() wall time: cumulative microseconds and
        // the single slowest, over `prepared` / `taken` respectively. This preparer is the
        // one asset path that is asynchronous today, so this is the one place "time from
        // request to residency" (roadmap Phase 3) is a real queue measurement rather than
        // the duration of a synchronous call. Includes queue wait by design -- that is the
        // number an admit loop experiences.
        uint64_t readyLatencyUsTotal = 0;
        uint64_t readyLatencyUsMax = 0;
        uint64_t takeLatencyUsTotal = 0;
        uint64_t takeLatencyUsMax = 0;
        size_t queued = 0;       // current queue depth
        size_t ready = 0;        // current Ready-and-not-taken count
        uint32_t workers = 0;
        // Texture prepare (stage 2, WGR_OBJECT_STREAM_TEXTURE_PREPARE): after a successful
        // parse the same worker reads the model's block-compressed loose PAAs into the
        // PreparedTextureStore so the main thread's upload skips the file read + LZO. All
        // still on the worker, all outside any lock.
        uint64_t texPrepared = 0;   // chains stored
        uint64_t texPreparedBytes = 0;
        uint64_t texSkipped = 0;    // ShouldPrepare said no (already uploaded/stored/full)
        uint64_t texUnreadable = 0; // named by the IR but not a readable loose DXT PAA
        double texMs = 0.0;         // wall time in ReadPAABlockChain + Put
        // Shape conversion (stage 3): the ShapeAdapter geometry conversion (measured
        // 3+ s per traverse on the main thread) runs on the same workers, fed by
        // bank tables the main thread resolved. All still outside any lock.
        uint64_t converted = 0;
        double convertMs = 0.0;
        double convertMaxMs = 0.0;
        uint64_t radiusCertificateBytes = 0; // actual compact-slot vector capacity * sizeof(slot)
        uint64_t sourceEnvelopeBytes = 0; // lazy opt-in slot capacity; default path allocates nothing
        uint64_t sourceEnvelopeScans = 0;
        uint64_t sourceEnvelopePublished = 0;
        double sourceEnvelopeMs = 0.0;
        uint64_t bc3PolicyBytes = 0; // owner-snapshot vector capacity
        uint64_t ddsPublicationBytes = 0; // DDS-enabled alias of texturePublicationBytes, never an additional allocation
        uint64_t texturePublicationBytes = 0; // bounded DDS/PBO atomic inventory; excludes shared_ptr allocator overhead
        uint64_t textureEncodeReservedBytes = 0; // source + bounded optional BC3 workspace/output
        uint64_t peakTextureEncodeReservedBytes = 0;
        uint64_t workerBc3Prepared = 0;
        uint64_t workerBc3BudgetSkipped = 0;
        double workerBc3Ms = 0.0;
        uint64_t warmTextureJobsSubmitted = 0, warmTextureJobsCompleted = 0,
            warmTextureJobsCancelled = 0, warmTextureJobsCoalesced = 0, warmTextureJobsRejected = 0;
        uint64_t warmTextureMembersPrepared = 0, warmTextureMembersSkipped = 0, warmTextureMembersUnreadable = 0;
        size_t warmTextureQueued = 0, warmTextureActive = 0;
        uint64_t warmTextureMetadataBytes = 0, warmTextureMetadataBytesPeak = 0; // conservative active+queued job debt
        uint64_t warmTextureQueueBytes = 0; // actual optional-state sizeof + queue capacity; not RSS
        uint64_t warmTextureScratchBytes = 0, warmTextureScratchBytesPeak = 0;
        double warmTextureMs = 0.0;
    };

    // WGR_OBJECT_STREAM_ASYNC. Read once.
    static bool AsyncEnabled();
    static uint32_t WorkerCount();
    static size_t QueueLimit();
    static size_t ReadyLimit();

    // Soft IR-payload budget. Unknown parses reserve 16 MiB each; an oversize result
    // may exceed it on in-flight completions, blocking new parses until consumed.
    // Conversion IR plus estimated workspace/output is charged until consumed.
    // Decoder scratch / actual converted Shape allocations are not measured.
    static uint64_t ReadyByteBudget();
    static bool WarmTextureJobsEnabled(); // both exact opt-in flags; cached, default OFF
    // Observer is copied before workers start. Its context must remain alive
    // until this preparer's destructor has joined ALL workers. Optional CPU-only
    // observation never grants renderer access or changes reservation lifetime.
    explicit ObjectStreamPreparer(uint64_t readyByteBudget = ReadyByteBudget(),
                                  const Bc3EncodingObserver* observer = nullptr,
                                  const render::DdsPublicationObserver* publicationObserver = nullptr);
    ~ObjectStreamPreparer(); // joins the workers; the only blocking call in this class

    ObjectStreamPreparer(const ObjectStreamPreparer&) = delete;
    ObjectStreamPreparer& operator=(const ObjectStreamPreparer&) = delete;

    // Main thread. Adopt a world's model table: `paths[i]` is the P3D path of model index i,
    // exactly the string ShapeBank::New would be given. Drops every outstanding request and
    // result of the previous world (in-flight parses finish on the worker and are discarded
    // there). Starts the workers on first use. modelCount == 0 is a plain flush.
    void Reset(const std::string* paths, size_t modelCount);
    bool Running() const;
    // Main thread. Size of the model table adopted by the last Reset (0 after a flush).
    size_t ModelCount() const;

    // Main thread. Ask for model `index` to be parsed. Returns true if it was enqueued now;
    // false if it is already tracked (any state but Unknown) or the queue is full.
    bool Request(uint32_t index);
    enum class SourceEnvelopeRequest : uint8_t { Requested, TooLate, Unavailable, Capacity, WrongThread };
    static constexpr size_t SourceEnvelopeInventoryLimit = 65536;
    static constexpr size_t SourceEnvelopeIdentityLimit = 4096;
    // Owner-only opt-in, captured BEFORE parsing. Any already tracked state,
    // including Queued, is TooLate; callers must explicitly request a fresh parse.
    // Canonical loaders own an exclusive IR; external loaders are never scanned.
    SourceEnvelopeRequest RequestWithStaticSourceEnvelope(uint32_t index);
    struct SourceEnvelopeSnapshot
    {
        uint64_t generation = 0;
        uint64_t diagnosticParseToken = 0; // nonzero only for an explicit opt-in parse; ordinary Request clears it
        std::string modelIdentity; // exact owned inventory token, never an IR reference
        bool attempted = false;
        Streaming::StaticSourceEnvelope envelope;
        Streaming::StaticPlainSourceSummary plainSource;
    };
    // Source evidence only, not Plain/Ready or a live config conclusion. Paired
    // source summary is copied from the SAME exclusive original parse and can be
    // probed without consulting a consumed/mutated IR. Neither value certifies
    // final bounds, disk freshness or the completion of a particular request.
    // Numeric
    // evidence survives Take, but DropStale and Reset invalidate it even after Take.
    // Describes the ORIGINAL immutable parse snapshot. Pair Query + Take of the
    // same Ready entry in one nonyielding owner operation (or maintain equivalent
    // exact association). Never apply it to a subsequently mutated/reparsed IR.
    // Generation is world/inventory identity, NOT a per-parse content revision;
    // neither disk freshness nor validity of an old retained snapshot is implied.
    SourceEnvelopeSnapshot QueryStaticSourceEnvelope(uint32_t index) const;
    // Main thread. Lock-free.
    State Query(uint32_t index) const;
    enum class RadiusState : uint8_t { Unknown, Certified };
    enum class RadiusProvenance : uint8_t { None, ParsedODOL, AdaptedShape };
    struct RadiusCertificate
    {
        RadiusState state = RadiusState::Unknown;
        RadiusProvenance provenance = RadiusProvenance::None;
        float radius = 0.0f;
        uint64_t generation = 0;
        std::string modelIdentity; // owned inventory identity; never a model/shape reference
    };
    // Certified pre-constructor model-space scalar, NOT a final world-object
    // broadphase certificate or geometric enclosure of arbitrary animation/proxy
    // poses. Config-backed NewObject branches can substitute a type shape or
    // change autocenter/recompute its sphere; world coverage must prove constructor
    // eligibility or reconcile the actual created shape before using this value.
    // Certificates survive payload Take/DropStale.
    // An unrequested model remains Unknown. Reset invalidates all certificates.
    RadiusCertificate QueryRadius(uint32_t index) const;
    struct WarmCaptureCursor { uint64_t generation = 0; size_t ordinal = 0; };
    // Owner-only optional queue state. No bank/readiness/source queries; wrong-owner
    // or disabled returns zero. Reset clears ordinal; recording refuses old worlds.
    WarmCaptureCursor QueryWarmTextureCaptureCursor() const;
    bool RecordWarmTextureCaptureCursor(WarmCaptureCursor token, size_t nextOrdinal);
    enum class WarmTextureSubmit : uint8_t
    { Submitted, Disabled, WrongThread, InvalidInventory, Unsupported, Coalesced, Capacity };
    // Owner-only; caller must compare every retained Init binding with the currently
    // mounted member in the SAME nonyielding operation. No source/header load here.
    // Unknown radius is allowed: this token binds exact inventory identity only.
    // Up to 8 queued+active jobs, 64 members/job, 2 MiB known metadata including
    // active jobs. No model State transition. Missing/unsupported data falls back.
    WarmTextureSubmit SubmitWarmTextures(uint32_t index, const RadiusCertificate& inventoryToken,
                                        std::vector<WarmTextureRead> reads);
    // Owner-thread install seam. Caller supplies the token/identity from QueryRadius
    // and the successfully installed shape's BoundingSphere; rejects stale inventories.
    bool RecordAdaptedRadius(uint32_t index, uint64_t generation,
                             const std::string& modelIdentity, float radius);
    // Main thread. If Ready, returns the IR and returns the slot to Unknown; else null.
    std::shared_ptr<Model::Model> Take(uint32_t index);
    // Main thread. For every model whose `epochs[index] != currentEpoch`: a Queued request is
    // removed from the queue and a Ready IR is freed, both back to Unknown. Queued
    // conversions are removed too; active parses/conversions discard their result on
    // completion unless a newer desired set or Request needs the slot again.
    // Called once per recentre with the desired set's
    // stamps, so the bounded ready set never fills with models nobody will Take. Returns how
    // many were dropped. Sticky NotLoose / Failed are left alone.
    size_t DropStale(const uint32_t* epochs, size_t count, uint32_t currentEpoch);
    // Main thread. Stage 3: hand a taken IR back for worker-side ShapeAdapter
    // conversion. `model->sourcePath` must already be the lowercased bank name, and
    // `tables` the main-thread bank resolution for it (BuildAdapterBankTables).
    // Returns false (caller converts synchronously) when workers are absent or the
    // slot is not Unknown. On success the slot is Converting until the worker
    // finishes, then Converted.
    // The optional exact inventory token permits certification when admission
    // has renamed sourcePath for the ShapeBank. Copied during this call. An
    // invalid token disables certification only, without changing conversion.
    // Without a token, sourcePath must match inventory identity exactly.
    struct SourceConversionToken
    {
        uint64_t generation = 0;
        uint64_t diagnosticParseToken = 0;
    };
    // Explicit originalSource requests association ONLY, not static eligibility.
    // Query + Take must refer to the SAME original immutable Ready IR in one
    // nonyielding owner operation. Controlled bank-name relabeling is allowed;
    // vertices/properties/audit must not be mutated or replaced afterwards.
    // Snapshot/sidecar identity, generation and per-parse token must match.
    // Invalid explicit binding rejects the job; never silently converts it as
    // ordinary work. Cancellation invalidates publication even after rescue.
    // ownerTextureAdmission is explicit for LandSave cold visual admission; CPU-only
    // actor conversions never opt into unused texture work. Dedicated exact1 flag required.
    // Optional owner callback captures the current initialized physical PAA source for
    // a validated raP NormalMap. It runs only on the main thread, before enqueue.
    using RapStageCapture = bool (*)(void*, const char*, WarmTextureRead&);
    bool SubmitConvert(uint32_t index, std::shared_ptr<Model::Model> model,
                       std::shared_ptr<const Model::ShapeAdapter::AdapterBankTables> tables,
                       const RadiusCertificate* inventoryToken = nullptr,
                       const SourceEnvelopeSnapshot* originalSource = nullptr,
                       bool ownerTextureAdmission = false,
                       RapStageCapture rapStageCapture = nullptr, void* rapStageContext = nullptr);

    // Owner-only, nonblocking second phase for the exact building pilot. The
    // preparer rechecks the material member, captures at most one current PAA
    // source through the callback, then resumes the original conversion job.
    // Empty/refused capture still resumes complete geometry admission.
    bool ResumeRapCapture(uint32_t index, RapStageCapture capture, void* context = nullptr);

    struct ConvertedShape
    {
        LODShapeWithShadow* shape = nullptr; // ownership transfers to the caller
        std::shared_ptr<Model::Model> model; // the IR it was converted from
        SourceConversionToken sourceToken; // actual job association; ordinary jobs remain zero
        std::unique_ptr<render::ColdPaaOwned> coldTexture; // operation-local, optional; no global TTL delivery
        std::unique_ptr<RapStageCandidateHandoff> rapStages; // names only; exact opt-in fixture, no PAA readiness
    };
    // Main thread. If Converted, returns the shape + IR and returns the slot to
    // Unknown; else {}. The caller MUST run CreateAdapterProxies + ShapeBank install.
    ConvertedShape TakeConverted(uint32_t index);

    // Main thread. Snapshot of the counters (takes the mutex briefly).
    Stats SnapshotStats() const;

  private:
    SourceEnvelopeRequest RequestInternal(uint32_t index, bool sourceEnvelope);
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace Poseidon
