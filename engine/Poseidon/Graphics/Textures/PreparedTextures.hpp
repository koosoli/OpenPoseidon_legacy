#pragma once

// The hand-off between the object-stream WORKER threads and the texture UPLOAD on the main
// thread (texture streaming, stage 2 of the streamed-admission work; stage 1 was the model
// parse in ObjectStreamPreparer).
//
// WHAT CROSSES THE BOUNDARY. A worker that has parsed a cold model's IR knows the model's
// texture PATHS (Model::Material::texturePath, embeddedStages[].texturePath). For each that is a
// loose block-compressed PAA it runs ReadPAABlockChain (pure: ifstream + LZO, no engine
// globals) and Puts the raw mip chain here, keyed by the same lower-cased name the texture bank
// will be asked for. When TextureWgpu::EnsureUploaded later runs on the main thread for that
// texture -- inside RegisterGpuModel, on the admission of the object -- it Takes the chain
// instead of opening the file, validates it against the header the bank already parsed
// (format, level count, per-level dimensions and byte size), and uploads. Any mismatch, or no
// entry, is exactly the old path. Nothing here ever creates a Texture, touches the bank, or
// uploads: workers produce bytes; the main thread owns every GPU and bank object.
//
// WHAT DOES NOT CROSS. Textures a material resolves at registration time (.rvmat/.emat
// stages: normal/specular/mask maps) are not in the IR; they are read the old way and are
// counted as misses so the coverage is measured, not assumed.
//
// BOUNDS. Bytes are capped (WGR_OBJECT_STREAM_TEXTURE_PREPARE_MB, default 512): a worker that
// finds the store full skips, it never blocks and never evicts something the main thread is
// about to want (entries are consumed in the same distance order they were produced). Entries
// that will never be consumed -- a texture that was already GPU-resident when the worker looked,
// or one belonging to a model the camera left behind -- are the leak this design has to close:
// (1) the main thread marks every texture that reaches a real upload (or a failed one) as
// `Uploaded`, and workers skip those names; (2) an entry older than
// WGR_OBJECT_STREAM_TEXTURE_PREPARE_TTL_S (default 30) is dropped at the next Put; (3) the whole
// store is cleared on world change (Landscape's preparer Reset).
//
// THREADING. One mutex around a name->entry map plus an insertion-ordered list, and a name set
// for the uploaded marks. Every critical section is a map operation or a pointer move; the
// file read and LZO happen outside it. Take moves the vector out, so the main thread never
// copies a mip chain under the lock.

#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/Graphics/Textures/PreparedDdsPublication.hpp>
#include <Poseidon/Graphics/Textures/AlphaShapeAnalysis.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace Poseidon { class TextureSourceDDS; struct DdsPreparationOptions; }

namespace Poseidon::render
{

// Raw pixel facts, never a renderer-policy verdict. Valid only for the exact
// leased member and mip chain published together. No alpha plane is retained.
struct PreparedAlphaFacts
{
    BankReadRequest source;
    uint16_t magic = 0;
    int width = 0, height = 0;
    size_t topOffset = 0, topBytes = 0;
    size_t sourceHeaderOffset = SIZE_MAX;
    AlphaStats histogram;
    AlphaShapeAnalysis shape;
private:
    friend class PreparedTextureStore;
    const uint8_t* pairedBlocks = nullptr; // used only before publication, then cleared
};

class PreparedTextureStore
{
  public:
    struct Stats
    {
        uint64_t puts = 0;          // chains stored
        uint64_t putBytes = 0;      // ...their bytes
        uint64_t takes = 0;         // chains handed to the main thread (any validity)
        uint64_t takeBytes = 0;
        uint64_t misses = 0;        // Take found nothing (texture not prepared, or already consumed)
        uint64_t rejectedFull = 0;  // Put refused: byte budget
        uint64_t rejectedDup = 0;   // Put refused: already present
        uint64_t rejectedStale = 0; // Worker finished after the world's Clear.
        uint64_t expired = 0;       // entries dropped by the TTL sweep
        uint64_t alphaFactsPuts = 0, alphaFactsHits = 0, alphaFactsMisses = 0;
        uint64_t alphaFactsCancelled = 0;
        uint64_t alphaFactsCapacitySkipped = 0; // advisory refused known retained byte charge
        uint64_t paaCancelled = 0; // invalid token puts or retired cancelled PAA entries
        uint64_t skippedUploaded = 0; // ShouldPrepare said no: main thread already uploaded it
        uint64_t uploadedMarks = 0; // names currently marked Uploaded
        uint64_t warmPuts = 0, warmTakes = 0, warmSourceRefused = 0;
        uint64_t physicalPuts = 0, physicalTakes = 0, physicalSourceRefused = 0;
        uint64_t warmUploads = 0, warmUploadBytes = 0, warmUploadFailed = 0;
        // The RGBA8 side (non-DXT PAAs -- the Reforger mask population): whole-file
        // decodes done on a worker so the main thread's fallback path uploads instead
        // of decoding. Counted separately because the byte shapes differ wildly.
        uint64_t decodedPuts = 0;
        uint64_t decodedTakes = 0;
        uint64_t ddsReusePuts = 0;
        uint64_t ddsReuseHits = 0;
        size_t ddsReuseBytes = 0;
        uint64_t ddsPreparedPuts = 0;
        uint64_t ddsPreparedTakes = 0;
        uint64_t ddsConfigurationMisses = 0;
        uint64_t ddsCancelled = 0; // invalid publication puts or retired entries
        size_t entries = 0;         // current
        size_t bytes = 0;           // payload/mip/facts/lease C++ capacities; excludes map/key/allocator/kernel overhead
    };

    static PreparedTextureStore& Instance();

    // Read once from the environment. Enabled() is the master switch
    // (WGR_OBJECT_STREAM_TEXTURE_PREPARE, default 1); the others are the bounds above.
    static bool Enabled();
    static bool NativeDdsEnabled();
    // Optional narrower native experiment. Default OFF; requires NativeDdsEnabled.
    static bool NativeDdsBc3Only(); // WGR_NATIVE_DDS_BC3_ONLY=1, read once
    static size_t ByteBudget();
    static double TtlSeconds();

    // Lower-case, backslash-normalised key -- the texture bank's spelling of a model texture
    // (ShapeAdapter lower-cases Model::Material::texturePath before GlobLoadTexture; the bank
    // stores the name verbatim). Both sides key through this so they cannot drift.
    static std::string Key(const char* name);
    static std::string Key(const std::string& name) { return Key(name.c_str()); }

    // Worker or main thread. False if the name is already Uploaded, already stored, or the
    // store is over budget: the worker should not spend a file read on it. Advisory (a
    // concurrent Put may still be refused as a duplicate).
    bool ShouldPrepare(const std::string& key);
    // Capture before checking the preparer's job generation. Clear invalidates
    // this token; publication checks it under the same mutex as Clear.
    uint64_t Generation() const;
    // Worker. Stores a chain; false if refused (full / duplicate / disabled).
    // Optional publication binds same-world cancellation to the captured job.
    // A cancellation after validation may leave an invalid retained entry;
    // claims validate the token before transferring ownership. Cancellation
    // after that claim accepts the prior claim, as on the DDS path.
    bool Put(const std::string& key, PAABlockChain&& chain,
             std::optional<uint64_t> generation = std::nullopt,
             std::unique_ptr<PreparedAlphaFacts> alphaFacts = nullptr,
             const DdsPublicationToken* publication = nullptr,
             std::shared_ptr<const ArchiveSourceBinding> warmSource = nullptr,
             const BankReadRequest* physicalSource = nullptr);
    // Worker-only pure analysis, <=16 MiB aggregate chain and transient plane.
    // Cancellation is checked between decode/histogram/shape/publication; the
    // callback must own its context until return. Caller supplies the unchanged
    // owned chain decoded from source.Read; that storage must remain unmodified
    // until Put. No renderer/environment reads or content-freshness promise.
    static std::unique_ptr<PreparedAlphaFacts> BuildAlphaFacts(const PAABlockChain& chain,
        const BankReadRequest& source, const std::function<bool()>& cancelled = {});
    // Advisory only: never reserves, evicts live entries, publishes or changes the ordinary
    // chain path. A concurrent Take/Clear/Put can change the answer immediately;
    // final Put still validates generation and actual retained capacity atomically.
    // A cancelled same-key entry may be retired; payload frees occur outside the lock.
    bool CanRetainAlphaFacts(const std::string& key, const PAABlockChain& chain,
        const BankReadRequest& source, uint64_t generation, const DdsPublicationToken* publication = nullptr);
    // Advisory metadata lookup avoids native recapture on absent facts. Copy
    // checks current generation/TTL and actual current leased member under mutex.
    bool HasAlphaFacts(const std::string& key) const;
    bool CopyAlphaFacts(const std::string& key, const BankReadRequest& current,
        uint16_t magic, int width, int height, size_t topBytes, int sourceHeaderOffset, PreparedAlphaFacts& out);
    void NoteAlphaFactsCancelled();
    // Main thread. Moves the entry out if present.
    // Bound warm chains require an explicit provenance-aware consumer. An older
    // caller cannot silently consume one without receiving its source witness.
    bool Take(const std::string& key, PAABlockChain& out,
              std::shared_ptr<const ArchiveSourceBinding>* warmSource = nullptr);
    // Exact physical-member hand-off for the optional raP building pilot. The
    // initialized PAA source supplies its copied numeric member identity; a
    // matching claim transfers both chain and retained worker read request.
    // The caller must still verify the current mount against source before
    // upload. Ordinary Take cannot consume an entry with this witness.
    bool TakePhysical(const std::string& key, const BankReadMemberIdentity& initializedMember,
                      PAABlockChain& out, BankReadRequest& source);
    // Read-only diagnostic. Copies scalars while holding the existing store mutex;
    // no TTL sweep, Take, lease retention, publication or upload side effect.
    struct WarmEntryProbe
    {
        bool entryPresent = false, validChain = false, warmBound = false;
        bool publicationValid = false, withinTtl = false, sameMember = false;
        bool uploadedMarkPresent = false;
        uint64_t storeGeneration = 0;
    };
    WarmEntryProbe ProbeWarmEntry(const std::string& key, const BankReadRequest* expected) const;
    // Only for bounded owner requests that observed a missing GPU texture and
    // verified its initialized source against the current loaded mount. This
    // advisory bypasses uploaded marks, not entry/cancellation/byte limits.
    bool ShouldPrepareWarm(const std::string& key);
    // Optional owner cut before per-model cancellation. Inspect at most 32 already
    // published entries in insertion order. Only a still-valid exact-source warm
    // chain for a model transitioning to stale loses its model token; later worker
    // Puts retain theirs and are cancelled normally. Clear/world generation,
    // existing byte budget/TTL, and owner source+header validation still apply.
    // A partial prefix may miss an eligible entry; this returns only actual promotions.
    size_t PromotePublishedWarmBeforeCancel(const DdsPublicationInventory& inventory,
        const uint32_t* wantedEpochs, const std::vector<bool>& priorStale,
        size_t count, uint32_t currentEpoch);
    void NoteWarmUpload(bool successful, size_t bytes);

    // The RGBA8 twin pair, for textures whose format has no DXT chain (the fallback
    // decode population): a worker decodes the whole file with the pure DecodePAABuffer
    // and stores the image; the main thread's fallback path takes it and only uploads.
    // Same budget, TTL, dedup and Uploaded-mark rules as the chain entries.
    bool PutDecoded(const std::string& key, DecodedImage&& image,
                    std::optional<uint64_t> generation = std::nullopt);
    bool TakeDecoded(const std::string& key, DecodedImage& out);

    // Completed CPU DDS composition; same store/budget/TTL/marks, not a second
    // queue. Caller must use these options for preparation. Generation is mandatory.
    // No pixel copy at hand-off. Publication refusal destroys the owned source.
    bool PutDdsPrepared(const std::string& key, std::unique_ptr<TextureSourceDDS> source,
                        const DdsPreparationOptions& options, uint64_t generation,
                        const DdsPublicationToken* publication = nullptr);
    std::unique_ptr<TextureSourceDDS> TakeDdsPrepared(const std::string& key,
                                                     const DdsPreparationOptions& options);

    // Main-thread native tint experiment: immutable, unmodified source decodes,
    // sharing this store's TTL/Clear and byte budget (also capped at 64 MiB).
    // Exact input bytes are checked outside the mutex before copying pixels.
    // The brief copied working payload is not part of the retained-store budget.
    bool CopyDdsDecode(const std::string& key, const std::vector<uint8_t>& source, TextureSourceDDS& out);
    bool PutDdsDecode(const std::string& key, const std::vector<uint8_t>& source, const TextureSourceDDS& decoded);

    // Main thread. A texture reached a real upload attempt (success or failure): its bytes are
    // no longer wanted from a worker; also drops any pending entry for it.
    void MarkUploaded(const std::string& key);
    // Main thread. The GPU copy was evicted; a later re-upload may want a prepared chain again.
    void MarkEvicted(const std::string& key);

    // Main thread. World change.
    void Clear();
    Stats SnapshotStats() const;

  private:
    struct Impl;
    Impl* _impl;
    PreparedTextureStore();
    ~PreparedTextureStore();
    PreparedTextureStore(const PreparedTextureStore&) = delete;
    PreparedTextureStore& operator=(const PreparedTextureStore&) = delete;
};

} // namespace Poseidon::render
